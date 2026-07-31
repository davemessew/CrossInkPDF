#pragma once

#include <cstdint>
#include <cstring>

#include "PdfSourceIdentity.h"

enum class PdfCachedProductStateKind : uint8_t {
  Available,
  Missing,
  Stale,
  Corrupt,
  Error,
};

struct PdfCachedProductState {
  char title[192]{};
  char author[128]{};
  char currentChapter[96]{};
  char coverPath[PDF_CACHE_PATH_CAPACITY]{};
  char thumbnailPath[PDF_CACHE_PATH_CAPACITY]{};
};

struct PdfCachedProductStateLoadResult {
  PdfCachedProductStateKind kind = PdfCachedProductStateKind::Error;
  PdfStatus status{false};
  constexpr bool available() const { return kind == PdfCachedProductStateKind::Available && status.ok(); }
};

struct PdfCacheIo {
  void* context = nullptr;
};

inline void copyProductValue(char* const destination, const size_t capacity, const std::string& value) {
  const size_t count = value.size() < capacity - 1U ? value.size() : capacity - 1U;
  std::memcpy(destination, value.data(), count);
  destination[count] = '\0';
}

inline PdfCachedProductStateLoadResult pdfLoadCachedProductState(const PdfCacheIo&, const char*, const char*,
                                                                 PdfCachedProductState* const state,
                                                                 const uint64_t* const cacheHashOverride = nullptr) {
  ++TEST_STATE.productLoads;
  ++TEST_STATE.sourceIdentityPasses;
  TEST_STATE.productHashOverrideSupplied = cacheHashOverride != nullptr;
  TEST_STATE.productHashOverride = cacheHashOverride == nullptr ? 0 : *cacheHashOverride;
  if (state == nullptr) return {};
  *state = {};
  copyProductValue(state->title, sizeof(state->title), TEST_STATE.productTitle);
  copyProductValue(state->author, sizeof(state->author), TEST_STATE.productAuthor);
  copyProductValue(state->thumbnailPath, sizeof(state->thumbnailPath), TEST_STATE.productThumbnail);
  const auto kind = static_cast<PdfCachedProductStateKind>(TEST_STATE.productKind);
  return {kind, {kind == PdfCachedProductStateKind::Available}};
}
