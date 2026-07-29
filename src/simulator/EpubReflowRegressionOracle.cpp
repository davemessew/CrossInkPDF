#ifdef SIMULATOR

#include "EpubReflowRegressionOracle.h"

#include <ArduinoJson.h>
#include <Epub.h>
#include <Epub/Page.h>
#include <Epub/Section.h>
#include <Epub/parsers/ChapterHtmlSlimParser.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
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
                           std::string imagePath)
      : documentPath_(std::move(documentPath)),
        cachePath_(std::move(cachePath)),
        sectionPath_(std::move(sectionPath)),
        imagePath_(std::move(imagePath)),
        imageHref_(imagePath_.empty() || imagePath_.front() != '/' ? imagePath_ : imagePath_.substr(1)) {}

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
  uint32_t getTotalWordCount() const override { return 0; }
  bool loadReadingPosition(ReflowReadingPosition&) const override { return false; }
  bool saveReadingPosition(const ReflowReadingPosition&) const override { return false; }

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

void pinReaderSettings(GfxRenderer& renderer) {
  RenderLock renderLock;
  SETTINGS.uiTheme = CrossPointSettings::UI_THEME::CLASSIC;
  SETTINGS.fontFamily = CrossPointSettings::FONT_FAMILY::LEXENDDECA;
  SETTINGS.fontSize = CrossPointSettings::FONT_SIZE::MEDIUM;
  SETTINGS.lineHeightPercent = 100;
  SETTINGS.orientation = CrossPointSettings::ORIENTATION::PORTRAIT;
  SETTINGS.screenMargin = 5;
  SETTINGS.publisherPageNumbers = 0;
  SETTINGS.paragraphAlignment = CrossPointSettings::PARAGRAPH_ALIGNMENT::BOOK_STYLE;
  SETTINGS.embeddedStyle = 1;
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
                                const FrameOracle& middle, JsonDocument& oracle, std::string& error) {
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
  if (BOOKMARKS.addBookmark(static_cast<uint16_t>(middle.sectionIndex), sectionProgress, middle.pageCount,
                            chapter.c_str(), UINT16_MAX, middle.text.c_str()) != BookmarkStore::AddResult::Added) {
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

  std::string xhtml =
      "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
      "<html xmlns=\"http://www.w3.org/1999/xhtml\"><body>"
      "<h1 id=\"preview\">Loose local source</h1>"
      "<p>This XHTML is immutable generation-owned input.</p>"
      "<img src=\"invalid.jpg\" alt=\"invalid borrowed image\"/>";
  for (int i = 0; i < 240; ++i) {
    xhtml += "<p>Preview cancellation must leave this borrowed source unchanged.</p>";
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

  auto document = std::make_shared<LooseLocalReflowDocument>(root + "/fixture.pdf", cachePath, sourcePath, imagePath);
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
    error = "borrowed local source was copied, streamed, or mutated";
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

  pinReaderSettings(renderer);
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
  if (!epub->load(true, false)) {
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

  if (!captureProgressAndBookmark(epub, renderer, layout, middle, oracle, error)) {
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
