#include "PdfHalIo.h"

#include <HalStorage.h>

#include <algorithm>

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

PdfStatus resetByteStore(void* context) {
  if (context == nullptr) {
    return PdfStatus::failure(PdfError::InvalidArgument);
  }
  auto& store = *static_cast<PdfHalByteStoreContext*>(context);
  if (store.file == nullptr || !store.file->isOpen() || !store.file->seek64(0)) {
    return PdfStatus::failure(PdfError::IoFailure);
  }
  store.logicalSize = 0;
  return PdfStatus::success();
}

uint64_t byteStoreSize(void* context) {
  return context == nullptr ? 0 : static_cast<PdfHalByteStoreContext*>(context)->logicalSize;
}

PdfStatus readByteStore(void* context, const uint64_t offset, uint8_t* destination, const size_t requested,
                        size_t* bytesRead) {
  if (context == nullptr || destination == nullptr || bytesRead == nullptr) {
    return PdfStatus::failure(PdfError::InvalidArgument, offset);
  }
  auto& store = *static_cast<PdfHalByteStoreContext*>(context);
  if (store.file == nullptr || !store.file->isOpen() || offset > store.logicalSize) {
    return PdfStatus::failure(PdfError::InvalidOffset, offset);
  }
  const size_t count = static_cast<size_t>(std::min<uint64_t>(requested, store.logicalSize - offset));
  *bytesRead = 0;
  if (count == 0) {
    return PdfStatus::success();
  }
  if (!store.file->seek64(offset)) {
    return PdfStatus::failure(PdfError::IoFailure, offset);
  }
  const int result = store.file->read(destination, count);
  if (result < 0) {
    return PdfStatus::failure(PdfError::IoFailure, offset);
  }
  *bytesRead = static_cast<size_t>(result);
  return PdfStatus::success();
}

PdfStatus writeByteStore(void* context, const uint8_t* source, const size_t requested, size_t* bytesWritten) {
  if (context == nullptr || source == nullptr || bytesWritten == nullptr) {
    return PdfStatus::failure(PdfError::InvalidArgument);
  }
  auto& store = *static_cast<PdfHalByteStoreContext*>(context);
  *bytesWritten = 0;
  if (store.file == nullptr || !store.file->isOpen() || store.logicalSize > store.capacity) {
    return PdfStatus::failure(PdfError::IoFailure, store.logicalSize);
  }
  if (requested > store.capacity - store.logicalSize) {
    return PdfStatus::failure(PdfError::InsufficientStorage, store.logicalSize);
  }
  if (requested == 0) {
    return PdfStatus::success();
  }
  if (!store.file->seek64(store.logicalSize)) {
    return PdfStatus::failure(PdfError::IoFailure, store.logicalSize);
  }
  *bytesWritten = store.file->write(source, requested);
  if (*bytesWritten > requested) {
    *bytesWritten = 0;
    return PdfStatus::failure(PdfError::Malformed, store.logicalSize);
  }
  store.logicalSize += *bytesWritten;
  return PdfStatus::success();
}

}  // namespace

PdfByteSource pdfHalByteSource(HalFile& file) { return {&file, file.fileSize64(), readAt}; }

PdfByteSink pdfHalByteSink(HalFile& file) { return {&file, write}; }

PdfFixedRecordStore pdfHalFixedRecordStore(HalFile& file, const size_t recordSize, const uint32_t capacity) {
  return {&file, capacity, recordSize, readRecord, writeRecord};
}

PdfStatus pdfInitializeHalByteStore(PdfHalByteStoreContext* const context, HalFile& file, const uint64_t capacity) {
  if (context == nullptr || !file.isOpen() || capacity == 0) {
    return PdfStatus::failure(PdfError::InvalidArgument);
  }
  *context = {&file, capacity, 0};
  return resetByteStore(context);
}

PdfByteStore pdfHalByteStore(PdfHalByteStoreContext& context) {
  return {&context, context.capacity, resetByteStore, byteStoreSize, readByteStore, writeByteStore};
}
