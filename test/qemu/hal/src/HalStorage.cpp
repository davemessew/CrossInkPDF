#define HAL_STORAGE_IMPL
#include "HalStorage.h"

#include <LittleFS.h>

#include <algorithm>
#include <climits>
#include <cstdio>
#include <cstring>
#include <utility>

#include "QemuHalControl.h"

HalStorage HalStorage::instance;

namespace {
constexpr uint64_t VIRTUAL_CAPACITY_BYTES = 64ULL * 1024ULL * 1024ULL;
constexpr uint64_t VIRTUAL_INITIAL_FREE_BYTES = 32ULL * 1024ULL * 1024ULL;
constexpr uint64_t LITTLEFS_CAPACITY_BYTES = 0x360000ULL;
constexpr int MAX_LISTED_FILES = 200;
constexpr uint8_t MAX_REMOVE_DIRECTORY_DEPTH = 8;
constexpr size_t MAX_STORAGE_PATH_BYTES = 192;

uint32_t qemuStorageOpenCount = 0;
uint32_t qemuStorageCloseCount = 0;
uint64_t qemuStorageQuota = UINT64_MAX;
uint64_t qemuVirtualBytesWritten = 0;

bool canWrite(size_t count) {
  if (static_cast<uint64_t>(count) > qemuStorageQuota) {
    return false;
  }
  const uint64_t used = LittleFS.usedBytes();
  return used <= LITTLEFS_CAPACITY_BYTES && static_cast<uint64_t>(count) <= LITTLEFS_CAPACITY_BYTES - used;
}

void accountWrite(size_t count) {
  if (qemuStorageQuota != UINT64_MAX) {
    qemuStorageQuota -= std::min<uint64_t>(qemuStorageQuota, count);
  }
  qemuVirtualBytesWritten =
      std::min<uint64_t>(VIRTUAL_INITIAL_FREE_BYTES, qemuVirtualBytesWritten + static_cast<uint64_t>(count));
}

const char* modeForFlags(const oflag_t flags, const bool exists) {
  if ((flags & O_RDWR) != 0) {
    if ((flags & (O_APPEND | O_AT_END)) != 0) {
      return "a+";
    }
    if ((flags & O_TRUNC) != 0 || ((flags & O_CREAT) != 0 && !exists)) {
      return "w+";
    }
    return "r+";
  }
  if ((flags & O_WRONLY) == 0) {
    return FILE_READ;
  }
  return (flags & (O_APPEND | O_AT_END)) != 0 ? FILE_APPEND : FILE_WRITE;
}

bool removeDirectoryTree(const char* path, const uint8_t depth) {
  if (path == nullptr || depth > MAX_REMOVE_DIRECTORY_DEPTH) {
    return false;
  }
  fs::File directory = LittleFS.open(path, FILE_READ);
  if (!directory || !directory.isDirectory()) {
    directory.close();
    return false;
  }
  bool success = true;
  fs::File entry = directory.openNextFile();
  while (entry && success) {
    char childPath[MAX_STORAGE_PATH_BYTES];
    const char* const entryPath = entry.path();
    const int written = std::snprintf(childPath, sizeof(childPath), "%s", entryPath == nullptr ? "" : entryPath);
    const bool directoryEntry = entry.isDirectory();
    entry.close();
    if (written <= 0 || static_cast<size_t>(written) >= sizeof(childPath)) {
      success = false;
      break;
    }
    success =
        directoryEntry ? removeDirectoryTree(childPath, static_cast<uint8_t>(depth + 1)) : LittleFS.remove(childPath);
    entry = directory.openNextFile();
  }
  entry.close();
  directory.close();
  return success && LittleFS.rmdir(path);
}
}  // namespace

HalStorage::HalStorage() = default;

bool HalStorage::begin() {
  initialized = LittleFS.begin(true, "/littlefs", 10, "spiffs");
  qemuStorageOpenCount = 0;
  qemuStorageCloseCount = 0;
  qemuStorageQuota = UINT64_MAX;
  qemuVirtualBytesWritten = 0;
  return initialized;
}

bool HalStorage::ready() const { return initialized; }

std::vector<String> HalStorage::listFiles(const char* path, int maxFiles) {
  std::vector<String> paths;
  const int limit = std::clamp(maxFiles, 0, MAX_LISTED_FILES);
  paths.reserve(static_cast<size_t>(limit));
  fs::File directory = LittleFS.open(path, FILE_READ);
  if (!directory || !directory.isDirectory()) {
    directory.close();
    return paths;
  }

  fs::File entry = directory.openNextFile();
  while (entry && static_cast<int>(paths.size()) < limit) {
    paths.emplace_back(entry.path());
    entry.close();
    entry = directory.openNextFile();
  }
  entry.close();
  directory.close();
  return paths;
}

String HalStorage::readFile(const char* path) {
  HalFile file = open(path, O_RDONLY);
  if (!file) {
    return {};
  }
  String content;
  content.reserve(file.size());
  while (file.available() > 0) {
    const int value = file.read();
    if (value < 0) {
      break;
    }
    content += static_cast<char>(value);
  }
  file.close();
  return content;
}

bool HalStorage::readFileToStream(const char* path, Print& out, size_t chunkSize) {
  HalFile file = open(path, O_RDONLY);
  if (!file) {
    return false;
  }
  uint8_t buffer[128];
  const size_t requestSize = std::clamp<size_t>(chunkSize, 1, sizeof(buffer));
  bool success = true;
  while (file.available() > 0) {
    const int count = file.read(buffer, requestSize);
    if (count <= 0 || out.write(buffer, static_cast<size_t>(count)) != static_cast<size_t>(count)) {
      success = false;
      break;
    }
  }
  file.close();
  return success;
}

size_t HalStorage::readFileToBuffer(const char* path, char* buffer, size_t bufferSize, size_t maxBytes) {
  if (buffer == nullptr || bufferSize == 0) {
    return 0;
  }
  HalFile file = open(path, O_RDONLY);
  if (!file) {
    buffer[0] = '\0';
    return 0;
  }
  size_t limit = bufferSize - 1;
  if (maxBytes != 0) {
    limit = std::min(limit, maxBytes);
  }
  const int count = file.read(buffer, limit);
  const size_t bytesRead = count > 0 ? static_cast<size_t>(count) : 0;
  buffer[bytesRead] = '\0';
  file.close();
  return bytesRead;
}

bool HalStorage::writeFile(const char* path, const String& content) {
  HalFile file = open(path, static_cast<oflag_t>(O_WRONLY | O_CREAT | O_TRUNC));
  if (!file) {
    return false;
  }
  const size_t expected = content.length();
  const bool success = file.write(content.c_str(), expected) == expected && file.sync();
  file.close();
  return success;
}

bool HalStorage::ensureDirectoryExists(const char* path) { return exists(path) || mkdir(path); }

void HalStorage::installDateTimeCallback(const uint8_t* utcOffsetQuarterHoursBiased) {
  (void)utcOffsetQuarterHoursBiased;
}

HalFile HalStorage::open(const char* path, const oflag_t oflag) {
  if (!initialized || path == nullptr) {
    return {};
  }
  fs::File opened = LittleFS.open(path, modeForFlags(oflag, LittleFS.exists(path)), (oflag & O_CREAT) != 0);
  if (opened && (oflag & O_AT_END) != 0) {
    opened.seek(static_cast<uint32_t>(opened.size()));
  }
  return HalFile(std::move(opened));
}

bool HalStorage::mkdir(const char* path, const bool pFlag) {
  (void)pFlag;
  return initialized && path != nullptr && LittleFS.mkdir(path);
}

bool HalStorage::exists(const char* path) { return initialized && path != nullptr && LittleFS.exists(path); }

bool HalStorage::remove(const char* path) { return initialized && path != nullptr && LittleFS.remove(path); }

bool HalStorage::rename(const char* oldPath, const char* newPath) {
  return initialized && oldPath != nullptr && newPath != nullptr && LittleFS.rename(oldPath, newPath);
}

bool HalStorage::rmdir(const char* path) { return initialized && path != nullptr && LittleFS.rmdir(path); }

bool HalStorage::openFileForRead(const char* moduleName, const char* path, HalFile& file) {
  (void)moduleName;
  file = open(path, O_RDONLY);
  return static_cast<bool>(file);
}

bool HalStorage::openFileForRead(const char* moduleName, const std::string& path, HalFile& file) {
  return openFileForRead(moduleName, path.c_str(), file);
}

bool HalStorage::openFileForRead(const char* moduleName, const String& path, HalFile& file) {
  return openFileForRead(moduleName, path.c_str(), file);
}

bool HalStorage::openFileForWrite(const char* moduleName, const char* path, HalFile& file) {
  (void)moduleName;
  file = open(path, static_cast<oflag_t>(O_WRONLY | O_CREAT | O_TRUNC));
  return static_cast<bool>(file);
}

bool HalStorage::openFileForWrite(const char* moduleName, const std::string& path, HalFile& file) {
  return openFileForWrite(moduleName, path.c_str(), file);
}

bool HalStorage::openFileForWrite(const char* moduleName, const String& path, HalFile& file) {
  return openFileForWrite(moduleName, path.c_str(), file);
}

bool HalStorage::removeDir(const char* path) { return initialized && path != nullptr && removeDirectoryTree(path, 0); }

HalStorageCapacityInfo HalStorage::capacityInfo() {
  return {{true, QemuHalControl::storageCapacity()}, {true, QemuHalControl::storageFree()}};
}

HalFile::HalFile() = default;

HalFile::HalFile(fs::File openedFile) : file(std::move(openedFile)), countedOpen(static_cast<bool>(file)) {
  if (countedOpen) {
    ++qemuStorageOpenCount;
  }
}

HalFile::~HalFile() { close(); }

HalFile::HalFile(HalFile&& other) : file(std::move(other.file)), countedOpen(other.countedOpen) {
  other.countedOpen = false;
}

HalFile& HalFile::operator=(HalFile&& other) {
  if (this != &other) {
    close();
    file = std::move(other.file);
    countedOpen = other.countedOpen;
    other.countedOpen = false;
  }
  return *this;
}

void HalFile::flush() {
  if (file) {
    file.flush();
  }
}

size_t HalFile::getName(char* name, size_t len) {
  if (!file || name == nullptr || len == 0) {
    return 0;
  }
  const char* const source = file.name();
  const int written = std::snprintf(name, len, "%s", source == nullptr ? "" : source);
  return written > 0 ? std::min(static_cast<size_t>(written), len - 1) : 0;
}

size_t HalFile::size() { return file ? file.size() : 0; }

size_t HalFile::fileSize() { return size(); }

uint64_t HalFile::fileSize64() { return static_cast<uint64_t>(size()); }

bool HalFile::seek(size_t pos) { return seek64(pos); }

bool HalFile::seek64(uint64_t pos) {
  return file && pos <= UINT32_MAX && file.seek(static_cast<uint32_t>(pos), fs::SeekSet);
}

bool HalFile::seekCur(int64_t offset) {
  if (!file) {
    return false;
  }
  const int64_t target = static_cast<int64_t>(file.position()) + offset;
  return target >= 0 && static_cast<uint64_t>(target) <= UINT32_MAX &&
         file.seek(static_cast<uint32_t>(target), fs::SeekSet);
}

bool HalFile::seekSet(size_t offset) { return seek64(offset); }

int HalFile::available() const { return file ? file.available() : 0; }

size_t HalFile::position() const { return file ? file.position() : 0; }

int HalFile::read(void* buf, size_t count) {
  if (!file || buf == nullptr) {
    return -1;
  }
  return static_cast<int>(file.read(static_cast<uint8_t*>(buf), count));
}

int HalFile::read() { return file ? file.read() : -1; }

size_t HalFile::write(const void* buf, size_t count) {
  if (!file || buf == nullptr || !canWrite(count)) {
    return 0;
  }
  const size_t written = file.write(static_cast<const uint8_t*>(buf), count);
  accountWrite(written);
  return written;
}

size_t HalFile::write(uint8_t b) { return write(&b, 1); }

bool HalFile::sync() {
  if (!file) {
    return false;
  }
  file.flush();
  return true;
}

bool HalFile::rename(const char* newPath) {
  if (!file || newPath == nullptr) {
    return false;
  }
  char oldPath[192];
  const char* const source = file.path();
  if (source == nullptr || std::snprintf(oldPath, sizeof(oldPath), "%s", source) < 0) {
    return false;
  }
  close();
  return LittleFS.rename(oldPath, newPath);
}

bool HalFile::isDirectory() const { return file && file.isDirectory(); }

void HalFile::rewindDirectory() {
  if (file) {
    file.rewindDirectory();
  }
}

bool HalFile::close() {
  const bool wasOpen = static_cast<bool>(file);
  if (wasOpen) {
    file.close();
  }
  if (countedOpen) {
    ++qemuStorageCloseCount;
    countedOpen = false;
  }
  return wasOpen;
}

HalFile HalFile::openNextFile() {
  if (!file || !file.isDirectory()) {
    return {};
  }
  return HalFile(file.openNextFile());
}

HalDirectoryNextStatus HalFile::openNextFile(HalFile& entry) {
  if (!file || !file.isDirectory() || &entry == this) {
    return HalDirectoryNextStatus::Error;
  }
  entry.close();
  entry.file = file.openNextFile();
  if (!entry.file) {
    return HalDirectoryNextStatus::End;
  }
  entry.countedOpen = true;
  ++qemuStorageOpenCount;
  return HalDirectoryNextStatus::Entry;
}

bool HalFile::isOpen() const { return static_cast<bool>(file); }

bool HalFile::modificationTime(uint64_t* const packedFatDateTime) {
  if (!file || packedFatDateTime == nullptr) {
    return false;
  }
  const time_t modified = file.getLastWrite();
  if (modified <= 0) {
    return false;
  }
  *packedFatDateTime = static_cast<uint64_t>(modified);
  return true;
}

HalFile::operator bool() const { return isOpen(); }

uint32_t QemuHalControl::storageOpenCount() { return qemuStorageOpenCount; }

uint32_t QemuHalControl::storageCloseCount() { return qemuStorageCloseCount; }

void QemuHalControl::setStorageQuota(uint64_t bytes) { qemuStorageQuota = bytes; }

uint64_t QemuHalControl::storageQuota() { return qemuStorageQuota; }

uint64_t QemuHalControl::storageCapacity() { return VIRTUAL_CAPACITY_BYTES; }

uint64_t QemuHalControl::storageFree() { return VIRTUAL_INITIAL_FREE_BYTES - qemuVirtualBytesWritten; }
