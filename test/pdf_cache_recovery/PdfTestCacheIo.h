#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "PdfCacheIo.h"

enum class PdfTestFaultPoint : uint8_t {
  None,
  Open,
  Read,
  Write,
  Flush,
  Sync,
  Close,
  Remove,
  Mkdir,
  List,
  Capacity,
  Metadata,
};

class PdfTestCacheIo {
 public:
  PdfTestCacheIo();

  PdfCacheIo io();

  void addDirectory(const std::string& path, bool symlinkLike = false);
  void addFile(const std::string& path, const std::vector<uint8_t>& bytes, uint64_t modificationTime = 0,
               bool modificationTimeKnown = false);
  void addFile(const std::string& path, const std::string& bytes);
  void truncateFile(const std::string& path, size_t length);
  void corruptByte(const std::string& path, size_t offset, uint8_t mask);
  bool exists(const std::string& path) const;
  bool isDirectory(const std::string& path) const;
  const std::vector<uint8_t>& bytes(const std::string& path) const;
  std::vector<std::string> paths() const;

  void setCapacity(uint64_t total, uint64_t free, bool known = true);
  void fail(PdfTestFaultPoint point, uint32_t occurrence = 1);
  void clearFault();
  void setWriteAllowance(uint64_t bytes);
  void clearWriteAllowance();

  uint32_t openCalls() const;
  uint32_t readCalls() const;
  uint32_t writeCalls() const;
  uint32_t closeCalls() const;
  uint32_t openHandleCount() const;
  uint64_t bytesReadTotal() const;
  uint64_t bytesWrittenTotal() const;
  size_t maximumReadRequest() const;
  size_t maximumWriteRequest() const;
  uint32_t openCallsForPath(const std::string& path) const;
  const std::vector<std::string>& events() const;
  void clearEvents();

 private:
  struct Node {
    std::vector<uint8_t> bytes;
    uint64_t modificationTime = 0;
    bool modificationTimeKnown = false;
    bool directory = false;
    bool symlinkLike = false;
  };

  struct OpenHandle {
    std::string path;
    size_t position = 0;
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
  static PdfStatus mkdirThunk(void* context, const char* path);
  static PdfStatus listThunk(void* context, const char* path, PdfCacheListVisitor visitor, void* visitorContext);
  static PdfStatus capacityThunk(void* context, PdfCacheCapacity* capacity);
  static PdfStatus metadataThunk(void* context, PdfCacheHandle handle, PdfCacheFileMetadata* metadata);

  bool shouldFail(PdfTestFaultPoint point);
  PdfStatus open(const char* path, PdfCacheOpenMode mode, PdfCacheHandle* handle);
  PdfStatus read(PdfCacheHandle handle, uint64_t offset, uint8_t* destination, size_t requested, size_t* bytesRead);
  PdfStatus write(PdfCacheHandle handle, const uint8_t* source, size_t requested, size_t* bytesWritten);
  PdfStatus flush(PdfCacheHandle handle);
  PdfStatus sync(PdfCacheHandle handle);
  PdfStatus close(PdfCacheHandle* handle);
  PdfStatus remove(const char* path, bool recursive);
  PdfStatus mkdir(const char* path);
  PdfStatus list(const char* path, PdfCacheListVisitor visitor, void* visitorContext);
  PdfStatus capacity(PdfCacheCapacity* capacity);
  PdfStatus metadata(PdfCacheHandle handle, PdfCacheFileMetadata* metadata);

  std::map<std::string, Node> nodes_;
  OpenHandle handles_[8]{};
  PdfTestFaultPoint faultPoint_ = PdfTestFaultPoint::None;
  uint32_t faultOccurrence_ = 0;
  uint32_t faultSeen_ = 0;
  uint64_t writeAllowance_ = UINT64_MAX;
  PdfCacheCapacity capacity_{};
  uint32_t openCalls_ = 0;
  uint32_t readCalls_ = 0;
  uint32_t writeCalls_ = 0;
  uint32_t closeCalls_ = 0;
  uint64_t bytesReadTotal_ = 0;
  uint64_t bytesWrittenTotal_ = 0;
  size_t maximumReadRequest_ = 0;
  size_t maximumWriteRequest_ = 0;
  std::map<std::string, uint32_t> pathOpenCalls_;
  std::vector<std::string> events_;
};
