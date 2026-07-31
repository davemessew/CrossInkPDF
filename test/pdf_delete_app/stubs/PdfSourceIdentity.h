#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>

struct PdfStatus {
  bool ok = false;
  explicit operator bool() const { return ok; }
};

inline uint64_t pdfPathHash64(const char* bytes, const size_t length) {
  uint64_t hash = 14695981039346656037ULL;
  for (size_t index = 0; index < length; ++index) {
    hash ^= static_cast<uint8_t>(bytes[index]);
    hash *= 1099511628211ULL;
  }
  return hash;
}

inline PdfStatus pdfFormatCacheRootForHash(const char* directory, const uint64_t hash, char* destination,
                                           const size_t capacity) {
  const int written =
      std::snprintf(destination, capacity, "%s/pdf_%llu", directory, static_cast<unsigned long long>(hash));
  return {written > 0 && static_cast<size_t>(written) < capacity};
}
