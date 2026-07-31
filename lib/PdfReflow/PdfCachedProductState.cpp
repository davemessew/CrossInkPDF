#include "PdfCachedProductState.h"

#include <cstdio>
#include <cstring>
#include <limits>
#include <new>

#include "PdfCacheFormat.h"
#include "PdfCacheManifest.h"
#include "PdfMetadataStore.h"
#include "PdfOutline.h"
#include "PdfSourceIdentity.h"
#include "PdfUnicode.h"

namespace {

#if defined(__GNUC__)
#define PDF_CACHED_PRODUCT_NOINLINE __attribute__((noinline))
#elif defined(_MSC_VER)
#define PDF_CACHED_PRODUCT_NOINLINE __declspec(noinline)
#else
#define PDF_CACHED_PRODUCT_NOINLINE
#endif

constexpr char kManifestNames[2][11] = {"manifest.a", "manifest.b"};
constexpr char kProgressNames[2][11] = {"progress.a", "progress.b"};
constexpr size_t kArtifactCount = 4;
constexpr size_t kCoverIndex = 0;
constexpr size_t kThumbnailIndex = 1;
constexpr size_t kMetadataIndex = 2;
constexpr size_t kOutlineIndex = 3;
constexpr uint8_t kAllArtifacts = (1U << kArtifactCount) - 1U;
constexpr uint32_t kCoverWidth = 240;
constexpr uint32_t kCoverHeight = 400;
constexpr uint32_t kThumbnailWidth = 96;
constexpr uint32_t kThumbnailHeight = 160;
constexpr size_t kBmpHeaderBytes = 62;
constexpr size_t kMetadataHeaderBytes = 24;
constexpr size_t kMetadataSectionBytes = 24;
constexpr size_t kOutlineHeaderBytes = 16;
constexpr size_t kOutlineRecordBytes = 128;
constexpr size_t kProgressRecordBytes = 96;
constexpr size_t kProgressCrcOffset = 92;
constexpr uint32_t kProgressHasPageCount = 1U;
constexpr uint32_t kProgressHasSemanticPosition = 2U;
constexpr uint32_t kProgressHasWordCursor = 4U;
constexpr uint32_t kProgressKnownFlags = kProgressHasPageCount | kProgressHasSemanticPosition | kProgressHasWordCursor;
constexpr uint32_t kProgressModificationTimeKnown = 1U << 31U;

static_assert(PdfCachedProductStateLimits::TitleBytes == PdfMetadataLimits::TitleBytes);
static_assert(PdfCachedProductStateLimits::AuthorBytes == PdfMetadataLimits::AuthorBytes);
static_assert(PdfCachedProductStateLimits::ChapterBytes == PdfOutlineLimits::TitleBytes);
static_assert(kOutlineRecordBytes == PdfOutlineLimits::EncodedRecordBytes);

uint16_t getU16(const uint8_t* const source) {
  return static_cast<uint16_t>(source[0]) | static_cast<uint16_t>(source[1]) << 8U;
}

uint32_t getU32(const uint8_t* const source) {
  return static_cast<uint32_t>(source[0]) | static_cast<uint32_t>(source[1]) << 8U |
         static_cast<uint32_t>(source[2]) << 16U | static_cast<uint32_t>(source[3]) << 24U;
}

uint64_t getU64(const uint8_t* const source) {
  uint64_t value = 0;
  for (uint8_t index = 0; index < 8; ++index) {
    value |= static_cast<uint64_t>(source[index]) << (index * 8U);
  }
  return value;
}

bool equalRecordPath(const PdfRequiredFileRecord& record, const char* const expected) {
  const size_t length = std::strlen(expected);
  return length == record.pathLength && std::memcmp(record.path, expected, length) == 0;
}

bool isLowerHex(const char value) { return (value >= '0' && value <= '9') || (value >= 'a' && value <= 'f'); }

bool matchesSectionPath(const PdfRequiredFileRecord& record, const char* const prefix, const uint16_t expectedIndex) {
  constexpr char suffix[] = ".xhtml";
  constexpr size_t digits = 6;
  const size_t prefixLength = std::strlen(prefix);
  if (record.pathLength != prefixLength + digits + sizeof(suffix) - 1U ||
      std::memcmp(record.path, prefix, prefixLength) != 0) {
    return false;
  }
  uint32_t decodedIndex = 0;
  for (size_t index = 0; index < digits; ++index) {
    const char value = record.path[prefixLength + index];
    if (value < '0' || value > '9') {
      return false;
    }
    decodedIndex = decodedIndex * 10U + static_cast<uint32_t>(value - '0');
  }
  return decodedIndex == expectedIndex &&
         std::memcmp(record.path + prefixLength + digits, suffix, sizeof(suffix) - 1U) == 0;
}

bool matchesRetainedImagePath(const PdfRequiredFileRecord& record, const char* const prefix) {
  constexpr char pixelSuffix[] = ".pxc";
  constexpr char jpegSuffix[] = ".jpg";
  constexpr size_t identityDigits = 16;
  constexpr size_t variantDigits = 8;
  constexpr size_t jpegFingerprintDigits = 16;
  const size_t prefixLength = std::strlen(prefix);
  constexpr size_t commonLength = identityDigits + 1U + variantDigits;
  const size_t pixelLength = prefixLength + commonLength + sizeof(pixelSuffix) - 1U;
  const size_t jpegLength =
      prefixLength + commonLength + 1U + jpegFingerprintDigits + sizeof(jpegSuffix) - 1U;
  if ((record.pathLength != pixelLength && record.pathLength != jpegLength) ||
      std::memcmp(record.path, prefix, prefixLength) != 0) {
    return false;
  }
  for (size_t index = 0; index < identityDigits; ++index) {
    if (!isLowerHex(record.path[prefixLength + index])) {
      return false;
    }
  }
  if (record.path[prefixLength + identityDigits] != '-') {
    return false;
  }
  const size_t variantOffset = prefixLength + identityDigits + 1U;
  for (size_t index = 0; index < variantDigits; ++index) {
    if (!isLowerHex(record.path[variantOffset + index])) {
      return false;
    }
  }
  const size_t suffixOffset = variantOffset + variantDigits;
  if (record.pathLength == pixelLength) {
    return std::memcmp(record.path + suffixOffset, pixelSuffix, sizeof(pixelSuffix) - 1U) == 0;
  }
  if (record.path[suffixOffset] != '-') {
    return false;
  }
  const size_t fingerprintOffset = suffixOffset + 1U;
  for (size_t index = 0; index < jpegFingerprintDigits; ++index) {
    if (!isLowerHex(record.path[fingerprintOffset + index])) {
      return false;
    }
  }
  return std::memcmp(record.path + fingerprintOffset + jpegFingerprintDigits, jpegSuffix,
                     sizeof(jpegSuffix) - 1U) == 0;
}

PdfStatus formatJoinedPath(const char* const root, const char* const leaf, char* const destination,
                           const size_t capacity) {
  if (root == nullptr || leaf == nullptr || destination == nullptr || capacity == 0) {
    return PdfStatus::failure(PdfError::InvalidArgument);
  }
  const int length = std::snprintf(destination, capacity, "%s/%s", root, leaf);
  if (length <= 0 || static_cast<size_t>(length) >= capacity) {
    destination[0] = '\0';
    return PdfStatus::failure(PdfError::LimitExceeded);
  }
  return PdfStatus::success();
}

PdfStatus closePreservingStatus(const PdfCacheIo& io, PdfCacheHandle* const handle, const PdfStatus prior) {
  if (handle == nullptr || !handle->valid()) {
    return prior;
  }
  const PdfStatus closeStatus = io.close(io.context, handle);
  return prior ? closeStatus : prior;
}

bool validUtf8(const char* const value, const size_t length) {
  size_t offset = 0;
  while (offset < length) {
    uint32_t scalar = 0;
    if (!pdfDecodeUtf8Scalar(reinterpret_cast<const uint8_t*>(value), length, &offset, &scalar)) {
      return false;
    }
  }
  return true;
}

struct ExpectedPaths {
  char sections[PDF_CACHE_REQUIRED_PATH_CAPACITY]{};
  char images[PDF_CACHE_REQUIRED_PATH_CAPACITY]{};
  char artifacts[kArtifactCount][PDF_CACHE_REQUIRED_PATH_CAPACITY]{};
  bool initialized = false;
};

struct MinimalRecords {
  PdfRequiredFileRecord artifacts[kArtifactCount]{};
  uint16_t sectionCount = 0;
  uint8_t seen = 0;
  uint8_t stage = 0;
};

struct CaptureContext {
  const PdfCacheManifest* manifest = nullptr;
  ExpectedPaths* expected = nullptr;
  MinimalRecords* records = nullptr;

  PdfStatus initializeExpected() {
    if (expected->initialized) {
      return PdfStatus::success();
    }
    const unsigned long generation = static_cast<unsigned long>(manifest->generation);
    const int sectionsLength =
        std::snprintf(expected->sections, sizeof(expected->sections), "gen_%lu/sections/", generation);
    const int imagesLength = std::snprintf(expected->images, sizeof(expected->images), "gen_%lu/images/", generation);
    const int coverLength = std::snprintf(expected->artifacts[kCoverIndex], sizeof(expected->artifacts[kCoverIndex]),
                                          "gen_%lu/cover.bmp", generation);
    const int thumbnailLength =
        std::snprintf(expected->artifacts[kThumbnailIndex], sizeof(expected->artifacts[kThumbnailIndex]),
                      "gen_%lu/thumb.bmp", generation);
    const int metadataLength =
        std::snprintf(expected->artifacts[kMetadataIndex], sizeof(expected->artifacts[kMetadataIndex]),
                      "gen_%lu/metadata.bin", generation);
    const int outlineLength =
        std::snprintf(expected->artifacts[kOutlineIndex], sizeof(expected->artifacts[kOutlineIndex]),
                      "gen_%lu/outline.bin", generation);
    if (sectionsLength <= 0 || static_cast<size_t>(sectionsLength) >= sizeof(expected->sections) || imagesLength <= 0 ||
        static_cast<size_t>(imagesLength) >= sizeof(expected->images) || coverLength <= 0 ||
        static_cast<size_t>(coverLength) >= sizeof(expected->artifacts[kCoverIndex]) || thumbnailLength <= 0 ||
        static_cast<size_t>(thumbnailLength) >= sizeof(expected->artifacts[kThumbnailIndex]) || metadataLength <= 0 ||
        static_cast<size_t>(metadataLength) >= sizeof(expected->artifacts[kMetadataIndex]) || outlineLength <= 0 ||
        static_cast<size_t>(outlineLength) >= sizeof(expected->artifacts[kOutlineIndex])) {
      return PdfStatus::failure(PdfError::LimitExceeded);
    }
    expected->initialized = true;
    return PdfStatus::success();
  }

  static PdfStatus accept(void* const context, const PdfRequiredFileRecord& record) {
    if (context == nullptr) {
      return PdfStatus::failure(PdfError::InvalidArgument);
    }
    auto& self = *static_cast<CaptureContext*>(context);
    PdfStatus status = self.initializeExpected();
    if (!status) {
      return status;
    }
    if (matchesSectionPath(record, self.expected->sections, self.records->sectionCount)) {
      if (self.records->stage != 0 || self.records->sectionCount >= PdfMetadataLimits::MaxSections) {
        return PdfStatus::failure(PdfError::Malformed);
      }
      ++self.records->sectionCount;
      return PdfStatus::success();
    }
    if (matchesRetainedImagePath(record, self.expected->images)) {
      if (self.records->sectionCount == 0 || self.records->stage > 1) {
        return PdfStatus::failure(PdfError::Malformed);
      }
      self.records->stage = 1;
      return PdfStatus::success();
    }
    for (size_t index = 0; index < kArtifactCount; ++index) {
      if (!equalRecordPath(record, self.expected->artifacts[index])) {
        continue;
      }
      const uint8_t bit = static_cast<uint8_t>(1U << index);
      if ((self.records->seen & bit) != 0 || self.records->seen != static_cast<uint8_t>(bit - 1U) ||
          self.records->stage > static_cast<uint8_t>(index + 2U) ||
          (index == kCoverIndex && self.records->sectionCount == 0)) {
        return PdfStatus::failure(PdfError::Malformed);
      }
      self.records->artifacts[index] = record;
      self.records->seen |= bit;
      self.records->stage = static_cast<uint8_t>(index + 2U);
      return PdfStatus::success();
    }
    return PdfStatus::failure(PdfError::Malformed);
  }
};

enum class SlotKind : uint8_t {
  Missing,
  Incomplete,
  Matching,
  Stale,
  Corrupt,
  Error,
};

struct SlotScan {
  PdfCacheManifest manifest{};
  MinimalRecords records{};
  SlotKind kind = SlotKind::Missing;
  PdfStatus status = PdfStatus::failure(PdfError::InvalidOffset);
};

struct CacheHandleSource {
  const PdfCacheIo* io = nullptr;
  PdfCacheHandle handle{};
  uint64_t size = 0;

  static PdfStatus read(void* const context, const uint64_t offset, uint8_t* const destination, const size_t requested,
                        size_t* const bytesRead) {
    auto& self = *static_cast<CacheHandleSource*>(context);
    return self.io->read(self.io->context, self.handle, offset, destination, requested, bytesRead);
  }

  PdfByteSource source() { return {this, size, read}; }
};

struct ProgressRecord {
  uint32_t sequence = 0;
  uint32_t currentWord = 0;
  uint16_t section = 0;
  bool valid = false;
};

struct ArtifactFile {
  PdfCacheHandle handle{};
  uint64_t size = 0;
  uint32_t internalCrc = 0;
};

struct Workspace {
  uint8_t scratch[PDF_SOURCE_FINGERPRINT_BYTES]{};
  SlotScan slots[2]{};
  ExpectedPaths expected{};
  PdfCachedProductState candidate{};
  char cacheRoot[PDF_CACHE_PATH_CAPACITY]{};
  char path[PDF_CACHE_PATH_CAPACITY]{};
  PdfSourceIdentity sourceIdentity{};
  ProgressRecord progress[2]{};
};

// All large parsing buffers and the candidate result live in this one
// fallible allocation. Artifact payloads are streamed through scratch.
static_assert(sizeof(Workspace) < 8U * 1024U);

class WorkspaceOwner {
 public:
  explicit WorkspaceOwner(const PdfCachedProductStateAllocator& allocator) : allocator_(allocator) {
    allocation_ = allocator_.allocate(allocator_.context, sizeof(Workspace));
    if (allocation_ != nullptr) {
      workspace_ = new (allocation_) Workspace{};
    }
  }

  ~WorkspaceOwner() {
    if (workspace_ != nullptr) {
      workspace_->~Workspace();
      allocator_.release(allocator_.context, allocation_);
    }
  }

  Workspace* get() const { return workspace_; }

 private:
  PdfCachedProductStateAllocator allocator_{};
  void* allocation_ = nullptr;
  Workspace* workspace_ = nullptr;
};

void* defaultAllocate(void*, const size_t size) { return ::operator new(size, std::nothrow); }
void defaultRelease(void*, void* const allocation) { ::operator delete(allocation); }

PdfCachedProductStateLoadResult result(const PdfCachedProductStateKind kind, const PdfStatus status) {
  return {kind, status};
}

PdfCachedProductStateLoadResult cacheFailure(const PdfStatus status) {
  if (status.error == PdfError::Malformed || status.error == PdfError::UnexpectedEof ||
      status.error == PdfError::InvalidOffset) {
    return result(PdfCachedProductStateKind::Corrupt, status);
  }
  return result(PdfCachedProductStateKind::Error, status);
}

void scanManifestSlot(const PdfCacheIo& io, const char* const cacheRoot, const char* const name,
                      const PdfSourceIdentity& expectedSource, Workspace* const workspace, SlotScan* const scan) {
  *scan = {};
  PdfStatus status = formatJoinedPath(cacheRoot, name, workspace->path, sizeof(workspace->path));
  if (!status) {
    scan->kind = SlotKind::Error;
    scan->status = status;
    return;
  }
  PdfCacheHandle handle{};
  status = io.open(io.context, workspace->path, PdfCacheOpenMode::Read, &handle);
  if (!status) {
    scan->kind = status.error == PdfError::InvalidOffset ? SlotKind::Missing : SlotKind::Error;
    scan->status = status;
    return;
  }
  PdfCacheFileMetadata metadata{};
  status = io.metadata(io.context, handle, &metadata);
  if (status && (metadata.directory || metadata.symlinkLike || metadata.size > PDF_CACHE_MAX_SLOT_BYTES)) {
    status = PdfStatus::failure(PdfError::Malformed);
  }
  if (status) {
    workspace->expected = {};
    CaptureContext capture{&scan->manifest, &workspace->expected, &scan->records};
    CacheHandleSource source{&io, handle, metadata.size};
    status = pdfDecodeCacheManifest(source.source(), &scan->manifest, {&capture, CaptureContext::accept});
  }
  status = closePreservingStatus(io, &handle, status);
  if (!status) {
    scan->kind = status.error == PdfError::Malformed || status.error == PdfError::UnexpectedEof ||
                         status.error == PdfError::InvalidArgument || status.error == PdfError::LimitExceeded
                     ? SlotKind::Corrupt
                     : SlotKind::Error;
    scan->status = status;
    return;
  }
  if (!scan->manifest.completed) {
    scan->kind = SlotKind::Incomplete;
  } else if (!pdfSourceIdentityEqual(scan->manifest.source, expectedSource)) {
    scan->kind = SlotKind::Stale;
  } else if (scan->records.sectionCount == 0 || scan->records.seen != kAllArtifacts) {
    scan->kind = SlotKind::Corrupt;
    status = PdfStatus::failure(PdfError::Malformed);
  } else {
    scan->kind = SlotKind::Matching;
  }
  scan->status = status;
}

PdfStatus readExact(const PdfCacheIo& io, const PdfCacheHandle handle, const uint64_t offset,
                    uint8_t* const destination, const size_t requested) {
  size_t total = 0;
  while (total < requested) {
    size_t bytesRead = 0;
    const PdfStatus status =
        io.read(io.context, handle, offset + total, destination + total, requested - total, &bytesRead);
    if (!status) {
      return status;
    }
    if (bytesRead == 0) {
      return PdfStatus::failure(PdfError::UnexpectedEof, offset + total);
    }
    total += bytesRead;
  }
  return PdfStatus::success();
}

PdfStatus openVerifiedArtifact(const PdfCacheIo& io, const char* const cacheRoot, const PdfRequiredFileRecord& record,
                               const bool hasInternalCrc, Workspace* const workspace, ArtifactFile* const file) {
  if (record.size == 0 || record.pathLength == 0 || record.pathLength >= sizeof(record.path) ||
      record.path[record.pathLength] != '\0') {
    return PdfStatus::failure(PdfError::Malformed);
  }
  PdfStatus status = formatJoinedPath(cacheRoot, record.path, workspace->path, sizeof(workspace->path));
  if (!status) {
    return status;
  }
  *file = {};
  status = io.open(io.context, workspace->path, PdfCacheOpenMode::Read, &file->handle);
  if (!status) {
    return status.error == PdfError::InvalidOffset ? PdfStatus::failure(PdfError::Malformed) : status;
  }
  PdfCacheFileMetadata metadata{};
  status = io.metadata(io.context, file->handle, &metadata);
  if (status && (metadata.directory || metadata.symlinkLike || metadata.size != record.size ||
                 (hasInternalCrc && metadata.size < sizeof(uint32_t)))) {
    status = PdfStatus::failure(PdfError::Malformed);
  }
  uint64_t offset = 0;
  uint32_t externalCrc = 0;
  uint32_t internalCrc = 0;
  const uint64_t internalBytes = hasInternalCrc ? record.size - sizeof(uint32_t) : 0;
  while (status && offset < record.size) {
    const uint64_t remaining = record.size - offset;
    const size_t requested =
        remaining < sizeof(workspace->scratch) ? static_cast<size_t>(remaining) : sizeof(workspace->scratch);
    status = readExact(io, file->handle, offset, workspace->scratch, requested);
    if (!status) {
      break;
    }
    externalCrc = pdfCacheCrc32(workspace->scratch, requested, externalCrc);
    if (hasInternalCrc && offset < internalBytes) {
      const uint64_t internalRemaining = internalBytes - offset;
      const size_t internalRequested =
          internalRemaining < requested ? static_cast<size_t>(internalRemaining) : requested;
      internalCrc = pdfCacheCrc32(workspace->scratch, internalRequested, internalCrc);
    }
    offset += requested;
  }
  if (status && externalCrc != record.crc32) {
    status = PdfStatus::failure(PdfError::Malformed);
  }
  if (status && hasInternalCrc) {
    status = readExact(io, file->handle, internalBytes, workspace->scratch, sizeof(uint32_t));
    if (status && getU32(workspace->scratch) != internalCrc) {
      status = PdfStatus::failure(PdfError::Malformed, internalBytes);
    }
  }
  if (!status) {
    return closePreservingStatus(io, &file->handle, status);
  }
  file->size = record.size;
  file->internalCrc = internalCrc;
  return PdfStatus::success();
}

PdfStatus closeArtifact(const PdfCacheIo& io, ArtifactFile* const file, const PdfStatus prior) {
  return closePreservingStatus(io, &file->handle, prior);
}

PdfStatus validateBmpArtifact(const PdfCacheIo& io, const char* const cacheRoot, const PdfRequiredFileRecord& record,
                              const uint32_t width, const uint32_t height, char destination[PDF_CACHE_PATH_CAPACITY],
                              Workspace* const workspace) {
  const uint32_t rowBytes = ((width + 31U) / 32U) * 4U;
  const uint32_t pixelBytes = rowBytes * height;
  const uint32_t expectedSize = static_cast<uint32_t>(kBmpHeaderBytes) + pixelBytes;
  if (record.size != expectedSize) {
    return PdfStatus::failure(PdfError::Malformed);
  }
  ArtifactFile file{};
  PdfStatus status = openVerifiedArtifact(io, cacheRoot, record, false, workspace, &file);
  if (status) {
    status = readExact(io, file.handle, 0, workspace->scratch, kBmpHeaderBytes);
  }
  if (status) {
    const uint8_t* const header = workspace->scratch;
    if (header[0] != 'B' || header[1] != 'M' || getU32(header + 2) != expectedSize ||
        getU32(header + 10) != kBmpHeaderBytes || getU32(header + 14) != 40 || getU32(header + 18) != width ||
        getU32(header + 22) != static_cast<uint32_t>(-static_cast<int32_t>(height)) || getU16(header + 26) != 1 ||
        getU16(header + 28) != 1 || getU32(header + 30) != 0 || getU32(header + 34) != pixelBytes ||
        getU32(header + 46) != 2 || getU32(header + 50) != 0 || header[54] != 0 || header[55] != 0 || header[56] != 0 ||
        header[57] != 0 || header[58] != 0xff || header[59] != 0xff || header[60] != 0xff || header[61] != 0) {
      status = PdfStatus::failure(PdfError::Malformed);
    }
  }
  if (status) {
    const size_t pathLength = std::strlen(workspace->path);
    if (pathLength >= PDF_CACHE_PATH_CAPACITY) {
      status = PdfStatus::failure(PdfError::LimitExceeded);
    } else {
      std::memcpy(destination, workspace->path, pathLength + 1U);
    }
  }
  return closeArtifact(io, &file, status);
}

bool decodeProgressRecord(const uint8_t* const encoded, const PdfSourceIdentity& source, const uint32_t totalWords,
                          ProgressRecord* const record) {
  constexpr uint8_t magic[] = {'P', 'R', 'P', 'G'};
  if (std::memcmp(encoded, magic, sizeof(magic)) != 0 || getU16(encoded + 4) != 1 ||
      getU16(encoded + 6) != kProgressRecordBytes ||
      (getU32(encoded + 12) & ~(kProgressKnownFlags | kProgressModificationTimeKnown)) != 0 || encoded[82] >= 11 ||
      encoded[83] != 0 || getU32(encoded + kProgressCrcOffset) != pdfCacheCrc32(encoded, kProgressCrcOffset)) {
    return false;
  }
  for (size_t index = 88; index < kProgressCrcOffset; ++index) {
    if (encoded[index] != 0) {
      return false;
    }
  }
  const uint32_t encodedFlags = getU32(encoded + 12);
  const uint32_t flags = encodedFlags & ~kProgressModificationTimeKnown;
  PdfSourceIdentity encodedSource{};
  encodedSource.size = getU64(encoded + 16);
  encodedSource.modificationTime.value = getU64(encoded + 24);
  encodedSource.headFingerprint = getU64(encoded + 32);
  encodedSource.tailFingerprint = getU64(encoded + 40);
  encodedSource.modificationTime.known = (encodedFlags & kProgressModificationTimeKnown) != 0;
  const int32_t section = static_cast<int32_t>(getU32(encoded + 52));
  const int32_t pageNumber = static_cast<int32_t>(getU32(encoded + 56));
  const int32_t pageCount = static_cast<int32_t>(getU32(encoded + 60));
  const uint32_t globalWordOrdinal = getU32(encoded + 64);
  const uint32_t wordCursor = getU32(encoded + 84);
  if (!pdfSourceIdentityEqual(encodedSource, source) || getU32(encoded + 48) != totalWords || section < 0 ||
      section > std::numeric_limits<uint16_t>::max() || pageNumber < 0 ||
      ((flags & kProgressHasPageCount) != 0 && pageCount < 0) ||
      ((flags & kProgressHasSemanticPosition) != 0 && (totalWords == 0 || globalWordOrdinal >= totalWords)) ||
      ((flags & kProgressHasWordCursor) != 0 && wordCursor > totalWords) ||
      ((flags & kProgressHasWordCursor) == 0 && wordCursor != 0) || encoded[72 + encoded[82]] != '\0') {
    return false;
  }
  record->sequence = getU32(encoded + 8);
  record->section = static_cast<uint16_t>(section);
  record->currentWord = (flags & kProgressHasWordCursor) != 0
                            ? wordCursor
                            : ((flags & kProgressHasSemanticPosition) != 0 ? globalWordOrdinal : 0);
  record->valid = true;
  return true;
}

void readProgressSlot(const PdfCacheIo& io, const char* const cacheRoot, const char* const name,
                      const PdfSourceIdentity& source, const uint32_t totalWords, Workspace* const workspace,
                      ProgressRecord* const record) {
  *record = {};
  if (!formatJoinedPath(cacheRoot, name, workspace->path, sizeof(workspace->path))) {
    return;
  }
  PdfCacheHandle handle{};
  PdfStatus status = io.open(io.context, workspace->path, PdfCacheOpenMode::Read, &handle);
  if (!status) {
    return;
  }
  PdfCacheFileMetadata metadata{};
  status = io.metadata(io.context, handle, &metadata);
  if (status && (metadata.directory || metadata.symlinkLike || metadata.size != kProgressRecordBytes)) {
    status = PdfStatus::failure(PdfError::Malformed);
  }
  if (status) {
    status = readExact(io, handle, 0, workspace->scratch, kProgressRecordBytes);
  }
  status = closePreservingStatus(io, &handle, status);
  if (status) {
    (void)decodeProgressRecord(workspace->scratch, source, totalWords, record);
  }
}

void loadProgress(const PdfCacheIo& io, const char* const cacheRoot, const PdfSourceIdentity& source,
                  const uint32_t totalWords, Workspace* const workspace) {
  readProgressSlot(io, cacheRoot, kProgressNames[0], source, totalWords, workspace, &workspace->progress[0]);
  readProgressSlot(io, cacheRoot, kProgressNames[1], source, totalWords, workspace, &workspace->progress[1]);
  const ProgressRecord* selected = nullptr;
  if (workspace->progress[0].valid) {
    selected = &workspace->progress[0];
  }
  if (workspace->progress[1].valid &&
      (selected == nullptr || pdfCacheSequenceNewer(workspace->progress[1].sequence, selected->sequence))) {
    selected = &workspace->progress[1];
  }
  if (selected != nullptr) {
    workspace->candidate.currentSection = selected->section;
    workspace->candidate.currentWord = selected->currentWord;
    workspace->candidate.hasProgress = true;
  }
}

PdfStatus decodeMetadataArtifact(const PdfCacheIo& io, const char* const cacheRoot, const PdfRequiredFileRecord& record,
                                 const PdfCacheManifest& manifest, const uint16_t expectedSectionCount,
                                 Workspace* const workspace, int16_t* const tocIndex,
                                 uint16_t* const metadataOutlineCount) {
  ArtifactFile file{};
  PdfStatus status = openVerifiedArtifact(io, cacheRoot, record, true, workspace, &file);
  if (status) {
    status = readExact(io, file.handle, 0, workspace->scratch, kMetadataHeaderBytes);
  }
  uint16_t sectionCount = 0;
  uint16_t outlineCount = 0;
  uint16_t titleLength = 0;
  uint16_t authorLength = 0;
  uint16_t languageLength = 0;
  if (status) {
    const uint8_t* const header = workspace->scratch;
    sectionCount = getU16(header + 8);
    outlineCount = getU16(header + 10);
    titleLength = getU16(header + 16);
    authorLength = getU16(header + 18);
    languageLength = getU16(header + 20);
    const uint64_t expectedSize = kMetadataHeaderBytes + static_cast<uint64_t>(titleLength) + authorLength +
                                  languageLength + static_cast<uint64_t>(sectionCount) * kMetadataSectionBytes +
                                  sizeof(uint32_t);
    constexpr uint8_t magic[] = {'X', 'P', 'M', 'D'};
    if (std::memcmp(header, magic, sizeof(magic)) != 0 || getU16(header + 4) != PdfMetadataLimits::CodecVersion ||
        getU16(header + 6) != kMetadataHeaderBytes || sectionCount == 0 || sectionCount != expectedSectionCount ||
        sectionCount > PdfMetadataLimits::MaxSections || outlineCount == 0 ||
        outlineCount > PdfMetadataLimits::MaxOutlineEntries || getU32(header + 12) != manifest.totalWords ||
        titleLength == 0 || titleLength >= PdfCachedProductStateLimits::TitleBytes ||
        authorLength >= PdfCachedProductStateLimits::AuthorBytes ||
        languageLength >= PdfMetadataLimits::LanguageBytes || getU16(header + 22) != 0 || expectedSize != file.size) {
      status = PdfStatus::failure(PdfError::Malformed);
    }
  }
  uint64_t offset = kMetadataHeaderBytes;
  if (status) {
    status = readExact(io, file.handle, offset, reinterpret_cast<uint8_t*>(workspace->candidate.title), titleLength);
    workspace->candidate.title[titleLength] = '\0';
    offset += titleLength;
  }
  if (status) {
    status = readExact(io, file.handle, offset, reinterpret_cast<uint8_t*>(workspace->candidate.author), authorLength);
    workspace->candidate.author[authorLength] = '\0';
    offset += authorLength;
  }
  if (status && languageLength != 0) {
    status = readExact(io, file.handle, offset, workspace->scratch, languageLength);
  }
  if (status &&
      (!validUtf8(workspace->candidate.title, titleLength) || !validUtf8(workspace->candidate.author, authorLength) ||
       (languageLength != 0 && !validUtf8(reinterpret_cast<const char*>(workspace->scratch), languageLength)))) {
    status = PdfStatus::failure(PdfError::Malformed);
  }
  offset += languageLength;
  if (status && workspace->candidate.hasProgress && workspace->candidate.currentSection >= sectionCount) {
    workspace->candidate.currentSection = 0;
    workspace->candidate.currentWord = 0;
    workspace->candidate.hasProgress = false;
  }
  uint32_t cumulativeBytes = 0;
  uint32_t cumulativeWords = 0;
  *tocIndex = -1;
  *metadataOutlineCount = outlineCount;
  for (uint16_t index = 0; status && index < sectionCount; ++index) {
    status = readExact(io, file.handle, offset, workspace->scratch, kMetadataSectionBytes);
    offset += kMetadataSectionBytes;
    if (!status) {
      break;
    }
    const uint32_t byteSize = getU32(workspace->scratch);
    const uint32_t nextCumulativeBytes = getU32(workspace->scratch + 4);
    const uint32_t firstWordOrdinal = getU32(workspace->scratch + 8);
    const uint32_t wordCount = getU32(workspace->scratch + 12);
    const int16_t sectionTocIndex = static_cast<int16_t>(getU16(workspace->scratch + 20));
    if (byteSize == 0 || byteSize > std::numeric_limits<uint32_t>::max() - cumulativeBytes ||
        nextCumulativeBytes != cumulativeBytes + byteSize || firstWordOrdinal != cumulativeWords ||
        wordCount > std::numeric_limits<uint32_t>::max() - cumulativeWords || sectionTocIndex < -1 ||
        sectionTocIndex >= static_cast<int16_t>(outlineCount) || getU16(workspace->scratch + 22) != 0) {
      status = PdfStatus::failure(PdfError::Malformed, index);
      break;
    }
    cumulativeBytes = nextCumulativeBytes;
    cumulativeWords += wordCount;
    if (index == workspace->candidate.currentSection) {
      *tocIndex = sectionTocIndex;
    }
  }
  if (status && cumulativeWords != manifest.totalWords) {
    status = PdfStatus::failure(PdfError::Malformed, cumulativeWords);
  }
  return closeArtifact(io, &file, status);
}

PdfStatus decodeOutlineArtifact(const PdfCacheIo& io, const char* const cacheRoot, const PdfRequiredFileRecord& record,
                                const uint16_t section, const int16_t tocIndex, const uint16_t metadataOutlineCount,
                                Workspace* const workspace) {
  workspace->candidate.currentChapter[0] = '\0';
  ArtifactFile file{};
  PdfStatus status = openVerifiedArtifact(io, cacheRoot, record, true, workspace, &file);
  if (status) {
    status = readExact(io, file.handle, 0, workspace->scratch, kOutlineHeaderBytes);
  }
  uint16_t count = 0;
  if (status) {
    constexpr uint8_t magic[] = {'X', 'P', 'O', 'L'};
    count = getU16(workspace->scratch + 8);
    const uint64_t expectedSize =
        kOutlineHeaderBytes + static_cast<uint64_t>(count) * kOutlineRecordBytes + sizeof(uint32_t);
    if (std::memcmp(workspace->scratch, magic, sizeof(magic)) != 0 ||
        getU16(workspace->scratch + 4) != PdfOutlineLimits::CodecVersion ||
        getU16(workspace->scratch + 6) != kOutlineRecordBytes || count == 0 || count > PdfOutlineLimits::MaxEntries ||
        count != metadataOutlineCount || tocIndex >= static_cast<int16_t>(count) ||
        getU16(workspace->scratch + 10) != 0 ||
        getU32(workspace->scratch + 12) != static_cast<uint32_t>(count) * kOutlineRecordBytes ||
        expectedSize != file.size) {
      status = PdfStatus::failure(PdfError::Malformed);
    }
  }
  if (status && tocIndex < 0) {
    return closeArtifact(io, &file, status);
  }
  int16_t parentIndex = -1;
  uint8_t level = 0;
  uint8_t titleLength = 0;
  if (status) {
    const uint64_t recordOffset = kOutlineHeaderBytes + static_cast<uint64_t>(tocIndex) * kOutlineRecordBytes;
    status = readExact(io, file.handle, recordOffset, workspace->scratch, kOutlineRecordBytes);
  }
  if (status) {
    parentIndex = static_cast<int16_t>(getU16(workspace->scratch + 6));
    level = workspace->scratch[10];
    titleLength = workspace->scratch[11];
    if (getU16(workspace->scratch + 8) != section || titleLength == 0 ||
        titleLength >= PdfCachedProductStateLimits::ChapterBytes || level == 0 || level > PdfOutlineLimits::MaxDepth ||
        parentIndex >= tocIndex || parentIndex < -1 || getU16(workspace->scratch + 20) != 0 ||
        getU16(workspace->scratch + 22) != 0) {
      status = PdfStatus::failure(PdfError::Malformed, tocIndex);
    } else {
      std::memcpy(workspace->candidate.currentChapter, workspace->scratch + 24, titleLength);
      workspace->candidate.currentChapter[titleLength] = '\0';
      if (!validUtf8(workspace->candidate.currentChapter, titleLength)) {
        status = PdfStatus::failure(PdfError::Malformed, tocIndex);
      }
    }
  }
  uint8_t parentLevel = 0;
  if (status && parentIndex >= 0) {
    const uint64_t parentOffset = kOutlineHeaderBytes + static_cast<uint64_t>(parentIndex) * kOutlineRecordBytes;
    status = readExact(io, file.handle, parentOffset, workspace->scratch, kOutlineRecordBytes);
    if (status) {
      parentLevel = workspace->scratch[10];
    }
  }
  if (status && ((parentIndex < 0 && level != 1) ||
                 (parentIndex >= 0 && (parentLevel == 0 || parentLevel >= PdfOutlineLimits::MaxDepth ||
                                       level != static_cast<uint8_t>(parentLevel + 1U))))) {
    status = PdfStatus::failure(PdfError::Malformed, tocIndex);
  }
  return closeArtifact(io, &file, status);
}

PDF_CACHED_PRODUCT_NOINLINE PdfCachedProductStateLoadResult selectCompletedManifest(const PdfCacheIo& io,
                                                                                    Workspace* const workspace,
                                                                                    const SlotScan** const selected) {
  *selected = nullptr;
  for (size_t index = 0; index < 2; ++index) {
    scanManifestSlot(io, workspace->cacheRoot, kManifestNames[index], workspace->sourceIdentity, workspace,
                     &workspace->slots[index]);
  }
  bool hasStale = false;
  bool hasCorrupt = false;
  for (const SlotScan& slot : workspace->slots) {
    if (slot.kind == SlotKind::Error) {
      return result(PdfCachedProductStateKind::Error, slot.status);
    }
    hasStale = hasStale || slot.kind == SlotKind::Stale;
    hasCorrupt = hasCorrupt || slot.kind == SlotKind::Corrupt;
    if (slot.kind == SlotKind::Matching &&
        (*selected == nullptr || pdfCacheSequenceNewer(slot.manifest.sequence, (*selected)->manifest.sequence))) {
      *selected = &slot;
    }
  }
  if (*selected == nullptr) {
    if (hasStale) {
      return result(PdfCachedProductStateKind::Stale, PdfStatus::failure(PdfError::InvalidOffset));
    }
    if (hasCorrupt) {
      return result(PdfCachedProductStateKind::Corrupt, PdfStatus::failure(PdfError::Malformed));
    }
    return result(PdfCachedProductStateKind::Missing, PdfStatus::failure(PdfError::InvalidOffset));
  }
  return result(PdfCachedProductStateKind::Available, PdfStatus::success());
}

PDF_CACHED_PRODUCT_NOINLINE PdfCachedProductStateLoadResult
loadSelectedArtifacts(const PdfCacheIo& io, PdfCachedProductState* const productState, Workspace* const workspace,
                      const SlotScan& selected) {
  workspace->candidate.generation = selected.manifest.generation;
  workspace->candidate.totalWords = selected.manifest.totalWords;
  loadProgress(io, workspace->cacheRoot, workspace->sourceIdentity, selected.manifest.totalWords, workspace);

  PdfStatus status = validateBmpArtifact(io, workspace->cacheRoot, selected.records.artifacts[kCoverIndex], kCoverWidth,
                                         kCoverHeight, workspace->candidate.coverPath, workspace);
  if (!status) {
    return cacheFailure(status);
  }
  status = validateBmpArtifact(io, workspace->cacheRoot, selected.records.artifacts[kThumbnailIndex], kThumbnailWidth,
                               kThumbnailHeight, workspace->candidate.thumbnailPath, workspace);
  if (!status) {
    return cacheFailure(status);
  }
  int16_t tocIndex = -1;
  uint16_t metadataOutlineCount = 0;
  status =
      decodeMetadataArtifact(io, workspace->cacheRoot, selected.records.artifacts[kMetadataIndex], selected.manifest,
                             selected.records.sectionCount, workspace, &tocIndex, &metadataOutlineCount);
  if (!status) {
    return cacheFailure(status);
  }
  status = decodeOutlineArtifact(io, workspace->cacheRoot, selected.records.artifacts[kOutlineIndex],
                                 workspace->candidate.currentSection, tocIndex, metadataOutlineCount, workspace);
  if (!status) {
    return cacheFailure(status);
  }

  *productState = workspace->candidate;
  return result(PdfCachedProductStateKind::Available, PdfStatus::success());
}

PDF_CACHED_PRODUCT_NOINLINE PdfCachedProductStateLoadResult loadWithWorkspace(const PdfCacheIo& io,
                                                                              const char* const sourcePath,
                                                                              const char* const cacheDirectory,
                                                                              PdfCachedProductState* const productState,
                                                                              Workspace* const workspace,
                                                                              const uint64_t* const cacheHashOverride) {
  PdfStatus status =
      cacheHashOverride == nullptr
          ? pdfFormatCacheRoot(cacheDirectory, sourcePath, workspace->cacheRoot, sizeof(workspace->cacheRoot))
          : pdfFormatCacheRootForHash(cacheDirectory, *cacheHashOverride, workspace->cacheRoot,
                                      sizeof(workspace->cacheRoot));
  if (!status) {
    return result(PdfCachedProductStateKind::Error, status);
  }
  status = pdfComputeSourceIdentity(io, sourcePath, workspace->scratch, sizeof(workspace->scratch),
                                    &workspace->sourceIdentity);
  if (!status) {
    return result(
        status.error == PdfError::InvalidOffset ? PdfCachedProductStateKind::Missing : PdfCachedProductStateKind::Error,
        status);
  }
  const SlotScan* selected = nullptr;
  const PdfCachedProductStateLoadResult selection = selectCompletedManifest(io, workspace, &selected);
  if (!selection.available()) {
    return selection;
  }
  return loadSelectedArtifacts(io, productState, workspace, *selected);
}

}  // namespace

PdfCachedProductStateLoadResult pdfLoadCachedProductState(const PdfCacheIo& io, const char* const sourcePath,
                                                          const char* const cacheDirectory,
                                                          PdfCachedProductState* const productState,
                                                          const uint64_t* const cacheHashOverride) {
  const PdfCachedProductStateAllocator allocator{nullptr, defaultAllocate, defaultRelease};
  return pdfLoadCachedProductState(io, sourcePath, cacheDirectory, productState, allocator, cacheHashOverride);
}

PdfCachedProductStateLoadResult pdfLoadCachedProductState(const PdfCacheIo& io, const char* const sourcePath,
                                                          const char* const cacheDirectory,
                                                          PdfCachedProductState* const productState,
                                                          const PdfCachedProductStateAllocator& allocator,
                                                          const uint64_t* const cacheHashOverride) {
  if (productState != nullptr) {
    *productState = {};
  }
  if (!io.valid() || sourcePath == nullptr || sourcePath[0] == '\0' || cacheDirectory == nullptr ||
      cacheDirectory[0] == '\0' || productState == nullptr || !allocator.valid()) {
    return result(PdfCachedProductStateKind::Error, PdfStatus::failure(PdfError::InvalidArgument));
  }

  WorkspaceOwner owner(allocator);
  Workspace* const workspace = owner.get();
  if (workspace == nullptr) {
    return result(PdfCachedProductStateKind::Error, PdfStatus::failure(PdfError::InsufficientMemory));
  }
  return loadWithWorkspace(io, sourcePath, cacheDirectory, productState, workspace, cacheHashOverride);
}
