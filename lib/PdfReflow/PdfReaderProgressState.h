#pragma once

#include <ReflowDocument.h>

#include <cstdint>
#include <cstring>

enum class PdfDeferredRestoreAction : uint8_t {
  RetryExact,
  KeepSafePosition,
};

inline bool pdfPopulateReadingPositionFromRange(const ReflowPageSemanticRange& range,
                                                ReflowReadingPosition* const position) {
  if (position == nullptr) {
    return false;
  }
  if (range.valid) {
    size_t anchorLength = 0;
    while (anchorLength < sizeof(range.blockAnchor) && range.blockAnchor[anchorLength] != '\0') {
      ++anchorLength;
    }
    if (anchorLength == sizeof(range.blockAnchor)) {
      return false;
    }
  }

  position->hasWordCursor = true;
  position->wordCursor = range.wordCursor;
  position->hasSemanticPosition = range.valid;
  position->globalWordOrdinal = range.valid ? range.firstGlobalWordOrdinal : 0;
  position->blockWordOffset = range.valid ? range.firstBlockWordOffset : 0;
  std::memset(position->blockAnchor, 0, sizeof(position->blockAnchor));
  if (range.valid) {
    std::memcpy(position->blockAnchor, range.blockAnchor, sizeof(position->blockAnchor));
  }
  return true;
}

inline bool pdfReadingPositionsEqualExact(const ReflowReadingPosition& left, const ReflowReadingPosition& right) {
  return left.sectionIndex == right.sectionIndex && left.pageNumber == right.pageNumber &&
         left.pageCount == right.pageCount && left.hasPageCount == right.hasPageCount &&
         left.hasSemanticPosition == right.hasSemanticPosition && left.hasWordCursor == right.hasWordCursor &&
         left.globalWordOrdinal == right.globalWordOrdinal && left.blockWordOffset == right.blockWordOffset &&
         left.wordCursor == right.wordCursor &&
         std::memcmp(left.blockAnchor, right.blockAnchor, sizeof(left.blockAnchor)) == 0;
}

struct PdfExactReadingOrigin {
  ReflowReadingPosition position{};
  bool valid = false;

  bool capture(const ReflowPageSemanticRange& range, const int sectionIndex, const int pageNumber,
               const int pageCount) {
    ReflowReadingPosition candidate;
    candidate.sectionIndex = sectionIndex;
    candidate.pageNumber = pageNumber;
    candidate.pageCount = pageCount;
    candidate.hasPageCount = pageCount > 0;
    if (!pdfPopulateReadingPositionFromRange(range, &candidate)) {
      return false;
    }
    position = candidate;
    valid = true;
    return true;
  }
};

static_assert(sizeof(PdfExactReadingOrigin) <= 48, "PDF exact footnote origin exceeded its bounded allocation");

struct PdfReaderProgressState {
  static constexpr uint32_t ProgressWriteIntervalMs = 30000U;
  static constexpr uint8_t MaxDeferredRestoreAttempts = 2;

  bool cachedHasSemanticPosition = false;
  bool cachedHasWordCursor = false;
  uint32_t cachedGlobalWordOrdinal = 0;
  uint32_t cachedBlockWordOffset = 0;
  uint32_t cachedWordCursor = 0;
  char cachedBlockAnchor[REFLOW_SEMANTIC_ANCHOR_BYTES] = {};

  ReflowPageSemanticRange currentPageSemanticRange{};
  int semanticRangeSectionIndex = -1;
  int semanticRangePageNumber = -1;
  bool hasLastKnownWordCursor = false;
  uint32_t lastKnownWordCursor = 0;

  uint32_t lastProgressAttemptMs = 0;
  bool hasProgressAttempt = false;
  int lastPersistedSpine = -1;
  int lastPersistedPage = -1;
  uint8_t deferredRestoreAttempts_ = 0;
  bool exactRestoreUnresolved_ = false;

  bool shouldAttemptSave(const int spine, const int page, const uint32_t now, const bool force) const {
    if (exactRestoreUnresolved_) {
      return false;
    }
    if (force) {
      return true;
    }
    if (lastPersistedSpine == spine && lastPersistedPage == page) {
      return false;
    }
    return !hasProgressAttempt || now - lastProgressAttemptMs >= ProgressWriteIntervalMs;
  }

  void recordSaveAttempt(const int spine, const int page, const uint32_t now, const bool success) {
    lastProgressAttemptMs = now;
    hasProgressAttempt = true;
    if (success) {
      lastPersistedSpine = spine;
      lastPersistedPage = page;
    }
  }

  void seedPersistedPosition(const int spine, const int page) {
    lastPersistedSpine = spine;
    lastPersistedPage = page;
  }

  void markPositionDirty() {
    lastPersistedSpine = -1;
    lastPersistedPage = -1;
  }

  PdfDeferredRestoreAction noteDeferredRestoreFailure() {
    exactRestoreUnresolved_ = true;
    if (deferredRestoreAttempts_ < MaxDeferredRestoreAttempts) {
      ++deferredRestoreAttempts_;
    }
    return deferredRestoreAttempts_ < MaxDeferredRestoreAttempts ? PdfDeferredRestoreAction::RetryExact
                                                                 : PdfDeferredRestoreAction::KeepSafePosition;
  }

  void noteDeferredRestoreSuccess() {
    deferredRestoreAttempts_ = 0;
    exactRestoreUnresolved_ = false;
  }

  bool shouldAttemptExactRestore() const { return deferredRestoreAttempts_ < MaxDeferredRestoreAttempts; }

  bool shouldDeferSaveForRestoreRetry() const { return exactRestoreUnresolved_; }

  uint8_t deferredRestoreAttempts() const { return deferredRestoreAttempts_; }

  void clearCachedExactPosition() {
    cachedHasSemanticPosition = false;
    cachedHasWordCursor = false;
    cachedGlobalWordOrdinal = 0;
    cachedBlockWordOffset = 0;
    cachedWordCursor = 0;
    std::memset(cachedBlockAnchor, 0, sizeof(cachedBlockAnchor));
  }

  void clearCurrentPageSemanticRange() {
    currentPageSemanticRange = {};
    semanticRangeSectionIndex = -1;
    semanticRangePageNumber = -1;
  }

  bool cacheExactPosition(const ReflowReadingPosition& position) {
    if (!position.hasWordCursor) {
      return false;
    }
    if (position.hasSemanticPosition) {
      size_t anchorLength = 0;
      while (anchorLength < sizeof(position.blockAnchor) && position.blockAnchor[anchorLength] != '\0') {
        ++anchorLength;
      }
      if (anchorLength == sizeof(position.blockAnchor)) {
        return false;
      }
    }

    cachedHasSemanticPosition = position.hasSemanticPosition;
    cachedHasWordCursor = true;
    cachedGlobalWordOrdinal = position.hasSemanticPosition ? position.globalWordOrdinal : 0;
    cachedBlockWordOffset = position.hasSemanticPosition ? position.blockWordOffset : 0;
    cachedWordCursor = position.wordCursor;
    std::memset(cachedBlockAnchor, 0, sizeof(cachedBlockAnchor));
    if (position.hasSemanticPosition) {
      std::memcpy(cachedBlockAnchor, position.blockAnchor, sizeof(cachedBlockAnchor));
    }
    noteDeferredRestoreSuccess();
    return true;
  }

  bool cacheExactRange(const ReflowPageSemanticRange& range) {
    ReflowReadingPosition position;
    return pdfPopulateReadingPositionFromRange(range, &position) && cacheExactPosition(position);
  }

  bool cacheRelayoutCapture(const ReflowPageSemanticRange* const matchingCurrentRange,
                            const ReflowPageSemanticRange* const firstSidecarRead,
                            const ReflowPageSemanticRange* const secondSidecarRead) {
    if (exactRestoreUnresolved_) {
      return false;
    }
    return (matchingCurrentRange != nullptr && cacheExactRange(*matchingCurrentRange)) ||
           (firstSidecarRead != nullptr && cacheExactRange(*firstSidecarRead)) ||
           (secondSidecarRead != nullptr && cacheExactRange(*secondSidecarRead));
  }

  void acceptNavigation(bool* const pendingRelayoutReposition) {
    if (pendingRelayoutReposition != nullptr) {
      *pendingRelayoutReposition = false;
    }
    clearCachedExactPosition();
    clearCurrentPageSemanticRange();
    noteDeferredRestoreSuccess();
    markPositionDirty();
  }
};

static_assert(sizeof(PdfReaderProgressState) <= 96, "PDF reader progress state exceeded its bounded allocation");
