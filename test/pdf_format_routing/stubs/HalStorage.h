#pragma once

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

struct TestDirectoryEntry {
  std::string name;
  bool directory = false;
};

class HalFile {
 public:
  explicit operator bool() const { return kind_ != Kind::Invalid; }

  bool isDirectory() const {
    return kind_ == Kind::Directory || (kind_ == Kind::Entry && entries_ != nullptr && entry_ < entries_->size() &&
                                        (*entries_)[entry_].directory);
  }

  void rewindDirectory() {
    if (kind_ == Kind::Directory) {
      cursor_ = 0;
    }
  }

  HalFile openNextFile() {
    if (kind_ != Kind::Directory || entries_ == nullptr) {
      return {};
    }
    ++(*openNextCalls_);
    if (cursor_ >= entries_->size()) {
      return {};
    }
    HalFile file;
    file.kind_ = Kind::Entry;
    file.entries_ = entries_;
    file.entry_ = cursor_++;
    file.openNextCalls_ = openNextCalls_;
    return file;
  }

  void getName(char* const output, const size_t capacity) const {
    if (output == nullptr || capacity == 0 || kind_ != Kind::Entry || entries_ == nullptr ||
        entry_ >= entries_->size()) {
      return;
    }
    const std::string& name = (*entries_)[entry_].name;
    const size_t length = std::min(name.size(), capacity - 1);
    std::memcpy(output, name.data(), length);
    output[length] = '\0';
  }

  void close() { kind_ = Kind::Invalid; }

 private:
  enum class Kind { Invalid, Directory, Entry };

  Kind kind_ = Kind::Invalid;
  std::vector<TestDirectoryEntry>* entries_ = nullptr;
  size_t cursor_ = 0;
  size_t entry_ = 0;
  size_t* openNextCalls_ = nullptr;

  friend class HalStorage;
};

class HalStorage {
 public:
  static HalStorage& getInstance() {
    static HalStorage instance;
    return instance;
  }

  void setDirectory(std::string path, std::vector<TestDirectoryEntry> entries) {
    path_ = std::move(path);
    entries_ = std::move(entries);
    openSucceeds_ = true;
    openCalls_ = 0;
    openNextCalls_ = 0;
  }

  void setOpenFailure() {
    openSucceeds_ = false;
    openCalls_ = 0;
    openNextCalls_ = 0;
  }

  HalFile open(const char* const path) {
    ++openCalls_;
    if (!openSucceeds_ || path == nullptr || path_ != path) {
      return {};
    }
    HalFile directory;
    directory.kind_ = HalFile::Kind::Directory;
    directory.entries_ = &entries_;
    directory.openNextCalls_ = &openNextCalls_;
    return directory;
  }

  size_t openCalls() const { return openCalls_; }
  size_t openNextCalls() const { return openNextCalls_; }

 private:
  std::string path_;
  std::vector<TestDirectoryEntry> entries_;
  bool openSucceeds_ = false;
  size_t openCalls_ = 0;
  size_t openNextCalls_ = 0;
};

#define Storage HalStorage::getInstance()
