#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <vector>

class FsFile {
 public:
  FsFile() = default;
  explicit FsFile(const std::vector<uint8_t>* bytes, uint32_t* activeReaders, uint32_t* closeCalls)
      : bytes_(bytes), activeReaders_(activeReaders), closeCalls_(closeCalls) {}
  ~FsFile() { close(); }

  FsFile(const FsFile&) = delete;
  FsFile& operator=(const FsFile&) = delete;

  FsFile(FsFile&& other) noexcept { moveFrom(other); }
  FsFile& operator=(FsFile&& other) noexcept {
    if (this != &other) {
      close();
      moveFrom(other);
    }
    return *this;
  }

  int read(void* destination, const size_t requested) {
    if (bytes_ == nullptr || destination == nullptr) {
      return -1;
    }
    const size_t available = bytes_->size() - std::min(position_, bytes_->size());
    const size_t count = std::min(requested, available);
    if (count != 0) {
      std::memcpy(destination, bytes_->data() + position_, count);
      position_ += count;
    }
    return static_cast<int>(count);
  }

  bool close() {
    if (bytes_ != nullptr) {
      if (activeReaders_ != nullptr && *activeReaders_ != 0) {
        --*activeReaders_;
      }
      if (closeCalls_ != nullptr) {
        ++*closeCalls_;
      }
    }
    bytes_ = nullptr;
    position_ = 0;
    activeReaders_ = nullptr;
    closeCalls_ = nullptr;
    return true;
  }

  size_t size() const { return bytes_ == nullptr ? 0 : bytes_->size(); }
  uint64_t fileSize64() const { return size(); }
  size_t position() const { return position_; }
  bool seek(const size_t position) {
    if (bytes_ == nullptr || position > bytes_->size()) {
      return false;
    }
    position_ = position;
    return true;
  }

  explicit operator bool() const { return bytes_ != nullptr; }

 private:
  void moveFrom(FsFile& other) {
    bytes_ = other.bytes_;
    position_ = other.position_;
    activeReaders_ = other.activeReaders_;
    closeCalls_ = other.closeCalls_;
    other.bytes_ = nullptr;
    other.position_ = 0;
    other.activeReaders_ = nullptr;
    other.closeCalls_ = nullptr;
  }

  const std::vector<uint8_t>* bytes_ = nullptr;
  size_t position_ = 0;
  uint32_t* activeReaders_ = nullptr;
  uint32_t* closeCalls_ = nullptr;
};

class TestHalStorage {
 public:
  void reset() {
    files_.clear();
    pathOpenCalls_.clear();
    openCalls_ = 0;
    closeCalls_ = 0;
    activeReaders_ = 0;
    maximumActiveReaders_ = 0;
  }

  void addFile(const std::string& path, const std::vector<uint8_t>& bytes) { files_[path] = bytes; }

  bool exists(const char* path) const { return path != nullptr && files_.find(path) != files_.end(); }

  bool openFileForRead(const char*, const std::string& path, FsFile& file) {
    ++openCalls_;
    ++pathOpenCalls_[path];
    const auto found = files_.find(path);
    if (found == files_.end()) {
      return false;
    }
    ++activeReaders_;
    maximumActiveReaders_ = std::max(maximumActiveReaders_, activeReaders_);
    file = FsFile(&found->second, &activeReaders_, &closeCalls_);
    return true;
  }

  uint32_t openCalls() const { return openCalls_; }
  uint32_t closeCalls() const { return closeCalls_; }
  uint32_t activeReaders() const { return activeReaders_; }
  uint32_t maximumActiveReaders() const { return maximumActiveReaders_; }
  uint32_t openCallsForPath(const std::string& path) const {
    const auto found = pathOpenCalls_.find(path);
    return found == pathOpenCalls_.end() ? 0 : found->second;
  }

 private:
  std::map<std::string, std::vector<uint8_t>> files_;
  std::map<std::string, uint32_t> pathOpenCalls_;
  uint32_t openCalls_ = 0;
  uint32_t closeCalls_ = 0;
  uint32_t activeReaders_ = 0;
  uint32_t maximumActiveReaders_ = 0;
};

inline TestHalStorage Storage;
