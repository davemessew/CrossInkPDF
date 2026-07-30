#include "Section.h"

#include <Arduino.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <InflateReader.h>
#include <Logging.h>
#include <Memory.h>
#include <MemoryBudget.h>
#include <PdfCacheIo.h>
#include <PdfHalIo.h>
#include <PdfLayoutWordIndex.h>
#include <PdfWordCounter.h>
#include <ScratchWorkspace.h>
#include <Serialization.h>

#include <cstring>

#include "Epub/css/CssParser.h"
#include "Page.h"
#include "hyphenation/Hyphenator.h"
#include "parsers/ChapterHtmlSlimParser.h"

namespace {
constexpr uint32_t SECTION_CACHE_MAGIC = 0x535843FF;  // bytes: 0xFF, "CXS"
// v44: TextBlock word data is stored as one flat arena with optional bionic,
// guide-dot, and word-flag arrays.
constexpr uint8_t SECTION_FILE_VERSION = 44;
constexpr uint16_t INITIAL_SECTION_PAGE_LUT_ENTRIES = 1024;
constexpr uint32_t HEADER_SIZE = sizeof(SECTION_CACHE_MAGIC) + sizeof(uint8_t) + sizeof(int) + sizeof(float) +
                                 sizeof(bool) + sizeof(bool) + sizeof(uint8_t) + sizeof(uint16_t) + sizeof(uint16_t) +
                                 sizeof(uint16_t) + sizeof(bool) + sizeof(bool) + sizeof(uint8_t) + sizeof(bool) +
                                 sizeof(bool) + sizeof(uint8_t) + sizeof(uint32_t) + sizeof(uint32_t) +
                                 sizeof(uint32_t) + sizeof(uint32_t);
constexpr size_t SECTION_HTML_STREAM_CHUNK_SIZE = 8192;
constexpr size_t LOW_MEMORY_SECTION_HTML_STREAM_CHUNK_SIZE = 1024;

struct PageLutEntry {
  uint32_t fileOffset;
  uint16_t paragraphIndex;
  uint16_t listItemIndex;
};

template <typename Entry>
bool ensurePageLutCapacity(std::unique_ptr<Entry[]>& lut, uint16_t& lutCapacity, const uint16_t lutCount) {
  if (lutCount < lutCapacity) return true;
  if (lutCapacity == UINT16_MAX) return false;

  uint32_t nextCapacity = static_cast<uint32_t>(lutCapacity) * 2U;
  if (nextCapacity > UINT16_MAX) {
    nextCapacity = UINT16_MAX;
  }

  auto grown = makeUniqueNoThrow<Entry[]>(nextCapacity);
  if (!grown) return false;

  for (uint16_t i = 0; i < lutCount; i++) {
    grown[i] = lut[i];
  }
  lut = std::move(grown);
  lutCapacity = static_cast<uint16_t>(nextCapacity);
  return true;
}

ScratchWorkspace::Lease acquireSectionZipInflateScratch(GfxRenderer& renderer, const int fontId, const char* reason) {
  if (ESP.getMaxAllocHeap() < InflateReader::STREAMING_DICT_SIZE && renderer.isSdCardFont(fontId)) {
    renderer.releaseSdCardFontForLowMemory(fontId);
  }

  auto scratch = ScratchWorkspace::acquire(InflateReader::STREAMING_DICT_SIZE, reason);
  if (scratch || !renderer.isSdCardFont(fontId)) {
    return scratch;
  }

  renderer.releaseSdCardFontForLowMemory(fontId);
  return ScratchWorkspace::acquire(InflateReader::STREAMING_DICT_SIZE, reason);
}

size_t sectionHtmlStreamChunkSize(const bool preview) {
  if (preview) {
    return LOW_MEMORY_SECTION_HTML_STREAM_CHUNK_SIZE;
  }

  const uint32_t maxAlloc = ESP.getMaxAllocHeap();
  const size_t largeStreamBudget = InflateReader::STREAMING_DICT_SIZE + (2U * SECTION_HTML_STREAM_CHUNK_SIZE);
  if (maxAlloc < largeStreamBudget) {
    LOG_DBG("SCT", "Using low-memory HTML stream chunk (maxAlloc=%u)", maxAlloc);
    return LOW_MEMORY_SECTION_HTML_STREAM_CHUNK_SIZE;
  }
  return SECTION_HTML_STREAM_CHUNK_SIZE;
}
}  // namespace

struct Section::PdfPageBuildContext {
  Section& section;
  std::unique_ptr<PageLutEntry[]>& lut;
  uint16_t& lutCapacity;
  uint16_t& lutCount;
  bool& pageCompletionFailed;
  bool* layoutAbortedForLowMemory;
  HalFile sidecarFile;
  PdfLayoutWordIndexWriter writer;
  PdfWordCounter wordCounter;
  PdfLayoutWordRange pageRange{};
  uint32_t sectionFirstWordOrdinal = 0;
  uint32_t nextGlobalWordOrdinal = 0;
  uint32_t wordsExtractedInBlock = 0;
  size_t scannedPageElements = 0;
  char currentBlockAnchor[PDF_LAYOUT_WORD_ANCHOR_BYTES] = {};
  char finalPath[PDF_CACHE_PATH_CAPACITY] = {};
  char tempPath[PDF_CACHE_PATH_CAPACITY] = {};
  bool blockHasToken = false;
  bool promoted = false;

  PdfPageBuildContext(Section& section, std::unique_ptr<PageLutEntry[]>& lut, uint16_t& lutCapacity, uint16_t& lutCount,
                      bool& pageCompletionFailed, bool* layoutAbortedForLowMemory)
      : section(section),
        lut(lut),
        lutCapacity(lutCapacity),
        lutCount(lutCount),
        pageCompletionFailed(pageCompletionFailed),
        layoutAbortedForLowMemory(layoutAbortedForLowMemory) {}

  ~PdfPageBuildContext() {
    if (sidecarFile) {
      sidecarFile.close();
    }
    if (!promoted && tempPath[0] != '\0' && Storage.exists(tempPath)) {
      Storage.remove(tempPath);
    }
  }

  bool initialize(const std::string& sectionCachePath, const int sectionIndex, const ReflowSectionInfo& info) {
    const int finalLength = std::snprintf(finalPath, sizeof(finalPath), "%s.pwi", sectionCachePath.c_str());
    const int tempLength = finalLength > 0 && static_cast<size_t>(finalLength) < sizeof(finalPath)
                               ? std::snprintf(tempPath, sizeof(tempPath), "%s.tmp", finalPath)
                               : -1;
    if (tempLength <= 0 || static_cast<size_t>(tempLength) >= sizeof(tempPath)) {
      LOG_ERR("SCT", "PDF semantic sidecar path exceeds fixed capacity");
      return false;
    }
    if (Storage.exists(tempPath)) {
      Storage.remove(tempPath);
    }
    if (!Storage.openFileForWrite("SCT", tempPath, sidecarFile)) {
      LOG_ERR("SCT", "Failed to create PDF semantic sidecar");
      return false;
    }
    const PdfStatus status = writer.begin(pdfHalByteSink(sidecarFile), static_cast<uint16_t>(sectionIndex),
                                          info.firstWordOrdinal, info.wordCount);
    if (!status) {
      LOG_ERR("SCT", "Failed to start PDF semantic sidecar (%u)", static_cast<unsigned>(status.error));
      return false;
    }
    sectionFirstWordOrdinal = info.firstWordOrdinal;
    nextGlobalWordOrdinal = info.firstWordOrdinal;
    return true;
  }

  bool beginTextBlock(const char* const anchor, const size_t anchorLength) {
    wordCounter.reset();
    wordsExtractedInBlock = 0;
    blockHasToken = false;
    std::memset(currentBlockAnchor, 0, sizeof(currentBlockAnchor));
    if (anchor != nullptr && anchorLength < sizeof(currentBlockAnchor)) {
      std::memcpy(currentBlockAnchor, anchor, anchorLength);
    }
    return true;
  }

  bool trackLine(const TextBlock& line) {
    const uint32_t wordsBefore = wordCounter.words();
    bool beginsWithSplitContinuation = false;
    for (uint16_t index = 0; index < line.wordCount(); ++index) {
      const uint8_t flags = line.wordFlags(index);
      const bool attaches =
          (flags & (TextBlock::WORD_FLAG_SEMANTIC_ATTACHES | TextBlock::WORD_FLAG_SEMANTIC_SPLIT_CONTINUATION)) != 0;
      if (index == 0) {
        beginsWithSplitContinuation = (flags & TextBlock::WORD_FLAG_SEMANTIC_SPLIT_CONTINUATION) != 0;
      }
      if (blockHasToken && !attaches) {
        static constexpr uint8_t separator = ' ';
        const PdfStatus status = wordCounter.consume(&separator, 1);
        if (!status) {
          LOG_ERR("SCT", "Failed to separate PDF layout words at byte %llu",
                  static_cast<unsigned long long>(status.offset));
          return false;
        }
      }
      const PdfStatus status =
          wordCounter.consume(reinterpret_cast<const uint8_t*>(line.wordText(index)), line.wordTextLen(index));
      if (!status) {
        LOG_ERR("SCT", "Failed to count PDF layout words at byte %llu", static_cast<unsigned long long>(status.offset));
        return false;
      }
      blockHasToken = true;
    }

    const uint32_t wordsAfter = wordCounter.words();
    const uint32_t addedWords = wordsAfter - wordsBefore;
    if (addedWords == 0 && !beginsWithSplitContinuation) {
      return true;
    }
    if (addedWords > UINT32_MAX - nextGlobalWordOrdinal) {
      LOG_ERR("SCT", "PDF semantic word ordinal overflow");
      return false;
    }
    const uint32_t firstOrdinal = beginsWithSplitContinuation && nextGlobalWordOrdinal > sectionFirstWordOrdinal
                                      ? nextGlobalWordOrdinal - 1U
                                      : nextGlobalWordOrdinal;
    const uint32_t lastOrdinal = addedWords == 0 ? firstOrdinal : nextGlobalWordOrdinal + addedWords - 1U;
    if (!pageRange.valid) {
      pageRange.valid = true;
      pageRange.firstGlobalWordOrdinal = firstOrdinal;
      pageRange.firstBlockWordOffset = beginsWithSplitContinuation && wordsExtractedInBlock != 0
                                           ? wordsExtractedInBlock - 1U
                                           : wordsExtractedInBlock;
      std::memcpy(pageRange.blockAnchor, currentBlockAnchor, sizeof(pageRange.blockAnchor));
    }
    pageRange.lastGlobalWordOrdinal = lastOrdinal;
    nextGlobalWordOrdinal += addedWords;
    wordsExtractedInBlock = wordsAfter;
    return true;
  }

  bool scanNewPageLines(const Page* const page) {
    if (page == nullptr) {
      return true;
    }
    if (scannedPageElements > page->elements.size()) {
      LOG_ERR("SCT", "PDF semantic page cursor moved backwards");
      return false;
    }
    for (size_t index = scannedPageElements; index < page->elements.size(); ++index) {
      const auto& element = page->elements[index];
      if (element && element->getTag() == TAG_PageLine) {
        const auto& line = static_cast<const PageLine&>(*element);
        if (line.getBlock() && !trackLine(*line.getBlock())) {
          return false;
        }
      }
    }
    scannedPageElements = page->elements.size();
    return true;
  }

  bool finishTextBlock(const Page* const page) {
    if (!scanNewPageLines(page)) {
      return false;
    }
    if (blockHasToken) {
      const PdfStatus status = wordCounter.finish();
      if (!status) {
        LOG_ERR("SCT", "Failed to finalize PDF word tracking at byte %llu",
                static_cast<unsigned long long>(status.offset));
        return false;
      }
    }
    return true;
  }

  bool appendPage(const Page* const page) {
    if (page == nullptr || !scanNewPageLines(page)) {
      return false;
    }
    pageRange.wordCursor = nextGlobalWordOrdinal;
    const PdfStatus status = writer.append(pageRange);
    if (!status) {
      LOG_ERR("SCT", "Failed PDF semantic page %u (%u): range=%lu-%lu", lutCount, static_cast<unsigned>(status.error),
              static_cast<unsigned long>(pageRange.firstGlobalWordOrdinal),
              static_cast<unsigned long>(pageRange.lastGlobalWordOrdinal));
      return false;
    }
    pageRange = {};
    scannedPageElements = 0;
    return true;
  }

  bool finishSidecar() {
    const PdfStatus status = writer.finish();
    if (!status || !sidecarFile.sync()) {
      LOG_ERR("SCT", "Failed to finalize PDF semantic sidecar (%u)", static_cast<unsigned>(status.error));
      return false;
    }
    sidecarFile.close();
    return true;
  }

  bool promoteSidecar() {
    if (Storage.exists(finalPath) && !Storage.remove(finalPath)) {
      LOG_ERR("SCT", "Failed to replace prior PDF semantic sidecar");
      return false;
    }
    if (!Storage.rename(tempPath, finalPath)) {
      LOG_ERR("SCT", "Failed to promote PDF semantic sidecar into place");
      return false;
    }
    promoted = true;
    return true;
  }
};

void Section::completePdfPage(void* const context, std::unique_ptr<Page> page, const uint16_t paragraphIndex,
                              const uint16_t listItemIndex) {
  auto& build = *static_cast<PdfPageBuildContext*>(context);
  if (build.pageCompletionFailed) {
    return;
  }
  if (!build.appendPage(page.get())) {
    build.pageCompletionFailed = true;
    return;
  }
  if (build.lutCount == UINT16_MAX) {
    LOG_ERR("SCT", "Section page count exceeded cache format limit");
    build.pageCompletionFailed = true;
    return;
  }
  if (!ensurePageLutCapacity(build.lut, build.lutCapacity, build.lutCount)) {
    LOG_ERR("SCT", "Failed to grow section page LUT from %u entries", build.lutCapacity);
    if (build.layoutAbortedForLowMemory) {
      *build.layoutAbortedForLowMemory = true;
    }
    build.pageCompletionFailed = true;
    return;
  }
  const uint32_t fileOffset = build.section.onPageComplete(std::move(page));
  if (fileOffset == 0) {
    build.pageCompletionFailed = true;
    return;
  }
  build.lut[build.lutCount++] = {fileOffset, paragraphIndex, listItemIndex};
}

bool Section::finishPdfTextBlock(void* const context, const Page* const currentPage) {
  return static_cast<PdfPageBuildContext*>(context)->finishTextBlock(currentPage);
}

bool Section::beginPdfTextBlock(void* const context, const char* const anchor, const size_t anchorLength) {
  return static_cast<PdfPageBuildContext*>(context)->beginTextBlock(anchor, anchorLength);
}

bool Section::trackPdfTextLine(void* const context, const TextBlock* const line) {
  return line != nullptr && static_cast<PdfPageBuildContext*>(context)->trackLine(*line);
}

Section::Section(const std::shared_ptr<ReflowDocument>& document, const int sectionIndex, GfxRenderer& renderer,
                 const char* cacheSuffix)
    : document(document),
      sectionIndex(sectionIndex),
      renderer(renderer),
      filePath(document->getCachePath() + "/sections/" + std::to_string(sectionIndex) +
               (cacheSuffix ? cacheSuffix : "") + ".bin") {}

bool Section::usesPdfWordIndex() const {
  return document->getFormat() == ReflowDocumentFormat::Pdf && filePath.find("_fn_") == std::string::npos;
}

uint32_t Section::onPageComplete(std::unique_ptr<Page> page) {
  if (!file) {
    LOG_ERR("SCT", "File not open for writing page %d", pageCount);
    return 0;
  }
  if (!page) {
    LOG_ERR("SCT", "Cannot write null page %d", pageCount);
    return 0;
  }

  const uint32_t position = file.position();
  const uint32_t serializeStart = millis();
  if (!page->serialize(file)) {
    LOG_ERR("SCT", "Failed to serialize page %d", pageCount);
    return 0;
  }
  LOG_DBG("SCT", "Page %d processed (pos=%lu, serialize=%lums, free=%u, maxAlloc=%u)", pageCount,
          static_cast<unsigned long>(position), static_cast<unsigned long>(millis() - serializeStart),
          ESP.getFreeHeap(), ESP.getMaxAllocHeap());

  pageCount++;
  return position;
}

bool Section::writeSectionFileHeader(const int fontId, const float lineCompression, const bool extraParagraphSpacing,
                                     const bool forceParagraphIndents, const uint8_t paragraphAlignment,
                                     const uint16_t viewportWidth, const uint16_t viewportHeight,
                                     const bool hyphenationEnabled, const bool embeddedStyle,
                                     const uint8_t imageRendering, const bool bionicReadingEnabled,
                                     const bool guideReadingEnabled, const EpubRenderMode renderMode) {
  if (!file) {
    LOG_DBG("SCT", "File not open for writing header");
    return false;
  }
  static_assert(HEADER_SIZE == sizeof(SECTION_CACHE_MAGIC) + sizeof(SECTION_FILE_VERSION) + sizeof(fontId) +
                                   sizeof(lineCompression) + sizeof(extraParagraphSpacing) +
                                   sizeof(forceParagraphIndents) + sizeof(paragraphAlignment) + sizeof(viewportWidth) +
                                   sizeof(viewportHeight) + sizeof(pageCount) + sizeof(hyphenationEnabled) +
                                   sizeof(embeddedStyle) + sizeof(imageRendering) + sizeof(bionicReadingEnabled) +
                                   sizeof(guideReadingEnabled) + sizeof(uint8_t) + sizeof(uint32_t) + sizeof(uint32_t) +
                                   sizeof(uint32_t) + sizeof(uint32_t),
                "Header size mismatch");
  return serialization::tryWritePod(file, SECTION_CACHE_MAGIC) &&
         serialization::tryWritePod(file, SECTION_FILE_VERSION) && serialization::tryWritePod(file, fontId) &&
         serialization::tryWritePod(file, lineCompression) && serialization::tryWritePod(file, extraParagraphSpacing) &&
         serialization::tryWritePod(file, forceParagraphIndents) &&
         serialization::tryWritePod(file, paragraphAlignment) && serialization::tryWritePod(file, viewportWidth) &&
         serialization::tryWritePod(file, viewportHeight) && serialization::tryWritePod(file, hyphenationEnabled) &&
         serialization::tryWritePod(file, embeddedStyle) && serialization::tryWritePod(file, imageRendering) &&
         serialization::tryWritePod(file, bionicReadingEnabled) &&
         serialization::tryWritePod(file, guideReadingEnabled) &&
         serialization::tryWritePod(file, static_cast<uint8_t>(renderMode)) &&
         serialization::tryWritePod(file,
                                    pageCount) &&  // Placeholder for page count (will be initially 0, patched later)
         serialization::tryWritePod(file, static_cast<uint32_t>(0)) &&  // Placeholder for LUT offset (patched later)
         serialization::tryWritePod(file,
                                    static_cast<uint32_t>(0)) &&  // Placeholder for anchor map offset (patched later)
         serialization::tryWritePod(
             file,
             static_cast<uint32_t>(0)) &&  // Placeholder for paragraph LUT offset (patched later)
         serialization::tryWritePod(file, static_cast<uint32_t>(0));  // Placeholder for li LUT offset (patched later)
}

bool Section::loadSectionFile(const int fontId, const float lineCompression, const bool extraParagraphSpacing,
                              const bool forceParagraphIndents, const uint8_t paragraphAlignment,
                              const uint16_t viewportWidth, const uint16_t viewportHeight,
                              const bool hyphenationEnabled, const bool embeddedStyle, const uint8_t imageRendering,
                              const bool bionicReadingEnabled, const bool guideReadingEnabled,
                              const EpubRenderMode renderMode) {
  if (!Storage.openFileForRead("SCT", filePath, file)) {
    return false;
  }

  // Match parameters
  {
    uint32_t magic;
    if (!serialization::tryReadPod(file, magic)) {
      file.close();
      LOG_ERR("SCT", "Deserialization failed: could not read cache magic");
      clearCache();
      return false;
    }
    if (magic != SECTION_CACHE_MAGIC) {
      file.close();
      LOG_ERR("SCT", "Deserialization failed: cache magic mismatch");
      clearCache();
      return false;
    }

    uint8_t version;
    if (!serialization::tryReadPod(file, version)) {
      file.close();
      LOG_ERR("SCT", "Deserialization failed: could not read version");
      clearCache();
      return false;
    }
    if (version != SECTION_FILE_VERSION) {
      // Explicit close() required: member variable persists beyond function scope
      file.close();
      LOG_ERR("SCT", "Deserialization failed: Unknown version %u", version);
      clearCache();
      return false;
    }

    int fileFontId;
    uint16_t fileViewportWidth, fileViewportHeight;
    float fileLineCompression;
    bool fileExtraParagraphSpacing;
    bool fileForceParagraphIndents;
    uint8_t fileParagraphAlignment;
    bool fileHyphenationEnabled;
    bool fileEmbeddedStyle;
    uint8_t fileImageRendering;
    bool fileBionicReadingEnabled;
    bool fileGuideReadingEnabled;
    uint8_t fileRenderMode;
    if (!serialization::tryReadPod(file, fileFontId) || !serialization::tryReadPod(file, fileLineCompression) ||
        !serialization::tryReadPod(file, fileExtraParagraphSpacing) ||
        !serialization::tryReadPod(file, fileForceParagraphIndents) ||
        !serialization::tryReadPod(file, fileParagraphAlignment) ||
        !serialization::tryReadPod(file, fileViewportWidth) || !serialization::tryReadPod(file, fileViewportHeight) ||
        !serialization::tryReadPod(file, fileHyphenationEnabled) ||
        !serialization::tryReadPod(file, fileEmbeddedStyle) || !serialization::tryReadPod(file, fileImageRendering) ||
        !serialization::tryReadPod(file, fileBionicReadingEnabled) ||
        !serialization::tryReadPod(file, fileGuideReadingEnabled) || !serialization::tryReadPod(file, fileRenderMode)) {
      file.close();
      LOG_ERR("SCT", "Deserialization failed: truncated section header");
      clearCache();
      return false;
    }

    if (fontId != fileFontId || lineCompression != fileLineCompression ||
        extraParagraphSpacing != fileExtraParagraphSpacing || forceParagraphIndents != fileForceParagraphIndents ||
        paragraphAlignment != fileParagraphAlignment || viewportWidth != fileViewportWidth ||
        viewportHeight != fileViewportHeight || hyphenationEnabled != fileHyphenationEnabled ||
        embeddedStyle != fileEmbeddedStyle || imageRendering != fileImageRendering ||
        bionicReadingEnabled != fileBionicReadingEnabled || guideReadingEnabled != fileGuideReadingEnabled ||
        static_cast<uint8_t>(renderMode) != fileRenderMode) {
      file.close();
      LOG_ERR("SCT", "Deserialization failed: Parameters do not match");
      clearCache();
      return false;
    }
  }

  if (!serialization::tryReadPod(file, pageCount)) {
    file.close();
    LOG_ERR("SCT", "Deserialization failed: missing page count");
    clearCache();
    return false;
  }

  // Explicit close() required: member variable persists beyond function scope
  file.close();
  if (usesPdfWordIndex()) {
    if (!document->validateLayoutWordIndex(filePath, sectionIndex, pageCount)) {
      LOG_ERR("SCT", "PDF semantic sidecar invalid for section %d", sectionIndex);
      clearCache();
      return false;
    }
  }
  LOG_DBG("SCT", "Deserialization succeeded: %d pages", pageCount);
  return true;
}

// Your updated class method (assuming you are using the 'SD' object, which is a wrapper for a specific filesystem)
bool Section::clearCache() const {
  if (usesPdfWordIndex()) {
    const bool hasSection = Storage.exists(filePath.c_str());
    bool removed = true;
    if (hasSection && !Storage.remove(filePath.c_str())) {
      LOG_ERR("SCT", "Failed to clear cache");
      removed = false;
    }
    if (!document->removeLayoutWordIndex(filePath)) {
      LOG_ERR("SCT", "Failed to clear PDF semantic sidecar");
      removed = false;
    }
    if (removed) {
      LOG_DBG("SCT", hasSection ? "Cache cleared successfully" : "Cache does not exist, no action needed");
    }
    return removed;
  }

  if (!Storage.exists(filePath.c_str())) {
    LOG_DBG("SCT", "Cache does not exist, no action needed");
    return true;
  }

  if (!Storage.remove(filePath.c_str())) {
    LOG_ERR("SCT", "Failed to clear cache");
    return false;
  }

  LOG_DBG("SCT", "Cache cleared successfully");
  return true;
}

bool Section::createSectionFile(const int fontId, const float lineCompression, const bool extraParagraphSpacing,
                                const bool forceParagraphIndents, const uint8_t paragraphAlignment,
                                const uint16_t viewportWidth, const uint16_t viewportHeight,
                                const bool hyphenationEnabled, const bool embeddedStyle, const uint8_t imageRendering,
                                const bool bionicReadingEnabled, const bool guideReadingEnabled,
                                const std::function<void()>& popupFn, bool* imagesWereSuppressed,
                                bool* layoutAbortedForLowMemory, const EpubRenderMode renderMode,
                                const SectionBuildOptions buildOptions) {
  const auto localPath = document->getSectionInfo(sectionIndex).href;
  if (localPath.empty()) {
    LOG_ERR("SCT", "Section %d has no source href", sectionIndex);
    return false;
  }
  const auto htmlDir = document->getCachePath() + "/html";
  const auto htmlPath = htmlDir + "/" + std::to_string(sectionIndex) + ".html";
  const auto tmpHtmlPath = htmlDir + "/.tmp_" + std::to_string(sectionIndex) + ".html";
  const auto tmpSectionPath = filePath + ".tmp";
  pageCount = 0;
  if (layoutAbortedForLowMemory) *layoutAbortedForLowMemory = false;
  const bool effectiveBionicReadingEnabled = bionicReadingEnabled;
  const bool effectiveGuideReadingEnabled = guideReadingEnabled;
  LOG_DBG("SCT",
          "Create section start: spine=%d mode=%u preview=%u viewport=%ux%u image=%u bionic=%u guide=%u free=%u "
          "maxAlloc=%u",
          sectionIndex, static_cast<unsigned>(renderMode), buildOptions.isPreview() ? 1U : 0U, viewportWidth,
          viewportHeight, imageRendering, effectiveBionicReadingEnabled, effectiveGuideReadingEnabled,
          ESP.getFreeHeap(), ESP.getMaxAllocHeap());

  // Create cache directory if it doesn't exist
  {
    const auto sectionsDir = document->getCachePath() + "/sections";
    Storage.mkdir(sectionsDir.c_str());
  }

  ReflowResource borrowedSection;
  const bool usesBorrowedSection = document->getImmutableLocalSection(sectionIndex, borrowedSection);

  // Reuse the previously unzipped HTML if we already have it. The unzipped HTML is keyed only on the
  // book (it lives in the per-book cache dir), not on render settings, so it survives the invalidation
  // that wipes the layout (.bin) caches when font/margin/orientation change -- rebuilds then skip zip
  // inflation entirely. It's promoted by an atomic rename as soon as the inflate succeeds (below), so
  // future rebuilds can skip the multi-second inflate. If htmlPath exists it is known-complete.
  const bool reusedHtml = !usesBorrowedSection && Storage.exists(htmlPath.c_str());
  bool htmlCached = reusedHtml;
  bool tempHtmlMayDelete = false;
  const auto cleanupTempHtml = [&]() {
    if (tempHtmlMayDelete && Storage.exists(tmpHtmlPath.c_str())) {
      Storage.remove(tmpHtmlPath.c_str());
    }
  };
  if (usesBorrowedSection) {
    LOG_DBG("SCT", "Parsing borrowed local HTML %s", borrowedSection.localPath.c_str());
  } else if (reusedHtml) {
    LOG_DBG("SCT", "Reusing cached HTML %s", htmlPath.c_str());
  } else {
    tempHtmlMayDelete = true;
    Storage.mkdir(htmlDir.c_str());

    // Retry logic for SD card timing issues
    bool streamed = false;
    uint32_t fileSize = 0;
    for (int attempt = 0; attempt < 3 && !streamed; attempt++) {
      if (attempt > 0) {
        LOG_DBG("SCT", "Retrying stream (attempt %d)...", attempt + 1);
        delay(50);  // Brief delay before retry
      }

      // Remove any incomplete file from previous attempt before retrying
      if (Storage.exists(tmpHtmlPath.c_str())) {
        Storage.remove(tmpHtmlPath.c_str());
      }

      HalFile tmpHtml;
      if (!Storage.openFileForWrite("SCT", tmpHtmlPath, tmpHtml)) {
        continue;
      }
      const size_t htmlStreamChunkSize = sectionHtmlStreamChunkSize(buildOptions.isPreview());
      {
        auto zipInflateScratch = acquireSectionZipInflateScratch(renderer, fontId, "section one-shot HTML inflate");
        streamed = document->streamSection(sectionIndex, tmpHtml, htmlStreamChunkSize);
      }
      fileSize = tmpHtml.size();
      // Explicitly close() file before calling Storage.remove()
      tmpHtml.close();

      // If streaming failed, remove the incomplete file immediately
      if (!streamed && Storage.exists(tmpHtmlPath.c_str())) {
        Storage.remove(tmpHtmlPath.c_str());
        LOG_DBG("SCT", "Removed incomplete temp file after failed attempt");
      }
    }

    if (!streamed) {
      LOG_ERR("SCT", "Failed to stream item contents to temp file after retries");
      return false;
    }

    LOG_DBG("SCT", "Streamed temp HTML to %s (%d bytes, free=%u, maxAlloc=%u)", tmpHtmlPath.c_str(), fileSize,
            ESP.getFreeHeap(), ESP.getMaxAllocHeap());

    // Promote to the persistent HTML cache immediately -- the inflate is complete, and caching it
    // lets future section rebuilds skip re-inflation. If the rename fails we just parse the temp.
    if (Storage.rename(tmpHtmlPath.c_str(), htmlPath.c_str())) {
      htmlCached = true;
      tempHtmlMayDelete = false;
    } else {
      LOG_DBG("SCT", "Failed to promote HTML cache; parsing from temp");
    }
  }
  const std::string& parsePath =
      usesBorrowedSection ? borrowedSection.localPath : (htmlCached ? htmlPath : tmpHtmlPath);

  if (Storage.exists(tmpSectionPath.c_str())) {
    Storage.remove(tmpSectionPath.c_str());
  }

  if (!Storage.openFileForWrite("SCT", tmpSectionPath, file)) {
    cleanupTempHtml();
    return false;
  }
  if (!writeSectionFileHeader(fontId, lineCompression, extraParagraphSpacing, forceParagraphIndents, paragraphAlignment,
                              viewportWidth, viewportHeight, hyphenationEnabled, embeddedStyle, imageRendering,
                              effectiveBionicReadingEnabled, effectiveGuideReadingEnabled, renderMode)) {
    LOG_ERR("SCT", "Failed to write section header");
    file.close();
    Storage.remove(tmpSectionPath.c_str());
    cleanupTempHtml();
    return false;
  }
  // 1024 entries is 8 KB. Stack is too small, and std::vector growth in the page callback can abort on OOM.
  uint16_t lutCapacity = INITIAL_SECTION_PAGE_LUT_ENTRIES;
  auto lut = makeUniqueNoThrow<PageLutEntry[]>(lutCapacity);
  if (!lut) {
    LOG_ERR("SCT", "Failed to allocate page LUT (%u bytes)", static_cast<unsigned>(sizeof(PageLutEntry) * lutCapacity));
    if (layoutAbortedForLowMemory) *layoutAbortedForLowMemory = true;
    file.close();
    Storage.remove(tmpSectionPath.c_str());
    cleanupTempHtml();
    return false;
  }
  uint16_t lutCount = 0;
  bool pageCompletionFailed = false;

  // Derive the content base directory and image cache path prefix for the parser
  size_t lastSlash = localPath.find_last_of('/');
  std::string contentBase = (lastSlash != std::string::npos) ? localPath.substr(0, lastSlash + 1) : "";
  std::string imageBasePath = document->getCachePath() + "/img_" + std::to_string(sectionIndex) + "_";

  CssParser* cssParser = nullptr;
  if (embeddedStyle) {
    cssParser = document->getCssParser();
    if (cssParser) {
      const auto cssHeapBefore = MemoryBudget::snapshot();
      const bool cssLoaded = cssParser->loadFromCache();
      const auto cssHeapAfter = MemoryBudget::snapshot();
      LOG_DBG("SCT", "CSS cache load: ok=%u partial=%u rules=%u free=%u->%u delta=%d maxAlloc=%u->%u delta=%d",
              cssLoaded ? 1U : 0U, cssParser->isCachePartial() ? 1U : 0U, static_cast<unsigned>(cssParser->ruleCount()),
              cssHeapBefore.freeHeap, cssHeapAfter.freeHeap,
              static_cast<int32_t>(cssHeapAfter.freeHeap) - static_cast<int32_t>(cssHeapBefore.freeHeap),
              cssHeapBefore.maxAllocHeap, cssHeapAfter.maxAllocHeap,
              static_cast<int32_t>(cssHeapAfter.maxAllocHeap) - static_cast<int32_t>(cssHeapBefore.maxAllocHeap));
      if (!cssLoaded) {
        LOG_ERR("SCT", "Failed to load CSS from cache");
      }
    }
  }

  // Collect TOC anchors for this spine so the parser can insert page breaks at chapter boundaries
  std::vector<std::string> tocAnchors;
  const int startTocIndex = buildOptions.isPreview() ? -1 : document->getTocIndexForSectionIndex(sectionIndex);
  if (startTocIndex >= 0) {
    for (int i = startTocIndex; i < document->getTocEntryCount(); i++) {
      auto entry = document->getTocEntry(i);
      if (entry.sectionIndex != sectionIndex) break;
      if (!entry.anchor.empty()) {
        tocAnchors.push_back(std::move(entry.anchor));
      }
    }
  }

  const bool semanticPositionEnabled = usesPdfWordIndex() && !buildOptions.isPreview();
  std::unique_ptr<PdfPageBuildContext> pdfBuild;
  std::function<void(std::unique_ptr<Page>, uint16_t, uint16_t)> completePage;
  ChapterHtmlPaginationHooks paginationHooks;
  if (semanticPositionEnabled) {
    const ReflowSectionInfo sectionInfo = document->getSectionInfo(sectionIndex);
    // This single PDF-only allocation keeps the writer, counter, and fixed path
    // buffers alive across parser callbacks without enlarging Section or parser.
    pdfBuild = makeUniqueNoThrow<PdfPageBuildContext>(*this, lut, lutCapacity, lutCount, pageCompletionFailed,
                                                      layoutAbortedForLowMemory);
    if (!pdfBuild || !pdfBuild->initialize(filePath, sectionIndex, sectionInfo)) {
      LOG_ERR("SCT", "Failed to allocate or initialize PDF semantic build context");
      if (!pdfBuild && layoutAbortedForLowMemory) {
        *layoutAbortedForLowMemory = true;
      }
      file.close();
      Storage.remove(tmpSectionPath.c_str());
      cleanupTempHtml();
      return false;
    }
    static const ChapterHtmlPaginationVtable pdfPaginationVtable = {
        completePdfPage,
        finishPdfTextBlock,
        beginPdfTextBlock,
        trackPdfTextLine,
    };
    paginationHooks = {pdfBuild.get(), &pdfPaginationVtable};
  } else {
    completePage = [this, &lut, &lutCapacity, &lutCount, &pageCompletionFailed, layoutAbortedForLowMemory](
                       std::unique_ptr<Page> page, const uint16_t paragraphIndex, const uint16_t listItemIndex) {
      if (pageCompletionFailed) {
        return;
      }
      if (lutCount == UINT16_MAX) {
        LOG_ERR("SCT", "Section page count exceeded cache format limit");
        pageCompletionFailed = true;
        return;
      }
      if (!ensurePageLutCapacity(lut, lutCapacity, lutCount)) {
        LOG_ERR("SCT", "Failed to grow section page LUT from %u entries", lutCapacity);
        if (layoutAbortedForLowMemory) {
          *layoutAbortedForLowMemory = true;
        }
        pageCompletionFailed = true;
        return;
      }
      const uint32_t fileOffset = onPageComplete(std::move(page));
      if (fileOffset == 0) {
        pageCompletionFailed = true;
        return;
      }
      lut[lutCount++] = {fileOffset, paragraphIndex, listItemIndex};
    };
  }

  ChapterHtmlSlimParser visitor(*document, sectionIndex, parsePath, renderer, fontId, lineCompression,
                                extraParagraphSpacing, forceParagraphIndents, paragraphAlignment, viewportWidth,
                                viewportHeight, hyphenationEnabled, effectiveBionicReadingEnabled,
                                effectiveGuideReadingEnabled, std::move(completePage), embeddedStyle, contentBase,
                                imageBasePath, imageRendering, std::move(tocAnchors), popupFn, cssParser, renderMode,
                                buildOptions.isPreview() ? std::string(buildOptions.previewAnchor) : std::string{},
                                buildOptions.previewMaxPages, paginationHooks);
  Hyphenator::setPreferredLanguage(document->getLanguage());
  LOG_DBG("SCT", "Parser start: spine=%d free=%u maxAlloc=%u", sectionIndex, ESP.getFreeHeap(), ESP.getMaxAllocHeap());
  const bool success = visitor.parseAndBuildPages();
  LOG_DBG("SCT", "Parser done: spine=%d success=%u pages=%u free=%u maxAlloc=%u", sectionIndex, success, pageCount,
          ESP.getFreeHeap(), ESP.getMaxAllocHeap());

  if (imagesWereSuppressed) *imagesWereSuppressed = visitor.wasLowMemoryFallbackTriggered();
  if (layoutAbortedForLowMemory) {
    *layoutAbortedForLowMemory = *layoutAbortedForLowMemory || visitor.wasLowMemoryAbortTriggered();
  }

  if (tempHtmlMayDelete) {
    if (success || pageCompletionFailed) {
      // Promote the freshly unzipped HTML to the persistent cache so future rebuilds (e.g. after a
      // settings change invalidates the layout caches) can skip zip inflation. If promotion fails,
      // drop the temp file; the section build can still continue from the already-open source.
      if (!Storage.rename(tmpHtmlPath.c_str(), htmlPath.c_str())) {
        LOG_DBG("SCT", "Failed to promote HTML cache, removing temp");
        Storage.remove(tmpHtmlPath.c_str());
      }
      tempHtmlMayDelete = false;
    } else {
      // Parse failed on a freshly unzipped file -- discard it rather than caching a bad source.
      Storage.remove(tmpHtmlPath.c_str());
      tempHtmlMayDelete = false;
    }
  }

  if (pdfBuild && success && !pageCompletionFailed && !pdfBuild->finishSidecar()) {
    pageCompletionFailed = true;
  }

  if (!success || pageCompletionFailed) {
    LOG_ERR("SCT", "Failed to parse XML and build pages");
    // Explicitly close() file before calling Storage.remove()
    file.close();
    Storage.remove(tmpSectionPath.c_str());
    if (cssParser) {
      cssParser->clear();
    }
    return false;
  }

  const uint32_t lutOffset = file.position();
  bool hasFailedLutRecords = false;
  // Write LUT
  for (uint16_t i = 0; i < lutCount; i++) {
    if (lut[i].fileOffset == 0) {
      hasFailedLutRecords = true;
      break;
    }
    if (!serialization::tryWritePod(file, lut[i].fileOffset)) {
      hasFailedLutRecords = true;
      break;
    }
  }

  if (hasFailedLutRecords) {
    LOG_ERR("SCT", "Failed to write LUT due to invalid page positions");
    // Explicitly close() file before calling Storage.remove()
    file.close();
    Storage.remove(tmpSectionPath.c_str());
    return false;
  }

  // Write anchor-to-page map for fragment navigation (e.g. footnote targets)
  const uint32_t anchorMapOffset = file.position();
  const auto& anchors = visitor.getAnchors();
  if (!serialization::tryWritePod(file, static_cast<uint16_t>(anchors.size()))) {
    file.close();
    Storage.remove(tmpSectionPath.c_str());
    return false;
  }
  for (const auto& [anchor, page] : anchors) {
    if (!serialization::tryWriteString(file, anchor) || !serialization::tryWritePod(file, page)) {
      file.close();
      Storage.remove(tmpSectionPath.c_str());
      return false;
    }
  }

  const uint32_t paragraphLutOffset = file.position();
  if (!serialization::tryWritePod(file, lutCount)) {
    file.close();
    Storage.remove(tmpSectionPath.c_str());
    return false;
  }
  for (uint16_t i = 0; i < lutCount; i++) {
    if (!serialization::tryWritePod(file, lut[i].paragraphIndex)) {
      file.close();
      Storage.remove(tmpSectionPath.c_str());
      return false;
    }
  }

  const uint32_t liLutFileOffset = static_cast<uint32_t>(file.position());
  for (uint16_t i = 0; i < lutCount; i++) {
    if (!serialization::tryWritePod(file, lut[i].listItemIndex)) {
      file.close();
      Storage.remove(tmpSectionPath.c_str());
      return false;
    }
  }

  // Patch header with final pageCount, lutOffset, anchorMapOffset, paragraphLutOffset, and liLutOffset.
  if (!file.seek(HEADER_SIZE - sizeof(uint32_t) * 4 - sizeof(pageCount)) ||
      !serialization::tryWritePod(file, pageCount) || !serialization::tryWritePod(file, lutOffset) ||
      !serialization::tryWritePod(file, anchorMapOffset) || !serialization::tryWritePod(file, paragraphLutOffset) ||
      !serialization::tryWritePod(file, liLutFileOffset) || !file.sync()) {
    LOG_ERR("SCT", "Failed to finalize section cache");
    file.close();
    Storage.remove(tmpSectionPath.c_str());
    if (cssParser) {
      cssParser->clear();
    }
    return false;
  }
  // Explicit close() required: member variable persists beyond function scope
  file.close();
  if (Storage.exists(filePath.c_str())) {
    Storage.remove(filePath.c_str());
  }
  if (!Storage.rename(tmpSectionPath.c_str(), filePath.c_str())) {
    LOG_ERR("SCT", "Failed to promote temp section cache into place");
    Storage.remove(tmpSectionPath.c_str());
    if (cssParser) {
      cssParser->clear();
    }
    return false;
  }
  if (pdfBuild && !pdfBuild->promoteSidecar()) {
    Storage.remove(filePath.c_str());
    if (cssParser) {
      cssParser->clear();
    }
    return false;
  }
  if (pdfBuild && !document->validateLayoutWordIndex(filePath, sectionIndex, pageCount)) {
    LOG_ERR("SCT", "Promoted PDF semantic sidecar failed validation");
    Storage.remove(filePath.c_str());
    document->removeLayoutWordIndex(filePath);
    if (cssParser) {
      cssParser->clear();
    }
    return false;
  }
  if (cssParser) {
    cssParser->clear();
  }
  LOG_DBG("SCT", "Create section done: spine=%d pages=%u free=%u maxAlloc=%u", sectionIndex, pageCount,
          ESP.getFreeHeap(), ESP.getMaxAllocHeap());
  return true;
}

bool Section::hasHtmlCache() const {
  ReflowResource borrowedSection;
  if (document->getImmutableLocalSection(sectionIndex, borrowedSection)) {
    return Storage.exists(borrowedSection.localPath.c_str());
  }
  const std::string htmlPath = document->getCachePath() + "/html/" + std::to_string(sectionIndex) + ".html";
  return Storage.exists(htmlPath.c_str());
}

std::unique_ptr<Page> Section::loadPageFromSectionFile() {
  if (!file) {
    if (!Storage.openFileForRead("SCT", filePath, file)) {
      return nullptr;
    }
  }

  auto closeAndReturnNull = [this]() -> std::unique_ptr<Page> {
    file.close();
    return nullptr;
  };

  if (!file.seek(HEADER_SIZE - sizeof(uint32_t) * 4)) {
    return closeAndReturnNull();
  }
  uint32_t lutOffset;
  if (!serialization::tryReadPod(file, lutOffset) || !file.seek(lutOffset + sizeof(uint32_t) * currentPage)) {
    return closeAndReturnNull();
  }
  uint32_t pagePos;
  if (!serialization::tryReadPod(file, pagePos) || !file.seek(pagePos)) {
    return closeAndReturnNull();
  }

  auto page = Page::deserialize(file);
  file.close();
  return page;
}

std::optional<ReflowPageSemanticRange> Section::getSemanticRangeForPage(const uint16_t page) {
  if (!usesPdfWordIndex() || page >= pageCount) {
    return std::nullopt;
  }
  file.close();
  ReflowPageSemanticRange range;
  return document->readLayoutWordRange(filePath, pageCount, page, range) ? std::optional<ReflowPageSemanticRange>(range)
                                                                         : std::nullopt;
}

std::optional<uint16_t> Section::getPageForSemanticPosition(const char* const blockAnchor,
                                                            const uint32_t blockWordOffset,
                                                            const uint32_t globalWordOrdinal) {
  if (!usesPdfWordIndex()) {
    return std::nullopt;
  }
  file.close();
  uint16_t page = 0;
  return document->findLayoutWordPage(filePath, blockAnchor, blockWordOffset, globalWordOrdinal, page)
             ? std::optional<uint16_t>(page)
             : std::nullopt;
}

std::optional<uint16_t> Section::getPageForSemanticCursor(const uint32_t wordCursor) {
  if (!usesPdfWordIndex()) {
    return std::nullopt;
  }
  file.close();
  uint16_t page = 0;
  return document->findLayoutWordCursor(filePath, wordCursor, page) ? std::optional<uint16_t>(page) : std::nullopt;
}

std::string Section::getTextFromSectionFile() {
  std::string fullText;
  auto p = loadPageFromSectionFile();
  if (p) {
    for (const auto& el : p->elements) {
      if (el->getTag() == TAG_PageLine) {
        const auto& line = static_cast<const PageLine&>(*el);
        if (line.getBlock()) {
          const auto& block = *line.getBlock();
          for (uint16_t i = 0; i < block.wordCount(); i++) {
            if (!fullText.empty()) fullText += " ";
            fullText += block.wordText(i);
          }
        }
      }
    }
  }
  return fullText;
}

std::optional<uint16_t> Section::getCachedPageCount() const {
  FsFile f;
  if (!Storage.openFileForRead("SCT", filePath, f)) {
    return std::nullopt;
  }

  const uint32_t fileSize = f.size();
  if (fileSize < HEADER_SIZE) {
    return std::nullopt;
  }

  uint32_t magic;
  if (!serialization::tryReadPod(f, magic) || magic != SECTION_CACHE_MAGIC) {
    return std::nullopt;
  }
  uint8_t version;
  if (!serialization::tryReadPod(f, version)) {
    return std::nullopt;
  }
  if (version != SECTION_FILE_VERSION) {
    return std::nullopt;
  }

  if (!f.seek(HEADER_SIZE - sizeof(uint32_t) * 4 - sizeof(uint16_t))) {
    return std::nullopt;
  }
  uint16_t count;
  if (!serialization::tryReadPod(f, count)) {
    return std::nullopt;
  }
  return count;
}

std::optional<uint16_t> Section::getPageForAnchor(const std::string& anchor) const {
  FsFile f;
  if (!Storage.openFileForRead("SCT", filePath, f)) {
    return std::nullopt;
  }

  const uint32_t fileSize = f.size();
  if (!f.seek(HEADER_SIZE - sizeof(uint32_t) * 3)) {
    return std::nullopt;
  }
  uint32_t anchorMapOffset;
  if (!serialization::tryReadPod(f, anchorMapOffset)) {
    return std::nullopt;
  }
  if (anchorMapOffset == 0 || anchorMapOffset >= fileSize) {
    return std::nullopt;
  }

  if (!f.seek(anchorMapOffset)) {
    return std::nullopt;
  }
  uint16_t count;
  if (!serialization::tryReadPod(f, count)) {
    return std::nullopt;
  }
  for (uint16_t i = 0; i < count; i++) {
    std::string key;
    uint16_t page;
    if (!serialization::tryReadString(f, key) || !serialization::tryReadPod(f, page)) {
      return std::nullopt;
    }
    if (key == anchor) {
      return page;
    }
  }

  return std::nullopt;
}

std::optional<uint16_t> Section::getPageForParagraphIndex(const uint16_t pIndex) const {
  FsFile f;
  if (!Storage.openFileForRead("SCT", filePath, f)) {
    return std::nullopt;
  }

  const uint32_t fileSize = f.size();
  if (!f.seek(HEADER_SIZE - sizeof(uint32_t) * 2)) {
    return std::nullopt;
  }
  uint32_t paragraphLutOffset;
  if (!serialization::tryReadPod(f, paragraphLutOffset)) {
    return std::nullopt;
  }
  if (paragraphLutOffset == 0 || paragraphLutOffset >= fileSize) {
    return std::nullopt;
  }

  if (!f.seek(paragraphLutOffset)) {
    return std::nullopt;
  }
  uint16_t count;
  if (!serialization::tryReadPod(f, count)) {
    return std::nullopt;
  }
  if (count == 0) {
    return std::nullopt;
  }

  const uint32_t lutEnd = paragraphLutOffset + sizeof(uint16_t) + count * sizeof(uint16_t);
  if (lutEnd > fileSize) {
    return std::nullopt;
  }

  uint16_t resultPage = count - 1;
  for (uint16_t i = 0; i < count; i++) {
    uint16_t pagePIdx;
    if (!serialization::tryReadPod(f, pagePIdx)) {
      return std::nullopt;
    }
    if (pagePIdx >= pIndex) {
      resultPage = i;
      break;
    }
  }

  return resultPage;
}

std::optional<uint16_t> Section::getParagraphIndexForPage(const uint16_t page) const {
  FsFile f;
  if (!Storage.openFileForRead("SCT", filePath, f)) {
    return std::nullopt;
  }

  const uint32_t fileSize = f.size();
  if (!f.seek(HEADER_SIZE - sizeof(uint32_t) * 2)) {
    return std::nullopt;
  }
  uint32_t paragraphLutOffset;
  if (!serialization::tryReadPod(f, paragraphLutOffset)) {
    return std::nullopt;
  }
  if (paragraphLutOffset == 0 || paragraphLutOffset >= fileSize) {
    return std::nullopt;
  }

  if (!f.seek(paragraphLutOffset)) {
    return std::nullopt;
  }
  uint16_t count;
  if (!serialization::tryReadPod(f, count)) {
    return std::nullopt;
  }
  if (count == 0 || page >= count) {
    return std::nullopt;
  }

  const uint32_t entryEnd = paragraphLutOffset + sizeof(uint16_t) + (page + 1) * sizeof(uint16_t);
  if (entryEnd > fileSize) {
    return std::nullopt;
  }

  if (!f.seek(paragraphLutOffset + sizeof(uint16_t) + page * sizeof(uint16_t))) {
    return std::nullopt;
  }
  uint16_t pIdx;
  if (!serialization::tryReadPod(f, pIdx)) {
    return std::nullopt;
  }
  return pIdx;
}

std::optional<uint16_t> Section::getListItemIndexForPage(const uint16_t page) const {
  FsFile f;
  if (!Storage.openFileForRead("SCT", filePath, f)) {
    return std::nullopt;
  }

  const uint32_t fileSize = f.size();
  if (!f.seek(HEADER_SIZE - sizeof(uint32_t))) {
    return std::nullopt;
  }
  uint32_t liLutOffset;
  if (!serialization::tryReadPod(f, liLutOffset)) {
    return std::nullopt;
  }
  if (liLutOffset == 0 || liLutOffset >= fileSize) {
    return std::nullopt;
  }

  if (!f.seek(HEADER_SIZE - sizeof(uint32_t) * 2)) {
    return std::nullopt;
  }
  uint32_t paragraphLutOffset;
  if (!serialization::tryReadPod(f, paragraphLutOffset)) {
    return std::nullopt;
  }
  if (paragraphLutOffset == 0 || paragraphLutOffset >= fileSize) {
    return std::nullopt;
  }

  if (!f.seek(paragraphLutOffset)) {
    return std::nullopt;
  }
  uint16_t count;
  if (!serialization::tryReadPod(f, count)) {
    return std::nullopt;
  }
  if (count == 0 || page >= count) {
    return std::nullopt;
  }

  const uint32_t entryEnd = liLutOffset + (page + 1) * sizeof(uint16_t);
  if (entryEnd > fileSize) {
    return std::nullopt;
  }

  if (!f.seek(liLutOffset + page * sizeof(uint16_t))) {
    return std::nullopt;
  }
  uint16_t liIdx;
  if (!serialization::tryReadPod(f, liIdx)) {
    return std::nullopt;
  }
  return liIdx;
}

std::optional<uint16_t> Section::getPageForListItemIndex(const uint16_t liIndex) const {
  FsFile f;
  if (!Storage.openFileForRead("SCT", filePath, f)) {
    return std::nullopt;
  }

  const uint32_t fileSize = f.size();
  if (!f.seek(HEADER_SIZE - sizeof(uint32_t))) {
    return std::nullopt;
  }
  uint32_t liLutOffset;
  if (!serialization::tryReadPod(f, liLutOffset)) {
    return std::nullopt;
  }
  if (liLutOffset == 0 || liLutOffset >= fileSize) {
    return std::nullopt;
  }

  // The li LUT shares count with the paragraph LUT; read count from paragraphLutOffset
  if (!f.seek(HEADER_SIZE - sizeof(uint32_t) * 2)) {
    return std::nullopt;
  }
  uint32_t paragraphLutOffset;
  if (!serialization::tryReadPod(f, paragraphLutOffset)) {
    return std::nullopt;
  }
  if (paragraphLutOffset == 0 || paragraphLutOffset >= fileSize) {
    return std::nullopt;
  }

  if (!f.seek(paragraphLutOffset)) {
    return std::nullopt;
  }
  uint16_t count;
  if (!serialization::tryReadPod(f, count)) {
    return std::nullopt;
  }
  if (count == 0) {
    return std::nullopt;
  }

  const uint32_t lutEnd = liLutOffset + count * sizeof(uint16_t);
  if (lutEnd > fileSize) {
    return std::nullopt;
  }

  if (!f.seek(liLutOffset)) {
    return std::nullopt;
  }
  uint16_t resultPage = count - 1;
  for (uint16_t i = 0; i < count; i++) {
    uint16_t pageLiIdx;
    if (!serialization::tryReadPod(f, pageLiIdx)) {
      return std::nullopt;
    }
    if (pageLiIdx >= liIndex) {
      resultPage = i;
      break;
    }
  }

  return resultPage;
}
