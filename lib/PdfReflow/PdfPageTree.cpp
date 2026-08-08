#include "PdfPageTree.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <new>

namespace {

bool valueIsName(const PdfObjectArena& arena, const PdfValue& value, const char* expected) {
  return value.kind == PdfValueKind::Name && pdfTextEquals(arena, value, expected);
}

bool numericFixed(const PdfValue& value, int32_t* const result) {
  if (result == nullptr) {
    return false;
  }
  if (value.kind == PdfValueKind::Real) {
    *result = value.fixedValue;
    return true;
  }
  constexpr int64_t kFixedOne = INT64_C(65536);
  if (value.kind != PdfValueKind::Integer ||
      value.integerValue <
          std::numeric_limits<int32_t>::min() / kFixedOne ||
      value.integerValue >
          std::numeric_limits<int32_t>::max() / kFixedOne) {
    return false;
  }
  *result = static_cast<int32_t>(value.integerValue * kFixedOne);
  return true;
}

PdfStatus parsePageBox(const PdfObjectArena& arena, const uint16_t valueIndex,
                       PdfRectangle* const result) {
  if (result == nullptr || valueIndex >= arena.valueCount ||
      arena.values[valueIndex].kind != PdfValueKind::Array ||
      arena.values[valueIndex].count != 4) {
    return PdfStatus::failure(PdfError::Malformed, valueIndex);
  }
  int32_t coordinates[4]{};
  for (uint16_t ordinal = 0; ordinal < 4; ++ordinal) {
    uint16_t coordinateIndex = PDF_INVALID_INDEX;
    if (!pdfArrayAt(arena, valueIndex, ordinal, &coordinateIndex) ||
        coordinateIndex >= arena.valueCount ||
        !numericFixed(arena.values[coordinateIndex], &coordinates[ordinal])) {
      return PdfStatus::failure(PdfError::Malformed, coordinateIndex);
    }
  }
  const PdfRectangle box{
      std::min(coordinates[0], coordinates[2]),
      std::min(coordinates[1], coordinates[3]),
      std::max(coordinates[0], coordinates[2]),
      std::max(coordinates[1], coordinates[3]),
  };
  if (box.xMin >= box.xMax || box.yMin >= box.yMax) {
    return PdfStatus::failure(PdfError::Malformed, valueIndex);
  }
  *result = box;
  return PdfStatus::success();
}

PdfStatus parsePageRotation(const PdfObjectArena& arena,
                            const uint16_t valueIndex,
                            uint16_t* const result) {
  if (result == nullptr || valueIndex >= arena.valueCount ||
      arena.values[valueIndex].kind != PdfValueKind::Integer ||
      arena.values[valueIndex].integerValue % 90 != 0) {
    return PdfStatus::failure(PdfError::Malformed, valueIndex);
  }
  int64_t normalized = arena.values[valueIndex].integerValue % 360;
  if (normalized < 0) {
    normalized += 360;
  }
  *result = static_cast<uint16_t>(normalized);
  return PdfStatus::success();
}

PdfRectangle effectivePageBox(const PdfPageTreeRecord& inherited) {
  if (!inherited.hasCropBox) {
    return inherited.mediaBox;
  }
  const PdfRectangle intersection{
      std::max(inherited.mediaBox.xMin, inherited.cropBox.xMin),
      std::max(inherited.mediaBox.yMin, inherited.cropBox.yMin),
      std::min(inherited.mediaBox.xMax, inherited.cropBox.xMax),
      std::min(inherited.mediaBox.yMax, inherited.cropBox.yMax),
  };
  return intersection.xMin < intersection.xMax &&
                 intersection.yMin < intersection.yMax
             ? intersection
             : inherited.mediaBox;
}

PdfStatus orientedPageExtent(const PdfPageTreeRecord& inherited,
                             uint16_t* const width,
                             uint16_t* const height) {
  if (!inherited.hasMediaBox || width == nullptr || height == nullptr) {
    return PdfStatus::failure(PdfError::Malformed);
  }
  const PdfRectangle box = effectivePageBox(inherited);
  const uint64_t fixedWidth =
      static_cast<uint64_t>(static_cast<int64_t>(box.xMax) - box.xMin);
  const uint64_t fixedHeight =
      static_cast<uint64_t>(static_cast<int64_t>(box.yMax) - box.yMin);
  const uint64_t roundedWidth = (fixedWidth + 65535U) >> 16U;
  const uint64_t roundedHeight = (fixedHeight + 65535U) >> 16U;
  if (roundedWidth == 0 || roundedHeight == 0 ||
      roundedWidth > UINT16_MAX || roundedHeight > UINT16_MAX) {
    return PdfStatus::failure(PdfError::LimitExceeded);
  }
  uint16_t resolvedWidth = static_cast<uint16_t>(roundedWidth);
  uint16_t resolvedHeight = static_cast<uint16_t>(roundedHeight);
  if (inherited.rotation == 90 || inherited.rotation == 270) {
    std::swap(resolvedWidth, resolvedHeight);
  }
  *width = resolvedWidth;
  *height = resolvedHeight;
  return PdfStatus::success();
}

}  // namespace

PdfPageTreeWalker::PdfPageTreeWalker(PdfObjectResolver& resolver, PdfObjectArena& arena,
                                     const PdfFixedRecordStore traversalStore, const PageFn pageFn, void* pageContext,
                                     const TraversalAccessFn traversalAccess, void* traversalContext,
                                     PdfPageInfo* const pageWorkspace,
                                     const PdfFixedRecordStore annotationOverflowStore,
                                     const uint32_t maxPages,
                                     const PdfFixedRecordStore contentOverflowStore)
    : resolver_(resolver),
      arena_(arena),
      traversalStore_(traversalStore),
      annotationOverflowStore_(annotationOverflowStore),
      contentOverflowStore_(contentOverflowStore),
      pageFn_(pageFn),
      pageContext_(pageContext),
      traversalAccess_(traversalAccess),
      traversalContext_(traversalContext),
      pageWorkspace_(pageWorkspace),
      maxPages_(maxPages) {}

PdfStatus PdfPageTreeWalker::begin(const PdfObjectReference rootPages) {
  if (!traversalStore_.valid() || traversalStore_.recordSize != sizeof(PdfPageTreeRecord) ||
      traversalStore_.capacity == 0 || pageFn_ == nullptr || maxPages_ == 0 || maxPages_ > PdfLimits::MaxPages ||
      rootPages.objectNumber == 0 || pageWorkspace_ == nullptr || traversalAccess_ == nullptr) {
    phase_ = Phase::Failed;
    return PdfStatus::failure(PdfError::InvalidArgument, rootPages.objectNumber);
  }
  recordCount_ = 0;
  stackTop_ = UINT32_MAX;
  currentOrdinal_ = UINT32_MAX;
  pageCount_ = 0;
  declaredRootCount_ = 0;
  hasDeclaredRootCount_ = false;
  pageCaptured_ = false;
  traversalOpen_ = false;
  current_ = {};
  recordScratch_ = {};
  recordScratch_.reference = rootPages;
  rootPages_ = rootPages;
  childReference_ = {};
  processingStackTop_ = UINT32_MAX;
  ancestorOrdinal_ = UINT32_MAX;
  overflowAnnotationRecordCount_ = 0;
  overflowContentRecordCount_ = 0;
  kidsValueIndex_ = PDF_INVALID_INDEX;
  contentsValueIndex_ = PDF_INVALID_INDEX;
  annotationsValueIndex_ = PDF_INVALID_INDEX;
  kidsRemaining_ = 0;
  contentOrdinal_ = 0;
  annotationOrdinal_ = 0;
  ancestorVisited_ = 0;
  processStage_ = ProcessStage::Idle;
  phase_ = Phase::Initialize;
  return PdfStatus::success();
}

PdfStepResult PdfPageTreeWalker::step(PdfWorkBudget& budget) {
  if (phase_ == Phase::Done) {
    return PdfStepResult::completed();
  }
  if (phase_ == Phase::Idle || phase_ == Phase::Failed) {
    return PdfStepResult::failure(PdfStatus::failure(PdfError::Malformed, currentOrdinal_));
  }

  auto fail = [this](const PdfStatus status) {
    phase_ = Phase::Failed;
    return PdfStepResult::failure(status);
  };

  while (true) {
    if (phase_ == Phase::Initialize) {
      constexpr uint32_t kInitializeOperations = 5;
      if (budget.cancelRequested()) {
        return fail(PdfStatus::failure(PdfError::Cancelled, rootPages_.objectNumber));
      }
      if (budget.stopRequested() || budget.operationsRemaining < kInitializeOperations ||
          budget.bytesRemaining < sizeof(PdfPageTreeRecord)) {
        return PdfStepResult::paused();
      }
      budget.operationsRemaining -= kInitializeOperations;
      budget.bytesRemaining -= sizeof(PdfPageTreeRecord);
      resolver_.invalidateSourceAccess();
      PdfStatus status = traversalAccess_(traversalContext_, true);
      if (status) {
        traversalOpen_ = true;
        status = pdfWriteRecord(traversalStore_, 0, &recordScratch_);
      }
      const PdfStatus closeStatus = traversalOpen_ ? traversalAccess_(traversalContext_, false) : PdfStatus::success();
      traversalOpen_ = false;
      if (status && !closeStatus) {
        status = closeStatus;
      }
      if (!status) {
        return fail(status);
      }
      recordCount_ = 1;
      currentOrdinal_ = 0;
      current_ = recordScratch_;
      stackTop_ = UINT32_MAX;
      const PdfStatus beginStatus = resolver_.begin(current_.reference);
      if (!beginStatus.ok()) {
        return fail(beginStatus);
      }
      phase_ = Phase::Resolving;
      // The root record is already resident after initialization. Consume it
      // directly instead of reopening the traversal spool to read it back.
      return PdfStepResult::paused();
    }

    if (phase_ == Phase::SelectNextNode) {
      if (!traversalOpen_) {
        return fail(PdfStatus::failure(PdfError::InvalidArgument, currentOrdinal_));
      }
      if (stackTop_ == UINT32_MAX) {
        const PdfStepResult closed = stepTraversalAccess(false, 1, budget);
        if (!closed.complete()) {
          return closed.failed() ? fail(closed.status) : closed;
        }
        if (hasDeclaredRootCount_ && declaredRootCount_ != pageCount_) {
          return fail(PdfStatus::failure(PdfError::Malformed, pageCount_));
        }
        phase_ = Phase::Done;
        pageCaptured_ = false;
        return PdfStepResult::completed();
      }
      constexpr uint32_t kSelectNodeOperations = 2;
      if (budget.cancelRequested()) {
        return fail(PdfStatus::failure(PdfError::Cancelled, stackTop_));
      }
      if (budget.stopRequested() || budget.operationsRemaining < kSelectNodeOperations ||
          budget.bytesRemaining < sizeof(PdfPageTreeRecord)) {
        return PdfStepResult::paused();
      }
      budget.operationsRemaining -= kSelectNodeOperations;
      budget.bytesRemaining -= sizeof(PdfPageTreeRecord);
      currentOrdinal_ = stackTop_;
      PdfStatus readStatus = pdfReadRecord(traversalStore_, currentOrdinal_, &current_);
      const PdfStatus closeStatus = traversalAccess_(traversalContext_, false);
      if (closeStatus) {
        traversalOpen_ = false;
      }
      if (readStatus && !closeStatus) {
        readStatus = closeStatus;
      }
      if (!readStatus.ok()) {
        return fail(readStatus);
      }
      stackTop_ = current_.nextStackOrdinal;
      const PdfStatus beginStatus = resolver_.begin(current_.reference);
      if (!beginStatus.ok()) {
        return fail(beginStatus);
      }
      phase_ = Phase::Resolving;
      processStage_ = ProcessStage::Idle;
      pageCaptured_ = false;
      // Select the next node before releasing the traversal reader. Xref/source
      // resolution still starts on the next public slice, preserving the
      // single-reader handoff boundary without reopening the traversal spool.
      return PdfStepResult::paused();
    }

    if (phase_ == Phase::Resolving) {
      const PdfStepResult resolveResult = resolver_.step(budget);
      if (!resolveResult.complete()) {
        if (resolveResult.failed()) {
          phase_ = Phase::Failed;
        }
        return resolveResult;
      }
      processStage_ = ProcessStage::Begin;
      phase_ = Phase::OpenTraversal;
      // Resolving can end with a source/xref switch. Defer traversal ownership
      // to the next slice so that another multi-operation reader handoff cannot
      // push the current call beyond its elapsed-time bound.
      return PdfStepResult::paused();
    }

    if (phase_ == Phase::OpenTraversal) {
      const PdfStepResult opened = stepTraversalAccess(true, 3, budget);
      if (!opened.complete()) {
        return opened.failed() ? fail(opened.status) : opened;
      }
      phase_ = Phase::Processing;
      continue;
    }

    if (phase_ == Phase::Processing) {
      const PdfStepResult processed = processResolvedNode(budget);
      if (!processed.complete()) {
        return processed.failed() ? fail(processed.status) : processed;
      }
      phase_ = Phase::SelectNextNode;
      continue;
    }

    return fail(PdfStatus::failure(PdfError::Malformed, currentOrdinal_));
  }
}

PdfStepResult PdfPageTreeWalker::processResolvedNode(PdfWorkBudget& budget) {
  auto fail = [](const PdfStatus status) { return PdfStepResult::failure(status); };

  if (processStage_ == ProcessStage::Begin) {
    const PdfResolvedObject& resolved = resolver_.result();
    if (resolved.rootIndex == PDF_INVALID_INDEX || resolved.rootIndex >= arena_.valueCount ||
        arena_.values[resolved.rootIndex].kind != PdfValueKind::Dictionary) {
      return fail(PdfStatus::failure(PdfError::Malformed, current_.reference.objectNumber));
    }

    uint16_t valueIndex = PDF_INVALID_INDEX;
    if (!pdfDictionaryFind(arena_, resolved.rootIndex, "Type", &valueIndex) || valueIndex >= arena_.valueCount) {
      return fail(PdfStatus::failure(PdfError::Malformed, current_.reference.objectNumber));
    }
    const PdfValue& type = arena_.values[valueIndex];

    if (pdfDictionaryFind(arena_, resolved.rootIndex, "MediaBox", &valueIndex)) {
      const PdfStatus status = parsePageBox(arena_, valueIndex, &current_.mediaBox);
      if (!status) {
        return fail(status);
      }
      current_.hasMediaBox = true;
    }
    if (pdfDictionaryFind(arena_, resolved.rootIndex, "CropBox", &valueIndex)) {
      const PdfStatus status = parsePageBox(arena_, valueIndex, &current_.cropBox);
      if (!status) {
        return fail(status);
      }
      current_.hasCropBox = true;
    }
    if (pdfDictionaryFind(arena_, resolved.rootIndex, "Rotate", &valueIndex)) {
      const PdfStatus status = parsePageRotation(arena_, valueIndex, &current_.rotation);
      if (!status) {
        return fail(status);
      }
    }
    if (pdfDictionaryFind(arena_, resolved.rootIndex, "Resources", &valueIndex)) {
      if (valueIndex >= arena_.valueCount) {
        return fail(PdfStatus::failure(PdfError::Malformed, current_.reference.objectNumber));
      }
      const PdfValue& resources = arena_.values[valueIndex];
      current_.hasResources = true;
      current_.resourceOwner = current_.reference;
      if (resources.kind == PdfValueKind::Reference) {
        current_.resourcesIndirect = true;
        current_.resourceReference = {resources.objectNumber, resources.generation};
      } else if (resources.kind == PdfValueKind::Dictionary) {
        current_.resourcesIndirect = false;
        current_.resourceReference = {};
      } else {
        return fail(PdfStatus::failure(PdfError::Malformed, current_.reference.objectNumber));
      }
    }

    if (valueIsName(arena_, type, "Pages")) {
      if (current_.depth >= PdfLimits::MaxPageTreeDepth) {
        return fail(PdfStatus::failure(PdfError::LimitExceeded, current_.reference.objectNumber));
      }
      if (!pdfDictionaryFind(arena_, resolved.rootIndex, "Count", &valueIndex) ||
          valueIndex >= arena_.valueCount) {
        return fail(PdfStatus::failure(PdfError::Malformed, current_.reference.objectNumber));
      }
      const PdfValue& count = arena_.values[valueIndex];
      if (count.kind != PdfValueKind::Integer || count.integerValue < 0 ||
          static_cast<uint64_t>(count.integerValue) > maxPages_) {
        return fail(PdfStatus::failure(PdfError::LimitExceeded, current_.reference.objectNumber));
      }
      if (currentOrdinal_ == 0) {
        declaredRootCount_ = static_cast<uint32_t>(count.integerValue);
        hasDeclaredRootCount_ = true;
      }
      if (!pdfDictionaryFind(arena_, resolved.rootIndex, "Kids", &valueIndex) || valueIndex >= arena_.valueCount ||
          arena_.values[valueIndex].kind != PdfValueKind::Array) {
        return fail(PdfStatus::failure(PdfError::Malformed, current_.reference.objectNumber));
      }
      kidsValueIndex_ = valueIndex;
      kidsRemaining_ = arena_.values[valueIndex].count;
      processingStackTop_ = stackTop_;
      processStage_ = kidsRemaining_ == 0 ? ProcessStage::Complete : ProcessStage::LoadChild;
    } else if (valueIsName(arena_, type, "Page")) {
      if (pageCount_ >= maxPages_) {
        return fail(PdfStatus::failure(PdfError::LimitExceeded, pageCount_));
      }

      PdfPageInfo& page = *pageWorkspace_;
      page.~PdfPageInfo();
      new (&page) PdfPageInfo();
      page.pageReference = current_.reference;
      page.pageIndex = pageCount_;
      page.hasResources = current_.hasResources;
      page.resourcesIndirect = current_.resourcesIndirect;
      page.resourceOwner = current_.resourceOwner;
      page.resourceReference = current_.resourceReference;
      page.rotation = current_.rotation;
      const PdfRectangle viewBox = effectivePageBox(current_);
      page.viewXMin = viewBox.xMin;
      page.viewYMin = viewBox.yMin;
      page.contentOverflowStart = overflowContentRecordCount_;
      const PdfStatus geometryStatus = orientedPageExtent(current_, &page.pageWidth, &page.pageHeight);
      if (!geometryStatus) {
        return fail(geometryStatus);
      }

      contentsValueIndex_ = PDF_INVALID_INDEX;
      ProcessStage contentStage = ProcessStage::BeginAnnotations;
      if (pdfDictionaryFind(arena_, resolved.rootIndex, "Contents", &valueIndex)) {
        if (valueIndex >= arena_.valueCount) {
          return fail(PdfStatus::failure(PdfError::Malformed, current_.reference.objectNumber));
        }
        const PdfValue& contents = arena_.values[valueIndex];
        if (contents.kind == PdfValueKind::Reference) {
          page.contents[0] = {contents.objectNumber, contents.generation};
          page.contentCount = 1;
        } else if (contents.kind == PdfValueKind::Array) {
          contentsValueIndex_ = valueIndex;
          contentOrdinal_ = 0;
          contentStage = contents.count == 0 ? ProcessStage::BeginAnnotations : ProcessStage::LoadContent;
        } else {
          return fail(PdfStatus::failure(PdfError::Malformed, current_.reference.objectNumber));
        }
      }

      if (pdfDictionaryFind(arena_, resolved.rootIndex, "Annots", &valueIndex)) {
        if (valueIndex >= arena_.valueCount) {
          return fail(PdfStatus::failure(PdfError::Malformed, current_.reference.objectNumber));
        }
        if (arena_.values[valueIndex].kind == PdfValueKind::Array) {
          annotationsValueIndex_ = valueIndex;
        } else if (arena_.values[valueIndex].kind == PdfValueKind::Reference) {
          annotationsValueIndex_ = PDF_INVALID_INDEX;
        } else {
          return fail(PdfStatus::failure(PdfError::Malformed, current_.reference.objectNumber));
        }
      } else {
        annotationsValueIndex_ = PDF_INVALID_INDEX;
      }
      annotationOrdinal_ = 0;
      processStage_ = contentStage;
    } else {
      return fail(PdfStatus::failure(PdfError::Malformed, current_.reference.objectNumber));
    }
  }

  while (true) {
    if (processStage_ == ProcessStage::LoadChild) {
      if (kidsRemaining_ == 0) {
        stackTop_ = processingStackTop_;
        processStage_ = ProcessStage::Complete;
        continue;
      }
      uint16_t childIndex = PDF_INVALID_INDEX;
      if (!pdfArrayAt(arena_, kidsValueIndex_, --kidsRemaining_, &childIndex) || childIndex >= arena_.valueCount) {
        return fail(PdfStatus::failure(PdfError::Malformed, current_.reference.objectNumber));
      }
      const PdfValue& child = arena_.values[childIndex];
      if (child.kind != PdfValueKind::Reference || child.objectNumber == 0) {
        return fail(PdfStatus::failure(PdfError::Malformed, current_.reference.objectNumber));
      }
      childReference_ = {child.objectNumber, child.generation};
      if (childReference_ == current_.reference) {
        return fail(PdfStatus::failure(PdfError::Malformed, childReference_.objectNumber));
      }
      ancestorOrdinal_ = current_.parentOrdinal;
      ancestorVisited_ = 0;
      processStage_ = ProcessStage::CheckAncestor;
      continue;
    }

    if (processStage_ == ProcessStage::CheckAncestor) {
      if (ancestorOrdinal_ == UINT32_MAX) {
        processStage_ = ProcessStage::WriteChild;
        continue;
      }
      if (ancestorOrdinal_ >= recordCount_ || ancestorVisited_ >= PdfLimits::MaxPageTreeDepth) {
        return fail(PdfStatus::failure(PdfError::Malformed, childReference_.objectNumber));
      }
      const PdfStepResult read = stepTraversalRecord(false, ancestorOrdinal_, &recordScratch_, budget);
      if (!read.complete()) {
        return read;
      }
      ++ancestorVisited_;
      if (recordScratch_.reference == childReference_) {
        return fail(PdfStatus::failure(PdfError::Malformed, childReference_.objectNumber));
      }
      ancestorOrdinal_ = recordScratch_.parentOrdinal;
      continue;
    }

    if (processStage_ == ProcessStage::WriteChild) {
      if (recordCount_ >= traversalStore_.capacity) {
        return fail(PdfStatus::failure(PdfError::LimitExceeded, childReference_.objectNumber));
      }
      recordScratch_ = {};
      recordScratch_.reference = childReference_;
      recordScratch_.resourceOwner = current_.resourceOwner;
      recordScratch_.resourceReference = current_.resourceReference;
      recordScratch_.mediaBox = current_.mediaBox;
      recordScratch_.cropBox = current_.cropBox;
      recordScratch_.parentOrdinal = currentOrdinal_;
      recordScratch_.nextStackOrdinal = processingStackTop_;
      recordScratch_.depth = static_cast<uint16_t>(current_.depth + 1U);
      recordScratch_.rotation = current_.rotation;
      recordScratch_.hasResources = current_.hasResources;
      recordScratch_.resourcesIndirect = current_.resourcesIndirect;
      recordScratch_.hasMediaBox = current_.hasMediaBox;
      recordScratch_.hasCropBox = current_.hasCropBox;
      const PdfStepResult write = stepTraversalRecord(true, recordCount_, &recordScratch_, budget);
      if (!write.complete()) {
        return write;
      }
      processingStackTop_ = recordCount_++;
      processStage_ = ProcessStage::LoadChild;
      // Mutable record writes perform a seek plus a write. Yield after one
      // child so a slow-card slice cannot start another two-operation write
      // after already spending time acquiring traversal ownership.
      return PdfStepResult::paused();
    }

    if (processStage_ == ProcessStage::LoadAnnotation) {
      if (annotationsValueIndex_ >= arena_.valueCount ||
          arena_.values[annotationsValueIndex_].kind != PdfValueKind::Array) {
        return fail(PdfStatus::failure(PdfError::Malformed, current_.reference.objectNumber));
      }
      const uint16_t annotationCount = arena_.values[annotationsValueIndex_].count;
      if (annotationOrdinal_ >= annotationCount) {
        processStage_ = ProcessStage::EmitPage;
        continue;
      }
      if (budget.cancelRequested()) {
        return fail(PdfStatus::failure(PdfError::Cancelled, current_.reference.objectNumber));
      }
      if (budget.stopRequested() || budget.operationsRemaining == 0 ||
          budget.bytesRemaining < sizeof(PdfObjectReference)) {
        return PdfStepResult::paused();
      }
      --budget.operationsRemaining;
      budget.bytesRemaining -= sizeof(PdfObjectReference);
      uint16_t annotationIndex = PDF_INVALID_INDEX;
      if (!pdfArrayAt(arena_, annotationsValueIndex_, annotationOrdinal_++, &annotationIndex) ||
          annotationIndex >= arena_.valueCount) {
        return fail(PdfStatus::failure(PdfError::Malformed, current_.reference.objectNumber));
      }
      const PdfValue& annotation = arena_.values[annotationIndex];
      if (annotation.kind != PdfValueKind::Reference) {
        continue;
      }
      PdfPageInfo& page = *pageWorkspace_;
      const PdfObjectReference reference{annotation.objectNumber, annotation.generation};
      if (page.annotationCount < PdfLimits::MaxLinkAnnotationsPerPage) {
        page.annotations[page.annotationCount++] = reference;
        continue;
      }
      if (!annotationOverflowStore_.valid() || page.overflowAnnotationCount == UINT16_MAX ||
          overflowAnnotationRecordCount_ >= annotationOverflowStore_.capacity) {
        return fail(PdfStatus::failure(PdfError::InsufficientStorage, overflowAnnotationRecordCount_));
      }
      const PdfStatus status =
          pdfWriteRecord(annotationOverflowStore_, overflowAnnotationRecordCount_, &reference);
      if (!status) {
        return fail(status);
      }
      ++overflowAnnotationRecordCount_;
      ++page.overflowAnnotationCount;
      // At most one SD write is issued in a public slice.
      return PdfStepResult::paused();
    }

    if (processStage_ == ProcessStage::EmitPage) {
      if (budget.cancelRequested()) {
        return fail(PdfStatus::failure(PdfError::Cancelled, current_.reference.objectNumber));
      }
      if (budget.stopRequested() || budget.operationsRemaining == 0 ||
          budget.bytesRemaining < sizeof(PdfPageInfo)) {
        return PdfStepResult::paused();
      }
      --budget.operationsRemaining;
      budget.bytesRemaining -= sizeof(PdfPageInfo);
      const PdfStatus callbackStatus = pageFn_(pageContext_, *pageWorkspace_);
      if (!callbackStatus) {
        return fail(callbackStatus);
      }
      ++pageCount_;
      pageCaptured_ = true;
      processStage_ = ProcessStage::Complete;
      continue;
    }

    if (processStage_ == ProcessStage::LoadContent) {
      if (contentsValueIndex_ >= arena_.valueCount ||
          arena_.values[contentsValueIndex_].kind != PdfValueKind::Array) {
        return fail(PdfStatus::failure(PdfError::Malformed, current_.reference.objectNumber));
      }
      const uint16_t contentCount = arena_.values[contentsValueIndex_].count;
      if (contentOrdinal_ >= contentCount) {
        processStage_ = ProcessStage::BeginAnnotations;
        continue;
      }
      const uint16_t ordinal = contentOrdinal_;
      uint16_t contentIndex = PDF_INVALID_INDEX;
      if (!pdfArrayAt(arena_, contentsValueIndex_, ordinal, &contentIndex) ||
          contentIndex >= arena_.valueCount) {
        return fail(PdfStatus::failure(PdfError::Malformed, current_.reference.objectNumber));
      }
      const PdfValue& content = arena_.values[contentIndex];
      if (content.kind != PdfValueKind::Reference) {
        return fail(PdfStatus::failure(PdfError::Malformed, current_.reference.objectNumber));
      }
      PdfPageInfo& page = *pageWorkspace_;
      const PdfObjectReference reference{content.objectNumber, content.generation};
      if (ordinal < PdfLimits::MaxContentStreamsPerPage) {
        page.contents[page.contentCount++] = reference;
        ++contentOrdinal_;
        continue;
      }
      constexpr uint16_t kMaximumOverflowContentCount =
          UINT16_MAX - PdfLimits::MaxContentStreamsPerPage;
      if (!contentOverflowStore_.valid() ||
          contentOverflowStore_.recordSize != sizeof(PdfPageContentOverflowRecord) ||
          page.overflowContentCount >= kMaximumOverflowContentCount ||
          overflowContentRecordCount_ >= contentOverflowStore_.capacity) {
        return fail(PdfStatus::failure(PdfError::InsufficientStorage, overflowContentRecordCount_));
      }
      if (budget.cancelRequested()) {
        return fail(PdfStatus::failure(PdfError::Cancelled, ordinal));
      }
      if (budget.stopRequested() || budget.operationsRemaining == 0 ||
          budget.bytesRemaining < sizeof(PdfPageContentOverflowRecord)) {
        return PdfStepResult::paused();
      }
      --budget.operationsRemaining;
      budget.bytesRemaining -= sizeof(PdfPageContentOverflowRecord);
      const PdfPageContentOverflowRecord record{page.pageIndex, ordinal, 0, reference};
      const PdfStatus status = pdfWriteRecord(contentOverflowStore_, overflowContentRecordCount_, &record);
      if (!status) {
        return fail(status);
      }
      ++contentOrdinal_;
      ++overflowContentRecordCount_;
      ++page.overflowContentCount;
      // At most one overflow-spool write is issued in a public slice.
      return PdfStepResult::paused();
    }

    if (processStage_ == ProcessStage::BeginAnnotations) {
      processStage_ =
          annotationsValueIndex_ < arena_.valueCount &&
                  arena_.values[annotationsValueIndex_].kind == PdfValueKind::Array &&
                  arena_.values[annotationsValueIndex_].count != 0
              ? ProcessStage::LoadAnnotation
              : ProcessStage::EmitPage;
      continue;
    }

    if (processStage_ == ProcessStage::Complete) {
      return PdfStepResult::completed();
    }
    return fail(PdfStatus::failure(PdfError::InvalidArgument, current_.reference.objectNumber));
  }
}

PdfStepResult PdfPageTreeWalker::stepTraversalRecord(const bool write, const uint32_t ordinal,
                                                     PdfPageTreeRecord* const record, PdfWorkBudget& budget) {
  if (record == nullptr || !traversalOpen_) {
    return PdfStepResult::failure(PdfStatus::failure(PdfError::InvalidArgument, ordinal));
  }
  if (budget.cancelRequested()) {
    return PdfStepResult::failure(PdfStatus::failure(PdfError::Cancelled, ordinal));
  }
  const uint32_t requiredOperations = write ? 2U : 1U;
  if (budget.stopRequested() || budget.operationsRemaining < requiredOperations ||
      budget.bytesRemaining < sizeof(PdfPageTreeRecord)) {
    return PdfStepResult::paused();
  }
  budget.operationsRemaining -= requiredOperations;
  budget.bytesRemaining -= sizeof(PdfPageTreeRecord);
  const PdfStatus status =
      write ? pdfWriteRecord(traversalStore_, ordinal, record) : pdfReadRecord(traversalStore_, ordinal, record);
  return status ? PdfStepResult::completed() : PdfStepResult::failure(status);
}

PdfStepResult PdfPageTreeWalker::stepTraversalAccess(const bool required, const uint32_t reservedOperations,
                                                     PdfWorkBudget& budget) {
  if (traversalOpen_ == required) {
    return PdfStepResult::completed();
  }
  if (budget.cancelRequested()) {
    return PdfStepResult::failure(PdfStatus::failure(PdfError::Cancelled, currentOrdinal_));
  }
  if (budget.stopRequested() || budget.operationsRemaining < reservedOperations) {
    return PdfStepResult::paused();
  }
  budget.operationsRemaining -= reservedOperations;
  if (required) {
    resolver_.invalidateSourceAccess();
  }
  const PdfStatus status = traversalAccess_(traversalContext_, required);
  if (status) {
    traversalOpen_ = required;
  }
  return status ? PdfStepResult::completed() : PdfStepResult::failure(status);
}
