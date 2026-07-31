#include "RecentBookProgress.h"

#include <Epub.h>
#include <FsHelpers.h>
#include <HalStorage.h>
#include <Serialization.h>
#include <Txt.h>
#include <Xtc.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>

#if defined(CROSSINK_ENABLE_PDF) && CROSSINK_ENABLE_PDF
#include <Memory.h>

#include <cstddef>
#include <cstring>
#include <new>

#include "PdfCachedProductState.h"
#include "PdfHalCacheIo.h"
#include "PdfSourceIdentity.h"
#include "Logging.h"
#include "util/BookMoveUtils.h"
#endif

#include "RecentBooksStore.h"
#include "activities/reader/EpubReaderUtils.h"

namespace {
constexpr uint32_t TXT_CACHE_MAGIC = 0x54585449;  // "TXTI"
constexpr uint8_t TXT_CACHE_VERSION = 3;
#if defined(CROSSINK_ENABLE_PDF) && CROSSINK_ENABLE_PDF
constexpr char PDF_CACHE_DIRECTORY[] = "/.crosspoint";
#endif

float clampProgressPercent(const float progress) { return std::clamp(progress, 0.0f, 100.0f); }

float loadEpubProgressPercent(const RecentBook& book) {
  Epub epub(book.path, "/.crosspoint");
  if (!epub.load(false, true)) {
    return -1.0f;
  }

  EpubReaderUtils::Progress progress;
  if (!EpubReaderUtils::loadProgress(epub, progress, "RBPR") || !progress.hasPageCount) {
    return -1.0f;
  }

  if (progress.pageCount <= 0) {
    return 0.0f;
  }

  const float chapterProgress = static_cast<float>(progress.pageNumber + 1) / static_cast<float>(progress.pageCount);
  return clampProgressPercent(epub.calculateProgress(progress.spineIndex, chapterProgress) * 100.0f);
}

float loadXtcProgressPercent(const RecentBook& book) {
  Xtc xtc(book.path, "/.crosspoint");
  if (!xtc.load()) {
    return -1.0f;
  }

  FsFile file;
  if (!Storage.openFileForRead("RBPR", xtc.getCachePath() + "/progress.bin", file)) {
    return -1.0f;
  }

  uint8_t data[4];
  const int bytesRead = file.read(data, sizeof(data));
  file.close();
  if (bytesRead != 4) {
    return -1.0f;
  }

  const uint32_t currentPage = static_cast<uint32_t>(data[0]) | (static_cast<uint32_t>(data[1]) << 8) |
                               (static_cast<uint32_t>(data[2]) << 16) | (static_cast<uint32_t>(data[3]) << 24);
  return clampProgressPercent(static_cast<float>(xtc.calculateProgress(currentPage)));
}

float loadTxtProgressPercent(const RecentBook& book) {
  Txt txt(book.path, "/.crosspoint");
  if (!txt.load()) {
    return -1.0f;
  }

  FsFile progressFile;
  if (!Storage.openFileForRead("RBPR", txt.getCachePath() + "/progress.bin", progressFile)) {
    return -1.0f;
  }

  uint8_t progressData[4];
  const int progressBytes = progressFile.read(progressData, sizeof(progressData));
  progressFile.close();
  if (progressBytes != 4) {
    return -1.0f;
  }

  const uint32_t currentPage = static_cast<uint32_t>(progressData[0]) | (static_cast<uint32_t>(progressData[1]) << 8) |
                               (static_cast<uint32_t>(progressData[2]) << 16) |
                               (static_cast<uint32_t>(progressData[3]) << 24);

  FsFile indexFile;
  if (!Storage.openFileForRead("RBPR", txt.getCachePath() + "/index.bin", indexFile)) {
    return -1.0f;
  }

  uint32_t magic = 0;
  uint8_t version = 0;
  uint32_t fileSize = 0;
  int32_t cachedWidth = 0;
  int32_t cachedLines = 0;
  int32_t fontId = 0;
  int32_t margin = 0;
  uint8_t alignment = 0;
  uint32_t totalPages = 0;
  const bool readOk =
      serialization::tryReadPod(indexFile, magic) && serialization::tryReadPod(indexFile, version) &&
      serialization::tryReadPod(indexFile, fileSize) && serialization::tryReadPod(indexFile, cachedWidth) &&
      serialization::tryReadPod(indexFile, cachedLines) && serialization::tryReadPod(indexFile, fontId) &&
      serialization::tryReadPod(indexFile, margin) && serialization::tryReadPod(indexFile, alignment) &&
      serialization::tryReadPod(indexFile, totalPages);
  indexFile.close();
  if (!readOk) {
    return -1.0f;
  }
  (void)cachedWidth;
  (void)cachedLines;
  (void)fontId;
  (void)margin;
  (void)alignment;

  if (magic != TXT_CACHE_MAGIC || version != TXT_CACHE_VERSION || fileSize != txt.getFileSize() || totalPages == 0) {
    return -1.0f;
  }

  return clampProgressPercent((static_cast<float>(currentPage + 1) / static_cast<float>(totalPages)) * 100.0f);
}

#if defined(CROSSINK_ENABLE_PDF) && CROSSINK_ENABLE_PDF
const char* emptyProductValue() {
  static constexpr char EMPTY[] = "";
  return EMPTY;
}

void applyPdfFallback(RecentBook& book, float* const progress, std::string* const chapter,
                      std::string* const fullCoverPath) {
  const size_t separator = book.path.find_last_of("/\\");
  const size_t filenameOffset = separator == std::string::npos ? 0 : separator + 1;
  book.title.assign(book.path, filenameOffset, std::string::npos);
  book.author.clear();
  book.coverBmpPath.clear();
  if (progress != nullptr) {
    *progress = 0.0f;
  }
  if (chapter != nullptr) {
    chapter->clear();
  }
  if (fullCoverPath != nullptr) {
    fullCoverPath->clear();
  }
}

void clearPdfHydrationOutputs(float* const progress, std::string* const chapter, std::string* const fullCoverPath) {
  if (progress != nullptr) {
    *progress = -1.0f;
  }
  if (chapter != nullptr) {
    chapter->clear();
  }
  if (fullCoverPath != nullptr) {
    fullCoverPath->clear();
  }
}
#endif
}  // namespace

#if defined(CROSSINK_ENABLE_PDF) && CROSSINK_ENABLE_PDF
struct RecentBookProgress::PdfProductCache::Impl {
  struct RetainedWorkspace {
    void* allocation = nullptr;
    size_t capacity = 0;
    bool inUse = false;

    ~RetainedWorkspace() { ::operator delete(allocation); }

    static void* allocate(void* const context, const size_t size) {
      if (context == nullptr || size == 0) {
        return nullptr;
      }

      auto& workspace = *static_cast<RetainedWorkspace*>(context);
      if (workspace.inUse) {
        return nullptr;
      }
      if (workspace.allocation == nullptr || workspace.capacity < size) {
        ::operator delete(workspace.allocation);
        workspace.allocation = ::operator new(size, std::nothrow);
        workspace.capacity = workspace.allocation != nullptr ? size : 0;
      }
      workspace.inUse = workspace.allocation != nullptr;
      return workspace.allocation;
    }

    static void release(void* const context, void* const allocation) {
      if (context == nullptr) {
        return;
      }
      auto& workspace = *static_cast<RetainedWorkspace*>(context);
      if (allocation == workspace.allocation) {
        workspace.inUse = false;
      }
    }
  };

  PdfHalCacheIoContext ioContext{};
  PdfCachedProductState state{};
  PdfCachedProductStateLoadResult result{};
  RetainedWorkspace workspace{};
  char loadedPath[PDF_CACHE_PATH_CAPACITY]{};
  char cacheRoot[PDF_CACHE_PATH_CAPACITY]{};
  uint64_t loadedCacheHash = 0;
  bool loadedReadOnlyFallback = false;
  bool preserveStoredFallback = false;
  bool attempted = false;
};

RecentBookProgress::PdfProductCache::PdfProductCache() = default;

RecentBookProgress::PdfProductCache::~PdfProductCache() = default;

bool RecentBookProgress::PdfProductCache::initialize() {
  if (impl_ != nullptr) {
    return true;
  }
  impl_ = makeUniqueNoThrow<Impl>();
  if (impl_ == nullptr) {
    LOG_ERR("RBPR", "PDF product cache allocation failed");
    return false;
  }
  return true;
}

void RecentBookProgress::PdfProductCache::reset() { impl_.reset(); }

bool RecentBookProgress::PdfProductCache::load(const std::string& sourcePath) {
  if (!initialize()) {
    return false;
  }

  if (sourcePath.empty() || sourcePath.size() >= sizeof(impl_->loadedPath)) {
    LOG_ERR("RBPR", "PDF source path exceeds cache path capacity");
    impl_->state = {};
    impl_->result = {};
    impl_->preserveStoredFallback = false;
    impl_->attempted = true;
    impl_->loadedPath[0] = '\0';
    impl_->cacheRoot[0] = '\0';
    return false;
  }

  const uint64_t normalCacheHash = pdfPathHash64(sourcePath.c_str(), sourcePath.size());
  uint64_t resolvedCacheHash = normalCacheHash;
  bool readOnlyFallback = true;
  const bool resolved =
      BookMoveUtils::migrationCacheHash(sourcePath, normalCacheHash, &resolvedCacheHash, &readOnlyFallback);
  if (impl_->attempted && sourcePath == impl_->loadedPath && resolved && impl_->loadedCacheHash == resolvedCacheHash &&
      impl_->loadedReadOnlyFallback == readOnlyFallback) {
    return impl_->result.available();
  }

  std::memcpy(impl_->loadedPath, sourcePath.c_str(), sourcePath.size() + 1);
  impl_->loadedCacheHash = resolvedCacheHash;
  impl_->loadedReadOnlyFallback = readOnlyFallback;
  impl_->preserveStoredFallback = readOnlyFallback;
  impl_->attempted = true;
  impl_->state = {};
  impl_->result = {};
  if (!resolved) {
    impl_->cacheRoot[0] = '\0';
    LOG_ERR("RBPR", "PDF migration cache resolution failed");
    return false;
  }
  if (!pdfFormatCacheRootForHash(PDF_CACHE_DIRECTORY, resolvedCacheHash, impl_->cacheRoot,
                                 sizeof(impl_->cacheRoot))) {
    impl_->cacheRoot[0] = '\0';
    LOG_ERR("RBPR", "PDF cache root exceeds fixed path capacity");
    return false;
  }

  const PdfCachedProductStateAllocator allocator{
      &impl_->workspace,
      Impl::RetainedWorkspace::allocate,
      Impl::RetainedWorkspace::release,
  };
  const uint64_t* const cacheHashOverride = resolvedCacheHash == normalCacheHash ? nullptr : &resolvedCacheHash;
  impl_->result = pdfLoadCachedProductState(pdfHalCacheIo(impl_->ioContext), sourcePath.c_str(), PDF_CACHE_DIRECTORY,
                                             &impl_->state, allocator, cacheHashOverride);
  return impl_->result.available();
}

bool RecentBookProgress::PdfProductCache::available() const { return impl_ != nullptr && impl_->result.available(); }

bool RecentBookProgress::PdfProductCache::preservesStoredFallback() const {
  return impl_ != nullptr && impl_->preserveStoredFallback;
}

const char* RecentBookProgress::PdfProductCache::title() const {
  return available() ? impl_->state.title : emptyProductValue();
}

const char* RecentBookProgress::PdfProductCache::author() const {
  return available() ? impl_->state.author : emptyProductValue();
}

const char* RecentBookProgress::PdfProductCache::chapter() const {
  return available() ? impl_->state.currentChapter : emptyProductValue();
}

const char* RecentBookProgress::PdfProductCache::coverPath() const {
  return available() ? impl_->state.coverPath : emptyProductValue();
}

const char* RecentBookProgress::PdfProductCache::thumbnailPath() const {
  return available() ? impl_->state.thumbnailPath : emptyProductValue();
}

const char* RecentBookProgress::PdfProductCache::cacheRoot() const {
  return available() ? impl_->cacheRoot : emptyProductValue();
}

uint16_t RecentBookProgress::PdfProductCache::currentSection() const {
  return available() ? impl_->state.currentSection : 0;
}

uint32_t RecentBookProgress::PdfProductCache::currentWord() const {
  return available() ? impl_->state.currentWord : 0;
}

uint32_t RecentBookProgress::PdfProductCache::totalWords() const {
  return available() ? impl_->state.totalWords : 0;
}

uint32_t RecentBookProgress::PdfProductCache::currentSectionFirstWordOrdinal() const {
  return available() ? impl_->state.currentSectionFirstWordOrdinal : 0;
}

uint32_t RecentBookProgress::PdfProductCache::currentSectionWordCount() const {
  return available() ? impl_->state.currentSectionWordCount : 0;
}

float RecentBookProgress::PdfProductCache::progressPercent() const {
  if (!available() || !impl_->state.hasProgress || impl_->state.totalWords == 0) {
    return 0.0f;
  }
  return clampProgressPercent(static_cast<float>(impl_->state.currentWord) /
                              static_cast<float>(impl_->state.totalWords) * 100.0f);
}

bool RecentBookProgress::hydratePdfBook(PdfProductCache& cache, RecentBook& book, float* const progress,
                                        std::string* const chapter, std::string* const fullCoverPath) {
  if (!FsHelpers::hasPdfExtension(book.path)) {
    return false;
  }

  if (!cache.load(book.path)) {
    if (cache.preservesStoredFallback()) {
      clearPdfHydrationOutputs(progress, chapter, fullCoverPath);
    } else {
      applyPdfFallback(book, progress, chapter, fullCoverPath);
    }
    return true;
  }

  applyPdfFallback(book, progress, chapter, fullCoverPath);
  if (cache.title()[0] != '\0') {
    book.title = cache.title();
  }
  book.author = cache.author();
  book.coverBmpPath = cache.thumbnailPath();
  if (progress != nullptr) {
    *progress = cache.progressPercent();
  }
  if (chapter != nullptr) {
    *chapter = cache.chapter();
  }
  if (fullCoverPath != nullptr) {
    *fullCoverPath = cache.coverPath();
  }
  return true;
}
#endif

float RecentBookProgress::loadPercent(const RecentBook& book) {
  if (FsHelpers::hasEpubExtension(book.path)) {
    return loadEpubProgressPercent(book);
  }
  if (FsHelpers::hasXtcExtension(book.path)) {
    return loadXtcProgressPercent(book);
  }
  if (FsHelpers::hasTxtExtension(book.path) || FsHelpers::hasMarkdownExtension(book.path)) {
    return loadTxtProgressPercent(book);
  }
  return -1.0f;
}

bool RecentBookProgress::hasPercent(const float progress) { return progress >= 0.0f; }

std::string RecentBookProgress::formatPercent(const float progress) {
  if (!hasPercent(progress)) {
    return "";
  }
  char buffer[8];
  snprintf(buffer, sizeof(buffer), "%.0f%%", clampProgressPercent(progress));
  return buffer;
}
