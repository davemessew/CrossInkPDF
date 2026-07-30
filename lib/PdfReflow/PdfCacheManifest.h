#pragma once

#include <cstddef>
#include <cstdint>

#include "PdfCacheFormat.h"
#include "PdfIo.h"
#include "PdfSourceIdentity.h"

constexpr size_t PDF_CACHE_REQUIRED_PATH_CAPACITY = 96;

struct PdfRequiredFileRecord {
  char path[PDF_CACHE_REQUIRED_PATH_CAPACITY]{};
  uint8_t pathLength = 0;
  uint64_t size = 0;
  uint32_t crc32 = 0;
};

struct PdfRequiredFileTableSource {
  using ReadFn = PdfStatus (*)(void* context, uint32_t index, PdfRequiredFileRecord* record);

  void* context = nullptr;
  uint32_t count = 0;
  ReadFn read = nullptr;

  constexpr bool valid() const { return count == 0 || read != nullptr; }
};

struct PdfRequiredFileTableVisitor {
  using AcceptFn = PdfStatus (*)(void* context, const PdfRequiredFileRecord& record);

  void* context = nullptr;
  AcceptFn accept = nullptr;

  constexpr bool valid() const { return accept != nullptr; }
};

struct PdfCacheManifest {
  uint16_t formatVersion = PDF_CACHE_FORMAT_VERSION;
  uint16_t capabilityVersion = PDF_CACHE_CAPABILITY_VERSION;
  uint32_t sequence = 0;
  bool completed = false;
  uint32_t warningFlags = 0;
  PdfSourceIdentity source{};
  uint32_t generation = 0;
  uint32_t totalWords = 0;
  uint32_t requiredFileCount = 0;
  uint64_t requiredFileBytes = 0;
  uint64_t requiredFileLedger = PDF_CACHE_FNV64_OFFSET;
};

bool pdfValidateCacheRelativePath(const char* path, size_t length);
uint64_t pdfUpdateRequiredFileLedger(uint64_t ledger, const PdfRequiredFileRecord& record);
PdfStatus pdfEncodeCacheManifest(const PdfCacheManifest& manifest, const PdfRequiredFileTableSource& files,
                                 const PdfByteSink& destination);
PdfStatus pdfDecodeCacheManifest(const PdfByteSource& source, PdfCacheManifest* manifest,
                                 const PdfRequiredFileTableVisitor& visitor);
