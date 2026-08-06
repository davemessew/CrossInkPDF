#include "TestStorage.h"

#include <algorithm>
#include <cstring>
#include <system_error>

TestStorage& TestStorage::instance() {
  static TestStorage storage;
  return storage;
}

void TestStorage::reset(const std::filesystem::path& root) {
  root_ = root;
  failedRemovalPath_.clear();
  failedOpenPath_.clear();
  failNextChildWrapperAllocation_ = false;
  failedDirectoryReadAfter_ = std::numeric_limits<size_t>::max();
  openHandles_ = 0;
  maxOpenHandles_ = 0;
  activeChildWrappers_ = 0;
  maxChildWrappers_ = 0;
  childWrapperAllocations_ = 0;
  openCalls_ = 0;
  removeCalls_ = 0;
  removeDirCalls_ = 0;
  std::error_code error;
  std::filesystem::remove_all(root_, error);
  std::filesystem::create_directories(root_, error);
}

std::filesystem::path TestStorage::physicalPath(const char* const path) const {
  if (path == nullptr) {
    return {};
  }
  std::string relative(path);
  while (!relative.empty() && (relative.front() == '/' || relative.front() == '\\')) {
    relative.erase(relative.begin());
  }
  return root_ / std::filesystem::path(relative);
}

bool TestStorage::exists(const char* const path) const {
  std::error_code error;
  return std::filesystem::exists(physicalPath(path), error);
}

bool TestStorage::mkdir(const char* const path, const bool parents) {
  std::error_code error;
  return parents ? std::filesystem::create_directories(physicalPath(path), error) || !error
                 : std::filesystem::create_directory(physicalPath(path), error) || !error;
}

bool TestStorage::shouldFailRemoval(const char* const path) const {
  return path != nullptr && failedRemovalPath_ == path;
}

bool TestStorage::remove(const char* const path) {
  ++removeCalls_;
  if (shouldFailRemoval(path)) {
    return false;
  }
  std::error_code error;
  const bool removed = std::filesystem::remove(physicalPath(path), error);
  return !error && removed;
}

bool TestStorage::rename(const char* const oldPath, const char* const newPath) {
  std::error_code error;
  std::filesystem::rename(physicalPath(oldPath), physicalPath(newPath), error);
  return !error;
}

bool TestStorage::removeDir(const char* const path) {
  ++removeDirCalls_;
  if (shouldFailRemoval(path)) {
    return false;
  }
  std::error_code error;
  const auto removed = std::filesystem::remove_all(physicalPath(path), error);
  return !error && removed != 0;
}

FsFile TestStorage::open(const char* const path) {
  ++openCalls_;
  if (path == nullptr || failedOpenPath_ == path || !exists(path)) {
    return {};
  }
  return FsFile(this, physicalPath(path));
}

void TestStorage::failRemovalOf(const std::string& path) { failedRemovalPath_ = path; }

void TestStorage::failOpenOf(const std::string& path) { failedOpenPath_ = path; }

void TestStorage::failNextChildWrapperAllocation() { failNextChildWrapperAllocation_ = true; }

void TestStorage::failDirectoryReadAfter(const size_t successfulEntries) {
  failedDirectoryReadAfter_ = successfulEntries;
}

void TestStorage::clearFailures() {
  failedRemovalPath_.clear();
  failedOpenPath_.clear();
  failNextChildWrapperAllocation_ = false;
  failedDirectoryReadAfter_ = std::numeric_limits<size_t>::max();
}

void TestStorage::opened() {
  ++openHandles_;
  maxOpenHandles_ = std::max(maxOpenHandles_, openHandles_);
}

void TestStorage::closed() {
  if (openHandles_ != 0) {
    --openHandles_;
  }
}

bool TestStorage::allocateChildWrapper() {
  if (failNextChildWrapperAllocation_) {
    failNextChildWrapperAllocation_ = false;
    return false;
  }
  ++childWrapperAllocations_;
  ++activeChildWrappers_;
  maxChildWrappers_ = std::max(maxChildWrappers_, activeChildWrappers_);
  return true;
}

void TestStorage::releaseChildWrapper() {
  if (activeChildWrappers_ != 0) {
    --activeChildWrappers_;
  }
}

bool TestStorage::shouldFailDirectoryRead(const size_t successfulEntries) const {
  return successfulEntries >= failedDirectoryReadAfter_;
}

FsFile::FsFile(TestStorage* const storage, std::filesystem::path path)
    : storage_(storage), path_(std::move(path)), open_(true) {
  storage_->opened();
  if (isDirectory()) {
    std::error_code error;
    for (const auto& entry : std::filesystem::directory_iterator(path_, error)) {
      entries_.push_back(entry.path());
    }
  }
}

FsFile::~FsFile() { close(); }

FsFile::FsFile(FsFile&& other) noexcept { moveFrom(std::move(other)); }

FsFile& FsFile::operator=(FsFile&& other) noexcept {
  if (this != &other) {
    close();
    moveFrom(std::move(other));
  }
  return *this;
}

void FsFile::moveFrom(FsFile&& other) {
  storage_ = other.storage_;
  path_ = std::move(other.path_);
  entries_ = std::move(other.entries_);
  nextEntry_ = other.nextEntry_;
  open_ = other.open_;
  reusableWrapperAllocated_ = other.reusableWrapperAllocated_;
  other.storage_ = nullptr;
  other.open_ = false;
  other.reusableWrapperAllocated_ = false;
}

bool FsFile::isDirectory() const {
  std::error_code error;
  return open_ && std::filesystem::is_directory(path_, error);
}

size_t FsFile::getName(char* const destination, const size_t capacity) const {
  if (!open_ || destination == nullptr || capacity == 0) {
    return 0;
  }
  const std::string name = path_.filename().string();
  if (name.size() + 1 > capacity) {
    destination[0] = '\0';
    return 0;
  }
  std::memcpy(destination, name.c_str(), name.size() + 1);
  return name.size();
}

FsFile FsFile::openNextFile() {
  if (!open_ || !isDirectory() || nextEntry_ >= entries_.size()) {
    return {};
  }
  return FsFile(storage_, entries_[nextEntry_++]);
}

HalDirectoryNextStatus FsFile::openNextFile(FsFile& entry) {
  if (!open_ || !isDirectory() || &entry == this) {
    return HalDirectoryNextStatus::Error;
  }
  if (!entry.reusableWrapperAllocated_) {
    if (!storage_->allocateChildWrapper()) {
      return HalDirectoryNextStatus::Error;
    }
    entry.reusableWrapperAllocated_ = true;
    entry.storage_ = storage_;
  }

  entry.closeHandle();
  if (storage_->shouldFailDirectoryRead(nextEntry_)) {
    return HalDirectoryNextStatus::Error;
  }
  if (nextEntry_ >= entries_.size()) {
    return HalDirectoryNextStatus::End;
  }
  entry.openReusableEntry(storage_, entries_[nextEntry_++]);
  return HalDirectoryNextStatus::Entry;
}

void FsFile::closeHandle() {
  if (open_ && storage_ != nullptr) {
    storage_->closed();
  }
  open_ = false;
  entries_.clear();
  nextEntry_ = 0;
}

void FsFile::openReusableEntry(TestStorage* const storage, const std::filesystem::path& path) {
  storage_ = storage;
  path_ = path;
  open_ = true;
  storage_->opened();
}

bool FsFile::close() {
  TestStorage* const storage = storage_;
  closeHandle();
  if (reusableWrapperAllocated_ && storage != nullptr) {
    storage->releaseChildWrapper();
  }
  reusableWrapperAllocated_ = false;
  storage_ = nullptr;
  return true;
}
