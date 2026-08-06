#pragma once

#include "BookRouteSpy.h"
#include "Epub.h"

namespace EpubReaderUtils {

struct Progress {
  int spineIndex = 1;
  int pageNumber = 1;
  int pageCount = 4;
  bool hasPageCount = true;
};

inline bool loadProgress(Epub&, Progress&, const char*) {
  ++BOOK_ROUTE_SPY.epubProgressLoads;
  return BOOK_ROUTE_SPY.epubProgressResult;
}

}  // namespace EpubReaderUtils
