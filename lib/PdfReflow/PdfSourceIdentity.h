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

enum class PdfSourceFingerprintWindow : uint8_t {
  Head,
  Tail,
};

bool pdfSourceIdentityEqual(const PdfSourceIdentity& left, const PdfSourceIdentity& right);
uint64_t pdfPathHash64(const char* path, size_t length);
uint64_t pdfFingerprintSourceWindow(PdfSourceFingerprintWindow window, uint64_t sourceSize, uint64_t offset,
                                    const uint8_t* bytes, size_t length);
PdfStatus pdfFormatCacheRootForHash(const char* cacheDirectory, uint64_t cacheHash, char* destination,
                                    size_t destinationCapacity);
PdfStatus pdfFormatCacheRoot(const char* cacheDirectory, const char* sourcePath, char* destination,
                             size_t destinationCapacity);
PdfStatus pdfComputeSourceIdentity(const PdfCacheIo& io, const char* sourcePath, uint8_t* workspace,
                                   size_t workspaceSize, PdfSourceIdentity* identity);
