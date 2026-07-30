#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "PdfTypes.h"

class PdfTestByteSource {
 public:
  explicit PdfTestByteSource(std::vector<uint8_t> bytes);

  PdfByteSource source(uint64_t advertisedSize = 0);
  void setMaximumRead(size_t maximumRead) { maximumRead_ = maximumRead; }
  void setFailureOffset(uint64_t failureOffset) { failureOffset_ = failureOffset; }

 private:
  static PdfStatus readAt(void* context, uint64_t offset, uint8_t* destination, size_t requested, size_t* bytesRead);

  std::vector<uint8_t> bytes_;
  size_t maximumRead_ = static_cast<size_t>(-1);
  uint64_t failureOffset_ = UINT64_MAX;
};

class PdfTestByteSink {
 public:
  PdfByteSink sink();
  const std::vector<uint8_t>& bytes() const { return bytes_; }
  void setMaximumWrite(size_t maximumWrite) { maximumWrite_ = maximumWrite; }

 private:
  static PdfStatus write(void* context, const uint8_t* source, size_t requested, size_t* bytesWritten);

  std::vector<uint8_t> bytes_;
  size_t maximumWrite_ = static_cast<size_t>(-1);
};

class PdfTestRecordStore {
 public:
  PdfTestRecordStore(size_t recordSize, uint32_t capacity);

  PdfFixedRecordStore store();
  void forbidReadsWhile(const bool* flag) { readForbiddenFlag_ = flag; }

 private:
  static PdfStatus read(void* context, uint32_t ordinal, void* record, size_t recordSize);
  static PdfStatus write(void* context, uint32_t ordinal, const void* record, size_t recordSize);

  std::vector<uint8_t> bytes_;
  size_t recordSize_ = 0;
  uint32_t capacity_ = 0;
  const bool* readForbiddenFlag_ = nullptr;
};

class PdfTestByteStore {
 public:
  explicit PdfTestByteStore(uint64_t capacity) : capacity_(capacity) {}

  PdfByteStore store();
  const std::vector<uint8_t>& bytes() const { return bytes_; }
  uint32_t resetCount() const { return resetCount_; }
  void forbidReadsWhile(const bool* flag) { readForbiddenFlag_ = flag; }

 private:
  static PdfStatus reset(void* context);
  static uint64_t size(void* context);
  static PdfStatus readAt(void* context, uint64_t offset, uint8_t* destination, size_t requested, size_t* bytesRead);
  static PdfStatus write(void* context, const uint8_t* source, size_t requested, size_t* bytesWritten);

  std::vector<uint8_t> bytes_;
  uint64_t capacity_ = 0;
  uint32_t resetCount_ = 0;
  const bool* readForbiddenFlag_ = nullptr;
};
