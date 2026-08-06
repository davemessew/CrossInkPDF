#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "PdfCacheIo.h"

class ContractCacheIo {
 public:
  struct Metrics {
    uint64_t operations = 0;
    uint64_t bytesRead = 0;
    uint64_t bytesWritten = 0;
    uint32_t opens = 0;
    uint32_t reads = 0;
    uint32_t writes = 0;
    uint32_t flushes = 0;
    uint32_t syncs = 0;
    uint32_t closes = 0;
  };

  ContractCacheIo();

  PdfCacheIo io();
  PdfCacheRenameFn renameCallback() const { return renameThunk; }

  void addFile(const std::string& path, const std::vector<uint8_t>& bytes);
  bool exists(const std::string& path) const;
  bool isDirectory(const std::string& path) const;
  const std::vector<uint8_t>& bytes(const std::string& path) const;
  uint32_t openHandleCount() const;
  uint32_t maximumReadHandleCount() const { return maximumReadHandleCount_; }
  const std::string& lastReaderOverlap() const { return lastReaderOverlap_; }
  const Metrics& metrics() const { return metrics_; }
  uint64_t ioBytes() const { return metrics_.bytesRead + metrics_.bytesWritten; }
  uint32_t syncCount(const std::string& path) const;
  void resetMetrics();
  void clearOperationTrace() { operationTrace_.clear(); }
  const std::vector<std::string>& operationTrace() const { return operationTrace_; }
  void advanceClockOnOperation(uint32_t* clock, uint32_t milliseconds = 1) {
    operationClock_ = clock;
    millisecondsPerOperation_ = milliseconds;
  }
  void failNextWriteAfter(size_t bytes) { failingWriteBytes_ = bytes; }
  void shortNextWriteAfter(size_t bytes) { shortWriteBytes_ = bytes; }
  void shortNextReadAfter(size_t bytes) { shortReadBytes_ = bytes; }

 private:
  struct Node {
    std::vector<uint8_t> bytes;
    uint64_t modificationTime = 1234;
    bool directory = false;
  };

  struct OpenHandle {
    std::string path;
    size_t position = 0;
    bool readable = false;
    bool writable = false;
    bool open = false;
  };

  static PdfStatus openThunk(void* context, const char* path, PdfCacheOpenMode mode, PdfCacheHandle* handle);
  static PdfStatus readThunk(void* context, PdfCacheHandle handle, uint64_t offset, uint8_t* destination,
                             size_t requested, size_t* bytesRead);
  static PdfStatus writeThunk(void* context, PdfCacheHandle handle, const uint8_t* source, size_t requested,
                              size_t* bytesWritten);
  static PdfStatus flushThunk(void* context, PdfCacheHandle handle);
  static PdfStatus syncThunk(void* context, PdfCacheHandle handle);
  static PdfStatus closeThunk(void* context, PdfCacheHandle* handle);
  static PdfStatus removeThunk(void* context, const char* path, bool recursive);
  static PdfStatus renameThunk(void* context, const char* sourcePath, const char* destinationPath);
  static PdfStatus mkdirThunk(void* context, const char* path);
  static PdfStatus listThunk(void* context, const char* path, PdfCacheListVisitor visitor, void* visitorContext);
  static PdfStatus capacityThunk(void* context, PdfCacheCapacity* capacity);
  static PdfStatus metadataThunk(void* context, PdfCacheHandle handle, PdfCacheFileMetadata* metadata);

  PdfStatus open(const char* path, PdfCacheOpenMode mode, PdfCacheHandle* handle);
  PdfStatus read(PdfCacheHandle handle, uint64_t offset, uint8_t* destination, size_t requested,
                 size_t* bytesRead);
  PdfStatus write(PdfCacheHandle handle, const uint8_t* source, size_t requested, size_t* bytesWritten);
  PdfStatus flush(PdfCacheHandle handle);
  PdfStatus sync(PdfCacheHandle handle);
  PdfStatus close(PdfCacheHandle* handle);
  PdfStatus remove(const char* path, bool recursive);
  PdfStatus rename(const char* sourcePath, const char* destinationPath);
  PdfStatus mkdir(const char* path);
  PdfStatus list(const char* path, PdfCacheListVisitor visitor, void* visitorContext);
  PdfStatus capacity(PdfCacheCapacity* capacity);
  PdfStatus metadata(PdfCacheHandle handle, PdfCacheFileMetadata* metadata);
  void recordOperation();
  void traceOperation(const char* operation, const std::string& path = {});

  std::map<std::string, Node> nodes_;
  std::array<OpenHandle, 16> handles_{};
  PdfCacheCapacity capacity_{};
  size_t failingWriteBytes_ = static_cast<size_t>(-1);
  size_t shortWriteBytes_ = static_cast<size_t>(-1);
  size_t shortReadBytes_ = static_cast<size_t>(-1);
  uint32_t maximumReadHandleCount_ = 0;
  std::string lastReaderOverlap_;
  Metrics metrics_{};
  std::map<std::string, uint32_t> syncCounts_;
  uint32_t* operationClock_ = nullptr;
  uint32_t millisecondsPerOperation_ = 1;
  std::vector<std::string> operationTrace_;
};
