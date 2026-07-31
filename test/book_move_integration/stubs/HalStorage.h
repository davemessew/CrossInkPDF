#pragma once

#include <fcntl.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

enum class HalDirectoryNextStatus : uint8_t {
  Entry,
  End,
  Error,
};

class HalStorage;

class HalFile {
 public:
  HalFile() = default;
  ~HalFile() { close(); }
  HalFile(HalFile&& other) noexcept { *this = std::move(other); }
  HalFile& operator=(HalFile&& other) noexcept;
  HalFile(const HalFile&) = delete;
  HalFile& operator=(const HalFile&) = delete;

  void flush();
  size_t getName(char* name, size_t length);
  uint64_t fileSize64() const;
  bool seek64(uint64_t offset);
  int read(void* destination, size_t length);
  size_t write(const void* source, size_t length);
  bool sync();
  bool isDirectory() const;
  size_t position() const {
    return isDirectory() ? directoryIndex_ : static_cast<size_t>(offset_);
  }
  bool close();
  HalFile openNextFile();
  HalDirectoryNextStatus openNextFile(HalFile& entry);
  bool isOpen() const { return storage_ != nullptr; }
  explicit operator bool() const { return isOpen(); }

 private:
  friend class HalStorage;
  HalStorage* storage_ = nullptr;
  std::string path_;
  uint64_t offset_ = 0;
  size_t directoryIndex_ = 0;
  bool writeMode_ = false;
  bool directDirectoryReader_ = false;
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
    activeFileHandles_ = 0;
    maximumFileHandles_ = 0;
    renameCalls_ = 0;
    failRenameCall_ = 0;
    closeCalls_ = 0;
    failCloseCall_ = 0;
    failShortWrite_ = false;
    failSync_ = false;
    failRead_ = false;
    failAllReads_ = false;
    failRemoveDir_ = false;
    failRemovePath_.clear();
    failOpenPath_.clear();
    failDirectoryIterationPath_.clear();
    activeDirectDirectoryReaders_ = 0;
    maximumDirectDirectoryReaders_ = 0;
    directoryIterationCalls_ = 0;
    pathOpenCounts_.clear();
    pathReadBytes_.clear();
    pathWrittenBytes_.clear();
    corruptOnSyncPath_.clear();
    events.clear();
    mkdir("/");
  }
  bool mkdir(const char* path, bool = true) {
    nodes_[path].directory = true;
    return true;
  }
  bool exists(const char* path) const { return nodes_.contains(path); }
  bool remove(const char* path) {
    if (failRemovePath_ == path) {
      failRemovePath_.clear();
      return false;
    }
    const auto found = nodes_.find(path);
    if (found == nodes_.end() || found->second.directory) return false;
    nodes_.erase(found);
    events.emplace_back(std::string("remove:") + path);
    return true;
  }
  bool rename(const char* oldPath, const char* newPath) {
    events.emplace_back(std::string("rename:") + oldPath + "->" + newPath);
    ++renameCalls_;
    if (failRenameCall_ != 0 && renameCalls_ == failRenameCall_) {
      failRenameCall_ = 0;
      return false;
    }
    const auto found = nodes_.find(oldPath);
    if (found == nodes_.end() || nodes_.contains(newPath)) return false;
    nodes_[newPath] = std::move(found->second);
    nodes_.erase(found);
    return true;
  }
  bool removeDir(const char* path) {
    if (failRemoveDir_) {
      failRemoveDir_ = false;
      return false;
    }
    const std::string prefix = std::string(path) + "/";
    for (auto it = nodes_.begin(); it != nodes_.end();) {
      if (it->first == path || it->first.rfind(prefix, 0) == 0) {
        it = nodes_.erase(it);
      } else {
        ++it;
      }
    }
    events.emplace_back(std::string("remove-dir:") + path);
    return true;
  }
  HalFile open(const char* path, int flags = O_RDONLY) {
    if (failOpenPath_ == path) {
      failOpenPath_.clear();
      return {};
    }
    if ((flags & O_CREAT) != 0 && !nodes_.contains(path)) nodes_[path] = {};
    if ((flags & O_TRUNC) != 0 && nodes_.contains(path)) nodes_[path].bytes.clear();
    if (!nodes_.contains(path)) return {};
    ++pathOpenCounts_[path];
    events.emplace_back(std::string(
                            (flags & (O_WRONLY | O_RDWR)) != 0
                                ? "open-file-write:"
                                : "open-file-read:") +
                        path);
    HalFile file;
    openInto(file, path, (flags & (O_WRONLY | O_RDWR)) != 0);
    if (file.isDirectory()) {
      file.directDirectoryReader_ = true;
      ++activeDirectDirectoryReaders_;
      maximumDirectDirectoryReaders_ =
          std::max(maximumDirectDirectoryReaders_, activeDirectDirectoryReaders_);
      events.emplace_back(std::string("open-direct:") + path);
    }
    return file;
  }
  bool openFileForRead(const char*, const char* path, HalFile& file) {
    file.close();
    if (!nodes_.contains(path)) return false;
    openInto(file, path, false);
    return true;
  }
  bool openFileForWrite(const char*, const char* path, HalFile& file) {
    file.close();
    nodes_[path] = {};
    openInto(file, path, true);
    return true;
  }
  void putFile(const std::string& path, std::vector<uint8_t> bytes) {
    ensureParents(path);
    nodes_[path] = {false, std::move(bytes)};
  }
  const std::vector<uint8_t>& bytes(const std::string& path) const { return nodes_.at(path).bytes; }
  void failNextRename() { failRenameOnCall(1); }
  void failRenameOnCall(size_t call) {
    renameCalls_ = 0;
    failRenameCall_ = call;
  }
  void failNextShortWrite() { failShortWrite_ = true; }
  void failNextSync() { failSync_ = true; }
  void failNextClose() { failCloseOnCall(1); }
  void failCloseOnCall(size_t call) {
    closeCalls_ = 0;
    failCloseCall_ = call;
  }
  void failNextRead() { failRead_ = true; }
  void failAllReads() { failAllReads_ = true; }
  void failNextRemoveDir() { failRemoveDir_ = true; }
  void failNextRemoveOf(const std::string& path) { failRemovePath_ = path; }
  void failNextOpenOf(const std::string& path) { failOpenPath_ = path; }
  void failNextDirectoryIterationOf(const std::string& path) { failDirectoryIterationPath_ = path; }
  void corruptOnNextSyncOf(const std::string& path) {
    corruptOnSyncPath_ = path;
  }
  size_t maximumFileHandles() const { return maximumFileHandles_; }
  size_t activeFileHandles() const { return activeFileHandles_; }
  size_t maximumDirectDirectoryReaders() const { return maximumDirectDirectoryReaders_; }
  size_t directoryIterationCalls() const { return directoryIterationCalls_; }
  size_t pathOpenCount(const std::string& path) const {
    const auto found = pathOpenCounts_.find(path);
    return found == pathOpenCounts_.end() ? 0U : found->second;
  }
  size_t pathReadBytes(const std::string& path) const {
    const auto found = pathReadBytes_.find(path);
    return found == pathReadBytes_.end() ? 0U : found->second;
  }
  size_t pathWrittenBytes(const std::string& path) const {
    const auto found = pathWrittenBytes_.find(path);
    return found == pathWrittenBytes_.end() ? 0U : found->second;
  }
  std::vector<std::string> events;

 private:
  friend class HalFile;
  std::unordered_map<std::string, Node> nodes_;
  size_t activeFileHandles_ = 0;
  size_t maximumFileHandles_ = 0;
  size_t renameCalls_ = 0;
  size_t failRenameCall_ = 0;
  size_t closeCalls_ = 0;
  size_t failCloseCall_ = 0;
  bool failShortWrite_ = false;
  bool failSync_ = false;
  bool failRead_ = false;
  bool failAllReads_ = false;
  bool failRemoveDir_ = false;
  std::string failRemovePath_;
  std::string failOpenPath_;
  std::string failDirectoryIterationPath_;
  size_t activeDirectDirectoryReaders_ = 0;
  size_t maximumDirectDirectoryReaders_ = 0;
  size_t directoryIterationCalls_ = 0;
  std::unordered_map<std::string, size_t> pathOpenCounts_;
  std::unordered_map<std::string, size_t> pathReadBytes_;
  std::unordered_map<std::string, size_t> pathWrittenBytes_;
  std::string corruptOnSyncPath_;

  void ensureParents(const std::string& path) {
    size_t slash = path.find('/', 1);
    while (slash != std::string::npos) {
      mkdir(path.substr(0, slash).c_str());
      slash = path.find('/', slash + 1);
    }
  }
  void openInto(HalFile& file, const std::string& path, bool writeMode) {
    file.storage_ = this;
    file.path_ = path;
    file.offset_ = 0;
    file.directoryIndex_ = 0;
    file.writeMode_ = writeMode;
    if (!nodes_[path].directory) {
      ++activeFileHandles_;
      maximumFileHandles_ = std::max(maximumFileHandles_, activeFileHandles_);
    }
  }
  std::vector<std::string> children(const std::string& directory) const {
    const std::string prefix = directory == "/" ? "/" : directory + "/";
    std::vector<std::string> result;
    for (const auto& [path, node] : nodes_) {
      (void)node;
      if (path.rfind(prefix, 0) != 0) continue;
      const std::string rest = path.substr(prefix.size());
      if (!rest.empty() && rest.find('/') == std::string::npos) result.push_back(path);
    }
    std::sort(result.begin(), result.end());
    return result;
  }
};

inline HalFile& HalFile::operator=(HalFile&& other) noexcept {
  if (this == &other) return *this;
  close();
  storage_ = other.storage_;
  path_ = std::move(other.path_);
  offset_ = other.offset_;
  directoryIndex_ = other.directoryIndex_;
  writeMode_ = other.writeMode_;
  directDirectoryReader_ = other.directDirectoryReader_;
  other.storage_ = nullptr;
  other.directDirectoryReader_ = false;
  return *this;
}

inline size_t HalFile::getName(char* name, size_t length) {
  const size_t slash = path_.rfind('/');
  const std::string leaf = slash == std::string::npos ? path_ : path_.substr(slash + 1);
  if (length == 0 || leaf.size() >= length) return 0;
  std::memcpy(name, leaf.c_str(), leaf.size() + 1);
  return leaf.size();
}

inline uint64_t HalFile::fileSize64() const { return isOpen() ? storage_->nodes_.at(path_).bytes.size() : 0; }

inline bool HalFile::seek64(uint64_t offset) {
  if (!isOpen()) return false;
  if (isDirectory()) {
    directoryIndex_ = static_cast<size_t>(offset);
  } else {
    offset_ = offset;
  }
  return true;
}

inline int HalFile::read(void* destination, size_t length) {
  if (!isOpen() || isDirectory()) return -1;
  if (storage_->failAllReads_) {
    return 0;
  }
  if (storage_->failRead_) {
    storage_->failRead_ = false;
    return 0;
  }
  const auto& bytes = storage_->nodes_.at(path_).bytes;
  if (offset_ >= bytes.size()) return 0;
  const size_t count = std::min(length, bytes.size() - static_cast<size_t>(offset_));
  std::memcpy(destination, bytes.data() + offset_, count);
  offset_ += count;
  storage_->pathReadBytes_[path_] += count;
  return static_cast<int>(count);
}

inline size_t HalFile::write(const void* source, size_t length) {
  if (!isOpen() || isDirectory() || !writeMode_) return 0;
  if (storage_->failShortWrite_) {
    storage_->failShortWrite_ = false;
    length = length == 0 ? 0 : length - 1U;
  }
  auto& bytes = storage_->nodes_[path_].bytes;
  if (offset_ + length > bytes.size()) bytes.resize(static_cast<size_t>(offset_ + length));
  std::memcpy(bytes.data() + offset_, source, length);
  offset_ += length;
  storage_->pathWrittenBytes_[path_] += length;
  return length;
}

inline bool HalFile::sync() {
  if (!isOpen()) return false;
  storage_->events.emplace_back(std::string("sync:") + path_);
  if (storage_->failSync_) {
    storage_->failSync_ = false;
    return false;
  }
  if (storage_->corruptOnSyncPath_ == path_) {
    storage_->corruptOnSyncPath_.clear();
    auto& bytes = storage_->nodes_[path_].bytes;
    if (!bytes.empty()) bytes.back() ^= 0x80U;
  }
  return true;
}

inline void HalFile::flush() {
  if (isOpen()) storage_->events.emplace_back(std::string("flush:") + path_);
}

inline bool HalFile::isDirectory() const { return isOpen() && storage_->nodes_.at(path_).directory; }

inline bool HalFile::close() {
  if (!isOpen()) return true;
  HalStorage* const storage = storage_;
  const std::string path = path_;
  if (!isDirectory()) --storage_->activeFileHandles_;
  if (directDirectoryReader_) {
    --storage_->activeDirectDirectoryReaders_;
    storage_->events.emplace_back(std::string("close-direct:") + path);
  }
  storage_ = nullptr;
  directDirectoryReader_ = false;
  storage->events.emplace_back(std::string("close:") + path);
  ++storage->closeCalls_;
  if (storage->failCloseCall_ != 0 && storage->closeCalls_ == storage->failCloseCall_) {
    storage->failCloseCall_ = 0;
    return false;
  }
  return true;
}

inline HalDirectoryNextStatus HalFile::openNextFile(HalFile& entry) {
  if (!isOpen() || !isDirectory()) return HalDirectoryNextStatus::Error;
  ++storage_->directoryIterationCalls_;
  if (storage_->failDirectoryIterationPath_ == path_) {
    storage_->failDirectoryIterationPath_.clear();
    return HalDirectoryNextStatus::Error;
  }
  const auto paths = storage_->children(path_);
  if (directoryIndex_ >= paths.size()) return HalDirectoryNextStatus::End;
  entry.close();
  storage_->openInto(entry, paths[directoryIndex_++], false);
  return HalDirectoryNextStatus::Entry;
}

inline HalFile HalFile::openNextFile() {
  HalFile entry;
  while (openNextFile(entry) == HalDirectoryNextStatus::Entry) {
    const size_t slash = entry.path_.rfind('/');
    const std::string leaf = slash == std::string::npos ? entry.path_ : entry.path_.substr(slash + 1U);
    if (leaf.empty() || leaf.front() != '.') return entry;
    entry.close();
  }
  return {};
}

inline HalStorage HalStorage::instance;

#define Storage HalStorage::instance
