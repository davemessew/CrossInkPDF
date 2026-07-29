#include "PdfHalIo.h"

#include <HalStorage.h>

#include "PdfCheckedMath.h"

namespace {

PdfStatus readAt(void* context, const uint64_t offset, uint8_t* destination, const size_t requested,
                 size_t* bytesRead) {
  if (context == nullptr || destination == nullptr || bytesRead == nullptr) {
    return PdfStatus::failure(PdfError::InvalidArgument, offset);
  }
  auto& file = *static_cast<HalFile*>(context);
  *bytesRead = 0;
  if (!file.seek64(offset)) {
    return PdfStatus::failure(PdfError::IoFailure, offset);
  }
  const int result = file.read(destination, requested);
  if (result < 0) {
    return PdfStatus::failure(PdfError::IoFailure, offset);
  }
  *bytesRead = static_cast<size_t>(result);
  return PdfStatus::success();
}

PdfStatus write(void* context, const uint8_t* source, const size_t requested, size_t* bytesWritten) {
  if (context == nullptr || source == nullptr || bytesWritten == nullptr) {
    return PdfStatus::failure(PdfError::InvalidArgument);
  }
  auto& file = *static_cast<HalFile*>(context);
  *bytesWritten = file.write(source, requested);
  return PdfStatus::success();
}

PdfStatus readRecord(void* context, const uint32_t ordinal, void* record, const size_t recordSize) {
  uint64_t offset = 0;
  if (context == nullptr || record == nullptr || !pdfCheckedMultiply(ordinal, recordSize, &offset)) {
    return PdfStatus::failure(PdfError::InvalidOffset);
  }
  auto& file = *static_cast<HalFile*>(context);
  if (!file.seek64(offset)) {
    return PdfStatus::failure(PdfError::IoFailure, offset);
  }
  const int result = file.read(record, recordSize);
  if (result < 0) {
    return PdfStatus::failure(PdfError::IoFailure, offset);
  }
  if (static_cast<size_t>(result) != recordSize) {
    return PdfStatus::failure(PdfError::UnexpectedEof, offset + static_cast<size_t>(result));
  }
  return PdfStatus::success();
}

PdfStatus writeRecord(void* context, const uint32_t ordinal, const void* record, const size_t recordSize) {
  uint64_t offset = 0;
  if (context == nullptr || record == nullptr || !pdfCheckedMultiply(ordinal, recordSize, &offset)) {
    return PdfStatus::failure(PdfError::InvalidOffset);
  }
  auto& file = *static_cast<HalFile*>(context);
  if (!file.seek64(offset)) {
    return PdfStatus::failure(PdfError::IoFailure, offset);
  }
  if (file.write(record, recordSize) != recordSize) {
    return PdfStatus::failure(PdfError::IoFailure, offset);
  }
  return PdfStatus::success();
}

}  // namespace

PdfByteSource pdfHalByteSource(HalFile& file) {
  return {&file, file.fileSize64(), readAt};
}

PdfByteSink pdfHalByteSink(HalFile& file) {
  return {&file, write};
}

PdfFixedRecordStore pdfHalFixedRecordStore(HalFile& file, const size_t recordSize, const uint32_t capacity) {
  return {&file, capacity, recordSize, readRecord, writeRecord};
}
