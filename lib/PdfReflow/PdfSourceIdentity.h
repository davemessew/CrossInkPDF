#pragma once

#include <cstddef>
#include <cstdint>

#include "PdfCacheFormat.h"
#include "PdfCacheIo.h"

constexpr size_t PDF_SOURCE_FINGERPRINT_BYTES = 4096;

struct PdfSourceIdentity {
  uint64_t size = 0;
  PdfOptionalU64 modificationTime{};
  uint64_t headFingerprint = 0;
  uint64_t tailFingerprint = 0;
};

bool pdfSourceIdentityEqual(const PdfSourceIdentity& left, const PdfSourceIdentity& right);
uint64_t pdfPathHash64(const char* path, size_t length);
PdfStatus pdfFormatCacheRoot(const char* cacheDirectory, const char* sourcePath, char* destination,
                             size_t destinationCapacity);
PdfStatus pdfComputeSourceIdentity(const PdfCacheIo& io, const char* sourcePath, uint8_t* workspace,
                                   size_t workspaceSize, PdfSourceIdentity* identity);
