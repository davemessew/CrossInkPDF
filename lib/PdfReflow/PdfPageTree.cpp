#include "PdfPageTree.h"

#include <cstring>

namespace {

bool valueIsName(const PdfObjectArena& arena, const PdfValue& value, const char* expected) {
  return value.kind == PdfValueKind::Name && pdfTextEquals(arena, value, expected);
}

}  // namespace

PdfPageTreeWalker::PdfPageTreeWalker(PdfObjectResolver& resolver, PdfObjectArena& arena,
                                     const PdfFixedRecordStore traversalStore, const PageFn pageFn, void* pageContext,
                                     const uint32_t maxPages)
    : resolver_(resolver),
      arena_(arena),
      traversalStore_(traversalStore),
      pageFn_(pageFn),
      pageContext_(pageContext),
      maxPages_(maxPages) {}

PdfStatus PdfPageTreeWalker::begin(const PdfObjectReference rootPages) {
  if (!traversalStore_.valid() || traversalStore_.recordSize != sizeof(PdfPageTreeRecord) ||
      traversalStore_.capacity == 0 || pageFn_ == nullptr || maxPages_ == 0 || maxPages_ > PdfLimits::MaxPages ||
      rootPages.objectNumber == 0) {
    phase_ = Phase::Failed;
    return PdfStatus::failure(PdfError::InvalidArgument, rootPages.objectNumber);
  }
  recordCount_ = 0;
  stackTop_ = UINT32_MAX;
  currentOrdinal_ = UINT32_MAX;
  pageCount_ = 0;
  declaredRootCount_ = 0;
  hasDeclaredRootCount_ = false;
  current_ = {};

  PdfPageTreeRecord root;
  root.reference = rootPages;
  const PdfStatus status = pdfWriteRecord(traversalStore_, 0, &root);
  if (!status.ok()) {
    phase_ = Phase::Failed;
    return status;
  }
  recordCount_ = 1;
  stackTop_ = 0;
  phase_ = Phase::NeedNode;
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
    if (phase_ == Phase::NeedNode) {
      if (stackTop_ == UINT32_MAX) {
        if (hasDeclaredRootCount_ && declaredRootCount_ != pageCount_) {
          return fail(PdfStatus::failure(PdfError::Malformed, pageCount_));
        }
        phase_ = Phase::Done;
        return PdfStepResult::completed();
      }
      currentOrdinal_ = stackTop_;
      const PdfStatus readStatus = pdfReadRecord(traversalStore_, currentOrdinal_, &current_);
      if (!readStatus.ok()) {
        return fail(readStatus);
      }
      stackTop_ = current_.nextStackOrdinal;
      const PdfStatus beginStatus = resolver_.begin(current_.reference);
      if (!beginStatus.ok()) {
        return fail(beginStatus);
      }
      phase_ = Phase::Resolving;
    }

    const PdfStepResult resolveResult = resolver_.step(budget);
    if (!resolveResult.complete()) {
      if (resolveResult.failed()) {
        phase_ = Phase::Failed;
      }
      return resolveResult;
    }
    const PdfStatus processStatus = processResolvedNode();
    if (!processStatus.ok()) {
      return fail(processStatus);
    }
    phase_ = Phase::NeedNode;
  }
}

PdfStatus PdfPageTreeWalker::processResolvedNode() {
  const PdfResolvedObject& resolved = resolver_.result();
  if (resolved.rootIndex == PDF_INVALID_INDEX || resolved.rootIndex >= arena_.valueCount) {
    return PdfStatus::failure(PdfError::Malformed, current_.reference.objectNumber);
  }
  const PdfValue& dictionary = arena_.values[resolved.rootIndex];
  if (dictionary.kind != PdfValueKind::Dictionary) {
    return PdfStatus::failure(PdfError::Malformed, current_.reference.objectNumber);
  }

  uint16_t valueIndex = PDF_INVALID_INDEX;
  if (!pdfDictionaryFind(arena_, resolved.rootIndex, "Type", &valueIndex) || valueIndex >= arena_.valueCount) {
    return PdfStatus::failure(PdfError::Malformed, current_.reference.objectNumber);
  }
  const PdfValue& type = arena_.values[valueIndex];

  PdfPageTreeRecord inherited = current_;
  if (pdfDictionaryFind(arena_, resolved.rootIndex, "Resources", &valueIndex)) {
    if (valueIndex >= arena_.valueCount) {
      return PdfStatus::failure(PdfError::Malformed, current_.reference.objectNumber);
    }
    const PdfValue& resources = arena_.values[valueIndex];
    inherited.hasResources = true;
    inherited.resourceOwner = current_.reference;
    if (resources.kind == PdfValueKind::Reference) {
      inherited.resourcesIndirect = true;
      inherited.resourceReference = {resources.objectNumber, resources.generation};
    } else if (resources.kind == PdfValueKind::Dictionary) {
      inherited.resourcesIndirect = false;
      inherited.resourceReference = {};
    } else {
      return PdfStatus::failure(PdfError::Malformed, current_.reference.objectNumber);
    }
  }

  if (valueIsName(arena_, type, "Pages")) {
    if (!pdfDictionaryFind(arena_, resolved.rootIndex, "Count", &valueIndex) || valueIndex >= arena_.valueCount) {
      return PdfStatus::failure(PdfError::Malformed, current_.reference.objectNumber);
    }
    const PdfValue& count = arena_.values[valueIndex];
    if (count.kind != PdfValueKind::Integer || count.integerValue < 0 ||
        static_cast<uint64_t>(count.integerValue) > maxPages_) {
      return PdfStatus::failure(PdfError::LimitExceeded, current_.reference.objectNumber);
    }
    if (currentOrdinal_ == 0) {
      declaredRootCount_ = static_cast<uint32_t>(count.integerValue);
      hasDeclaredRootCount_ = true;
    }
    if (!pdfDictionaryFind(arena_, resolved.rootIndex, "Kids", &valueIndex) || valueIndex >= arena_.valueCount ||
        arena_.values[valueIndex].kind != PdfValueKind::Array) {
      return PdfStatus::failure(PdfError::Malformed, current_.reference.objectNumber);
    }
    const PdfValue& kids = arena_.values[valueIndex];
    uint32_t firstChild = UINT32_MAX;
    uint32_t lastChild = UINT32_MAX;
    for (uint16_t ordinal = 0; ordinal < kids.count; ++ordinal) {
      uint16_t childIndex = PDF_INVALID_INDEX;
      if (!pdfArrayAt(arena_, valueIndex, ordinal, &childIndex) || childIndex >= arena_.valueCount) {
        return PdfStatus::failure(PdfError::Malformed, current_.reference.objectNumber);
      }
      const PdfValue& child = arena_.values[childIndex];
      if (child.kind != PdfValueKind::Reference) {
        return PdfStatus::failure(PdfError::Malformed, current_.reference.objectNumber);
      }
      const PdfStatus appendStatus =
          appendChild({child.objectNumber, child.generation}, inherited, currentOrdinal_, &firstChild, &lastChild);
      if (!appendStatus.ok()) {
        return appendStatus;
      }
    }
    if (firstChild != UINT32_MAX) {
      PdfPageTreeRecord last;
      PdfStatus status = pdfReadRecord(traversalStore_, lastChild, &last);
      if (!status.ok()) {
        return status;
      }
      last.nextStackOrdinal = stackTop_;
      status = pdfWriteRecord(traversalStore_, lastChild, &last);
      if (!status.ok()) {
        return status;
      }
      stackTop_ = firstChild;
    }
    return PdfStatus::success();
  }

  if (!valueIsName(arena_, type, "Page")) {
    return PdfStatus::failure(PdfError::Malformed, current_.reference.objectNumber);
  }
  if (pageCount_ >= maxPages_) {
    return PdfStatus::failure(PdfError::LimitExceeded, pageCount_);
  }

  PdfPageInfo page;
  page.pageReference = current_.reference;
  page.pageIndex = pageCount_;
  page.hasResources = inherited.hasResources;
  page.resourcesIndirect = inherited.resourcesIndirect;
  page.resourceOwner = inherited.resourceOwner;
  page.resourceReference = inherited.resourceReference;

  if (pdfDictionaryFind(arena_, resolved.rootIndex, "Contents", &valueIndex)) {
    if (valueIndex >= arena_.valueCount) {
      return PdfStatus::failure(PdfError::Malformed, current_.reference.objectNumber);
    }
    const PdfValue& contents = arena_.values[valueIndex];
    if (contents.kind == PdfValueKind::Reference) {
      page.contents[0] = {contents.objectNumber, contents.generation};
      page.contentCount = 1;
    } else if (contents.kind == PdfValueKind::Array) {
      if (contents.count > PdfLimits::MaxContentStreamsPerPage) {
        return PdfStatus::failure(PdfError::LimitExceeded, current_.reference.objectNumber);
      }
      for (uint16_t ordinal = 0; ordinal < contents.count; ++ordinal) {
        uint16_t contentIndex = PDF_INVALID_INDEX;
        if (!pdfArrayAt(arena_, valueIndex, ordinal, &contentIndex) || contentIndex >= arena_.valueCount) {
          return PdfStatus::failure(PdfError::Malformed, current_.reference.objectNumber);
        }
        const PdfValue& content = arena_.values[contentIndex];
        if (content.kind != PdfValueKind::Reference) {
          return PdfStatus::failure(PdfError::Malformed, current_.reference.objectNumber);
        }
        page.contents[page.contentCount++] = {content.objectNumber, content.generation};
      }
    } else {
      return PdfStatus::failure(PdfError::Malformed, current_.reference.objectNumber);
    }
  }

  if (pdfDictionaryFind(arena_, resolved.rootIndex, "Annots", &valueIndex)) {
    if (valueIndex >= arena_.valueCount || arena_.values[valueIndex].kind != PdfValueKind::Array ||
        arena_.values[valueIndex].count > PdfLimits::MaxLinkAnnotationsPerPage) {
      return PdfStatus::failure(PdfError::LimitExceeded, current_.reference.objectNumber);
    }
    for (uint16_t ordinal = 0; ordinal < arena_.values[valueIndex].count; ++ordinal) {
      uint16_t annotationIndex = PDF_INVALID_INDEX;
      if (!pdfArrayAt(arena_, valueIndex, ordinal, &annotationIndex) || annotationIndex >= arena_.valueCount) {
        return PdfStatus::failure(PdfError::Malformed, current_.reference.objectNumber);
      }
      const PdfValue& annotation = arena_.values[annotationIndex];
      if (annotation.kind == PdfValueKind::Reference) {
        page.annotations[page.annotationCount++] = {annotation.objectNumber, annotation.generation};
      }
    }
  }

  const PdfStatus callbackStatus = pageFn_(pageContext_, page);
  if (!callbackStatus.ok()) {
    return callbackStatus;
  }
  ++pageCount_;
  return PdfStatus::success();
}

PdfStatus PdfPageTreeWalker::appendChild(const PdfObjectReference reference, const PdfPageTreeRecord& parent,
                                         const uint32_t parentOrdinal, uint32_t* firstChild, uint32_t* lastChild) {
  if (firstChild == nullptr || lastChild == nullptr || reference.objectNumber == 0) {
    return PdfStatus::failure(PdfError::Malformed, reference.objectNumber);
  }
  if (parent.depth >= PdfLimits::MaxPageTreeDepth) {
    return PdfStatus::failure(PdfError::LimitExceeded, reference.objectNumber);
  }
  PdfStatus status = checkAncestorCycle(reference, parentOrdinal);
  if (!status.ok()) {
    return status;
  }
  if (recordCount_ >= traversalStore_.capacity) {
    return PdfStatus::failure(PdfError::LimitExceeded, reference.objectNumber);
  }

  const uint32_t childOrdinal = recordCount_;
  PdfPageTreeRecord child;
  child.reference = reference;
  child.resourceOwner = parent.resourceOwner;
  child.resourceReference = parent.resourceReference;
  child.parentOrdinal = parentOrdinal;
  child.depth = static_cast<uint16_t>(parent.depth + 1);
  child.hasResources = parent.hasResources;
  child.resourcesIndirect = parent.resourcesIndirect;
  status = pdfWriteRecord(traversalStore_, childOrdinal, &child);
  if (!status.ok()) {
    return status;
  }
  ++recordCount_;

  if (*firstChild == UINT32_MAX) {
    *firstChild = childOrdinal;
  } else {
    PdfPageTreeRecord previous;
    status = pdfReadRecord(traversalStore_, *lastChild, &previous);
    if (!status.ok()) {
      return status;
    }
    previous.nextStackOrdinal = childOrdinal;
    status = pdfWriteRecord(traversalStore_, *lastChild, &previous);
    if (!status.ok()) {
      return status;
    }
  }
  *lastChild = childOrdinal;
  return PdfStatus::success();
}

PdfStatus PdfPageTreeWalker::checkAncestorCycle(const PdfObjectReference reference, uint32_t parentOrdinal) const {
  uint16_t visited = 0;
  while (parentOrdinal != UINT32_MAX) {
    if (parentOrdinal >= recordCount_ || visited++ > PdfLimits::MaxPageTreeDepth) {
      return PdfStatus::failure(PdfError::Malformed, reference.objectNumber);
    }
    PdfPageTreeRecord ancestor;
    const PdfStatus status = pdfReadRecord(traversalStore_, parentOrdinal, &ancestor);
    if (!status.ok()) {
      return status;
    }
    if (ancestor.reference == reference) {
      return PdfStatus::failure(PdfError::Malformed, reference.objectNumber);
    }
    parentOrdinal = ancestor.parentOrdinal;
  }
  return PdfStatus::success();
}
