#include "PdfReflowDocument.h"

#include <Print.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <limits>

#include "Memory.h"
#include "PdfLayoutWordIndex.h"
#include "PdfProgressStore.h"
#include "PdfSourceIdentity.h"

namespace {

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
                                        const char* const cacheDirectory) {
  resetLoadedState();
  if (!io.valid() || sourcePath == nullptr || sourcePath[0] == '\0' || cacheDirectory == nullptr) {
    status_ = PdfStatus::failure(PdfError::InvalidArgument);
    return status_;
  }
  io_ = io;
  sourcePath_ = sourcePath;
  char root[PDF_CACHE_PATH_CAPACITY]{};
  status_ = pdfFormatCacheRoot(cacheDirectory, sourcePath, root, sizeof(root));
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
  metadataRecord_ = {};
  outlineRecord_ = {};
  progressStore_ = {};
  savedItemsStore_ = {};
  savedItemsReady_ = false;
  metadataPath_.clear();
  outlinePath_.clear();
  sections_.reset();
  manifestSectionSizes_.fill(0);
  manifestSectionSeen_.fill(0);
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
  metadataFilesSeen_ = 0;
  outlineFilesSeen_ = 0;
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

PdfStatus PdfReflowDocument::validateRequiredFile(void* context, const PdfRequiredFileRecord& record) {
  return context == nullptr ? PdfStatus::failure(PdfError::InvalidArgument)
                            : static_cast<PdfReflowDocument*>(context)->validateFile(record);
}

PdfStatus PdfReflowDocument::validateFile(const PdfRequiredFileRecord& record) {
  ++requiredFilesSeen_;
  char path[PDF_CACHE_PATH_CAPACITY]{};
  const int length = std::snprintf(path, sizeof(path), "%s/%s", cacheRoot_.c_str(), record.path);
  if (length < 0 || static_cast<size_t>(length) >= sizeof(path)) {
    return PdfStatus::failure(PdfError::LimitExceeded);
  }

  PdfCacheHandle handle{};
  PdfStatus status = io_.open(io_.context, path, PdfCacheOpenMode::Read, &handle);
  if (!status) {
    return status;
  }
  PdfCacheFileMetadata metadata{};
  status = io_.metadata(io_.context, handle, &metadata);
  if (status && (metadata.directory || metadata.symlinkLike || metadata.size != record.size)) {
    status = PdfStatus::failure(PdfError::Malformed);
  }

  uint64_t offset = 0;
  uint32_t crc = 0;
  while (status && offset < record.size) {
    const size_t requested =
        static_cast<size_t>(std::min<uint64_t>(PDF_SOURCE_FINGERPRINT_BYTES, record.size - offset));
    size_t bytesRead = 0;
    status = io_.read(io_.context, handle, offset, ioWorkspace_.get(), requested, &bytesRead);
    if (!status) {
      break;
    }
    if (bytesRead != requested) {
      status = PdfStatus::failure(PdfError::UnexpectedEof, offset + bytesRead);
      break;
    }
    crc = pdfCacheCrc32(ioWorkspace_.get(), bytesRead, crc);
    offset += bytesRead;
  }
  if (status && crc != record.crc32) {
    status = PdfStatus::failure(PdfError::Malformed);
  }
  status = closePreservingStatus(io_, &handle, status);
  if (!status) {
    return status;
  }

  char expected[PDF_CACHE_REQUIRED_PATH_CAPACITY]{};
  const int metadataLength = std::snprintf(expected, sizeof(expected), "gen_%lu/metadata.bin",
                                           static_cast<unsigned long>(validationGeneration_));
  if (metadataLength > 0 && static_cast<size_t>(metadataLength) < sizeof(expected) &&
      recordPathEquals(record, expected)) {
    ++metadataFilesSeen_;
    if (metadataFilesSeen_ != 1) {
      return PdfStatus::failure(PdfError::Malformed);
    }
    metadataPath_ = path;
    metadataRecord_ = record;
    return PdfStatus::success();
  }

  const int outlineLength = std::snprintf(expected, sizeof(expected), "gen_%lu/outline.bin",
                                          static_cast<unsigned long>(validationGeneration_));
  if (outlineLength > 0 && static_cast<size_t>(outlineLength) < sizeof(expected) &&
      recordPathEquals(record, expected)) {
    ++outlineFilesSeen_;
    if (outlineFilesSeen_ != 1) {
      return PdfStatus::failure(PdfError::Malformed);
    }
    outlinePath_ = path;
    outlineRecord_ = record;
    return PdfStatus::success();
  }

  uint32_t generation = 0;
  uint16_t sectionIndex = 0;
  if (parseSectionRecordPath(record, &generation, &sectionIndex)) {
    if (generation != validationGeneration_ || record.size > UINT32_MAX) {
      return PdfStatus::failure(PdfError::Malformed);
    }
    const size_t byteIndex = sectionIndex / 8U;
    const uint8_t mask = static_cast<uint8_t>(1U << (sectionIndex & 7U));
    if ((manifestSectionSeen_[byteIndex] & mask) != 0) {
      return PdfStatus::failure(PdfError::Malformed, sectionIndex);
    }
    manifestSectionSeen_[byteIndex] |= mask;
    manifestSectionSizes_[sectionIndex] = static_cast<uint32_t>(record.size);
    ++xhtmlFilesSeen_;
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

PdfStatus PdfReflowDocument::loadMetadataCache() {
  if (metadataPath_.empty() || metadataRecord_.size == 0) {
    return PdfStatus::failure(PdfError::Malformed);
  }
  PdfCacheHandle handle{};
  PdfStatus status = io_.open(io_.context, metadataPath_.c_str(), PdfCacheOpenMode::Read, &handle);
  if (!status) {
    return status;
  }
  ManifestSource sourceContext{&io_, handle, metadataRecord_.size};
  PdfMetadata inspected{};
  status = pdfInspectMetadata(sourceContext.source(), &inspected);
  if (status) {
    sections_ = makeUniqueNoThrow<PdfMetadataSection[]>(inspected.sectionCount);
    if (!sections_) {
      status = PdfStatus::failure(PdfError::InsufficientMemory);
    }
  }
  if (status) {
    metadata_ = inspected;
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

  PdfCacheStore store;
  status_ = store.initialize(io_, cacheRoot_.c_str());
  PdfCacheManifestSelection selection{};
  if (status_) {
    status_ = store.loadManifestSlots(sourceIdentity_, &selection);
  }
  if (!status_ || !selection.selected || !selection.manifest.completed) {
    if (status_ && (!selection.selected || !selection.manifest.completed)) {
      status_ = PdfStatus::failure(PdfError::InvalidOffset);
    }
    return status_;
  }

  const char* manifestName = selection.selectedSlot == PdfCacheSlot::A ? "manifest.a" : "manifest.b";
  char manifestPath[PDF_CACHE_PATH_CAPACITY]{};
  const int pathLength = std::snprintf(manifestPath, sizeof(manifestPath), "%s/%s", cacheRoot_.c_str(), manifestName);
  if (pathLength < 0 || static_cast<size_t>(pathLength) >= sizeof(manifestPath)) {
    status_ = PdfStatus::failure(PdfError::LimitExceeded);
    return status_;
  }

  PdfCacheHandle handle{};
  status_ = io_.open(io_.context, manifestPath, PdfCacheOpenMode::Read, &handle);
  if (!status_) {
    return status_;
  }
  PdfCacheFileMetadata metadata{};
  status_ = io_.metadata(io_.context, handle, &metadata);
  if (status_ && (metadata.directory || metadata.symlinkLike)) {
    status_ = PdfStatus::failure(PdfError::Malformed);
  }
  PdfCacheManifest decoded{};
  if (status_) {
    validationGeneration_ = selection.manifest.generation;
    ManifestSource source{&io_, handle, metadata.size};
    status_ = pdfDecodeCacheManifest(source.source(), &decoded, {this, validateRequiredFile});
  }
  status_ = closePreservingStatus(io_, &handle, status_);
  if (!status_) {
    resetLoadedState();
    return status_;
  }
  if (!decoded.completed || !pdfSourceIdentityEqual(decoded.source, sourceIdentity_) ||
      decoded.generation != selection.manifest.generation || decoded.sequence != selection.manifest.sequence ||
      requiredFilesSeen_ != decoded.requiredFileCount || xhtmlFilesSeen_ == 0 || metadataFilesSeen_ != 1 ||
      outlineFilesSeen_ != 1 || metadataPath_.empty() || outlinePath_.empty()) {
    resetLoadedState();
    status_ = PdfStatus::failure(PdfError::Malformed);
    return status_;
  }

  manifest_ = decoded;
  status_ = loadMetadataCache();
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

std::string PdfReflowDocument::getCoverBmpPath(bool) const { return {}; }
bool PdfReflowDocument::generateCoverBmp(bool, const GfxRenderer*, int) const { return false; }
std::string PdfReflowDocument::getThumbBmpPath() const { return {}; }
std::string PdfReflowDocument::getThumbBmpPath(int, int) const { return {}; }
std::string PdfReflowDocument::getAdaptiveThumbBmpPath(int, int) const { return {}; }
bool PdfReflowDocument::generateThumbBmp(int, int, const GfxRenderer*, int) const { return false; }
bool PdfReflowDocument::generateAdaptiveThumbBmp(int, int, const GfxRenderer*, int) const { return false; }

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
  char href[PdfOutlineLimits::HrefBytes]{};
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
  PdfOutlineEntry decoded{};
  status = pdfReadOutlineEntry(sourceContext.source(), static_cast<uint16_t>(tocIndex), &decoded);
  status = closePreservingStatus(io_, &handle, status);
  if (!status || decoded.sectionIndex >= metadata_.sectionCount) {
    return false;
  }
  cachedOutlineEntry_ = decoded;
  cachedOutlineIndex_ = tocIndex;
  *entry = decoded;
  return true;
}

ReflowTocEntry PdfReflowDocument::getTocEntry(const int tocIndex) const {
  PdfOutlineEntry source{};
  if (!readOutlineEntry(tocIndex, &source)) {
    return {};
  }
  char href[PdfOutlineLimits::HrefBytes]{};
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
  char path[PDF_CACHE_PATH_CAPACITY]{};
  if (!formatLayoutWordIndexPath(sectionCachePath, path)) {
    return PdfStatus::failure(PdfError::LimitExceeded);
  }
  ManifestSource source;
  if (!openLayoutWordIndex(path, source)) return PdfStatus::failure(PdfError::IoFailure);

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

bool PdfReflowDocument::resolveResource(int, const std::string&, ReflowResource& out) const {
  out = ReflowResource::streamed();
  return false;
}

bool PdfReflowDocument::streamResource(int, const std::string&, Print&, size_t) const { return false; }
bool PdfReflowDocument::getResourceSize(int, const std::string&, size_t*) const { return false; }
