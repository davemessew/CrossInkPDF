#pragma once

#include <cctype>
#include <cstring>
#include <string_view>

namespace FsHelpers {

inline bool checkFileExtension(const std::string_view value, const char* suffix) {
  const size_t suffixLength = std::strlen(suffix);
  if (value.size() < suffixLength) return false;
  const size_t offset = value.size() - suffixLength;
  for (size_t index = 0; index < suffixLength; ++index) {
    const auto actual = static_cast<unsigned char>(value[offset + index]);
    const auto expected = static_cast<unsigned char>(suffix[index]);
    if (std::tolower(actual) != std::tolower(expected)) return false;
  }
  return true;
}

inline bool hasPdfExtension(const std::string_view path) { return checkFileExtension(path, ".pdf"); }
inline bool hasEpubExtension(const std::string_view path) { return checkFileExtension(path, ".epub"); }
inline bool hasXtcExtension(const std::string_view path) {
  return checkFileExtension(path, ".xtc") || checkFileExtension(path, ".xtch");
}
inline bool hasTxtExtension(const std::string_view path) { return checkFileExtension(path, ".txt"); }
inline bool hasMarkdownExtension(const std::string_view path) { return checkFileExtension(path, ".md"); }

}  // namespace FsHelpers
