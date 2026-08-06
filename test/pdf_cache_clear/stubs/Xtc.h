#pragma once

#include <string>

#include "TestStorage.h"

class Xtc {
 public:
  Xtc(const std::string& path, const char*) : cachePath_("/.crosspoint/xtc_stub_" + path) {}
  const std::string& getCachePath() const { return cachePath_; }
  bool clearCache() { return !Storage.exists(cachePath_.c_str()) || Storage.removeDir(cachePath_.c_str()); }

 private:
  std::string cachePath_;
};
