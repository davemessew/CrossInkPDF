#pragma once

#include <Epub.h>
#include <HalStorage.h>
#include <Logging.h>

#include <cstdint>
#include <string>

namespace EpubReaderUtils {

struct Progress {
  int spineIndex = 0;
  int pageNumber = 0;
  int pageCount = 0;
  bool hasPageCount = false;
};

inline bool readProgressFile(const char* moduleName, const std::string& path, Progress& progress) {
  if (!Storage.exists(path.c_str())) return false;

  FsFile file;
  if (!Storage.openFileForRead(moduleName, path, file)) return false;

  uint8_t data[6];
  const int dataSize = file.read(data, sizeof(data));
  file.close();
  if (dataSize != 4 && dataSize != 6) {
    LOG_ERR(moduleName, "Progress file has unexpected size: %d", dataSize);
    return false;
  }

  progress.spineIndex = static_cast<int>(static_cast<uint16_t>(data[0]) | (static_cast<uint16_t>(data[1]) << 8));
  progress.pageNumber = static_cast<int>(static_cast<uint16_t>(data[2]) | (static_cast<uint16_t>(data[3]) << 8));
  if (progress.pageNumber == UINT16_MAX) progress.pageNumber = 0;
  progress.hasPageCount = dataSize == 6;
  progress.pageCount = progress.hasPageCount
                           ? static_cast<int>(static_cast<uint16_t>(data[4]) | (static_cast<uint16_t>(data[5]) << 8))
                           : 0;
  return true;
}

inline bool loadProgress(const Epub& epub, Progress& progress, const char* moduleName = "ERS") {
  (void)moduleName;
  ReflowReadingPosition position;
  if (!epub.loadReadingPosition(position)) {
    return false;
  }
  progress.spineIndex = position.sectionIndex;
  progress.pageNumber = position.pageNumber;
  progress.pageCount = position.pageCount;
  progress.hasPageCount = position.hasPageCount;
  return true;
}

// Persists reader progress for an EPUB to its cache directory. Returns true on success.
inline bool saveProgress(Epub& epub, int spineIndex, int pageNumber, int pageCount) {
  ReflowReadingPosition position;
  position.sectionIndex = spineIndex;
  position.pageNumber = pageNumber;
  position.pageCount = pageCount;
  position.hasPageCount = true;
  return epub.saveReadingPosition(position);
}

}  // namespace EpubReaderUtils
