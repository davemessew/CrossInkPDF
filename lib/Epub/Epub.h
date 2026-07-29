#pragma once

#include <Print.h>
#include <ReflowDocument.h>

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "Epub/BookMetadataCache.h"
#include "Epub/css/CssParser.h"

class ZipFile;
class GfxRenderer;

class Epub : public ReflowDocument {
  // the ncx file (EPUB 2)
  std::string tocNcxItem;
  // the nav file (EPUB 3)
  std::string tocNavItem;
  // where is the EPUBfile?
  std::string filepath;
  // the base path for items in the EPUB file
  std::string contentBasePath;
  // Stable cache path based on filepath
  std::string cachePath;
  // Spine and TOC cache
  std::unique_ptr<BookMetadataCache> bookMetadataCache;
  // CSS parser for styling
  std::unique_ptr<CssParser> cssParser;
  // CSS files
  std::vector<std::string> cssFiles;
  struct LocationSpineEntry {
    uint32_t startLocation = 0;
    uint32_t endLocation = 0;
    uint32_t wordStart = 0;
    uint32_t wordCount = 0;
  };
  std::vector<LocationSpineEntry> locationSpine;
  uint32_t totalLocations = 0;
  uint32_t totalWords = 0;
  uint32_t wordsPerReferencePage = 0;
  uint32_t totalReferencePages = 0;
  bool xLocationsLoaded = false;
  enum class CssParseStatus : uint8_t {
    Failed,
    Partial,
    Complete,
  };

  void migrateLegacyCachePath(const std::string& cacheDir) const;
  bool findContentOpfFile(std::string* contentOpfFile) const;
  bool parseContentOpf(BookMetadataCache::BookMetadata& bookMetadata, bool writeSpineEntries = true);
  bool parseTocNcxFile() const;
  bool parseTocNavFile() const;
  CssParseStatus parseCssFiles(bool forceRebuild = false) const;
  void discoverCssFilesFromZip();

 public:
  explicit Epub(std::string filepath, const std::string& cacheDir);
  ~Epub() override = default;
  ReflowDocumentFormat getFormat() const override;
  const char* getStoreFormatKey() const override;
  ReflowCapabilitySet getCapabilities() const override;
  static std::string cachePathForFilePath(const std::string& filepath, const std::string& cacheDir);
  // True when a metadata cache already exists for this book, i.e. load() will
  // hit the fast path instead of rebuilding. Cheap: no parsing, just a stat.
  static bool hasCache(const std::string& filepath, const std::string& cacheDir);
  std::string& getBasePath() { return contentBasePath; }
  bool load(bool buildIfMissing = true, bool skipLoadingCss = false);
  bool clearCache() const;
  void setupCacheDir() const;
  const std::string& getCachePath() const override;
  const std::string& getPath() const override;
  const std::string& getTitle() const override;
  const std::string& getAuthor() const override;
  const std::string& getLanguage() const override;
  std::string getCoverBmpPath(bool cropped = false) const override;
  bool generateCoverBmp(bool cropped = false, const GfxRenderer* renderer = nullptr,
                        int readerFontId = 0) const override;
  std::string getThumbBmpPath() const override;
  // Deprecated compatibility wrapper; forwards to getThumbBmpPath(0, height).
  [[deprecated("use getThumbBmpPath(int width, int height)")]]
  std::string getThumbBmpPath(int height) const;
  // Returns the thumbnail cache path. width <= 0 derives the default 3:5
  // (width:height) thumbnail width from height; height <= 0 uses the default
  // thumbnail height.
  std::string getThumbBmpPath(int width, int height) const override;
  // Returns a Minimal-style adaptive thumbnail path. Normal cover ratios fill
  // the requested box; unusual ratios are contained inside the box.
  std::string getAdaptiveThumbBmpPath(int width, int height) const override;
  // Deprecated compatibility wrapper; forwards to generateThumbBmp(0, height).
  [[deprecated("use generateThumbBmp(int width, int height)")]]
  bool generateThumbBmp(int height, const GfxRenderer* renderer = nullptr, int readerFontId = 0) const;
  // Writes a thumbnail BMP to cache. width <= 0 derives the default 3:5
  // (width:height) thumbnail width from height; height <= 0 uses the default
  // thumbnail height.
  // Returns false on missing cache/cover, unsupported image format, or conversion failure.
  bool generateThumbBmp(int width, int height, const GfxRenderer* renderer = nullptr,
                        int readerFontId = 0) const override;
  // Writes a thumbnail that can either crop-to-fill or contain unusual cover
  // ratios, depending on the source image dimensions.
  bool generateAdaptiveThumbBmp(int width, int height, const GfxRenderer* renderer = nullptr,
                                int readerFontId = 0) const override;
  uint8_t* readItemContentsToBytes(const std::string& itemHref, size_t* size = nullptr,
                                   bool trailingNullByte = false) const;
  bool readItemContentsToStream(const std::string& itemHref, Print& out, size_t chunkSize) const;
  bool getItemSize(const std::string& itemHref, size_t* size) const;
  BookMetadataCache::SpineEntry getSpineItem(int spineIndex) const;
  BookMetadataCache::TocEntry getTocItem(int tocIndex) const;
  int getSpineItemsCount() const;
  int getTocItemsCount() const;
  int getSpineIndexForTocIndex(int tocIndex) const;
  int getTocIndexForSpineIndex(int spineIndex) const;
  size_t getCumulativeSpineItemSize(int spineIndex) const;
  int getSpineIndexForTextReference() const;

  size_t getBookSize() const;
  bool hasXLocations() const { return xLocationsLoaded; }
  bool hasStablePageNumbers() const {
    return xLocationsLoaded && totalWords > 0 && wordsPerReferencePage > 0 && totalReferencePages > 0;
  }
  float calculateSizeProgress(int currentSpineIndex, float currentSpineRead) const override;
  float calculateProgress(int currentSpineIndex, float currentSpineRead) const override;
  bool resolveLocationPercentToSpineProgress(int percent, int& spineIndex, float& spineProgress) const;
  bool resolveReferencePage(int currentSpineIndex, float currentSpineRead, uint32_t& currentPage,
                            uint32_t& pageCount) const override;
  CssParser* getCssParser() const override { return cssParser.get(); }
  int resolveHrefToSpineIndex(const std::string& href) const;

  bool getLocalSectionPath(int sectionIndex, ReflowResource& out) const override;
  bool streamSection(int sectionIndex, Print& out, size_t chunkSize) const override;
  bool resolveResource(int sectionIndex, const std::string& href, ReflowResource& out) const override;
  bool streamResource(int sectionIndex, const std::string& href, Print& out, size_t chunkSize) const override;
  bool getResourceSize(int sectionIndex, const std::string& href, size_t* size) const override;

  int getSectionCount() const override;
  ReflowSectionInfo getSectionInfo(int sectionIndex) const override;
  bool getSectionSize(int sectionIndex, size_t* size) const override;
  size_t getCumulativeSectionSize(int sectionIndex) const override;
  size_t getDocumentSize() const override;
  int getSectionIndexForTextReference() const override;

  int getTocEntryCount() const override;
  ReflowTocEntry getTocEntry(int tocIndex) const override;
  int getSectionIndexForTocIndex(int tocIndex) const override;
  int getTocIndexForSectionIndex(int sectionIndex) const override;
  int resolveHrefToSectionIndex(const std::string& href) const override;

  bool resolveProgressPercentToSection(int percent, int& sectionIndex, float& sectionProgress) const override;
  bool hasStableReferencePages() const override;
  uint32_t getTotalWordCount() const override;
  bool loadReadingPosition(ReflowReadingPosition& position) const override;
  bool saveReadingPosition(const ReflowReadingPosition& position) const override;

 private:
  bool loadXLocations();
  std::string getCachedCoverImagePath(const std::string& coverImageHref) const;
  bool ensureCachedCoverImage(const std::string& coverImageHref, std::string& outPath) const;
  bool generateThumbBmpInternal(int width, int height, bool adaptiveContain, const GfxRenderer* renderer,
                                int readerFontId) const;
};
