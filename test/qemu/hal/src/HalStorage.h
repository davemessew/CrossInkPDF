#pragma once

#include <FS.h>
#include <Print.h>
#include <common/FsApiConstants.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#define HAL_STORAGE_HAS_CACHE_METADATA 1

struct HalStorageOptionalUInt64 {
  bool known = false;
  uint64_t value = 0;
};

struct HalStorageCapacityInfo {
  HalStorageOptionalUInt64 total{};
  HalStorageOptionalUInt64 free{};
};

class HalFile;

enum class HalDirectoryNextStatus : uint8_t {
  Entry,
  End,
  Error,
};

class HalStorage {
 public:
  HalStorage();
  bool begin();
  bool ready() const;
  std::vector<String> listFiles(const char* path = "/", int maxFiles = 200);
  String readFile(const char* path);
  bool readFileToStream(const char* path, Print& out, size_t chunkSize = 256);
  size_t readFileToBuffer(const char* path, char* buffer, size_t bufferSize, size_t maxBytes = 0);
  bool writeFile(const char* path, const String& content);
  bool ensureDirectoryExists(const char* path);
  void installDateTimeCallback(const uint8_t* utcOffsetQuarterHoursBiased);

  HalFile open(const char* path, const oflag_t oflag = O_RDONLY);
  bool mkdir(const char* path, const bool pFlag = true);
  bool exists(const char* path);
  bool remove(const char* path);
  bool rename(const char* oldPath, const char* newPath);
  bool rmdir(const char* path);

  bool openFileForRead(const char* moduleName, const char* path, HalFile& file);
  bool openFileForRead(const char* moduleName, const std::string& path, HalFile& file);
  bool openFileForRead(const char* moduleName, const String& path, HalFile& file);
  bool openFileForWrite(const char* moduleName, const char* path, HalFile& file);
  bool openFileForWrite(const char* moduleName, const std::string& path, HalFile& file);
  bool openFileForWrite(const char* moduleName, const String& path, HalFile& file);
  bool removeDir(const char* path);
  HalStorageCapacityInfo capacityInfo();

  static HalStorage& getInstance() { return instance; }

  class StorageLock;

 private:
  static HalStorage instance;
  bool initialized = false;
};

#define Storage HalStorage::getInstance()

class HalFile : public Print {
  friend class HalStorage;
  class Impl;
  explicit HalFile(std::unique_ptr<Impl> impl);
  explicit HalFile(fs::File file, bool writable = false);
  mutable fs::File file;
  bool countedOpen = false;
  bool writable = false;

 public:
  HalFile();
  ~HalFile();
  HalFile(HalFile&&);
  HalFile& operator=(HalFile&&);
  HalFile(const HalFile&) = delete;
  HalFile& operator=(const HalFile&) = delete;

  void flush();
  size_t getName(char* name, size_t len);
  size_t size();
  size_t fileSize();
  uint64_t fileSize64();
  bool seek(size_t pos);
  bool seek64(uint64_t pos);
  bool truncate64(uint64_t length);
  bool seekCur(int64_t offset);
  bool seekSet(size_t offset);
  int available() const;
  size_t position() const;
  int read(void* buf, size_t count);
  int read();
  size_t write(const void* buf, size_t count);
  size_t write(uint8_t b) override;
  bool sync();
  bool rename(const char* newPath);
  bool isDirectory() const;
  void rewindDirectory();
  bool close();
  HalFile openNextFile();
  // Reuses `entry` across a directory scan. Do not close `entry` between
  // calls; this method closes the previous child before opening the next one.
  // The caller must close `entry` once after Entry, End, or Error.
  HalDirectoryNextStatus openNextFile(HalFile& entry);
  bool isOpen() const;
  bool modificationTime(uint64_t* packedFatDateTime);
  operator bool() const;
};

#ifndef HAL_STORAGE_IMPL
using FsFile = HalFile;
#endif

#ifdef SdMan
#undef SdMan
#endif
