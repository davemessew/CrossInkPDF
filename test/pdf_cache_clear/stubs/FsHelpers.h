#pragma once

#include <cctype>
#include <cstring>
#include <string_view>

namespace FsHelpers {

inline bool checkFileExtension(const std::string_view fileName, const char* const extension) {
  const size_t length = std::strlen(extension);
  if (fileName.size() < length) {
    return false;
  }
  const size_t offset = fileName.size() - length;
  for (size_t index = 0; index < length; ++index) {
    if (std::tolower(static_cast<unsigned char>(fileName[offset + index])) !=
        std::tolower(static_cast<unsigned char>(extension[index]))) {
      return false;
    }
  }
  return true;
}

inline bool hasEpubExtension(const std::string_view path) { return checkFileExtension(path, ".epub"); }
inline bool hasPdfExtension(const std::string_view path) { return checkFileExtension(path, ".pdf"); }
inline bool hasTxtExtension(const std::string_view path) { return checkFileExtension(path, ".txt"); }
inline bool hasXtcExtension(const std::string_view path) {
  return checkFileExtension(path, ".xtc") || checkFileExtension(path, ".xtch");
}

}  // namespace FsHelpers
