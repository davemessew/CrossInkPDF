#define HAL_STORAGE_IMPL
#include "HalStorage.h"

#include <Arduino.h>
#include <FS.h>
#include <LittleFS.h>
#include <Logging.h>
#include <Memory.h>
#include <SdFat.h>

#include <algorithm>
#include <cassert>
#include <climits>
#include <cstdio>
#include <cstring>
#include <utility>

#include "QemuHalControl.h"
#include "QemuSdBlockDevice.h"

HalStorage HalStorage::instance;

namespace {
constexpr size_t kMaximumStoragePathBytes = 192;
constexpr uint32_t kSectorBytes = 512U;

QemuSdBlockDevice qemuSd;
FsVolume qemuVolume;
uint8_t qemuIoBuffer[kSectorBytes];
char qemuPathBufferA[kMaximumStoragePathBytes];
char qemuPathBufferB[kMaximumStoragePathBytes];
uint32_t qemuStorageOpenCount = 0;
uint32_t qemuStorageCloseCount = 0;
uint32_t qemuStorageReaderCount = 0;
uint32_t qemuStorageReaderPeak = 0;
uint64_t qemuStorageQuota = UINT64_MAX;

uint64_t cardCapacityBytes() { return static_cast<uint64_t>(qemuSd.sectorCount()) * kSectorBytes; }

uint64_t volumeFreeBytes() {
  const int32_t freeClusters = qemuVolume.freeClusterCount();
  return freeClusters < 0 ? 0 : static_cast<uint64_t>(freeClusters) * qemuVolume.bytesPerCluster();
}

bool canWrite(const size_t count) {
  return static_cast<uint64_t>(count) <= qemuStorageQuota;
}

void accountWrite(const size_t count) {
  if (qemuStorageQuota != UINT64_MAX) {
    qemuStorageQuota -= std::min<uint64_t>(qemuStorageQuota, count);
  }
}

bool copyLittleFsFixtures() {
  if (!LittleFS.begin(false, "/littlefs", 10, "spiffs")) {
    return false;
  }
  bool success = qemuVolume.exists("/qemu") || qemuVolume.mkdir("/qemu", true);
  fs::File directory = LittleFS.open("/qemu", FILE_READ);
  if (!directory || !directory.isDirectory()) {
    success = false;
  }

  fs::File entry = success ? directory.openNextFile() : fs::File{};
  while (entry && success) {
    const char* const path = entry.path();
    if (entry.isDirectory() || path == nullptr || std::strlen(path) >= kMaximumStoragePathBytes) {
      success = false;
      break;
    }
    FsFile output = qemuVolume.open(path, static_cast<oflag_t>(O_WRONLY | O_CREAT | O_TRUNC));
    if (!output) {
      success = false;
      break;
    }
    while (entry.available() > 0) {
      const int count = entry.read(qemuIoBuffer, sizeof(qemuIoBuffer));
      if (count <= 0 || output.write(qemuIoBuffer, static_cast<size_t>(count)) != static_cast<size_t>(count)) {
        success = false;
        break;
      }
    }
    success = success && output.sync();
    output.close();
    entry.close();
    entry = success ? directory.openNextFile() : fs::File{};
  }
  entry.close();
  directory.close();
  LittleFS.end();
  return success;
}

bool removeDirectoryTree(const char* const path) {
  if (path == nullptr) {
    return false;
  }
  FsFile directory = qemuVolume.open(path, O_RDONLY);
  if (!directory || !directory.isDirectory()) {
    directory.close();
    return false;
  }
  bool success = true;
  FsFile entry;
  while (success && entry.openNext(&directory)) {
    if (!entry.getName(qemuPathBufferA, sizeof(qemuPathBufferA))) {
      success = false;
      break;
    }
    const bool isDirectory = entry.isDirectory();
    entry.close();
    const int written = std::snprintf(qemuPathBufferB, sizeof(qemuPathBufferB), "%s%s%s", path,
                                      path[std::strlen(path) - 1] == '/' ? "" : "/", qemuPathBufferA);
    if (written <= 0 || static_cast<size_t>(written) >= sizeof(qemuPathBufferB)) {
      success = false;
      break;
    }
    success = isDirectory ? removeDirectoryTree(qemuPathBufferB)
                          : qemuVolume.remove(qemuPathBufferB);
  }
  entry.close();
  directory.close();
  return success && qemuVolume.rmdir(path);
}

void noteReaderOpen() {
  ++qemuStorageReaderCount;
  qemuStorageReaderPeak = std::max(qemuStorageReaderPeak, qemuStorageReaderCount);
}
}  // namespace

HalStorage::HalStorage() {
  storageMutex = xSemaphoreCreateMutex();
  assert(storageMutex != nullptr);
}

class HalStorage::StorageLock {
 public:
  StorageLock() { xSemaphoreTake(HalStorage::getInstance().storageMutex, portMAX_DELAY); }
  ~StorageLock() { xSemaphoreGive(HalStorage::getInstance().storageMutex); }
};

bool HalStorage::begin() {
  if (!qemuSd.begin()) {
    return false;
  }
  initialized = qemuVolume.begin(&qemuSd, true, 1);
  if (!initialized && qemuSd.formatAllowed()) {
    FsFormatter formatter;
    initialized = formatter.format(&qemuSd, qemuIoBuffer) && qemuVolume.begin(&qemuSd, true, 1);
  }
  if (!initialized || !copyLittleFsFixtures()) {
    initialized = false;
    return false;
  }

  qemuStorageOpenCount = 0;
  qemuStorageCloseCount = 0;
  qemuStorageReaderCount = 0;
  qemuStorageReaderPeak = 0;
  qemuStorageQuota = UINT64_MAX;
  return true;
}

bool HalStorage::ready() const { return initialized; }

uint64_t HalStorage::totalBytes() const { return cardCapacityBytes(); }

uint64_t HalStorage::usedBytes() {
  const HalStorageCapacityInfo capacity = capacityInfo();
  if (!capacity.total.known || !capacity.free.known || capacity.free.value > capacity.total.value) {
    return 0;
  }
  return capacity.total.value - capacity.free.value;
}

std::vector<String> HalStorage::listFiles(const char* const path, const int maxFiles) {
  std::vector<String> paths;
  const int limit = std::max(maxFiles, 0);
  paths.reserve(static_cast<size_t>(limit));
  HalFile directory = open(path, O_RDONLY);
  if (!directory || !directory.isDirectory()) {
    return paths;
  }
  HalFile entry;
  while (static_cast<int>(paths.size()) < limit && directory.openNextFile(entry) == HalDirectoryNextStatus::Entry) {
    char name[kMaximumStoragePathBytes];
    if (entry.getName(name, sizeof(name)) == 0) {
      break;
    }
    paths.emplace_back(name);
  }
  entry.close();
  directory.close();
  return paths;
}

String HalStorage::readFile(const char* const path) {
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

bool HalStorage::readFileToStream(const char* const path, Print& out, const size_t chunkSize) {
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

size_t HalStorage::readFileToBuffer(const char* const path, char* const buffer, const size_t bufferSize,
                                    const size_t maxBytes) {
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

bool HalStorage::writeFile(const char* const path, const String& content) {
  HalFile file = open(path, static_cast<oflag_t>(O_WRONLY | O_CREAT | O_TRUNC));
  if (!file) {
    return false;
  }
  const size_t expected = content.length();
  const bool success = file.write(content.c_str(), expected) == expected && file.sync();
  file.close();
  return success;
}

bool HalStorage::ensureDirectoryExists(const char* const path) { return exists(path) || mkdir(path); }

void HalStorage::installDateTimeCallback(const uint8_t* const utcOffsetQuarterHoursBiased) {
  (void)utcOffsetQuarterHoursBiased;
}

class HalFile::Impl {
 public:
  Impl() = default;
  Impl(FsFile&& opened, const bool isReader, const bool isWritable)
      : file(std::move(opened)), reader(isReader), writable(isWritable) {}

  FsFile file;
  bool reader = false;
  bool writable = false;
};

HalFile::HalFile() = default;
HalFile::HalFile(std::unique_ptr<Impl> opened) : impl(std::move(opened)) {}
HalFile::~HalFile() { close(); }
HalFile::HalFile(HalFile&&) = default;

HalFile& HalFile::operator=(HalFile&& other) {
  if (this != &other) {
    close();
    impl = std::move(other.impl);
    allocationFailed_ = other.allocationFailed_;
    other.allocationFailed_ = false;
  }
  return *this;
}

HalFile HalStorage::open(const char* const path, const oflag_t oflag) {
  if (!initialized || path == nullptr) {
    return {};
  }
  StorageLock lock;
  FsFile opened = qemuVolume.open(path, oflag);
  if (!opened) {
    return {};
  }
  const bool writable = (oflag & (O_WRONLY | O_RDWR)) != 0;
  const bool reader = !opened.isDirectory() && (oflag & O_WRONLY) == 0;
  if (reader && qemuStorageReaderCount != 0) {
    opened.close();
    return {};
  }
  auto wrapped = makeUniqueNoThrow<HalFile::Impl>(std::move(opened), reader, writable);
  if (!wrapped) {
    opened.close();
    HalFile failed;
    failed.allocationFailed_ = true;
    return failed;
  }
  ++qemuStorageOpenCount;
  if (reader) {
    noteReaderOpen();
  }
  return HalFile(std::move(wrapped));
}

bool HalStorage::mkdir(const char* const path, const bool pFlag) {
  StorageLock lock;
  return initialized && path != nullptr && qemuVolume.mkdir(path, pFlag);
}

bool HalStorage::exists(const char* const path) {
  StorageLock lock;
  return initialized && path != nullptr && qemuVolume.exists(path);
}

bool HalStorage::remove(const char* const path) {
  StorageLock lock;
  return initialized && path != nullptr && qemuVolume.remove(path);
}

bool HalStorage::rename(const char* const oldPath, const char* const newPath) {
  StorageLock lock;
  return initialized && oldPath != nullptr && newPath != nullptr && qemuVolume.rename(oldPath, newPath);
}

bool HalStorage::rmdir(const char* const path) {
  StorageLock lock;
  return initialized && path != nullptr && qemuVolume.rmdir(path);
}

bool HalStorage::openFileForRead(const char* const moduleName, const char* const path, HalFile& file) {
  (void)moduleName;
  file = open(path, O_RDONLY);
  return static_cast<bool>(file);
}

bool HalStorage::openFileForRead(const char* const moduleName, const std::string& path, HalFile& file) {
  return openFileForRead(moduleName, path.c_str(), file);
}

bool HalStorage::openFileForRead(const char* const moduleName, const String& path, HalFile& file) {
  return openFileForRead(moduleName, path.c_str(), file);
}

bool HalStorage::openFileForWrite(const char* const moduleName, const char* const path, HalFile& file) {
  (void)moduleName;
  file = open(path, static_cast<oflag_t>(O_WRONLY | O_CREAT | O_TRUNC));
  return static_cast<bool>(file);
}

bool HalStorage::openFileForWrite(const char* const moduleName, const std::string& path, HalFile& file) {
  return openFileForWrite(moduleName, path.c_str(), file);
}

bool HalStorage::openFileForWrite(const char* const moduleName, const String& path, HalFile& file) {
  return openFileForWrite(moduleName, path.c_str(), file);
}

bool HalStorage::removeDir(const char* const path) {
  StorageLock lock;
  return initialized && path != nullptr && path[0] != '\0' && removeDirectoryTree(path);
}

HalStorageCapacityInfo HalStorage::capacityInfo() {
  StorageLock lock;
  const uint64_t capacity = cardCapacityBytes();
  const uint64_t free = initialized ? volumeFreeBytes() : 0;
  return {{capacity != 0, capacity}, {initialized, free}};
}

#define HAL_FILE_WRAPPED_CALL(method, ...) \
  HalStorage::StorageLock lock;            \
  assert(impl != nullptr);                 \
  return impl->file.method(__VA_ARGS__)

void HalFile::flush() {
  if (impl != nullptr) {
    HalStorage::StorageLock lock;
    impl->file.flush();
  }
}

size_t HalFile::getName(char* const name, const size_t len) { HAL_FILE_WRAPPED_CALL(getName, name, len); }
size_t HalFile::size() { return impl != nullptr ? impl->file.size() : 0; }
size_t HalFile::fileSize() { return impl != nullptr ? impl->file.fileSize() : 0; }
uint64_t HalFile::fileSize64() { return impl != nullptr ? impl->file.fileSize() : 0; }
bool HalFile::seek(const size_t pos) { HAL_FILE_WRAPPED_CALL(seekSet, pos); }
bool HalFile::seek64(const uint64_t pos) { HAL_FILE_WRAPPED_CALL(seekSet, pos); }
bool HalFile::truncate64(const uint64_t length) { HAL_FILE_WRAPPED_CALL(truncate, length); }
bool HalFile::seekCur(const int64_t offset) { HAL_FILE_WRAPPED_CALL(seekCur, offset); }
bool HalFile::seekSet(const size_t offset) { HAL_FILE_WRAPPED_CALL(seekSet, offset); }
int HalFile::available() const { HAL_FILE_WRAPPED_CALL(available, ); }
size_t HalFile::position() const { HAL_FILE_WRAPPED_CALL(position, ); }
int HalFile::read(void* const buf, const size_t count) { HAL_FILE_WRAPPED_CALL(read, buf, count); }
int HalFile::read() { HAL_FILE_WRAPPED_CALL(read, ); }

size_t HalFile::write(const void* const buf, const size_t count) {
  if (impl == nullptr || buf == nullptr || !canWrite(count)) {
    return 0;
  }
  HalStorage::StorageLock lock;
  const size_t written = impl->file.write(buf, count);
  accountWrite(written);
  return written;
}

size_t HalFile::write(const uint8_t value) { return write(&value, 1); }
bool HalFile::sync() { HAL_FILE_WRAPPED_CALL(sync, ); }
bool HalFile::rename(const char* const newPath) { HAL_FILE_WRAPPED_CALL(rename, newPath); }
bool HalFile::isDirectory() const { return impl != nullptr && impl->file.isDirectory(); }
void HalFile::rewindDirectory() {
  if (impl != nullptr) {
    HalStorage::StorageLock lock;
    impl->file.rewindDirectory();
  }
}

bool HalFile::close() {
  if (impl == nullptr) {
    allocationFailed_ = false;
    return true;
  }
  HalStorage::StorageLock lock;
  const bool reader = impl->reader;
  const bool result = impl->file.close();
  impl.reset();
  allocationFailed_ = false;
  ++qemuStorageCloseCount;
  if (reader && qemuStorageReaderCount != 0) {
    --qemuStorageReaderCount;
  }
  return result;
}

HalFile HalFile::openNextFile() {
  allocationFailed_ = false;
  if (impl == nullptr || !impl->file.isDirectory()) {
    return {};
  }
  HalStorage::StorageLock lock;
  FsFile child;
  if (!child.openNext(&impl->file)) {
    return {};
  }
  const bool reader = !child.isDirectory();
  if (reader && qemuStorageReaderCount != 0) {
    child.close();
    return {};
  }
  auto wrapped = makeUniqueNoThrow<Impl>(std::move(child), reader, false);
  if (!wrapped) {
    child.close();
    allocationFailed_ = true;
    return {};
  }
  ++qemuStorageOpenCount;
  if (reader) {
    noteReaderOpen();
  }
  return HalFile(std::move(wrapped));
}

HalDirectoryNextStatus HalFile::openNextFile(HalFile& entry) {
  if (impl == nullptr || !impl->file.isDirectory() || &entry == this) {
    return HalDirectoryNextStatus::Error;
  }
  entry.close();
  HalStorage::StorageLock lock;
  FsFile child;
  if (!child.openNext(&impl->file)) {
    return impl->file.getError() == 0 ? HalDirectoryNextStatus::End : HalDirectoryNextStatus::Error;
  }
  const bool reader = !child.isDirectory();
  if (reader && qemuStorageReaderCount != 0) {
    child.close();
    return HalDirectoryNextStatus::Error;
  }
  auto wrapped = makeUniqueNoThrow<Impl>(std::move(child), reader, false);
  if (!wrapped) {
    child.close();
    return HalDirectoryNextStatus::Error;
  }
  ++qemuStorageOpenCount;
  if (reader) {
    noteReaderOpen();
  }
  entry.impl = std::move(wrapped);
  return HalDirectoryNextStatus::Entry;
}

bool HalFile::isOpen() const { return impl != nullptr && impl->file.isOpen(); }

bool HalFile::modificationTime(uint64_t* const packedFatDateTime) {
  if (impl == nullptr || packedFatDateTime == nullptr) {
    return false;
  }
  uint16_t date = 0;
  uint16_t time = 0;
  HalStorage::StorageLock lock;
  if (!impl->file.getModifyDateTime(&date, &time)) {
    return false;
  }
  *packedFatDateTime = static_cast<uint64_t>(date) << 16U | time;
  return true;
}

HalFile::operator bool() const { return isOpen(); }

uint32_t QemuHalControl::storageOpenCount() { return qemuStorageOpenCount; }
uint32_t QemuHalControl::storageCloseCount() { return qemuStorageCloseCount; }
uint32_t QemuHalControl::storageReaderCount() { return qemuStorageReaderCount; }
uint32_t QemuHalControl::storageReaderPeak() { return qemuStorageReaderPeak; }
void QemuHalControl::setStorageQuota(const uint64_t bytes) { qemuStorageQuota = bytes; }
uint64_t QemuHalControl::storageQuota() { return qemuStorageQuota; }
uint64_t QemuHalControl::storageCapacity() { return cardCapacityBytes(); }
uint64_t QemuHalControl::storageFree() { return volumeFreeBytes(); }
