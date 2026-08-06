#pragma once

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

class FsFile {
 public:
  FsFile() = default;
  FsFile(std::filesystem::path path, bool directory);
  FsFile(std::filesystem::path path, std::ios::openmode mode);
  ~FsFile();
  FsFile(FsFile&&) noexcept = default;
  FsFile& operator=(FsFile&&) noexcept = default;
  FsFile(const FsFile&) = delete;
  FsFile& operator=(const FsFile&) = delete;

  explicit operator bool() const { return open_; }
  bool isDirectory() const;
  int read(void* destination, size_t count);
  size_t write(const void* source, size_t count);
  size_t fileSize() const;
  bool close();

 private:
  std::filesystem::path path_;
  std::fstream stream_;
  bool directory_ = false;
  bool open_ = false;
};

class ProductionTestStorage {
 public:
  static ProductionTestStorage& instance();

  void reset(const std::filesystem::path& root);
  std::filesystem::path physicalPath(const char* path) const;
  bool exists(const char* path) const;
  bool mkdir(const char* path, bool parents = true);
  FsFile open(const char* path);
  bool openFileForRead(const char* moduleName, const char* path, FsFile& file);
  bool openFileForRead(const char* moduleName, const std::string& path, FsFile& file);
  bool openFileForWrite(const char* moduleName, const char* path, FsFile& file);
  bool openFileForWrite(const char* moduleName, const std::string& path, FsFile& file);
  bool remove(const char* path);
  bool rename(const char* oldPath, const char* newPath);

  void failMkdirOf(const std::string& path);
  void failDirectoryOpenOf(const std::string& path);
  void failExistingReadOf(const std::string& path);
  void failWriteOf(const std::string& path);
  size_t mkdirCallCount() const { return mkdirCalls_; }
  size_t writeAttemptCount() const { return writeAttempts_; }
  const std::vector<std::string>& writtenPaths() const { return writtenPaths_; }

 private:
  std::filesystem::path root_;
  std::string failedMkdirPath_;
  std::string failedDirectoryOpenPath_;
  std::string failedExistingReadPath_;
  std::string failedWritePath_;
  size_t mkdirCalls_ = 0;
  size_t writeAttempts_ = 0;
  std::vector<std::string> writtenPaths_;
};

#define Storage ProductionTestStorage::instance()
