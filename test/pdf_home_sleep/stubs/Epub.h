#pragma once

#include <string>

#include "BookRouteSpy.h"
#include "GfxRenderer.h"
#include "HalStorage.h"

class Epub {
 public:
  Epub(std::string path, const char*) : path_(std::move(path)) {}

  bool load(bool = false, bool = false) {
    ++BOOK_ROUTE_SPY.epubLoads;
    return BOOK_ROUTE_SPY.epubLoadResult;
  }

  float calculateProgress(int, float) const { return BOOK_ROUTE_SPY.epubCalculatedProgress; }

  bool generateCoverBmp(bool cropped = false, const GfxRenderer* = nullptr, int = 0) const {
    ++BOOK_ROUTE_SPY.epubCoverGenerations;
    Storage.addFile(getCoverBmpPath(cropped), {1});
    return true;
  }

  bool generateAdaptiveThumbBmp(int width, int height, const GfxRenderer* = nullptr, int = 0) const {
    ++BOOK_ROUTE_SPY.epubThumbGenerations;
    Storage.addFile(getAdaptiveThumbBmpPath(width, height), {1});
    return true;
  }

  std::string getCoverBmpPath(bool cropped = false) const {
    return std::string("/epub/") + (cropped ? "cover_cropped.bmp" : "cover.bmp");
  }

  std::string getAdaptiveThumbBmpPath(const int width, const int height) const {
    return "/epub/thumb_" + std::to_string(width) + "x" + std::to_string(height) + ".bmp";
  }

  std::string getThumbBmpPath() const { return "/epub/thumb_[WIDTH]x[HEIGHT].bmp"; }

 private:
  std::string path_;
};
