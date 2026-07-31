#pragma once

#include <cstdint>
#include <string>

#include "BookRouteSpy.h"
#include "HalStorage.h"

class Xtc {
 public:
  Xtc(std::string path, const char*) : path_(std::move(path)) {}

  bool load() {
    ++BOOK_ROUTE_SPY.xtcLoads;
    return BOOK_ROUTE_SPY.xtcLoadResult;
  }

  float calculateProgress(uint32_t) const { return BOOK_ROUTE_SPY.xtcCalculatedProgress; }
  std::string getCachePath() const { return "/xtc"; }
  std::string getCoverBmpPath() const { return "/xtc/cover.bmp"; }
  std::string getThumbBmpPath() const { return "/xtc/thumb_[HEIGHT].bmp"; }

  bool generateCoverBmp() const {
    ++BOOK_ROUTE_SPY.xtcCoverGenerations;
    Storage.addFile(getCoverBmpPath(), {1});
    return true;
  }

  bool generateThumbBmp(uint16_t) const {
    ++BOOK_ROUTE_SPY.xtcThumbGenerations;
    return true;
  }

  bool generateThumbBmp(uint16_t, uint16_t) const {
    ++BOOK_ROUTE_SPY.xtcThumbGenerations;
    return true;
  }

 private:
  std::string path_;
};
