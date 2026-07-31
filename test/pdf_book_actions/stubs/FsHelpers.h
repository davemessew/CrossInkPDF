#pragma once

#include <string>

namespace FsHelpers {

inline bool endsWith(const std::string& value, const char* suffix) {
  const std::string ending(suffix);
  return value.size() >= ending.size() && value.compare(value.size() - ending.size(), ending.size(), ending) == 0;
}

inline bool hasPdfExtension(const std::string& path) { return endsWith(path, ".pdf") || endsWith(path, ".PDF"); }
inline bool hasEpubExtension(const std::string& path) { return endsWith(path, ".epub"); }
inline bool hasXtcExtension(const std::string& path) { return endsWith(path, ".xtc"); }
inline bool hasTxtExtension(const std::string& path) { return endsWith(path, ".txt"); }
inline bool hasMarkdownExtension(const std::string& path) { return endsWith(path, ".md"); }

}  // namespace FsHelpers
