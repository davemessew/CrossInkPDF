#include "PdfImageBuildSpool.h"

#include <algorithm>
#include <cstring>
#include <new>

#include "PdfCacheFormat.h"

namespace {

constexpr uint8_t kHeaderMagic[] = {'P', 'I', 'B', 'S'};
constexpr size_t kHeaderBytes = 16;
constexpr uint8_t kFooterMagic[] = {'P', 'I', 'B', 'E'};
constexpr size_t kFooterBytes = 24;
constexpr uint8_t kFileHeaderMagic[] = {'P', 'I', 'F', 'S'};
constexpr uint8_t kFileFooterMagic[] = {'P', 'I', 'F', 'E'};

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

bool bytesAreZero(const uint8_t* const input, const size_t length) {
  for (size_t index = 0; index < length; ++index) {
    if (input[index] != 0) {
      return false;
    }
  }
  return true;
}

bool validRecord(const PdfDeferredImageRecord& record) {
  const uint64_t tagEnd = static_cast<uint64_t>(record.tagOffset) + record.tagLength;
  return record.reference.objectNumber != 0 && record.streamLength != 0 && record.width != 0 && record.height != 0 &&
         record.filterCount <= PdfLimits::MaxFiltersPerStream &&
         record.auxiliaryFilterCount <= PdfLimits::MaxFiltersPerStream &&
         record.paletteBytes <= sizeof(record.palette) && record.tagLength != 0 && tagEnd <= UINT32_MAX &&
         (!record.hasAuxiliary ||
          (record.auxiliaryReference.objectNumber != 0 && record.auxiliaryStreamLength != 0 &&
           record.auxiliaryWidth != 0 && record.auxiliaryHeight != 0));
}

void encodeReference(const PdfObjectReference reference, uint8_t* const output) {
  writeLe32(output, reference.objectNumber);
  writeLe16(output + 4, reference.generation);
}

PdfObjectReference decodeReference(const uint8_t* const input) {
  return {readLe32(input), readLe16(input + 4)};
}

void encodeRecord(const PdfDeferredImageRecord& record, uint8_t* output) {
  std::memset(output, 0, PDF_IMAGE_BUILD_RECORD_BYTES);
  writeLe64(output, record.streamOffset);
  writeLe64(output + 8, record.streamLength);
  writeLe64(output + 16, record.contentHash);
  writeLe64(output + 24, record.auxiliaryStreamOffset);
  writeLe64(output + 32, record.auxiliaryStreamLength);
  writeLe32(output + 40, record.sourceCrc32);
  writeLe32(output + 44, record.width);
  writeLe32(output + 48, record.height);
  writeLe32(output + 52, record.auxiliaryWidth);
  writeLe32(output + 56, record.auxiliaryHeight);
  output[60] = record.bitsPerComponent;
  output[61] = record.predictor;
  output[62] = static_cast<uint8_t>(record.colorSpace);
  output[63] = static_cast<uint8_t>(record.decode);
  output[64] = record.filterCount;
  for (uint8_t index = 0; index < PdfLimits::MaxFiltersPerStream; ++index) {
    output[65 + index] = static_cast<uint8_t>(record.filters[index]);
  }
  output[69] = record.auxiliaryBitsPerComponent;
  output[70] = record.auxiliaryPredictor;
  output[71] = static_cast<uint8_t>(record.auxiliaryColorSpace);
  output[72] = static_cast<uint8_t>(record.auxiliaryDecode);
  output[73] = record.auxiliaryFilterCount;
  for (uint8_t index = 0; index < PdfLimits::MaxFiltersPerStream; ++index) {
    output[74 + index] = static_cast<uint8_t>(record.auxiliaryFilters[index]);
  }
  output[78] = static_cast<uint8_t>(static_cast<uint8_t>(record.auxiliaryKind) | (record.hasAuxiliary ? 0x80U : 0U));
  output[79] = record.imageMaskPaintLuminance;
  writeLe16(output + 80, record.paletteBytes);
  writeLe16(output + 82, record.paletteEntries);
  std::memcpy(output + 84, record.palette, record.paletteBytes);
  writeLe16(output + 852, record.sectionIndex);
  writeLe16(output + 854, record.tagLength);
  writeLe32(output + 856, record.tagOffset);
  encodeReference(record.reference, output + 860);
  encodeReference(record.auxiliaryReference, output + 866);
  writeLe32(output + PDF_IMAGE_BUILD_RECORD_BYTES - 4U, pdfCacheCrc32(output, PDF_IMAGE_BUILD_RECORD_BYTES - 4U));
}

bool decodeRecord(const uint8_t* input, PdfDeferredImageRecord* record) {
  if (readLe32(input + PDF_IMAGE_BUILD_RECORD_BYTES - 4U) !=
          pdfCacheCrc32(input, PDF_IMAGE_BUILD_RECORD_BYTES - 4U) ||
      !bytesAreZero(input + 872, 4)) {
    return false;
  }
  record->~PdfDeferredImageRecord();
  new (record) PdfDeferredImageRecord();
  record->streamOffset = readLe64(input);
  record->streamLength = readLe64(input + 8);
  record->contentHash = readLe64(input + 16);
  record->auxiliaryStreamOffset = readLe64(input + 24);
  record->auxiliaryStreamLength = readLe64(input + 32);
  record->sourceCrc32 = readLe32(input + 40);
  record->width = readLe32(input + 44);
  record->height = readLe32(input + 48);
  record->auxiliaryWidth = readLe32(input + 52);
  record->auxiliaryHeight = readLe32(input + 56);
  record->bitsPerComponent = input[60];
  record->predictor = input[61];
  record->colorSpace = static_cast<PdfImageColorSpace>(input[62]);
  record->decode = static_cast<PdfImageDecode>(input[63]);
  record->filterCount = input[64];
  for (uint8_t index = 0; index < PdfLimits::MaxFiltersPerStream; ++index) {
    record->filters[index] = static_cast<PdfStreamFilter>(input[65 + index]);
  }
  record->auxiliaryBitsPerComponent = input[69];
  record->auxiliaryPredictor = input[70];
  record->auxiliaryColorSpace = static_cast<PdfImageColorSpace>(input[71]);
  record->auxiliaryDecode = static_cast<PdfImageDecode>(input[72]);
  record->auxiliaryFilterCount = input[73];
  for (uint8_t index = 0; index < PdfLimits::MaxFiltersPerStream; ++index) {
    record->auxiliaryFilters[index] = static_cast<PdfStreamFilter>(input[74 + index]);
  }
  record->auxiliaryKind = static_cast<PdfImageAuxiliaryKind>(input[78] & 0x7FU);
  record->hasAuxiliary = (input[78] & 0x80U) != 0;
  record->imageMaskPaintLuminance = input[79];
  record->paletteBytes = readLe16(input + 80);
  record->paletteEntries = readLe16(input + 82);
  if (record->paletteBytes > sizeof(record->palette)) {
    return false;
  }
  std::memcpy(record->palette, input + 84, record->paletteBytes);
  record->sectionIndex = readLe16(input + 852);
  record->tagLength = readLe16(input + 854);
  record->tagOffset = readLe32(input + 856);
  record->reference = decodeReference(input + 860);
  record->auxiliaryReference = decodeReference(input + 866);
  return validRecord(*record);
}

bool validEncodedRecord(const uint8_t* input) {
  if (readLe32(input + PDF_IMAGE_BUILD_RECORD_BYTES - 4U) !=
          pdfCacheCrc32(input, PDF_IMAGE_BUILD_RECORD_BYTES - 4U) ||
      !bytesAreZero(input + 872, 4)) {
    return false;
  }
  const uint8_t filterCount = input[64];
  const uint8_t auxiliaryFilterCount = input[73];
  const bool hasAuxiliary = (input[78] & 0x80U) != 0;
  const uint16_t paletteBytes = readLe16(input + 80);
  const uint16_t tagLength = readLe16(input + 854);
  const uint32_t tagOffset = readLe32(input + 856);
  const PdfObjectReference reference = decodeReference(input + 860);
  const PdfObjectReference auxiliaryReference = decodeReference(input + 866);
  return reference.objectNumber != 0 && readLe64(input + 8) != 0 && readLe32(input + 44) != 0 &&
         readLe32(input + 48) != 0 &&
         filterCount <= PdfLimits::MaxFiltersPerStream && auxiliaryFilterCount <= PdfLimits::MaxFiltersPerStream &&
         paletteBytes <= PDF_IMAGE_BUILD_PALETTE_BYTES && tagLength != 0 &&
         static_cast<uint64_t>(tagOffset) + tagLength <= UINT32_MAX &&
         (!hasAuxiliary ||
          (auxiliaryReference.objectNumber != 0 && readLe64(input + 32) != 0 && readLe32(input + 52) != 0 &&
           readLe32(input + 56) != 0));
}

bool validFileRecord(const PdfRequiredFileRecord& record) {
  return record.pathLength != 0 && record.pathLength < sizeof(record.path) && record.path[record.pathLength] == '\0' &&
         record.size != 0;
}

void encodeFileRecord(const PdfRequiredFileRecord& record, uint8_t* output) {
  std::memset(output, 0, PDF_IMAGE_FILE_RECORD_BYTES);
  std::memcpy(output, record.path, record.pathLength);
  output[96] = record.pathLength;
  writeLe64(output + 100, record.size);
  writeLe32(output + 108, record.crc32);
  writeLe32(output + 112, pdfCacheCrc32(output, 112));
}

bool decodeFileRecord(const uint8_t* input, PdfRequiredFileRecord* record) {
  if (readLe32(input + 112) != pdfCacheCrc32(input, 112) || input[96] == 0 || input[96] >= sizeof(record->path)) {
    return false;
  }
  record->~PdfRequiredFileRecord();
  new (record) PdfRequiredFileRecord();
  record->pathLength = input[96];
  std::memcpy(record->path, input, record->pathLength);
  record->path[record->pathLength] = '\0';
  record->size = readLe64(input + 100);
  record->crc32 = readLe32(input + 108);
  return validFileRecord(*record);
}

}  // namespace

PdfStatus PdfImageBuildSpool::beginWrite(const PdfCacheIo& io, const char* path, uint8_t* const workspace,
                                         const size_t workspaceBytes) {
  if (writing_ || reading_ || !io.valid() || path == nullptr || path[0] == '\0' || std::strlen(path) >= sizeof(path_) ||
      workspace == nullptr || workspaceBytes < PDF_IMAGE_BUILD_RECORD_BYTES) {
    return PdfStatus::failure(PdfError::InvalidArgument);
  }
  io_ = io;
  std::strcpy(path_, path);
  workspace_ = workspace;
  workspaceBytes_ = workspaceBytes;
  recordCount_ = 0;
  recordsCrc32_ = 0;
  validated_ = false;
  PdfStatus status = io_.open(io_.context, path_, PdfCacheOpenMode::WriteTruncate, &handle_);
  if (!status) {
    return status;
  }
  writing_ = true;
  uint8_t header[kHeaderBytes]{};
  std::memcpy(header, kHeaderMagic, sizeof(kHeaderMagic));
  writeLe16(header + 4, PDF_IMAGE_BUILD_SPOOL_VERSION);
  header[6] = PDF_IMAGE_BUILD_SPOOL_MAX_RECORDS;
  writeLe16(header + 8, static_cast<uint16_t>(PDF_IMAGE_BUILD_RECORD_BYTES));
  writeLe32(header + 12, pdfCacheCrc32(header, 12));
  size_t written = 0;
  status = io_.write(io_.context, handle_, header, sizeof(header), &written);
  if (!status || written != sizeof(header)) {
    const PdfStatus result = status ? PdfStatus::failure(PdfError::IoFailure, written) : status;
    abort();
    return result;
  }
  return PdfStatus::success();
}

PdfStatus PdfImageBuildSpool::append(const PdfDeferredImageRecord& record) {
  if (!writing_ || recordCount_ >= PDF_IMAGE_BUILD_SPOOL_MAX_RECORDS || !validRecord(record)) {
    return PdfStatus::failure(
        recordCount_ >= PDF_IMAGE_BUILD_SPOOL_MAX_RECORDS ? PdfError::LimitExceeded : PdfError::InvalidArgument,
        recordCount_);
  }
  encodeRecord(record, workspace_);
  size_t written = 0;
  const PdfStatus status = io_.write(io_.context, handle_, workspace_, PDF_IMAGE_BUILD_RECORD_BYTES, &written);
  if (!status || written != PDF_IMAGE_BUILD_RECORD_BYTES) {
    return status ? PdfStatus::failure(PdfError::IoFailure, written) : status;
  }
  recordsCrc32_ = pdfCacheCrc32(workspace_, PDF_IMAGE_BUILD_RECORD_BYTES, recordsCrc32_);
  ++recordCount_;
  return PdfStatus::success();
}

PdfStatus PdfImageBuildSpool::closeWrite() {
  if (!writing_ || recordCount_ == 0) {
    return PdfStatus::failure(PdfError::InvalidArgument);
  }
  uint8_t footer[kFooterBytes]{};
  std::memcpy(footer, kFooterMagic, sizeof(kFooterMagic));
  writeLe16(footer + 4, PDF_IMAGE_BUILD_SPOOL_VERSION);
  footer[6] = recordCount_;
  writeLe64(footer + 8, kHeaderBytes);
  writeLe32(footer + 16, recordsCrc32_);
  writeLe32(footer + 20, pdfCacheCrc32(footer, 20));
  size_t written = 0;
  PdfStatus status = io_.write(io_.context, handle_, footer, sizeof(footer), &written);
  if (status && written != sizeof(footer)) {
    status = PdfStatus::failure(PdfError::IoFailure, written);
  }
  if (status) {
    status = io_.flush(io_.context, handle_);
  }
  if (status) {
    status = io_.sync(io_.context, handle_);
  }
  const PdfStatus closeStatus = io_.close(io_.context, &handle_);
  writing_ = false;
  if (!status || !closeStatus) {
    const PdfStatus result = status ? closeStatus : status;
    remove();
    return result;
  }
  return PdfStatus::success();
}

PdfStatus PdfImageBuildSpool::beginRead(const PdfCacheIo& io, const char* path, uint8_t* workspace,
                                        const size_t workspaceBytes, PdfImageSpoolReadRuntime* const runtime) {
  if (writing_ || reading_ || !io.valid() || path == nullptr || path[0] == '\0' || std::strlen(path) >= sizeof(path_) ||
      workspace == nullptr || workspaceBytes < PDF_IMAGE_BUILD_RECORD_BYTES || runtime == nullptr ||
      runtime->stage != PdfImageSpoolReadStage::Idle) {
    return PdfStatus::failure(PdfError::InvalidArgument);
  }
  io_ = io;
  std::strcpy(path_, path);
  workspace_ = workspace;
  workspaceBytes_ = workspaceBytes;
  PdfStatus status = io_.open(io_.context, path_, PdfCacheOpenMode::Read, &handle_);
  PdfCacheFileMetadata metadata{};
  if (status) {
    status = io_.metadata(io_.context, handle_, &metadata);
  }
  if (!status || metadata.directory || metadata.symlinkLike ||
      metadata.size < kHeaderBytes + PDF_IMAGE_BUILD_RECORD_BYTES + kFooterBytes) {
    const PdfStatus result = status ? PdfStatus::failure(PdfError::Malformed) : status;
    abort();
    return result;
  }
  *runtime = {};
  runtime->fileBytes = metadata.size;
  runtime->stage = PdfImageSpoolReadStage::Header;
  return PdfStatus::success();
}

PdfStepResult PdfImageBuildSpool::stepReadOpen(PdfImageSpoolReadRuntime& runtime, PdfWorkBudget& budget) {
  const auto failRead = [&](PdfStatus status) {
    abort();
    runtime = {};
    return PdfStepResult::failure(status);
  };
  if (!handle_.valid() || workspace_ == nullptr || workspaceBytes_ < PDF_IMAGE_BUILD_RECORD_BYTES ||
      runtime.stage == PdfImageSpoolReadStage::Idle) {
    return PdfStepResult::failure(PdfStatus::failure(PdfError::InvalidArgument));
  }
  if (runtime.stage == PdfImageSpoolReadStage::Complete) {
    return PdfStepResult::completed();
  }
  if (runtime.stage == PdfImageSpoolReadStage::Header) {
    if (!budget.consumeOperation() || budget.takeBytes(kHeaderBytes) != kHeaderBytes) {
      return PdfStepResult::paused();
    }
    size_t bytesRead = 0;
    PdfStatus status = io_.read(io_.context, handle_, 0, workspace_, kHeaderBytes, &bytesRead);
    if (!status || bytesRead != kHeaderBytes || std::memcmp(workspace_, kHeaderMagic, 4) != 0 ||
        readLe16(workspace_ + 4) != PDF_IMAGE_BUILD_SPOOL_VERSION ||
        workspace_[6] != PDF_IMAGE_BUILD_SPOOL_MAX_RECORDS ||
        readLe16(workspace_ + 8) != PDF_IMAGE_BUILD_RECORD_BYTES ||
        readLe32(workspace_ + 12) != pdfCacheCrc32(workspace_, 12)) {
      return failRead(status ? PdfStatus::failure(PdfError::Malformed) : status);
    }
    runtime.stage = PdfImageSpoolReadStage::Footer;
    return PdfStepResult::paused();
  }
  if (runtime.stage == PdfImageSpoolReadStage::Footer) {
    if (!budget.consumeOperation() || budget.takeBytes(kFooterBytes) != kFooterBytes) {
      return PdfStepResult::paused();
    }
    size_t bytesRead = 0;
    PdfStatus status =
        io_.read(io_.context, handle_, runtime.fileBytes - kFooterBytes, workspace_, kFooterBytes, &bytesRead);
    if (!status || bytesRead != kFooterBytes || std::memcmp(workspace_, kFooterMagic, 4) != 0 ||
        readLe16(workspace_ + 4) != PDF_IMAGE_BUILD_SPOOL_VERSION || readLe64(workspace_ + 8) != kHeaderBytes ||
        readLe32(workspace_ + 20) != pdfCacheCrc32(workspace_, 20)) {
      return failRead(status ? PdfStatus::failure(PdfError::Malformed) : status);
    }
    recordCount_ = workspace_[6];
    const uint64_t expectedSize =
        kHeaderBytes + static_cast<uint64_t>(recordCount_) * PDF_IMAGE_BUILD_RECORD_BYTES + kFooterBytes;
    if (recordCount_ == 0 || recordCount_ > PDF_IMAGE_BUILD_SPOOL_MAX_RECORDS || runtime.fileBytes != expectedSize) {
      return failRead(PdfStatus::failure(PdfError::Malformed));
    }
    runtime.expectedRecordsCrc32 = readLe32(workspace_ + 16);
    runtime.recordsCrc32 = 0;
    runtime.nextRecord = 0;
    runtime.stage = PdfImageSpoolReadStage::Records;
    return PdfStepResult::paused();
  }
  if (runtime.nextRecord < recordCount_) {
    if (!budget.consumeOperation() || budget.takeBytes(PDF_IMAGE_BUILD_RECORD_BYTES) != PDF_IMAGE_BUILD_RECORD_BYTES) {
      return PdfStepResult::paused();
    }
    PdfStatus status = readEncodedBytes(handle_, runtime.nextRecord);
    if (!status || !validEncodedRecord(workspace_)) {
      return failRead(status ? PdfStatus::failure(PdfError::Malformed, runtime.nextRecord) : status);
    }
    runtime.recordsCrc32 = pdfCacheCrc32(workspace_, PDF_IMAGE_BUILD_RECORD_BYTES, runtime.recordsCrc32);
    ++runtime.nextRecord;
    return PdfStepResult::paused();
  }
  if (runtime.recordsCrc32 != runtime.expectedRecordsCrc32) {
    return failRead(PdfStatus::failure(PdfError::Malformed));
  }
  reading_ = true;
  validated_ = true;
  runtime.stage = PdfImageSpoolReadStage::Complete;
  return PdfStepResult::completed();
}

PdfStatus PdfImageBuildSpool::readEncodedBytes(const PdfCacheHandle handle, const uint8_t index) const {
  if (!handle.valid() || workspace_ == nullptr || workspaceBytes_ < PDF_IMAGE_BUILD_RECORD_BYTES ||
      index >= recordCount_) {
    return PdfStatus::failure(PdfError::InvalidArgument, index);
  }
  size_t bytesRead = 0;
  const uint64_t offset = kHeaderBytes + static_cast<uint64_t>(index) * PDF_IMAGE_BUILD_RECORD_BYTES;
  const PdfStatus status = io_.read(io_.context, handle, offset, workspace_, PDF_IMAGE_BUILD_RECORD_BYTES, &bytesRead);
  if (!status || bytesRead != PDF_IMAGE_BUILD_RECORD_BYTES) {
    return status ? PdfStatus::failure(PdfError::Malformed, index) : status;
  }
  return PdfStatus::success();
}

PdfStatus PdfImageBuildSpool::readEncodedRecord(const PdfCacheHandle handle, const uint8_t index,
                                                PdfDeferredImageRecord* record) const {
  if (record == nullptr) {
    return PdfStatus::failure(PdfError::InvalidArgument, index);
  }
  const PdfStatus status = readEncodedBytes(handle, index);
  if (!status || !decodeRecord(workspace_, record)) {
    return status ? PdfStatus::failure(PdfError::Malformed, index) : status;
  }
  return PdfStatus::success();
}

PdfStatus PdfImageBuildSpool::readRecord(const uint8_t index, PdfDeferredImageRecord* record) const {
  if (!reading_) {
    return PdfStatus::failure(PdfError::InvalidArgument, index);
  }
  return readEncodedRecord(handle_, index, record);
}

PdfStatus PdfImageBuildSpool::closeRead() {
  if (!reading_) {
    return PdfStatus::success();
  }
  reading_ = false;
  return io_.close(io_.context, &handle_);
}

PdfStatus PdfImageBuildSpool::readRecordDetached(const uint8_t index, PdfDeferredImageRecord* record) {
  if (!validated_ || writing_ || reading_ || index >= recordCount_) {
    return PdfStatus::failure(PdfError::InvalidArgument, index);
  }
  PdfCacheHandle handle{};
  PdfStatus status = io_.open(io_.context, path_, PdfCacheOpenMode::Read, &handle);
  if (status) {
    status = readEncodedRecord(handle, index, record);
  }
  const PdfStatus closeStatus = handle.valid() ? io_.close(io_.context, &handle) : PdfStatus::success();
  return status ? closeStatus : status;
}

void PdfImageBuildSpool::remove() {
  if (path_[0] != '\0' && io_.remove != nullptr) {
    (void)io_.remove(io_.context, path_, false);
  }
  validated_ = false;
  recordCount_ = 0;
}

void PdfImageBuildSpool::abort() {
  if (handle_.valid() && io_.close != nullptr) {
    (void)io_.close(io_.context, &handle_);
  }
  writing_ = false;
  reading_ = false;
  remove();
}

PdfStatus PdfImageFileSpool::beginWrite(const PdfCacheIo& io, const char* path) {
  if (writing_ || reading_ || !io.valid() || path == nullptr || path[0] == '\0' || std::strlen(path) >= sizeof(path_)) {
    return PdfStatus::failure(PdfError::InvalidArgument);
  }
  io_ = io;
  std::strcpy(path_, path);
  recordCount_ = 0;
  recordsCrc32_ = 0;
  PdfStatus status = io_.open(io_.context, path_, PdfCacheOpenMode::WriteTruncate, &handle_);
  if (!status) {
    return status;
  }
  writing_ = true;
  uint8_t header[kHeaderBytes]{};
  std::memcpy(header, kFileHeaderMagic, sizeof(kFileHeaderMagic));
  writeLe16(header + 4, PDF_IMAGE_BUILD_SPOOL_VERSION);
  header[6] = PDF_IMAGE_BUILD_SPOOL_MAX_RECORDS;
  writeLe16(header + 8, static_cast<uint16_t>(PDF_IMAGE_FILE_RECORD_BYTES));
  writeLe32(header + 12, pdfCacheCrc32(header, 12));
  size_t written = 0;
  status = io_.write(io_.context, handle_, header, sizeof(header), &written);
  if (!status || written != sizeof(header)) {
    const PdfStatus result = status ? PdfStatus::failure(PdfError::IoFailure, written) : status;
    abort();
    return result;
  }
  return PdfStatus::success();
}

PdfStatus PdfImageFileSpool::append(const PdfRequiredFileRecord& record) {
  if (!writing_ || recordCount_ >= PDF_IMAGE_BUILD_SPOOL_MAX_RECORDS || !validFileRecord(record)) {
    return PdfStatus::failure(
        recordCount_ >= PDF_IMAGE_BUILD_SPOOL_MAX_RECORDS ? PdfError::LimitExceeded : PdfError::InvalidArgument,
        recordCount_);
  }
  uint8_t encoded[PDF_IMAGE_FILE_RECORD_BYTES]{};
  encodeFileRecord(record, encoded);
  size_t written = 0;
  const PdfStatus status = io_.write(io_.context, handle_, encoded, sizeof(encoded), &written);
  if (!status || written != sizeof(encoded)) {
    return status ? PdfStatus::failure(PdfError::IoFailure, written) : status;
  }
  recordsCrc32_ = pdfCacheCrc32(encoded, sizeof(encoded), recordsCrc32_);
  ++recordCount_;
  return PdfStatus::success();
}

PdfStatus PdfImageFileSpool::closeWrite() {
  if (!writing_ || recordCount_ == 0) {
    return PdfStatus::failure(PdfError::InvalidArgument);
  }
  uint8_t footer[kFooterBytes]{};
  std::memcpy(footer, kFileFooterMagic, sizeof(kFileFooterMagic));
  writeLe16(footer + 4, PDF_IMAGE_BUILD_SPOOL_VERSION);
  footer[6] = recordCount_;
  writeLe64(footer + 8, kHeaderBytes);
  writeLe32(footer + 16, recordsCrc32_);
  writeLe32(footer + 20, pdfCacheCrc32(footer, 20));
  size_t written = 0;
  PdfStatus status = io_.write(io_.context, handle_, footer, sizeof(footer), &written);
  if (status && written != sizeof(footer)) {
    status = PdfStatus::failure(PdfError::IoFailure, written);
  }
  if (status) {
    status = io_.flush(io_.context, handle_);
  }
  if (status) {
    status = io_.sync(io_.context, handle_);
  }
  const PdfStatus closeStatus = io_.close(io_.context, &handle_);
  writing_ = false;
  if (!status || !closeStatus) {
    const PdfStatus result = status ? closeStatus : status;
    remove();
    return result;
  }
  return PdfStatus::success();
}

PdfStatus PdfImageFileSpool::beginRead(const PdfCacheIo& io, const char* path, uint8_t* workspace,
                                       const size_t workspaceBytes, PdfImageSpoolReadRuntime* const runtime) {
  if (writing_ || reading_ || !io.valid() || path == nullptr || path[0] == '\0' || std::strlen(path) >= sizeof(path_) ||
      workspace == nullptr || workspaceBytes < PDF_IMAGE_FILE_RECORD_BYTES || runtime == nullptr ||
      runtime->stage != PdfImageSpoolReadStage::Idle) {
    return PdfStatus::failure(PdfError::InvalidArgument);
  }
  io_ = io;
  std::strcpy(path_, path);
  workspace_ = workspace;
  workspaceBytes_ = workspaceBytes;
  PdfStatus status = io_.open(io_.context, path_, PdfCacheOpenMode::Read, &handle_);
  PdfCacheFileMetadata metadata{};
  if (status) {
    status = io_.metadata(io_.context, handle_, &metadata);
  }
  if (!status || metadata.directory || metadata.symlinkLike ||
      metadata.size < kHeaderBytes + PDF_IMAGE_FILE_RECORD_BYTES + kFooterBytes) {
    const PdfStatus result = status ? PdfStatus::failure(PdfError::Malformed) : status;
    abort();
    return result;
  }
  *runtime = {};
  runtime->fileBytes = metadata.size;
  runtime->stage = PdfImageSpoolReadStage::Header;
  return PdfStatus::success();
}

PdfStepResult PdfImageFileSpool::stepReadOpen(PdfImageSpoolReadRuntime& runtime, PdfWorkBudget& budget) {
  const auto failRead = [&](PdfStatus status) {
    abort();
    runtime = {};
    return PdfStepResult::failure(status);
  };
  if (!handle_.valid() || workspace_ == nullptr || workspaceBytes_ < PDF_IMAGE_FILE_RECORD_BYTES ||
      runtime.stage == PdfImageSpoolReadStage::Idle) {
    return PdfStepResult::failure(PdfStatus::failure(PdfError::InvalidArgument));
  }
  if (runtime.stage == PdfImageSpoolReadStage::Complete) {
    return PdfStepResult::completed();
  }
  if (runtime.stage == PdfImageSpoolReadStage::Header) {
    if (!budget.consumeOperation() || budget.takeBytes(kHeaderBytes) != kHeaderBytes) {
      return PdfStepResult::paused();
    }
    size_t bytesRead = 0;
    PdfStatus status = io_.read(io_.context, handle_, 0, workspace_, kHeaderBytes, &bytesRead);
    if (!status || bytesRead != kHeaderBytes || std::memcmp(workspace_, kFileHeaderMagic, 4) != 0 ||
        readLe16(workspace_ + 4) != PDF_IMAGE_BUILD_SPOOL_VERSION ||
        workspace_[6] != PDF_IMAGE_BUILD_SPOOL_MAX_RECORDS || readLe16(workspace_ + 8) != PDF_IMAGE_FILE_RECORD_BYTES ||
        readLe32(workspace_ + 12) != pdfCacheCrc32(workspace_, 12)) {
      return failRead(status ? PdfStatus::failure(PdfError::Malformed) : status);
    }
    runtime.stage = PdfImageSpoolReadStage::Footer;
    return PdfStepResult::paused();
  }
  if (runtime.stage == PdfImageSpoolReadStage::Footer) {
    if (!budget.consumeOperation() || budget.takeBytes(kFooterBytes) != kFooterBytes) {
      return PdfStepResult::paused();
    }
    size_t bytesRead = 0;
    PdfStatus status =
        io_.read(io_.context, handle_, runtime.fileBytes - kFooterBytes, workspace_, kFooterBytes, &bytesRead);
    if (!status || bytesRead != kFooterBytes || std::memcmp(workspace_, kFileFooterMagic, 4) != 0 ||
        readLe16(workspace_ + 4) != PDF_IMAGE_BUILD_SPOOL_VERSION || readLe64(workspace_ + 8) != kHeaderBytes ||
        readLe32(workspace_ + 20) != pdfCacheCrc32(workspace_, 20)) {
      return failRead(status ? PdfStatus::failure(PdfError::Malformed) : status);
    }
    recordCount_ = workspace_[6];
    const uint64_t expectedSize =
        kHeaderBytes + static_cast<uint64_t>(recordCount_) * PDF_IMAGE_FILE_RECORD_BYTES + kFooterBytes;
    if (recordCount_ == 0 || recordCount_ > PDF_IMAGE_BUILD_SPOOL_MAX_RECORDS || runtime.fileBytes != expectedSize) {
      return failRead(PdfStatus::failure(PdfError::Malformed));
    }
    runtime.expectedRecordsCrc32 = readLe32(workspace_ + 16);
    runtime.recordsCrc32 = 0;
    runtime.nextRecord = 0;
    runtime.stage = PdfImageSpoolReadStage::Records;
    return PdfStepResult::paused();
  }
  if (runtime.nextRecord < recordCount_) {
    if (!budget.consumeOperation() || budget.takeBytes(PDF_IMAGE_FILE_RECORD_BYTES) != PDF_IMAGE_FILE_RECORD_BYTES) {
      return PdfStepResult::paused();
    }
    PdfRequiredFileRecord decoded{};
    PdfStatus status = readEncodedRecord(runtime.nextRecord, &decoded);
    if (!status) {
      return failRead(status);
    }
    runtime.recordsCrc32 = pdfCacheCrc32(workspace_, PDF_IMAGE_FILE_RECORD_BYTES, runtime.recordsCrc32);
    ++runtime.nextRecord;
    return PdfStepResult::paused();
  }
  if (runtime.recordsCrc32 != runtime.expectedRecordsCrc32) {
    return failRead(PdfStatus::failure(PdfError::Malformed));
  }
  reading_ = true;
  runtime.stage = PdfImageSpoolReadStage::Complete;
  return PdfStepResult::completed();
}

PdfStatus PdfImageFileSpool::readEncodedRecord(const uint8_t index, PdfRequiredFileRecord* record) const {
  if (!handle_.valid() || workspace_ == nullptr || workspaceBytes_ < PDF_IMAGE_FILE_RECORD_BYTES || record == nullptr ||
      index >= recordCount_) {
    return PdfStatus::failure(PdfError::InvalidArgument, index);
  }
  const uint64_t offset = kHeaderBytes + static_cast<uint64_t>(index) * PDF_IMAGE_FILE_RECORD_BYTES;
  size_t bytesRead = 0;
  const PdfStatus status = io_.read(io_.context, handle_, offset, workspace_, PDF_IMAGE_FILE_RECORD_BYTES, &bytesRead);
  if (!status || bytesRead != PDF_IMAGE_FILE_RECORD_BYTES || !decodeFileRecord(workspace_, record)) {
    return status ? PdfStatus::failure(PdfError::Malformed, index) : status;
  }
  return PdfStatus::success();
}

PdfStatus PdfImageFileSpool::readRecord(const uint8_t index, PdfRequiredFileRecord* record) const {
  if (!reading_) {
    return PdfStatus::failure(PdfError::InvalidArgument, index);
  }
  return readEncodedRecord(index, record);
}

PdfStatus PdfImageFileSpool::closeRead() {
  if (!reading_) {
    return PdfStatus::success();
  }
  reading_ = false;
  return io_.close(io_.context, &handle_);
}

void PdfImageFileSpool::remove() {
  if (path_[0] != '\0' && io_.remove != nullptr) {
    (void)io_.remove(io_.context, path_, false);
  }
  recordCount_ = 0;
}

void PdfImageFileSpool::abort() {
  if (handle_.valid() && io_.close != nullptr) {
    (void)io_.close(io_.context, &handle_);
  }
  writing_ = false;
  reading_ = false;
  remove();
}
