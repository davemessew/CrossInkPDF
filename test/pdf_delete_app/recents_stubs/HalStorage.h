#pragma once

#include <WString.h>
#include <fcntl.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

class HalStorage;

class HalFile {
 public:
  HalFile() = default;
  ~HalFile() { close(); }
  HalFile(HalFile&& other) noexcept { *this = std::move(other); }
  HalFile& operator=(HalFile&& other) noexcept;
  HalFile(const HalFile&) = delete;
  HalFile& operator=(const HalFile&) = delete;

  void flush() {}
  uint64_t fileSize64() const;
  size_t size() const { return static_cast<size_t>(fileSize64()); }
  int available() const;
  bool seek64(uint64_t offset);
  int read(void* destination, size_t length);
  int read() {
    uint8_t value = 0;
    return read(&value, 1) == 1 ? value : -1;
  }
  size_t write(const void* source, size_t length);
  bool sync() { return isOpen(); }
  bool isDirectory() const;
  bool close();
  bool isOpen() const { return storage_ != nullptr; }
  explicit operator bool() const { return isOpen(); }

 private:
  friend class HalStorage;
  HalStorage* storage_ = nullptr;
  std::string path_;
  uint64_t offset_ = 0;
  bool writeMode_ = false;
};

using FsFile = HalFile;

class HalStorage {
 public:
  struct Node {
    bool directory = false;
    std::vector<uint8_t> bytes;
  };

  static HalStorage instance;

  void reset() {
    nodes_.clear();
    failReadPath_.clear();
    activeHandles_ = 0;
    maximumHandles_ = 0;
    mkdir("/");
  }

  bool mkdir(const char* path, bool = true) {
    nodes_[path].directory = true;
    return true;
  }

  bool exists(const char* path) const { return nodes_.contains(path); }

  bool remove(const char* path) {
    const auto found = nodes_.find(path);
    if (found == nodes_.end() || found->second.directory) return false;
    nodes_.erase(found);
    return true;
  }

  bool rename(const char* oldPath, const char* newPath) {
    const auto found = nodes_.find(oldPath);
    if (found == nodes_.end() || nodes_.contains(newPath)) return false;
    nodes_[newPath] = std::move(found->second);
    nodes_.erase(found);
    return true;
  }

  bool rmdir(const char*) { return false; }
  bool removeDir(const char*) { return false; }

  HalFile open(const char* path, const int flags = O_RDONLY) {
    if ((flags & O_CREAT) != 0 && !nodes_.contains(path)) nodes_[path] = {};
    if ((flags & O_TRUNC) != 0 && nodes_.contains(path)) nodes_[path].bytes.clear();
    HalFile file;
    if (nodes_.contains(path)) openInto(file, path, (flags & (O_WRONLY | O_RDWR)) != 0);
    return file;
  }

  bool openFileForRead(const char*, const char* path, HalFile& file) {
    file.close();
    if (!nodes_.contains(path) || nodes_[path].directory) return false;
    openInto(file, path, false);
    return true;
  }

  bool openFileForWrite(const char*, const char* path, HalFile& file) {
    file.close();
    ensureParents(path);
    nodes_[path] = {};
    openInto(file, path, true);
    return true;
  }

  String readFile(const char* path) {
    HalFile file;
    if (!openFileForRead("test", path, file)) return {};
    std::string bytes(nodes_[path].bytes.begin(), nodes_[path].bytes.end());
    file.close();
    return String(bytes);
  }

  bool writeFile(const char* path, const String& value) {
    putText(path, std::string(value.c_str(), value.length()));
    return true;
  }

  void putText(const std::string& path, const std::string& value) {
    ensureParents(path);
    nodes_[path] = {false, std::vector<uint8_t>(value.begin(), value.end())};
  }

  std::string text(const std::string& path) const {
    const auto& bytes = nodes_.at(path).bytes;
    return std::string(bytes.begin(), bytes.end());
  }

  void failNextReadOf(const std::string& path) { failReadPath_ = path; }
  size_t maximumFileHandles() const { return maximumHandles_; }

 private:
  friend class HalFile;
  std::unordered_map<std::string, Node> nodes_;
  std::string failReadPath_;
  size_t activeHandles_ = 0;
  size_t maximumHandles_ = 0;

  void ensureParents(const std::string& path) {
    size_t slash = path.find('/', 1);
    while (slash != std::string::npos) {
      mkdir(path.substr(0, slash).c_str());
      slash = path.find('/', slash + 1);
    }
  }

  void openInto(HalFile& file, const std::string& path, const bool writeMode) {
    file.storage_ = this;
    file.path_ = path;
    file.offset_ = 0;
    file.writeMode_ = writeMode;
    ++activeHandles_;
    maximumHandles_ = std::max(maximumHandles_, activeHandles_);
  }
};

inline HalFile& HalFile::operator=(HalFile&& other) noexcept {
  if (this == &other) return *this;
  close();
  storage_ = other.storage_;
  path_ = std::move(other.path_);
  offset_ = other.offset_;
  writeMode_ = other.writeMode_;
  other.storage_ = nullptr;
  return *this;
}

inline uint64_t HalFile::fileSize64() const {
  return isOpen() ? storage_->nodes_.at(path_).bytes.size() : 0;
}

inline int HalFile::available() const {
  if (!isOpen()) return 0;
  const size_t size = storage_->nodes_.at(path_).bytes.size();
  return offset_ < size ? static_cast<int>(size - static_cast<size_t>(offset_)) : 0;
}

inline bool HalFile::seek64(const uint64_t offset) {
  if (!isOpen()) return false;
  offset_ = offset;
  return true;
}

inline int HalFile::read(void* destination, const size_t length) {
  if (!isOpen() || isDirectory()) return -1;
  if (storage_->failReadPath_ == path_) {
    storage_->failReadPath_.clear();
    return -1;
  }
  const auto& bytes = storage_->nodes_.at(path_).bytes;
  if (offset_ >= bytes.size()) return 0;
  const size_t count = std::min(length, bytes.size() - static_cast<size_t>(offset_));
  std::memcpy(destination, bytes.data() + offset_, count);
  offset_ += count;
  return static_cast<int>(count);
}

inline size_t HalFile::write(const void* source, const size_t length) {
  if (!isOpen() || isDirectory() || !writeMode_) return 0;
  auto& bytes = storage_->nodes_[path_].bytes;
  if (offset_ + length > bytes.size()) bytes.resize(static_cast<size_t>(offset_ + length));
  std::memcpy(bytes.data() + offset_, source, length);
  offset_ += length;
  return length;
}

inline bool HalFile::isDirectory() const {
  return isOpen() && storage_->nodes_.at(path_).directory;
}

inline bool HalFile::close() {
  if (!isOpen()) return true;
  --storage_->activeHandles_;
  storage_ = nullptr;
  return true;
}

inline HalStorage HalStorage::instance;

#define Storage HalStorage::instance
