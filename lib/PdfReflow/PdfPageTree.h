#pragma once

#include <cstdint>

#include "PdfLimits.h"
#include "PdfObjectResolver.h"

struct PdfPageTreeRecord {
  PdfObjectReference reference{};
  PdfObjectReference resourceOwner{};
  PdfObjectReference resourceReference{};
  PdfRectangle mediaBox{};
  PdfRectangle cropBox{};
  uint32_t parentOrdinal = UINT32_MAX;
  uint32_t nextStackOrdinal = UINT32_MAX;
  uint16_t depth = 0;
  uint16_t rotation = 0;
  bool hasResources = false;
  bool resourcesIndirect = false;
  bool hasMediaBox = false;
  bool hasCropBox = false;
};

struct PdfPageInfo {
  PdfObjectReference pageReference{};
  PdfObjectReference resourceOwner{};
  PdfObjectReference resourceReference{};
  PdfObjectReference contents[PdfLimits::MaxContentStreamsPerPage]{};
  PdfObjectReference annotations[PdfLimits::MaxLinkAnnotationsPerPage]{};
  int32_t viewXMin = 0;
  int32_t viewYMin = 0;
  uint32_t pageIndex = 0;
  uint16_t pageWidth = 0;
  uint16_t pageHeight = 0;
  uint16_t rotation = 0;
  uint8_t contentCount = 0;
  uint8_t annotationCount = 0;
  bool hasResources = false;
  bool resourcesIndirect = false;
};

static_assert(sizeof(PdfPageTreeRecord) <= 72,
              "page-tree inheritance state must stay in fixed record storage");
static_assert(sizeof(PdfPageInfo) <= 304,
              "captured page metadata must stay bounded");

class PdfPageTreeWalker {
 public:
  using PageFn = PdfStatus (*)(void* context, const PdfPageInfo& page);
  using TraversalAccessFn = PdfStatus (*)(void* context, bool required);

  PdfPageTreeWalker(PdfObjectResolver& resolver, PdfObjectArena& arena, PdfFixedRecordStore traversalStore,
                    PageFn pageFn, void* pageContext, TraversalAccessFn traversalAccess, void* traversalContext,
                    PdfPageInfo* pageWorkspace,
                    uint32_t maxPages = PdfLimits::MaxPages);

  PdfStatus begin(PdfObjectReference rootPages);
  PdfStepResult step(PdfWorkBudget& budget);
  uint32_t pageCount() const { return pageCount_; }

 private:
  enum class Phase : uint8_t {
    Idle,
    Initialize,
    NeedNode,
    Resolving,
    OpenTraversal,
    Processing,
    CloseTraversal,
    Done,
    Failed,
  };

  enum class ProcessStage : uint8_t {
    Idle,
    Begin,
    LoadChild,
    CheckAncestor,
    WriteChild,
    EmitPage,
    Complete,
  };

  PdfStepResult processResolvedNode(PdfWorkBudget& budget);
  PdfStepResult stepTraversalRecord(bool write, uint32_t ordinal, PdfPageTreeRecord* record,
                                    PdfWorkBudget& budget);
  PdfStepResult stepTraversalAccess(bool required, uint32_t reservedOperations, PdfWorkBudget& budget);

  PdfObjectResolver& resolver_;
  PdfObjectArena& arena_;
  PdfFixedRecordStore traversalStore_{};
  PageFn pageFn_ = nullptr;
  void* pageContext_ = nullptr;
  TraversalAccessFn traversalAccess_ = nullptr;
  void* traversalContext_ = nullptr;
  PdfPageInfo* pageWorkspace_ = nullptr;
  uint32_t maxPages_ = PdfLimits::MaxPages;
  uint32_t recordCount_ = 0;
  uint32_t stackTop_ = UINT32_MAX;
  uint32_t currentOrdinal_ = UINT32_MAX;
  uint32_t pageCount_ = 0;
  uint32_t declaredRootCount_ = 0;
  uint32_t processingStackTop_ = UINT32_MAX;
  uint32_t ancestorOrdinal_ = UINT32_MAX;
  uint16_t kidsValueIndex_ = PDF_INVALID_INDEX;
  uint16_t kidsRemaining_ = 0;
  uint16_t ancestorVisited_ = 0;
  bool hasDeclaredRootCount_ = false;
  bool pageCaptured_ = false;
  bool traversalOpen_ = false;
  PdfObjectReference rootPages_{};
  PdfObjectReference childReference_{};
  PdfPageTreeRecord current_{};
  PdfPageTreeRecord recordScratch_{};
  ProcessStage processStage_ = ProcessStage::Idle;
  Phase phase_ = Phase::Idle;
};
