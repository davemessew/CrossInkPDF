#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <vector>

class HalStorage;

using HalStorageReadHook = void (*)();
inline HalStorageReadHook halStorageReadHook = nullptr;
inline bool halStorageReadHookActive = false;

class FsFile {
  friend class HalStorage;

 public:
  int read(void* const destination, const size_t count) {
    if (!open_ || bytes_ == nullptr || destination == nullptr) {
      return -1;
    }
    if (halStorageReadHook != nullptr && !halStorageReadHookActive) {
      halStorageReadHookActive = true;
      halStorageReadHook();
      halStorageReadHookActive = false;
    }
    const size_t available = bytes_->size() - std::min(position_, bytes_->size());
    const size_t copied = std::min(count, available);
    if (copied != 0) {
      std::memcpy(destination, bytes_->data() + position_, copied);
      position_ += copied;
    }
    return static_cast<int>(copied);
  }

  size_t write(const void* const source, const size_t count) {
    if (!open_ || bytes_ == nullptr || !writable_ || source == nullptr) {
      return 0;
    }
    if (position_ + count > bytes_->size()) {
      bytes_->resize(position_ + count);
    }
    std::memcpy(bytes_->data() + position_, source, count);
    position_ += count;
    return count;
  }

  bool seek(const size_t position) {
    if (!open_ || bytes_ == nullptr || position > bytes_->size()) {
      return false;
    }
    position_ = position;
    return true;
  }

  size_t size() const { return bytes_ == nullptr ? 0 : bytes_->size(); }
  bool close() {
    open_ = false;
    bytes_ = nullptr;
    position_ = 0;
    writable_ = false;
    return true;
  }
  bool isOpen() const { return open_; }
  explicit operator bool() const { return open_; }

 private:
  std::vector<uint8_t>* bytes_ = nullptr;
  size_t position_ = 0;
  bool writable_ = false;
  bool open_ = false;
};

using HalFile = FsFile;

class HalStorage {
 public:
  static HalStorage& getInstance() {
    static HalStorage storage;
    return storage;
  }

  void reset() {
    files_.clear();
    opens_ = 0;
    halStorageReadHook = nullptr;
    halStorageReadHookActive = false;
  }

  void addFile(const std::string& path, const std::vector<uint8_t>& bytes) { files_[path] = bytes; }

  bool openFileForRead(const char*, const char* const path, FsFile& file) {
    auto found = files_.find(path == nullptr ? std::string{} : std::string(path));
    if (found == files_.end()) {
      return false;
    }
    ++opens_;
    file.bytes_ = &found->second;
    file.position_ = 0;
    file.writable_ = false;
    file.open_ = true;
    return true;
  }

  bool openFileForRead(const char* module, const std::string& path, FsFile& file) {
    return openFileForRead(module, path.c_str(), file);
  }

  bool openFileForWrite(const char*, const char* const path, FsFile& file) {
    if (path == nullptr) {
      return false;
    }
    ++opens_;
    std::vector<uint8_t>& bytes = files_[path];
    bytes.clear();
    file.bytes_ = &bytes;
    file.position_ = 0;
    file.writable_ = true;
    file.open_ = true;
    return true;
  }

  bool openFileForWrite(const char* module, const std::string& path, FsFile& file) {
    return openFileForWrite(module, path.c_str(), file);
  }

  bool exists(const char* const path) const {
    return path != nullptr && files_.find(path) != files_.end();
  }
  bool remove(const char* const path) { return path != nullptr && files_.erase(path) != 0; }
  bool remove(const std::string& path) { return remove(path.c_str()); }
  uint32_t opens() const { return opens_; }
  void setReadHook(const HalStorageReadHook hook) { halStorageReadHook = hook; }

 private:
  std::map<std::string, std::vector<uint8_t>> files_;
  uint32_t opens_ = 0;
};

#define Storage HalStorage::getInstance()
