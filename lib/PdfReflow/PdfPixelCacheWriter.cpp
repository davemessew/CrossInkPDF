#include "PdfPixelCacheWriter.h"

namespace {

PdfStatus toPdfStatus(const pixel_cache::Status status) {
  switch (status) {
    case pixel_cache::Status::Ok:
      return PdfStatus::success();
    case pixel_cache::Status::SizeOverflow:
      return PdfStatus::failure(PdfError::LimitExceeded);
    case pixel_cache::Status::InvalidArgument:
    case pixel_cache::Status::InvalidDimensions:
      return PdfStatus::failure(PdfError::InvalidArgument);
  }
  return PdfStatus::failure(PdfError::Malformed);
}

}  // namespace

PdfStatus PdfPixelCacheWriter::begin(const PdfByteSink sink, const size_t width, const size_t height) {
  if (!status_.ok()) {
    return status_;
  }
  if (initialized_) {
    return fail(PdfStatus::failure(PdfError::InvalidArgument));
  }
  initialized_ = true;

  if (!sink.valid()) {
    return fail(PdfStatus::failure(PdfError::InvalidArgument));
  }

  const pixel_cache::Status layoutStatus = pixel_cache::calculateLayout(width, height, layout_);
  if (layoutStatus != pixel_cache::Status::Ok) {
    return fail(toPdfStatus(layoutStatus));
  }

  sink_ = sink;
  outputOffset_ = 0;
  nextRow_ = 0;
  finished_ = false;

  uint8_t header[pixel_cache::kHeaderSize]{};
  pixel_cache::encodeHeader(layout_, header);
  return writeChunk(header, sizeof(header));
}

PdfStatus PdfPixelCacheWriter::writeRow(const uint32_t rowIndex, const uint8_t* const pixels, const size_t pixelCount) {
  if (!status_.ok()) {
    return status_;
  }
  if (!initialized_ || finished_ || pixels == nullptr) {
    return fail(PdfStatus::failure(PdfError::InvalidArgument, pixelCount));
  }
  if (rowIndex != nextRow_ || rowIndex >= layout_.height) {
    return fail(PdfStatus::failure(PdfError::InvalidOffset, rowIndex));
  }
  if (pixelCount != layout_.width) {
    return fail(PdfStatus::failure(PdfError::InvalidArgument, pixelCount));
  }
  for (size_t pixelIndex = 0; pixelIndex < pixelCount; ++pixelIndex) {
    if (pixels[pixelIndex] > 0x03U) {
      return fail(PdfStatus::failure(PdfError::InvalidArgument, pixelIndex));
    }
  }

  uint8_t chunk[kChunkBytes]{};
  size_t byteIndex = 0;
  while (byteIndex < layout_.bytesPerRow) {
    const size_t remaining = layout_.bytesPerRow - byteIndex;
    const size_t chunkSize = remaining < kChunkBytes ? remaining : kChunkBytes;

    for (size_t chunkIndex = 0; chunkIndex < chunkSize; ++chunkIndex) {
      uint8_t packed = 0;
      const size_t firstPixel = (byteIndex + chunkIndex) * 4U;
      for (size_t slot = 0; slot < 4U; ++slot) {
        const size_t pixelIndex = firstPixel + slot;
        if (pixelIndex >= layout_.width) {
          break;
        }
        packed |= static_cast<uint8_t>((pixels[pixelIndex] & 0x03U) << (6U - slot * 2U));
      }
      chunk[chunkIndex] = packed;
    }

    const PdfStatus status = writeChunk(chunk, chunkSize);
    if (!status.ok()) {
      return status;
    }
    byteIndex += chunkSize;
  }

  ++nextRow_;
  return PdfStatus::success();
}

PdfStatus PdfPixelCacheWriter::finish() {
  if (!status_.ok()) {
    return status_;
  }
  if (!initialized_ || finished_) {
    return fail(PdfStatus::failure(PdfError::InvalidArgument));
  }
  if (nextRow_ != layout_.height) {
    return fail(PdfStatus::failure(PdfError::UnexpectedEof, nextRow_));
  }
  finished_ = true;
  return PdfStatus::success();
}

PdfStatus PdfPixelCacheWriter::writeChunk(const uint8_t* const bytes, const size_t size) {
  if (!status_.ok()) {
    return status_;
  }
  size_t written = 0;
  const PdfStatus status = sink_.write(sink_.context, bytes, size, &written);
  if (!status.ok()) {
    if (written <= size) {
      outputOffset_ += written;
    }
    return fail(status);
  }
  if (written != size) {
    if (written < size) {
      outputOffset_ += written;
    }
    return fail(PdfStatus::failure(PdfError::IoFailure, outputOffset_));
  }
  outputOffset_ += written;
  return PdfStatus::success();
}

PdfStatus PdfPixelCacheWriter::fail(const PdfStatus status) {
  if (status_.ok()) {
    status_ = status;
  }
  return status_;
}
