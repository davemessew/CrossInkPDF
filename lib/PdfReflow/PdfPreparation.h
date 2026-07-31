#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>

#include "PdfCacheStore.h"
#include "PdfImageBuildSpool.h"
#include "PdfImageCache.h"
#include "PdfJpegPreview.h"
#include "PdfLimits.h"
#include "PdfMaskSpool.h"
#include "PdfMetadataStore.h"
#include "PdfObjectResolver.h"
#include "PdfOutline.h"
#include "PdfPageTree.h"
#include "PdfResourceTracker.h"
#include "PdfSemanticWriter.h"

using PdfPreparationNowMsFn = uint32_t (*)(void* context);

struct PdfPreparationConfig {
  PdfCacheIo io{};
  const char* sourcePath = nullptr;
  const char* cacheDirectory = nullptr;
  void* clockContext = nullptr;
  PdfPreparationNowMsFn nowMs = nullptr;
  PdfResourceHooks resourceHooks{};
  PdfCacheRenameFn rename = nullptr;
  uint16_t maximumRasterOutputWidth = 0;
  uint16_t maximumRasterOutputHeight = 0;
};

struct PdfCoverCandidateSource {
  PdfObjectReference reference{};
  uint16_t sourcePageIndex = 0;
  bool referenceIsResourceDictionary = false;
};

enum class PdfPreparationPhase : uint8_t {
  Idle,
  ResourceGate,
  AllocateWorkspaces,
  OpenSource,
  FingerprintHead,
  FingerprintTail,
  PrepareCache,
  ParseXref,
  ResolveCatalog,
  WalkPages,
  ResolveNavigation,
  ReadXmpMetadata,
  ResolveImageResources,
  ResolveContent,
  ExtractText,
  CacheImage,
  SpoolNavigation,
  DecodeImages,
  RestoreNavigation,
  RepairImageSections,
  CloseSource,
  OpenSection,
  EmitSection,
  CloseSection,
  OpenMetadata,
  WriteMetadata,
  CloseMetadata,
  OpenOutline,
  WriteOutline,
  CloseOutline,
  CommitManifest,
  CommitCheckpoint,
  Cleanup,
  Complete,
  Failed,
  Cancelled,
};

class PdfPreparationPaintGate {
 public:
  bool shouldPaint(uint8_t progressPercent, uint32_t nowMs);
  uint8_t intermediatePaintCount() const { return intermediatePaintCount_; }

 private:
  uint32_t lastPaintMs_ = 0;
  uint8_t lastPaintPercent_ = 0;
  uint8_t intermediatePaintCount_ = 0;
};

class PdfPreparation {
 public:
  PdfPreparation() = default;
  ~PdfPreparation();

  PdfPreparation(const PdfPreparation&) = delete;
  PdfPreparation& operator=(const PdfPreparation&) = delete;

  PdfStatus begin(const PdfPreparationConfig& config);
  PdfStepResult step();
  void requestCancel() { cancelRequested_ = true; }

  PdfPreparationPhase phase() const { return phase_; }
  uint8_t progressPercent() const { return progressPercent_; }
  PdfStatus status() const { return status_; }
  const char* cacheRoot() const { return cacheRoot_; }
  const PdfSourceIdentity& sourceIdentity() const { return sourceIdentity_; }
  uint32_t generation() const { return generation_; }
  uint32_t totalWords() const { return totalWords_; }
  bool resumedFromCheckpoint() const { return resumedFromCheckpoint_; }
  size_t resourceCurrentBytes() const;
  size_t resourcePeakBytes() const;
  uint8_t coverCandidateSourceCount() const { return coverCandidateSourceCount_; }
  bool coverCandidateSource(uint8_t index, PdfCoverCandidateSource* output) const;
  uint32_t navigationSpoolBytes() const { return navigationSpoolBytes_; }
  uint8_t navigationSpoolWriteCount() const { return navigationSpoolWriteCount_; }
  uint8_t navigationSpoolReadCount() const { return navigationSpoolReadCount_; }
  uint8_t maskSpoolWriteCount() const { return maskSpoolWriteCount_; }
  uint8_t maskSpoolReadCount() const { return maskSpoolReadCount_; }

 private:
  struct NavigationWorkspace;
  struct ExtractedBlockRecord;
  struct RasterBatchWorkspace;
  struct PlacementWorkspace;
  struct SectionRepairRuntime;

  enum class NavigationTask : uint8_t {
    None,
    Info,
    Xmp,
    NamedDestinations,
    PageLabels,
    OutlineRoot,
    OutlineNode,
    Annotation,
    Complete,
  };

  enum class ImageResolveTask : uint8_t {
    None,
    ResourceOwner,
    XObjectDictionary,
    ImageObject,
    ColorSpace,
    IndexedBaseColorSpace,
    IndexedPalette,
    AuxiliaryImageObject,
  };

  enum class RasterDecodeStage : uint8_t {
    Idle,
    LoadCandidate,
    DecodeUnmasked,
    DecodeMaskedBase,
    DecodeMaskedAlpha,
    CloseMaskSpool,
    OpenMaskSpool,
    CompositeMasks,
    Finalize,
  };

  enum class ImageCacheStage : uint8_t {
    Idle,
    RasterPrimary,
    RasterAuxiliary,
    RasterIdentity,
    Jpeg,
  };

  enum class SectionRepairStage : uint8_t {
    Idle,
    CollectTags,
    OpenOriginal,
    PatchToTemporary,
    CloseTemporary,
    OpenTemporary,
    CopyToFinal,
    CloseFinal,
    Done,
  };

  enum class InlineNavigationSpillStage : uint8_t {
    None,
    Writing,
    Spilled,
    Reading,
  };

  enum class InlineImageContainer : uint8_t {
    None,
    FilterArray,
    DecodeArray,
    ColorSpaceArray,
    DecodeParametersDictionary,
  };

  enum class InlineIndexedStage : uint8_t {
    Family,
    Base,
    High,
    Palette,
    Complete,
  };

  enum class NavigationSpoolStage : uint8_t {
    None,
    Writing,
    Flush,
    Sync,
    Close,
    ReadyToRead,
    Reading,
  };

  enum class TypographyAssetStage : uint8_t {
    Idle,
    OpenSource,
    ReadSourceHeader,
    BeginAsset,
    Header,
    Rows,
    Close,
    CloseSource,
    Complete,
  };

  enum class ManifestCommitStage : uint8_t {
    Idle,
    LedgerSections,
    LedgerImages,
    LedgerCovers,
    LedgerMetadata,
    OpenWriter,
    WriteHeader,
    WriteRecords,
    WriteTrailer,
    CloseWriter,
    CloseImageSpool,
    OpenVerifier,
    VerifyHeader,
    VerifyRecords,
    VerifyTrailer,
    CloseVerifier,
    Complete,
  };

  enum class CacheSetupStage : uint8_t {
    Idle,
    CloseSource,
    CreateCacheDirectory,
    InitializeStore,
    OpenCheckpointSlot,
    ReadCheckpointMetadata,
    ReadCheckpoint,
    CloseCheckpointSlot,
    OpenManifestSlot,
    ReadManifestMetadata,
    ReadManifestHeader,
    ReadManifestRecordHeader,
    ReadManifestRecordPath,
    ReadManifestTrailer,
    CloseManifestSlot,
    SelectGeneration,
    CreateCacheRoot,
    CreateGeneration,
    CreateSectionDirectory,
    ReadCapacity,
    InitializeBudget,
    InitializeImageCache,
    FormatOutputPaths,
    ReopenSource,
    Complete,
  };

  enum class CheckpointCommitStage : uint8_t {
    Idle,
    CreateCacheRoot,
    OpenWriter,
    Write,
    Flush,
    Sync,
    CloseWriter,
    OpenVerifier,
    ReadVerifierMetadata,
    ReadVerifier,
    CloseVerifier,
    Verify,
    Complete,
  };

  enum class CleanupStage : uint8_t {
    Idle,
    List,
    Remove,
    Complete,
  };

  enum class SectionEmitStage : uint8_t {
    Idle,
    BeginBlock,
    Images,
    EndBlock,
    Finish,
    Complete,
  };

  struct MemoryRecordContext {
    uint8_t* bytes = nullptr;
    size_t recordSize = 0;
    uint32_t capacity = 0;
  };

  struct SourceContext {
    const PdfCacheIo* io = nullptr;
    PdfCacheHandle* handle = nullptr;
    uint64_t size = 0;
  };

  static PdfStatus readSource(void* context, uint64_t offset, uint8_t* destination, size_t requested,
                              size_t* bytesRead);
  static PdfStatus readMemoryRecord(void* context, uint32_t ordinal, void* record, size_t recordSize);
  static PdfStatus writeMemoryRecord(void* context, uint32_t ordinal, const void* record, size_t recordSize);
  static PdfStatus capturePage(void* context, const PdfPageInfo& page);
  static PdfStatus writeSection(void* context, const uint8_t* source, size_t requested, size_t* bytesWritten);
  static PdfStatus writeMetadata(void* context, const uint8_t* source, size_t requested, size_t* bytesWritten);
  static PdfStatus writeOutline(void* context, const uint8_t* source, size_t requested, size_t* bytesWritten);
  static PdfStatus discardInlineImageDecoded(void* context, const uint8_t* source, size_t requested,
                                             size_t* bytesWritten);
  static PdfStatus emitBlock(void* context, const PdfSemanticBlockRecord& record);
  static PdfStatus readMetadataSection(void* context, uint16_t index, PdfMetadataSection* record);
  static PdfStatus readOutlineEntry(void* context, uint16_t index, PdfOutlineEntry* record);
  static PdfStatus readRequiredFile(void* context, uint32_t index, PdfRequiredFileRecord* record);
  static bool cancelRequested(void* context);
  static bool sliceExpired(void* context);

  PdfFixedRecordStore recordStore(MemoryRecordContext& context);
  PdfByteSource source();
  PdfStepResult pause();
  PdfStepResult fail(PdfStatus status);
  PdfStepResult cancel();
  PdfStatus closeSource();
  PdfStatus reopenSource();
  void destroyParsers();
  void releaseWorkspaces();
  bool allocateNextWorkspace();
  PdfStatus initializeParserStorage();
  PdfStepResult stepSetupCache(PdfWorkBudget& budget);
  PdfStatus startXref();
  PdfStatus finishXref();
  PdfStatus finishCatalog();
  PdfStatus finishPageTree();
  PdfStatus beginNavigationDiscovery();
  PdfStatus finishNavigationObject();
  PdfStatus startNextNavigationObject();
  PdfStatus readXmpMetadata();
  PdfStatus resolveDestination(const PdfRawDestination& raw, PdfResolvedDestination* destination) const;
  PdfStatus beginCurrentPageImages();
  PdfStatus finishImageResolution();
  PdfStatus finishAuxiliaryImageResolution();
  PdfStatus continueImageDescriptorResolution();
  PdfStatus finishResolvedImageColorSpace(bool indexedBase);
  PdfStatus finishResolvedImagePalette();
  PdfStatus allocateImagePalette(uint8_t** palette);
  PdfStatus collectImageCandidates(uint16_t dictionaryIndex);
  PdfStatus beginNextImageObject();
  PdfStepResult cacheCurrentPageImage(PdfWorkBudget& budget);
  PdfStatus appendDeferredImageRecord(uint8_t candidateIndex, uint32_t tagOffset, uint16_t tagLength);
  PdfStatus appendImageFileRecord(const PdfRequiredFileRecord& record);
  PdfStepResult spoolNavigation(PdfWorkBudget& budget);
  PdfStepResult decodeRasterBatch(PdfWorkBudget& budget);
  PdfStatus beginUnmaskedRaster(const PdfDeferredImageRecord& candidate, uint64_t byteLimit);
  PdfStatus beginMaskedRaster(const PdfDeferredImageRecord& candidate, uint64_t byteLimit);
  PdfStepResult stepActiveRaster(PdfWorkBudget& budget, bool masked);
  PdfStatus beginActiveMask(const PdfDeferredImageRecord& candidate);
  PdfStepResult stepActiveMask(PdfWorkBudget& budget);
  void abortActiveImageRuntime();
  PdfStepResult omitActiveRasterImage(PdfStatus status);
  PdfStatus beginMaskCompositeSpool();
  PdfStepResult stepMaskCompositeSpool(PdfWorkBudget& budget);
  PdfStatus beginMaskCompositeRecord(const PdfMaskSpoolRecord& record);
  PdfStepResult stepMaskCompositeRecord(PdfWorkBudget& budget);
  PdfStepResult restoreNavigation(PdfWorkBudget& budget);
  void abortNavigationSpool();
  PdfStepResult repairFailedImageSections(PdfWorkBudget& budget);
  void abortSectionRepairRuntime();
  PdfStatus beginCurrentPageContent();
  PdfStatus finishContentObject();
  PdfStatus appendContentToken(const PdfToken& token);
  PdfStatus consumeInlineImageToken(const PdfToken& token);
  void finalizeInlineImageDictionary();
  void resetInlineImageDictionaryState();
  PdfStatus initializeInlineImageDataOffset(const PdfByteSource& contentSource);
  PdfStepResult finishInlineImageData(PdfWorkBudget& budget);
  void abortInlineNavigationSpill();
  PdfStatus retainInlineImage(uint64_t dataOffset, uint64_t dataLength);
  PdfStatus finishExtractedPage();
  PdfStatus formatCurrentSectionPath();
  PdfStatus formatInternalLink(uint16_t sourcePageIndex, const uint8_t* text, size_t textLength, char* href,
                               size_t capacity, size_t* hrefLength) const;
  PdfStatus openSection();
  PdfStepResult emitSection(PdfWorkBudget& budget);
  PdfStatus closeSection();
  PdfStatus prepareNavigationRecords();
  PdfStepResult stepTypographyAssets(PdfWorkBudget& budget);
  PdfStatus openMetadata();
  PdfStatus writeMetadata();
  PdfStatus closeMetadata();
  PdfStatus openOutline();
  PdfStepResult stepWriteOutline(PdfWorkBudget& budget);
  PdfStatus closeOutline();
  PdfStepResult stepCommitManifest(PdfWorkBudget& budget);
  PdfStepResult stepCommitCheckpoint(PdfWorkBudget& budget, PdfBuildPhase phase);
  PdfStepResult stepCleanup(PdfWorkBudget& budget);
  void abortManifestCommit();
  PdfStatus commitCheckpoint(PdfBuildPhase phase);
  uint32_t nowMs() const;
  void setPhase(PdfPreparationPhase phase, uint8_t progressPercent);

  PdfPreparationConfig config_{};
  char sourcePath_[512]{};
  char cacheRoot_[PDF_CACHE_PATH_CAPACITY]{};
  char sectionPath_[PDF_CACHE_PATH_CAPACITY]{};
  char sectionRelativePath_[PDF_CACHE_REQUIRED_PATH_CAPACITY]{};
  char metadataPath_[PDF_CACHE_PATH_CAPACITY]{};
  char metadataRelativePath_[PDF_CACHE_REQUIRED_PATH_CAPACITY]{};
  char outlinePath_[PDF_CACHE_PATH_CAPACITY]{};
  char outlineRelativePath_[PDF_CACHE_REQUIRED_PATH_CAPACITY]{};
  PdfPreparationPhase phase_ = PdfPreparationPhase::Idle;
  PdfStatus status_{};
  PdfStatus operationStatus_{};
  uint8_t progressPercent_ = 0;
  uint8_t allocationIndex_ = 0;
  bool cancelRequested_ = false;
  bool resumedFromCheckpoint_ = false;
  uint32_t sliceStartedAtMs_ = 0;

  std::optional<PdfResourceTracker> resources_;
  std::unique_ptr<uint8_t[]> dictionary_;
  std::unique_ptr<uint8_t[]> sourceWindow_;
  std::unique_ptr<uint8_t[]> decoderOutput_;
  std::unique_ptr<uint8_t[]> pageText_;
  std::unique_ptr<uint8_t[]> runRecords_;
  std::unique_ptr<uint8_t[]> operandScratch_;

  PdfCacheHandle sourceHandle_{};
  PdfCacheFileMetadata sourceMetadata_{};
  PdfSourceIdentity sourceIdentity_{};
  SourceContext sourceContext_{};
  PdfObjectArena arena_{};
  MemoryRecordContext xrefRecords_{};
  MemoryRecordContext traversalRecords_{};
  std::optional<PdfXrefTable> xref_;
  std::optional<PdfXrefParser> xrefParser_;
  std::optional<PdfObjectResolver> resolver_;
  std::optional<PdfPageTreeWalker> pageWalker_;
  NavigationWorkspace* navigation_ = nullptr;
  std::optional<PdfNamedDestinationMap> namedDestinations_;
  std::optional<PdfPageLabelMap> pageLabels_;
  std::optional<PdfOutlineBuilder> outlineBuilder_;
  PdfCatalogNavigation catalogNavigation_{};
  PdfObjectReference infoReference_{};
  PdfObjectReference activeNavigationReference_{};
  uint32_t pageCount_ = 0;
  uint32_t currentPageIndex_ = 0;
  uint16_t currentContentIndex_ = 0;
  uint16_t extractedBlockCount_ = 0;
  uint16_t currentBlockIndex_ = 0;
  uint16_t sectionEmitEndBlock_ = 0;
  uint16_t sectionCount_ = 0;
  uint16_t explicitOutlineCount_ = 0;
  uint16_t outlinePendingCount_ = 0;
  uint16_t outlineSeenCount_ = 0;
  int16_t currentOutlineParent_ = -1;
  uint16_t currentAnnotationPage_ = 0;
  uint8_t currentAnnotationIndex_ = 0;
  uint8_t navigationStage_ = 0;
  NavigationTask navigationTask_ = NavigationTask::None;
  ImageResolveTask imageResolveTask_ = ImageResolveTask::None;
  uint8_t imageCandidateCount_ = 0;
  uint8_t currentPageImageStart_ = 0;
  uint8_t currentPageImageEnd_ = 0;
  uint8_t sectionEmitImageIndex_ = 0;
  uint8_t imageResolveIndex_ = 0;
  uint8_t imagePaletteCount_ = 0;
  uint8_t rasterDecodeIndex_ = 0;
  int8_t currentPageImageCandidate_ = -1;
  ImageCacheStage imageCacheStage_ = ImageCacheStage::Idle;
  PdfByteRange imageCacheRange_{};
  PdfCapturedJpeg inlineCapturedJpeg_{};
  uint64_t imageCacheOffset_ = 0;
  uint8_t rasterIdentityScanIndex_ = 0;
  uint8_t retainedImageFileCount_ = 0;
  uint8_t lastContentNameLength_ = 0;
  char lastContentName_[32]{};
  PdfImageParameters inlineImageParameters_{};
  PdfStreamFilter inlineImageFilters_[PdfLimits::MaxFiltersPerStream]{};
  PdfByteRange inlineImageRange_{};
  std::optional<PdfStreamDecoder> inlineImageDecoder_;
  char inlineImageKey_[20]{};
  int16_t inlineImageDecodeValues_[6]{};
  char inlineNavigationSpoolPath_[PDF_CACHE_PATH_CAPACITY]{};
  PdfCacheHandle inlineNavigationSpoolHandle_{};
  uint64_t inlineImageIdEnd_ = 0;
  uint64_t inlineImageDataOffset_ = 0;
  uint64_t inlineImageScanOffset_ = 0;
  uint64_t inlineImageEncodedLength_ = 0;
  size_t inlineImageScanPendingBytes_ = 0;
  size_t inlineImageScanPendingBufferOffset_ = 0;
  uint64_t inlineNavigationSpoolOffset_ = 0;
  uint32_t inlineNavigationSpoolCrc32_ = 0;
  uint32_t inlineNavigationSpoolReadCrc32_ = 0;
  uint16_t inlineImagePredictorColumns_ = 1;
  uint8_t inlineImageFilterCount_ = 0;
  uint8_t inlineImageKeyLength_ = 0;
  uint8_t inlineImageDecodeValueCount_ = 0;
  uint8_t inlineImagePredictorColors_ = 1;
  uint8_t inlineImagePredictorBitsPerComponent_ = 8;
  bool inlineImageDictionaryActive_ = false;
  bool inlineImageAwaitingData_ = false;
  bool inlineImageJpeg_ = false;
  bool inlineImageScanSawJpegMarker_ = false;
  bool inlineImageCaptureStarted_ = false;
  bool inlineImageCaptureFailed_ = false;
  bool inlineImageSupported_ = true;
  InlineImageContainer inlineImageContainer_ = InlineImageContainer::None;
  InlineIndexedStage inlineIndexedStage_ = InlineIndexedStage::Family;
  InlineNavigationSpillStage inlineNavigationSpillStage_ = InlineNavigationSpillStage::None;
  PlacementWorkspace* placement_ = nullptr;
  RasterBatchWorkspace* rasterBatch_ = nullptr;
  PdfMaskSpool* maskSpool_ = nullptr;
  char navigationSpoolPath_[PDF_CACHE_PATH_CAPACITY]{};
  char maskSpoolPath_[PDF_CACHE_PATH_CAPACITY]{};
  char imageBuildSpoolPath_[PDF_CACHE_PATH_CAPACITY]{};
  char imageFileSpoolPath_[PDF_CACHE_PATH_CAPACITY]{};
  PdfCacheHandle navigationSpoolHandle_{};
  uint64_t navigationSpoolOffset_ = 0;
  uint32_t navigationSpoolCrc32_ = 0;
  uint32_t navigationSpoolReadCrc32_ = 0;
  uint32_t navigationSpoolBytes_ = 0;
  uint8_t navigationSpoolWriteCount_ = 0;
  uint8_t navigationSpoolReadCount_ = 0;
  uint8_t maskSpoolWriteCount_ = 0;
  uint8_t maskSpoolReadCount_ = 0;
  NavigationSpoolStage navigationSpoolStage_ = NavigationSpoolStage::None;
  PdfImageBuildSpool imageBuildSpool_{};
  PdfImageFileSpool imageFileSpool_{};
  PdfDeferredImageRecord activeRasterCandidate_{};
  uint8_t rasterCanonicalRecordIndices_[PDF_IMAGE_BUILD_SPOOL_MAX_RECORDS]{};
  PdfObjectReference imageRepetitionReferences_[16]{};
  uint8_t imageRepetitionCounts_[16]{};
  uint8_t imageRepetitionEntryCount_ = 0;
  bool continueAfterImageDecode_ = false;
  SectionEmitStage sectionEmitStage_ = SectionEmitStage::Idle;
  bool rasterRuntimeActive_ = false;
  bool maskDecodeRuntimeActive_ = false;
  bool maskCompositeRuntimeActive_ = false;
  bool sectionRepairRuntimeActive_ = false;
  RasterDecodeStage rasterDecodeStage_ = RasterDecodeStage::Idle;
  uint64_t failedRasterImages_ = 0;
  bool hasInfoReference_ = false;
  PdfByteRange contentRange_{};
  std::optional<PdfLexer> contentLexer_;
  size_t transcriptLength_ = 0;
  int16_t currentFontSize_ = 0;
  int16_t lastNumericValue_ = 0;
  uint32_t nextAnchorOrdinal_ = 0;
  uint32_t currentSectionFirstWord_ = 0;
  uint32_t currentSectionFirstAnchor_ = 0;
  uint64_t cumulativeSectionBytes_ = 0;
  size_t metadataEncodeBytes_ = 0;
  uint64_t xmpStreamOffset_ = 0;
  uint64_t xmpStreamLength_ = 0;

  PdfCacheStore cacheStore_;
  PdfCacheCapacity cacheCapacity_{};
  PdfCacheBudget cacheBudget_{};
  PdfCacheManifestSelection manifestSelection_{};
  PdfBuildCheckpointSelection checkpointSelection_{};
  PdfCacheTrackedWriter sectionWriter_{};
  PdfCacheTrackedWriter metadataWriter_{};
  PdfCacheTrackedWriter outlineWriter_{};
  PdfOutlineEncodeRuntime outlineEncodeRuntime_{};
  PdfCacheHandle cacheSetupHandle_{};
  uint64_t cacheSetupFileSize_ = 0;
  uint64_t cacheSetupOffset_ = 0;
  uint64_t cacheSetupDecodedFileBytes_ = 0;
  uint64_t cacheSetupDecodedLedger_ = PDF_CACHE_FNV64_OFFSET;
  uint32_t cacheSetupCrc32_ = 0;
  uint32_t cacheSetupRecordIndex_ = 0;
  uint8_t cacheSetupSlot_ = 0;
  CacheSetupStage cacheSetupStage_ = CacheSetupStage::Idle;
  PdfCacheHandle checkpointCommitHandle_{};
  CheckpointCommitStage checkpointCommitStage_ = CheckpointCommitStage::Idle;
  uint8_t cleanupIndex_ = 0;
  CleanupStage cleanupStage_ = CleanupStage::Idle;
  PdfCacheHandle manifestHandle_{};
  char manifestPath_[PDF_CACHE_PATH_CAPACITY]{};
  uint64_t manifestOffset_ = 0;
  uint32_t manifestEncodedBytes_ = 0;
  uint32_t manifestRecordIndex_ = 0;
  uint32_t manifestCrc32_ = 0;
  uint32_t manifestReadCrc32_ = 0;
  uint8_t manifestTargetSlot_ = 0;
  ManifestCommitStage manifestCommitStage_ = ManifestCommitStage::Idle;
  PdfRequiredFileRecord sectionRecord_{};
  PdfRequiredFileRecord metadataRecord_{};
  PdfRequiredFileRecord outlineRecord_{};
  PdfRequiredFileRecord coverImageSourceRecord_{};
  PdfRequiredFileRecord coverRecords_[2]{};
  uint8_t coverFileCount_ = 0;
  uint8_t typographyAssetIndex_ = 0;
  uint16_t typographyRow_ = 0;
  PdfCacheHandle typographySourceHandle_{};
  uint64_t coverImageContentHash_ = 0;
  uint32_t coverImageSourceCrc32_ = 0;
  uint16_t typographySourceWidth_ = 0;
  uint16_t typographySourceHeight_ = 0;
  uint16_t typographySourceRowBytes_ = 0;
  uint16_t typographySourceLoadedRow_ = UINT16_MAX;
  uint16_t typographyScaledWidth_ = 0;
  uint16_t typographyScaledHeight_ = 0;
  uint16_t typographyOffsetX_ = 0;
  uint16_t typographyOffsetY_ = 0;
  PdfJpegPreview jpegPreview_{};
  bool meaningfulEarlyImageSeen_ = false;
  bool coverImageFingerprintSelected_ = false;
  bool coverImageRecordAvailable_ = false;
  bool coverImageSourceJpeg_ = false;
  TypographyAssetStage typographyAssetStage_ = TypographyAssetStage::Idle;
  bool navigationRecordsPrepared_ = false;
  PdfSemanticWriter semanticWriter_{};
  PdfMetadataBuilder metadataBuilder_{};
  PdfMetadata metadata_{};
  uint32_t generation_ = 0;
  uint32_t sequence_ = 0;
  uint32_t totalWords_ = 0;
  uint32_t warningFlags_ = 0;
  PdfCoverCandidateSource coverCandidateSources_[PdfLimits::MaxCoverCandidateSources]{};
  uint8_t coverCandidateSourceCount_ = 0;
};

static_assert(sizeof(PdfCoverCandidateSource) <= 16, "cover discovery must retain only small object references");
static_assert(sizeof(PdfPreparation) + PdfLimits::TotalWorkspaceBytes <= PDF_MAX_OWNED_HEAP_BYTES,
              "PDF preparation state and fixed workspaces must stay within the 80 KiB heap envelope");
