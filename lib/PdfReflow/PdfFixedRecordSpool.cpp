#include "PdfFixedRecordSpool.h"

#include <limits>

PdfStatus PdfFixedRecordSpool::configure(const PdfCacheIo* const io, const size_t recordSize,
                                         const uint32_t capacity) {
  if (handle_.valid() || io == nullptr || !io->valid() || recordSize == 0 || capacity == 0 ||
      recordSize > std::numeric_limits<uint64_t>::max() / capacity) {
    return PdfStatus::failure(PdfError::InvalidArgument, capacity);
  }
  io_ = io;
  recordSize_ = recordSize;
  capacity_ = capacity;
  recordCount_ = 0;
  readOperations_ = 0;
  writeOperations_ = 0;
  return PdfStatus::success();
}

PdfStatus PdfFixedRecordSpool::open(const char* const path, const PdfCacheOpenMode mode,
                                    const uint32_t existingRecordCount) {
  if (io_ == nullptr || handle_.valid() || path == nullptr || path[0] == '\0' ||
      existingRecordCount > capacity_ || mode == PdfCacheOpenMode::ReadWrite ||
      (mode == PdfCacheOpenMode::WriteTruncate && existingRecordCount != 0)) {
    return PdfStatus::failure(PdfError::InvalidArgument, existingRecordCount);
  }
  PdfStatus status = io_->open(io_->context, path, mode, &handle_);
  if (!status) {
    return status;
  }
  mode_ = mode;
  recordCount_ = mode == PdfCacheOpenMode::WriteTruncate ? 0 : existingRecordCount;
  return PdfStatus::success();
}

PdfStatus PdfFixedRecordSpool::openForAppend(const char* const path, const uint32_t existingRecordCount) {
  if (io_ == nullptr || handle_.valid() || path == nullptr || path[0] == '\0' ||
      existingRecordCount > capacity_) {
    return PdfStatus::failure(PdfError::InvalidArgument, existingRecordCount);
  }
  PdfStatus status = io_->open(io_->context, path, PdfCacheOpenMode::Write, &handle_);
  if (!status) {
    return status;
  }
  PdfCacheFileMetadata metadata{};
  status = io_->metadata(io_->context, handle_, &metadata);
  const uint64_t expectedBytes = static_cast<uint64_t>(existingRecordCount) * recordSize_;
  if (!status || metadata.directory || metadata.symlinkLike || metadata.size != expectedBytes) {
    abortClose();
    return status ? PdfStatus::failure(PdfError::Malformed, metadata.size) : status;
  }
  status = pdfCacheSeek(*io_, handle_, expectedBytes);
  if (!status) {
    abortClose();
    return status;
  }
  mode_ = PdfCacheOpenMode::Write;
  recordCount_ = existingRecordCount;
  return PdfStatus::success();
}

PdfStatus PdfFixedRecordSpool::openForUpdates(const char* const path, const uint32_t existingRecordCount) {
  if (io_ == nullptr || handle_.valid() || path == nullptr || path[0] == '\0' || existingRecordCount == 0 ||
      existingRecordCount > capacity_) {
    return PdfStatus::failure(PdfError::InvalidArgument, existingRecordCount);
  }
  PdfStatus status = io_->open(io_->context, path, PdfCacheOpenMode::ReadWrite, &handle_);
  if (!status) {
    return status;
  }
  PdfCacheFileMetadata metadata{};
  status = io_->metadata(io_->context, handle_, &metadata);
  const uint64_t expectedBytes = static_cast<uint64_t>(existingRecordCount) * recordSize_;
  if (!status || metadata.directory || metadata.symlinkLike || metadata.size != expectedBytes) {
    abortClose();
    return status ? PdfStatus::failure(PdfError::Malformed, metadata.size) : status;
  }
  mode_ = PdfCacheOpenMode::ReadWrite;
  recordCount_ = existingRecordCount;
  return PdfStatus::success();
}

PdfStatus PdfFixedRecordSpool::appendRecords(const void* const records, const uint32_t count) {
  if (io_ == nullptr || !handle_.valid() ||
      (mode_ != PdfCacheOpenMode::WriteTruncate && mode_ != PdfCacheOpenMode::Write) || records == nullptr ||
      count == 0 || count > capacity_ - recordCount_) {
    return PdfStatus::failure(PdfError::InvalidOffset, recordCount_);
  }
  const size_t bytes = static_cast<size_t>(count) * recordSize_;
  size_t bytesWritten = 0;
  const PdfStatus status =
      io_->write(io_->context, handle_, static_cast<const uint8_t*>(records), bytes, &bytesWritten);
  if (!status) {
    abortClose();
    return status;
  }
  if (bytesWritten != bytes) {
    const uint64_t offset = static_cast<uint64_t>(recordCount_) * recordSize_ + bytesWritten;
    abortClose();
    return PdfStatus::failure(PdfError::InsufficientStorage, offset);
  }
  recordCount_ += count;
  writeOperations_ += count;
  return PdfStatus::success();
}

PdfStatus PdfFixedRecordSpool::readRecords(const uint32_t ordinal, void* const records, const uint32_t count) {
  if (io_ == nullptr || !handle_.valid() || mode_ == PdfCacheOpenMode::WriteTruncate || records == nullptr ||
      count == 0 || ordinal > recordCount_ || count > recordCount_ - ordinal ||
      recordSize_ > std::numeric_limits<size_t>::max() / count) {
    return PdfStatus::failure(PdfError::InvalidOffset, ordinal);
  }
  const size_t bytes = static_cast<size_t>(count) * recordSize_;
  const uint64_t offset = static_cast<uint64_t>(ordinal) * recordSize_;
  size_t bytesRead = 0;
  const PdfStatus status =
      io_->read(io_->context, handle_, offset, static_cast<uint8_t*>(records), bytes, &bytesRead);
  if (!status) {
    return status;
  }
  readOperations_ += count;
  return bytesRead == bytes ? PdfStatus::success()
                            : PdfStatus::failure(PdfError::UnexpectedEof, offset + bytesRead);
}

PdfStatus PdfFixedRecordSpool::rewriteExisting(const uint32_t ordinal, const void* const record,
                                                const size_t recordSize) {
  if (io_ == nullptr || !handle_.valid() || mode_ != PdfCacheOpenMode::ReadWrite || record == nullptr ||
      recordSize != recordSize_ || ordinal >= recordCount_) {
    return PdfStatus::failure(PdfError::InvalidOffset, ordinal);
  }
  const uint64_t offset = static_cast<uint64_t>(ordinal) * recordSize_;
  PdfStatus status = pdfCacheSeek(*io_, handle_, offset);
  if (!status) {
    abortClose();
    return status;
  }
  size_t bytesWritten = 0;
  status = io_->write(io_->context, handle_, static_cast<const uint8_t*>(record), recordSize_, &bytesWritten);
  if (!status || bytesWritten != recordSize_) {
    abortClose();
    return status ? PdfStatus::failure(PdfError::InsufficientStorage, offset + bytesWritten) : status;
  }
  ++writeOperations_;
  return PdfStatus::success();
}

PdfStatus PdfFixedRecordSpool::flush() {
  return io_ != nullptr && handle_.valid() ? io_->flush(io_->context, handle_)
                                           : PdfStatus::failure(PdfError::InvalidArgument);
}

PdfStatus PdfFixedRecordSpool::sync() {
  return io_ != nullptr && handle_.valid() ? io_->sync(io_->context, handle_)
                                           : PdfStatus::failure(PdfError::InvalidArgument);
}

PdfStatus PdfFixedRecordSpool::close() {
  return io_ != nullptr && handle_.valid() ? io_->close(io_->context, &handle_)
                                           : PdfStatus::failure(PdfError::InvalidArgument);
}

void PdfFixedRecordSpool::abortClose() {
  if (io_ != nullptr && handle_.valid()) {
    (void)io_->close(io_->context, &handle_);
  }
  handle_ = {};
}

PdfFixedRecordStore PdfFixedRecordSpool::store() {
  return {this, capacity_, recordSize_, readRecord, writeRecord, writeRecords};
}

PdfStatus PdfFixedRecordSpool::readRecord(void* context, const uint32_t ordinal, void* record,
                                          const size_t recordSize) {
  if (context == nullptr || record == nullptr) {
    return PdfStatus::failure(PdfError::InvalidArgument, ordinal);
  }
  auto& spool = *static_cast<PdfFixedRecordSpool*>(context);
  if (spool.io_ == nullptr || !spool.handle_.valid() || spool.mode_ == PdfCacheOpenMode::WriteTruncate ||
      recordSize != spool.recordSize_ || ordinal >= spool.recordCount_) {
    return PdfStatus::failure(PdfError::InvalidOffset, ordinal);
  }
  const uint64_t offset = static_cast<uint64_t>(ordinal) * recordSize;
  size_t bytesRead = 0;
  const PdfStatus status =
      spool.io_->read(spool.io_->context, spool.handle_, offset, static_cast<uint8_t*>(record), recordSize, &bytesRead);
  if (!status) {
    return status;
  }
  ++spool.readOperations_;
  return bytesRead == recordSize ? PdfStatus::success()
                                 : PdfStatus::failure(PdfError::UnexpectedEof, offset + bytesRead);
}

PdfStatus PdfFixedRecordSpool::writeRecord(void* context, const uint32_t ordinal, const void* record,
                                           const size_t recordSize) {
  if (context == nullptr || record == nullptr) {
    return PdfStatus::failure(PdfError::InvalidArgument, ordinal);
  }
  auto& spool = *static_cast<PdfFixedRecordSpool*>(context);
  if (spool.io_ == nullptr || !spool.handle_.valid() ||
      (spool.mode_ != PdfCacheOpenMode::WriteTruncate && spool.mode_ != PdfCacheOpenMode::Write) ||
      recordSize != spool.recordSize_ || ordinal != spool.recordCount_ || ordinal >= spool.capacity_) {
    return PdfStatus::failure(PdfError::InvalidOffset, ordinal);
  }
  size_t bytesWritten = 0;
  const PdfStatus status = spool.io_->write(spool.io_->context, spool.handle_, static_cast<const uint8_t*>(record),
                                            recordSize, &bytesWritten);
  if (!status) {
    spool.abortClose();
    return status;
  }
  if (bytesWritten != recordSize) {
    spool.abortClose();
    return PdfStatus::failure(PdfError::InsufficientStorage,
                              static_cast<uint64_t>(ordinal) * recordSize + bytesWritten);
  }
  ++spool.recordCount_;
  ++spool.writeOperations_;
  return PdfStatus::success();
}

PdfStatus PdfFixedRecordSpool::writeRecords(void* context, const uint32_t ordinal, const void* records,
                                            const uint32_t count, const size_t recordSize) {
  if (context == nullptr || records == nullptr || count == 0) {
    return PdfStatus::failure(PdfError::InvalidArgument, ordinal);
  }
  auto& spool = *static_cast<PdfFixedRecordSpool*>(context);
  if (recordSize != spool.recordSize_ || ordinal != spool.recordCount_) {
    return PdfStatus::failure(PdfError::InvalidOffset, ordinal);
  }
  return spool.appendRecords(records, count);
}

PdfStatus PdfMutableRecordSpool::configure(const PdfCacheIo* const io, const size_t recordSize,
                                           const uint32_t capacity) {
  if (handle_.valid() || io == nullptr || !io->valid() || recordSize == 0 || capacity == 0 ||
      recordSize > std::numeric_limits<uint64_t>::max() / capacity) {
    return PdfStatus::failure(PdfError::InvalidArgument, capacity);
  }
  io_ = io;
  recordSize_ = recordSize;
  capacity_ = capacity;
  recordCount_ = 0;
  return PdfStatus::success();
}

PdfStatus PdfMutableRecordSpool::create(const char* const path) {
  if (io_ == nullptr || handle_.valid() || path == nullptr || path[0] == '\0') {
    return PdfStatus::failure(PdfError::InvalidArgument);
  }
  PdfStatus status = io_->open(io_->context, path, PdfCacheOpenMode::WriteTruncate, &handle_);
  if (status) {
    status = io_->close(io_->context, &handle_);
  }
  if (!status) {
    abortClose();
  }
  recordCount_ = 0;
  return status;
}

PdfStatus PdfMutableRecordSpool::openSession(const char* const path) {
  if (io_ == nullptr || handle_.valid() || path == nullptr || path[0] == '\0') {
    return PdfStatus::failure(PdfError::InvalidArgument);
  }
  return io_->open(io_->context, path, PdfCacheOpenMode::ReadWrite, &handle_);
}

PdfStatus PdfMutableRecordSpool::appendRecords(const void* const records, const uint32_t count) {
  if (io_ == nullptr || !handle_.valid() || records == nullptr || count == 0 || recordCount_ > capacity_ ||
      count > capacity_ - recordCount_) {
    return PdfStatus::failure(PdfError::InvalidOffset, recordCount_);
  }
  if (recordSize_ > std::numeric_limits<size_t>::max() / count) {
    return PdfStatus::failure(PdfError::LimitExceeded, recordCount_);
  }
  const uint64_t offset = static_cast<uint64_t>(recordCount_) * recordSize_;
  PdfStatus status = pdfCacheSeek(*io_, handle_, offset);
  if (!status) {
    abortClose();
    return status;
  }
  const size_t bytes = static_cast<size_t>(count) * recordSize_;
  size_t bytesWritten = 0;
  status = io_->write(io_->context, handle_, static_cast<const uint8_t*>(records), bytes, &bytesWritten);
  if (!status) {
    abortClose();
    return status;
  }
  if (bytesWritten != bytes) {
    abortClose();
    return PdfStatus::failure(PdfError::InsufficientStorage, offset + bytesWritten);
  }
  recordCount_ += count;
  return PdfStatus::success();
}

PdfStatus PdfMutableRecordSpool::closeSession() {
  return io_ != nullptr && handle_.valid() ? io_->close(io_->context, &handle_)
                                           : PdfStatus::failure(PdfError::InvalidArgument);
}

void PdfMutableRecordSpool::abortClose() {
  if (io_ != nullptr && handle_.valid()) {
    (void)io_->close(io_->context, &handle_);
  }
  handle_ = {};
}

PdfFixedRecordStore PdfMutableRecordSpool::store() {
  return {this, capacity_, recordSize_, readRecord, writeRecord};
}

PdfStatus PdfMutableRecordSpool::readRecord(void* context, const uint32_t ordinal, void* record,
                                            const size_t recordSize) {
  if (context == nullptr || record == nullptr) {
    return PdfStatus::failure(PdfError::InvalidArgument, ordinal);
  }
  auto& spool = *static_cast<PdfMutableRecordSpool*>(context);
  if (spool.io_ == nullptr || !spool.handle_.valid() || recordSize != spool.recordSize_ ||
      ordinal >= spool.recordCount_) {
    return PdfStatus::failure(PdfError::InvalidOffset, ordinal);
  }
  const uint64_t offset = static_cast<uint64_t>(ordinal) * recordSize;
  size_t bytesRead = 0;
  const PdfStatus status =
      spool.io_->read(spool.io_->context, spool.handle_, offset, static_cast<uint8_t*>(record), recordSize, &bytesRead);
  if (!status) {
    return status;
  }
  return bytesRead == recordSize ? PdfStatus::success()
                                 : PdfStatus::failure(PdfError::UnexpectedEof, offset + bytesRead);
}

PdfStatus PdfMutableRecordSpool::writeRecord(void* context, const uint32_t ordinal, const void* record,
                                             const size_t recordSize) {
  if (context == nullptr || record == nullptr) {
    return PdfStatus::failure(PdfError::InvalidArgument, ordinal);
  }
  auto& spool = *static_cast<PdfMutableRecordSpool*>(context);
  if (spool.io_ == nullptr || !spool.handle_.valid() || recordSize != spool.recordSize_ ||
      ordinal > spool.recordCount_ || ordinal >= spool.capacity_) {
    return PdfStatus::failure(PdfError::InvalidOffset, ordinal);
  }
  const uint64_t offset = static_cast<uint64_t>(ordinal) * recordSize;
  PdfStatus status = pdfCacheSeek(*spool.io_, spool.handle_, offset);
  if (!status) {
    spool.abortClose();
    return status;
  }
  size_t bytesWritten = 0;
  status = spool.io_->write(spool.io_->context, spool.handle_, static_cast<const uint8_t*>(record), recordSize,
                            &bytesWritten);
  if (!status || bytesWritten != recordSize) {
    spool.abortClose();
    return status ? PdfStatus::failure(PdfError::InsufficientStorage, offset + bytesWritten) : status;
  }
  if (ordinal == spool.recordCount_) {
    ++spool.recordCount_;
  }
  return PdfStatus::success();
}
