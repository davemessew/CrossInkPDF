#pragma once

#include <ReflowDocument.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

#include "PdfCacheStore.h"
#include "PdfLayoutWordIndex.h"
#include "PdfMetadataStore.h"
#include "PdfOutline.h"
#include "PdfProgressStore.h"
#include "PdfSavedItemWordMap.h"
#include "PdfSavedItemsStore.h"

enum class PdfCachedResourceKind : uint8_t {
  None,
  PixelCache,
  Jpeg,
};

struct PdfCachedResourceRecord {
  uint64_t contentHash = 0;
  uint64_t encodedLength = 0;
  uint64_t fileSize = 0;
  uint32_t nameCrc32 = 0;
  uint32_t fileCrc32 = 0;
  uint16_t width = 0;
  uint16_t height = 0;
  PdfCachedResourceKind kind = PdfCachedResourceKind::None;
};

static_assert(sizeof(PdfCachedResourceRecord) <= 40, "cached PDF resource records must remain compact");

class PdfReflowDocument : public ReflowDocument {
 public:
  PdfStatus initialize(const PdfCacheIo& io, const char* sourcePath, const char* cacheDirectory,
                       const uint64_t* cacheHashOverride = nullptr);
  PdfStatus loadCompletedCache();
  PdfStatus lastStatus() const { return status_; }
  bool optionalContentWasSkipped() const {
    return (manifest_.warningFlags & PDF_CACHE_WARNING_OPTIONAL_CONTENT_OMITTED) != 0;
  }

  ReflowDocumentFormat getFormat() const override { return ReflowDocumentFormat::Pdf; }
  const char* getStoreFormatKey() const override { return "pdf"; }
  ReflowCapabilitySet getCapabilities() const override {
    return savedItemsReady_ ? reflowCapabilityMask(ReflowCapability::SavedItems) : 0;
  }

  const std::string& getPath() const override { return sourcePath_; }
  const std::string& getCachePath() const override { return cacheRoot_; }
  const std::string& getTitle() const override { return title_; }
  const std::string& getAuthor() const override { return author_; }
  const std::string& getLanguage() const override { return language_; }

  std::string getCoverBmpPath(bool cropped = false) const override;
  bool generateCoverBmp(bool cropped = false, const GfxRenderer* renderer = nullptr,
                        int readerFontId = 0) const override;
  std::string getThumbBmpPath() const override;
  std::string getThumbBmpPath(int width, int height) const override;
  std::string getAdaptiveThumbBmpPath(int width, int height) const override;
  bool generateThumbBmp(int width, int height, const GfxRenderer* renderer = nullptr,
                        int readerFontId = 0) const override;
  bool generateAdaptiveThumbBmp(int width, int height, const GfxRenderer* renderer = nullptr,
                                int readerFontId = 0) const override;

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

  float calculateSizeProgress(int sectionIndex, float sectionProgress) const override;
  float calculateProgress(int sectionIndex, float sectionProgress) const override;
  bool resolveProgressPercentToSection(int percent, int& sectionIndex, float& sectionProgress) const override;
  bool hasStableReferencePages() const override { return false; }
  bool resolveReferencePage(int sectionIndex, float sectionProgress, uint32_t& currentPage,
                            uint32_t& pageCount) const override;
  uint32_t getTotalWordCount() const override;
  bool loadReadingPosition(ReflowReadingPosition& position) const override;
  bool saveReadingPosition(const ReflowReadingPosition& position) const override;
  PdfStatus loadPdfSavedItems(PdfSavedItemsBuffer* output) const;
  PdfStatus savePdfSavedItems(const PdfSavedItem* items, uint16_t count) const;
  PdfStatus validatePdfSavedItem(const PdfSavedItem& item) const;
  PdfStatus mapPdfSavedItem(const std::string& sectionCachePath, uint32_t layoutFingerprint,
                            const PdfSavedItem& item, PdfSavedItemPageRange* pages) const;
  PdfStatus pdfSavedItemsLayoutFingerprint(const std::string& sectionCachePath, uint32_t* fingerprint) const;
  bool validateLayoutWordIndex(const std::string& sectionCachePath, int sectionIndex,
                               uint16_t pageCount) const override;
  bool removeLayoutWordIndex(const std::string& sectionCachePath) const override;
  bool readLayoutWordRange(const std::string& sectionCachePath, uint16_t pageCount, uint16_t page,
                           ReflowPageSemanticRange& range) const override;
  bool findLayoutWordPage(const std::string& sectionCachePath, const char* blockAnchor, uint32_t blockWordOffset,
                          uint32_t globalWordOrdinal, uint16_t& page) const override;
  bool findLayoutWordCursor(const std::string& sectionCachePath, uint32_t wordCursor, uint16_t& page) const override;

  bool getLocalSectionPath(int sectionIndex, ReflowResource& out) const override;
  bool streamSection(int sectionIndex, Print& out, size_t chunkSize) const override;
  bool resolveResource(int sectionIndex, const std::string& href, ReflowResource& out) const override;
  bool streamResource(int sectionIndex, const std::string& href, Print& out, size_t chunkSize) const override;
  bool getResourceSize(int sectionIndex, const std::string& href, size_t* size) const override;
  CssParser* getCssParser() const override { return nullptr; }

 protected:
  const PdfCacheIo& cacheIo() const { return io_; }

 private:
  struct ManifestSource {
    const PdfCacheIo* io = nullptr;
    PdfCacheHandle handle{};
    uint64_t size = 0;

    static PdfStatus read(void* context, uint64_t offset, uint8_t* destination, size_t requested, size_t* bytesRead);
    PdfByteSource source() { return {this, size, read}; }
  };

  struct SavedItemMapSource {
    const PdfReflowDocument* document = nullptr;
    const std::string* sectionCachePath = nullptr;
    ManifestSource file{};
    PdfLayoutWordIndexInfo info{};
    bool inspected = false;

    static PdfStatus inspect(void* context, uint16_t sectionIndex, PdfLayoutWordIndexInfo* info);
    static PdfStatus readRanges(void* context, uint16_t sectionIndex, uint16_t firstPage, uint16_t count,
                                PdfLayoutWordRange* ranges);
  };

  enum class ManifestFileStage : uint8_t {
    Sections,
    Images,
    Cover,
    Thumbnail,
    Metadata,
    Outline,
  };

  static constexpr uint8_t MaxCachedResources = 64;

  struct ManifestSectionValidationRecord {
    uint32_t size = 0;
    uint32_t crc32 = 0;
  };

  struct ValidationStorageLayout {
    size_t sectionRecordsOffset = 0;
    size_t sectionSeenOffset = 0;
    size_t resourcesOffset = 0;
    size_t bytes = 0;
  };

  struct ManifestRecordCursor {
    uint32_t generation = 0;
    uint32_t filesSeen = 0;
    uint16_t sectionCount = 0;
    uint8_t resourceCount = 0;
    uint8_t metadataFiles = 0;
    uint8_t outlineFiles = 0;
    ManifestFileStage stage = ManifestFileStage::Sections;
  };

  enum class ManifestRecordKind : uint8_t {
    Section,
    Resource,
    Cover,
    Thumbnail,
    Metadata,
    Outline,
  };

  struct ManifestRecordClassification {
    ManifestRecordKind kind = ManifestRecordKind::Section;
    uint16_t sectionIndex = 0;
    PdfCachedResourceRecord resource{};
  };

  struct ManifestCountContext {
    const PdfCacheManifest* manifest = nullptr;
    ManifestRecordCursor cursor{};
    PdfStatus status{};
    bool initialized = false;
  };

  static constexpr size_t MaxValidationStorageBytes =
      PdfMetadataLimits::MaxSections * sizeof(ManifestSectionValidationRecord) +
      (PdfMetadataLimits::MaxSections + 7U) / 8U + MaxCachedResources * sizeof(PdfCachedResourceRecord);
  static_assert(MaxValidationStorageBytes == 4640,
                "maximum exact PDF validation storage must not exceed the former fixed capacities");

  static PdfStatus captureRequiredFile(void* context, const PdfRequiredFileRecord& record);
  static PdfStatus countRequiredFile(void* context, const PdfRequiredFileRecord& record);
  static PdfStatus captureMetadataSection(void* context, uint16_t index, const PdfMetadataSection& record);
  static PdfStatus validateOutlineEntry(void* context, uint16_t index, const PdfOutlineEntry& record);
  static PdfStatus classifyManifestRecord(ManifestRecordCursor* cursor, const PdfRequiredFileRecord& record,
                                          ManifestRecordClassification* classification);
  static PdfStatus computeValidationStorageLayout(size_t sectionCount, size_t resourceCount,
                                                  ValidationStorageLayout* layout);
  PdfStatus allocateValidationStorage(uint16_t sectionCount, uint8_t resourceCount);
#ifdef CROSSINK_PDF_REFLOW_DOCUMENT_TEST
 public:
  static PdfStatus validationStorageLayoutForTest(size_t sectionCount, size_t resourceCount, size_t* bytes) {
    if (bytes == nullptr) {
      return PdfStatus::failure(PdfError::InvalidArgument);
    }
    ValidationStorageLayout layout;
    const PdfStatus status = computeValidationStorageLayout(sectionCount, resourceCount, &layout);
    *bytes = status ? layout.bytes : 0;
    return status;
  }
  size_t validationStorageBytesForTest() const { return validationStorageLayout_.bytes; }

 private:
#endif
  ManifestSectionValidationRecord* manifestSectionRecords();
  const ManifestSectionValidationRecord* manifestSectionRecords() const;
  uint8_t* manifestSectionSeen();
  const uint8_t* manifestSectionSeen() const;
  PdfCachedResourceRecord* cachedResources();
  const PdfCachedResourceRecord* cachedResources() const;
  PdfStatus captureFile(const PdfRequiredFileRecord& record);
  PdfStatus validateCapturedFiles();
  PdfStatus validateCachedFile(const char* path, uint64_t expectedSize, uint32_t expectedCrc32,
                               PdfCachedResourceRecord* resource);
  PdfStatus validateResourceFile(uint8_t resourceIndex);
  PdfStatus selectCompletedManifest(PdfCacheSlot* selectedSlot);
  PdfStatus decodeSelectedManifest(PdfCacheSlot selectedSlot);
  PdfStatus loadMetadataCache();
  PdfStatus loadOutlineCache();
  bool readOutlineEntry(int tocIndex, PdfOutlineEntry* entry) const;
  bool formatSectionHref(int sectionIndex, char* output, size_t capacity) const;
  bool formatSectionPath(int sectionIndex, char* output, size_t capacity) const;
  bool streamCachedFile(const std::string& path, uint64_t fileSize, Print& out, size_t chunkSize) const;
  bool formatResourcePath(const PdfCachedResourceRecord& resource, char* output, size_t capacity) const;
  const PdfCachedResourceRecord* findResource(int sectionIndex, const std::string& href) const;
  static void fitResourceDimensions(uint16_t sourceWidth, uint16_t sourceHeight, uint16_t* width, uint16_t* height);
  bool formatLayoutWordIndexPath(const std::string& sectionCachePath, char destination[PDF_CACHE_PATH_CAPACITY]) const;
  bool openLayoutWordIndex(const char* path, ManifestSource& source) const;
  bool closeLayoutWordIndex(ManifestSource& source) const;
  void resetLoadedState();
  void deriveFallbackTitle();

  PdfCacheIo io_{};
  std::string sourcePath_;
  std::string cacheRoot_;
  std::string title_;
  std::string author_;
  std::string language_;
  std::string metadataPath_;
  std::string outlinePath_;
  std::string coverPath_;
  std::string thumbnailPath_;
  PdfSourceIdentity sourceIdentity_{};
  PdfCacheManifest manifest_{};
  PdfMetadata metadata_{};
  PdfRequiredFileRecord coverRecord_{};
  PdfRequiredFileRecord thumbnailRecord_{};
  PdfRequiredFileRecord metadataRecord_{};
  PdfRequiredFileRecord outlineRecord_{};
  PdfProgressStore progressStore_{};
  PdfSavedItemsStore savedItemsStore_{};
  std::unique_ptr<PdfMetadataSection[]> sections_;
  std::unique_ptr<uint8_t[]> ioWorkspace_;
  std::unique_ptr<uint8_t[]> validationStorage_;
  ValidationStorageLayout validationStorageLayout_{};
  uint16_t validationSectionCount_ = 0;
  uint8_t validationResourceCount_ = 0;
  std::array<char, PDF_CACHE_PATH_CAPACITY> validationPath_{};
  mutable PdfOutlineEntry cachedOutlineEntry_{};
  mutable int cachedOutlineIndex_ = -1;
  // PDF-only four-record decode window: kept off the 4 KiB reader task stack
  // and reused for every page lookup.
  mutable std::array<PdfLayoutWordRange, 4> layoutWordWindow_{};
  mutable std::array<char, PDF_CACHE_PATH_CAPACITY> layoutWordIndexPath_{};
  mutable uint16_t layoutWordWindowStart_ = 0;
  mutable uint8_t layoutWordWindowCount_ = 0;
  mutable bool layoutWordIndexAvailable_ = false;
  ManifestRecordCursor manifestCursor_{};
  bool loaded_ = false;
  bool savedItemsReady_ = false;
  PdfStatus status_{};
};

static_assert(sizeof(PdfReflowDocument) <= 2560,
              "PdfReflowDocument must not embed maximum-capacity validation tables");
