#pragma once

namespace BidiUtils {

inline int detectParagraphLevel(const char* const text, const int fallbackLevel = 0,
                                const int = 64) {
  if (text == nullptr) {
    return fallbackLevel & 1;
  }
  const auto* cursor = reinterpret_cast<const unsigned char*>(text);
  while (*cursor != 0) {
    if ((*cursor >= 'A' && *cursor <= 'Z') || (*cursor >= 'a' && *cursor <= 'z')) {
      return 0;
    }
    ++cursor;
  }
  return fallbackLevel & 1;
}

}  // namespace BidiUtils
