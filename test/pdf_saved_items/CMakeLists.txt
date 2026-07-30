if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  cmake_minimum_required(VERSION 3.16)
  project(pdf_saved_items_tests CXX)

  set(CMAKE_CXX_STANDARD 20)
  set(CMAKE_CXX_STANDARD_REQUIRED ON)
  set(CMAKE_CXX_EXTENSIONS OFF)

  include(FetchContent)
  FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG v1.17.0
  )
  set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
  set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
  set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(googletest)

  get_filename_component(REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
  add_library(crosspoint_test_common INTERFACE)
  target_include_directories(crosspoint_test_common INTERFACE
    ${REPO_ROOT}
    ${REPO_ROOT}/lib
  )
  target_compile_options(crosspoint_test_common INTERFACE
    -Wall
    -Wextra
    -pedantic
  )

  enable_testing()
  include(GoogleTest)
endif()

add_executable(PdfSavedItemsTest
  ${REPO_ROOT}/lib/PdfReflow/PdfSavedItemsStore.cpp
  ${REPO_ROOT}/lib/PdfReflow/PdfSavedItemWordMap.cpp
  ${REPO_ROOT}/test/pdf_cache_recovery/PdfTestCacheIo.cpp
  PdfSavedItemsTest.cpp
)

target_include_directories(PdfSavedItemsTest PRIVATE
  ${REPO_ROOT}/lib/PdfReflow
  ${REPO_ROOT}/test/pdf_cache_recovery
)

target_link_libraries(PdfSavedItemsTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
  # The allocation interceptor deliberately pairs malloc/free behind global
  # new/delete; GCC otherwise reports this intentional test-only arrangement.
  target_compile_options(PdfSavedItemsTest PRIVATE -Wno-mismatched-new-delete)
endif()

gtest_discover_tests(PdfSavedItemsTest TEST_PREFIX "PdfSavedItems")

set(PDF_SAVED_ITEMS_STUB_DIR "${CMAKE_CURRENT_BINARY_DIR}/legacy_store_stubs")
file(MAKE_DIRECTORY "${PDF_SAVED_ITEMS_STUB_DIR}")

file(WRITE "${PDF_SAVED_ITEMS_STUB_DIR}/Arduino.h" [=[
#pragma once
#include <cstdint>
inline unsigned long millis() { return 123000UL; }
]=])

file(WRITE "${PDF_SAVED_ITEMS_STUB_DIR}/Logging.h" [=[
#pragma once
template <typename... Args>
inline void pdfSavedItemsTestLogSink(Args&&...) {}
#define LOG_DBG(...) pdfSavedItemsTestLogSink(__VA_ARGS__)
#define LOG_ERR(...) pdfSavedItemsTestLogSink(__VA_ARGS__)
#define LOG_INF(...) pdfSavedItemsTestLogSink(__VA_ARGS__)
]=])

file(WRITE "${PDF_SAVED_ITEMS_STUB_DIR}/uzlib.h" [=[
#pragma once
#include <cstdint>
extern "C" inline uint32_t uzlib_crc32(const void* data, unsigned int length, uint32_t crc) {
  const auto* bytes = static_cast<const uint8_t*>(data);
  crc = ~crc;
  while (length-- != 0U) {
    crc ^= *bytes++;
    for (uint8_t bit = 0; bit < 8; ++bit) {
      crc = (crc >> 1U) ^ (0xedb88320U & (0U - (crc & 1U)));
    }
  }
  return ~crc;
}
]=])

file(WRITE "${PDF_SAVED_ITEMS_STUB_DIR}/HalStorage.h" [=[
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

using String = std::string;
using oflag_t = int;
inline constexpr oflag_t O_RDONLY = 0x01;
inline constexpr oflag_t O_WRONLY = 0x02;
inline constexpr oflag_t O_CREAT = 0x04;
inline constexpr oflag_t O_TRUNC = 0x08;

enum class TestStorageFault : uint8_t {
  None,
  Read,
  Write,
  Flush,
  Sync,
  Close,
  Rename,
  Remove,
};

class HalStorage;

class FsFile {
 public:
  FsFile() = default;
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
  ~FsFile() { close(); }

  explicit operator bool() const { return bytes_ != nullptr; }
  int available() const {
    return bytes_ == nullptr ? 0 : static_cast<int>(bytes_->size() - std::min(position_, bytes_->size()));
  }
  int read(void* destination, size_t count);
  int read();
  size_t write(const void* source, size_t count);
  size_t write(uint8_t value);
  void flush();
  bool sync();
  bool close();

 private:
  friend class HalStorage;
  FsFile(HalStorage* owner, std::vector<uint8_t>* bytes, bool writable)
      : owner_(owner), bytes_(bytes), writable_(writable) {}
  void moveFrom(FsFile& other) {
    owner_ = other.owner_;
    bytes_ = other.bytes_;
    position_ = other.position_;
    writable_ = other.writable_;
    flushFailed_ = other.flushFailed_;
    other.owner_ = nullptr;
    other.bytes_ = nullptr;
    other.position_ = 0;
    other.writable_ = false;
    other.flushFailed_ = false;
  }

  HalStorage* owner_ = nullptr;
  std::vector<uint8_t>* bytes_ = nullptr;
  size_t position_ = 0;
  bool writable_ = false;
  bool flushFailed_ = false;
};

class HalStorage {
 public:
  bool exists(const char* path) const {
    if (path == nullptr) return false;
    return files_.contains(path) || directories_.contains(path);
  }
  bool mkdir(const char* path, bool = true) {
    if (path == nullptr) return false;
    directories_.insert(path);
    return true;
  }
  bool remove(const char* path) {
    if (path == nullptr || shouldFail(TestStorageFault::Remove)) return false;
    return files_.erase(path) != 0;
  }
  FsFile open(const char* path, oflag_t flags = O_RDONLY) {
    if (path == nullptr) return {};
    const bool writable = (flags & O_WRONLY) != 0;
    auto it = files_.find(path);
    if (it == files_.end()) {
      if (!writable || (flags & O_CREAT) == 0) return {};
      it = files_.emplace(path, std::vector<uint8_t>{}).first;
    }
    if (writable && (flags & O_TRUNC) != 0) it->second.clear();
    ++openHandles_;
    maximumOpenHandles_ = std::max(maximumOpenHandles_, openHandles_);
    return FsFile(this, &it->second, writable);
  }
  bool openFileForRead(const char*, const std::string& path, FsFile& output) {
    return openFileForRead(nullptr, path.c_str(), output);
  }
  bool openFileForRead(const char*, const char* path, FsFile& output) {
    output = open(path, O_RDONLY);
    return static_cast<bool>(output);
  }
  bool openFileForWrite(const char*, const std::string& path, FsFile& output) {
    return openFileForWrite(nullptr, path.c_str(), output);
  }
  bool openFileForWrite(const char*, const char* path, FsFile& output) {
    output = open(path, O_WRONLY | O_CREAT | O_TRUNC);
    return static_cast<bool>(output);
  }
  std::vector<String> listFiles(const char* directory, int = 200) const {
    std::vector<String> result;
    if (directory == nullptr) return result;
    const std::string prefix = std::string(directory) + "/";
    for (const auto& [path, ignored] : files_) {
      (void)ignored;
      if (path.rfind(prefix, 0) != 0) continue;
      const std::string name = path.substr(prefix.size());
      if (name.find('/') == std::string::npos) result.push_back(name);
    }
    return result;
  }
  bool rename(const char* oldPath, const char* newPath) {
    if (oldPath == nullptr || newPath == nullptr || shouldFail(TestStorageFault::Rename)) return false;
    const auto it = files_.find(oldPath);
    if (it == files_.end()) return false;
    files_[newPath] = std::move(it->second);
    files_.erase(it);
    return true;
  }
  void reset() {
    files_.clear();
    directories_.clear();
    openHandles_ = 0;
    maximumOpenHandles_ = 0;
    clearFault();
  }
  void addFile(const std::string& path, const std::vector<uint8_t>& bytes) { files_[path] = bytes; }
  void fail(TestStorageFault fault, uint16_t occurrence = 1) {
    fault_ = fault;
    faultOccurrence_ = occurrence;
    faultCalls_ = 0;
  }
  void clearFault() {
    fault_ = TestStorageFault::None;
    faultOccurrence_ = 0;
    faultCalls_ = 0;
  }
  const std::map<std::string, std::vector<uint8_t>>& files() const { return files_; }
  size_t maximumOpenHandles() const { return maximumOpenHandles_; }
  static HalStorage& getInstance() {
    static HalStorage instance;
    return instance;
  }

 private:
  friend class FsFile;
  void handleClosed() {
    if (openHandles_ != 0) --openHandles_;
  }
  bool shouldFail(TestStorageFault fault) {
    if (fault_ != fault) return false;
    ++faultCalls_;
    return faultCalls_ == faultOccurrence_;
  }

  std::map<std::string, std::vector<uint8_t>> files_;
  std::set<std::string> directories_{"/"};
  size_t openHandles_ = 0;
  size_t maximumOpenHandles_ = 0;
  TestStorageFault fault_ = TestStorageFault::None;
  uint16_t faultOccurrence_ = 0;
  uint16_t faultCalls_ = 0;
};

inline int FsFile::read(void* destination, size_t count) {
  if (bytes_ == nullptr || destination == nullptr || owner_->shouldFail(TestStorageFault::Read)) return 0;
  const size_t readable = std::min(count, bytes_->size() - std::min(position_, bytes_->size()));
  if (readable != 0) {
    std::memcpy(destination, bytes_->data() + position_, readable);
    position_ += readable;
  }
  return static_cast<int>(readable);
}

inline int FsFile::read() {
  uint8_t value = 0;
  return read(&value, 1) == 1 ? value : -1;
}

inline size_t FsFile::write(const void* source, size_t count) {
  if (bytes_ == nullptr || !writable_ || (count != 0 && source == nullptr) ||
      owner_->shouldFail(TestStorageFault::Write)) {
    return 0;
  }
  if (position_ + count > bytes_->size()) bytes_->resize(position_ + count);
  if (count != 0) {
    std::memcpy(bytes_->data() + position_, source, count);
    position_ += count;
  }
  return count;
}

inline size_t FsFile::write(uint8_t value) { return write(&value, 1); }

inline void FsFile::flush() {
  if (owner_ != nullptr && owner_->shouldFail(TestStorageFault::Flush)) flushFailed_ = true;
}

inline bool FsFile::sync() {
  return bytes_ != nullptr && !flushFailed_ && !owner_->shouldFail(TestStorageFault::Sync);
}

inline bool FsFile::close() {
  const bool failed = owner_ != nullptr && owner_->shouldFail(TestStorageFault::Close);
  if (owner_ != nullptr) owner_->handleClosed();
  owner_ = nullptr;
  bytes_ = nullptr;
  position_ = 0;
  writable_ = false;
  flushFailed_ = false;
  return !failed;
}

#define Storage HalStorage::getInstance()
]=])

add_executable(PdfSavedItemsSessionTest
  ${REPO_ROOT}/lib/PdfReflow/PdfSavedItemsStore.cpp
  ${REPO_ROOT}/test/pdf_cache_recovery/PdfTestCacheIo.cpp
  ${REPO_ROOT}/src/activities/reader/PdfSavedItemsSession.cpp
  ${REPO_ROOT}/src/BookmarkStore.cpp
  ${REPO_ROOT}/src/ClippingStore.cpp
  PdfSavedItemsSessionTest.cpp
)

target_include_directories(PdfSavedItemsSessionTest PRIVATE
  ${PDF_SAVED_ITEMS_STUB_DIR}
  ${REPO_ROOT}/src
  ${REPO_ROOT}/src/activities/reader
  ${REPO_ROOT}/lib/Memory
  ${REPO_ROOT}/lib/PdfReflow
  ${REPO_ROOT}/lib/Serialization
  ${REPO_ROOT}/test/pdf_cache_recovery
)

target_link_libraries(PdfSavedItemsSessionTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
  # Serialization.h intentionally defines both stream and FsFile overloads.
  target_compile_options(PdfSavedItemsSessionTest PRIVATE -Wno-unused-function)
endif()

gtest_discover_tests(PdfSavedItemsSessionTest TEST_PREFIX "PdfSavedItemsSession")
