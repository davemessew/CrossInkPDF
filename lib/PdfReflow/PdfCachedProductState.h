#pragma once

#include <cstddef>
#include <cstdint>

#include "PdfCacheIo.h"

namespace PdfCachedProductStateLimits {

inline constexpr size_t TitleBytes = 192;
inline constexpr size_t AuthorBytes = 128;
inline constexpr size_t ChapterBytes = 96;

}  // namespace PdfCachedProductStateLimits

enum class PdfCachedProductStateKind : uint8_t {
  Available,
  Missing,
  Stale,
  Corrupt,
  Error,
};

struct PdfCachedProductState {
  char title[PdfCachedProductStateLimits::TitleBytes]{};
  char author[PdfCachedProductStateLimits::AuthorBytes]{};
  char currentChapter[PdfCachedProductStateLimits::ChapterBytes]{};
  char coverPath[PDF_CACHE_PATH_CAPACITY]{};
  char thumbnailPath[PDF_CACHE_PATH_CAPACITY]{};
  uint32_t generation = 0;
  uint32_t totalWords = 0;
  uint32_t currentWord = 0;
  uint32_t currentSectionFirstWordOrdinal = 0;
  uint32_t currentSectionWordCount = 0;
  uint16_t currentSection = 0;
  bool hasProgress = false;
};

struct PdfCachedProductStateLoadResult {
  PdfCachedProductStateKind kind = PdfCachedProductStateKind::Error;
  PdfStatus status = PdfStatus::failure(PdfError::InvalidArgument);

  constexpr bool available() const { return kind == PdfCachedProductStateKind::Available && status.ok(); }
};

struct PdfCachedProductStateAllocator {
  using AllocateFn = void* (*)(void* context, size_t size);
  using ReleaseFn = void (*)(void* context, void* allocation);

  void* context = nullptr;
  AllocateFn allocate = nullptr;
  ReleaseFn release = nullptr;

  constexpr bool valid() const { return allocate != nullptr && release != nullptr; }
};

// Custom allocators must return storage aligned for any ordinary C++ object.
// PdfCachedProductState is intentionally fixed-capacity and larger than the
// firmware's small-stack budget. Store it in an owning activity/model object or
// another long-lived allocation rather than as a task-local temporary.
PdfCachedProductStateLoadResult pdfLoadCachedProductState(const PdfCacheIo& io, const char* sourcePath,
                                                          const char* cacheDirectory,
                                                          PdfCachedProductState* productState,
                                                          const uint64_t* cacheHashOverride = nullptr);
PdfCachedProductStateLoadResult pdfLoadCachedProductState(const PdfCacheIo& io, const char* sourcePath,
                                                           const char* cacheDirectory,
                                                           PdfCachedProductState* productState,
                                                           const PdfCachedProductStateAllocator& allocator,
                                                           const uint64_t* cacheHashOverride = nullptr);

#if defined(CROSSINK_ENABLE_PDF) && CROSSINK_ENABLE_PDF
// Sleep-only transient view: identifies the cache by source path/hash but never
// opens sourcePath. It selects the newest structurally valid completed
// manifest, then validates progress against that manifest's embedded source
// identity. Do not use this stale-tolerant view for Home or Reader state.
PdfCachedProductStateLoadResult pdfLoadCachedProductStateForSleep(
    const PdfCacheIo& io, const char* sourcePath, const char* cacheDirectory,
    PdfCachedProductState* productState, const uint64_t* cacheHashOverride = nullptr);
#endif
