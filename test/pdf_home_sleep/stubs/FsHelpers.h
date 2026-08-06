#pragma once

#include <algorithm>
#include <cctype>
#include <string>
#include <string_view>

namespace FsHelpers {

inline bool hasExtension(const std::string_view path, const std::string_view suffix) {
  if (path.size() < suffix.size()) {
    return false;
  }
  const auto tail = path.substr(path.size() - suffix.size());
  return std::equal(tail.begin(), tail.end(), suffix.begin(), suffix.end(), [](const char left, const char right) {
    return std::tolower(static_cast<unsigned char>(left)) == std::tolower(static_cast<unsigned char>(right));
  });
}

inline bool hasEpubExtension(const std::string_view path) { return hasExtension(path, ".epub"); }
inline bool hasPdfExtension(const std::string_view path) { return hasExtension(path, ".pdf"); }
inline bool hasXtcExtension(const std::string_view path) { return hasExtension(path, ".xtc"); }
inline bool hasTxtExtension(const std::string_view path) { return hasExtension(path, ".txt"); }
inline bool hasMarkdownExtension(const std::string_view path) {
  return hasExtension(path, ".md") || hasExtension(path, ".markdown");
}

}  // namespace FsHelpers
