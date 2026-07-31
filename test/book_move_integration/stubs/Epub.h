#pragma once

#include <ZipFile.h>

#include <string>

class Epub {
 public:
  static std::string cachePathForFilePath(const std::string& path, const std::string& cacheRoot) {
    return cacheRoot + "/epub_" + std::to_string(ZipFile::fnvHash64(path.c_str(), path.size()));
  }
};
