#pragma once

#include <algorithm>
#include <cctype>
#include <string>
#include <string_view>

namespace FsHelpers {
inline bool hasExtension(const std::string_view value, const std::string_view extension) {
  if (value.size() < extension.size()) return false;
  return std::equal(extension.rbegin(), extension.rend(), value.rbegin(),
                    [](const char a, const char b) { return std::tolower(a) == std::tolower(b); });
}
inline bool hasCssExtension(const std::string_view value) { return hasExtension(value, ".css"); }
inline bool hasGifExtension(const std::string_view value) { return hasExtension(value, ".gif"); }
inline bool hasJpgExtension(const std::string_view value) {
  return hasExtension(value, ".jpg") || hasExtension(value, ".jpeg");
}
inline bool hasPngExtension(const std::string_view value) { return hasExtension(value, ".png"); }
inline std::string normalisePath(const std::string_view value, bool = false) { return std::string(value); }
inline std::string decodeUriEscapes(const std::string_view value) { return std::string(value); }
}  // namespace FsHelpers
