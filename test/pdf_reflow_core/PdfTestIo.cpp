#include "PdfTestIo.h"

#include <algorithm>
#include <cstring>
#include <utility>

PdfTestByteSource::PdfTestByteSource(std::vector<uint8_t> bytes) : bytes_(std::move(bytes)) {}

PdfByteSource PdfTestByteSource::source(const uint64_t advertisedSize) {
  return {this, advertisedSize == 0 ? bytes_.size() : advertisedSize, readAt};
}

PdfStatus PdfTestByteSource::readAt(void* context, const uint64_t offset, uint8_t* destination, const size_t requested,
                                    size_t* bytesRead) {
  auto& source = *static_cast<PdfTestByteSource*>(context);
  if (destination == nullptr || bytesRead == nullptr) {
    return PdfStatus::failure(PdfError::InvalidArgument, offset);
  }
  if (offset >= source.failureOffset_) {
    return PdfStatus::failure(PdfError::IoFailure, offset);
  }
  if (offset >= source.bytes_.size()) {
    *bytesRead = 0;
    return PdfStatus::success();
  }
  const size_t available = source.bytes_.size() - static_cast<size_t>(offset);
  *bytesRead = std::min({requested, available, source.maximumRead_});
  std::memcpy(destination, source.bytes_.data() + offset, *bytesRead);
  return PdfStatus::success();
}

PdfByteSink PdfTestByteSink::sink() { return {this, write}; }

PdfStatus PdfTestByteSink::write(void* context, const uint8_t* source, const size_t requested, size_t* bytesWritten) {
  auto& sink = *static_cast<PdfTestByteSink*>(context);
  if (source == nullptr || bytesWritten == nullptr) {
    return PdfStatus::failure(PdfError::InvalidArgument);
  }
  *bytesWritten = std::min(requested, sink.maximumWrite_);
  sink.bytes_.insert(sink.bytes_.end(), source, source + *bytesWritten);
  return PdfStatus::success();
}

PdfTestRecordStore::PdfTestRecordStore(const size_t recordSize, const uint32_t capacity)
    : bytes_(recordSize * capacity), recordSize_(recordSize), capacity_(capacity) {}

PdfFixedRecordStore PdfTestRecordStore::store() { return {this, capacity_, recordSize_, read, write}; }

PdfStatus PdfTestRecordStore::read(void* context, const uint32_t ordinal, void* record, const size_t recordSize) {
  auto& store = *static_cast<PdfTestRecordStore*>(context);
  if (store.readForbiddenFlag_ != nullptr && *store.readForbiddenFlag_) {
    return PdfStatus::failure(PdfError::IoFailure, ordinal);
  }
  if (record == nullptr || recordSize != store.recordSize_ || ordinal >= store.capacity_) {
    return PdfStatus::failure(PdfError::InvalidOffset, ordinal);
  }
  std::memcpy(record, store.bytes_.data() + ordinal * recordSize, recordSize);
  return PdfStatus::success();
}

PdfStatus PdfTestRecordStore::write(void* context, const uint32_t ordinal, const void* record,
                                    const size_t recordSize) {
  auto& store = *static_cast<PdfTestRecordStore*>(context);
  if (record == nullptr || recordSize != store.recordSize_ || ordinal >= store.capacity_) {
    return PdfStatus::failure(PdfError::InvalidOffset, ordinal);
  }
  std::memcpy(store.bytes_.data() + ordinal * recordSize, record, recordSize);
  return PdfStatus::success();
}

PdfByteStore PdfTestByteStore::store() { return {this, capacity_, reset, size, readAt, write}; }

PdfStatus PdfTestByteStore::reset(void* context) {
  if (context == nullptr) {
    return PdfStatus::failure(PdfError::InvalidArgument);
  }
  auto& store = *static_cast<PdfTestByteStore*>(context);
  store.bytes_.clear();
  ++store.resetCount_;
  return PdfStatus::success();
}

uint64_t PdfTestByteStore::size(void* context) {
  return context == nullptr ? 0 : static_cast<PdfTestByteStore*>(context)->bytes_.size();
}

PdfStatus PdfTestByteStore::readAt(void* context, const uint64_t offset, uint8_t* destination, const size_t requested,
                                   size_t* bytesRead) {
  if (context == nullptr || destination == nullptr || bytesRead == nullptr) {
    return PdfStatus::failure(PdfError::InvalidArgument, offset);
  }
  auto& store = *static_cast<PdfTestByteStore*>(context);
  if (store.readForbiddenFlag_ != nullptr && *store.readForbiddenFlag_) {
    return PdfStatus::failure(PdfError::IoFailure, offset);
  }
  if (offset > store.bytes_.size()) {
    return PdfStatus::failure(PdfError::InvalidOffset, offset);
  }
  *bytesRead = std::min(requested, store.bytes_.size() - static_cast<size_t>(offset));
  if (*bytesRead != 0) {
    std::memcpy(destination, store.bytes_.data() + offset, *bytesRead);
  }
  return PdfStatus::success();
}

PdfStatus PdfTestByteStore::write(void* context, const uint8_t* source, const size_t requested, size_t* bytesWritten) {
  if (context == nullptr || source == nullptr || bytesWritten == nullptr) {
    return PdfStatus::failure(PdfError::InvalidArgument);
  }
  auto& store = *static_cast<PdfTestByteStore*>(context);
  const uint64_t remaining = store.capacity_ - static_cast<uint64_t>(store.bytes_.size());
  *bytesWritten = static_cast<size_t>(std::min<uint64_t>(requested, remaining));
  store.bytes_.insert(store.bytes_.end(), source, source + *bytesWritten);
  if (*bytesWritten == 0 && requested != 0) {
    return PdfStatus::failure(PdfError::InsufficientStorage, store.bytes_.size());
  }
  return PdfStatus::success();
}
