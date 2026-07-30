#pragma once

#include <cstddef>
#include <cstdint>

#include "PdfBuildCheckpoint.h"
#include "PdfCacheIo.h"
#include "PdfCacheManifest.h"

enum class PdfCacheSlot : uint8_t {
  A,
  B,
};

struct PdfCacheManifestSlotState {
  bool valid = false;
  bool sourceMatches = false;
  PdfCacheManifest manifest{};
};

struct PdfCacheManifestSelection {
  PdfCacheManifestSlotState slots[2]{};
  bool selected = false;
  PdfCacheSlot selectedSlot = PdfCacheSlot::A;
  PdfCacheManifest manifest{};
};

struct PdfBuildCheckpointSlotState {
  bool valid = false;
  bool sourceMatches = false;
  PdfBuildCheckpoint checkpoint{};
};

struct PdfBuildCheckpointSelection {
  PdfBuildCheckpointSlotState slots[2]{};
  bool selected = false;
  PdfCacheSlot selectedSlot = PdfCacheSlot::A;
  PdfBuildCheckpoint checkpoint{};
};

struct PdfCacheCommitEvidence {
  bool allWritersClosed = false;
  uint32_t requiredFileCount = 0;
  uint64_t requiredFileBytes = 0;
  uint64_t requiredFileLedger = PDF_CACHE_FNV64_OFFSET;
};

enum class PdfCacheFileKind : uint8_t {
  Required,
  Optional,
};

struct PdfCacheBudget {
  uint64_t hardLimit = 0;
  uint64_t limit = 0;
  uint64_t requiredReserve = 0;
  uint64_t requiredBytes = 0;
  uint64_t optionalBytes = 0;
  bool optionalOmitted = false;
};

struct PdfCacheTrackedWriter {
  PdfCacheIo io{};
  PdfCacheHandle handle{};
  PdfRequiredFileRecord record{};
  char fullPath[PDF_CACHE_PATH_CAPACITY]{};
  uint64_t byteLimit = 0;
  PdfCacheFileKind kind = PdfCacheFileKind::Required;
  bool open = false;
  bool failed = false;
};

PdfStatus pdfInitializeCacheBudget(uint64_t sourceSize, const PdfCacheCapacity& capacity, uint64_t requiredReserve,
                                   PdfCacheBudget* budget);
PdfStatus pdfReserveCacheBytes(PdfCacheBudget* budget, uint64_t bytes, PdfCacheFileKind kind);

PdfStatus pdfOpenTrackedCacheWriter(const PdfCacheIo& io, const char* fullPath, const char* relativePath,
                                    PdfCacheFileKind kind, uint64_t byteLimit, PdfCacheTrackedWriter* writer);
PdfStatus pdfWriteTrackedCacheFile(PdfCacheTrackedWriter* writer, const uint8_t* bytes, size_t length);
PdfStatus pdfCloseTrackedCacheFile(PdfCacheTrackedWriter* writer, PdfRequiredFileRecord* record);
void pdfAbortTrackedCacheFile(PdfCacheTrackedWriter* writer);

class PdfCacheStore {
 public:
  PdfStatus initialize(const PdfCacheIo& io, const char* cacheRoot);
  PdfStatus ensureGeneration(uint32_t generation);

  PdfStatus loadManifestSlots(const PdfSourceIdentity& expectedSource, PdfCacheManifestSelection* selection) const;
  PdfStatus commitManifest(const PdfCacheManifest& manifest, const PdfRequiredFileTableSource& files,
                           const PdfCacheCommitEvidence& evidence, const PdfCacheManifestSelection& prior,
                           PdfCacheManifestSelection* committed) const;

  PdfStatus loadCheckpointSlots(const PdfSourceIdentity& expectedSource, PdfBuildCheckpointSelection* selection) const;
  PdfStatus commitCheckpoint(const PdfBuildCheckpoint& checkpoint) const;

  PdfStatus cleanupUnreferencedGenerations() const;

 private:
  PdfStatus formatPath(const char* leaf, char* destination, size_t capacity) const;

  PdfCacheIo io_{};
  char root_[PDF_CACHE_PATH_CAPACITY]{};
};
