#include "PdfPageModel.h"

#include <algorithm>
#include <cstring>
#include <limits>

PdfStatus PdfPageModel::reset() {
  if (workspace_.text == nullptr || workspace_.textCapacity == 0 || workspace_.runs == nullptr ||
      workspace_.runCapacity == 0 || workspace_.images == nullptr || workspace_.imageCapacity == 0) {
    return PdfStatus::failure(PdfError::InvalidArgument);
  }
  textLength_ = 0;
  pendingTextStart_ = 0;
  runCount_ = 0;
  imageCount_ = 0;
  warnings_ = PdfPageWarning::None;
  overflowSeparator_ = OverflowSeparator::None;
  duplicateOverlayOffset_ = UINT16_MAX;
  textTailLength_ = 0;
  pendingTextTailLength_ = 0;
  runPending_ = false;
  return PdfStatus::success();
}

void PdfPageModel::rememberTextTail(const uint8_t* const text, const size_t length) {
  if (length == 0) {
    return;
  }
  if (length >= sizeof(textTail_)) {
    std::memcpy(textTail_, text + length - sizeof(textTail_), sizeof(textTail_));
    textTailLength_ = sizeof(textTail_);
    return;
  }
  const size_t retained = std::min<size_t>(textTailLength_, sizeof(textTail_) - length);
  if (retained != 0) {
    std::memmove(textTail_, textTail_ + textTailLength_ - retained, retained);
  }
  std::memcpy(textTail_ + retained, text, length);
  textTailLength_ = static_cast<uint8_t>(retained + length);
}

PdfStatus PdfPageModel::appendStoredText(const uint8_t* const text, const size_t length) {
  if ((text == nullptr && length != 0) || length > std::numeric_limits<size_t>::max() - textLength_) {
    return PdfStatus::failure(PdfError::LimitExceeded, textLength_);
  }
  const size_t resident = textLength_ < workspace_.textCapacity
                              ? std::min(length, workspace_.textCapacity - textLength_)
                              : 0U;
  if (resident != 0) {
    std::memcpy(workspace_.text + textLength_, text, resident);
  }
  const size_t spilled = length - resident;
  if (spilled != 0) {
    if (workspace_.spillText == nullptr) {
      return PdfStatus::failure(PdfError::LimitExceeded, textLength_ + length);
    }
    const PdfStatus status = workspace_.spillText(workspace_.spillTextContext, textLength_ + resident,
                                                  text + resident, spilled);
    if (!status) {
      return status;
    }
  }
  rememberTextTail(text, length);
  textLength_ += length;
  return PdfStatus::success();
}

void PdfPageModel::rollbackPendingText() {
  textLength_ = pendingTextStart_;
  std::memcpy(textTail_, pendingTextTail_, sizeof(textTail_));
  textTailLength_ = pendingTextTailLength_;
}

PdfStatus PdfPageModel::beginTextRun(const PdfTextRun& run) {
  if (runPending_ || runCount_ >= workspace_.runCapacity || textLength_ > std::numeric_limits<uint32_t>::max()) {
    return PdfStatus::failure(PdfError::LimitExceeded, runCount_);
  }
  workspace_.runs[runCount_] = run;
  workspace_.runs[runCount_].textOffset = static_cast<uint32_t>(textLength_);
  workspace_.runs[runCount_].textLength = 0;
  pendingTextStart_ = textLength_;
  std::memcpy(pendingTextTail_, textTail_, sizeof(textTail_));
  pendingTextTailLength_ = textTailLength_;
  overflowSeparator_ = OverflowSeparator::None;
  runPending_ = true;
  return PdfStatus::success();
}

PdfStatus PdfPageModel::appendText(const uint8_t* const text, const size_t length) {
  if (!runPending_ || text == nullptr || length > std::numeric_limits<size_t>::max() - textLength_ ||
      length > std::numeric_limits<uint32_t>::max() - (textLength_ - pendingTextStart_)) {
    return PdfStatus::failure(PdfError::LimitExceeded, textLength_);
  }
  const PdfStatus appendStatus = appendStoredText(text, length);
  if (!appendStatus) {
    return appendStatus;
  }
  workspace_.runs[runCount_].textLength = static_cast<uint32_t>(textLength_ - pendingTextStart_);
  return PdfStatus::success();
}

PdfStatus PdfPageModel::beginOverflowTextRun(const PdfTextRun& run, uint16_t* const runIndex) {
  if (runIndex == nullptr || runPending_ || runCount_ == 0 || runCount_ < workspace_.runCapacity) {
    return PdfStatus::failure(PdfError::InvalidArgument, runCount_);
  }
  PdfTextRun& previous = workspace_.runs[runCount_ - 1U];
  if (((previous.flags ^ run.flags) & PdfTextHidden) != 0 ||
      static_cast<uint64_t>(previous.textOffset) + previous.textLength != textLength_) {
    return PdfStatus::failure(PdfError::LimitExceeded, runCount_);
  }
  if (((previous.flags ^ run.flags) & (PdfTextLight | PdfTextBold)) != 0U) {
    previous.flags &= static_cast<uint16_t>(~(PdfTextLight | PdfTextBold));
  }
  previous.flags |= static_cast<uint16_t>(run.flags & PdfTextActualText);
  const bool tightContinuation = (run.flags & PdfTextArrayTightContinuation) != 0;
  const bool explicitGap = (run.flags & PdfTextArrayExplicitGap) != 0;
  overflowSeparator_ = explicitGap ? OverflowSeparator::Explicit
                                   : (tightContinuation ? OverflowSeparator::None : OverflowSeparator::Inferred);
  previous.xMin = std::min(previous.xMin, run.xMin);
  previous.xMax = std::max(previous.xMax, run.xMax);
  previous.yMin = std::min(previous.yMin, run.yMin);
  previous.yMax = std::max(previous.yMax, run.yMax);
  *runIndex = static_cast<uint16_t>(runCount_ - 1U);
  return PdfStatus::success();
}

PdfStatus PdfPageModel::appendOverflowText(const uint8_t* const text, const size_t length) {
  if (runPending_ || runCount_ == 0 || text == nullptr) {
    return PdfStatus::failure(PdfError::LimitExceeded, textLength_);
  }
  PdfTextRun& previous = workspace_.runs[runCount_ - 1U];
  if (static_cast<uint64_t>(previous.textOffset) + previous.textLength != textLength_) {
    return PdfStatus::failure(PdfError::LimitExceeded, textLength_);
  }

  const bool currentSoftHyphen = length == 2U && text[0] == 0xC2U && text[1] == 0xADU;
  const bool currentUnicodeHyphen = length == 3U && text[0] == 0xE2U && text[1] == 0x80U &&
                                    (text[2] == 0x90U || text[2] == 0x91U);
  const bool previousSoftHyphen = textTailLength_ >= 2U && textTail_[textTailLength_ - 2U] == 0xC2U &&
                                  textTail_[textTailLength_ - 1U] == 0xADU;
  const bool previousUnicodeHyphen = textTailLength_ >= 3U && textTail_[textTailLength_ - 3U] == 0xE2U &&
                                     textTail_[textTailLength_ - 2U] == 0x80U &&
                                     (textTail_[textTailLength_ - 1U] == 0x90U ||
                                      textTail_[textTailLength_ - 1U] == 0x91U);
  const bool inferredWordJoin = currentSoftHyphen || currentUnicodeHyphen || previousSoftHyphen ||
                                previousUnicodeHyphen;
  const bool addSeparator = overflowSeparator_ == OverflowSeparator::Explicit ||
                            (overflowSeparator_ == OverflowSeparator::Inferred && !inferredWordJoin);
  const bool insertSeparator =
      addSeparator && textLength_ != 0 && textTailLength_ != 0 && textTail_[textTailLength_ - 1U] != ' ';
  overflowSeparator_ = OverflowSeparator::None;

  const size_t separatorLength = insertSeparator ? 1U : 0U;
  if (separatorLength > std::numeric_limits<size_t>::max() - textLength_ ||
      length > std::numeric_limits<size_t>::max() - textLength_ - separatorLength ||
      separatorLength > UINT32_MAX - previous.textLength ||
      length > UINT32_MAX - previous.textLength - separatorLength) {
    return PdfStatus::failure(PdfError::LimitExceeded, textLength_);
  }
  if (insertSeparator) {
    constexpr uint8_t separator = ' ';
    const PdfStatus status = appendStoredText(&separator, 1U);
    if (!status) {
      return status;
    }
    ++previous.textLength;
  }
  const PdfStatus appendStatus = appendStoredText(text, length);
  if (!appendStatus) {
    return appendStatus;
  }
  previous.textLength += static_cast<uint32_t>(length);
  return PdfStatus::success();
}

PdfStatus PdfPageModel::expandTextRunBounds(const uint16_t runIndex, const int32_t x, const int32_t y) {
  const bool completed = runIndex < runCount_;
  const bool pending = runPending_ && runIndex == runCount_;
  if (!completed && !pending) {
    return PdfStatus::failure(PdfError::InvalidArgument, runIndex);
  }
  PdfTextRun& run = workspace_.runs[runIndex];
  run.xMin = std::min(run.xMin, x);
  run.xMax = std::max(run.xMax, x);
  run.yMin = std::min(run.yMin, y);
  run.yMax = std::max(run.yMax, y);
  return PdfStatus::success();
}

PdfStatus PdfPageModel::setTextRunBaselineEnd(const uint16_t runIndex, const int32_t x, const int32_t y) {
  const bool completed = runIndex < runCount_;
  const bool pending = runPending_ && runIndex == runCount_;
  if (!completed && !pending) {
    return PdfStatus::failure(PdfError::InvalidArgument, runIndex);
  }
  PdfTextRun& run = workspace_.runs[runIndex];
  const int64_t dx = static_cast<int64_t>(x) - run.baselineX;
  const int64_t dy = static_cast<int64_t>(y) - run.baseline;
  if (dx < std::numeric_limits<int32_t>::min() || dx > std::numeric_limits<int32_t>::max() ||
      dy < std::numeric_limits<int32_t>::min() || dy > std::numeric_limits<int32_t>::max()) {
    return PdfStatus::failure(PdfError::LimitExceeded, runIndex);
  }
  run.baselineDx = static_cast<int32_t>(dx);
  run.baselineDy = static_cast<int32_t>(dy);
  return PdfStatus::success();
}

PdfStatus PdfPageModel::finishTextRun() {
  if (!runPending_) {
    return PdfStatus::failure(PdfError::InvalidArgument);
  }
  runPending_ = false;
  if (workspace_.runs[runCount_].textLength == 0) {
    rollbackPendingText();
    return PdfStatus::success();
  }
  if (runCount_ != 0) {
    PdfTextRun& previous = workspace_.runs[runCount_ - 1U];
    const PdfTextRun& current = workspace_.runs[runCount_];
    constexpr uint16_t duplicateRelevantFlags =
        PdfTextHidden | PdfTextActualText | PdfTextLight | PdfTextBold;
    constexpr int64_t duplicateCoordinateTolerance = 512;
    const auto duplicateCoordinate = [](const int32_t left, const int32_t right) {
      constexpr int64_t tolerance = duplicateCoordinateTolerance;
      const int64_t difference = static_cast<int64_t>(left) - right;
      return difference >= -tolerance && difference <= tolerance;
    };
    const bool duplicateGeometry =
        previous.fontId == current.fontId &&
        (previous.flags & duplicateRelevantFlags) == (current.flags & duplicateRelevantFlags) &&
        duplicateCoordinate(previous.baseline, current.baseline) &&
        static_cast<int64_t>(current.xMin) >= static_cast<int64_t>(previous.xMin) - duplicateCoordinateTolerance &&
        static_cast<int64_t>(current.xMax) <= static_cast<int64_t>(previous.xMax) + duplicateCoordinateTolerance &&
        duplicateCoordinate(previous.yMin, current.yMin) && duplicateCoordinate(previous.yMax, current.yMax);
    const bool currentTextResident =
        static_cast<uint64_t>(current.textOffset) + current.textLength <= workspace_.textCapacity;
    if (!currentTextResident) {
      duplicateOverlayOffset_ = UINT16_MAX;
      ++runCount_;
      return PdfStatus::success();
    }
    if (duplicateOverlayOffset_ != UINT16_MAX) {
      const uint32_t duplicateEnd = static_cast<uint32_t>(duplicateOverlayOffset_) + current.textLength;
      if (duplicateGeometry && duplicateEnd <= previous.textLength &&
          std::memcmp(workspace_.text + previous.textOffset + duplicateOverlayOffset_,
                      workspace_.text + current.textOffset, current.textLength) == 0) {
        duplicateOverlayOffset_ = duplicateEnd == previous.textLength
                                      ? UINT16_MAX
                                      : static_cast<uint16_t>(duplicateEnd);
        rollbackPendingText();
        return PdfStatus::success();
      }
      duplicateOverlayOffset_ = UINT16_MAX;
    }
    const bool exactPaintDuplicate =
        previous.textLength == current.textLength && previous.fontId == current.fontId &&
        (previous.flags & duplicateRelevantFlags) == (current.flags & duplicateRelevantFlags) &&
        duplicateCoordinate(previous.xMin, current.xMin) && duplicateCoordinate(previous.yMin, current.yMin) &&
        duplicateCoordinate(previous.xMax, current.xMax) && duplicateCoordinate(previous.yMax, current.yMax) &&
        duplicateCoordinate(previous.baselineX, current.baselineX) &&
        duplicateCoordinate(previous.baseline, current.baseline) &&
        duplicateCoordinate(previous.baselineDx, current.baselineDx) &&
        duplicateCoordinate(previous.baselineDy, current.baselineDy) &&
        std::memcmp(workspace_.text + previous.textOffset, workspace_.text + current.textOffset,
                    current.textLength) == 0;
    if (exactPaintDuplicate) {
      rollbackPendingText();
      return PdfStatus::success();
    }
    const bool fragmentedPaintDuplicate =
        duplicateGeometry && current.textLength < previous.textLength && previous.textLength <= UINT16_MAX &&
        (current.flags & PdfTextPositionReset) != 0 && duplicateCoordinate(previous.baselineX, current.baselineX) &&
        std::memcmp(workspace_.text + previous.textOffset, workspace_.text + current.textOffset,
                    current.textLength) == 0;
    if (fragmentedPaintDuplicate) {
      duplicateOverlayOffset_ = static_cast<uint16_t>(current.textLength);
      rollbackPendingText();
      return PdfStatus::success();
    }
    const int64_t baselineDifference = static_cast<int64_t>(current.baseline) - previous.baseline;
    const uint64_t previousTextEnd = static_cast<uint64_t>(previous.textOffset) + previous.textLength;
    const int64_t previousBaselineEnd = static_cast<int64_t>(previous.baselineX) + previous.baselineDx;
    const int64_t baselineGap = static_cast<int64_t>(current.baselineX) - previousBaselineEnd;
    const int64_t currentEndX = static_cast<int64_t>(current.baselineX) + current.baselineDx;
    const int64_t currentEndY = static_cast<int64_t>(current.baseline) + current.baselineDy;
    const int64_t lineHeight =
        std::max(static_cast<int64_t>(previous.yMax) - previous.yMin,
                 static_cast<int64_t>(current.yMax) - current.yMin);
    const int64_t previousHeight = static_cast<int64_t>(previous.yMax) - previous.yMin;
    const int64_t currentHeight = static_cast<int64_t>(current.yMax) - current.yMin;
    const int64_t minimumHeight = std::min(previousHeight, currentHeight);
    const int64_t verticalOverlap =
        std::min<int64_t>(previous.yMax, current.yMax) - std::max<int64_t>(previous.yMin, current.yMin);
    const int64_t absoluteBaselineDifference =
        baselineDifference < 0 ? -baselineDifference : baselineDifference;
    const bool overlappingInlineFragment =
        minimumHeight > 0 && verticalOverlap > 0 && verticalOverlap * 2 >= minimumHeight &&
        absoluteBaselineDifference <= std::max<int64_t>(65536, minimumHeight / 2) &&
        baselineGap >= -std::max<int64_t>(65536, minimumHeight / 2);
    const int64_t maximumMergeGap = std::max<int64_t>(65536, lineHeight * 2);
    const bool wideAbsolutePosition =
        (current.flags & PdfTextPositionReset) != 0 && baselineGap > lineHeight * 4;
    constexpr uint16_t mergeRelevantFlags = PdfTextHidden | PdfTextExplicitWhitespace;
    constexpr uint16_t weightFlags = PdfTextLight | PdfTextBold;
    const uint16_t weightDifference = (previous.flags ^ current.flags) & weightFlags;
    const bool isolatedSoftHyphen = current.textLength == 2U &&
                                    workspace_.text[current.textOffset] == 0xC2U &&
                                    workspace_.text[current.textOffset + 1U] == 0xADU;
    const bool isolatedUnicodeHyphen = current.textLength == 3U &&
                                       workspace_.text[current.textOffset] == 0xE2U &&
                                       workspace_.text[current.textOffset + 1U] == 0x80U &&
                                        (workspace_.text[current.textOffset + 2U] == 0x90U ||
                                         workspace_.text[current.textOffset + 2U] == 0x91U);
    if ((previous.flags & mergeRelevantFlags) == (current.flags & mergeRelevantFlags) &&
        (weightDifference == 0U || overlappingInlineFragment) &&
        (!wideAbsolutePosition || isolatedSoftHyphen || isolatedUnicodeHyphen) &&
        (absoluteBaselineDifference <= 65536 || overlappingInlineFragment) &&
        baselineGap <= maximumMergeGap &&
        previousTextEnd == current.textOffset && current.textLength <= UINT32_MAX - previous.textLength &&
        currentEndX - previous.baselineX >= INT32_MIN && currentEndX - previous.baselineX <= INT32_MAX &&
        currentEndY - previous.baseline >= INT32_MIN && currentEndY - previous.baseline <= INT32_MAX) {
      // Invisible OCR layers commonly use tightly positioned word fragments
      // whose visual gaps are smaller than normal typeset spaces. They still
      // need word separation when the PDF omitted literal space glyphs.
      const bool hiddenTextLayer = (previous.flags & PdfTextHidden) != 0U &&
                                   (current.flags & PdfTextHidden) != 0U;
      const int64_t wordGap = std::max<int64_t>(32768, lineHeight / (hiddenTextLayer ? 6 : 3));
      const uint8_t currentFirst = workspace_.text[current.textOffset];
      const bool currentJoinsPunctuation = currentFirst == '-' || currentFirst == ',' || currentFirst == '.' ||
                                           currentFirst == ';' || currentFirst == ':' || currentFirst == '!' ||
                                           currentFirst == '?' || currentFirst == ')' || currentFirst == ']' ||
                                           currentFirst == '}';
      const bool previousSoftHyphen = previousTextEnd >= 2U &&
                                      workspace_.text[previousTextEnd - 2U] == 0xC2U &&
                                      workspace_.text[previousTextEnd - 1U] == 0xADU;
      const bool previousUnicodeHyphen = previousTextEnd >= 3U &&
                                         workspace_.text[previousTextEnd - 3U] == 0xE2U &&
                                         workspace_.text[previousTextEnd - 2U] == 0x80U &&
                                         (workspace_.text[previousTextEnd - 1U] == 0x90U ||
                                          workspace_.text[previousTextEnd - 1U] == 0x91U);
      const bool previousJoinsWord = previousSoftHyphen || previousUnicodeHyphen ||
                                     (workspace_.text[current.textOffset - 1U] == '-' &&
                                      (baselineGap <= lineHeight / 4 ||
                                       (currentFirst >= '0' && currentFirst <= '9')));
      bool splitAllCapsGlyph = false;
      bool currentAllCapsFragment = current.textLength >= 1U && current.textLength <= 2U;
      for (uint32_t index = 0; currentAllCapsFragment && index < current.textLength; ++index) {
        const uint8_t value = workspace_.text[current.textOffset + index];
        currentAllCapsFragment = value >= 'A' && value <= 'Z';
      }
      if (currentAllCapsFragment &&
          (current.textLength != 1U || (currentFirst != 'A' && currentFirst != 'I')) &&
          baselineGap <= lineHeight / 4 &&
          (current.flags & PdfTextArrayTightContinuation) != 0) {
        bool hasUppercase = false;
        bool hasLowercase = false;
        for (uint64_t offset = previous.textOffset; offset < previousTextEnd; ++offset) {
          const uint8_t value = workspace_.text[offset];
          hasUppercase = hasUppercase || (value >= 'A' && value <= 'Z');
          hasLowercase = hasLowercase || (value >= 'a' && value <= 'z');
        }
        splitAllCapsGlyph = hasUppercase && !hasLowercase;
      }
      const bool charactersNeedSpace = workspace_.text[current.textOffset - 1U] != ' ' &&
                                       workspace_.text[current.textOffset] != ' ';
      const bool explicitArrayGap = (current.flags & PdfTextArrayExplicitGap) != 0;
      const auto asciiLetter = [](const uint8_t value) {
        return (value >= 'A' && value <= 'Z') || (value >= 'a' && value <= 'z');
      };
      uint64_t previousWordStart = previousTextEnd;
      while (previousWordStart > previous.textOffset &&
             asciiLetter(workspace_.text[previousWordStart - 1U])) {
        --previousWordStart;
      }
      const uint64_t previousWordLength = previousTextEnd - previousWordStart;
      const bool repeatedWordGap =
          (current.flags & PdfTextExplicitWhitespace) != 0 &&
          (current.flags & PdfTextArrayTightContinuation) == 0 && baselineGap > wordGap &&
          previousWordLength >= 4U && current.textLength >= 2U &&
          asciiLetter(workspace_.text[current.textOffset]) &&
          asciiLetter(workspace_.text[current.textOffset + 1U]) &&
          workspace_.text[previousWordStart] == workspace_.text[current.textOffset] &&
          workspace_.text[previousWordStart + 1U] == workspace_.text[current.textOffset + 1U];
      const bool inferredGap = (current.flags & PdfTextArrayTightContinuation) == 0 &&
                               (current.flags & PdfTextExplicitWhitespace) == 0 && baselineGap > wordGap &&
                               !currentJoinsPunctuation && !previousJoinsWord && !splitAllCapsGlyph;
      const int64_t positionedContinuationGap = std::max(wordGap, lineHeight + lineHeight / 4);
      const bool positionedWordContinuation =
          asciiLetter(workspace_.text[previousTextEnd - 1U]) && asciiLetter(currentFirst) &&
          baselineGap <= positionedContinuationGap;
      const bool positionedGap = (current.flags & PdfTextPositionReset) != 0 &&
                                 (current.flags & PdfTextArrayTightContinuation) == 0 &&
                                 baselineGap > wordGap && !currentJoinsPunctuation &&
                                 !previousJoinsWord && !splitAllCapsGlyph && !positionedWordContinuation;
      const bool insertSpace =
          charactersNeedSpace && (explicitArrayGap || inferredGap || positionedGap || repeatedWordGap);
      if (insertSpace) {
        if (textLength_ >= workspace_.textCapacity || current.textLength == UINT32_MAX - previous.textLength) {
          ++runCount_;
          return PdfStatus::success();
        }
        std::memmove(workspace_.text + current.textOffset + 1U, workspace_.text + current.textOffset,
                     current.textLength);
        workspace_.text[current.textOffset] = ' ';
        ++textLength_;
        ++previous.textLength;
      }
      previous.textLength += current.textLength;
      if (weightDifference != 0U) {
        previous.flags &= static_cast<uint16_t>(~weightFlags);
      }
      previous.xMin = std::min(previous.xMin, current.xMin);
      previous.xMax = std::max(previous.xMax, current.xMax);
      previous.yMin = std::min(previous.yMin, current.yMin);
      previous.yMax = std::max(previous.yMax, current.yMax);
      previous.baselineDx = static_cast<int32_t>(currentEndX - previous.baselineX);
      previous.baselineDy = static_cast<int32_t>(currentEndY - previous.baseline);
      duplicateOverlayOffset_ = UINT16_MAX;
      return PdfStatus::success();
    }
  }
  duplicateOverlayOffset_ = UINT16_MAX;
  ++runCount_;
  return PdfStatus::success();
}

void PdfPageModel::abortTextRun() {
  if (!runPending_) {
    return;
  }
  rollbackPendingText();
  duplicateOverlayOffset_ = UINT16_MAX;
  runPending_ = false;
}

PdfStatus PdfPageModel::appendImage(const PdfImagePlacement& image) {
  if (runPending_ || imageCount_ >= workspace_.imageCapacity) {
    return PdfStatus::failure(PdfError::LimitExceeded, imageCount_);
  }
  duplicateOverlayOffset_ = UINT16_MAX;
  workspace_.images[imageCount_++] = image;
  return PdfStatus::success();
}
