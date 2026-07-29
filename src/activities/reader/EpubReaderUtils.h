#pragma once

#include <Epub.h>

namespace EpubReaderUtils {

struct Progress {
  int spineIndex = 0;
  int pageNumber = 0;
  int pageCount = 0;
  bool hasPageCount = false;
};

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
