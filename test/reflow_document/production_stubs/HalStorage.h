#pragma once

#include <Arduino.h>
#include <Print.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>

#include "EpubProductionTestState.h"

class FsFile final : public Print {
 public:
  FsFile() = default;

  explicit operator bool() const { return fixture_ != nullptr; }

  size_t size() const {
    if (!fixture_) return 0;
    return fixture_->reportedSize == epub_production_test::kUseDataSize ? fixture_->data.size()
                                                                             : fixture_->reportedSize;
  }

  int available() const {
    if (!fixture_ || position_ >= fixture_->data.size()) return 0;
    return static_cast<int>(fixture_->data.size() - position_);
  }

  int read(void* destination, const size_t requested) {
    if (!fixture_ || destination == nullptr) return -1;
    auto& storage = epub_production_test::storage;
    storage.requestedReadCapacities.push_back(requested);
    const size_t readOffset = position_;
    const size_t readable = fixture_->readableSize == epub_production_test::kUseDataSize
                                ? fixture_->data.size()
                                : fixture_->readableSize;
    const size_t remaining = position_ < fixture_->data.size() ? fixture_->data.size() - position_ : 0;
    const size_t copied = std::min({requested, readable, remaining});
    if (copied != 0) {
      std::memcpy(destination, fixture_->data.data() + position_, copied);
      position_ += copied;
    }
    storage.readObservations.push_back({path_, readOffset, requested, copied});
    return static_cast<int>(copied);
  }

  size_t write(const uint8_t data) override { return write(&data, 1); }

  size_t write(const uint8_t* data, const size_t size) override {
    if (!fixture_ || data == nullptr) return 0;
    auto& storage = epub_production_test::storage;
    const size_t writeOffset = position_;
    size_t written = size;
    const bool injectedPatchFault =
        patchMode_ && path_ == storage.failPatchWritePath && position_ == storage.failPatchWriteOffset;
    if (injectedPatchFault) {
      written = storage.shortPatchWrite && size != 0 ? size - 1U : 0;
    }
    if (position_ + written > fixture_->data.size()) fixture_->data.resize(position_ + written);
    std::copy_n(data, written, fixture_->data.begin() + static_cast<std::ptrdiff_t>(position_));
    position_ += written;
    fixture_->reportedSize = fixture_->data.size();
    if (injectedPatchFault) {
      const char* operation = nullptr;
      if (storage.shortPatchWrite) {
        ++storage.shortPatchFaultsReached;
        operation = "patch-short:";
      } else if (writeOffset == 0) {
        ++storage.headerPatchFaultsReached;
        operation = "patch-header-failed:";
      } else {
        ++storage.footerPatchFaultsReached;
        operation = "patch-footer-failed:";
      }
      storage.recordBoundary(std::string(operation) + path_ + "@" + std::to_string(writeOffset));
    }
    return written;
  }

  bool seek(const uint32_t position) {
    if (!fixture_) return false;
    position_ = position;
    return true;
  }

  bool seekCur(const int32_t delta) {
    if (!fixture_ || (delta < 0 && static_cast<size_t>(-delta) > position_)) return false;
    position_ = static_cast<size_t>(static_cast<int64_t>(position_) + delta);
    return true;
  }

  uint32_t position() const { return static_cast<uint32_t>(position_); }
  void flush() {}
  bool sync() {
    if (fixture_ == nullptr) return false;
    auto& storage = epub_production_test::storage;
    const size_t occurrence = ++storage.syncCounts[path_];
    const bool fails = path_ == storage.failSyncPath && occurrence == storage.failSyncOccurrence;
    if (fails) ++storage.syncFaultsReached;
    storage.recordBoundary(std::string(fails ? "sync-failed:" : "sync:") + path_, path_);
    return !fails;
  }

  bool close() {
    if (!fixture_) return true;
    const bool succeeds = fixture_->closeSucceeds;
    ++epub_production_test::storage.closeCount;
    fixture_ = nullptr;
    position_ = 0;
    path_.clear();
    patchMode_ = false;
    return succeeds;
  }

 private:
  friend class HalStorage;
  epub_production_test::FileFixture* fixture_ = nullptr;
  size_t position_ = 0;
  std::string path_;
  bool patchMode_ = false;
};

using HalFile = FsFile;

inline constexpr int O_RDWR = 2;

class HalStorage {
 public:
  static HalStorage& getInstance() {
    static HalStorage instance;
    return instance;
  }

  bool exists(const char* path) const {
    epub_production_test::storage.recordPath("exists:", path);
    return path != nullptr && epub_production_test::storage.files.contains(path);
  }
  bool exists(const std::string& path) const { return exists(path.c_str()); }

  bool openFileForRead(const char*, const char* path, FsFile& file) {
    if (path == nullptr) return false;
    auto found = epub_production_test::storage.files.find(path);
    if (found == epub_production_test::storage.files.end()) return false;
    epub_production_test::storage.openedReadPaths.emplace_back(path);
    file.fixture_ = &found->second;
    file.position_ = 0;
    file.path_ = path;
    file.patchMode_ = false;
    return true;
  }
  bool openFileForRead(const char* tag, const std::string& path, FsFile& file) {
    return openFileForRead(tag, path.c_str(), file);
  }

  bool openFileForWrite(const char*, const char* path, FsFile& file) {
    if (path == nullptr) return false;
    epub_production_test::storage.openedWritePaths.emplace_back(path);
    if (epub_production_test::storage.failWrites) return false;
    auto& fixture = epub_production_test::storage.files[path];
    fixture = {};
    file.fixture_ = &fixture;
    file.position_ = 0;
    file.path_ = path;
    file.patchMode_ = false;
    return true;
  }
  bool openFileForWrite(const char* tag, const std::string& path, FsFile& file) {
    return openFileForWrite(tag, path.c_str(), file);
  }

  FsFile open(const char* path, int) {
    FsFile file;
    if (path == nullptr) return file;
    auto found = epub_production_test::storage.files.find(path);
    if (found == epub_production_test::storage.files.end()) return file;
    file.fixture_ = &found->second;
    file.position_ = 0;
    file.path_ = path;
    file.patchMode_ = true;
    return file;
  }

  bool remove(const char* path) {
    epub_production_test::storage.recordPath("remove:", path);
    if (path != nullptr && epub_production_test::storage.failRemovePath == path) {
      ++epub_production_test::storage.removeFaultsReached;
      epub_production_test::storage.recordBoundary(std::string("remove-failed:") + path);
      return false;
    }
    const bool removed = path != nullptr && epub_production_test::storage.files.erase(path) == 1;
    if (removed) epub_production_test::storage.recordBoundary(std::string("remove:") + path);
    return removed;
  }
  bool remove(const std::string& path) { return remove(path.c_str()); }

  bool rename(const char* oldPath, const char* newPath) {
    epub_production_test::storage.recordPath("rename-old:", oldPath);
    epub_production_test::storage.recordPath("rename-new:", newPath);
    if (oldPath == nullptr || newPath == nullptr) return false;
    if (epub_production_test::storage.failRename ||
        epub_production_test::storage.failRenameOldPath == oldPath) {
      ++epub_production_test::storage.renameFaultsReached;
      epub_production_test::storage.recordBoundary(std::string("rename-failed:") + oldPath + "->" + newPath);
      return false;
    }
    auto found = epub_production_test::storage.files.find(oldPath);
    if (found == epub_production_test::storage.files.end()) return false;
    epub_production_test::storage.files[newPath] = std::move(found->second);
    epub_production_test::storage.files.erase(found);
    auto& storage = epub_production_test::storage;
    if (storage.corruptRenameDestinationPath == newPath) {
      auto& destination = storage.files[newPath].data;
      if (storage.corruptRenameOffset < destination.size()) {
        destination[storage.corruptRenameOffset] ^= storage.corruptRenameXor;
        ++storage.renameCorruptionsReached;
      }
    }
    epub_production_test::storage.recordBoundary(std::string("rename:") + oldPath + "->" + newPath);
    return true;
  }
  bool rename(const std::string& oldPath, const std::string& newPath) {
    return rename(oldPath.c_str(), newPath.c_str());
  }

  bool mkdir(const char*) { return true; }
  bool mkdir(const std::string& path) { return mkdir(path.c_str()); }

  bool removeDir(const char* path) {
    epub_production_test::storage.recordPath("remove-dir:", path);
    if (path == nullptr) return false;
    if (epub_production_test::storage.failRemoveDir) return false;
    const std::string_view prefix(path);
    for (auto it = epub_production_test::storage.files.begin(); it != epub_production_test::storage.files.end();) {
      if (std::string_view(it->first).starts_with(prefix)) {
        it = epub_production_test::storage.files.erase(it);
      } else {
        ++it;
      }
    }
    return true;
  }
};

#define Storage HalStorage::getInstance()
