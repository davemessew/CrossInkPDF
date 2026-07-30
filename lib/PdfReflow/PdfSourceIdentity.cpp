#include "PdfSourceIdentity.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace {

constexpr uint8_t kHeadDomain[] = {'C', 'R', 'O', 'S', 'S', 'I', 'N', 'K', '-',
                                   'P', 'D', 'F', '-', 'H', 'E', 'A', 'D', 0};
constexpr uint8_t kTailDomain[] = {'C', 'R', 'O', 'S', 'S', 'I', 'N', 'K', '-',
                                   'P', 'D', 'F', '-', 'T', 'A', 'I', 'L', 0};

uint64_t updateLittleEndian64(uint64_t hash, const uint64_t value) {
  uint8_t bytes[8];
  for (uint8_t index = 0; index < sizeof(bytes); ++index) {
    bytes[index] = static_cast<uint8_t>(value >> (index * 8U));
  }
  return pdfCacheFnv64(bytes, sizeof(bytes), hash);
}

uint64_t fingerprint(const uint8_t* const domain, const size_t domainLength, const uint64_t sourceSize,
                     const uint64_t offset, const size_t length, const uint8_t* const bytes) {
  uint64_t hash = pdfCacheFnv64(domain, domainLength);
  hash = updateLittleEndian64(hash, sourceSize);
  hash = updateLittleEndian64(hash, offset);
  hash = updateLittleEndian64(hash, length);
  return pdfCacheFnv64(bytes, length, hash);
}

PdfStatus readExact(const PdfCacheIo& io, const PdfCacheHandle handle, const uint64_t offset, uint8_t* destination,
                    const size_t length) {
  size_t bytesRead = 0;
  const PdfStatus status = io.read(io.context, handle, offset, destination, length, &bytesRead);
  if (!status) {
    return status;
  }
  if (bytesRead != length) {
    return PdfStatus::failure(PdfError::UnexpectedEof, offset + bytesRead);
  }
  return PdfStatus::success();
}

PdfStatus closePreservingFailure(const PdfCacheIo& io, PdfCacheHandle* handle, const PdfStatus prior) {
  const PdfStatus closeStatus = io.close(io.context, handle);
  return prior ? closeStatus : prior;
}

}  // namespace

bool pdfSourceIdentityEqual(const PdfSourceIdentity& left, const PdfSourceIdentity& right) {
  if (left.size != right.size || left.headFingerprint != right.headFingerprint ||
      left.tailFingerprint != right.tailFingerprint || left.modificationTime.known != right.modificationTime.known) {
    return false;
  }
  return !left.modificationTime.known || left.modificationTime.value == right.modificationTime.value;
}

uint64_t pdfPathHash64(const char* const path, const size_t length) {
  return path == nullptr && length != 0 ? 0 : pdfCacheFnv64(path, length);
}

uint64_t pdfFingerprintSourceWindow(const PdfSourceFingerprintWindow window, const uint64_t sourceSize,
                                    const uint64_t offset, const uint8_t* const bytes, const size_t length) {
  if (bytes == nullptr && length != 0) {
    return 0;
  }
  const uint8_t* const domain = window == PdfSourceFingerprintWindow::Head ? kHeadDomain : kTailDomain;
  const size_t domainLength = window == PdfSourceFingerprintWindow::Head ? sizeof(kHeadDomain) : sizeof(kTailDomain);
  return fingerprint(domain, domainLength, sourceSize, offset, length, bytes);
}

PdfStatus pdfFormatCacheRoot(const char* const cacheDirectory, const char* const sourcePath, char* const destination,
                             const size_t destinationCapacity) {
  if (cacheDirectory == nullptr || sourcePath == nullptr || destination == nullptr || destinationCapacity == 0) {
    return PdfStatus::failure(PdfError::InvalidArgument);
  }
  const size_t sourceLength = std::strlen(sourcePath);
  const int written = std::snprintf(destination, destinationCapacity, "%s/pdf_%llu", cacheDirectory,
                                    static_cast<unsigned long long>(pdfPathHash64(sourcePath, sourceLength)));
  if (written < 0 || static_cast<size_t>(written) >= destinationCapacity) {
    destination[0] = '\0';
    return PdfStatus::failure(PdfError::LimitExceeded);
  }
  return PdfStatus::success();
}

PdfStatus pdfComputeSourceIdentity(const PdfCacheIo& io, const char* const sourcePath, uint8_t* const workspace,
                                   const size_t workspaceSize, PdfSourceIdentity* const identity) {
  if (!io.valid() || sourcePath == nullptr || workspace == nullptr || workspaceSize < PDF_SOURCE_FINGERPRINT_BYTES ||
      identity == nullptr) {
    return PdfStatus::failure(PdfError::InvalidArgument);
  }
  *identity = {};
  PdfCacheHandle handle{};
  PdfStatus status = io.open(io.context, sourcePath, PdfCacheOpenMode::Read, &handle);
  if (!status) {
    return status;
  }

  PdfCacheFileMetadata metadata{};
  status = io.metadata(io.context, handle, &metadata);
  if (!status || metadata.directory || metadata.symlinkLike) {
    if (status && (metadata.directory || metadata.symlinkLike)) {
      status = PdfStatus::failure(PdfError::InvalidArgument);
    }
    return closePreservingFailure(io, &handle, status);
  }

  identity->size = metadata.size;
  identity->modificationTime = metadata.modificationTime;
  const size_t headLength = static_cast<size_t>(std::min<uint64_t>(metadata.size, PDF_SOURCE_FINGERPRINT_BYTES));
  if (headLength != 0) {
    status = readExact(io, handle, 0, workspace, headLength);
    if (!status) {
      return closePreservingFailure(io, &handle, status);
    }
  }
  identity->headFingerprint =
      pdfFingerprintSourceWindow(PdfSourceFingerprintWindow::Head, metadata.size, 0, workspace, headLength);

  const uint64_t tailOffset =
      metadata.size > PDF_SOURCE_FINGERPRINT_BYTES ? metadata.size - PDF_SOURCE_FINGERPRINT_BYTES : 0;
  const size_t tailLength = static_cast<size_t>(std::min<uint64_t>(metadata.size, PDF_SOURCE_FINGERPRINT_BYTES));
  if (metadata.size > PDF_SOURCE_FINGERPRINT_BYTES) {
    status = readExact(io, handle, tailOffset, workspace, tailLength);
    if (!status) {
      return closePreservingFailure(io, &handle, status);
    }
  }
  identity->tailFingerprint =
      pdfFingerprintSourceWindow(PdfSourceFingerprintWindow::Tail, metadata.size, tailOffset, workspace, tailLength);
  return closePreservingFailure(io, &handle, PdfStatus::success());
}
