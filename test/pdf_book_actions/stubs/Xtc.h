#pragma once

#include <string>

#include "TestState.h"

class Xtc {
 public:
  Xtc(const std::string&, const char*) { ++TEST_STATE.xtcConstructs; }

  bool load() {
    ++TEST_STATE.xtcLoads;
    return TEST_STATE.xtcLoadResult;
  }
  void setupCacheDir() { ++TEST_STATE.xtcSetups; }
  std::string getCachePath() const { return "/.crosspoint/xtc-cache"; }
  std::string getTitle() const { return "XTC title"; }
  std::string getAuthor() const { return "XTC author"; }
  std::string getThumbBmpPath() const { return "/.crosspoint/xtc-cache/thumb.bmp"; }
};
