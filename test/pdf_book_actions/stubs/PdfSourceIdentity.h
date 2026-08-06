#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>

#include "TestState.h"

constexpr size_t PDF_CACHE_PATH_CAPACITY = 128;

struct PdfStatus {
  bool success = true;
  constexpr explicit operator bool() const { return success; }
  constexpr bool ok() const { return success; }
};

inline uint64_t pdfPathHash64(const char* path, const size_t length) {
  uint64_t hash = 1469598103934665603ULL;
  for (size_t index = 0; index < length; ++index) {
    hash ^= static_cast<uint8_t>(path[index]);
    hash *= 1099511628211ULL;
  }
  return hash;
}

inline PdfStatus pdfFormatCacheRootForHash(const char*, const uint64_t cacheHash, char* const destination,
                                           const size_t capacity) {
  ++TEST_STATE.cacheFormats;
  if (destination == nullptr || capacity == 0) return {false};
  const int written =
      std::snprintf(destination, capacity, "/.crosspoint/pdf_%016llx", static_cast<unsigned long long>(cacheHash));
  return {written > 0 && static_cast<size_t>(written) < capacity};
}
