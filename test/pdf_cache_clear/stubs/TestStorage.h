#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <string>
#include <vector>

class FsFile;

#ifndef SIMULATOR
enum class HalDirectoryNextStatus : uint8_t {
  Entry,
  End,
  Error,
};
#endif

class TestStorage {
 public:
  static TestStorage& instance();

  void reset(const std::filesystem::path& root);
  bool exists(const char* path) const;
  bool mkdir(const char* path, bool parents = true);
  bool remove(const char* path);
  bool rename(const char* oldPath, const char* newPath);
  bool removeDir(const char* path);
  FsFile open(const char* path);

  std::filesystem::path physicalPath(const char* path) const;
  void failRemovalOf(const std::string& path);
  void failOpenOf(const std::string& path);
  void failNextChildWrapperAllocation();
  void failDirectoryReadAfter(size_t successfulEntries);
  void clearFailures();
  size_t openHandleCount() const { return openHandles_; }
  size_t maxOpenHandleCount() const { return maxOpenHandles_; }
  size_t childWrapperCount() const { return activeChildWrappers_; }
  size_t childWrapperAllocationCount() const { return childWrapperAllocations_; }
  size_t maxChildWrapperCount() const { return maxChildWrappers_; }
  size_t openCallCount() const { return openCalls_; }
  size_t removeCallCount() const { return removeCalls_; }
  size_t removeDirCallCount() const { return removeDirCalls_; }

 private:
  friend class FsFile;

  void opened();
  void closed();
  bool allocateChildWrapper();
  void releaseChildWrapper();
  bool shouldFailRemoval(const char* path) const;
  bool shouldFailDirectoryRead(size_t successfulEntries) const;

  std::filesystem::path root_;
  std::string failedRemovalPath_;
  std::string failedOpenPath_;
  bool failNextChildWrapperAllocation_ = false;
  size_t failedDirectoryReadAfter_ = std::numeric_limits<size_t>::max();
  size_t openHandles_ = 0;
  size_t maxOpenHandles_ = 0;
  size_t activeChildWrappers_ = 0;
  size_t maxChildWrappers_ = 0;
  size_t childWrapperAllocations_ = 0;
  size_t openCalls_ = 0;
  size_t removeCalls_ = 0;
  size_t removeDirCalls_ = 0;
};

class FsFile {
 public:
  FsFile() = default;
  FsFile(TestStorage* storage, std::filesystem::path path);
  ~FsFile();
  FsFile(FsFile&& other) noexcept;
  FsFile& operator=(FsFile&& other) noexcept;
  FsFile(const FsFile&) = delete;
  FsFile& operator=(const FsFile&) = delete;

  explicit operator bool() const { return open_; }
  bool isDirectory() const;
  size_t getName(char* destination, size_t capacity) const;
  FsFile openNextFile();
#ifndef SIMULATOR
  HalDirectoryNextStatus openNextFile(FsFile& entry);
#endif
  bool close();

 private:
  void closeHandle();
  void moveFrom(FsFile&& other);
  void openReusableEntry(TestStorage* storage, const std::filesystem::path& path);

  TestStorage* storage_ = nullptr;
  std::filesystem::path path_;
  std::vector<std::filesystem::path> entries_;
  size_t nextEntry_ = 0;
  bool open_ = false;
  bool reusableWrapperAllocated_ = false;
};

#define Storage TestStorage::instance()
