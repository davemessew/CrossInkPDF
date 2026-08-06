#ifdef SIMULATOR

#include "EpubReflowRegressionOracle.h"

#include <ArduinoJson.h>
#include <Epub.h>
#include <Epub/Page.h>
#include <Epub/Section.h>
#include <Epub/parsers/ChapterHtmlSlimParser.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <PdfHalIo.h>
#include <PdfLayoutWordIndex.h>
#include <PdfWordCounter.h>
#include <Print.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "BookmarkStore.h"
#include "CrossPointSettings.h"
#include "activities/RenderLock.h"
#include "activities/reader/EpubReaderUtils.h"
#include "activities/reader/ReaderUtils.h"
#include "components/UITheme.h"

namespace {

constexpr char CACHE_ROOT[] = "/.crosspoint";
constexpr char UNCACHED_PASS[] = "uncached";
constexpr char CACHED_PASS[] = "cached";
constexpr char UNCACHED_MARKER[] = "SIM_REFLOW_UNCACHED_PASS ";
constexpr char CACHED_MARKER[] = "SIM_REFLOW_CACHED_PASS ";
constexpr uint64_t FNV_OFFSET = 14695981039346656037ULL;
constexpr uint64_t FNV_PRIME = 1099511628211ULL;
constexpr size_t HASH_BUFFER_SIZE = 256;

class FnvPrint final : public Print {
 public:
  size_t write(const uint8_t byte) override {
    hash_ ^= byte;
    hash_ *= FNV_PRIME;
    ++bytes_;
    return 1;
  }

  size_t write(const uint8_t* buffer, const size_t size) override {
    if (buffer == nullptr) {
      return 0;
    }
    for (size_t i = 0; i < size; ++i) {
      hash_ ^= buffer[i];
      hash_ *= FNV_PRIME;
    }
    bytes_ += size;
    return size;
  }

  void separator() { write(0); }
  uint64_t hash() const { return hash_; }
  size_t bytes() const { return bytes_; }

 private:
  uint64_t hash_ = FNV_OFFSET;
  size_t bytes_ = 0;
};

class LooseLocalReflowDocument final : public ReflowDocument {
 public:
  LooseLocalReflowDocument(std::string documentPath, std::string cachePath, std::string sectionPath,
                           std::string imagePath, const uint32_t wordCount)
      : documentPath_(std::move(documentPath)),
        cachePath_(std::move(cachePath)),
        sectionPath_(std::move(sectionPath)),
        imagePath_(std::move(imagePath)),
        imageHref_(imagePath_),
        wordCount_(wordCount) {}

  ReflowDocumentFormat getFormat() const override { return ReflowDocumentFormat::Pdf; }
  const char* getStoreFormatKey() const override { return "pdf"; }
  ReflowCapabilitySet getCapabilities() const override { return 0; }
  const std::string& getPath() const override { return documentPath_; }
  const std::string& getCachePath() const override { return cachePath_; }
  const std::string& getTitle() const override { return title_; }
  const std::string& getAuthor() const override { return author_; }
  const std::string& getLanguage() const override { return language_; }
  std::string getCoverBmpPath(const bool) const override { return {}; }
  bool generateCoverBmp(const bool, const GfxRenderer*, const int) const override { return false; }
  std::string getThumbBmpPath() const override { return {}; }
  std::string getThumbBmpPath(const int, const int) const override { return {}; }
  std::string getAdaptiveThumbBmpPath(const int, const int) const override { return {}; }
  bool generateThumbBmp(const int, const int, const GfxRenderer*, const int) const override { return false; }
  bool generateAdaptiveThumbBmp(const int, const int, const GfxRenderer*, const int) const override { return false; }

  bool getLocalSectionPath(const int sectionIndex, ReflowResource& out) const override {
    ++localSectionQueries_;
    if (sectionIndex != 0) {
      out = ReflowResource::streamed();
      return false;
    }
    out = ReflowResource::borrowedLocalFile(sectionPath_, ReflowImageKind::EncodedImage);
    return true;
  }

  bool streamSection(const int, Print&, const size_t) const override {
    ++sectionStreams_;
    return false;
  }

  bool resolveResource(const int sectionIndex, const std::string& href, ReflowResource& out) const override {
    ++resourceQueries_;
    if (sectionIndex != 0 || href != imageHref_) {
      out = ReflowResource::streamed();
      return false;
    }
    out = ReflowResource::borrowedLocalFile(imagePath_, ReflowImageKind::EncodedImage);
    return true;
  }

  bool streamResource(const int, const std::string&, Print&, const size_t) const override {
    ++resourceStreams_;
    return false;
  }

  bool getResourceSize(const int, const std::string&, size_t*) const override { return false; }
  CssParser* getCssParser() const override { return nullptr; }
  int getSectionCount() const override { return 1; }

  ReflowSectionInfo getSectionInfo(const int sectionIndex) const override {
    if (sectionIndex != 0) {
      return {};
    }
    return {
        .href = sectionPath_,
        .title = title_,
        .firstWordOrdinal = 0,
        .wordCount = wordCount_,
        .tocIndex = 0,
    };
  }

  bool getSectionSize(const int sectionIndex, size_t* size) const override {
    if (sectionIndex != 0 || size == nullptr) {
      return false;
    }
    *size = 0;
    return true;
  }

  size_t getCumulativeSectionSize(const int) const override { return 0; }
  size_t getDocumentSize() const override { return 0; }
  int getSectionIndexForTextReference() const override { return 0; }
  int getTocEntryCount() const override { return 1; }

  ReflowTocEntry getTocEntry(const int tocIndex) const override {
    if (tocIndex != 0) {
      return {};
    }
    return {
        .title = title_,
        .href = sectionPath_,
        .sectionIndex = 0,
    };
  }

  int getSectionIndexForTocIndex(const int tocIndex) const override { return tocIndex == 0 ? 0 : -1; }
  int getTocIndexForSectionIndex(const int sectionIndex) const override { return sectionIndex == 0 ? 0 : -1; }
  int resolveHrefToSectionIndex(const std::string& href) const override { return href == sectionPath_ ? 0 : -1; }
  float calculateSizeProgress(const int, const float sectionProgress) const override { return sectionProgress; }
  float calculateProgress(const int, const float sectionProgress) const override { return sectionProgress; }

  bool resolveProgressPercentToSection(const int percent, int& sectionIndex, float& sectionProgress) const override {
    sectionIndex = 0;
    sectionProgress = static_cast<float>(percent) / 100.0f;
    return percent >= 0 && percent <= 100;
  }

  bool hasStableReferencePages() const override { return false; }
  bool resolveReferencePage(const int, const float, uint32_t&, uint32_t&) const override { return false; }
  uint32_t getTotalWordCount() const override { return wordCount_; }
  bool loadReadingPosition(ReflowReadingPosition&) const override { return false; }
  bool saveReadingPosition(const ReflowReadingPosition&) const override { return false; }

  bool validateLayoutWordIndex(const std::string& sectionCachePath, const int sectionIndex,
                               const uint16_t pageCount) const override {
    if (sectionIndex != 0) {
      return false;
    }
    HalFile file;
    const std::string path = sectionCachePath + ".pwi";
    if (!Storage.openFileForRead("ORACLE", path, file)) {
      return false;
    }
    PdfLayoutWordIndexInfo info;
    const PdfStatus status = pdfInspectLayoutWordIndex(pdfHalByteSource(file), &info);
    file.close();
    return status.ok() && info.sectionIndex == 0 && info.pageCount == pageCount && info.firstGlobalWordOrdinal == 0 &&
           info.sectionWordCount == wordCount_;
  }

  bool removeLayoutWordIndex(const std::string& sectionCachePath) const override {
    const std::string path = sectionCachePath + ".pwi";
    return !Storage.exists(path.c_str()) || Storage.remove(path.c_str());
  }

  bool readLayoutWordRange(const std::string& sectionCachePath, const uint16_t pageCount, const uint16_t page,
                           ReflowPageSemanticRange& range) const override {
    range = {};
    if (page >= pageCount) {
      return false;
    }
    HalFile file;
    const std::string path = sectionCachePath + ".pwi";
    if (!Storage.openFileForRead("ORACLE", path, file)) {
      return false;
    }
    PdfLayoutWordRange decoded;
    const PdfStatus status = pdfReadLayoutWordRange(pdfHalByteSource(file), page, &decoded);
    file.close();
    if (!status) {
      return false;
    }
    range.firstGlobalWordOrdinal = decoded.firstGlobalWordOrdinal;
    range.lastGlobalWordOrdinal = decoded.lastGlobalWordOrdinal;
    range.firstBlockWordOffset = decoded.firstBlockWordOffset;
    range.wordCursor = decoded.wordCursor;
    range.valid = decoded.valid;
    std::memcpy(range.blockAnchor, decoded.blockAnchor, sizeof(range.blockAnchor));
    return true;
  }

  bool findLayoutWordPage(const std::string& sectionCachePath, const char* blockAnchor, const uint32_t blockWordOffset,
                          const uint32_t globalWordOrdinal, uint16_t& page) const override {
    HalFile file;
    const std::string path = sectionCachePath + ".pwi";
    if (!Storage.openFileForRead("ORACLE", path, file)) {
      return false;
    }
    const PdfByteSource source = pdfHalByteSource(file);
    PdfStatus status = PdfStatus::failure(PdfError::InvalidOffset);
    if (blockAnchor && blockAnchor[0] != '\0') {
      status = pdfFindLayoutAnchor(source, blockAnchor, blockWordOffset, &page);
    }
    if (!status) {
      status = pdfFindLayoutPage(source, globalWordOrdinal, &page);
    }
    file.close();
    return status.ok();
  }

  bool findLayoutWordCursor(const std::string& sectionCachePath, const uint32_t wordCursor,
                            uint16_t& page) const override {
    HalFile file;
    const std::string path = sectionCachePath + ".pwi";
    if (!Storage.openFileForRead("ORACLE", path, file)) {
      return false;
    }
    const PdfStatus status = pdfFindLayoutCursor(pdfHalByteSource(file), wordCursor, &page);
    file.close();
    return status.ok();
  }

  int localSectionQueries() const { return localSectionQueries_; }
  int sectionStreams() const { return sectionStreams_; }
  int resourceQueries() const { return resourceQueries_; }
  int resourceStreams() const { return resourceStreams_; }

 private:
  std::string documentPath_;
  std::string cachePath_;
  std::string sectionPath_;
  std::string imagePath_;
  std::string imageHref_;
  uint32_t wordCount_ = 0;
  std::string title_ = "Loose local XHTML";
  std::string author_ = "CrossInk simulator";
  std::string language_ = "en";
  mutable int localSectionQueries_ = 0;
  mutable int sectionStreams_ = 0;
  mutable int resourceQueries_ = 0;
  mutable int resourceStreams_ = 0;
};

struct OracleLayout {
  int top = 0;
  int right = 0;
  int bottom = 0;
  int left = 0;
  uint16_t width = 0;
  uint16_t height = 0;
};

struct FrameOracle {
  int sectionIndex = 0;
  int pageIndex = 0;
  int pageCount = 0;
  std::string text;
  std::string framebufferHash;
};

std::string hashHex(const uint64_t hash) {
  char buffer[17];
  snprintf(buffer, sizeof(buffer), "%016llX", static_cast<unsigned long long>(hash));
  return buffer;
}

uint64_t hashBytes(const uint8_t* bytes, const size_t size) {
  uint64_t hash = FNV_OFFSET;
  for (size_t i = 0; i < size; ++i) {
    hash ^= bytes[i];
    hash *= FNV_PRIME;
  }
  return hash;
}

bool hashFile(const std::string& path, uint64_t& hash, size_t& bytes, std::string& error) {
  FsFile file;
  if (!Storage.openFileForRead("ORACLE", path, file)) {
    error = "cannot open cache file " + path;
    return false;
  }

  std::array<uint8_t, HASH_BUFFER_SIZE> buffer{};
  while (true) {
    const int read = file.read(buffer.data(), buffer.size());
    if (read < 0) {
      file.close();
      error = "cannot read cache file " + path;
      return false;
    }
    if (read == 0) {
      break;
    }
    for (int i = 0; i < read; ++i) {
      hash ^= buffer[static_cast<size_t>(i)];
      hash *= FNV_PRIME;
    }
    bytes += static_cast<size_t>(read);
  }
  file.close();
  hash ^= 0;
  hash *= FNV_PRIME;
  return true;
}

bool writeOracleFile(const std::string& path, const std::string& contents, std::string& error) {
  FsFile file;
  if (!Storage.openFileForWrite("ORACLE", path, file)) {
    error = "cannot create oracle file " + path;
    return false;
  }
  const size_t written = file.write(contents.data(), contents.size());
  const bool synced = written == contents.size() && file.sync();
  const bool closed = file.close();
  if (!synced || !closed) {
    error = "cannot persist oracle file " + path;
    return false;
  }
  return true;
}

void pinReaderSettings(GfxRenderer& renderer, const bool embeddedStyle) {
  RenderLock renderLock;
  SETTINGS.uiTheme = CrossPointSettings::UI_THEME::CLASSIC;
  SETTINGS.fontFamily = CrossPointSettings::FONT_FAMILY::LEXENDDECA;
  SETTINGS.fontSize = CrossPointSettings::FONT_SIZE::MEDIUM;
  SETTINGS.lineHeightPercent = 100;
  SETTINGS.orientation = CrossPointSettings::ORIENTATION::PORTRAIT;
  SETTINGS.screenMargin = 5;
  SETTINGS.publisherPageNumbers = 0;
  SETTINGS.paragraphAlignment = CrossPointSettings::PARAGRAPH_ALIGNMENT::BOOK_STYLE;
  SETTINGS.embeddedStyle = embeddedStyle ? 1 : 0;
  SETTINGS.hyphenationEnabled = 0;
  SETTINGS.textAntiAliasing = 0;
  SETTINGS.readerDarkMode = 0;
  SETTINGS.imageRendering = CrossPointSettings::IMAGE_RENDERING::IMAGES_DISPLAY;
  SETTINGS.extraParagraphSpacing = 1;
  SETTINGS.forceParagraphIndents = 0;
  SETTINGS.bionicReadingEnabled = 0;
  SETTINGS.guideReadingEnabled = 0;
  SETTINGS.epubRenderMode = 0;
  SETTINGS.sdFontFamilyName[0] = '\0';

  SETTINGS.statusBarChapterPageCount = 0;
  SETTINGS.statusBarBookProgressPercentage = 0;
  SETTINGS.stablePageNumbers = 0;
  SETTINGS.statusBarProgressBar = CrossPointSettings::STATUS_BAR_PROGRESS_BAR::HIDE_PROGRESS;
  SETTINGS.statusBarTitle = CrossPointSettings::STATUS_BAR_TITLE::HIDE_TITLE;
  SETTINGS.statusBarTimeLeft = CrossPointSettings::STATUS_BAR_TIME_LEFT::TIME_LEFT_HIDE;
  SETTINGS.statusBarBattery = 0;
  SETTINGS.hideClock = CrossPointSettings::HIDE_CLOCK_MODE::HIDE_CLOCK_ALWAYS;

  UITheme::getInstance().reload();
  ReaderUtils::applyOrientation(renderer, SETTINGS.orientation);
}

bool computeLayout(GfxRenderer& renderer, OracleLayout& layout, std::string& error) {
  if (renderer.getScreenWidth() != 480 || renderer.getScreenHeight() != 800) {
    error = "oracle requires a 480x800 portrait viewport";
    return false;
  }
  if (UITheme::getStatusBarHeight() != 0 || ReaderUtils::getTopClockStatusBarReservedHeight() != 0) {
    error = "oracle status and clock inputs are not disabled";
    return false;
  }

  renderer.getOrientedViewableTRBL(&layout.top, &layout.right, &layout.bottom, &layout.left);
  layout.top += SETTINGS.screenMargin;
  layout.right += SETTINGS.screenMargin;
  layout.bottom += std::max<int>(SETTINGS.screenMargin, ReaderUtils::STATUS_BAR_TEXT_PADDING);
  layout.left += SETTINGS.screenMargin;
  layout.width = static_cast<uint16_t>(renderer.getScreenWidth() - layout.left - layout.right);
  layout.height = static_cast<uint16_t>(renderer.getScreenHeight() - layout.top - layout.bottom);
  return layout.width > 0 && layout.height > 0;
}

void resetUserPositionState(const Epub& epub) {
  const std::string progress = epub.getCachePath() + "/progress.bin";
  for (const char* suffix : {"", ".bak", ".tmp"}) {
    const std::string path = progress + suffix;
    if (Storage.exists(path.c_str())) {
      Storage.remove(path.c_str());
    }
  }
  BookmarkStore::deleteForFilePath(epub.getPath(), epub.getStoreFormatKey());
}

bool loadOrCreateSection(const std::shared_ptr<Epub>& epub, GfxRenderer& renderer, const OracleLayout& layout,
                         const int sectionIndex, uint16_t& pageCount, std::string& error) {
  Section section(epub, sectionIndex, renderer);
  bool loaded = section.loadSectionFile(
      SETTINGS.getReaderFontId(), SETTINGS.getReaderLineCompression(), SETTINGS.extraParagraphSpacing,
      SETTINGS.forceParagraphIndents, SETTINGS.paragraphAlignment, layout.width, layout.height,
      SETTINGS.hyphenationEnabled, SETTINGS.embeddedStyle, SETTINGS.imageRendering, SETTINGS.bionicReadingEnabled,
      SETTINGS.guideReadingEnabled, EpubRenderMode::CrossInkDefault);
  if (!loaded) {
    loaded = section.createSectionFile(
        SETTINGS.getReaderFontId(), SETTINGS.getReaderLineCompression(), SETTINGS.extraParagraphSpacing,
        SETTINGS.forceParagraphIndents, SETTINGS.paragraphAlignment, layout.width, layout.height,
        SETTINGS.hyphenationEnabled, SETTINGS.embeddedStyle, SETTINGS.imageRendering, SETTINGS.bionicReadingEnabled,
        SETTINGS.guideReadingEnabled, nullptr, nullptr, nullptr, EpubRenderMode::CrossInkDefault);
  }
  if (!loaded || section.pageCount == 0) {
    error = "cannot load or create section " + std::to_string(sectionIndex);
    return false;
  }
  pageCount = section.pageCount;
  return true;
}

bool captureFrame(const std::shared_ptr<Epub>& epub, GfxRenderer& renderer, const OracleLayout& layout,
                  const int sectionIndex, const int pageIndex, FrameOracle& output, std::string& error) {
  Section section(epub, sectionIndex, renderer);
  if (!section.loadSectionFile(SETTINGS.getReaderFontId(), SETTINGS.getReaderLineCompression(),
                               SETTINGS.extraParagraphSpacing, SETTINGS.forceParagraphIndents,
                               SETTINGS.paragraphAlignment, layout.width, layout.height, SETTINGS.hyphenationEnabled,
                               SETTINGS.embeddedStyle, SETTINGS.imageRendering, SETTINGS.bionicReadingEnabled,
                               SETTINGS.guideReadingEnabled, EpubRenderMode::CrossInkDefault)) {
    error = "cannot reopen section " + std::to_string(sectionIndex);
    return false;
  }
  if (pageIndex < 0 || pageIndex >= section.pageCount) {
    error = "page index is outside section " + std::to_string(sectionIndex);
    return false;
  }

  section.currentPage = pageIndex;
  output.text = section.getTextFromSectionFile();
  section.currentPage = pageIndex;
  auto page = section.loadPageFromSectionFile();
  if (!page) {
    error = "cannot deserialize oracle page";
    return false;
  }

  renderer.clearScreen(0xFF);
  page->render(renderer, SETTINGS.getReaderFontId(), layout.left, layout.top, true);
  output.sectionIndex = sectionIndex;
  output.pageIndex = pageIndex;
  output.pageCount = section.pageCount;
  output.framebufferHash = hashHex(hashBytes(renderer.getFrameBuffer(), renderer.getBufferSize()));
  return true;
}

void appendFrame(JsonObject destination, const FrameOracle& frame) {
  destination["section_index"] = frame.sectionIndex;
  destination["page_index"] = frame.pageIndex;
  destination["page_count"] = frame.pageCount;
  destination["text"] = frame.text;
  destination["framebuffer_hash"] = frame.framebufferHash;
}

bool captureProgressAndBookmark(const std::shared_ptr<Epub>& epub, GfxRenderer& renderer, const OracleLayout& layout,
                                 const FrameOracle& middle, const char* bookmarkSnippet, JsonDocument& oracle,
                                 std::string& error) {
  if (!EpubReaderUtils::saveProgress(*epub, middle.sectionIndex, middle.pageIndex, middle.pageCount)) {
    error = "cannot save progress";
    return false;
  }
  ReflowReadingPosition documentProgress;
  if (!epub->loadReadingPosition(documentProgress)) {
    error = "cannot load saved progress through the document seam";
    return false;
  }
  EpubReaderUtils::Progress loadedProgress;
  if (!EpubReaderUtils::loadProgress(*epub, loadedProgress, "ORACLE")) {
    error = "cannot load saved progress";
    return false;
  }
  if (loadedProgress.spineIndex != middle.sectionIndex || loadedProgress.pageNumber != middle.pageIndex ||
      !loadedProgress.hasPageCount || loadedProgress.pageCount != middle.pageCount ||
      documentProgress.sectionIndex != loadedProgress.spineIndex ||
      documentProgress.pageNumber != loadedProgress.pageNumber || !documentProgress.hasPageCount ||
      documentProgress.pageCount != loadedProgress.pageCount) {
    error = "saved progress did not round-trip";
    return false;
  }

  const float sectionProgress =
      static_cast<float>(middle.pageIndex) / static_cast<float>(std::max(1, middle.pageCount));
  JsonObject progress = oracle["progress"].to<JsonObject>();
  progress["section_index"] = loadedProgress.spineIndex;
  progress["page_index"] = loadedProgress.pageNumber;
  progress["page_count"] = loadedProgress.pageCount;
  progress["book_millionths"] =
      static_cast<uint32_t>(std::lround(epub->calculateProgress(middle.sectionIndex, sectionProgress) * 1000000.0f));

  FrameOracle resumed;
  if (!captureFrame(epub, renderer, layout, loadedProgress.spineIndex, loadedProgress.pageNumber, resumed, error)) {
    return false;
  }
  JsonObject resume = oracle["resume"].to<JsonObject>();
  resume["text"] = resumed.text;
  resume["framebuffer_hash"] = resumed.framebufferHash;

  BOOKMARKS.unload();
  BookmarkStore::deleteForFilePath(epub->getPath(), epub->getStoreFormatKey());
  if (!BOOKMARKS.loadForBook(epub->getPath(), epub->getTitle(), epub->getAuthor(), epub->getStoreFormatKey())) {
    error = "cannot initialize bookmark store";
    return false;
  }
  const auto tocIndex = epub->getTocIndexForSectionIndex(middle.sectionIndex);
  const std::string chapter = tocIndex >= 0 ? epub->getTocEntry(tocIndex).title : std::string{};
  const char* selectedSnippet = bookmarkSnippet != nullptr ? bookmarkSnippet : middle.text.c_str();
  if (BOOKMARKS.addBookmark(static_cast<uint16_t>(middle.sectionIndex), sectionProgress, middle.pageCount,
                            chapter.c_str(), UINT16_MAX, selectedSnippet) != BookmarkStore::AddResult::Added) {
    error = "cannot save bookmark";
    BOOKMARKS.unload();
    return false;
  }
  BOOKMARKS.unload();
  if (!BOOKMARKS.loadForBook(epub->getPath(), epub->getTitle(), epub->getAuthor(), epub->getStoreFormatKey()) ||
      BOOKMARKS.getBookmarks().size() != 1) {
    error = "saved bookmark did not round-trip";
    BOOKMARKS.unload();
    return false;
  }

  const Bookmark& bookmark = BOOKMARKS.getBookmarks().front();
  if (bookmarkSnippet != nullptr && std::strcmp(bookmark.snippet, bookmarkSnippet) != 0) {
    error = "explicit bookmark snippet did not round-trip";
    BOOKMARKS.clearAll();
    BOOKMARKS.unload();
    return false;
  }
  JsonObject bookmarkJson = oracle["bookmark"].to<JsonObject>();
  bookmarkJson["section_index"] = bookmark.spineIndex;
  bookmarkJson["page_count"] = middle.pageCount;
  bookmarkJson["progress_millionths"] = static_cast<uint32_t>(std::lround(bookmark.progress * 1000000.0f));
  bookmarkJson["chapter"] = bookmark.chapterTitle;
  bookmarkJson["snippet"] = bookmark.snippet;
  BOOKMARKS.clearAll();
  BOOKMARKS.unload();
  return true;
}

bool verifySplitWordProducer(const std::string& root, const std::string& cachePath, GfxRenderer& renderer,
                             std::string& error) {
  const std::string sourcePath = root + "/generated/split-section.xhtml";
  std::string xhtml =
      "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
      "<html xmlns=\"http://www.w3.org/1999/xhtml\"><body><p id=\"b00000000\">lead ";
  xhtml.append(199, 'A');
  xhtml += " tail</p></body></html>";
  if (!writeOracleFile(sourcePath, xhtml, error)) {
    return false;
  }

  const int fontId = SETTINGS.getReaderFontId();
  const float lineCompression = SETTINGS.getReaderLineCompression();
  const int lineHeight = std::max(1, static_cast<int>(renderer.getLineHeight(fontId) * lineCompression + 0.5F));
  const int textWidth = renderer.getTextWidth(fontId, "AAAAAAAA");
  if (textWidth <= 0 || textWidth > UINT16_MAX || lineHeight <= 0 || lineHeight > UINT16_MAX / 2) {
    error = "split-word producer viewport is outside uint16 bounds";
    return false;
  }

  auto document =
      std::make_shared<LooseLocalReflowDocument>(root + "/split-fixture.pdf", cachePath, sourcePath, "", 3);
  Section oneLine(document, 0, renderer, "_split_one_line");
  if (!oneLine.createSectionFile(
          fontId, lineCompression, false, false, CrossPointSettings::LEFT_ALIGN, static_cast<uint16_t>(textWidth),
          static_cast<uint16_t>(lineHeight), true, false, CrossPointSettings::IMAGES_SUPPRESS, false, false, nullptr,
          nullptr, nullptr, EpubRenderMode::Light) ||
      oneLine.pageCount < 4) {
    error = "split-word producer did not force one-line pagination";
    return false;
  }

  ReflowPageSemanticRange savedSplitRange;
  bool foundExactOverlap = false;
  for (uint16_t page = 0; page + 1U < oneLine.pageCount; ++page) {
    const auto left = oneLine.getSemanticRangeForPage(page);
    const auto right = oneLine.getSemanticRangeForPage(static_cast<uint16_t>(page + 1U));
    if (!left || !right || !left->valid || !right->valid || left->firstGlobalWordOrdinal != 1 ||
        left->lastGlobalWordOrdinal != 1 || right->firstGlobalWordOrdinal != 1 ||
        right->lastGlobalWordOrdinal != 1 || left->wordCursor != 2 || right->wordCursor != 2 ||
        left->firstBlockWordOffset != 1 || right->firstBlockWordOffset != 1 ||
        std::strcmp(left->blockAnchor, "b00000000") != 0 ||
        std::strcmp(right->blockAnchor, "b00000000") != 0) {
      continue;
    }
    const uint32_t overlapFirst = std::max(left->firstGlobalWordOrdinal, right->firstGlobalWordOrdinal);
    const uint32_t overlapLast = std::min(left->lastGlobalWordOrdinal, right->lastGlobalWordOrdinal);
    if (overlapFirst == 1 && overlapLast == 1) {
      savedSplitRange = *right;
      foundExactOverlap = true;
      break;
    }
  }
  if (!foundExactOverlap) {
    error = "producer sidecar did not preserve the exact one-word split overlap";
    return false;
  }

  Section twoLines(document, 0, renderer, "_split_two_lines");
  if (!twoLines.createSectionFile(
          fontId, lineCompression, false, false, CrossPointSettings::LEFT_ALIGN, static_cast<uint16_t>(textWidth),
          static_cast<uint16_t>(lineHeight * 2), true, false, CrossPointSettings::IMAGES_SUPPRESS, false, false,
          nullptr, nullptr, nullptr, EpubRenderMode::Light) ||
      twoLines.pageCount == 0 || twoLines.pageCount >= oneLine.pageCount) {
    error = "split-word relayout did not change pagination";
    return false;
  }

  const auto semanticPage =
      twoLines.getPageForSemanticPosition(savedSplitRange.blockAnchor, savedSplitRange.firstBlockWordOffset,
                                          savedSplitRange.firstGlobalWordOrdinal);
  const auto semanticRange =
      semanticPage ? twoLines.getSemanticRangeForPage(*semanticPage) : std::optional<ReflowPageSemanticRange>{};
  if (!semanticRange || !semanticRange->valid || semanticRange->firstGlobalWordOrdinal > 1 ||
      semanticRange->lastGlobalWordOrdinal < 1 || std::strcmp(semanticRange->blockAnchor, "b00000000") != 0 ||
      semanticRange->firstBlockWordOffset > savedSplitRange.firstBlockWordOffset) {
    error = "split-word semantic tuple did not resume after changed pagination";
    return false;
  }

  const auto cursorPage = twoLines.getPageForSemanticCursor(savedSplitRange.wordCursor);
  const auto cursorRange =
      cursorPage ? twoLines.getSemanticRangeForPage(*cursorPage) : std::optional<ReflowPageSemanticRange>{};
  if (!cursorRange || !cursorRange->valid || cursorRange->firstGlobalWordOrdinal > 2 ||
      cursorRange->lastGlobalWordOrdinal < 2) {
    error = "split-word cursor did not resume after changed pagination";
    return false;
  }
  return true;
}

bool verifyTableSemanticProducer(const std::string& root, const std::string& cachePath, GfxRenderer& renderer,
                                 std::string& error) {
  constexpr uint32_t GRID_ROWS = 6;
  constexpr uint32_t WORDS_PER_CELL = 2;
  constexpr uint32_t GRID_COLUMNS = 2;
  const int fontId = SETTINGS.getReaderFontId();
  const float lineCompression = SETTINGS.getReaderLineCompression();
  const int lineHeight = std::max(1, static_cast<int>(renderer.getLineHeight(fontId) * lineCompression + 0.5F));
  const int viewportWidth = renderer.getScreenWidth() - 40;
  const int fragmentViewportHeight = lineHeight + 13;
  if (viewportWidth <= 80 || viewportWidth > UINT16_MAX || fragmentViewportHeight <= 0 ||
      fragmentViewportHeight > UINT16_MAX) {
    error = "table semantic producer viewport is outside uint16 bounds";
    return false;
  }

  const std::string gridSourcePath = root + "/generated/table-grid.xhtml";
  std::string grid =
      "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
      "<html xmlns=\"http://www.w3.org/1999/xhtml\"><body><table>";
  for (uint32_t row = 0; row < GRID_ROWS; ++row) {
    char leftAnchor[16] = {};
    char rightAnchor[16] = {};
    std::snprintf(leftAnchor, sizeof(leftAnchor), "b%08lu", static_cast<unsigned long>(row * GRID_COLUMNS));
    std::snprintf(rightAnchor, sizeof(rightAnchor), "b%08lu",
                  static_cast<unsigned long>(row * GRID_COLUMNS + 1U));
    grid += "<tr><td id=\"";
    grid += leftAnchor;
    grid += "\">left alpha</td><td id=\"";
    grid += rightAnchor;
    grid += "\">right beta</td></tr>";
  }
  grid += "</table></body></html>";
  if (!writeOracleFile(gridSourcePath, grid, error)) {
    return false;
  }

  constexpr uint32_t GRID_WORD_COUNT = GRID_ROWS * GRID_COLUMNS * WORDS_PER_CELL;
  auto gridDocument =
      std::make_shared<LooseLocalReflowDocument>(root + "/table-grid.pdf", cachePath, gridSourcePath, "",
                                                 GRID_WORD_COUNT);
  Section gridSection(gridDocument, 0, renderer, "_table_grid");
  if (!gridSection.createSectionFile(
          fontId, lineCompression, false, false, CrossPointSettings::LEFT_ALIGN,
          static_cast<uint16_t>(viewportWidth), static_cast<uint16_t>(fragmentViewportHeight), false, false,
          CrossPointSettings::IMAGES_SUPPRESS, false, false, nullptr, nullptr, nullptr,
          EpubRenderMode::CrossInkDefault) ||
      gridSection.pageCount < GRID_ROWS) {
    error = "multi-page table fragment semantic sidecar did not finish";
    return false;
  }
  const auto gridLast =
      gridSection.getSemanticRangeForPage(static_cast<uint16_t>(gridSection.pageCount - 1U));
  if (!gridLast || gridLast->wordCursor != GRID_WORD_COUNT) {
    error = "multi-page table fragment semantic sidecar has an incomplete cursor";
    return false;
  }
  for (uint32_t row = 0; row < GRID_ROWS; ++row) {
    char anchor[16] = {};
    std::snprintf(anchor, sizeof(anchor), "b%08lu", static_cast<unsigned long>(row * GRID_COLUMNS));
    const uint32_t ordinal = row * GRID_COLUMNS * WORDS_PER_CELL;
    const auto page = gridSection.getPageForSemanticPosition(anchor, 0, ordinal);
    const auto range = page ? gridSection.getSemanticRangeForPage(*page) : std::optional<ReflowPageSemanticRange>{};
    if (!range || !range->valid || range->firstGlobalWordOrdinal > ordinal ||
        range->lastGlobalWordOrdinal < ordinal || range->firstBlockWordOffset != 0 ||
        std::strcmp(range->blockAnchor, anchor) != 0) {
      error = "table fragment cell block did not retain its exact anchor and offset";
      return false;
    }
  }

  const int semanticFragmentWidth = renderer.getTextWidth(fontId, "AAAAAAAA") + 12;
  const int semanticFragmentHeight = lineHeight * 8 + 13;
  if (semanticFragmentWidth <= 32 || semanticFragmentWidth > UINT16_MAX || semanticFragmentHeight <= 0 ||
      semanticFragmentHeight > UINT16_MAX) {
    error = "table split-semantic witness viewport is outside uint16 bounds";
    return false;
  }
  const std::string semanticSourcePath = root + "/generated/table-semantic-flags.xhtml";
  constexpr char SEMANTIC_TABLE_XHTML[] =
      "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
      "<html xmlns=\"http://www.w3.org/1999/xhtml\"><body><table>"
      "<tr><td id=\"b00000000\">join<em>ed</em></td></tr>"
      "<tr><td id=\"b00000001\">lead AAAAAAAAAAAAAAAAAAAAAAAAAAAAAA tail</td></tr>"
      "</table></body></html>";
  if (!writeOracleFile(semanticSourcePath, SEMANTIC_TABLE_XHTML, error)) {
    return false;
  }
  constexpr uint32_t SEMANTIC_TABLE_WORD_COUNT = 4;
  auto semanticDocument =
      std::make_shared<LooseLocalReflowDocument>(root + "/table-semantic-flags.pdf", cachePath,
                                                 semanticSourcePath, "", SEMANTIC_TABLE_WORD_COUNT);
  Section semanticSection(semanticDocument, 0, renderer, "_table_semantic_flags");
  if (!semanticSection.createSectionFile(
          fontId, lineCompression, false, false, CrossPointSettings::LEFT_ALIGN,
          static_cast<uint16_t>(semanticFragmentWidth), static_cast<uint16_t>(semanticFragmentHeight), false, false,
          CrossPointSettings::IMAGES_SUPPRESS, false, false, nullptr, nullptr, nullptr,
          EpubRenderMode::CrossInkDefault) ||
      semanticSection.pageCount == 0) {
    error = "table fragment lost inline-attachment or split-continuation semantics";
    return false;
  }
  auto semanticPage = semanticSection.loadPageFromSectionFile();
  const bool usedTableFragment =
      semanticPage &&
      std::any_of(semanticPage->elements.begin(), semanticPage->elements.end(),
                  [](const std::shared_ptr<PageElement>& element) {
                    return element && element->getTag() == TAG_PageTableFragment;
                  });
  const auto semanticLast =
      semanticSection.getSemanticRangeForPage(static_cast<uint16_t>(semanticSection.pageCount - 1U));
  const auto longCellPage = semanticSection.getPageForSemanticPosition("b00000001", 0, 1);
  const auto longCellRange =
      longCellPage ? semanticSection.getSemanticRangeForPage(*longCellPage)
                   : std::optional<ReflowPageSemanticRange>{};
  if (!usedTableFragment || !semanticLast || semanticLast->wordCursor != SEMANTIC_TABLE_WORD_COUNT ||
      !longCellRange || !longCellRange->valid || longCellRange->firstGlobalWordOrdinal > 1 ||
      longCellRange->lastGlobalWordOrdinal < 3) {
    error = "table fragment semantic flags did not preserve the exact four-word cursor";
    return false;
  }

  const std::string fallbackSourcePath = root + "/generated/table-fallback.xhtml";
  constexpr char FALLBACK_XHTML[] =
      "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
      "<html xmlns=\"http://www.w3.org/1999/xhtml\"><body><table>"
      "<tr><td id=\"b00000000\">zero alpha</td><td id=\"b00000001\">one beta</td></tr>"
      "<tr><td id=\"b00000002\" colspan=\"2\">two gamma</td><td id=\"b00000003\">three delta</td></tr>"
      "</table></body></html>";
  if (!writeOracleFile(fallbackSourcePath, FALLBACK_XHTML, error)) {
    return false;
  }
  constexpr uint32_t FALLBACK_WORD_COUNT = 8;
  auto fallbackDocument =
      std::make_shared<LooseLocalReflowDocument>(root + "/table-fallback.pdf", cachePath, fallbackSourcePath, "",
                                                 FALLBACK_WORD_COUNT);
  Section fallbackSection(fallbackDocument, 0, renderer, "_table_fallback");
  if (!fallbackSection.createSectionFile(
          fontId, lineCompression, false, false, CrossPointSettings::LEFT_ALIGN,
          static_cast<uint16_t>(viewportWidth), static_cast<uint16_t>(lineHeight), false, false,
          CrossPointSettings::IMAGES_SUPPRESS, false, false, nullptr, nullptr, nullptr,
          EpubRenderMode::CrossInkDefault) ||
      fallbackSection.pageCount < 4) {
    error = "table paragraph fallback semantic sidecar did not finish";
    return false;
  }
  const auto fallbackLast =
      fallbackSection.getSemanticRangeForPage(static_cast<uint16_t>(fallbackSection.pageCount - 1U));
  if (!fallbackLast || fallbackLast->wordCursor != FALLBACK_WORD_COUNT) {
    error = "table paragraph fallback semantic sidecar has an incomplete cursor";
    return false;
  }
  for (uint32_t cell = 0; cell < 4; ++cell) {
    char anchor[16] = {};
    std::snprintf(anchor, sizeof(anchor), "b%08lu", static_cast<unsigned long>(cell));
    const uint32_t ordinal = cell * WORDS_PER_CELL;
    const auto page = fallbackSection.getPageForSemanticPosition(anchor, 0, ordinal);
    const auto range =
        page ? fallbackSection.getSemanticRangeForPage(*page) : std::optional<ReflowPageSemanticRange>{};
    if (!range || !range->valid || range->firstGlobalWordOrdinal > ordinal ||
        range->lastGlobalWordOrdinal < ordinal || range->firstBlockWordOffset != 0 ||
        std::strcmp(range->blockAnchor, anchor) != 0) {
      error = "table paragraph fallback cell did not retain its exact anchor and offset";
      return false;
    }
  }
  return true;
}

bool verifyLooseLocalSource(const char* passName, GfxRenderer& renderer, const OracleLayout& layout,
                              std::string& error) {
  const std::string root = std::string(CACHE_ROOT) + "/loose_source_" + passName;
  const std::string generatedDir = root + "/generated";
  const std::string cachePath = root + "/cache";
  const std::string sourcePath = generatedDir + "/section-0.xhtml";
  const std::string imagePath = generatedDir + "/invalid.jpg";
  const std::string htmlPath = cachePath + "/html/0.html";
  const std::string tempHtmlPath = cachePath + "/html/.tmp_0.html";

  if (!Storage.mkdir(generatedDir.c_str()) || !Storage.mkdir(cachePath.c_str())) {
    error = "cannot create loose-source oracle directories";
    return false;
  }

  constexpr char HEADING_TEXT[] = "Loose local source";
  constexpr char INTRO_TEXT[] = "This XHTML is immutable generation-owned input.";
  constexpr char IMAGE_FALLBACK_TEXT[] = "[Image: invalid borrowed image]";
  constexpr char REPEATED_TEXT[] = "Preview cancellation must leave this borrowed source unchanged.";
  std::string xhtml =
      "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
      "<html xmlns=\"http://www.w3.org/1999/xhtml\"><body><h1 id=\"preview\">";
  xhtml += HEADING_TEXT;
  xhtml += "</h1><p>";
  xhtml += INTRO_TEXT;
  xhtml += "</p><img src=\"invalid.jpg\" alt=\"invalid borrowed image\"/>";
  for (int i = 0; i < 240; ++i) {
    xhtml += "<p>";
    xhtml += REPEATED_TEXT;
    xhtml += "</p>";
  }
  xhtml += "</body></html>";

  if (!writeOracleFile(sourcePath, xhtml, error) || !writeOracleFile(imagePath, "not a valid jpeg payload", error)) {
    return false;
  }

  uint64_t sourceHashBefore = FNV_OFFSET;
  uint64_t imageHashBefore = FNV_OFFSET;
  size_t sourceBytesBefore = 0;
  size_t imageBytesBefore = 0;
  if (!hashFile(sourcePath, sourceHashBefore, sourceBytesBefore, error) ||
      !hashFile(imagePath, imageHashBefore, imageBytesBefore, error)) {
    return false;
  }

  PdfWordCounter wordCounter;
  if (!wordCounter.consume(reinterpret_cast<const uint8_t*>(HEADING_TEXT), sizeof(HEADING_TEXT) - 1) ||
      !wordCounter.consume(reinterpret_cast<const uint8_t*>(" "), 1) ||
      !wordCounter.consume(reinterpret_cast<const uint8_t*>(INTRO_TEXT), sizeof(INTRO_TEXT) - 1) ||
      !wordCounter.consume(reinterpret_cast<const uint8_t*>(" "), 1) ||
      !wordCounter.consume(reinterpret_cast<const uint8_t*>(IMAGE_FALLBACK_TEXT), sizeof(IMAGE_FALLBACK_TEXT) - 1)) {
    error = "cannot count loose-source semantic words";
    return false;
  }
  for (int i = 0; i < 240; ++i) {
    if (!wordCounter.consume(reinterpret_cast<const uint8_t*>(" "), 1) ||
        !wordCounter.consume(reinterpret_cast<const uint8_t*>(REPEATED_TEXT), sizeof(REPEATED_TEXT) - 1)) {
      error = "cannot count repeated loose-source semantic words";
      return false;
    }
  }
  if (!wordCounter.finish()) {
    error = "cannot finish loose-source semantic word count";
    return false;
  }

  auto document = std::make_shared<LooseLocalReflowDocument>(root + "/fixture.pdf", cachePath, sourcePath, imagePath,
                                                             wordCounter.words());
  Section section(document, 0, renderer, "_loose");
  if (!section.hasHtmlCache() || Storage.exists(htmlPath.c_str()) || Storage.exists(tempHtmlPath.c_str())) {
    error = "borrowed local section was not recognized without an HTML cache copy";
    return false;
  }
  if (!section.createSectionFile(
          SETTINGS.getReaderFontId(), SETTINGS.getReaderLineCompression(), SETTINGS.extraParagraphSpacing,
          SETTINGS.forceParagraphIndents, SETTINGS.paragraphAlignment, layout.width, layout.height,
          SETTINGS.hyphenationEnabled, false, SETTINGS.imageRendering, SETTINGS.bionicReadingEnabled,
          SETTINGS.guideReadingEnabled, nullptr, nullptr, nullptr, EpubRenderMode::CrossInkDefault)) {
    error = "cannot paginate loose local XHTML";
    return false;
  }

  const uint16_t mediumPageCount = section.pageCount;
  const uint16_t mediumSemanticPage = mediumPageCount / 2U;
  const auto mediumSemantic = section.getSemanticRangeForPage(mediumSemanticPage);
  const auto mediumLastSemantic = mediumPageCount == 0
                                      ? std::optional<ReflowPageSemanticRange>{}
                                      : section.getSemanticRangeForPage(static_cast<uint16_t>(mediumPageCount - 1U));
  if (!mediumSemantic || !mediumSemantic->valid || !mediumLastSemantic || !mediumLastSemantic->valid ||
      mediumLastSemantic->lastGlobalWordOrdinal + 1U != wordCounter.words()) {
    error = "medium-font PDF semantic sidecar is incomplete";
    return false;
  }
  {
    RenderLock renderLock;
    SETTINGS.fontSize = CrossPointSettings::FONT_SIZE::LARGE;
    UITheme::getInstance().reload();
  }
  Section largeFontSection(document, 0, renderer, "_font_large");
  const bool largeFontCreated = largeFontSection.createSectionFile(
      SETTINGS.getReaderFontId(), SETTINGS.getReaderLineCompression(), SETTINGS.extraParagraphSpacing,
      SETTINGS.forceParagraphIndents, SETTINGS.paragraphAlignment, layout.width, layout.height,
      SETTINGS.hyphenationEnabled, false, SETTINGS.imageRendering, SETTINGS.bionicReadingEnabled,
      SETTINGS.guideReadingEnabled, nullptr, nullptr, nullptr, EpubRenderMode::CrossInkDefault);
  const uint16_t largePageCount = largeFontSection.pageCount;
  const auto largeSemanticPage = largeFontSection.getPageForSemanticPosition(
      mediumSemantic->blockAnchor, mediumSemantic->firstBlockWordOffset, mediumSemantic->firstGlobalWordOrdinal);
  const auto largeSemantic = largeSemanticPage ? largeFontSection.getSemanticRangeForPage(*largeSemanticPage)
                                               : std::optional<ReflowPageSemanticRange>{};
  const auto largeLastSemantic =
      largePageCount == 0 ? std::optional<ReflowPageSemanticRange>{}
                          : largeFontSection.getSemanticRangeForPage(static_cast<uint16_t>(largePageCount - 1U));
  {
    RenderLock renderLock;
    SETTINGS.fontSize = CrossPointSettings::FONT_SIZE::MEDIUM;
    UITheme::getInstance().reload();
  }
  if (!largeFontCreated || largePageCount == 0 || largePageCount == mediumPageCount || !largeSemantic ||
      !largeSemantic->valid || mediumSemantic->firstGlobalWordOrdinal < largeSemantic->firstGlobalWordOrdinal ||
      mediumSemantic->firstGlobalWordOrdinal > largeSemantic->lastGlobalWordOrdinal || !largeLastSemantic ||
      !largeLastSemantic->valid || largeLastSemantic->lastGlobalWordOrdinal + 1U != wordCounter.words()) {
    error = "device font-size positive control did not alter loose-source pagination";
    return false;
  }

  if (!verifySplitWordProducer(root, cachePath, renderer, error)) {
    return false;
  }
  if (!verifyTableSemanticProducer(root, cachePath, renderer, error)) {
    return false;
  }

  Section previewSection(document, 0, renderer, "_preview");
  const SectionBuildOptions previewOptions = {
      .previewAnchor = "preview",
      .previewMaxPages = 1,
  };
  if (!previewSection.createSectionFile(
          SETTINGS.getReaderFontId(), SETTINGS.getReaderLineCompression(), SETTINGS.extraParagraphSpacing,
          SETTINGS.forceParagraphIndents, SETTINGS.paragraphAlignment, layout.width, layout.height,
          SETTINGS.hyphenationEnabled, false, SETTINGS.imageRendering, SETTINGS.bionicReadingEnabled,
          SETTINGS.guideReadingEnabled, nullptr, nullptr, nullptr, EpubRenderMode::CrossInkDefault, previewOptions)) {
    error = "cannot exercise preview stop on loose local XHTML";
    return false;
  }

  bool lowMemoryAbort = false;
  ChapterHtmlSlimParser::setSimulatorFault(ChapterHtmlSlimParserSimulatorFault::LowMemoryAfterSourceOpen);
  Section lowMemorySection(document, 0, renderer, "_low_memory");
  if (lowMemorySection.createSectionFile(
          SETTINGS.getReaderFontId(), SETTINGS.getReaderLineCompression(), SETTINGS.extraParagraphSpacing,
          SETTINGS.forceParagraphIndents, SETTINGS.paragraphAlignment, layout.width, layout.height,
          SETTINGS.hyphenationEnabled, false, SETTINGS.imageRendering, SETTINGS.bionicReadingEnabled,
          SETTINGS.guideReadingEnabled, nullptr, nullptr, &lowMemoryAbort, EpubRenderMode::CrossInkDefault) ||
      !lowMemoryAbort) {
    error = "loose local XHTML low-memory fault did not abort safely";
    return false;
  }

  ChapterHtmlSlimParser::setSimulatorFault(ChapterHtmlSlimParserSimulatorFault::ParserBufferOom);
  Section oomSection(document, 0, renderer, "_oom");
  if (oomSection.createSectionFile(
          SETTINGS.getReaderFontId(), SETTINGS.getReaderLineCompression(), SETTINGS.extraParagraphSpacing,
          SETTINGS.forceParagraphIndents, SETTINGS.paragraphAlignment, layout.width, layout.height,
          SETTINGS.hyphenationEnabled, false, SETTINGS.imageRendering, SETTINGS.bionicReadingEnabled,
          SETTINGS.guideReadingEnabled, nullptr, nullptr, nullptr, EpubRenderMode::CrossInkDefault)) {
    error = "loose local XHTML parser-buffer OOM did not abort";
    return false;
  }

  uint64_t sourceHashAfter = FNV_OFFSET;
  uint64_t imageHashAfter = FNV_OFFSET;
  size_t sourceBytesAfter = 0;
  size_t imageBytesAfter = 0;
  if (!hashFile(sourcePath, sourceHashAfter, sourceBytesAfter, error) ||
      !hashFile(imagePath, imageHashAfter, imageBytesAfter, error)) {
    return false;
  }

  if (sourceHashAfter != sourceHashBefore || sourceBytesAfter != sourceBytesBefore ||
      imageHashAfter != imageHashBefore || imageBytesAfter != imageBytesBefore || Storage.exists(htmlPath.c_str()) ||
      Storage.exists(tempHtmlPath.c_str()) || document->sectionStreams() != 0 || document->resourceStreams() != 0 ||
      document->localSectionQueries() < 5 || document->resourceQueries() < 1) {
    error = "borrowed local source invariant failed: source_hash=" +
            std::to_string(sourceHashAfter != sourceHashBefore) +
            " source_size=" + std::to_string(sourceBytesAfter != sourceBytesBefore) +
            " image_hash=" + std::to_string(imageHashAfter != imageHashBefore) +
            " image_size=" + std::to_string(imageBytesAfter != imageBytesBefore) +
            " html_copy=" + std::to_string(Storage.exists(htmlPath.c_str())) +
            " html_temp=" + std::to_string(Storage.exists(tempHtmlPath.c_str())) +
            " section_streams=" + std::to_string(document->sectionStreams()) +
            " resource_streams=" + std::to_string(document->resourceStreams()) +
            " section_queries=" + std::to_string(document->localSectionQueries()) +
            " resource_queries=" + std::to_string(document->resourceQueries());
    return false;
  }
  return true;
}

}  // namespace

bool runEpubReflowRegressionOracle(GfxRenderer& renderer, const char* bookPath, const char* passName,
                                   std::string& error) {
  if (bookPath == nullptr || bookPath[0] == '\0') {
    error = "oracle book path is empty";
    return false;
  }
  const bool cachedPass = passName != nullptr && std::strcmp(passName, CACHED_PASS) == 0;
  if (!cachedPass && (passName == nullptr || std::strcmp(passName, UNCACHED_PASS) != 0)) {
    error = "oracle pass must be uncached or cached";
    return false;
  }

  bool embeddedStyle = true;
  if (const char* value = std::getenv("CROSSINK_SIMULATOR_REFLOW_EMBEDDED_STYLE"); value != nullptr) {
    if (std::strcmp(value, "0") == 0) {
      embeddedStyle = false;
    } else if (std::strcmp(value, "1") != 0) {
      error = "oracle embedded style must be 0 or 1";
      return false;
    }
  }

  const char* bookmarkSnippet = std::getenv("CROSSINK_SIMULATOR_REFLOW_BOOKMARK_SNIPPET");
  if (bookmarkSnippet != nullptr) {
    if (embeddedStyle) {
      error = "explicit bookmark snippet is candidate-only";
      return false;
    }
    const size_t snippetLength = std::strlen(bookmarkSnippet);
    if (snippetLength == 0 || snippetLength > BOOKMARK_SNIPPET_MAX - 1) {
      error = "explicit bookmark snippet must contain 1 to 63 ASCII bytes";
      return false;
    }
    for (const char* cursor = bookmarkSnippet; *cursor != '\0'; ++cursor) {
      if (static_cast<unsigned char>(*cursor) > 0x7F) {
        error = "explicit bookmark snippet must be ASCII";
        return false;
      }
    }
  }

  pinReaderSettings(renderer, embeddedStyle);
  OracleLayout layout;
  if (!computeLayout(renderer, layout, error)) {
    return false;
  }

  const bool hadCache = Epub::hasCache(bookPath, CACHE_ROOT);
  if (hadCache != cachedPass) {
    error = cachedPass ? "cached pass started without an EPUB cache" : "uncached pass started with an EPUB cache";
    return false;
  }

  // This oracle is simulator-only and never enters the ESP32-C3 image. Its
  // document lifetime is one deterministic smoke-test process.
  auto epub = std::make_shared<Epub>(bookPath, CACHE_ROOT);
  resetUserPositionState(*epub);
  if (!epub->load(true, !embeddedStyle)) {
    error = "cannot load EPUB metadata";
    return false;
  }

  constexpr ReflowCapabilitySet expectedCapabilities =
      ReflowCapability::ExternalProgressSync | ReflowCapability::NearbyProgressSync |
      ReflowCapability::PublisherRenderModes | ReflowCapability::EmbeddedStyles | ReflowCapability::SavedItems;
  if (epub->getFormat() != ReflowDocumentFormat::Epub || std::strcmp(epub->getStoreFormatKey(), "epub") != 0 ||
      epub->getCapabilities() != expectedCapabilities || !epub->hasCapability(ReflowCapability::ExternalProgressSync) ||
      !epub->hasCapability(ReflowCapability::NearbyProgressSync) ||
      !epub->hasCapability(ReflowCapability::PublisherRenderModes) ||
      !epub->hasCapability(ReflowCapability::EmbeddedStyles) || !epub->hasCapability(ReflowCapability::SavedItems)) {
    error = "EPUB reflow capabilities are not preserved";
    return false;
  }
  if (epub->hasStableReferencePages() != epub->hasStablePageNumbers()) {
    error = "EPUB reference-page capability changed";
    return false;
  }

  const int sectionCount = epub->getSectionCount();
  if (sectionCount <= 0) {
    error = "EPUB has no sections";
    return false;
  }
  std::vector<uint16_t> pageCounts;
  pageCounts.reserve(static_cast<size_t>(sectionCount));
  for (int index = 0; index < sectionCount; ++index) {
    const auto sectionInfo = epub->getSectionInfo(index);
    size_t sectionSize = 0;
    if (sectionInfo.href != epub->getSpineItem(index).href || !epub->getSectionSize(index, &sectionSize) ||
        sectionSize != sectionInfo.byteSize || epub->getCumulativeSectionSize(index) != sectionInfo.cumulativeSize) {
      error = "EPUB generic section metadata changed at " + std::to_string(index);
      return false;
    }
    uint16_t pageCount = 0;
    if (!loadOrCreateSection(epub, renderer, layout, index, pageCount, error)) {
      return false;
    }
    const std::string semanticSidecar = epub->getCachePath() + "/sections/" + std::to_string(index) + ".bin.pwi";
    if (Storage.exists(semanticSidecar.c_str())) {
      error = "EPUB unexpectedly produced a PDF semantic sidecar";
      return false;
    }
    const std::string semanticSidecarTemp = semanticSidecar + ".tmp";
    if (Storage.exists(semanticSidecarTemp.c_str())) {
      error = "EPUB unexpectedly produced a temporary PDF semantic sidecar";
      return false;
    }
    pageCounts.push_back(pageCount);
  }

  FnvPrint xhtmlHash;
  for (int index = 0; index < sectionCount; ++index) {
    ReflowResource localSection;
    if (epub->getLocalSectionPath(index, localSection) || localSection.kind != ReflowResourceKind::Streamed ||
        localSection.paginatorMayDelete || !epub->streamSection(index, xhtmlHash, 1024)) {
      error = "cannot stream XHTML section " + std::to_string(index);
      return false;
    }
    xhtmlHash.separator();
  }

  constexpr char kCssResource[] = "OEBPS/styles/test.css";
  ReflowResource localResource;
  FnvPrint resourceHash;
  size_t resourceSize = 0;
  if (epub->resolveResource(0, kCssResource, localResource) || localResource.kind != ReflowResourceKind::Streamed ||
      localResource.paginatorMayDelete || !epub->streamResource(0, kCssResource, resourceHash, 256) ||
      !epub->getResourceSize(0, kCssResource, &resourceSize) || resourceSize != resourceHash.bytes()) {
    error = "EPUB streamed resource behavior changed";
    return false;
  }

  const int middleSection = sectionCount / 2;
  FrameOracle first;
  FrameOracle middle;
  FrameOracle last;
  if (!captureFrame(epub, renderer, layout, 0, 0, first, error) ||
      !captureFrame(epub, renderer, layout, middleSection, pageCounts[static_cast<size_t>(middleSection)] / 2, middle,
                    error) ||
      !captureFrame(epub, renderer, layout, sectionCount - 1, pageCounts.back() - 1, last, error)) {
    return false;
  }

  uint64_t sectionCacheHash = FNV_OFFSET;
  size_t sectionCacheBytes = 0;
  for (int index = 0; index < sectionCount; ++index) {
    const std::string path = epub->getCachePath() + "/sections/" + std::to_string(index) + ".bin";
    if (!hashFile(path, sectionCacheHash, sectionCacheBytes, error)) {
      return false;
    }
  }

  JsonDocument oracle;
  oracle["schema_version"] = 1;
  JsonObject metadata = oracle["metadata"].to<JsonObject>();
  metadata["title"] = epub->getTitle();
  metadata["author"] = epub->getAuthor();
  metadata["language"] = epub->getLanguage();

  JsonObject settings = oracle["settings"].to<JsonObject>();
  settings["screen_width"] = renderer.getScreenWidth();
  settings["screen_height"] = renderer.getScreenHeight();
  settings["orientation"] = SETTINGS.orientation;
  settings["font_family"] = SETTINGS.fontFamily;
  settings["font_size"] = SETTINGS.fontSize;
  settings["font_id"] = SETTINGS.getReaderFontId();
  settings["line_height_percent"] = SETTINGS.lineHeightPercent;
  settings["screen_margin"] = SETTINGS.screenMargin;
  settings["embedded_style"] = SETTINGS.embeddedStyle;
  settings["hyphenation"] = SETTINGS.hyphenationEnabled;
  settings["text_antialiasing"] = SETTINGS.textAntiAliasing;
  if (bookmarkSnippet != nullptr) {
    settings["bookmark_snippet"] = bookmarkSnippet;
  }

  JsonObject viewport = oracle["viewport"].to<JsonObject>();
  viewport["top"] = layout.top;
  viewport["right"] = layout.right;
  viewport["bottom"] = layout.bottom;
  viewport["left"] = layout.left;
  viewport["width"] = layout.width;
  viewport["height"] = layout.height;

  oracle["section_count"] = sectionCount;
  JsonArray pageCountJson = oracle["section_page_counts"].to<JsonArray>();
  for (const uint16_t count : pageCounts) {
    pageCountJson.add(count);
  }

  JsonArray toc = oracle["selected_toc"].to<JsonArray>();
  const int tocCount = epub->getTocEntryCount();
  const std::array<int, 3> selectedToc = {0, tocCount > 0 ? tocCount / 2 : 0, tocCount > 0 ? tocCount - 1 : 0};
  for (const int index : selectedToc) {
    if (index < 0 || index >= tocCount) {
      error = "EPUB does not have the required TOC entries";
      return false;
    }
    const auto entry = epub->getTocEntry(index);
    JsonObject item = toc.add<JsonObject>();
    item["index"] = index;
    item["title"] = entry.title;
    item["href"] = entry.href;
    item["anchor"] = entry.anchor;
    item["level"] = entry.level;
    item["spine_index"] = entry.sectionIndex;
    item["resolved_spine_index"] = epub->resolveHrefToSectionIndex(entry.href);
  }

  JsonObject stream = oracle["streamed_xhtml"].to<JsonObject>();
  stream["bytes"] = xhtmlHash.bytes();
  stream["fnv1a64"] = hashHex(xhtmlHash.hash());

  JsonObject frames = oracle["frames"].to<JsonObject>();
  appendFrame(frames["first"].to<JsonObject>(), first);
  appendFrame(frames["middle"].to<JsonObject>(), middle);
  appendFrame(frames["last"].to<JsonObject>(), last);

  JsonObject cache = oracle["section_cache"].to<JsonObject>();
  cache["bytes"] = sectionCacheBytes;
  cache["fnv1a64"] = hashHex(sectionCacheHash);

  if (!embeddedStyle) {
    uint64_t representativeCacheHash = FNV_OFFSET;
    size_t representativeCacheBytes = 0;
    const std::string representativeCachePath =
        epub->getCachePath() + "/sections/" + std::to_string(middleSection) + ".bin";
    if (!hashFile(representativeCachePath, representativeCacheHash, representativeCacheBytes, error)) {
      return false;
    }
    JsonObject platformSmoke = oracle["platform_smoke"].to<JsonObject>();
    platformSmoke["section_index"] = middleSection;
    platformSmoke["page_count"] = middle.pageCount;
    platformSmoke["section_cache_bytes"] = representativeCacheBytes;
    platformSmoke["section_cache_fnv1a64"] = hashHex(representativeCacheHash);
    platformSmoke["framebuffer_hash"] = middle.framebufferHash;
  }

  if (!captureProgressAndBookmark(epub, renderer, layout, middle, bookmarkSnippet, oracle, error)) {
    return false;
  }
  if (!verifyLooseLocalSource(passName, renderer, layout, error)) {
    return false;
  }

  std::string serialized;
  serializeJson(oracle, serialized);
  const char* marker = cachedPass ? CACHED_MARKER : UNCACHED_MARKER;
  std::printf("%s%s\n", marker, serialized.c_str());
  std::fflush(stdout);
  return true;
}

#endif
