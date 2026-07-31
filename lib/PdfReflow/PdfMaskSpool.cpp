#include "PdfMaskSpool.h"

#include <algorithm>
#include <cstring>

#include "PdfCacheFormat.h"

namespace {

constexpr uint8_t kHeaderMagic[] = {'P', 'M', 'S', 'P'};
constexpr size_t kHeaderBytes = 12;
constexpr uint8_t kFooterMagic[] = {'P', 'M', 'E', 'N'};
constexpr size_t kFooterBytes = 24;

void writeLe16(uint8_t* output, const uint16_t value) {
  output[0] = static_cast<uint8_t>(value);
  output[1] = static_cast<uint8_t>(value >> 8U);
}

void writeLe32(uint8_t* output, const uint32_t value) {
  for (uint8_t index = 0; index < 4; ++index) {
    output[index] = static_cast<uint8_t>(value >> (index * 8U));
  }
}

void writeLe64(uint8_t* output, const uint64_t value) {
  for (uint8_t index = 0; index < 8; ++index) {
    output[index] = static_cast<uint8_t>(value >> (index * 8U));
  }
}

uint16_t readLe16(const uint8_t* input) {
  return static_cast<uint16_t>(input[0]) | static_cast<uint16_t>(input[1]) << 8U;
}

uint32_t readLe32(const uint8_t* input) {
  uint32_t value = 0;
  for (uint8_t index = 0; index < 4; ++index) {
    value |= static_cast<uint32_t>(input[index]) << (index * 8U);
  }
  return value;
}

uint64_t readLe64(const uint8_t* input) {
  uint64_t value = 0;
  for (uint8_t index = 0; index < 8; ++index) {
    value |= static_cast<uint64_t>(input[index]) << (index * 8U);
  }
  return value;
}

constexpr size_t kRecordBytes = 60;

void encodeRecord(const PdfMaskSpoolRecord& record, uint8_t* output) {
  writeLe64(output, record.contentHash);
  writeLe64(output + 8, record.baseOffset);
  writeLe64(output + 16, record.baseBytes);
  writeLe64(output + 24, record.alphaOffset);
  writeLe64(output + 32, record.alphaBytes);
  writeLe32(output + 40, record.baseCrc32);
  writeLe32(output + 44, record.alphaCrc32);
  writeLe16(output + 48, record.width);
  writeLe16(output + 50, record.height);
  writeLe32(output + 52, record.sourceCrc32);
  writeLe32(output + 56, pdfCacheCrc32(output, 56));
}

bool decodeRecord(const uint8_t* input, PdfMaskSpoolRecord* record) {
  if (readLe32(input + 56) != pdfCacheCrc32(input, 56)) {
    return false;
  }
  record->contentHash = readLe64(input);
  record->baseOffset = readLe64(input + 8);
  record->baseBytes = readLe64(input + 16);
  record->alphaOffset = readLe64(input + 24);
  record->alphaBytes = readLe64(input + 32);
  record->baseCrc32 = readLe32(input + 40);
  record->alphaCrc32 = readLe32(input + 44);
  record->width = readLe16(input + 48);
  record->height = readLe16(input + 50);
  record->sourceCrc32 = readLe32(input + 52);
  return record->width != 0 && record->height != 0 &&
         record->alphaBytes == static_cast<uint64_t>(record->width) * record->height;
}

uint32_t mappedCenter(const uint32_t output, const uint32_t source, const uint32_t destination) {
  return static_cast<uint32_t>(
      std::min<uint64_t>(source - 1U, (static_cast<uint64_t>(output) * 2U + 1U) * source / (destination * 2U)));
}

uint8_t packedSample(const uint8_t* row, const uint32_t x, const uint8_t bits) {
  if (bits == 8) {
    return row[x];
  }
  return static_cast<uint8_t>((row[x / 8U] >> (7U - x % 8U)) & 1U);
}

}  // namespace

PdfStatus PdfMaskPlaneWriter::begin(const PdfMaskPlaneConfig& config) {
  if (!config.io.valid() || !config.handle.valid() || config.writeOffset == nullptr || config.record == nullptr ||
      config.sourceWidth == 0 || config.sourceHeight == 0 || config.outputWidth == 0 || config.outputHeight == 0 ||
      (config.bitsPerComponent != 1 && config.bitsPerComponent != 8) ||
      (config.predictor != 1 && config.predictor != 2 && (config.predictor < 10 || config.predictor > 15)) ||
      (config.bitsPerComponent == 1 && config.predictor == 2) || config.rowWorkspace == nullptr ||
      config.outputWorkspace == nullptr) {
    return PdfStatus::failure(PdfError::InvalidArgument);
  }
  const size_t rowBytes =
      static_cast<size_t>((static_cast<uint64_t>(config.sourceWidth) * config.bitsPerComponent + 7U) / 8U);
  if (rowBytes == 0 || rowBytes > config.rowWorkspaceBytes || config.outputWidth > config.outputWorkspaceBytes) {
    return PdfStatus::failure(PdfError::InsufficientMemory, rowBytes);
  }
  config_ = config;
  sourceRowBytes_ = rowBytes;
  readingPngFilter_ = config.predictor >= 10;
  if (readingPngFilter_) {
    std::memset(config_.rowWorkspace, 0, sourceRowBytes_);
  }
  nextOutputSourceRow_ = mappedCenter(0, config.sourceHeight, config.outputHeight);
  initialized_ = true;
  return PdfStatus::success();
}

PdfByteSink PdfMaskPlaneWriter::decodedSink() { return initialized_ ? PdfByteSink{this, writeDecoded} : PdfByteSink{}; }

PdfStatus PdfMaskPlaneWriter::writeDecoded(void* context, const uint8_t* source, const size_t requested,
                                           size_t* bytesWritten) {
  return static_cast<PdfMaskPlaneWriter*>(context)->consume(source, requested, bytesWritten);
}

PdfStatus PdfMaskPlaneWriter::consume(const uint8_t* source, const size_t requested, size_t* bytesWritten) {
  if (!initialized_ || bytesWritten == nullptr || (source == nullptr && requested != 0)) {
    return PdfStatus::failure(PdfError::InvalidArgument);
  }
  size_t consumed = 0;
  while (consumed < requested) {
    if (sourceRow_ >= config_.sourceHeight) {
      *bytesWritten = consumed;
      return PdfStatus::failure(PdfError::Malformed, sourceRow_);
    }
    if (readingPngFilter_) {
      pngFilter_ = source[consumed++];
      if (pngFilter_ > 4) {
        return PdfStatus::failure(PdfError::Malformed, sourceRow_);
      }
      pngUpLeft_ = 0;
      readingPngFilter_ = false;
      continue;
    }
    const size_t take = std::min(sourceRowBytes_ - rowPosition_, requested - consumed);
    for (size_t index = 0; index < take; ++index) {
      const size_t position = rowPosition_ + index;
      const uint8_t encoded = source[consumed + index];
      const uint8_t up = config_.rowWorkspace[position];
      const uint8_t left = position == 0 ? 0 : config_.rowWorkspace[position - 1];
      const uint8_t upLeft = position == 0 ? 0 : pngUpLeft_;
      pngUpLeft_ = up;
      uint8_t predictor = 0;
      if (config_.predictor == 2 || pngFilter_ == 1) {
        predictor = left;
      } else if (pngFilter_ == 2) {
        predictor = up;
      } else if (pngFilter_ == 3) {
        predictor = static_cast<uint8_t>((static_cast<uint16_t>(left) + up) / 2U);
      } else if (pngFilter_ == 4) {
        const int value = static_cast<int>(left) + up - upLeft;
        const int dl = value > left ? value - left : left - value;
        const int du = value > up ? value - up : up - value;
        const int dul = value > upLeft ? value - upLeft : upLeft - value;
        predictor = dl <= du && dl <= dul ? left : (du <= dul ? up : upLeft);
      }
      config_.rowWorkspace[position] = static_cast<uint8_t>(encoded + predictor);
    }
    rowPosition_ += take;
    consumed += take;
    if (rowPosition_ == sourceRowBytes_) {
      PdfStatus status = finishRow();
      if (!status) {
        *bytesWritten = consumed;
        return status;
      }
      rowPosition_ = 0;
      ++sourceRow_;
      readingPngFilter_ = config_.predictor >= 10;
    }
  }
  *bytesWritten = consumed;
  return PdfStatus::success();
}

PdfStatus PdfMaskPlaneWriter::finishRow() {
  if (sourceRow_ != nextOutputSourceRow_) {
    return PdfStatus::success();
  }
  for (uint16_t x = 0; x < config_.outputWidth; ++x) {
    const uint32_t sourceX = mappedCenter(x, config_.sourceWidth, config_.outputWidth);
    uint8_t sample = packedSample(config_.rowWorkspace, sourceX, config_.bitsPerComponent);
    if (config_.bitsPerComponent == 8 && config_.decode == PdfImageDecode::Inverted) {
      sample = static_cast<uint8_t>(255U - sample);
    }
    if (config_.explicitMask) {
      sample = sample == 0 ? 255U : 0U;
      if (config_.decode == PdfImageDecode::Inverted) {
        sample = static_cast<uint8_t>(255U - sample);
      }
    }
    config_.outputWorkspace[x] = sample;
  }
  size_t written = 0;
  PdfStatus status =
      config_.io.write(config_.io.context, config_.handle, config_.outputWorkspace, config_.outputWidth, &written);
  if (!status || written != config_.outputWidth) {
    return status ? PdfStatus::failure(PdfError::IoFailure, *config_.writeOffset + written) : status;
  }
  config_.record->alphaCrc32 = pdfCacheCrc32(config_.outputWorkspace, config_.outputWidth, config_.record->alphaCrc32);
  config_.record->alphaBytes += config_.outputWidth;
  *config_.writeOffset += config_.outputWidth;
  ++outputRow_;
  if (outputRow_ < config_.outputHeight) {
    nextOutputSourceRow_ = mappedCenter(outputRow_, config_.sourceHeight, config_.outputHeight);
  }
  return PdfStatus::success();
}

PdfStatus PdfMaskPlaneWriter::finish() {
  if (!initialized_ || rowPosition_ != 0 || sourceRow_ != config_.sourceHeight || outputRow_ != config_.outputHeight) {
    return PdfStatus::failure(PdfError::Malformed, sourceRow_);
  }
  initialized_ = false;
  return PdfStatus::success();
}

PdfStatus PdfMaskSpool::beginWrite(const PdfCacheIo& io, const char* path) {
  if (writing_ || reading_ || !io.valid() || path == nullptr || path[0] == '\0' || std::strlen(path) >= sizeof(path_)) {
    return PdfStatus::failure(PdfError::InvalidArgument);
  }
  recordsOffset_ = 0;
  recordCount_ = 0;
  recordOpen_ = false;
  alphaOpen_ = false;
  for (auto& record : records_) {
    record = {};
  }
  io_ = io;
  std::strcpy(path_, path);
  PdfStatus status = io_.open(io_.context, path_, PdfCacheOpenMode::WriteTruncate, &handle_);
  if (!status) {
    return status;
  }
  writing_ = true;
  uint8_t header[kHeaderBytes]{};
  std::memcpy(header, kHeaderMagic, sizeof(kHeaderMagic));
  writeLe16(header + 4, PDF_MASK_SPOOL_VERSION);
  header[6] = PDF_MASK_SPOOL_MAX_RECORDS;
  writeLe32(header + 8, pdfCacheCrc32(header, 8));
  size_t written = 0;
  status = io_.write(io_.context, handle_, header, sizeof(header), &written);
  if (!status || written != sizeof(header)) {
    const PdfStatus result = status ? PdfStatus::failure(PdfError::IoFailure, written) : status;
    abort();
    return result;
  }
  writeOffset_ = sizeof(header);
  return PdfStatus::success();
}

PdfStatus PdfMaskSpool::beginRecord(const uint64_t contentHash, const uint32_t sourceCrc32, const uint16_t width,
                                    const uint16_t height, PdfByteSink* baseSink) {
  if (!writing_ || recordsOffset_ != 0 || recordOpen_ || baseSink == nullptr ||
      recordCount_ >= PDF_MASK_SPOOL_MAX_RECORDS || width == 0 || height == 0) {
    return PdfStatus::failure(PdfError::InvalidArgument);
  }
  PdfMaskSpoolRecord& record = records_[recordCount_];
  record = {};
  record.contentHash = contentHash;
  record.sourceCrc32 = sourceCrc32;
  record.width = width;
  record.height = height;
  record.baseOffset = writeOffset_;
  recordOpen_ = true;
  *baseSink = {this, writeBase};
  return PdfStatus::success();
}

PdfStatus PdfMaskSpool::writeBase(void* context, const uint8_t* source, const size_t requested, size_t* bytesWritten) {
  if (bytesWritten == nullptr) {
    return PdfStatus::failure(PdfError::InvalidArgument);
  }
  auto& self = *static_cast<PdfMaskSpool*>(context);
  PdfStatus status = self.append(source, requested, &self.records_[self.recordCount_].baseCrc32,
                                 &self.records_[self.recordCount_].baseBytes);
  *bytesWritten = status ? requested : 0;
  return status;
}

PdfStatus PdfMaskSpool::append(const uint8_t* source, const size_t requested, uint32_t* crc, uint64_t* bytesWritten) {
  size_t written = 0;
  PdfStatus status = io_.write(io_.context, handle_, source, requested, &written);
  if (!status || written != requested) {
    return status ? PdfStatus::failure(PdfError::IoFailure, writeOffset_ + written) : status;
  }
  *crc = pdfCacheCrc32(source, requested, *crc);
  *bytesWritten += requested;
  writeOffset_ += requested;
  return PdfStatus::success();
}

PdfStatus PdfMaskSpool::beginAlpha(const PdfMaskPlaneConfig& config, PdfMaskPlaneWriter* plane) {
  if (!recordOpen_ || alphaOpen_ || plane == nullptr || config.outputWidth != records_[recordCount_].width ||
      config.outputHeight != records_[recordCount_].height) {
    return PdfStatus::failure(PdfError::InvalidArgument);
  }
  records_[recordCount_].alphaOffset = writeOffset_;
  PdfMaskPlaneConfig actual = config;
  actual.io = io_;
  actual.handle = handle_;
  actual.writeOffset = &writeOffset_;
  actual.record = &records_[recordCount_];
  PdfStatus status = plane->begin(actual);
  alphaOpen_ = status.ok();
  return status;
}

PdfStatus PdfMaskSpool::finishRecord() {
  PdfMaskSpoolRecord& record = records_[recordCount_];
  if (!recordOpen_ || !alphaOpen_ || record.baseBytes == 0 ||
      record.alphaBytes != static_cast<uint64_t>(record.width) * record.height) {
    return PdfStatus::failure(PdfError::Malformed, recordCount_);
  }
  recordOpen_ = false;
  alphaOpen_ = false;
  ++recordCount_;
  return PdfStatus::success();
}

PdfStatus PdfMaskSpool::beginCloseWrite(PdfMaskSpoolCloseRuntime* const runtime) {
  if (!writing_ || recordOpen_ || recordCount_ == 0 || recordsOffset_ != 0 || runtime == nullptr ||
      runtime->stage != PdfMaskSpoolCloseStage::Idle) {
    return PdfStatus::failure(PdfError::InvalidArgument);
  }
  recordsOffset_ = writeOffset_;
  *runtime = {};
  runtime->stage = PdfMaskSpoolCloseStage::Records;
  return PdfStatus::success();
}

PdfStepResult PdfMaskSpool::stepCloseWrite(PdfMaskSpoolCloseRuntime& runtime, PdfWorkBudget& budget) {
  const auto fail = [&](const PdfStatus status) {
    abort();
    runtime = {};
    return PdfStepResult::failure(status);
  };
  if (runtime.stage == PdfMaskSpoolCloseStage::Complete) {
    return PdfStepResult::completed();
  }
  if (!writing_ || !handle_.valid() || runtime.stage == PdfMaskSpoolCloseStage::Idle) {
    return fail(PdfStatus::failure(PdfError::InvalidArgument));
  }

  if (runtime.stage == PdfMaskSpoolCloseStage::Records) {
    if (runtime.recordIndex >= recordCount_) {
      runtime.stage = PdfMaskSpoolCloseStage::Footer;
      return PdfStepResult::paused();
    }
    if (budget.bytesRemaining < kRecordBytes || !budget.consumeOperation()) {
      return PdfStepResult::paused();
    }
    (void)budget.takeBytes(kRecordBytes);
    uint8_t encoded[kRecordBytes]{};
    encodeRecord(records_[runtime.recordIndex], encoded);
    size_t written = 0;
    const PdfStatus status = io_.write(io_.context, handle_, encoded, sizeof(encoded), &written);
    if (!status || written != sizeof(encoded)) {
      return fail(status ? PdfStatus::failure(PdfError::IoFailure, writeOffset_ + written) : status);
    }
    runtime.recordsCrc = pdfCacheCrc32(encoded, sizeof(encoded), runtime.recordsCrc);
    writeOffset_ += sizeof(encoded);
    ++runtime.recordIndex;
    return PdfStepResult::paused();
  }

  if (runtime.stage == PdfMaskSpoolCloseStage::Footer) {
    if (budget.bytesRemaining < kFooterBytes || !budget.consumeOperation()) {
      return PdfStepResult::paused();
    }
    (void)budget.takeBytes(kFooterBytes);
    uint8_t footer[kFooterBytes]{};
    std::memcpy(footer, kFooterMagic, sizeof(kFooterMagic));
    writeLe16(footer + 4, PDF_MASK_SPOOL_VERSION);
    footer[6] = recordCount_;
    writeLe64(footer + 8, recordsOffset_);
    writeLe32(footer + 16, runtime.recordsCrc);
    writeLe32(footer + 20, pdfCacheCrc32(footer, 20));
    size_t written = 0;
    const PdfStatus status = io_.write(io_.context, handle_, footer, sizeof(footer), &written);
    if (!status || written != sizeof(footer)) {
      return fail(status ? PdfStatus::failure(PdfError::IoFailure, writeOffset_ + written) : status);
    }
    writeOffset_ += sizeof(footer);
    runtime.stage = PdfMaskSpoolCloseStage::Flush;
    return PdfStepResult::paused();
  }

  if (runtime.stage == PdfMaskSpoolCloseStage::Flush) {
    if (!budget.consumeOperation()) {
      return PdfStepResult::paused();
    }
    const PdfStatus status = io_.flush(io_.context, handle_);
    if (!status) {
      return fail(status);
    }
    runtime.stage = PdfMaskSpoolCloseStage::Sync;
    return PdfStepResult::paused();
  }

  if (runtime.stage == PdfMaskSpoolCloseStage::Sync) {
    if (!budget.consumeOperation()) {
      return PdfStepResult::paused();
    }
    const PdfStatus status = io_.sync(io_.context, handle_);
    if (!status) {
      return fail(status);
    }
    runtime.stage = PdfMaskSpoolCloseStage::Close;
    return PdfStepResult::paused();
  }

  if (runtime.stage == PdfMaskSpoolCloseStage::Close) {
    if (!budget.consumeOperation()) {
      return PdfStepResult::paused();
    }
    const PdfStatus status = io_.close(io_.context, &handle_);
    writing_ = false;
    if (!status) {
      (void)io_.remove(io_.context, path_, false);
      runtime = {};
      return PdfStepResult::failure(status);
    }
    runtime.stage = PdfMaskSpoolCloseStage::Complete;
    return PdfStepResult::completed();
  }

  return fail(PdfStatus::failure(PdfError::InvalidArgument));
}

PdfStatus PdfMaskSpool::closeWrite() {
  PdfMaskSpoolCloseRuntime runtime{};
  PdfStatus status = beginCloseWrite(&runtime);
  if (!status) {
    return status;
  }
  for (uint8_t step = 0; step < PDF_MASK_SPOOL_MAX_RECORDS + 8U; ++step) {
    PdfWorkBudget budget{1, kRecordBytes};
    const PdfStepResult result = stepCloseWrite(runtime, budget);
    if (result.failed()) {
      return result.status;
    }
    if (result.complete()) {
      return PdfStatus::success();
    }
  }
  abort();
  return PdfStatus::failure(PdfError::BudgetExhausted);
}

PdfStatus PdfMaskSpool::beginRead(const PdfCacheIo& io, const char* path, uint8_t* ioWorkspace,
                                  const size_t ioWorkspaceBytes, PdfMaskSpoolReadRuntime* const runtime) {
  if (writing_ || reading_ || runtime == nullptr || runtime->stage != PdfMaskSpoolReadStage::Idle || !io.valid() ||
      path == nullptr || ioWorkspace == nullptr || path[0] == '\0' || std::strlen(path) >= sizeof(path_) ||
      ioWorkspaceBytes < kRecordBytes) {
    return PdfStatus::failure(PdfError::InvalidArgument);
  }
  io_ = io;
  std::strcpy(path_, path);
  ioWorkspace_ = ioWorkspace;
  ioWorkspaceBytes_ = ioWorkspaceBytes;
  PdfStatus status = io_.open(io_.context, path, PdfCacheOpenMode::Read, &handle_);
  PdfCacheFileMetadata metadata{};
  if (status) {
    status = io_.metadata(io_.context, handle_, &metadata);
  }
  if (!status || metadata.size < kHeaderBytes + kFooterBytes) {
    const PdfStatus result = status ? PdfStatus::failure(PdfError::Malformed) : status;
    abort();
    *runtime = {};
    return result;
  }
  *runtime = {};
  runtime->metadataSize = metadata.size;
  runtime->stage = PdfMaskSpoolReadStage::Header;
  return PdfStatus::success();
}

PdfStepResult PdfMaskSpool::stepReadOpen(PdfMaskSpoolReadRuntime& runtime, PdfWorkBudget& budget) {
  const auto fail = [&](const PdfStatus status) {
    abort();
    runtime = {};
    return PdfStepResult::failure(status.ok() ? PdfStatus::failure(PdfError::Malformed) : status);
  };
  if (!handle_.valid() || ioWorkspace_ == nullptr || runtime.stage == PdfMaskSpoolReadStage::Idle) {
    return fail(PdfStatus::failure(PdfError::InvalidArgument));
  }

  if (runtime.stage == PdfMaskSpoolReadStage::Complete) {
    return PdfStepResult::completed();
  }

  if (runtime.stage == PdfMaskSpoolReadStage::Header) {
    if (budget.bytesRemaining < kHeaderBytes || !budget.consumeOperation()) {
      return PdfStepResult::paused();
    }
    (void)budget.takeBytes(kHeaderBytes);
    size_t bytesRead = 0;
    const PdfStatus status = io_.read(io_.context, handle_, 0, ioWorkspace_, kHeaderBytes, &bytesRead);
    if (!status || bytesRead != kHeaderBytes || std::memcmp(ioWorkspace_, kHeaderMagic, 4) != 0 ||
        readLe16(ioWorkspace_ + 4) != PDF_MASK_SPOOL_VERSION || ioWorkspace_[6] != PDF_MASK_SPOOL_MAX_RECORDS ||
        readLe32(ioWorkspace_ + 8) != pdfCacheCrc32(ioWorkspace_, 8)) {
      return fail(status ? PdfStatus::failure(PdfError::Malformed) : status);
    }
    runtime.stage = PdfMaskSpoolReadStage::Footer;
    return PdfStepResult::paused();
  }

  if (runtime.stage == PdfMaskSpoolReadStage::Footer) {
    if (budget.bytesRemaining < kFooterBytes || !budget.consumeOperation()) {
      return PdfStepResult::paused();
    }
    (void)budget.takeBytes(kFooterBytes);
    size_t bytesRead = 0;
    const PdfStatus status =
        io_.read(io_.context, handle_, runtime.metadataSize - kFooterBytes, ioWorkspace_, kFooterBytes, &bytesRead);
    if (!status || bytesRead != kFooterBytes || std::memcmp(ioWorkspace_, kFooterMagic, 4) != 0 ||
        readLe16(ioWorkspace_ + 4) != PDF_MASK_SPOOL_VERSION ||
        readLe32(ioWorkspace_ + 20) != pdfCacheCrc32(ioWorkspace_, 20)) {
      return fail(status ? PdfStatus::failure(PdfError::Malformed) : status);
    }
    recordCount_ = ioWorkspace_[6];
    recordsOffset_ = readLe64(ioWorkspace_ + 8);
    runtime.expectedRecordsCrc = readLe32(ioWorkspace_ + 16);
    if (recordCount_ == 0 || recordCount_ > PDF_MASK_SPOOL_MAX_RECORDS ||
        recordsOffset_ + static_cast<uint64_t>(recordCount_) * kRecordBytes + kFooterBytes != runtime.metadataSize) {
      return fail(PdfStatus::failure(PdfError::Malformed));
    }
    runtime.expectedPayloadOffset = kHeaderBytes;
    runtime.recordIndex = 0;
    runtime.recordsCrc = 0;
    runtime.stage = PdfMaskSpoolReadStage::Records;
    return PdfStepResult::paused();
  }

  if (runtime.stage == PdfMaskSpoolReadStage::Records) {
    if (runtime.recordIndex < recordCount_) {
      if (budget.bytesRemaining < kRecordBytes || !budget.consumeOperation()) {
        return PdfStepResult::paused();
      }
      (void)budget.takeBytes(kRecordBytes);
      size_t bytesRead = 0;
      const uint64_t offset = recordsOffset_ + static_cast<uint64_t>(runtime.recordIndex) * kRecordBytes;
      const PdfStatus status = io_.read(io_.context, handle_, offset, ioWorkspace_, kRecordBytes, &bytesRead);
      if (!status || bytesRead != kRecordBytes || !decodeRecord(ioWorkspace_, &records_[runtime.recordIndex])) {
        return fail(status ? PdfStatus::failure(PdfError::Malformed, runtime.recordIndex) : status);
      }
      const PdfMaskSpoolRecord& record = records_[runtime.recordIndex];
      if (record.baseOffset != runtime.expectedPayloadOffset || record.baseBytes == 0 ||
          record.alphaOffset != record.baseOffset + record.baseBytes ||
          record.alphaOffset + record.alphaBytes > recordsOffset_) {
        return fail(PdfStatus::failure(PdfError::Malformed, runtime.recordIndex));
      }
      runtime.expectedPayloadOffset = record.alphaOffset + record.alphaBytes;
      runtime.recordsCrc = pdfCacheCrc32(ioWorkspace_, kRecordBytes, runtime.recordsCrc);
      ++runtime.recordIndex;
      return PdfStepResult::paused();
    }
    if (runtime.recordsCrc != runtime.expectedRecordsCrc || runtime.expectedPayloadOffset != recordsOffset_) {
      return fail(PdfStatus::failure(PdfError::Malformed));
    }
    runtime.recordIndex = 0;
    runtime.planeIndex = 0;
    runtime.planeConsumed = 0;
    runtime.planeCrc = 0;
    runtime.stage = PdfMaskSpoolReadStage::Planes;
    return PdfStepResult::paused();
  }

  if (runtime.stage == PdfMaskSpoolReadStage::Planes) {
    if (runtime.recordIndex >= recordCount_) {
      reading_ = true;
      runtime.stage = PdfMaskSpoolReadStage::Complete;
      return PdfStepResult::completed();
    }
    const PdfMaskSpoolRecord& record = records_[runtime.recordIndex];
    const uint64_t planeOffset = runtime.planeIndex == 0 ? record.baseOffset : record.alphaOffset;
    const uint64_t planeBytes = runtime.planeIndex == 0 ? record.baseBytes : record.alphaBytes;
    const uint32_t expectedCrc = runtime.planeIndex == 0 ? record.baseCrc32 : record.alphaCrc32;
    if (runtime.planeConsumed < planeBytes) {
      if (budget.bytesRemaining == 0 || !budget.consumeOperation()) {
        return PdfStepResult::paused();
      }
      const size_t requested = static_cast<size_t>(std::min<uint64_t>(
          std::min<size_t>(ioWorkspaceBytes_, budget.bytesRemaining), planeBytes - runtime.planeConsumed));
      if (requested == 0) {
        return PdfStepResult::paused();
      }
      (void)budget.takeBytes(requested);
      size_t bytesRead = 0;
      const PdfStatus status =
          io_.read(io_.context, handle_, planeOffset + runtime.planeConsumed, ioWorkspace_, requested, &bytesRead);
      if (!status || bytesRead != requested) {
        return fail(status ? PdfStatus::failure(PdfError::Malformed, runtime.recordIndex) : status);
      }
      runtime.planeCrc = pdfCacheCrc32(ioWorkspace_, bytesRead, runtime.planeCrc);
      runtime.planeConsumed += bytesRead;
      return PdfStepResult::paused();
    }
    if (runtime.planeCrc != expectedCrc) {
      return fail(PdfStatus::failure(PdfError::Malformed, runtime.recordIndex));
    }
    runtime.planeConsumed = 0;
    runtime.planeCrc = 0;
    if (++runtime.planeIndex == 2) {
      runtime.planeIndex = 0;
      ++runtime.recordIndex;
    }
    return PdfStepResult::paused();
  }

  return fail(PdfStatus::failure(PdfError::InvalidArgument));
}

PdfStatus PdfMaskSpool::read(const uint64_t offset, uint8_t* destination, const size_t requested,
                             size_t* bytesRead) const {
  if (!reading_) {
    return PdfStatus::failure(PdfError::InvalidArgument);
  }
  return io_.read(io_.context, handle_, offset, destination, requested, bytesRead);
}

PdfStatus PdfMaskSpool::closeRead() {
  if (!reading_) {
    return PdfStatus::success();
  }
  reading_ = false;
  return io_.close(io_.context, &handle_);
}

void PdfMaskSpool::abort() {
  if (handle_.valid() && io_.close != nullptr) {
    (void)io_.close(io_.context, &handle_);
  }
  writing_ = false;
  reading_ = false;
  recordOpen_ = false;
  alphaOpen_ = false;
  if (path_[0] != '\0' && io_.remove != nullptr) {
    (void)io_.remove(io_.context, path_, false);
  }
}
