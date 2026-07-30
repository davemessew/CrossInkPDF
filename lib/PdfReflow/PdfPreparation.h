#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>

#include "PdfCacheStore.h"
#include "PdfLimits.h"
#include "PdfObjectResolver.h"
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
  ResolveContent,
  ExtractText,
  CloseSource,
  OpenSection,
  EmitSection,
  CloseSection,
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

 private:
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
  static PdfStatus emitBlock(void* context, const PdfSemanticBlockRecord& record);
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
  PdfStatus finishContentObject();
  PdfStatus appendContentToken(const PdfToken& token);
  PdfStatus openSection();
  PdfStatus emitSection();
  PdfStatus closeSection();
  PdfStatus commitManifest();
  PdfStatus commitCheckpoint(PdfBuildPhase phase);
  uint32_t nowMs() const;
  void setPhase(PdfPreparationPhase phase, uint8_t progressPercent);

  PdfPreparationConfig config_{};
  char sourcePath_[512]{};
  char cacheRoot_[PDF_CACHE_PATH_CAPACITY]{};
  char sectionPath_[PDF_CACHE_PATH_CAPACITY]{};
  char sectionRelativePath_[PDF_CACHE_REQUIRED_PATH_CAPACITY]{};
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
  PdfPageInfo firstPage_{};
  uint32_t pageCount_ = 0;
  PdfByteRange contentRange_{};
  std::optional<PdfLexer> contentLexer_;
  size_t transcriptLength_ = 0;

  PdfCacheStore cacheStore_;
  PdfCacheCapacity cacheCapacity_{};
  PdfCacheBudget cacheBudget_{};
  PdfBuildCheckpointSelection checkpointSelection_{};
  PdfCacheTrackedWriter sectionWriter_{};
  PdfRequiredFileRecord sectionRecord_{};
  PdfSemanticWriter semanticWriter_{};
  uint32_t generation_ = 0;
  uint32_t sequence_ = 0;
  uint32_t totalWords_ = 0;
};

static_assert(sizeof(PdfPreparation) + PdfLimits::TotalWorkspaceBytes <= PDF_MAX_OWNED_HEAP_BYTES,
              "PDF preparation state and fixed workspaces must stay within the 80 KiB heap envelope");
