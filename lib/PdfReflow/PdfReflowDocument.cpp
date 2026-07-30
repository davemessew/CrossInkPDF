#include "PdfReflowDocument.h"

#include <Print.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <limits>

#include "Memory.h"
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
  sectionRecord_ = {};
  sectionPath_.clear();
  requiredFilesSeen_ = 0;
  xhtmlFilesSeen_ = 0;
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

  if (endsWith(record.path, record.pathLength, ".xhtml")) {
    ++xhtmlFilesSeen_;
    if (xhtmlFilesSeen_ != 1) {
      return PdfStatus::failure(PdfError::Unsupported);
    }
    sectionPath_ = path;
    sectionRecord_ = record;
  }
  return PdfStatus::success();
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
      requiredFilesSeen_ != decoded.requiredFileCount || xhtmlFilesSeen_ != 1 || sectionPath_.empty()) {
    resetLoadedState();
    status_ = PdfStatus::failure(PdfError::Malformed);
    return status_;
  }

  manifest_ = decoded;
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

int PdfReflowDocument::getSectionCount() const { return loaded_ ? 1 : 0; }

ReflowSectionInfo PdfReflowDocument::getSectionInfo(const int sectionIndex) const {
  if (!loaded_ || sectionIndex != 0) {
    return {};
  }
  ReflowSectionInfo section;
  section.href = "section.xhtml";
  section.title = title_;
  section.byteSize = static_cast<uint32_t>(std::min<uint64_t>(sectionRecord_.size, UINT32_MAX));
  section.cumulativeSize = section.byteSize;
  section.firstWordOrdinal = 0;
  section.wordCount = manifest_.totalWords;
  section.tocIndex = 0;
  return section;
}

bool PdfReflowDocument::getSectionSize(const int sectionIndex, size_t* const size) const {
  if (!loaded_ || sectionIndex != 0 || size == nullptr ||
      sectionRecord_.size > static_cast<uint64_t>(std::numeric_limits<size_t>::max())) {
    return false;
  }
  *size = static_cast<size_t>(sectionRecord_.size);
  return true;
}

size_t PdfReflowDocument::getCumulativeSectionSize(const int sectionIndex) const {
  size_t size = 0;
  return getSectionSize(sectionIndex, &size) ? size : 0;
}

size_t PdfReflowDocument::getDocumentSize() const { return getCumulativeSectionSize(0); }
int PdfReflowDocument::getSectionIndexForTextReference() const { return loaded_ ? 0 : -1; }
int PdfReflowDocument::getTocEntryCount() const { return loaded_ ? 1 : 0; }

ReflowTocEntry PdfReflowDocument::getTocEntry(const int tocIndex) const {
  if (!loaded_ || tocIndex != 0) {
    return {};
  }
  return {title_, "section.xhtml", "", 0, 0, -1};
}

int PdfReflowDocument::getSectionIndexForTocIndex(const int tocIndex) const {
  return loaded_ && tocIndex == 0 ? 0 : -1;
}

int PdfReflowDocument::getTocIndexForSectionIndex(const int sectionIndex) const {
  return loaded_ && sectionIndex == 0 ? 0 : -1;
}

int PdfReflowDocument::resolveHrefToSectionIndex(const std::string& href) const {
  if (!loaded_) {
    return -1;
  }
  const size_t fragment = href.find('#');
  const std::string path = href.substr(0, fragment);
  if (path.empty() || path == "section.xhtml" || path == sectionRecord_.path) {
    return 0;
  }
  return -1;
}

float PdfReflowDocument::calculateSizeProgress(const int sectionIndex, const float sectionProgress) const {
  return loaded_ && sectionIndex == 0 ? clampUnit(sectionProgress) : 0.0F;
}

float PdfReflowDocument::calculateProgress(const int sectionIndex, const float sectionProgress) const {
  return calculateSizeProgress(sectionIndex, sectionProgress);
}

bool PdfReflowDocument::resolveProgressPercentToSection(const int percent, int& sectionIndex,
                                                        float& sectionProgress) const {
  if (!loaded_) {
    return false;
  }
  sectionIndex = 0;
  sectionProgress = clampUnit(static_cast<float>(std::clamp(percent, 0, 100)) / 100.0F);
  return true;
}

bool PdfReflowDocument::resolveReferencePage(int, float, uint32_t&, uint32_t&) const { return false; }
uint32_t PdfReflowDocument::getTotalWordCount() const { return loaded_ ? manifest_.totalWords : 0; }
bool PdfReflowDocument::loadReadingPosition(ReflowReadingPosition&) const { return false; }
bool PdfReflowDocument::saveReadingPosition(const ReflowReadingPosition&) const { return false; }

bool PdfReflowDocument::getLocalSectionPath(const int sectionIndex, ReflowResource& out) const {
  if (!loaded_ || sectionIndex != 0 || sectionPath_.empty()) {
    out = ReflowResource::streamed();
    return false;
  }
  out = ReflowResource::borrowedLocalFile(sectionPath_, ReflowImageKind::EncodedImage);
  return true;
}

bool PdfReflowDocument::streamCachedFile(const std::string& path, Print& out, size_t chunkSize) const {
  if (!loaded_ || !ioWorkspace_ || path.empty()) {
    return false;
  }
  chunkSize = std::clamp<size_t>(chunkSize, 1, PDF_SOURCE_FINGERPRINT_BYTES);
  PdfCacheHandle handle{};
  PdfStatus status = io_.open(io_.context, path.c_str(), PdfCacheOpenMode::Read, &handle);
  uint64_t offset = 0;
  while (status && offset < sectionRecord_.size) {
    const size_t requested = static_cast<size_t>(std::min<uint64_t>(chunkSize, sectionRecord_.size - offset));
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
  return loaded_ && sectionIndex == 0 && streamCachedFile(sectionPath_, out, chunkSize);
}

bool PdfReflowDocument::resolveResource(int, const std::string&, ReflowResource& out) const {
  out = ReflowResource::streamed();
  return false;
}

bool PdfReflowDocument::streamResource(int, const std::string&, Print&, size_t) const { return false; }
bool PdfReflowDocument::getResourceSize(int, const std::string&, size_t*) const { return false; }
