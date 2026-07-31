#pragma once

#include <algorithm>
#include <cctype>
#include <string>
#include <string_view>

namespace FsHelpers {
inline bool hasExtension(const std::string_view path, const char* extension) {
  if (path.size() < std::char_traits<char>::length(extension)) return false;
  std::string suffix(path.substr(path.size() - std::char_traits<char>::length(extension)));
  std::transform(suffix.begin(), suffix.end(), suffix.begin(),
                 [](unsigned char value) { return static_cast<char>(std::tolower(value)); });
  return suffix == extension;
}
inline bool hasEpubExtension(const std::string_view path) { return hasExtension(path, ".epub"); }
inline bool hasPdfExtension(const std::string_view path) { return hasExtension(path, ".pdf"); }
inline bool hasXtcExtension(const std::string_view path) { return hasExtension(path, ".xtc"); }
inline bool hasTxtExtension(const std::string_view path) { return hasExtension(path, ".txt"); }
inline bool hasMarkdownExtension(const std::string_view path) { return hasExtension(path, ".md"); }
}  // namespace FsHelpers
