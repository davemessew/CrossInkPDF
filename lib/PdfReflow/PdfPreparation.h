#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>

#include "PdfCacheStore.h"
#include "PdfLimits.h"
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
  ResolveContent,
  ExtractText,
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

 private:
  struct NavigationWorkspace;
  struct ExtractedBlockRecord;

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
  static PdfStatus emitBlock(void* context, const PdfSemanticBlockRecord& record);
  static PdfStatus readMetadataSection(void* context, uint16_t index, PdfMetadataSection* record);
  static PdfStatus readOutlineEntry(void* context, uint16_t index, PdfOutlineEntry* record);
  static PdfStatus readRequiredFile(void* context, uint32_t index, PdfRequiredFileRecord* record);
  static bool stopRequested(void* context);

  PdfFixedRecordStore recordStore(MemoryRecordContext& context);
  PdfByteSource source();
  PdfStepResult pause();
  PdfStepResult fail(PdfStatus status);
  PdfStepResult cancel();
  PdfStatus closeSource();
  void destroyParsers();
  void releaseWorkspaces();
  bool allocateNextWorkspace();
  PdfStatus initializeParserStorage();
  PdfStatus setupCache();
  PdfStatus startXref();
  PdfStatus finishXref();
  PdfStatus finishCatalog();
  PdfStatus finishPageTree();
  PdfStatus beginNavigationDiscovery();
  PdfStatus finishNavigationObject();
  PdfStatus startNextNavigationObject();
  PdfStatus readXmpMetadata();
  PdfStatus resolveDestination(const PdfRawDestination& raw, PdfResolvedDestination* destination) const;
  PdfStatus beginCurrentPageContent();
  PdfStatus finishContentObject();
  PdfStatus appendContentToken(const PdfToken& token);
  PdfStatus finishExtractedPage();
  PdfStatus formatCurrentSectionPath();
  PdfStatus formatInternalLink(uint16_t sourcePageIndex, const uint8_t* text, size_t textLength, char* href,
                               size_t capacity, size_t* hrefLength) const;
  PdfStatus openSection();
  PdfStatus emitSection();
  PdfStatus closeSection();
  PdfStatus prepareNavigationRecords();
  PdfStatus openMetadata();
  PdfStatus writeMetadata();
  PdfStatus closeMetadata();
  PdfStatus openOutline();
  PdfStatus writeOutline();
  PdfStatus closeOutline();
  PdfStatus commitManifest();
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
  uint16_t sectionCount_ = 0;
  uint16_t explicitOutlineCount_ = 0;
  uint16_t outlinePendingCount_ = 0;
  uint16_t outlineSeenCount_ = 0;
  int16_t currentOutlineParent_ = -1;
  uint16_t currentAnnotationPage_ = 0;
  uint8_t currentAnnotationIndex_ = 0;
  uint8_t navigationStage_ = 0;
  NavigationTask navigationTask_ = NavigationTask::None;
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
  uint64_t xmpStreamOffset_ = 0;
  uint64_t xmpStreamLength_ = 0;

  PdfCacheStore cacheStore_;
  PdfCacheCapacity cacheCapacity_{};
  PdfCacheBudget cacheBudget_{};
  PdfBuildCheckpointSelection checkpointSelection_{};
  PdfCacheTrackedWriter sectionWriter_{};
  PdfCacheTrackedWriter metadataWriter_{};
  PdfCacheTrackedWriter outlineWriter_{};
  PdfRequiredFileRecord sectionRecord_{};
  PdfRequiredFileRecord metadataRecord_{};
  PdfRequiredFileRecord outlineRecord_{};
  PdfSemanticWriter semanticWriter_{};
  PdfMetadataBuilder metadataBuilder_{};
  PdfMetadata metadata_{};
  uint32_t generation_ = 0;
  uint32_t sequence_ = 0;
  uint32_t totalWords_ = 0;
  PdfCoverCandidateSource coverCandidateSources_[PdfLimits::MaxCoverCandidateSources]{};
  uint8_t coverCandidateSourceCount_ = 0;
};

static_assert(sizeof(PdfCoverCandidateSource) <= 16, "cover discovery must retain only small object references");
static_assert(sizeof(PdfPreparation) + PdfLimits::TotalWorkspaceBytes <= PDF_MAX_OWNED_HEAP_BYTES,
              "PDF preparation state and fixed workspaces must stay within the 80 KiB heap envelope");
