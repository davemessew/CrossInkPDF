#include <HalStorage.h>

#include <system_error>
#include <utility>

#include "TestState.h"

FsFile::FsFile(std::filesystem::path path, const bool directory)
    : path_(std::move(path)), directory_(directory), open_(directory) {}

FsFile::FsFile(std::filesystem::path path, const std::ios::openmode mode)
    : path_(std::move(path)), stream_(path_, mode), open_(stream_.is_open()) {}

FsFile::~FsFile() { close(); }

bool FsFile::isDirectory() const {
  std::error_code error;
  return open_ && directory_ && std::filesystem::is_directory(path_, error);
}

int FsFile::read(void* const destination, const size_t count) {
  if (!open_ || directory_ || destination == nullptr) return 0;
  stream_.read(static_cast<char*>(destination), static_cast<std::streamsize>(count));
  return static_cast<int>(stream_.gcount());
}

size_t FsFile::write(const void* const source, const size_t count) {
  if (!open_ || directory_ || source == nullptr) return 0;
  stream_.write(static_cast<const char*>(source), static_cast<std::streamsize>(count));
  return stream_ ? count : 0;
}

size_t FsFile::fileSize() const {
  std::error_code error;
  const uintmax_t size = std::filesystem::file_size(path_, error);
  return error ? 0 : static_cast<size_t>(size);
}

bool FsFile::close() {
  if (stream_.is_open()) stream_.close();
  open_ = false;
  return true;
}

ProductionTestStorage& ProductionTestStorage::instance() {
  static ProductionTestStorage storage;
  return storage;
}

void ProductionTestStorage::reset(const std::filesystem::path& root) {
  root_ = root;
  failedMkdirPath_.clear();
  failedDirectoryOpenPath_.clear();
  failedExistingReadPath_.clear();
  failedWritePath_.clear();
  mkdirCalls_ = 0;
  writeAttempts_ = 0;
  writtenPaths_.clear();
  std::error_code error;
  std::filesystem::remove_all(root_, error);
  error.clear();
  std::filesystem::create_directories(root_, error);
}

std::filesystem::path ProductionTestStorage::physicalPath(const char* const path) const {
  if (path == nullptr) return {};
  std::string relative(path);
  while (!relative.empty() && (relative.front() == '/' || relative.front() == '\\')) {
    relative.erase(relative.begin());
  }
  return root_ / std::filesystem::path(relative);
}

bool ProductionTestStorage::exists(const char* const path) const {
  std::error_code error;
  return std::filesystem::exists(physicalPath(path), error);
}

bool ProductionTestStorage::mkdir(const char* const path, const bool parents) {
  ++mkdirCalls_;
  if (path == nullptr || failedMkdirPath_ == path) return false;
  std::error_code error;
  const std::filesystem::path physical = physicalPath(path);
  const bool created = parents ? std::filesystem::create_directories(physical, error)
                               : std::filesystem::create_directory(physical, error);
  return created || (!error && std::filesystem::is_directory(physical));
}

FsFile ProductionTestStorage::open(const char* const path) {
  if (path == nullptr || failedDirectoryOpenPath_ == path || !exists(path)) return {};
  const std::filesystem::path physical = physicalPath(path);
  std::error_code error;
  const bool directory = std::filesystem::is_directory(physical, error);
  return error ? FsFile{} : FsFile(physical, directory);
}

bool ProductionTestStorage::openFileForRead(const char*, const char* const path, FsFile& file) {
  file.close();
  if (path == nullptr || !exists(path)) return false;
  if (failedExistingReadPath_ == path) return false;
  file = FsFile(physicalPath(path), std::ios::in | std::ios::binary);
  return static_cast<bool>(file);
}

bool ProductionTestStorage::openFileForRead(const char* const moduleName, const std::string& path, FsFile& file) {
  return openFileForRead(moduleName, path.c_str(), file);
}

bool ProductionTestStorage::openFileForWrite(const char*, const char* const path, FsFile& file) {
  file.close();
  ++writeAttempts_;
  writtenPaths_.emplace_back(path == nullptr ? "" : path);
  if (path == nullptr || failedWritePath_ == path) return false;
  file = FsFile(physicalPath(path), std::ios::out | std::ios::binary | std::ios::trunc);
  return static_cast<bool>(file);
}

bool ProductionTestStorage::openFileForWrite(const char* const moduleName, const std::string& path, FsFile& file) {
  return openFileForWrite(moduleName, path.c_str(), file);
}

bool ProductionTestStorage::remove(const char* const path) {
  std::error_code error;
  return std::filesystem::remove(physicalPath(path), error) && !error;
}

bool ProductionTestStorage::rename(const char* const oldPath, const char* const newPath) {
  ++TEST_STATE.storageRenames;
  TEST_STATE.storageRenameOld = oldPath == nullptr ? "" : oldPath;
  TEST_STATE.storageRenameNew = newPath == nullptr ? "" : newPath;
  if (!TEST_STATE.storageRenameResult) return false;
  std::error_code error;
  std::filesystem::rename(physicalPath(oldPath), physicalPath(newPath), error);
  return !error;
}

void ProductionTestStorage::failMkdirOf(const std::string& path) { failedMkdirPath_ = path; }

void ProductionTestStorage::failDirectoryOpenOf(const std::string& path) {
  failedDirectoryOpenPath_ = path;
}

void ProductionTestStorage::failExistingReadOf(const std::string& path) {
  failedExistingReadPath_ = path;
}

void ProductionTestStorage::failWriteOf(const std::string& path) { failedWritePath_ = path; }
