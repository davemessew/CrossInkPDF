#include "PdfReflowDocument.h"

#include <PixelCache.h>
#include <Print.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <limits>
#include <new>

#include "Memory.h"
#include "PdfLayoutWordIndex.h"
#include "PdfProgressStore.h"
#include "PdfSourceIdentity.h"

namespace {

#if defined(__GNUC__) || defined(__clang__)
#define PDF_REFLOW_DOCUMENT_NOINLINE __attribute__((noinline))
#elif defined(_MSC_VER)
#define PDF_REFLOW_DOCUMENT_NOINLINE __declspec(noinline)
#else
#define PDF_REFLOW_DOCUMENT_NOINLINE
#endif

template <typename T>
void resetInPlace(T& value) {
  value.~T();
  new (&value) T();
}

bool endsWith(const char* value, const size_t valueLength, const char* suffix) {
  const size_t suffixLength = std::strlen(suffix);
  return value != nullptr && valueLength >= suffixLength &&
         std::memcmp(value + valueLength - suffixLength, suffix, suffixLength) == 0;
}

PdfStatus closePreservingStatus(const PdfCacheIo& io, PdfCacheHandle* handle, const PdfStatus prior) {
  if (handle == nullptr || !handle->valid()) {
    return prior;
  }
  const PdfStatus closed = io.close(io.context, handle);
  return prior ? closed : prior;
}

float clampUnit(const float value) { return std::clamp(value, 0.0F, 1.0F); }

bool parseDecimal(const char* const bytes, const size_t length, uint32_t* const value) {
  if (bytes == nullptr || value == nullptr || length == 0) {
    return false;
  }
  uint32_t parsed = 0;
  for (size_t index = 0; index < length; ++index) {
    if (bytes[index] < '0' || bytes[index] > '9' ||
        parsed > (std::numeric_limits<uint32_t>::max() - static_cast<uint32_t>(bytes[index] - '0')) / 10U) {
      return false;
    }
    parsed = parsed * 10U + static_cast<uint32_t>(bytes[index] - '0');
  }
  *value = parsed;
  return true;
}

bool parseSectionRecordPath(const PdfRequiredFileRecord& record, uint32_t* const generation,
                            uint16_t* const sectionIndex) {
  static constexpr char prefix[] = "gen_";
  static constexpr char middle[] = "/sections/";
  static constexpr char suffix[] = ".xhtml";
  if (generation == nullptr || sectionIndex == nullptr ||
      record.pathLength < sizeof(prefix) - 1 + 1 + sizeof(middle) - 1 + 6 + sizeof(suffix) - 1 ||
      std::memcmp(record.path, prefix, sizeof(prefix) - 1) != 0 || !endsWith(record.path, record.pathLength, suffix)) {
    return false;
  }
  const char* const middlePosition = std::strstr(record.path + sizeof(prefix) - 1, middle);
  if (middlePosition == nullptr) {
    return false;
  }
  const size_t generationLength = static_cast<size_t>(middlePosition - (record.path + sizeof(prefix) - 1));
  const char* const section = middlePosition + sizeof(middle) - 1;
  const size_t suffixLength = sizeof(suffix) - 1;
  if (generationLength == 0 || static_cast<size_t>(record.path + record.pathLength - section) != 6 + suffixLength) {
    return false;
  }
  uint32_t parsedGeneration = 0;
  uint32_t parsedSection = 0;
  if (!parseDecimal(record.path + sizeof(prefix) - 1, generationLength, &parsedGeneration) ||
      !parseDecimal(section, 6, &parsedSection) || parsedSection >= PdfMetadataLimits::MaxSections) {
    return false;
  }
  *generation = parsedGeneration;
  *sectionIndex = static_cast<uint16_t>(parsedSection);
  return true;
}

bool recordPathEquals(const PdfRequiredFileRecord& record, const char* const expected) {
  return expected != nullptr && std::strlen(expected) == record.pathLength &&
         std::memcmp(record.path, expected, record.pathLength) == 0;
}

bool isLowerHex(const char value) { return (value >= '0' && value <= '9') || (value >= 'a' && value <= 'f'); }

bool parseHex64(const char* const bytes, const size_t length, uint64_t* const value) {
  if (bytes == nullptr || value == nullptr || length == 0 || length > 16) {
    return false;
  }
  uint64_t parsed = 0;
  for (size_t index = 0; index < length; ++index) {
    if (!isLowerHex(bytes[index])) {
      return false;
    }
    const uint8_t nibble = bytes[index] <= '9' ? static_cast<uint8_t>(bytes[index] - '0')
                                               : static_cast<uint8_t>(bytes[index] - 'a' + 10);
    parsed = (parsed << 4U) | nibble;
  }
  *value = parsed;
  return true;
}

bool parseImageLeaf(const char* const leaf, const size_t length, PdfCachedResourceRecord* const resource) {
  constexpr size_t hashDigits = 16;
  constexpr size_t crcDigits = 8;
  constexpr size_t lengthDigits = 16;
  constexpr char pixelSuffix[] = ".pxc";
  constexpr char jpegSuffix[] = ".jpg";
  constexpr size_t pixelLength = hashDigits + 1U + crcDigits + sizeof(pixelSuffix) - 1U;
  constexpr size_t jpegLength =
      hashDigits + 1U + crcDigits + 1U + lengthDigits + sizeof(jpegSuffix) - 1U;
  if (leaf == nullptr || resource == nullptr || (length != pixelLength && length != jpegLength) ||
      leaf[hashDigits] != '-') {
    return false;
  }

  PdfCachedResourceRecord parsed{};
  uint64_t nameCrc = 0;
  if (!parseHex64(leaf, hashDigits, &parsed.contentHash) ||
      !parseHex64(leaf + hashDigits + 1U, crcDigits, &nameCrc)) {
    return false;
  }
  parsed.nameCrc32 = static_cast<uint32_t>(nameCrc);
  const size_t commonEnd = hashDigits + 1U + crcDigits;
  if (length == pixelLength) {
    if (std::memcmp(leaf + commonEnd, pixelSuffix, sizeof(pixelSuffix) - 1U) != 0) {
      return false;
    }
    parsed.kind = PdfCachedResourceKind::PixelCache;
  } else {
    if (leaf[commonEnd] != '-' ||
        !parseHex64(leaf + commonEnd + 1U, lengthDigits, &parsed.encodedLength) ||
        parsed.encodedLength == 0 ||
        std::memcmp(leaf + commonEnd + 1U + lengthDigits, jpegSuffix, sizeof(jpegSuffix) - 1U) != 0) {
      return false;
    }
    parsed.kind = PdfCachedResourceKind::Jpeg;
  }
  *resource = parsed;
  return true;
}

bool sameResourceIdentity(const PdfCachedResourceRecord& left, const PdfCachedResourceRecord& right) {
  return left.kind == right.kind && left.contentHash == right.contentHash && left.nameCrc32 == right.nameCrc32 &&
         left.encodedLength == right.encodedLength;
}

bool jpegMarkerHasNoPayload(const uint8_t marker) {
  return marker == 0x01U || marker == 0xd8U || marker == 0xd9U || (marker >= 0xd0U && marker <= 0xd7U);
}

bool jpegMarkerIsStartOfFrame(const uint8_t marker) {
  return marker >= 0xc0U && marker <= 0xcfU && marker != 0xc4U && marker != 0xc8U && marker != 0xccU;
}

class JpegDimensionScanner {
 public:
  bool consume(const uint8_t* const bytes, const size_t length) {
    if ((bytes == nullptr && length != 0) || failed_) {
      return false;
    }
    for (size_t index = 0; index < length && !found_; ++index) {
      if (!consumeByte(bytes[index])) {
        failed_ = true;
        return false;
      }
    }
    return true;
  }

  bool finish(uint16_t* const width, uint16_t* const height) const {
    if (width == nullptr || height == nullptr || failed_ || !found_ || width_ == 0 || height_ == 0) {
      return false;
    }
    *width = width_;
    *height = height_;
    return true;
  }

 private:
  enum class State : uint8_t {
    SoiPrefix,
    SoiCode,
    MarkerPrefix,
    MarkerCode,
    LengthHigh,
    LengthLow,
    Segment,
  };

  bool consumeByte(const uint8_t byte) {
    switch (state_) {
      case State::SoiPrefix:
        state_ = State::SoiCode;
        return byte == 0xffU;
      case State::SoiCode:
        state_ = State::MarkerPrefix;
        return byte == 0xd8U;
      case State::MarkerPrefix:
        if (byte != 0xffU) {
          return false;
        }
        state_ = State::MarkerCode;
        return true;
      case State::MarkerCode:
        if (byte == 0xffU) {
          return true;
        }
        if (byte == 0x00U || byte == 0xdaU || byte == 0xd9U) {
          return false;
        }
        marker_ = byte;
        if (jpegMarkerHasNoPayload(marker_)) {
          state_ = State::MarkerPrefix;
        } else {
          state_ = State::LengthHigh;
        }
        return true;
      case State::LengthHigh:
        segmentLength_ = static_cast<uint16_t>(byte) << 8U;
        state_ = State::LengthLow;
        return true;
      case State::LengthLow:
        segmentLength_ |= byte;
        if (segmentLength_ < 2U || (jpegMarkerIsStartOfFrame(marker_) && segmentLength_ < 8U)) {
          return false;
        }
        segmentRemaining_ = static_cast<uint16_t>(segmentLength_ - 2U);
        segmentOffset_ = 0;
        state_ = segmentRemaining_ == 0 ? State::MarkerPrefix : State::Segment;
        return true;
      case State::Segment:
        if (jpegMarkerIsStartOfFrame(marker_)) {
          if (segmentOffset_ == 1U) {
            height_ = static_cast<uint16_t>(byte) << 8U;
          } else if (segmentOffset_ == 2U) {
            height_ |= byte;
          } else if (segmentOffset_ == 3U) {
            width_ = static_cast<uint16_t>(byte) << 8U;
          } else if (segmentOffset_ == 4U) {
            width_ |= byte;
            found_ = width_ != 0 && height_ != 0;
          }
        }
        ++segmentOffset_;
        if (--segmentRemaining_ == 0) {
          state_ = State::MarkerPrefix;
        }
        return true;
    }
    return false;
  }

  State state_ = State::SoiPrefix;
  uint16_t segmentLength_ = 0;
  uint16_t segmentRemaining_ = 0;
  uint16_t segmentOffset_ = 0;
  uint16_t width_ = 0;
  uint16_t height_ = 0;
  uint8_t marker_ = 0;
  bool found_ = false;
  bool failed_ = false;
};

struct ManifestSelectionWorkspace {
  PdfCacheStore store;
  PdfCacheManifestSelection selection{};
};

}  // namespace

PdfStatus PdfReflowDocument::ManifestSource::read(void* context, const uint64_t offset, uint8_t* destination,
                                                  const size_t requested, size_t* bytesRead) {
  if (context == nullptr) {
    return PdfStatus::failure(PdfError::InvalidArgument, offset);
  }
  auto& self = *static_cast<ManifestSource*>(context);
  return self.io->read(self.io->context, self.handle, offset, destination, requested, bytesRead);
}

PdfStatus PdfReflowDocument::initialize(const PdfCacheIo& io, const char* const sourcePath,
                                        const char* const cacheDirectory, const uint64_t* const cacheHashOverride) {
  resetLoadedState();
  if (!io.valid() || sourcePath == nullptr || sourcePath[0] == '\0' || cacheDirectory == nullptr) {
    status_ = PdfStatus::failure(PdfError::InvalidArgument);
    return status_;
  }
  io_ = io;
  sourcePath_ = sourcePath;
  char root[PDF_CACHE_PATH_CAPACITY]{};
  status_ = cacheHashOverride == nullptr
                ? pdfFormatCacheRoot(cacheDirectory, sourcePath, root, sizeof(root))
                : pdfFormatCacheRootForHash(cacheDirectory, *cacheHashOverride, root, sizeof(root));
  if (!status_) {
    sourcePath_.clear();
    return status_;
  }
  cacheRoot_ = root;
  deriveFallbackTitle();
  status_ = PdfStatus::success();
  return status_;
}

void PdfReflowDocument::resetLoadedState() {
  loaded_ = false;
  sourceIdentity_ = {};
  manifest_ = {};
  metadata_ = {};
  coverRecord_ = {};
  thumbnailRecord_ = {};
  metadataRecord_ = {};
  outlineRecord_ = {};
  progressStore_ = {};
  resetInPlace(savedItemsStore_);
  savedItemsReady_ = false;
  metadataPath_.clear();
  outlinePath_.clear();
  coverPath_.clear();
  thumbnailPath_.clear();
  sections_.reset();
  manifestSectionSizes_.fill(0);
  manifestSectionCrcs_.fill(0);
  manifestSectionSeen_.fill(0);
  resources_.fill({});
  validationPath_.fill('\0');
  cachedOutlineEntry_ = {};
  cachedOutlineIndex_ = -1;
  layoutWordWindow_.fill({});
  layoutWordIndexPath_.fill('\0');
  layoutWordWindowStart_ = 0;
  layoutWordWindowCount_ = 0;
  layoutWordIndexAvailable_ = false;
  validationGeneration_ = 0;
  requiredFilesSeen_ = 0;
  xhtmlFilesSeen_ = 0;
  resourceCount_ = 0;
  metadataFilesSeen_ = 0;
  outlineFilesSeen_ = 0;
  manifestFileStage_ = ManifestFileStage::Sections;
}

void PdfReflowDocument::deriveFallbackTitle() {
  const size_t slash = sourcePath_.find_last_of("/\\");
  const size_t start = slash == std::string::npos ? 0 : slash + 1;
  size_t end = sourcePath_.size();
  if (end >= start + 4) {
    const std::string extension = sourcePath_.substr(end - 4);
    if (extension == ".pdf" || extension == ".PDF" || extension == ".Pdf" || extension == ".pDf" ||
        extension == ".pdF" || extension == ".PDf" || extension == ".pDF" || extension == ".PdF") {
      end -= 4;
    }
  }
  title_ = sourcePath_.substr(start, end - start);
  if (title_.empty()) {
    title_ = "PDF";
  }
  author_.clear();
  language_.clear();
}

PdfStatus PdfReflowDocument::captureRequiredFile(void* context, const PdfRequiredFileRecord& record) {
  return context == nullptr ? PdfStatus::failure(PdfError::InvalidArgument)
                            : static_cast<PdfReflowDocument*>(context)->captureFile(record);
}

PDF_REFLOW_DOCUMENT_NOINLINE PdfStatus PdfReflowDocument::captureFile(const PdfRequiredFileRecord& record) {
  ++requiredFilesSeen_;
  uint32_t generation = 0;
  uint16_t sectionIndex = 0;
  if (parseSectionRecordPath(record, &generation, &sectionIndex)) {
    if (manifestFileStage_ != ManifestFileStage::Sections || generation != validationGeneration_ ||
        sectionIndex != xhtmlFilesSeen_ || record.size == 0 || record.size > UINT32_MAX) {
      return PdfStatus::failure(PdfError::Malformed);
    }
    const size_t byteIndex = sectionIndex / 8U;
    const uint8_t mask = static_cast<uint8_t>(1U << (sectionIndex & 7U));
    if ((manifestSectionSeen_[byteIndex] & mask) != 0) {
      return PdfStatus::failure(PdfError::Malformed, sectionIndex);
    }
    manifestSectionSeen_[byteIndex] |= mask;
    manifestSectionSizes_[sectionIndex] = static_cast<uint32_t>(record.size);
    manifestSectionCrcs_[sectionIndex] = record.crc32;
    ++xhtmlFilesSeen_;
    return PdfStatus::success();
  }

  char expected[PDF_CACHE_REQUIRED_PATH_CAPACITY]{};
  const int imagePrefixLength =
      std::snprintf(expected, sizeof(expected), "gen_%lu/images/", static_cast<unsigned long>(validationGeneration_));
  if (imagePrefixLength <= 0 || static_cast<size_t>(imagePrefixLength) >= sizeof(expected)) {
    return PdfStatus::failure(PdfError::LimitExceeded);
  }
  if (record.pathLength > static_cast<size_t>(imagePrefixLength) &&
      std::memcmp(record.path, expected, static_cast<size_t>(imagePrefixLength)) == 0) {
    if ((manifestFileStage_ != ManifestFileStage::Sections && manifestFileStage_ != ManifestFileStage::Images) ||
        xhtmlFilesSeen_ == 0) {
      return PdfStatus::failure(PdfError::Malformed);
    }
    if (resourceCount_ >= resources_.size()) {
      return PdfStatus::failure(PdfError::LimitExceeded, resourceCount_);
    }
    PdfCachedResourceRecord resource{};
    if (!parseImageLeaf(record.path + imagePrefixLength, record.pathLength - imagePrefixLength, &resource) ||
        record.size == 0) {
      return PdfStatus::failure(PdfError::Malformed, resourceCount_);
    }
    resource.fileSize = record.size;
    resource.fileCrc32 = record.crc32;
    if (resource.kind == PdfCachedResourceKind::Jpeg &&
        (resource.encodedLength != record.size || resource.nameCrc32 != record.crc32)) {
      return PdfStatus::failure(PdfError::Malformed, resourceCount_);
    }
    for (uint8_t index = 0; index < resourceCount_; ++index) {
      if (sameResourceIdentity(resources_[index], resource)) {
        return PdfStatus::failure(PdfError::Malformed, index);
      }
    }
    resources_[resourceCount_++] = resource;
    manifestFileStage_ = ManifestFileStage::Images;
    return PdfStatus::success();
  }

  const auto matchesArtifact = [&](const char* const leaf) {
    const int length =
        std::snprintf(expected, sizeof(expected), "gen_%lu/%s", static_cast<unsigned long>(validationGeneration_), leaf);
    return length > 0 && static_cast<size_t>(length) < sizeof(expected) && recordPathEquals(record, expected);
  };

  if (matchesArtifact("cover.bmp")) {
    if ((manifestFileStage_ != ManifestFileStage::Sections && manifestFileStage_ != ManifestFileStage::Images) ||
        xhtmlFilesSeen_ == 0 || record.size == 0) {
      return PdfStatus::failure(PdfError::Malformed);
    }
    coverRecord_ = record;
    manifestFileStage_ = ManifestFileStage::Cover;
    return PdfStatus::success();
  }
  if (matchesArtifact("thumb.bmp")) {
    if (manifestFileStage_ != ManifestFileStage::Cover || record.size == 0) {
      return PdfStatus::failure(PdfError::Malformed);
    }
    thumbnailRecord_ = record;
    manifestFileStage_ = ManifestFileStage::Thumbnail;
    return PdfStatus::success();
  }
  if (matchesArtifact("metadata.bin")) {
    if (manifestFileStage_ != ManifestFileStage::Thumbnail || record.size == 0) {
      return PdfStatus::failure(PdfError::Malformed);
    }
    ++metadataFilesSeen_;
    metadataRecord_ = record;
    manifestFileStage_ = ManifestFileStage::Metadata;
    return PdfStatus::success();
  }
  if (matchesArtifact("outline.bin")) {
    if (manifestFileStage_ != ManifestFileStage::Metadata || record.size == 0) {
      return PdfStatus::failure(PdfError::Malformed);
    }
    ++outlineFilesSeen_;
    outlineRecord_ = record;
    manifestFileStage_ = ManifestFileStage::Outline;
    return PdfStatus::success();
  }
  return PdfStatus::failure(PdfError::Malformed, requiredFilesSeen_ - 1U);
}

PDF_REFLOW_DOCUMENT_NOINLINE PdfStatus PdfReflowDocument::validateCachedFile(
    const char* const path, const uint64_t expectedSize, const uint32_t expectedCrc32,
    PdfCachedResourceRecord* const resource) {
  if (path == nullptr || path[0] == '\0' || expectedSize == 0 || !ioWorkspace_) {
    return PdfStatus::failure(PdfError::InvalidArgument);
  }
  PdfCacheHandle handle{};
  PdfStatus status = io_.open(io_.context, path, PdfCacheOpenMode::Read, &handle);
  if (!status) {
    return status;
  }
  PdfCacheFileMetadata metadata{};
  status = io_.metadata(io_.context, handle, &metadata);
  if (status && (metadata.directory || metadata.symlinkLike || metadata.size != expectedSize)) {
    status = PdfStatus::failure(PdfError::Malformed);
  }

  JpegDimensionScanner jpegDimensions;
  uint8_t pixelHeader[pixel_cache::kHeaderSize]{};
  uint64_t offset = 0;
  uint64_t contentHash = PDF_CACHE_FNV64_OFFSET;
  uint32_t crc = 0;
  while (status && offset < expectedSize) {
    const size_t requested =
        static_cast<size_t>(std::min<uint64_t>(PDF_SOURCE_FINGERPRINT_BYTES, expectedSize - offset));
    size_t bytesRead = 0;
    status = io_.read(io_.context, handle, offset, ioWorkspace_.get(), requested, &bytesRead);
    if (!status) {
      break;
    }
    if (bytesRead != requested) {
      status = PdfStatus::failure(PdfError::UnexpectedEof, offset + bytesRead);
      break;
    }
    if (resource != nullptr && resource->kind == PdfCachedResourceKind::PixelCache && offset == 0 &&
        bytesRead >= sizeof(pixelHeader)) {
      std::memcpy(pixelHeader, ioWorkspace_.get(), sizeof(pixelHeader));
    }
    if (resource != nullptr && resource->kind == PdfCachedResourceKind::Jpeg) {
      contentHash = pdfCacheFnv64(ioWorkspace_.get(), bytesRead, contentHash);
      if (!jpegDimensions.consume(ioWorkspace_.get(), bytesRead)) {
        status = PdfStatus::failure(PdfError::Malformed, offset);
        break;
      }
    }
    crc = pdfCacheCrc32(ioWorkspace_.get(), bytesRead, crc);
    offset += bytesRead;
  }
  if (status && crc != expectedCrc32) {
    status = PdfStatus::failure(PdfError::Malformed);
  }
  status = closePreservingStatus(io_, &handle, status);
  if (!status || resource == nullptr) {
    return status;
  }

  if (resource->kind == PdfCachedResourceKind::PixelCache) {
    pixel_cache::Layout layout{};
    if (expectedSize < sizeof(pixelHeader) ||
        pixel_cache::decodeHeader(pixelHeader, sizeof(pixelHeader), layout) != pixel_cache::Status::Ok ||
        layout.fileBytes != expectedSize) {
      return PdfStatus::failure(PdfError::Malformed);
    }
    resource->width = layout.width;
    resource->height = layout.height;
    return PdfStatus::success();
  }
  if (resource->kind == PdfCachedResourceKind::Jpeg) {
    uint16_t width = 0;
    uint16_t height = 0;
    if (contentHash != resource->contentHash || crc != resource->nameCrc32 ||
        expectedSize != resource->encodedLength || !jpegDimensions.finish(&width, &height)) {
      return PdfStatus::failure(PdfError::Malformed);
    }
    resource->width = width;
    resource->height = height;
    return PdfStatus::success();
  }
  return PdfStatus::failure(PdfError::Malformed);
}

PdfStatus PdfReflowDocument::validateResourceFile(const uint8_t resourceIndex) {
  if (resourceIndex >= resourceCount_ ||
      !formatResourcePath(resources_[resourceIndex], validationPath_.data(), validationPath_.size())) {
    return PdfStatus::failure(PdfError::InvalidArgument, resourceIndex);
  }
  PdfCachedResourceRecord& resource = resources_[resourceIndex];
  return validateCachedFile(validationPath_.data(), resource.fileSize, resource.fileCrc32, &resource);
}

PDF_REFLOW_DOCUMENT_NOINLINE PdfStatus PdfReflowDocument::validateCapturedFiles() {
  if (manifestFileStage_ != ManifestFileStage::Outline || xhtmlFilesSeen_ == 0 || metadataFilesSeen_ != 1 ||
      outlineFilesSeen_ != 1) {
    return PdfStatus::failure(PdfError::Malformed);
  }

  for (uint16_t index = 0; index < xhtmlFilesSeen_; ++index) {
    const int length = std::snprintf(validationPath_.data(), validationPath_.size(),
                                     "%s/gen_%lu/sections/%06u.xhtml", cacheRoot_.c_str(),
                                     static_cast<unsigned long>(validationGeneration_), static_cast<unsigned>(index));
    if (length <= 0 || static_cast<size_t>(length) >= validationPath_.size()) {
      return PdfStatus::failure(PdfError::LimitExceeded, index);
    }
    const PdfStatus status =
        validateCachedFile(validationPath_.data(), manifestSectionSizes_[index], manifestSectionCrcs_[index], nullptr);
    if (!status) {
      return status;
    }
  }
  for (uint8_t index = 0; index < resourceCount_; ++index) {
    const PdfStatus status = validateResourceFile(index);
    if (!status) {
      return status;
    }
  }

  struct Artifact {
    const char* leaf;
    const PdfRequiredFileRecord* record;
    std::string* path;
  };
  const Artifact artifacts[] = {
      {"cover.bmp", &coverRecord_, &coverPath_},
      {"thumb.bmp", &thumbnailRecord_, &thumbnailPath_},
      {"metadata.bin", &metadataRecord_, &metadataPath_},
      {"outline.bin", &outlineRecord_, &outlinePath_},
  };
  for (const Artifact& artifact : artifacts) {
    const int length = std::snprintf(validationPath_.data(), validationPath_.size(), "%s/gen_%lu/%s",
                                     cacheRoot_.c_str(), static_cast<unsigned long>(validationGeneration_),
                                     artifact.leaf);
    if (length <= 0 || static_cast<size_t>(length) >= validationPath_.size()) {
      return PdfStatus::failure(PdfError::LimitExceeded);
    }
    const PdfStatus status =
        validateCachedFile(validationPath_.data(), artifact.record->size, artifact.record->crc32, nullptr);
    if (!status) {
      return status;
    }
    *artifact.path = validationPath_.data();
  }
  return PdfStatus::success();
}

PdfStatus PdfReflowDocument::captureMetadataSection(void* context, const uint16_t index,
                                                    const PdfMetadataSection& record) {
  if (context == nullptr) {
    return PdfStatus::failure(PdfError::InvalidArgument);
  }
  auto& self = *static_cast<PdfReflowDocument*>(context);
  if (!self.sections_ || index >= self.metadata_.sectionCount) {
    return PdfStatus::failure(PdfError::InvalidOffset, index);
  }
  self.sections_[index] = record;
  return PdfStatus::success();
}

PdfStatus PdfReflowDocument::validateOutlineEntry(void* context, const uint16_t index, const PdfOutlineEntry& record) {
  if (context == nullptr) {
    return PdfStatus::failure(PdfError::InvalidArgument);
  }
  auto& self = *static_cast<PdfReflowDocument*>(context);
  if (!self.sections_ || index >= self.metadata_.outlineCount || record.sectionIndex >= self.metadata_.sectionCount) {
    return PdfStatus::failure(PdfError::Malformed, index);
  }
  const PdfMetadataSection& section = self.sections_[record.sectionIndex];
  if (record.anchorOrdinal < section.firstAnchorOrdinal ||
      (record.sectionIndex + 1U < self.metadata_.sectionCount &&
       record.anchorOrdinal >= self.sections_[record.sectionIndex + 1U].firstAnchorOrdinal)) {
    return PdfStatus::failure(PdfError::Malformed, index);
  }
  return PdfStatus::success();
}

PDF_REFLOW_DOCUMENT_NOINLINE PdfStatus PdfReflowDocument::selectCompletedManifest(PdfCacheSlot* const selectedSlot) {
  if (selectedSlot == nullptr) {
    return PdfStatus::failure(PdfError::InvalidArgument);
  }
  // This is one bounded cold-path allocation per book open. Keeping the store
  // plus its three-manifest selection workspace off the reader task stack saves
  // roughly 400 bytes; it is released before any pagination or rendering.
  auto workspace = makeUniqueNoThrow<ManifestSelectionWorkspace>();
  if (!workspace) {
    return PdfStatus::failure(PdfError::InsufficientMemory);
  }
  PdfStatus status = workspace->store.initialize(io_, cacheRoot_.c_str());
  if (status) {
    status = workspace->store.loadManifestSlots(sourceIdentity_, &workspace->selection);
  }
  if (!status) {
    return status;
  }
  if (!workspace->selection.selected || !workspace->selection.manifest.completed) {
    return PdfStatus::failure(PdfError::InvalidOffset);
  }
  manifest_ = workspace->selection.manifest;
  *selectedSlot = workspace->selection.selectedSlot;
  return PdfStatus::success();
}

PDF_REFLOW_DOCUMENT_NOINLINE PdfStatus PdfReflowDocument::decodeSelectedManifest(const PdfCacheSlot selectedSlot) {
  const char* const manifestName = selectedSlot == PdfCacheSlot::A ? "manifest.a" : "manifest.b";
  const int pathLength = std::snprintf(validationPath_.data(), validationPath_.size(), "%s/%s", cacheRoot_.c_str(),
                                       manifestName);
  if (pathLength < 0 || static_cast<size_t>(pathLength) >= validationPath_.size()) {
    return PdfStatus::failure(PdfError::LimitExceeded);
  }

  PdfCacheHandle handle{};
  PdfStatus status = io_.open(io_.context, validationPath_.data(), PdfCacheOpenMode::Read, &handle);
  if (!status) {
    return status;
  }
  PdfCacheFileMetadata metadata{};
  status = io_.metadata(io_.context, handle, &metadata);
  if (status && (metadata.directory || metadata.symlinkLike)) {
    status = PdfStatus::failure(PdfError::Malformed);
  }
  PdfCacheManifest decoded{};
  if (status) {
    validationGeneration_ = manifest_.generation;
    ManifestSource source{&io_, handle, metadata.size};
    status = pdfDecodeCacheManifest(source.source(), &decoded, {this, captureRequiredFile});
  }
  status = closePreservingStatus(io_, &handle, status);
  if (!status) {
    return status;
  }
  if (!decoded.completed || !pdfSourceIdentityEqual(decoded.source, sourceIdentity_) ||
      decoded.generation != manifest_.generation || decoded.sequence != manifest_.sequence ||
      requiredFilesSeen_ != decoded.requiredFileCount || xhtmlFilesSeen_ == 0 || metadataFilesSeen_ != 1 ||
      outlineFilesSeen_ != 1 || manifestFileStage_ != ManifestFileStage::Outline) {
    return PdfStatus::failure(PdfError::Malformed);
  }
  manifest_ = decoded;
  return PdfStatus::success();
}

PDF_REFLOW_DOCUMENT_NOINLINE PdfStatus PdfReflowDocument::loadMetadataCache() {
  if (metadataPath_.empty() || metadataRecord_.size == 0) {
    return PdfStatus::failure(PdfError::Malformed);
  }
  PdfCacheHandle handle{};
  PdfStatus status = io_.open(io_.context, metadataPath_.c_str(), PdfCacheOpenMode::Read, &handle);
  if (!status) {
    return status;
  }
  ManifestSource sourceContext{&io_, handle, metadataRecord_.size};
  metadata_ = {};
  status = pdfInspectMetadata(sourceContext.source(), &metadata_);
  if (status) {
    sections_ = makeUniqueNoThrow<PdfMetadataSection[]>(metadata_.sectionCount);
    if (!sections_) {
      status = PdfStatus::failure(PdfError::InsufficientMemory);
    }
  }
  if (status) {
    status = pdfDecodeMetadata(sourceContext.source(), &metadata_, {this, captureMetadataSection});
  }
  status = closePreservingStatus(io_, &handle, status);
  if (!status) {
    return status;
  }
  if (metadata_.totalWords != manifest_.totalWords || metadata_.sectionCount != xhtmlFilesSeen_ ||
      metadata_.outlineCount == 0) {
    return PdfStatus::failure(PdfError::Malformed);
  }
  for (uint16_t index = 0; index < metadata_.sectionCount; ++index) {
    const size_t byteIndex = index / 8U;
    const uint8_t mask = static_cast<uint8_t>(1U << (index & 7U));
    if ((manifestSectionSeen_[byteIndex] & mask) == 0 || manifestSectionSizes_[index] != sections_[index].byteSize) {
      return PdfStatus::failure(PdfError::Malformed, index);
    }
  }
  return PdfStatus::success();
}

PdfStatus PdfReflowDocument::loadOutlineCache() {
  if (outlinePath_.empty() || outlineRecord_.size == 0) {
    return PdfStatus::failure(PdfError::Malformed);
  }
  PdfCacheHandle handle{};
  PdfStatus status = io_.open(io_.context, outlinePath_.c_str(), PdfCacheOpenMode::Read, &handle);
  if (!status) {
    return status;
  }
  ManifestSource sourceContext{&io_, handle, outlineRecord_.size};
  PdfOutlineHeader header{};
  status = pdfDecodeOutline(sourceContext.source(), &header, {this, validateOutlineEntry});
  status = closePreservingStatus(io_, &handle, status);
  if (status && header.entryCount != metadata_.outlineCount) {
    status = PdfStatus::failure(PdfError::Malformed);
  }
  return status;
}

PdfStatus PdfReflowDocument::loadCompletedCache() {
  resetLoadedState();
  if (!io_.valid() || sourcePath_.empty() || cacheRoot_.empty()) {
    status_ = PdfStatus::failure(PdfError::InvalidArgument);
    return status_;
  }
  if (!ioWorkspace_) {
    ioWorkspace_ = makeUniqueNoThrow<uint8_t[]>(PDF_SOURCE_FINGERPRINT_BYTES);
    if (!ioWorkspace_) {
      status_ = PdfStatus::failure(PdfError::InsufficientMemory);
      return status_;
    }
  }

  status_ = pdfComputeSourceIdentity(io_, sourcePath_.c_str(), ioWorkspace_.get(), PDF_SOURCE_FINGERPRINT_BYTES,
                                     &sourceIdentity_);
  if (!status_) {
    return status_;
  }

  PdfCacheSlot selectedSlot = PdfCacheSlot::A;
  status_ = selectCompletedManifest(&selectedSlot);
  if (status_) {
    status_ = decodeSelectedManifest(selectedSlot);
  }
  if (!status_) {
    resetLoadedState();
    return status_;
  }
  status_ = validateCapturedFiles();
  if (status_) {
    status_ = loadMetadataCache();
  }
  if (status_) {
    status_ = loadOutlineCache();
  }
  if (!status_) {
    resetLoadedState();
    return status_;
  }
  status_ = progressStore_.initialize(io_, cacheRoot_.c_str(), sourceIdentity_, metadata_.totalWords);
  if (!status_) {
    resetLoadedState();
    return status_;
  }
  status_ = savedItemsStore_.initialize(io_, cacheRoot_.c_str(), sourceIdentity_, metadata_.totalWords);
  if (!status_) {
    resetLoadedState();
    return status_;
  }
  savedItemsReady_ = true;
  title_.assign(metadata_.title, metadata_.titleLength);
  author_.assign(metadata_.author, metadata_.authorLength);
  language_.assign(metadata_.language, metadata_.languageLength);
  loaded_ = true;
  status_ = PdfStatus::success();
  return status_;
}

std::string PdfReflowDocument::getCoverBmpPath(bool) const { return loaded_ ? coverPath_ : std::string{}; }
bool PdfReflowDocument::generateCoverBmp(bool, const GfxRenderer*, int) const {
  return loaded_ && !coverPath_.empty();
}
std::string PdfReflowDocument::getThumbBmpPath() const { return loaded_ ? thumbnailPath_ : std::string{}; }
std::string PdfReflowDocument::getThumbBmpPath(int, int) const { return getThumbBmpPath(); }
std::string PdfReflowDocument::getAdaptiveThumbBmpPath(int, int) const { return getThumbBmpPath(); }
bool PdfReflowDocument::generateThumbBmp(int, int, const GfxRenderer*, int) const {
  return loaded_ && !thumbnailPath_.empty();
}
bool PdfReflowDocument::generateAdaptiveThumbBmp(int, int, const GfxRenderer*, int) const {
  return loaded_ && !thumbnailPath_.empty();
}

int PdfReflowDocument::getSectionCount() const { return loaded_ ? metadata_.sectionCount : 0; }

bool PdfReflowDocument::formatSectionHref(const int sectionIndex, char* const output, const size_t capacity) const {
  if (!loaded_ || output == nullptr || capacity == 0 || sectionIndex < 0 || sectionIndex >= metadata_.sectionCount) {
    return false;
  }
  const int written = std::snprintf(output, capacity, "sections/%06d.xhtml", sectionIndex);
  return written > 0 && static_cast<size_t>(written) < capacity;
}

bool PdfReflowDocument::formatSectionPath(const int sectionIndex, char* const output, const size_t capacity) const {
  if (!loaded_ || output == nullptr || capacity == 0 || sectionIndex < 0 || sectionIndex >= metadata_.sectionCount) {
    return false;
  }
  const int written = std::snprintf(output, capacity, "%s/gen_%lu/sections/%06d.xhtml", cacheRoot_.c_str(),
                                    static_cast<unsigned long>(manifest_.generation), sectionIndex);
  return written > 0 && static_cast<size_t>(written) < capacity;
}

ReflowSectionInfo PdfReflowDocument::getSectionInfo(const int sectionIndex) const {
  if (!loaded_ || !sections_ || sectionIndex < 0 || sectionIndex >= metadata_.sectionCount) {
    return {};
  }
  const PdfMetadataSection& source = sections_[sectionIndex];
  ReflowSectionInfo section;
  char href[32]{};
  if (!formatSectionHref(sectionIndex, href, sizeof(href))) {
    return {};
  }
  section.href = href;
  section.byteSize = source.byteSize;
  section.cumulativeSize = source.cumulativeSize;
  section.firstWordOrdinal = source.firstWordOrdinal;
  section.wordCount = source.wordCount;
  section.tocIndex = source.tocIndex;
  if (source.tocIndex >= 0) {
    section.title = getTocEntry(source.tocIndex).title;
  }
  if (section.title.empty() && sectionIndex == 0) {
    section.title = title_;
  }
  return section;
}

bool PdfReflowDocument::getSectionSize(const int sectionIndex, size_t* const size) const {
  if (!loaded_ || !sections_ || sectionIndex < 0 || sectionIndex >= metadata_.sectionCount || size == nullptr) {
    return false;
  }
  *size = sections_[sectionIndex].byteSize;
  return true;
}

size_t PdfReflowDocument::getCumulativeSectionSize(const int sectionIndex) const {
  return loaded_ && sections_ && sectionIndex >= 0 && sectionIndex < metadata_.sectionCount
             ? sections_[sectionIndex].cumulativeSize
             : 0;
}

size_t PdfReflowDocument::getDocumentSize() const {
  return loaded_ && metadata_.sectionCount != 0 ? getCumulativeSectionSize(metadata_.sectionCount - 1) : 0;
}
int PdfReflowDocument::getSectionIndexForTextReference() const { return loaded_ ? 0 : -1; }
int PdfReflowDocument::getTocEntryCount() const { return loaded_ ? metadata_.outlineCount : 0; }

bool PdfReflowDocument::readOutlineEntry(const int tocIndex, PdfOutlineEntry* const entry) const {
  if (!loaded_ || entry == nullptr || tocIndex < 0 || tocIndex >= metadata_.outlineCount || outlinePath_.empty()) {
    return false;
  }
  if (cachedOutlineIndex_ == tocIndex) {
    *entry = cachedOutlineEntry_;
    return true;
  }
  PdfCacheHandle handle{};
  PdfStatus status = io_.open(io_.context, outlinePath_.c_str(), PdfCacheOpenMode::Read, &handle);
  if (!status) {
    return false;
  }
  ManifestSource sourceContext{&io_, handle, outlineRecord_.size};
  cachedOutlineIndex_ = -1;
  status = pdfReadOutlineEntry(sourceContext.source(), static_cast<uint16_t>(tocIndex), &cachedOutlineEntry_);
  status = closePreservingStatus(io_, &handle, status);
  if (!status || cachedOutlineEntry_.sectionIndex >= metadata_.sectionCount) {
    return false;
  }
  cachedOutlineIndex_ = tocIndex;
  *entry = cachedOutlineEntry_;
  return true;
}

ReflowTocEntry PdfReflowDocument::getTocEntry(const int tocIndex) const {
  if (!readOutlineEntry(tocIndex, &cachedOutlineEntry_)) {
    return {};
  }
  const PdfOutlineEntry& source = cachedOutlineEntry_;
  char href[32]{};
  size_t hrefLength = 0;
  const PdfStatus status = pdfResolveInternalAction(
      PdfActionKind::GoTo, {source.sectionIndex, source.anchorOrdinal, source.sourcePageIndex, true}, href,
      sizeof(href), &hrefLength);
  if (!status || hrefLength == 0) {
    return {};
  }
  return {
      .title = std::string(source.title, source.titleLength),
      .href = href,
      .anchor = source.anchor,
      .level = source.level,
      .sectionIndex = source.sectionIndex,
      .parentIndex = source.parentIndex,
  };
}

int PdfReflowDocument::getSectionIndexForTocIndex(const int tocIndex) const {
  PdfOutlineEntry entry{};
  return readOutlineEntry(tocIndex, &entry) ? entry.sectionIndex : -1;
}

int PdfReflowDocument::getTocIndexForSectionIndex(const int sectionIndex) const {
  return loaded_ && sections_ && sectionIndex >= 0 && sectionIndex < metadata_.sectionCount
             ? sections_[sectionIndex].tocIndex
             : -1;
}

int PdfReflowDocument::resolveHrefToSectionIndex(const std::string& href) const {
  if (!loaded_) {
    return -1;
  }
  const size_t fragment = href.find('#');
  const std::string path = href.substr(0, fragment);
  if (path.empty()) {
    return 0;
  }
  static constexpr char prefix[] = "sections/";
  static constexpr char suffix[] = ".xhtml";
  if (path.size() == sizeof(prefix) - 1 + 6 + sizeof(suffix) - 1 && path.compare(0, sizeof(prefix) - 1, prefix) == 0 &&
      path.compare(path.size() - (sizeof(suffix) - 1), sizeof(suffix) - 1, suffix) == 0) {
    uint32_t sectionIndex = 0;
    if (parseDecimal(path.data() + sizeof(prefix) - 1, 6, &sectionIndex) && sectionIndex < metadata_.sectionCount) {
      return static_cast<int>(sectionIndex);
    }
  }
  return -1;
}

float PdfReflowDocument::calculateSizeProgress(const int sectionIndex, const float sectionProgress) const {
  if (!loaded_ || !sections_ || sectionIndex < 0 || sectionIndex >= metadata_.sectionCount) {
    return 0.0F;
  }
  const size_t documentSize = getDocumentSize();
  if (documentSize == 0) {
    return 0.0F;
  }
  const uint32_t prior = sectionIndex == 0 ? 0 : sections_[sectionIndex - 1].cumulativeSize;
  const float position =
      static_cast<float>(prior) + static_cast<float>(sections_[sectionIndex].byteSize) * clampUnit(sectionProgress);
  return clampUnit(position / static_cast<float>(documentSize));
}

float PdfReflowDocument::calculateProgress(const int sectionIndex, const float sectionProgress) const {
  if (!loaded_ || !sections_ || sectionIndex < 0 || sectionIndex >= metadata_.sectionCount ||
      metadata_.totalWords == 0) {
    return 0.0F;
  }
  const uint32_t priorWords = sections_[sectionIndex].firstWordOrdinal;
  const float reached = static_cast<float>(priorWords) +
                        static_cast<float>(sections_[sectionIndex].wordCount) * clampUnit(sectionProgress);
  return clampUnit(reached / static_cast<float>(metadata_.totalWords));
}

bool PdfReflowDocument::resolveProgressPercentToSection(const int percent, int& sectionIndex,
                                                        float& sectionProgress) const {
  if (!loaded_) {
    return false;
  }
  const float target =
      static_cast<float>(metadata_.totalWords) * (static_cast<float>(std::clamp(percent, 0, 100)) / 100.0F);
  sectionIndex = metadata_.sectionCount - 1;
  for (uint16_t index = 0; index < metadata_.sectionCount; ++index) {
    const uint64_t sectionEnd =
        static_cast<uint64_t>(sections_[index].firstWordOrdinal) + static_cast<uint64_t>(sections_[index].wordCount);
    if (target <= static_cast<float>(sectionEnd)) {
      sectionIndex = index;
      break;
    }
  }
  const uint32_t prior = sections_[sectionIndex].firstWordOrdinal;
  const uint32_t words = sections_[sectionIndex].wordCount;
  sectionProgress = words == 0 ? 0.0F : clampUnit((target - static_cast<float>(prior)) / static_cast<float>(words));
  return true;
}

bool PdfReflowDocument::resolveReferencePage(int, float, uint32_t&, uint32_t&) const { return false; }
uint32_t PdfReflowDocument::getTotalWordCount() const { return loaded_ ? manifest_.totalWords : 0; }
bool PdfReflowDocument::loadReadingPosition(ReflowReadingPosition& position) const {
  if (!loaded_) {
    return false;
  }
  return progressStore_.load(&position).ok();
}

bool PdfReflowDocument::saveReadingPosition(const ReflowReadingPosition& position) const {
  if (!loaded_) {
    return false;
  }
  return progressStore_.save(position).ok();
}

PdfStatus PdfReflowDocument::loadPdfSavedItems(PdfSavedItemsBuffer* const output) const {
  return savedItemsReady_ ? savedItemsStore_.load(output) : PdfStatus::failure(PdfError::InvalidArgument);
}

PdfStatus PdfReflowDocument::savePdfSavedItems(const PdfSavedItem* const items, const uint16_t count) const {
  return savedItemsReady_ ? savedItemsStore_.save(items, count) : PdfStatus::failure(PdfError::InvalidArgument);
}

PdfStatus PdfReflowDocument::validatePdfSavedItem(const PdfSavedItem& item) const {
  return savedItemsReady_ ? savedItemsStore_.validate(item) : PdfStatus::failure(PdfError::InvalidArgument);
}

PdfStatus PdfReflowDocument::SavedItemMapSource::inspect(void* const context, const uint16_t sectionIndex,
                                                        PdfLayoutWordIndexInfo* const info) {
  if (context == nullptr || info == nullptr) return PdfStatus::failure(PdfError::InvalidArgument);
  auto& source = *static_cast<SavedItemMapSource*>(context);
  if (source.document == nullptr || source.sectionCachePath == nullptr) {
    return PdfStatus::failure(PdfError::InvalidArgument);
  }
  if (!source.inspected) {
    PdfStatus status = pdfInspectLayoutWordIndex(source.file.source(), &source.info);
    if (!status) {
      return status;
    }
    source.inspected = true;
  }
  if (source.info.sectionIndex != sectionIndex) return PdfStatus::failure(PdfError::Malformed);
  *info = source.info;
  return PdfStatus::success();
}

PdfStatus PdfReflowDocument::SavedItemMapSource::readRanges(void* const context, const uint16_t sectionIndex,
                                                           const uint16_t firstPage, const uint16_t count,
                                                           PdfLayoutWordRange* const ranges) {
  if (context == nullptr || ranges == nullptr) return PdfStatus::failure(PdfError::InvalidArgument);
  auto& source = *static_cast<SavedItemMapSource*>(context);
  if (!source.inspected || source.info.sectionIndex != sectionIndex || firstPage >= source.info.pageCount ||
      count == 0 || count > static_cast<uint16_t>(source.info.pageCount - firstPage)) {
    return PdfStatus::failure(PdfError::InvalidOffset, firstPage);
  }
  return pdfReadValidatedLayoutWordRanges(source.file.source(), source.info, firstPage, count, ranges);
}

PdfStatus PdfReflowDocument::mapPdfSavedItem(const std::string& sectionCachePath,
                                             const uint32_t layoutFingerprint, const PdfSavedItem& item,
                                             PdfSavedItemPageRange* const pages) const {
  if (!savedItemsReady_ || sectionCachePath.empty()) return PdfStatus::failure(PdfError::InvalidArgument);
  if (!formatLayoutWordIndexPath(sectionCachePath, layoutWordIndexPath_.data())) {
    return PdfStatus::failure(PdfError::LimitExceeded);
  }
  SavedItemMapSource context;
  context.document = this;
  context.sectionCachePath = &sectionCachePath;
  if (!openLayoutWordIndex(layoutWordIndexPath_.data(), context.file)) {
    return PdfStatus::failure(PdfError::IoFailure);
  }
  const PdfSavedItemWordIndexSource source{
      &context,
      SavedItemMapSource::inspect,
      SavedItemMapSource::readRanges,
      layoutFingerprint,
  };
  PdfStatus status = pdfMapSavedItemWordRange(source, item, pages);
  if (!closeLayoutWordIndex(context.file) && status) status = PdfStatus::failure(PdfError::IoFailure);
  return status;
}

PdfStatus PdfReflowDocument::pdfSavedItemsLayoutFingerprint(const std::string& sectionCachePath,
                                                            uint32_t* const fingerprint) const {
  if (!savedItemsReady_ || sectionCachePath.empty() || fingerprint == nullptr) {
    return PdfStatus::failure(PdfError::InvalidArgument);
  }
  *fingerprint = 0;
  if (!formatLayoutWordIndexPath(sectionCachePath, layoutWordIndexPath_.data())) {
    return PdfStatus::failure(PdfError::LimitExceeded);
  }
  ManifestSource source;
  if (!openLayoutWordIndex(layoutWordIndexPath_.data(), source)) return PdfStatus::failure(PdfError::IoFailure);

  // Do not CRC the complete sidecar: each fixed-size record carries its own CRC,
  // and a payload plus its updated CRC has a fixed CRC residue. FNV-1a still
  // distinguishes same-size relayouts while keeping this cold-path scan bounded.
  constexpr uint32_t kFnv32Offset = 2166136261U;
  constexpr uint32_t kFnv32Prime = 16777619U;
  uint8_t workspace[64];
  uint64_t offset = 0;
  uint32_t hash = kFnv32Offset;
  PdfStatus status = PdfStatus::success();
  while (offset < source.size) {
    const size_t requested = static_cast<size_t>(std::min<uint64_t>(sizeof(workspace), source.size - offset));
    size_t bytesRead = 0;
    status = io_.read(io_.context, source.handle, offset, workspace, requested, &bytesRead);
    if (!status || bytesRead != requested) {
      if (status) status = PdfStatus::failure(PdfError::UnexpectedEof, offset + bytesRead);
      break;
    }
    for (size_t index = 0; index < bytesRead; ++index) {
      hash = (hash ^ workspace[index]) * kFnv32Prime;
    }
    offset += bytesRead;
  }
  if (!closeLayoutWordIndex(source) && status) status = PdfStatus::failure(PdfError::IoFailure);
  if (status) *fingerprint = hash == 0 ? 1U : hash;
  return status;
}

bool PdfReflowDocument::formatLayoutWordIndexPath(const std::string& sectionCachePath,
                                                  char destination[PDF_CACHE_PATH_CAPACITY]) const {
  const int length = std::snprintf(destination, PDF_CACHE_PATH_CAPACITY, "%s.pwi", sectionCachePath.c_str());
  return length > 0 && static_cast<size_t>(length) < PDF_CACHE_PATH_CAPACITY;
}

bool PdfReflowDocument::openLayoutWordIndex(const char* const path, ManifestSource& source) const {
  source = {};
  source.io = &io_;
  PdfStatus status = io_.open(io_.context, path, PdfCacheOpenMode::Read, &source.handle);
  PdfCacheFileMetadata metadata;
  if (status) {
    status = io_.metadata(io_.context, source.handle, &metadata);
  }
  if (!status || metadata.directory || metadata.symlinkLike) {
    closeLayoutWordIndex(source);
    return false;
  }
  source.size = metadata.size;
  return true;
}

bool PdfReflowDocument::closeLayoutWordIndex(ManifestSource& source) const {
  PdfStatus status = PdfStatus::success();
  if (source.handle.valid()) {
    status = io_.close(io_.context, &source.handle);
  }
  source = {};
  return status.ok();
}

bool PdfReflowDocument::validateLayoutWordIndex(const std::string& sectionCachePath, const int sectionIndex,
                                                const uint16_t pageCount) const {
  layoutWordIndexAvailable_ = false;
  layoutWordWindowCount_ = 0;
  char path[PDF_CACHE_PATH_CAPACITY]{};
  if (!loaded_ || !sections_ || sectionIndex < 0 || sectionIndex >= metadata_.sectionCount ||
      !formatLayoutWordIndexPath(sectionCachePath, path)) {
    return false;
  }
  ManifestSource source;
  if (!openLayoutWordIndex(path, source)) {
    return false;
  }
  PdfLayoutWordIndexInfo info;
  const PdfStatus status = pdfInspectLayoutWordIndex(source.source(), &info);
  const bool closed = closeLayoutWordIndex(source);
  const auto& section = sections_[sectionIndex];
  if (!status || !closed || info.sectionIndex != sectionIndex || info.pageCount != pageCount ||
      info.firstGlobalWordOrdinal != section.firstWordOrdinal || info.sectionWordCount != section.wordCount) {
    return false;
  }
  std::memcpy(layoutWordIndexPath_.data(), path, sizeof(path));
  layoutWordIndexAvailable_ = true;
  return true;
}

bool PdfReflowDocument::removeLayoutWordIndex(const std::string& sectionCachePath) const {
  char path[PDF_CACHE_PATH_CAPACITY]{};
  if (!formatLayoutWordIndexPath(sectionCachePath, path)) {
    return false;
  }
  const PdfStatus status = io_.remove(io_.context, path, false);
  if (!status && status.error != PdfError::InvalidOffset) {
    return false;
  }
  if (std::strcmp(layoutWordIndexPath_.data(), path) == 0) {
    layoutWordIndexAvailable_ = false;
    layoutWordWindowCount_ = 0;
    layoutWordIndexPath_.fill('\0');
  }
  return true;
}

bool PdfReflowDocument::readLayoutWordRange(const std::string& sectionCachePath, const uint16_t pageCount,
                                            const uint16_t page, ReflowPageSemanticRange& range) const {
  range = {};
  char path[PDF_CACHE_PATH_CAPACITY]{};
  if (!layoutWordIndexAvailable_ || page >= pageCount || !formatLayoutWordIndexPath(sectionCachePath, path) ||
      std::strcmp(layoutWordIndexPath_.data(), path) != 0) {
    return false;
  }
  if (layoutWordWindowCount_ == 0 || page < layoutWordWindowStart_ ||
      page >= static_cast<uint16_t>(layoutWordWindowStart_ + layoutWordWindowCount_)) {
    constexpr uint16_t windowPages = 4;
    const uint16_t windowStart = static_cast<uint16_t>(page - page % windowPages);
    const uint16_t windowCount = std::min<uint16_t>(windowPages, pageCount - windowStart);
    // The decode target is shared scratch. Invalidate its published metadata
    // before any refill can partially overwrite it.
    layoutWordWindowCount_ = 0;
    ManifestSource source;
    if (!openLayoutWordIndex(path, source)) {
      return false;
    }
    const PdfStatus status =
        pdfReadLayoutWordRanges(source.source(), windowStart, windowCount, layoutWordWindow_.data());
    const bool closed = closeLayoutWordIndex(source);
    if (!status || !closed) {
      return false;
    }
    layoutWordWindowStart_ = windowStart;
    layoutWordWindowCount_ = static_cast<uint8_t>(windowCount);
  }
  const auto& decoded = layoutWordWindow_[page - layoutWordWindowStart_];
  range.firstGlobalWordOrdinal = decoded.firstGlobalWordOrdinal;
  range.lastGlobalWordOrdinal = decoded.lastGlobalWordOrdinal;
  range.firstBlockWordOffset = decoded.firstBlockWordOffset;
  range.wordCursor = decoded.wordCursor;
  range.valid = decoded.valid;
  std::memcpy(range.blockAnchor, decoded.blockAnchor, sizeof(range.blockAnchor));
  return true;
}

bool PdfReflowDocument::findLayoutWordPage(const std::string& sectionCachePath, const char* const blockAnchor,
                                           const uint32_t blockWordOffset, const uint32_t globalWordOrdinal,
                                           uint16_t& page) const {
  char path[PDF_CACHE_PATH_CAPACITY]{};
  if (!layoutWordIndexAvailable_ || !formatLayoutWordIndexPath(sectionCachePath, path) ||
      std::strcmp(layoutWordIndexPath_.data(), path) != 0) {
    return false;
  }
  ManifestSource source;
  if (!openLayoutWordIndex(path, source)) {
    return false;
  }
  PdfStatus status = PdfStatus::failure(PdfError::InvalidOffset);
  if (blockAnchor && blockAnchor[0] != '\0') {
    PdfLayoutWordRange anchorRange;
    status = pdfFindLayoutAnchor(source.source(), blockAnchor, blockWordOffset, &page, &anchorRange);
    if (status && (!anchorRange.valid || globalWordOrdinal < anchorRange.firstGlobalWordOrdinal ||
                   globalWordOrdinal > anchorRange.lastGlobalWordOrdinal)) {
      status = PdfStatus::failure(PdfError::InvalidOffset, globalWordOrdinal);
    }
  }
  if (!status) {
    status = pdfFindLayoutPage(source.source(), globalWordOrdinal, &page);
  }
  const bool closed = closeLayoutWordIndex(source);
  return status.ok() && closed;
}

bool PdfReflowDocument::findLayoutWordCursor(const std::string& sectionCachePath, const uint32_t wordCursor,
                                             uint16_t& page) const {
  char path[PDF_CACHE_PATH_CAPACITY]{};
  if (!layoutWordIndexAvailable_ || !formatLayoutWordIndexPath(sectionCachePath, path) ||
      std::strcmp(layoutWordIndexPath_.data(), path) != 0) {
    return false;
  }
  ManifestSource source;
  if (!openLayoutWordIndex(path, source)) {
    return false;
  }
  const PdfStatus status = pdfFindLayoutCursor(source.source(), wordCursor, &page);
  const bool closed = closeLayoutWordIndex(source);
  return status.ok() && closed;
}

bool PdfReflowDocument::getLocalSectionPath(const int sectionIndex, ReflowResource& out) const {
  char path[PDF_CACHE_PATH_CAPACITY]{};
  if (!formatSectionPath(sectionIndex, path, sizeof(path))) {
    out = ReflowResource::streamed();
    return false;
  }
  out = ReflowResource::borrowedLocalFile(path, ReflowImageKind::EncodedImage);
  return true;
}

bool PdfReflowDocument::streamCachedFile(const std::string& path, const uint64_t fileSize, Print& out,
                                         size_t chunkSize) const {
  if (!loaded_ || !ioWorkspace_ || path.empty()) {
    return false;
  }
  chunkSize = std::clamp<size_t>(chunkSize, 1, PDF_SOURCE_FINGERPRINT_BYTES);
  PdfCacheHandle handle{};
  PdfStatus status = io_.open(io_.context, path.c_str(), PdfCacheOpenMode::Read, &handle);
  uint64_t offset = 0;
  while (status && offset < fileSize) {
    const size_t requested = static_cast<size_t>(std::min<uint64_t>(chunkSize, fileSize - offset));
    size_t bytesRead = 0;
    status = io_.read(io_.context, handle, offset, ioWorkspace_.get(), requested, &bytesRead);
    if (!status || bytesRead != requested || out.write(ioWorkspace_.get(), bytesRead) != bytesRead) {
      status = status && bytesRead == requested ? PdfStatus::failure(PdfError::IoFailure)
                                                : (status ? PdfStatus::failure(PdfError::UnexpectedEof) : status);
      break;
    }
    offset += bytesRead;
  }
  status = closePreservingStatus(io_, &handle, status);
  return status.ok();
}

bool PdfReflowDocument::streamSection(const int sectionIndex, Print& out, const size_t chunkSize) const {
  char path[PDF_CACHE_PATH_CAPACITY]{};
  return loaded_ && sections_ && sectionIndex >= 0 && sectionIndex < metadata_.sectionCount &&
         formatSectionPath(sectionIndex, path, sizeof(path)) &&
         streamCachedFile(path, sections_[sectionIndex].byteSize, out, chunkSize);
}

bool PdfReflowDocument::formatResourcePath(const PdfCachedResourceRecord& resource, char* const output,
                                           const size_t capacity) const {
  if (output == nullptr || capacity == 0 || cacheRoot_.empty() || resource.kind == PdfCachedResourceKind::None) {
    return false;
  }
  const uint32_t generation = loaded_ ? manifest_.generation : validationGeneration_;
  int written = -1;
  if (resource.kind == PdfCachedResourceKind::PixelCache) {
    written = std::snprintf(output, capacity, "%s/gen_%lu/images/%016llx-%08lx.pxc", cacheRoot_.c_str(),
                            static_cast<unsigned long>(generation),
                            static_cast<unsigned long long>(resource.contentHash),
                            static_cast<unsigned long>(resource.nameCrc32));
  } else if (resource.kind == PdfCachedResourceKind::Jpeg) {
    written = std::snprintf(output, capacity, "%s/gen_%lu/images/%016llx-%08lx-%016llx.jpg", cacheRoot_.c_str(),
                            static_cast<unsigned long>(generation),
                            static_cast<unsigned long long>(resource.contentHash),
                            static_cast<unsigned long>(resource.nameCrc32),
                            static_cast<unsigned long long>(resource.encodedLength));
  }
  return written > 0 && static_cast<size_t>(written) < capacity;
}

const PdfCachedResourceRecord* PdfReflowDocument::findResource(const int sectionIndex, const std::string& href) const {
  if (!loaded_ || sectionIndex < 0 || sectionIndex >= metadata_.sectionCount || href.empty()) {
    return nullptr;
  }
  char prefix[PDF_CACHE_PATH_CAPACITY]{};
  const int prefixLength = std::snprintf(prefix, sizeof(prefix), "%s/gen_%lu/images/", cacheRoot_.c_str(),
                                         static_cast<unsigned long>(manifest_.generation));
  if (prefixLength <= 0 || static_cast<size_t>(prefixLength) >= sizeof(prefix) ||
      href.size() <= static_cast<size_t>(prefixLength) ||
      std::memcmp(href.data(), prefix, static_cast<size_t>(prefixLength)) != 0) {
    return nullptr;
  }
  PdfCachedResourceRecord requested{};
  if (!parseImageLeaf(href.data() + prefixLength, href.size() - static_cast<size_t>(prefixLength), &requested)) {
    return nullptr;
  }
  for (uint8_t index = 0; index < resourceCount_; ++index) {
    if (sameResourceIdentity(resources_[index], requested)) {
      return &resources_[index];
    }
  }
  return nullptr;
}

void PdfReflowDocument::fitResourceDimensions(const uint16_t sourceWidth, const uint16_t sourceHeight,
                                              uint16_t* const width, uint16_t* const height) {
  if (width == nullptr || height == nullptr || sourceWidth == 0 || sourceHeight == 0) {
    return;
  }
  constexpr uint32_t maximum = static_cast<uint32_t>(std::numeric_limits<int16_t>::max());
  const uint32_t largest = std::max<uint32_t>(sourceWidth, sourceHeight);
  if (largest <= maximum) {
    *width = sourceWidth;
    *height = sourceHeight;
    return;
  }
  *width = static_cast<uint16_t>(
      std::max<uint64_t>(1U, (static_cast<uint64_t>(sourceWidth) * maximum) / largest));
  *height = static_cast<uint16_t>(
      std::max<uint64_t>(1U, (static_cast<uint64_t>(sourceHeight) * maximum) / largest));
}

bool PdfReflowDocument::resolveResource(const int sectionIndex, const std::string& href, ReflowResource& out) const {
  const PdfCachedResourceRecord* const resource = findResource(sectionIndex, href);
  if (resource == nullptr) {
    out = ReflowResource::streamed();
    return false;
  }
  uint16_t width = 0;
  uint16_t height = 0;
  fitResourceDimensions(resource->width, resource->height, &width, &height);
  if (width == 0 || height == 0) {
    out = ReflowResource::streamed();
    return false;
  }
  const ReflowImageKind kind = resource->kind == PdfCachedResourceKind::PixelCache
                                   ? ReflowImageKind::PixelCache
                                   : ReflowImageKind::EncodedImage;
  out = ReflowResource::borrowedLocalFile(href, kind, width, height);
  return true;
}

bool PdfReflowDocument::streamResource(const int sectionIndex, const std::string& href, Print& out,
                                       const size_t chunkSize) const {
  const PdfCachedResourceRecord* const resource = findResource(sectionIndex, href);
  return resource != nullptr && streamCachedFile(href, resource->fileSize, out, chunkSize);
}

bool PdfReflowDocument::getResourceSize(const int sectionIndex, const std::string& href, size_t* const size) const {
  const PdfCachedResourceRecord* const resource = findResource(sectionIndex, href);
  if (resource == nullptr || size == nullptr || resource->fileSize > std::numeric_limits<size_t>::max()) {
    return false;
  }
  *size = static_cast<size_t>(resource->fileSize);
  return true;
}
