#include "QemuSdBlockDevice.h"

#include <riscv/semihosting.h>

#include <climits>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace {
constexpr char kImagePath[] = "qemu_sd.img";
constexpr char kFormatMarkerPath[] = "qemu_sd_format.ok";
constexpr uint32_t kSectorBytes = 512U;
constexpr long kSysOpen = 0x01;
constexpr long kSysClose = 0x02;
constexpr long kSysWrite = 0x05;
constexpr long kSysRead = 0x06;
constexpr long kSysSeek = 0x0A;
constexpr long kSysFileLength = 0x0C;
constexpr long kOpenReadBinary = 1;
constexpr long kOpenReadWriteBinary = 3;
}  // namespace

int QemuSemihost::openRead(const char* const path) {
  if (path == nullptr) {
    return -1;
  }
  long args[] = {reinterpret_cast<long>(path), kOpenReadBinary, static_cast<long>(std::strlen(path))};
  return static_cast<int>(semihosting_call_noerrno(kSysOpen, args));
}

int QemuSemihost::openReadWrite(const char* const path) {
  if (path == nullptr) {
    return -1;
  }
  long args[] = {reinterpret_cast<long>(path), kOpenReadWriteBinary, static_cast<long>(std::strlen(path))};
  return static_cast<int>(semihosting_call_noerrno(kSysOpen, args));
}

bool QemuSemihost::close(const int handle) {
  long args[] = {handle};
  return handle >= 0 && semihosting_call_noerrno(kSysClose, args) == 0;
}

int64_t QemuSemihost::length(const int handle) {
  long args[] = {handle};
  return handle < 0 ? -1 : semihosting_call_noerrno(kSysFileLength, args);
}

size_t QemuSemihost::read(const int handle, void* const destination, const size_t bytes) {
  if (handle < 0 || destination == nullptr || bytes == 0) {
    return 0;
  }
  long args[] = {handle, reinterpret_cast<long>(destination), static_cast<long>(bytes)};
  const long remaining = semihosting_call_noerrno(kSysRead, args);
  return remaining < 0 || static_cast<size_t>(remaining) > bytes ? 0 : bytes - static_cast<size_t>(remaining);
}

size_t QemuSemihost::write(const int handle, const void* const source, const size_t bytes) {
  if (handle < 0 || source == nullptr || bytes == 0) {
    return 0;
  }
  long args[] = {handle, reinterpret_cast<long>(source), static_cast<long>(bytes)};
  const long remaining = semihosting_call_noerrno(kSysWrite, args);
  return remaining < 0 || static_cast<size_t>(remaining) > bytes ? 0 : bytes - static_cast<size_t>(remaining);
}

bool QemuSemihost::seek(const int handle, const uint64_t offset) {
  if (handle < 0 || offset > LONG_MAX) {
    return false;
  }
  long args[] = {handle, static_cast<long>(offset)};
  return semihosting_call_noerrno(kSysSeek, args) == 0;
}

bool QemuSdBlockDevice::begin() {
  end();
  image_ = QemuSemihost::openReadWrite(kImagePath);
  const int64_t imageBytes = QemuSemihost::length(image_);
  if (image_ < 0 || imageBytes < static_cast<int64_t>(kSectorBytes) || imageBytes % kSectorBytes != 0) {
    end();
    return false;
  }
  const uint64_t sectors = static_cast<uint64_t>(imageBytes) / kSectorBytes;
  if (sectors == 0 || sectors > static_cast<uint64_t>(UINT32_MAX)) {
    end();
    return false;
  }
  sectorCount_ = static_cast<Sector_t>(sectors);
  return true;
}

void QemuSdBlockDevice::end() {
  if (image_ >= 0) {
    QemuSemihost::close(image_);
    image_ = -1;
  }
  sectorCount_ = 0;
}

bool QemuSdBlockDevice::transfer(const Sector_t sector, uint8_t* const data, const size_t count, const bool write) {
  if (image_ < 0 || data == nullptr || count == 0 || sector >= sectorCount_ ||
      count > static_cast<size_t>(sectorCount_ - sector)) {
    return false;
  }
  const uint64_t byteOffset = static_cast<uint64_t>(sector) * kSectorBytes;
  const uint64_t byteCount = static_cast<uint64_t>(count) * kSectorBytes;
  if (byteCount > SIZE_MAX || !QemuSemihost::seek(image_, byteOffset)) {
    return false;
  }
  const size_t requested = static_cast<size_t>(byteCount);
  return write ? QemuSemihost::write(image_, data, requested) == requested
               : QemuSemihost::read(image_, data, requested) == requested;
}

bool QemuSdBlockDevice::readSector(const Sector_t sector, uint8_t* const dst) {
  return readSectors(sector, dst, 1);
}

bool QemuSdBlockDevice::readSectors(const Sector_t sector, uint8_t* const dst, const size_t count) {
  return transfer(sector, dst, count, false);
}

bool QemuSdBlockDevice::writeSector(const Sector_t sector, const uint8_t* const src) {
  return writeSectors(sector, src, 1);
}

bool QemuSdBlockDevice::writeSectors(const Sector_t sector, const uint8_t* const src, const size_t count) {
  return transfer(sector, const_cast<uint8_t*>(src), count, true);
}

bool QemuSdBlockDevice::syncDevice() { return image_ >= 0; }

bool QemuSdBlockDevice::formatAllowed() const {
  const int marker = QemuSemihost::openRead(kFormatMarkerPath);
  return marker >= 0 && QemuSemihost::close(marker);
}
