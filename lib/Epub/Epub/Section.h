#pragma once

#include <HalStorage.h>
#include <ReflowDocument.h>

#include <functional>
#include <memory>
#include <optional>
#include <string>

#include "EpubRenderMode.h"
#include "ReaderRenderSpec.h"

class Page;
class GfxRenderer;
class TextBlock;
class ChapterHtmlSlimParser;
class CssParser;

struct SectionBuildOptions {
  const char* previewAnchor = nullptr;
  uint16_t previewMaxPages = 0;

  bool isPreview() const { return previewAnchor && previewAnchor[0] != '\0' && previewMaxPages > 0; }
};

class Section {
  std::shared_ptr<ReflowDocument> document;
  const int sectionIndex;
  GfxRenderer& renderer;
  std::string filePath;
  HalFile file;

  struct PageLutEntry {
    uint32_t fileOffset;
    uint16_t paragraphIndex;
    uint16_t listItemIndex;
  };

  struct BuildContext {
    std::unique_ptr<ChapterHtmlSlimParser> parser;
    std::unique_ptr<PageLutEntry[]> lut;
    uint16_t lutCapacity = 0;
    uint16_t lutCount = 0;
    std::string parsePath;
    std::string contentBase;
    std::string imageBasePath;
    std::string htmlPath;
    std::string tmpHtmlPath;
    std::string tmpSectionPath;
    bool reusedHtml = false;
    bool pageCompletionFailed = false;
    CssParser* cssParser = nullptr;
    uint32_t bytesConsumed = 0;
    uint32_t totalBytes = 0;
    float smoothedEstimate = 0;
    uint32_t smoothedAtConsumed = 0;
  };

  std::unique_ptr<BuildContext> build_;
  bool buildComplete_ = false;
  bool lastImagesWereSuppressed_ = false;
  bool lastLayoutAbortedForLowMemory_ = false;
  uint16_t builtPageCount_ = 0;
  bool partial_ = false;
  uint16_t partialPageCount_ = 0;
  uint32_t partialBytesConsumed_ = 0;
  uint32_t partialTotalBytes_ = 0;
  std::string activeBuildTmpSectionPath_;

  struct PdfPageBuildContext;
  bool writeSectionFileHeader(const ReaderRenderSpec& spec);
  uint32_t onPageComplete(std::unique_ptr<Page> page);
  bool ensureBuildFileOpen();
  bool finalizeBuild();
  bool commitBuildFile(uint8_t version, uint32_t bytesConsumed, uint32_t totalBytes);
  std::string binTmpPath() const { return filePath + ".part"; }
  std::unique_ptr<Page> loadPageAt(int page) const;
  std::unique_ptr<Page> loadPageDuringBuild(int page);
  bool usesPdfWordIndex() const;
  static void completePdfPage(void* context, std::unique_ptr<Page> page, uint16_t paragraphIndex,
                              uint16_t listItemIndex);
  static bool finishPdfTextBlock(void* context, const Page* currentPage);
  static bool beginPdfTextBlock(void* context, const char* anchor, size_t anchorLength);
  static bool trackPdfTextLine(void* context, const TextBlock* line);

 public:
  uint16_t pageCount = 0;
  int currentPage = 0;

  explicit Section(const std::shared_ptr<ReflowDocument>& document, int sectionIndex, GfxRenderer& renderer,
                   const char* cacheSuffix = "");
  ~Section();
  bool loadSectionFile(const ReaderRenderSpec& spec);
  bool clearCache() const;
  bool createSectionFile(const ReaderRenderSpec& spec, const std::function<void()>& popupFn = nullptr,
                         bool* imagesWereSuppressed = nullptr, bool* layoutAbortedForLowMemory = nullptr,
                         SectionBuildOptions buildOptions = {});

  bool startBuild(const ReaderRenderSpec& spec, SectionBuildOptions buildOptions = {},
                  const std::function<void()>& popupFn = nullptr);
  bool buildSomeMore(int maxPages);
  void releaseBuildFile();
  bool lastBuildImagesWereSuppressed() const { return lastImagesWereSuppressed_; }
  bool lastBuildLayoutAbortedForLowMemory() const { return lastLayoutAbortedForLowMemory_; }
  bool isBuilding() const { return static_cast<bool>(build_); }
  bool isBuildComplete() const { return buildComplete_; }
  bool activeBuildHasCaughtReadablePages() const { return !build_ || builtPageCount_ >= pageCount; }
  // Best-known total page count: the exact pageCount once finalized, or a smoothed byte-based
  // estimate (pages so far scaled by totalBytes/bytesConsumed, damped by an EMA) while a giant spine
  // is still building, so "page X of Y" / progress don't read off the small build watermark.
  uint16_t estimatedTotalPages() const;
  void abandonBuild();
  // Persist an in-progress build as a partial section file (version sentinel + LUTs +
  // watermark trailer) instead of discarding it, so the next open of this spine can show
  // its pages instantly and only rebuild in the background. Called by the destructor, so
  // any teardown path (exit, sleep, navigation) keeps the work already done. Keeps a
  // pre-existing partial when it covers more pages than this build reached.
  void suspendBuild();
  // True when a partial file was loaded: pageCount is a watermark, not the chapter total.
  bool isPartial() const { return partial_; }

  // Unified page read: from the active build if it has reached the page, otherwise from
  // the on-disk file (finalized section, or a partial the rebuild hasn't caught up to).
  std::unique_ptr<Page> loadPage(int page);

  std::unique_ptr<Page> loadPageFromSectionFile();
  std::string getTextFromSectionFile();

  // Resolve an anchor from the in-progress build first, then the on-disk anchor map
  // (covers finalized sections and partials from a previous session).
  std::optional<uint16_t> findAnchor(const std::string& anchor) const;

  // True if this spine's unzipped HTML is already cached, so a build won't pay the (multi-second on a
  // giant spine) zip inflation. Lets the reader skip the indexing popup on a fast reopen/rebuild.
  bool hasHtmlCache() const;

  // Look up the page number for an anchor id from the section cache file.
  std::optional<uint16_t> getPageForAnchor(const std::string& anchor) const;

  // Look up an anchor among the pages built so far by the in-progress build, so an anchor jump
  // (TOC / chapter select, usually the chapter top = page 0) can resolve without laying out the
  // whole chapter. Returns nullopt if the anchor hasn't been reached yet (build more) or no build.
  std::optional<uint16_t> findAnchorDuringBuild(const std::string& anchor) const;

  // Get the page count from the section cache file without fully loading it.
  std::optional<uint16_t> getCachedPageCount() const;

  // Look up the page number for a synthetic paragraph index from XPath p[N].
  // Checks the active incremental build before falling back to the committed cache.
  std::optional<uint16_t> getPageForParagraphIndex(uint16_t pIndex) const;

  // Look up a synthetic paragraph among pages produced by the active build only.
  std::optional<uint16_t> findParagraphDuringBuild(uint16_t pIndex) const;

  // Look up the page number for a running list-item index from the li LUT.
  std::optional<uint16_t> getPageForListItemIndex(uint16_t liIndex) const;

  // Look up the synthetic paragraph index for the given rendered page.
  std::optional<uint16_t> getParagraphIndexForPage(uint16_t page) const;

  // Look up the running list-item index for the given rendered page.
  std::optional<uint16_t> getListItemIndexForPage(uint16_t page) const;

  // PDF-only, fixed-record semantic positions. EPUB sections never create or
  // read this sidecar, so their v44 cache bytes remain unchanged.
  std::optional<ReflowPageSemanticRange> getSemanticRangeForPage(uint16_t page);
  std::optional<uint16_t> getPageForSemanticPosition(const char* blockAnchor, uint32_t blockWordOffset,
                                                     uint32_t globalWordOrdinal);
  std::optional<uint16_t> getPageForSemanticCursor(uint32_t wordCursor);
  const std::string& getCacheFilePath() const { return filePath; }
};
