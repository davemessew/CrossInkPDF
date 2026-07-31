#include "PdfImageExtractor.h"

#include <cstring>
#include <limits>

namespace {

constexpr uint64_t kMaximumSourcePixels = 16000000ULL;
constexpr size_t kMaximumSourceRowBytes = 8192;
constexpr size_t kMaximumOutputRowBytes = 4096;
constexpr size_t kReadChunkBytes = 96;

struct CenterMapCursor {
  uint32_t coordinate = 0;
  uint32_t baseStep = 0;
  uint32_t stepRemainder = 0;
  uint32_t error = 0;
  uint16_t denominator = 0;
};

bool isPngPredictor(const uint8_t predictor) { return predictor >= 10U && predictor <= 15U; }

bool isValidDecode(const PdfImageDecode decode) {
  return decode == PdfImageDecode::Normal || decode == PdfImageDecode::Inverted;
}

bool isValidBits(const PdfImageColorSpace colorSpace, const uint8_t bits) {
  switch (colorSpace) {
    case PdfImageColorSpace::Gray:
    case PdfImageColorSpace::IndexedGray:
    case PdfImageColorSpace::IndexedRGB:
      return bits == 1U || bits == 2U || bits == 4U || bits == 8U;
    case PdfImageColorSpace::RGB:
      return bits == 8U;
    case PdfImageColorSpace::ImageMask:
      return bits == 1U;
  }
  return false;
}

uint8_t componentCount(const PdfImageColorSpace colorSpace) { return colorSpace == PdfImageColorSpace::RGB ? 3U : 1U; }

uint8_t readPackedSample(const uint8_t* const row, const size_t sampleIndex, const uint8_t bits) {
  if (bits == 8U) {
    return row[sampleIndex];
  }
  const size_t bitOffset = sampleIndex * bits;
  const uint8_t shift = static_cast<uint8_t>(8U - bits - bitOffset % 8U);
  const uint8_t mask = static_cast<uint8_t>((1U << bits) - 1U);
  return static_cast<uint8_t>((row[bitOffset / 8U] >> shift) & mask);
}

void writePackedSample(uint8_t* const row, const size_t sampleIndex, const uint8_t bits, const uint8_t sample) {
  if (bits == 8U) {
    row[sampleIndex] = sample;
    return;
  }
  const size_t bitOffset = sampleIndex * bits;
  const uint8_t shift = static_cast<uint8_t>(8U - bits - bitOffset % 8U);
  const uint8_t mask = static_cast<uint8_t>((1U << bits) - 1U);
  const uint8_t shiftedMask = static_cast<uint8_t>(mask << shift);
  row[bitOffset / 8U] =
      static_cast<uint8_t>((row[bitOffset / 8U] & ~shiftedMask) | static_cast<uint8_t>((sample & mask) << shift));
}

uint8_t paethPredictor(const uint8_t left, const uint8_t up, const uint8_t upLeft) {
  const int prediction = static_cast<int>(left) + static_cast<int>(up) - static_cast<int>(upLeft);
  const int leftDistance = prediction > left ? prediction - left : left - prediction;
  const int upDistance = prediction > up ? prediction - up : up - prediction;
  const int upLeftDistance = prediction > upLeft ? prediction - upLeft : upLeft - prediction;
  if (leftDistance <= upDistance && leftDistance <= upLeftDistance) {
    return left;
  }
  if (upDistance <= upLeftDistance) {
    return up;
  }
  return upLeft;
}

uint32_t mapCenter(const uint32_t outputCoordinate, const uint32_t sourceExtent, const uint32_t outputExtent) {
  const uint64_t numerator = (static_cast<uint64_t>(outputCoordinate) * 2ULL + 1ULL) * sourceExtent;
  uint32_t mapped = static_cast<uint32_t>(numerator / (static_cast<uint64_t>(outputExtent) * 2ULL));
  if (mapped >= sourceExtent) {
    mapped = sourceExtent - 1U;
  }
  return mapped;
}

CenterMapCursor makeCenterMapCursor(const uint32_t sourceExtent, const uint16_t outputExtent) {
  CenterMapCursor cursor{};
  cursor.denominator = static_cast<uint16_t>(static_cast<uint32_t>(outputExtent) * 2U);
  cursor.coordinate = sourceExtent / cursor.denominator;
  cursor.error = sourceExtent % cursor.denominator;
  const uint64_t delta = static_cast<uint64_t>(sourceExtent) * 2ULL;
  cursor.baseStep = static_cast<uint32_t>(delta / cursor.denominator);
  cursor.stepRemainder = static_cast<uint32_t>(delta % cursor.denominator);
  return cursor;
}

void advanceCenterMapCursor(CenterMapCursor* const cursor) {
  cursor->coordinate += cursor->baseStep;
  cursor->error += cursor->stepRemainder;
  if (cursor->error >= cursor->denominator) {
    cursor->error -= cursor->denominator;
    ++cursor->coordinate;
  }
}

void calculateOutputDimensions(const PdfImageParameters& parameters, uint16_t* const width, uint16_t* const height) {
  uint64_t outputWidth = parameters.width;
  uint64_t outputHeight = parameters.height;
  if (parameters.width > parameters.maximumOutputWidth || parameters.height > parameters.maximumOutputHeight) {
    if (static_cast<uint64_t>(parameters.width) * parameters.maximumOutputHeight >
        static_cast<uint64_t>(parameters.height) * parameters.maximumOutputWidth) {
      outputWidth = parameters.maximumOutputWidth;
      outputHeight = static_cast<uint64_t>(parameters.height) * outputWidth / parameters.width;
    } else {
      outputHeight = parameters.maximumOutputHeight;
      outputWidth = static_cast<uint64_t>(parameters.width) * outputHeight / parameters.height;
    }
    if (outputWidth == 0) {
      outputWidth = 1;
    }
    if (outputHeight == 0) {
      outputHeight = 1;
    }
  }
  *width = static_cast<uint16_t>(outputWidth);
  *height = static_cast<uint16_t>(outputHeight);
}

uint8_t normalizeSample(const uint8_t sample, const uint8_t bits) {
  const uint16_t maximum = static_cast<uint16_t>((1U << bits) - 1U);
  return static_cast<uint8_t>((static_cast<uint16_t>(sample) * 255U + maximum / 2U) / maximum);
}

}  // namespace

PdfStatus PdfImageExtractor::begin(const PdfImageParameters& parameters, const PdfByteSink output,
                                   const PdfImageWorkspace& workspace) {
  if (!status_.ok()) {
    return status_;
  }
  if (initialized_) {
    return fail(PdfStatus::failure(PdfError::InvalidArgument));
  }
  initialized_ = true;
  parameters_ = parameters;
  workspace_ = workspace;

  if (parameters.width == 0 || parameters.height == 0 || parameters.maximumOutputWidth == 0 ||
      parameters.maximumOutputHeight == 0 || parameters.maximumOutputBytes == 0 || !output.valid()) {
    return fail(PdfStatus::failure(PdfError::InvalidArgument));
  }
  if (!isValidDecode(parameters.decode) || !isValidDecode(parameters.softMaskDecode) ||
      !isValidBits(parameters.colorSpace, parameters.bitsPerComponent) ||
      (parameters.predictor != 1U && parameters.predictor != 2U && !isPngPredictor(parameters.predictor))) {
    return fail(PdfStatus::failure(PdfError::UnsupportedEncoding));
  }
  if (parameters.hasSoftMask && parameters.colorSpace == PdfImageColorSpace::ImageMask) {
    return fail(PdfStatus::failure(PdfError::UnsupportedEncoding));
  }

  const uint64_t sourcePixels = static_cast<uint64_t>(parameters.width) * parameters.height;
  if (sourcePixels > kMaximumSourcePixels) {
    return fail(PdfStatus::failure(PdfError::LimitExceeded, sourcePixels));
  }

  components_ = componentCount(parameters.colorSpace);
  const uint64_t rowBits = static_cast<uint64_t>(parameters.width) * components_ * parameters.bitsPerComponent;
  const uint64_t rowBytes = rowBits / 8ULL + (rowBits % 8ULL == 0ULL ? 0ULL : 1ULL);
  if (rowBytes == 0 || rowBytes > kMaximumSourceRowBytes ||
      rowBytes > static_cast<uint64_t>(std::numeric_limits<size_t>::max())) {
    return fail(PdfStatus::failure(PdfError::LimitExceeded, rowBytes));
  }
  info_.sourceRowBytes = static_cast<size_t>(rowBytes);

  if (parameters.colorSpace == PdfImageColorSpace::IndexedGray ||
      parameters.colorSpace == PdfImageColorSpace::IndexedRGB) {
    const size_t paletteComponents = parameters.colorSpace == PdfImageColorSpace::IndexedRGB ? 3U : 1U;
    if (parameters.palette == nullptr || parameters.paletteEntries == 0 || parameters.paletteEntries > 256U ||
        parameters.paletteBytes < static_cast<size_t>(parameters.paletteEntries) * paletteComponents) {
      return fail(PdfStatus::failure(PdfError::InvalidArgument, parameters.paletteEntries));
    }
  }

  calculateOutputDimensions(parameters, &info_.outputWidth, &info_.outputHeight);
  if (info_.outputWidth == 0 || info_.outputHeight == 0 || info_.outputWidth > kMaximumOutputRowBytes) {
    return fail(PdfStatus::failure(PdfError::LimitExceeded, info_.outputWidth));
  }
  if (workspace.sourceRow == nullptr || workspace.sourceRowCapacity < info_.sourceRowBytes) {
    return fail(PdfStatus::failure(PdfError::InsufficientMemory, info_.sourceRowBytes));
  }
  if (workspace.outputRow == nullptr || workspace.outputRowCapacity < info_.outputWidth) {
    return fail(PdfStatus::failure(PdfError::InsufficientMemory, info_.outputWidth));
  }

  pixel_cache::Layout outputLayout{};
  const pixel_cache::Status layoutStatus =
      pixel_cache::calculateLayout(info_.outputWidth, info_.outputHeight, outputLayout);
  if (layoutStatus != pixel_cache::Status::Ok) {
    return fail(PdfStatus::failure(PdfError::LimitExceeded));
  }
  if (outputLayout.fileBytes > parameters.maximumOutputBytes) {
    return fail(PdfStatus::failure(PdfError::LimitExceeded, outputLayout.fileBytes));
  }

  const uint64_t decodedRowBytes = rowBytes + (isPngPredictor(parameters.predictor) ? 1ULL : 0ULL);
  if (decodedRowBytes > std::numeric_limits<uint64_t>::max() / parameters.height) {
    return fail(PdfStatus::failure(PdfError::LimitExceeded));
  }
  info_.sourceWidth = parameters.width;
  info_.sourceHeight = parameters.height;
  info_.paletteEntries = parameters.paletteEntries;
  info_.expectedDecodedBytes = decodedRowBytes * parameters.height;
  info_.outputBytes = outputLayout.fileBytes;
  pngBytesPerPixel_ = static_cast<uint8_t>((components_ * parameters.bitsPerComponent + 7U) / 8U);
  if (pngBytesPerPixel_ == 0U) {
    pngBytesPerPixel_ = 1U;
  }
  readingPngFilter_ = isPngPredictor(parameters.predictor);
  nextSelectedSourceRow_ = mapCenter(0, parameters.height, info_.outputHeight);
  if (readingPngFilter_) {
    std::memset(workspace.sourceRow, 0, info_.sourceRowBytes);
  }

  const PdfStatus writerStatus = writer_.begin(output, info_.outputWidth, info_.outputHeight);
  if (!writerStatus.ok()) {
    return fail(writerStatus);
  }
  return PdfStatus::success();
}

PdfByteSink PdfImageExtractor::decodedSink() { return initialized_ ? PdfByteSink{this, writeDecoded} : PdfByteSink{}; }

PdfByteSink PdfImageExtractor::softMaskSink() {
  return initialized_ && parameters_.hasSoftMask ? PdfByteSink{this, writeSoftMask} : PdfByteSink{};
}

PdfStatus PdfImageExtractor::extractDecoded(const PdfByteSource& decodedSource) {
  if (!status_.ok()) {
    return status_;
  }
  if (!initialized_ || finished_ || parameters_.hasSoftMask || !decodedSource.valid()) {
    return fail(PdfStatus::failure(PdfError::InvalidArgument));
  }
  if (decodedSource.size > info_.expectedDecodedBytes) {
    return fail(PdfStatus::failure(PdfError::Malformed, info_.expectedDecodedBytes));
  }

  uint8_t chunk[kReadChunkBytes]{};
  uint64_t offset = 0;
  while (offset < decodedSource.size) {
    const uint64_t remaining = decodedSource.size - offset;
    const size_t requested = remaining < sizeof(chunk) ? static_cast<size_t>(remaining) : sizeof(chunk);
    size_t bytesRead = 0;
    const PdfStatus readStatus = decodedSource.readAt(decodedSource.context, offset, chunk, requested, &bytesRead);
    if (!readStatus.ok()) {
      return fail(readStatus);
    }
    if (bytesRead == 0) {
      return fail(PdfStatus::failure(PdfError::UnexpectedEof, offset));
    }
    if (bytesRead > requested) {
      return fail(PdfStatus::failure(PdfError::IoFailure, offset));
    }

    size_t consumed = 0;
    const PdfStatus consumeStatus = consumeDecoded(chunk, bytesRead, &consumed);
    if (!consumeStatus.ok()) {
      return consumeStatus;
    }
    if (consumed != bytesRead) {
      return fail(PdfStatus::failure(PdfError::IoFailure, offset + consumed));
    }
    offset += bytesRead;
  }
  if (offset < info_.expectedDecodedBytes) {
    return fail(PdfStatus::failure(PdfError::UnexpectedEof, offset));
  }
  return PdfStatus::success();
}

PdfStatus PdfImageExtractor::finish() {
  if (!status_.ok()) {
    return status_;
  }
  if (!initialized_ || finished_) {
    return fail(PdfStatus::failure(PdfError::InvalidArgument));
  }
  if (awaitingSoftMask_) {
    return fail(PdfStatus::failure(PdfError::UnexpectedEof, softMaskPosition_));
  }
  if (sourceRowIndex_ != parameters_.height || sourceRowPosition_ != 0 || decodedBytes_ != info_.expectedDecodedBytes) {
    return fail(PdfStatus::failure(PdfError::UnexpectedEof, decodedBytes_));
  }
  if (outputRowIndex_ != info_.outputHeight) {
    return fail(PdfStatus::failure(PdfError::Malformed, outputRowIndex_));
  }
  const PdfStatus writerStatus = writer_.finish();
  if (!writerStatus.ok()) {
    return fail(writerStatus);
  }
  finished_ = true;
  return PdfStatus::success();
}

PdfStatus PdfImageExtractor::writeDecoded(void* const context, const uint8_t* const source, const size_t requested,
                                          size_t* const bytesWritten) {
  if (context == nullptr) {
    return PdfStatus::failure(PdfError::InvalidArgument);
  }
  return static_cast<PdfImageExtractor*>(context)->consumeDecoded(source, requested, bytesWritten);
}

PdfStatus PdfImageExtractor::writeSoftMask(void* const context, const uint8_t* const source, const size_t requested,
                                           size_t* const bytesWritten) {
  if (context == nullptr) {
    return PdfStatus::failure(PdfError::InvalidArgument);
  }
  return static_cast<PdfImageExtractor*>(context)->consumeSoftMask(source, requested, bytesWritten);
}

PdfStatus PdfImageExtractor::consumeDecoded(const uint8_t* const source, const size_t requested,
                                            size_t* const bytesWritten) {
  if (bytesWritten == nullptr) {
    return fail(PdfStatus::failure(PdfError::InvalidArgument));
  }
  *bytesWritten = 0;
  if (!status_.ok()) {
    return status_;
  }
  if (!initialized_ || finished_ || awaitingSoftMask_ || (source == nullptr && requested != 0)) {
    return fail(PdfStatus::failure(PdfError::InvalidArgument, decodedBytes_));
  }

  size_t consumed = 0;
  while (consumed < requested) {
    if (sourceRowIndex_ >= parameters_.height) {
      *bytesWritten = consumed;
      return fail(PdfStatus::failure(PdfError::Malformed, decodedBytes_));
    }

    if (readingPngFilter_) {
      pngFilter_ = source[consumed++];
      ++decodedBytes_;
      if (pngFilter_ > 4U) {
        *bytesWritten = consumed;
        return fail(PdfStatus::failure(PdfError::Malformed, decodedBytes_ - 1U));
      }
      for (uint8_t& byte : pngUpLeft_) {
        byte = 0;
      }
      readingPngFilter_ = false;
      continue;
    }

    const size_t remaining = info_.sourceRowBytes - sourceRowPosition_;
    const size_t available = requested - consumed;
    const size_t take = remaining < available ? remaining : available;
    if (isPngPredictor(parameters_.predictor)) {
      for (size_t index = 0; index < take; ++index) {
        const size_t rowIndex = sourceRowPosition_ + index;
        const uint8_t encoded = source[consumed + index];
        const uint8_t up = workspace_.sourceRow[rowIndex];
        const uint8_t ringIndex = static_cast<uint8_t>(rowIndex % pngBytesPerPixel_);
        const uint8_t upLeft = rowIndex >= pngBytesPerPixel_ ? pngUpLeft_[ringIndex] : 0U;
        const uint8_t left = rowIndex >= pngBytesPerPixel_ ? workspace_.sourceRow[rowIndex - pngBytesPerPixel_] : 0U;
        pngUpLeft_[ringIndex] = up;

        uint8_t predictor = 0;
        switch (pngFilter_) {
          case 0:
            break;
          case 1:
            predictor = left;
            break;
          case 2:
            predictor = up;
            break;
          case 3:
            predictor = static_cast<uint8_t>((static_cast<uint16_t>(left) + up) / 2U);
            break;
          case 4:
            predictor = paethPredictor(left, up, upLeft);
            break;
          default:
            *bytesWritten = consumed + index;
            return fail(PdfStatus::failure(PdfError::Malformed, decodedBytes_ + index));
        }
        workspace_.sourceRow[rowIndex] = static_cast<uint8_t>(encoded + predictor);
      }
    } else {
      std::memcpy(workspace_.sourceRow + sourceRowPosition_, source + consumed, take);
    }
    sourceRowPosition_ += take;
    consumed += take;
    decodedBytes_ += take;

    if (sourceRowPosition_ == info_.sourceRowBytes) {
      if (parameters_.predictor == 2U) {
        const size_t sampleCount = static_cast<size_t>(parameters_.width) * components_;
        const uint8_t sampleMask = static_cast<uint8_t>((1U << parameters_.bitsPerComponent) - 1U);
        for (size_t sampleIndex = components_; sampleIndex < sampleCount; ++sampleIndex) {
          const uint8_t difference = readPackedSample(workspace_.sourceRow, sampleIndex, parameters_.bitsPerComponent);
          const uint8_t left =
              readPackedSample(workspace_.sourceRow, sampleIndex - components_, parameters_.bitsPerComponent);
          writePackedSample(workspace_.sourceRow, sampleIndex, parameters_.bitsPerComponent,
                            static_cast<uint8_t>((difference + left) & sampleMask));
        }
      }

      const PdfStatus rowStatus = completeSourceRow();
      if (!rowStatus.ok()) {
        *bytesWritten = consumed;
        return rowStatus;
      }
      sourceRowPosition_ = 0;
      ++sourceRowIndex_;
      readingPngFilter_ = isPngPredictor(parameters_.predictor);
      if (parameters_.hasSoftMask) {
        awaitingSoftMask_ = true;
        softMaskPosition_ = 0;
        nextMaskOutputX_ = 0;
        break;
      }
    }
  }

  *bytesWritten = consumed;
  return PdfStatus::success();
}

PdfStatus PdfImageExtractor::consumeSoftMask(const uint8_t* const source, const size_t requested,
                                             size_t* const bytesWritten) {
  if (bytesWritten == nullptr) {
    return fail(PdfStatus::failure(PdfError::InvalidArgument));
  }
  *bytesWritten = 0;
  if (!status_.ok()) {
    return status_;
  }
  if (!initialized_ || finished_ || !parameters_.hasSoftMask || !awaitingSoftMask_ ||
      (source == nullptr && requested != 0)) {
    return fail(PdfStatus::failure(PdfError::InvalidArgument, softMaskPosition_));
  }

  const size_t remaining = static_cast<size_t>(parameters_.width) - softMaskPosition_;
  const size_t consumed = requested < remaining ? requested : remaining;
  if (currentRowSelected_) {
    for (size_t index = 0; index < consumed; ++index) {
      const uint32_t sourceX = static_cast<uint32_t>(softMaskPosition_ + index);
      while (nextMaskOutputX_ < info_.outputWidth && maskSourceX_ == sourceX) {
        uint8_t alpha = source[index];
        if (parameters_.softMaskDecode == PdfImageDecode::Inverted) {
          alpha = static_cast<uint8_t>(255U - alpha);
        }
        const uint8_t base = workspace_.outputRow[nextMaskOutputX_];
        const uint32_t flattened = (static_cast<uint32_t>(base) * alpha + 255U * (255U - alpha) + 127U) / 255U;
        workspace_.outputRow[nextMaskOutputX_] = static_cast<uint8_t>(flattened >> 6U);
        ++nextMaskOutputX_;
        maskSourceX_ += maskBaseStep_;
        maskError_ += maskStepRemainder_;
        if (maskError_ >= maskDenominator_) {
          maskError_ -= maskDenominator_;
          ++maskSourceX_;
        }
      }
    }
  }
  softMaskPosition_ += consumed;
  *bytesWritten = consumed;

  if (softMaskPosition_ == parameters_.width) {
    if (currentRowSelected_ && nextMaskOutputX_ != info_.outputWidth) {
      return fail(PdfStatus::failure(PdfError::Malformed, nextMaskOutputX_));
    }
    if (currentRowSelected_) {
      const PdfStatus writeStatus = writeCurrentOutputRow();
      if (!writeStatus.ok()) {
        return writeStatus;
      }
    }
    awaitingSoftMask_ = false;
    currentRowSelected_ = false;
    softMaskPosition_ = 0;
    nextMaskOutputX_ = 0;
  }
  return PdfStatus::success();
}

PdfStatus PdfImageExtractor::completeSourceRow() {
  currentRowSelected_ = outputRowIndex_ < info_.outputHeight && nextSelectedSourceRow_ == sourceRowIndex_;
  if (!currentRowSelected_) {
    return PdfStatus::success();
  }

  CenterMapCursor horizontalMap = makeCenterMapCursor(parameters_.width, info_.outputWidth);
  for (uint16_t outputX = 0; outputX < info_.outputWidth; ++outputX) {
    const uint32_t sourceX = horizontalMap.coordinate;
    uint8_t luminance = 0;
    if (parameters_.colorSpace == PdfImageColorSpace::ImageMask) {
      uint8_t paint = readPackedSample(workspace_.sourceRow, sourceX, 1);
      if (parameters_.decode == PdfImageDecode::Inverted) {
        paint ^= 1U;
      }
      luminance = paint == 0U ? parameters_.imageMaskPaintLuminance : 255U;
    } else if (parameters_.colorSpace == PdfImageColorSpace::Gray) {
      const uint8_t sample = readPackedSample(workspace_.sourceRow, sourceX, parameters_.bitsPerComponent);
      luminance = normalizeSample(sample, parameters_.bitsPerComponent);
      if (parameters_.decode == PdfImageDecode::Inverted) {
        luminance = static_cast<uint8_t>(255U - luminance);
      }
    } else if (parameters_.colorSpace == PdfImageColorSpace::RGB) {
      const size_t sampleIndex = static_cast<size_t>(sourceX) * 3U;
      uint8_t red = workspace_.sourceRow[sampleIndex];
      uint8_t green = workspace_.sourceRow[sampleIndex + 1U];
      uint8_t blue = workspace_.sourceRow[sampleIndex + 2U];
      if (parameters_.decode == PdfImageDecode::Inverted) {
        red = static_cast<uint8_t>(255U - red);
        green = static_cast<uint8_t>(255U - green);
        blue = static_cast<uint8_t>(255U - blue);
      }
      luminance = static_cast<uint8_t>((77U * red + 150U * green + 29U * blue + 128U) >> 8U);
    } else {
      uint8_t paletteIndex = readPackedSample(workspace_.sourceRow, sourceX, parameters_.bitsPerComponent);
      const uint8_t maximumSample = static_cast<uint8_t>((1U << parameters_.bitsPerComponent) - 1U);
      if (parameters_.decode == PdfImageDecode::Inverted) {
        paletteIndex = static_cast<uint8_t>(maximumSample - paletteIndex);
      }
      if (paletteIndex >= parameters_.paletteEntries) {
        paletteIndex = static_cast<uint8_t>(parameters_.paletteEntries - 1U);
      }
      if (parameters_.colorSpace == PdfImageColorSpace::IndexedGray) {
        luminance = parameters_.palette[paletteIndex];
      } else {
        const size_t paletteOffset = static_cast<size_t>(paletteIndex) * 3U;
        luminance = static_cast<uint8_t>((77U * parameters_.palette[paletteOffset] +
                                          150U * parameters_.palette[paletteOffset + 1U] +
                                          29U * parameters_.palette[paletteOffset + 2U] + 128U) >>
                                         8U);
      }
    }
    workspace_.outputRow[outputX] = parameters_.hasSoftMask ? luminance : static_cast<uint8_t>(luminance >> 6U);
    advanceCenterMapCursor(&horizontalMap);
  }

  if (!parameters_.hasSoftMask) {
    return writeCurrentOutputRow();
  }
  const CenterMapCursor maskMap = makeCenterMapCursor(parameters_.width, info_.outputWidth);
  maskSourceX_ = maskMap.coordinate;
  maskBaseStep_ = maskMap.baseStep;
  maskStepRemainder_ = maskMap.stepRemainder;
  maskError_ = maskMap.error;
  maskDenominator_ = maskMap.denominator;
  return PdfStatus::success();
}

PdfStatus PdfImageExtractor::writeCurrentOutputRow() {
  const PdfStatus writeStatus = writer_.writeRow(outputRowIndex_, workspace_.outputRow, info_.outputWidth);
  if (!writeStatus.ok()) {
    return fail(writeStatus);
  }
  ++outputRowIndex_;
  if (outputRowIndex_ < info_.outputHeight) {
    nextSelectedSourceRow_ = mapCenter(outputRowIndex_, parameters_.height, info_.outputHeight);
  } else {
    nextSelectedSourceRow_ = parameters_.height;
  }
  return PdfStatus::success();
}

PdfStatus PdfImageExtractor::fail(const PdfStatus status) {
  if (status_.ok()) {
    status_ = status;
  }
  return status_;
}
