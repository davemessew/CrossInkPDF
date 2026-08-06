#pragma once

#include <cstddef>
#include <cstdint>

#include "PdfCacheStore.h"
#include "PdfLimits.h"
#include "PdfWorkBudget.h"

constexpr uint8_t PDF_IMAGE_CACHE_MAX_ENTRIES = 64;

enum class PdfImageCacheEntryKind : uint8_t {
  None,
  Jpeg,
  Raster,
};

struct PdfImageCacheEntry {
  uint64_t contentHash = 0;
  uint64_t sourceBytes = 0;
  uint32_t sourceCrc32 = 0;
  uint8_t fileOrdinal = UINT8_MAX;
  PdfImageCacheEntryKind kind = PdfImageCacheEntryKind::None;
};

static_assert(sizeof(PdfImageCacheEntry) <= 24, "image identity entries must remain compact");

struct PdfCachedImage {
  char fullPath[PDF_CACHE_PATH_CAPACITY]{};
  PdfRequiredFileRecord record{};
  uint64_t contentHash = 0;
  uint16_t width = 0;
  uint16_t height = 0;
  bool reused = false;
};

struct PdfCapturedJpeg {
  uint64_t contentHash = PDF_CACHE_FNV64_OFFSET;
  uint64_t sourceBytes = 0;
  uint32_t sourceCrc32 = 0;
  uint8_t temporaryOrdinal = UINT8_MAX;
};

struct PdfImageCacheConfig {
  PdfCacheIo io{};
  const char* cacheRoot = nullptr;
  uint32_t generation = 0;
  PdfCacheBudget* budget = nullptr;
  // Reuses the preparation-time 4 KiB decoder workspace. No image-sized
  // allocation is made by this module.
  uint8_t* ioWorkspace = nullptr;
  size_t ioWorkspaceBytes = 0;
  PdfImageCacheEntry* entries = nullptr;
  uint8_t entryCapacity = 0;
  PdfCacheRenameFn rename = nullptr;
};

enum class PdfImageCacheJpegStage : uint8_t {
  Idle,
  Capture,
  Copy,
  Close,
  Dedupe,
  Publish,
};

struct PdfImageCacheRuntime {
  PdfByteSource source{};
  PdfCachedImage* result = nullptr;
  uint64_t hash = PDF_CACHE_FNV64_OFFSET;
  uint64_t bytes = 0;
  uint64_t offset = 0;
  size_t pendingBytes = 0;
  uint32_t crc32 = 0;
  uint16_t width = 0;
  uint16_t height = 0;
  uint8_t dedupeIndex = 0;
  uint8_t temporaryOrdinal = UINT8_MAX;
  bool capturedSource = false;
  PdfImageCacheJpegStage stage = PdfImageCacheJpegStage::Idle;
};

static_assert(sizeof(PdfImageCacheRuntime) <= 96, "resumable JPEG state must remain a small fixed workspace");

class PdfImageCache {
 public:
  PdfStatus begin(const PdfImageCacheConfig& config);
  PdfStatus beginJpeg(const PdfByteSource& encoded, uint16_t width, uint16_t height, PdfCachedImage* result,
                      PdfImageCacheRuntime* runtime);
  PdfStatus beginJpegCapture(uint8_t temporaryOrdinal, uint64_t byteLimit, PdfImageCacheRuntime* runtime);
  PdfStatus appendJpegCapture(const uint8_t* bytes, size_t length, PdfImageCacheRuntime& runtime);
  PdfStatus finishJpegCapture(PdfImageCacheRuntime& runtime, PdfCapturedJpeg* captured);
  PdfStatus beginCapturedJpeg(const PdfCapturedJpeg& captured, uint16_t width, uint16_t height,
                              PdfCachedImage* result, PdfImageCacheRuntime* runtime);
  PdfStatus discardCapturedJpeg(uint8_t temporaryOrdinal);
  PdfStepResult stepJpeg(PdfImageCacheRuntime& runtime, PdfWorkBudget& budget);
  void abortJpeg(PdfImageCacheRuntime& runtime);
  PdfStepResult stepRasterIdentity(uint64_t contentHash, uint32_t sourceCrc32, uint64_t sourceBytes, uint8_t* scanIndex,
                                   uint8_t* identityIndex, PdfWorkBudget& budget);
  PdfStatus bindRasterRecord(uint8_t identityIndex, uint8_t recordIndex, uint8_t* canonicalRecordIndex);

  uint8_t entryCount() const { return entryCount_; }
  const PdfImageCacheEntry* entries() const { return config_.entries; }

 private:
  PdfStatus formatTemporaryPaths(char* fullPath, size_t fullCapacity, char* relativePath,
                                 size_t relativeCapacity) const;
  PdfStatus formatCapturedTemporaryPaths(uint8_t temporaryOrdinal, char* fullPath, size_t fullCapacity,
                                         char* relativePath, size_t relativeCapacity) const;
  PdfStatus formatContentPaths(uint64_t contentHash, uint32_t sourceCrc32, uint64_t sourceBytes, char* fullPath,
                               size_t fullCapacity, char* relativePath, size_t relativeCapacity) const;
  PdfStatus failJpeg(PdfStatus status, PdfImageCacheRuntime& runtime);
  void finishJpeg(PdfImageCacheRuntime& runtime);

  PdfImageCacheConfig config_{};
  char cacheRoot_[PDF_CACHE_PATH_CAPACITY]{};
  char relativePath_[PDF_CACHE_REQUIRED_PATH_CAPACITY]{};
  PdfCacheTrackedWriter writer_{};
  uint8_t entryCount_ = 0;
  bool initialized_ = false;
};

static_assert(sizeof(PdfImageCache) <= 768, "image cache coordinator must not retain image-sized buffers");
