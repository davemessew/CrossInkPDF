#pragma once

#include <cstdint>
#include <string>

#include "BookRouteSpy.h"
#include "HalStorage.h"

class Txt {
 public:
  Txt(std::string path, const char*) : path_(std::move(path)) {}

  bool load() {
    ++BOOK_ROUTE_SPY.txtLoads;
    return BOOK_ROUTE_SPY.txtLoadResult;
  }

  uint32_t getFileSize() const { return 1234; }
  std::string getCachePath() const { return "/txt"; }
  std::string getCoverBmpPath() const { return "/txt/cover.bmp"; }

  bool generateCoverBmp() const {
    ++BOOK_ROUTE_SPY.txtCoverGenerations;
    Storage.addFile(getCoverBmpPath(), {1});
    return true;
  }

 private:
  std::string path_;
};
