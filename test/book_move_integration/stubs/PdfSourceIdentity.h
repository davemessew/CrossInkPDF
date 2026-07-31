#pragma once

#include <cstddef>
#include <cstdint>

inline uint64_t pdfPathHash64(const char* bytes, size_t length) {
  uint64_t hash = 14695981039346656037ULL;
  for (size_t index = 0; index < length; ++index) {
    hash ^= static_cast<uint8_t>(bytes[index]);
    hash *= 1099511628211ULL;
  }
  return hash;
}
