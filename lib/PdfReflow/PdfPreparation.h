#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>

#include "PdfCacheStore.h"
#include "PdfFixedRecordSpool.h"
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

struct PdfContentXObject;

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

struct PdfPreparationWorkCounters {
  uint32_t xrefSteps = 0;
  uint32_t pagesWalked = 0;
  uint32_t contentTokens = 0;
  uint32_t sectionsEmitted = 0;
  uint32_t imagesEmitted = 0;
  uint64_t sourceBytesRead = 0;
  uint32_t xrefSpoolRecordsRead = 0;
  uint32_t xrefSpoolRecordsWritten = 0;
};

enum class PdfPreparationPhase : uint8_t {
  Idle,
  ResourceGate,
  AllocateWorkspaces,
  OpenSource,
  FingerprintHead,
  FingerprintTail,
  PrepareCache,
  ResumeEmitSections,
  ParseXref,
  SortXref,
  ResolveCatalog,
  InitializePageTree,
  WalkPages,
  FinalizePageTree,
  ResolveNavigation,
  ReadXmpMetadata,
  ResolveImageResources,
  ResolveContent,
  SuspendContentNavigation,
  CheckContentCapacity,
  OpenContentStore,
  DecodeContent,
  RestoreContentNavigation,
  OpenDecodedContent,
  ExtractText,
  SpoolFontNavigation,
  DecodeFonts,
  ParseFonts,
  RestoreFontNavigation,
  ReopenPreparedContent,
  InterpretContent,
  CleanupContentStore,
  OrderText,
  CacheImage,
  SpoolNavigation,
  DecodeImages,
  RestoreNavigation,
  RepairImageSections,
  CloseSource,
  OpenSection,
  EmitSection,
  CloseSection,
  CommitResumePoint,
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
  DiscoverForms,
  PrepareFormContent,
  BeginPage,
  PreparePageLinks,
  PrepareNavigationRecords,
  CommitDiscoveryResume,
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
  PdfBuildResumePhase durableResumePhase() const { return durableResumePhase_; }
  PdfBuildResumePhase resumedPhase() const { return resumedPhase_; }
  uint32_t durableResumePage() const { return durableResumePage_; }
  size_t resourceCurrentBytes() const;
  size_t resourcePeakBytes() const;
  uint8_t coverCandidateSourceCount() const { return coverCandidateSourceCount_; }
  bool coverCandidateSource(uint8_t index, PdfCoverCandidateSource* output) const;
  uint32_t navigationSpoolBytes() const { return navigationSpoolBytes_; }
  uint8_t navigationSpoolWriteCount() const { return navigationSpoolWriteCount_; }
  uint8_t navigationSpoolReadCount() const { return navigationSpoolReadCount_; }
  uint8_t maskSpoolWriteCount() const { return maskSpoolWriteCount_; }
  uint8_t maskSpoolReadCount() const { return maskSpoolReadCount_; }
 const PdfPreparationWorkCounters& workCounters() const { return workCounters_; }

 private:
  friend struct PdfPreparationTestAccess;

  struct NavigationWorkspace;
  struct ExtractedBlockRecord;
  struct ReadingOrderWorkspace;
  struct RasterBatchWorkspace;
  struct PlacementWorkspace;
  struct SectionRepairRuntime;
  struct PreparedContentRuntime;
  struct PreparedContentOverlay;

  struct PreparedSectionRecord {
    PdfMetadataSection section{};
    PdfRequiredFileRecord file{};
    uint32_t firstSourcePage = 0;
    uint32_t lastSourcePageExclusive = 0;
  };

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

  enum class XrefSortStage : uint8_t {
    Idle,
    CloseSource,
    CloseAppend,
    OpenInitialInput,
    OpenInitialOutput,
    InitialLoad,
    InitialWrite,
    ClosePassInput,
    ClosePassOutput,
    SelectNextPass,
    OpenMergeInput,
    OpenMergeOutput,
    Merge,
    OpenCompactInput,
    OpenCompactOutput,
    Compact,
    Adopt,
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
    FontDictionary,
    FontObject,
    ToUnicodeObject,
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
    UpdateRecord,
    Done,
  };

  enum class InlineNavigationSpillStage : uint8_t {
    None,
    Writing,
    Spilled,
    Reading,
    FontWriting,
    FontSpilled,
    FontReading,
    ObservedCloseReader,
    ObservedOpenWriter,
    ObservedSeekWriter,
    ObservedWriteNavigation,
    ObservedWriteRecords,
    ObservedCloseWriter,
    ObservedReopenReader,
    ObservedSpilled,
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
    ObjectStore,
    ContentStore,
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
    MaterializeImages,
    CloseMaterializedImageSpool,
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
    ReadVerifierMetadata,
    VerifyHeader,
    VerifyRecords,
    VerifyTrailer,
    CloseVerifier,
    PublishResume,
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
    OpenResumeLedger,
    OpenResumeJournal,
    ReadResumeJournalMetadata,
    ReadResumeDiscoveryHeader,
    ValidateResumeDiscoveryXref,
    ValidateResumeDiscoveryPages,
    ValidateResumeDiscoveryOverflow,
    ValidateResumeDiscoveryTrailer,
    RestoreResumeDiscoveryHeader,
    ValidateResumeDiscoveryCatalog,
    RestoreResumeDiscoveryXref,
    ValidateResumeDiscoveryPage,
    RestoreResumeDiscoveryPages,
    ReadResumeJournalRecord,
    CloseResumeJournal,
    ValidateResumePageOpen,
    ValidateResumePageMetadata,
    ValidateResumePageRead,
    ValidateResumePageClose,
    ReadResumeMetadata,
    ReadResumeHeader,
    ReadResumeRecordHeader,
    ReadResumeRecordPath,
    ReadResumeTrailer,
    CloseResumeLedger,
    ValidateResumeOpen,
    ValidateResumeMetadata,
    ValidateResumeRead,
    ValidateResumeClose,
    ValidateEmitSectionsControlOpen,
    ValidateEmitSectionsControlMetadata,
    ValidateEmitSectionsControlRead,
    ValidateEmitSectionsControlClose,
    ValidateEmitSectionsControlDecode,
    ValidateEmitSectionsMetadataOpen,
    ValidateEmitSectionsMetadataMetadata,
    ValidateEmitSectionsMetadataRead,
    ValidateEmitSectionsMetadataClose,
    ValidateEmitSectionsMetadataDecode,
    ValidateEmitSectionsImageBuildOpen,
    ValidateEmitSectionsImageBuild,
    ValidateEmitSectionsImageBuildRecords,
    ValidateEmitSectionsImageBuildClose,
    ValidateEmitSectionsImageFilesOpen,
    ValidateEmitSectionsImageFiles,
    ValidateEmitSectionsImageFileRecords,
    ValidateEmitSectionsImageFilesClose,
    SelectGeneration,
    CreateCacheRoot,
    CreateGeneration,
    CreateSectionDirectory,
    ReadCapacity,
    InitializeBudget,
    InitializeImageCache,
    FormatOutputPaths,
    OpenResumeImageFileWriter,
    CopyResumeImageFileRecords,
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

  enum class CancelStage : uint8_t {
    Idle,
    AbortActiveRasterBeforeNavigationRestore,
    RestoreCancellationNavigation,
    RestoreAfterImageNavigation,
    PrepareEmitSectionsResume,
    PrepareResumeNavigationRecords,
    OpenResumeMetadata,
    WriteResumeMetadata,
    CloseResumeMetadata,
    OpenResumeOutline,
    WriteResumeOutline,
    CloseResumeOutline,
    CommitEmitSectionsControl,
    CommitResumeLedger,
    DestroyPreparedContentRuntime,
    ClosePreparedContentStore,
    RemovePreparedContentStore,
    CloseInlineNavigationSpool,
    RemoveInlineNavigationSpool,
    ResetPreparedContentSnapshot,
    DestroyParsers,
    AbortWriters,
    CloseSource,
    AbortSectionState,
    AbortImageState,
    AbortSpools,
    CloseAuxiliaryHandles,
    PrepareGeneration,
    CommitCheckpoint,
    Release,
    Complete,
  };

  enum class ResumeControlStage : uint8_t {
    Idle,
    OpenWriter,
    Write,
    CloseWriter,
    OpenVerifier,
    ReadVerifierMetadata,
    ReadVerifier,
    CloseVerifier,
    Verify,
    Publish,
    Complete,
  };

  enum class ResumeRecordRestoreStage : uint8_t {
    Idle,
    Open,
    Metadata,
    Header,
    RecordHeader,
    RecordPath,
    Close,
    Complete,
  };

  enum class ResumePointStage : uint8_t {
    Idle,
    OpenJournal,
    SeekJournal,
    WriteDiscoveryHeader,
    WriteDiscoveryXref,
    WriteDiscoveryPages,
    WriteDiscoveryOverflow,
    WriteDiscoveryTrailer,
    WriteRecord,
    CloseJournal,
    CommitCheckpoint,
    Complete,
  };

  enum class ResumeReferenceStage : uint8_t {
    Idle,
    Lookup,
    LookupObjectStream,
    Complete,
  };

  enum class CleanupStage : uint8_t {
    Idle,
    RemoveBuildArtifacts,
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

  enum class PendingSectionFinishStage : uint8_t {
    Idle,
    Append,
    Close,
    Update,
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
    uint64_t* bytesRead = nullptr;
  };

  static PdfStatus readSource(void* context, uint64_t offset, uint8_t* destination, size_t requested,
                              size_t* bytesRead);
  static PdfStepResult setResolverSourceAccess(void* context, PdfObjectResolverReader reader,
                                               PdfWorkBudget& budget);
  static PdfStatus setPageTraversalAccess(void* context, bool traversalRequired);
  static PdfStatus readMemoryRecord(void* context, uint32_t ordinal, void* record, size_t recordSize);
  static PdfStatus writeMemoryRecord(void* context, uint32_t ordinal, const void* record, size_t recordSize);
  static PdfStatus capturePage(void* context, const PdfPageInfo& page);
  static PdfStatus writeSection(void* context, const uint8_t* source, size_t requested, size_t* bytesWritten);
  static PdfStatus writeMetadata(void* context, const uint8_t* source, size_t requested, size_t* bytesWritten);
  static PdfStatus writeOutline(void* context, const uint8_t* source, size_t requested, size_t* bytesWritten);
  static PdfStatus discardInlineImageDecoded(void* context, const uint8_t* source, size_t requested,
                                             size_t* bytesWritten);
  static PdfStatus readPreparedContentSource(void* context, uint64_t offset, uint8_t* destination,
                                             size_t requested, size_t* bytesRead);
  static PdfStatus readPreparedFormContent(void* context, uint64_t offset, uint8_t* destination,
                                           size_t requested, size_t* bytesRead);
  static PdfStepResult replayPreparedInlineImage(void* context, const PdfByteSource& source,
                                                 uint64_t idEndOffset, PdfWorkBudget& budget,
                                                 uint64_t* resumeOffset, PdfContentXObject* image);
  static PdfStatus resetPreparedContentStore(void* context);
  static uint64_t preparedContentStoreSize(void* context);
  static PdfStatus readPreparedContentStore(void* context, uint64_t offset, uint8_t* destination,
                                            size_t requested, size_t* bytesRead);
  static PdfStatus writePreparedContentStore(void* context, const uint8_t* source, size_t requested,
                                             size_t* bytesWritten);
  static PdfStatus readPreparedFontStore(void* context, uint64_t offset, uint8_t* destination,
                                         size_t requested, size_t* bytesRead);
  static PdfStatus writePreparedFontStore(void* context, const uint8_t* source, size_t requested,
                                          size_t* bytesWritten);
  static PdfStatus observePreparedFontRecord(void* context, uint32_t ordinal, const void* record,
                                             size_t recordSize);
  static PdfStatus setPreparedFontSourceAccess(void* context, bool sourceRequired);
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
  bool cancelledGenerationReady() const;
  bool canPreserveSelectedPageResumeOnCancel() const;
  PdfStepResult finishCancelledFailure(PdfStatus status);
  PdfStatus closeSource();
  PdfStatus reopenSource();
  void destroyParsers();
  PdfJpegPreview* constructJpegPreview();
  void destroyJpegPreview();
  void releaseWorkspaces();
  bool allocateNextWorkspace();
  PdfStatus initializeParserStorage();
  PdfStatus configureDefaultParserArena();
  PdfStatus configureNavigationParserArena();
  PdfStatus beginXrefSpool();
  PdfStepResult stepSortXref(PdfWorkBudget& budget);
  PdfStepResult stepResolverObjectStoreWriter(PdfWorkBudget& budget);
  PdfStepResult stepResolverObjectStoreReader(PdfWorkBudget& budget);
  PdfStatus resetResolverWorkspace();
  PdfStatus accountResolverStreamBytes();
  uint64_t resolverObjectStoreCapacity() const;
  void abortResolverObjectStore();
  PdfStatus switchResolverSourceAccess(PdfObjectResolverReader reader);
  PdfStatus switchPageTraversalAccess(bool traversalRequired);
  PdfStatus formatXrefSpoolPath(uint8_t index, char* output, size_t capacity) const;
  void abortXrefSpools();
  PdfStatus beginPageSpools();
  PdfStatus beginPreparedPageSpool();
  PdfStatus resumePreparedPageSpool();
  PdfStatus finishPageDiscoverySpool();
  PdfStatus finishPreparedPageSpool();
  PdfStatus sealPreparedPageSpool();
  PdfStatus loadPageRecord(uint32_t index);
  PdfStatus loadAnnotationOverflowBatch(uint16_t count);
  PdfStatus appendResolvedLink(const void* record, size_t recordSize);
  PdfStatus finishResolvedLinkSpool();
  PdfStepResult stepPreparePageLinks(PdfWorkBudget& budget);
  PdfStatus readPreparedPageRecord(uint16_t index, PreparedSectionRecord* record);
  PdfStatus writePreparedPageRecord(const PreparedSectionRecord& record);
  PdfStatus rewritePreparedPageRecord(PdfWorkBudget& budget, uint16_t index, const PreparedSectionRecord& record);
  PdfStatus formatPageSpoolPath(bool prepared, char* output, size_t capacity) const;
  PdfStatus formatAnnotationOverflowSpoolPath(char* output, size_t capacity) const;
  PdfStatus formatResolvedLinkSpoolPath(char* output, size_t capacity) const;
  PdfStatus formatTraversalSpoolPath(char* output, size_t capacity) const;
  void abortPageSpools();
  PdfStatus readCacheSetupBytes(PdfWorkBudget& budget, uint64_t offset, uint8_t* destination, size_t length);
  void rejectResumeState();
  PdfStatus decodeEmitSectionsResumeMetadataPrefix(size_t length);
  PdfStepResult stepSetupCache(PdfWorkBudget& budget);
  PdfStepResult stepSetupCheckpointAndManifest(PdfWorkBudget& budget);
  PdfStepResult stepSetupDiscoveryRestore(PdfWorkBudget& budget);
  PdfStepResult stepSetupResumeProductValidation(PdfWorkBudget& budget);
  PdfStepResult stepSetupGeneration(PdfWorkBudget& budget);
  PdfStepResult stepSetupOutputs(PdfWorkBudget& budget);
  bool canResumeAfterEmitSections() const;
  PdfStatus encodeEmitSectionsResumeControl(uint8_t* output, size_t capacity) const;
  PdfStatus decodeEmitSectionsResumeControl(const uint8_t* input, size_t length);
  PdfStepResult stepCommitEmitSectionsResumeControl(PdfWorkBudget& budget);
  PdfStatus encodePageResumeRecord(uint8_t* output, size_t capacity) const;
  PdfStatus decodePageResumeRecord(const uint8_t* input, size_t length, uint32_t expectedSequence,
                                   uint32_t expectedPage);
  PdfStatus finalizeDiscoveryXref();
  PdfStatus encodeDiscoveryHeader(uint8_t* output, size_t capacity) const;
  PdfStatus decodeDiscoveryHeader(const uint8_t* input, size_t length, bool restore);
  PdfStatus encodeDiscoveryXrefRecord(uint32_t ordinal, uint8_t* output, size_t capacity);
  PdfStatus decodeDiscoveryXrefRecord(const uint8_t* input, size_t length, uint32_t ordinal, PdfXrefEntry* entry) const;
  PdfStatus encodeDiscoveryPageRecord(uint16_t ordinal, uint8_t* output, size_t capacity);
  PdfStatus decodeDiscoveryPageRecord(const uint8_t* input, size_t length, uint16_t ordinal, PdfPageInfo* page) const;
  PdfStatus encodeDiscoveryOverflowRecord(uint32_t ordinal, uint8_t* output, size_t capacity);
  PdfStatus decodeDiscoveryOverflowRecord(const uint8_t* input, size_t length, uint32_t ordinal,
                                          PdfObjectReference* reference) const;
  PdfStatus encodeDiscoveryTrailer(uint8_t* output, size_t capacity) const;
  PdfStatus decodeDiscoveryTrailer(const uint8_t* input, size_t length) const;
  PdfStatus beginDiscoveryXrefRestore();
  PdfStatus finishDiscoveryXrefRestore();
  PdfStatus appendDiscoveryPage(const PdfPageInfo& page);
  PdfStatus beginResumeReferenceValidation(PdfObjectReference reference, bool allowNull,
                                           bool validateObjectStream);
  PdfStepResult stepResumeReferenceValidation(PdfWorkBudget& budget);
  PdfStepResult writeResumeJournalBuffer(PdfWorkBudget& budget, const uint8_t* source, size_t length);
  PdfStepResult stepCommitPageResume(PdfWorkBudget& budget);
  PdfStatus continueAfterPageResume();
  PdfStepResult stepResumeAfterEmitSections(PdfWorkBudget& budget);
  PdfStepResult stepRestoreResumeRecords(PdfWorkBudget& budget);
  PdfStatus startXref();
  PdfStatus finishXref();
  PdfStatus finishCatalog();
  PdfStatus finishPageTree();
  PdfStatus beginNavigationDiscovery();
  PdfStatus finishNavigationObject();
  PdfStepResult stepStartNextNavigationObject(PdfWorkBudget& budget);
  PdfStatus flushOutlineBatch();
  PdfStatus readNavigationRecord(uint32_t index, PdfOutlineEntry* record);
  PdfStatus writeNavigationRecord(uint32_t index, const PdfOutlineEntry& record);
  uint16_t* sectionBoundaryPages();
  const uint16_t* sectionBoundaryPages() const;
  bool isSectionBoundary(uint32_t pageIndex) const;
  uint16_t sectionForPage(uint32_t pageIndex) const;
  PdfStatus readXmpMetadata();
  uint32_t pageReferenceLookupCapacity() const;
  void rememberPageReference(uint32_t pageIndex, PdfObjectReference reference);
  PdfObjectReference recalledPageReference(uint32_t pageIndex) const;
  PdfStatus resolveDestination(const PdfRawDestination& raw, PdfResolvedDestination* destination);
  PdfStatus beginCurrentPageImages();
  PdfStatus skipCurrentUnreadablePage();
  PdfStatus finishImageResolution();
  PdfStatus finishAuxiliaryImageResolution();
  PdfStatus continueImageDescriptorResolution();
  PdfStatus finishResolvedImageColorSpace(bool indexedBase);
  PdfStatus finishResolvedImagePalette();
  PdfStatus allocateImagePalette(uint8_t** palette);
  PdfStatus collectImageCandidates(uint16_t dictionaryIndex, uint8_t ownerScopeIndex);
  PdfStatus collectFontCandidates(uint16_t dictionaryIndex, uint8_t scopeIndex);
  PdfStatus beginNextImageObject();
  PdfStatus beginNextFontObject();
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
  PdfStatus beginPreparedContentDecode();
  PdfStepResult suspendContentNavigation(PdfWorkBudget& budget);
  PdfStepResult checkPreparedContentCapacity(PdfWorkBudget& budget);
  PdfStepResult openPreparedContentStore(PdfWorkBudget& budget);
  PdfStepResult decodePreparedContent(PdfWorkBudget& budget);
  PdfStepResult restoreContentNavigation(PdfWorkBudget& budget);
  PdfStepResult openDecodedContent(PdfWorkBudget& budget);
  PdfStepResult cleanupPreparedContentStore(PdfWorkBudget& budget);
  PdfStatus beginFormReachability();
  PdfStepResult stepFormReachability(PdfWorkBudget& budget);
  PdfStepResult prepareReachableForm(PdfWorkBudget& budget);
  PdfStatus setPreparedContentRange(uint8_t logicalIndex);
  PdfStatus beginDecodedContentExtraction();
  PdfStatus beginPreparedFonts();
  PdfStepResult spoolFontNavigation(PdfWorkBudget& budget);
  PdfStepResult decodePreparedFonts(PdfWorkBudget& budget);
  PdfStepResult parsePreparedFonts(PdfWorkBudget& budget);
  PdfStepResult restoreFontNavigation(PdfWorkBudget& budget);
  PdfStepResult reopenPreparedContent(PdfWorkBudget& budget);
  void abortPreparedFontStore();
  PdfStatus beginContentInterpretation();
  PdfStepResult stepContentInterpretation(PdfWorkBudget& budget);
  PdfStatus finishContentInterpretation();
  void destroyContentInterpretation();
  PdfStatus observeFontAlias();
  void clearPendingObservedCodes();
  void commitPendingObservedCodes();
  bool observedJournalSpillPending() const;
  PdfStatus beginObservedJournalSpill(const PdfToken* retryToken = nullptr);
  PdfStepResult stepObservedJournalSpill(PdfWorkBudget& budget);
  uint8_t* preparedNavigationSpillBytes(size_t offset, size_t* contiguousBytes);
  bool preparedContentRuntimeConstructed() const;
  void destroyPreparedContentRuntime();
  void abortPreparedContentStore();
  PdfStatus appendContentToken(const PdfToken& token);
  PdfStatus consumeInlineImageToken(const PdfToken& token);
  void finalizeInlineImageDictionary();
  void resetInlineImageDictionaryState();
  PdfStatus initializeInlineImageDataOffset(const PdfByteSource& contentSource);
  PdfStepResult finishInlineImageData(PdfWorkBudget& budget);
  void abortInlineNavigationSpill();
  PdfStatus retainInlineImage(uint64_t dataOffset, uint64_t dataLength);
  PdfStatus finishExtractedPage();
  PdfStepResult stepReadingOrder(PdfWorkBudget& budget);
  PdfStatus formatCurrentSectionPath();
  PdfStatus formatInternalLink(uint16_t sourcePageIndex, int16_t x, int16_t y, char* href, size_t capacity,
                               size_t* hrefLength) const;
  PdfStepResult stepOpenSection(PdfWorkBudget& budget);
  PdfStepResult stepFinishPendingSection(PdfWorkBudget& budget);
  PdfStepResult emitSection(PdfWorkBudget& budget);
  PdfStepResult stepCloseSection(PdfWorkBudget& budget);
  PdfStatus beginNavigationRecords();
  PdfStepResult stepPrepareNavigationRecords(PdfWorkBudget& budget);
  PdfStepResult stepTypographyAssets(PdfWorkBudget& budget);
  PdfStatus openMetadata();
  PdfStepResult stepWriteMetadata(PdfWorkBudget& budget);
  PdfStatus closeMetadata();
  PdfStatus openOutline();
  PdfStepResult stepWriteOutline(PdfWorkBudget& budget);
  PdfStatus closeOutline();
  PdfStepResult stepCommitManifest(PdfWorkBudget& budget, bool resumeLedger = false);
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
  CancelStage cancelStage_ = CancelStage::Idle;
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
  PdfFixedRecordSpool xrefSpools_[2]{};
  PdfFixedRecordSpool pageSpool_{};
  PdfFixedRecordSpool annotationOverflowSpool_{};
  PdfFixedRecordSpool resolvedLinkSpool_{};
  PdfFixedRecordSpool preparedPageSpool_{};
  PdfMutableRecordSpool traversalSpool_{};
  XrefSortStage xrefSortStage_ = XrefSortStage::Idle;
  PdfObjectReference xrefSortRoot_{};
  PdfObjectReference xrefSortInfo_{};
  PdfXrefEntry xrefSortLeft_{};
  PdfXrefEntry xrefSortRight_{};
  uint32_t xrefSortTotal_ = 0;
  uint32_t xrefSortRunLength_ = 0;
  uint32_t xrefSortRunStart_ = 0;
  uint32_t xrefSortRunCount_ = 0;
  uint32_t xrefSortBufferIndex_ = 0;
  uint32_t xrefSortPairStart_ = 0;
  uint32_t xrefSortMiddle_ = 0;
  uint32_t xrefSortEnd_ = 0;
  uint32_t xrefSortLeftIndex_ = 0;
  uint32_t xrefSortRightIndex_ = 0;
  uint32_t xrefSortDestination_ = 0;
  uint32_t xrefSortUniqueCount_ = 0;
  uint32_t xrefSortPreviousObject_ = 0;
  uint8_t xrefSortInput_ = 0;
  uint8_t xrefSortOutput_ = 1;
  uint8_t xrefFinalSpool_ = 0xff;
  bool xrefSortHasInfo_ = false;
  bool xrefSortLeftLoaded_ = false;
  bool xrefSortRightLoaded_ = false;
  bool xrefSortPreviousValid_ = false;
  bool xrefSortCompacting_ = false;
  bool xrefSortFastPath_ = false;
  uint32_t loadedPageIndex_ = UINT32_MAX;
  uint32_t currentPageFirstAnchor_ = UINT32_MAX;
  uint32_t nextPageAnchorHint_ = UINT32_MAX;
  uint32_t nextPageAnchorHintIndex_ = UINT32_MAX;
  uint16_t currentPageFirstSection_ = UINT16_MAX;
  uint16_t currentPageWidth_ = 0;
  uint16_t currentPageHeight_ = 0;
  bool preparedPageSpoolWriting_ = false;
  bool preparedPageSpoolUpdating_ = false;
  NavigationWorkspace* navigation_ = nullptr;
  std::optional<PdfNamedDestinationMap> namedDestinations_;
  std::optional<PdfPageLabelMap> pageLabels_;
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
  uint16_t sectionBoundaryCount_ = 0;
  bool pendingSectionBoundary_ = false;
  uint16_t outlineVisitedCount_ = 0;
  bool synthesizedOutline_ = false;
  int16_t currentOutlineParent_ = -1;
  uint8_t currentOutlineParentLevel_ = 0;
  uint8_t outlineBatchCount_ = 0;
  uint16_t currentAnnotationPage_ = 0;
  uint16_t currentPageOverflowAnnotationIndex_ = 0;
  uint32_t currentAnnotationOverflowOrdinal_ = 0;
  uint32_t resolvedLinkCount_ = 0;
  uint32_t currentResolvedLinkOrdinal_ = 0;
  uint8_t currentAnnotationIndex_ = 0;
  uint8_t currentAnnotationOverflowBatchIndex_ = 0;
  uint8_t currentAnnotationOverflowBatchCount_ = 0;
  uint16_t currentPageLinkBlockIndex_ = 0;
  uint8_t currentPageLinkBatchIndex_ = 0;
  uint8_t currentPageLinkBatchCount_ = 0;
  bool resolvedLinksSpilled_ = false;
  bool resolvedLinkSpoolFinished_ = false;
  bool currentPageLinkFinalizing_ = false;
  uint8_t navigationStage_ = 0;
  NavigationTask navigationTask_ = NavigationTask::None;
  ImageResolveTask imageResolveTask_ = ImageResolveTask::None;
  uint8_t imageCandidateCount_ = 0;
  uint8_t xObjectCandidateCount_ = 0;
  uint8_t formScopeCount_ = 0;
  uint8_t imageDescriptorCandidateIndex_ = UINT8_MAX;
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
  bool sectionOpenPrepared_ = false;
  bool sectionClosePrepared_ = false;
  bool sectionCloseNewSection_ = false;
  bool pendingSectionFinish_ = false;
  PendingSectionFinishStage pendingSectionFinishStage_ = PendingSectionFinishStage::Idle;
  bool rasterRuntimeActive_ = false;
  bool maskDecodeRuntimeActive_ = false;
  bool maskCompositeRuntimeActive_ = false;
  bool sectionRepairRuntimeActive_ = false;
  RasterDecodeStage rasterDecodeStage_ = RasterDecodeStage::Idle;
  uint64_t failedRasterImages_ = 0;
  bool hasInfoReference_ = false;
  PdfByteRange contentRange_{};
  std::optional<PdfLexer> contentLexer_;
  uint32_t contentAppendOffset_ = 0;
  uint32_t contentAppendLength_ = 0;
  uint8_t formReachabilityIndex_ = 0;
  uint8_t formAppendCandidateIndex_ = UINT8_MAX;
  bool contentAppendActive_ = false;
  size_t transcriptLength_ = 0;
  uint32_t nextAnchorOrdinal_ = 0;
  uint32_t currentSectionFirstWord_ = 0;
  uint32_t currentSectionFirstAnchor_ = 0;
  uint64_t cumulativeSectionBytes_ = 0;
  uint64_t cumulativeImageBytes_ = 0;
  size_t metadataEncodeBytes_ = 0;
  uint64_t xmpStreamOffset_ = 0;
  uint64_t xmpStreamLength_ = 0;

  PdfCacheStore cacheStore_;
  PdfCacheCapacity cacheCapacity_{};
  PdfCacheBudget cacheBudget_{};
  PdfCacheManifestSelection manifestSelection_{};
  PdfBuildCheckpointSelection checkpointSelection_{};
  // Section, metadata, and outline output phases never overlap. Reuse one
  // tracked writer so their 320-byte path/record state is paid once on C3.
  PdfCacheTrackedWriter outputWriter_{};
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
  uint64_t resumeValidationOffset_ = 0;
  uint32_t resumeValidationCrc32_ = 0;
  bool resumeLedgerReady_ = false;
  bool resumeLedgerValid_ = false;
  bool resumeGenerationRejected_ = false;
  bool resumeValidationFailed_ = false;
  bool resumePageNeedsTruncate_ = false;
  bool resumeAfterPage_ = false;
  bool manifestRecordsMaterialized_ = false;
  bool cacheGenerationsListed_ = false;
  bool emitSectionsResumeReady_ = false;
  bool resumeAfterEmitSections_ = false;
  bool emitSectionsResumeValidated_ = false;
  ResumeControlStage resumeControlStage_ = ResumeControlStage::Idle;
  ResumeRecordRestoreStage resumeRecordRestoreStage_ = ResumeRecordRestoreStage::Idle;
  ResumePointStage resumePointStage_ = ResumePointStage::Idle;
  PdfCacheHandle resumeJournalHandle_{};
  char resumeJournalPath_[PDF_CACHE_PATH_CAPACITY]{};
  uint64_t resumeJournalCommittedBytes_ = 0;
  uint64_t resumeJournalScanOffset_ = 0;
  uint64_t resumeJournalPhysicalBytes_ = 0;
  uint64_t resumePageValidationOffset_ = 0;
  uint32_t resumePageValidationCrc32_ = 0;
  uint32_t resumeJournalRecordSequence_ = 0;
  uint32_t resumeJournalScanSequence_ = 0;
  uint32_t durableResumePage_ = 0;
  uint32_t pendingResumePage_ = 0;
  uint16_t resumePageValidationIndex_ = 0;
  uint8_t resumeReferenceIndex_ = 0;
  bool resumeReferenceValidateObjectStream_ = false;
  PdfObjectReference resumeReference_{};
  PdfXrefLookupState resumeXrefLookup_{};
  PdfXrefEntry resumeXrefEntry_{};
  ResumeReferenceStage resumeReferenceStage_ = ResumeReferenceStage::Idle;
  PdfBuildResumePhase durableResumePhase_ = PdfBuildResumePhase::None;
  PdfBuildResumePhase resumedPhase_ = PdfBuildResumePhase::None;
  PdfCacheHandle checkpointCommitHandle_{};
  CheckpointCommitStage checkpointCommitStage_ = CheckpointCommitStage::Idle;
  uint8_t cleanupIndex_ = 0;
  CleanupStage cleanupStage_ = CleanupStage::Idle;
  PdfCacheHandle manifestHandle_{};
  char manifestPath_[PDF_CACHE_PATH_CAPACITY]{};
  char resumeLedgerPath_[PDF_CACHE_PATH_CAPACITY]{};
  uint64_t manifestOffset_ = 0;
  uint32_t manifestEncodedBytes_ = 0;
  uint32_t manifestRecordIndex_ = 0;
  uint32_t manifestCrc32_ = 0;
  uint32_t manifestReadCrc32_ = 0;
  uint8_t manifestTargetSlot_ = 0;
  ManifestCommitStage manifestCommitStage_ = ManifestCommitStage::Idle;
  bool manifestResumeLedger_ = false;
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
  PdfJpegPreview* jpegPreview_ = nullptr;
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
  PdfPreparationWorkCounters workCounters_{};
  PdfCoverCandidateSource coverCandidateSources_[PdfLimits::MaxCoverCandidateSources]{};
  uint8_t coverCandidateSourceCount_ = 0;
  uint32_t expandedRequiredBytes_ = 0;
};

#if UINTPTR_MAX == UINT32_MAX
static_assert(sizeof(PdfPreparation) <= 13120,
              "PDF preparation control state must stay bounded with SD-backed link spools");
#endif

static_assert(sizeof(PdfCoverCandidateSource) <= 16, "cover discovery must retain only small object references");
static_assert(sizeof(PdfPreparation) + PdfLimits::TotalWorkspaceBytes <= PDF_MAX_OWNED_HEAP_BYTES,
              "PDF preparation state and fixed workspaces must stay within the 80 KiB heap envelope");
