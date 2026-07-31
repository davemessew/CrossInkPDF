#pragma once

#include <string>

#include "TestState.h"

class Epub {
 public:
  Epub(const std::string&, const char*) { ++TEST_STATE.epubConstructs; }

  void clearCache() { TEST_STATE.metadataDeletes.emplace_back("epub-cache"); }
  void setupCacheDir() { ++TEST_STATE.epubSetups; }
  std::string getCachePath() const { return "/.crosspoint/epub-cache"; }
  std::string getTitle() const { return "EPUB title"; }
  std::string getAuthor() const { return "EPUB author"; }
  std::string getThumbBmpPath() const { return "/.crosspoint/epub-cache/thumb.bmp"; }
};
