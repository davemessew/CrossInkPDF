#include "LegacyPageProgressStore.h"

#include <utility>

namespace {
constexpr char kProgressFileName[] = "/progress.bin";
constexpr char kTempSuffix[] = ".tmp";
constexpr char kBackupSuffix[] = ".bak";
constexpr size_t kLegacyTupleSize = 4;
constexpr size_t kCurrentTupleSize = 6;
constexpr int kMaximumTupleValue = 0xFFFF;

uint16_t readLe16(const uint8_t* data) {
  return static_cast<uint16_t>(static_cast<uint16_t>(data[0]) | (static_cast<uint16_t>(data[1]) << 8));
}

void writeLe16(uint8_t* data, const int value) {
  const auto encoded = static_cast<uint16_t>(value);
  data[0] = static_cast<uint8_t>(encoded & 0xFFu);
  data[1] = static_cast<uint8_t>(encoded >> 8);
}

bool isTupleValue(const int value) { return value >= 0 && value <= kMaximumTupleValue; }
}  // namespace

bool LegacyPageProgressStore::load(ReflowReadingPosition& position) const {
  if (!io_.isValid()) {
    return false;
  }

  const std::string progressPath = std::string(cachePath_) + kProgressFileName;
  if (readPosition(progressPath, position)) {
    return true;
  }
  return readPosition(progressPath + kBackupSuffix, position);
}

bool LegacyPageProgressStore::save(const ReflowReadingPosition& position) const {
  if (!io_.isValid() || !isTupleValue(position.sectionIndex) || !isTupleValue(position.pageNumber) ||
      !isTupleValue(position.pageCount)) {
    return false;
  }

  uint8_t data[kCurrentTupleSize];
  writeLe16(data, position.sectionIndex);
  writeLe16(data + 2, position.pageNumber);
  writeLe16(data + 4, position.pageCount);

  const std::string progressPath = std::string(cachePath_) + kProgressFileName;
  const std::string tempPath = progressPath + kTempSuffix;
  const std::string backupPath = progressPath + kBackupSuffix;

  if (io_.exists(io_.context, tempPath.c_str()) && !io_.remove(io_.context, tempPath.c_str())) {
    return false;
  }

  if (!io_.writeSynced(io_.context, tempPath.c_str(), data, sizeof(data))) {
    if (io_.exists(io_.context, tempPath.c_str())) {
      io_.remove(io_.context, tempPath.c_str());
    }
    return false;
  }

  if (io_.exists(io_.context, backupPath.c_str()) && !io_.remove(io_.context, backupPath.c_str())) {
    io_.remove(io_.context, tempPath.c_str());
    return false;
  }

  const bool hadProgress = io_.exists(io_.context, progressPath.c_str());
  if (hadProgress && !io_.rename(io_.context, progressPath.c_str(), backupPath.c_str())) {
    io_.remove(io_.context, tempPath.c_str());
    return false;
  }

  if (io_.rename(io_.context, tempPath.c_str(), progressPath.c_str())) {
    return true;
  }

  if (hadProgress && io_.exists(io_.context, backupPath.c_str()) && !io_.exists(io_.context, progressPath.c_str())) {
    io_.rename(io_.context, backupPath.c_str(), progressPath.c_str());
  }
  if (io_.exists(io_.context, tempPath.c_str())) {
    io_.remove(io_.context, tempPath.c_str());
  }
  return false;
}

bool LegacyPageProgressStore::readPosition(const std::string& path, ReflowReadingPosition& position) const {
  if (!io_.exists(io_.context, path.c_str())) {
    return false;
  }

  uint8_t data[kCurrentTupleSize] = {};
  size_t fileSize = 0;
  if (!io_.read(io_.context, path.c_str(), data, sizeof(data), &fileSize) ||
      (fileSize != kLegacyTupleSize && fileSize != kCurrentTupleSize)) {
    return false;
  }

  ReflowReadingPosition decoded;
  decoded.sectionIndex = static_cast<int>(readLe16(data));
  decoded.pageNumber = static_cast<int>(readLe16(data + 2));
  if (decoded.pageNumber == kMaximumTupleValue) {
    decoded.pageNumber = 0;
  }
  if (fileSize == kCurrentTupleSize) {
    decoded.pageCount = static_cast<int>(readLe16(data + 4));
    decoded.hasPageCount = true;
  }
  position = std::move(decoded);
  return true;
}
