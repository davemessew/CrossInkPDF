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
  runPending_ = false;
  return PdfStatus::success();
}

PdfStatus PdfPageModel::beginTextRun(const PdfTextRun& run) {
  if (runPending_ || runCount_ >= workspace_.runCapacity || textLength_ > std::numeric_limits<uint32_t>::max()) {
    return PdfStatus::failure(PdfError::LimitExceeded, runCount_);
  }
  workspace_.runs[runCount_] = run;
  workspace_.runs[runCount_].textOffset = static_cast<uint32_t>(textLength_);
  workspace_.runs[runCount_].textLength = 0;
  pendingTextStart_ = textLength_;
  runPending_ = true;
  return PdfStatus::success();
}

PdfStatus PdfPageModel::appendText(const uint8_t* const text, const size_t length) {
  if (!runPending_ || text == nullptr || length > workspace_.textCapacity - textLength_ ||
      length > std::numeric_limits<uint32_t>::max() - (textLength_ - pendingTextStart_)) {
    return PdfStatus::failure(PdfError::LimitExceeded, textLength_);
  }
  std::memcpy(workspace_.text + textLength_, text, length);
  textLength_ += length;
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
  previous.flags |= static_cast<uint16_t>(run.flags & PdfTextActualText);
  if (textLength_ != 0 && workspace_.text[textLength_ - 1U] != ' ') {
    if (textLength_ >= workspace_.textCapacity || previous.textLength == UINT32_MAX) {
      return PdfStatus::failure(PdfError::LimitExceeded, textLength_);
    }
    workspace_.text[textLength_++] = ' ';
    ++previous.textLength;
  }
  previous.xMin = std::min(previous.xMin, run.xMin);
  previous.xMax = std::max(previous.xMax, run.xMax);
  previous.yMin = std::min(previous.yMin, run.yMin);
  previous.yMax = std::max(previous.yMax, run.yMax);
  *runIndex = static_cast<uint16_t>(runCount_ - 1U);
  return PdfStatus::success();
}

PdfStatus PdfPageModel::appendOverflowText(const uint8_t* const text, const size_t length) {
  if (runPending_ || runCount_ == 0 || text == nullptr || length > workspace_.textCapacity - textLength_) {
    return PdfStatus::failure(PdfError::LimitExceeded, textLength_);
  }
  PdfTextRun& previous = workspace_.runs[runCount_ - 1U];
  if (static_cast<uint64_t>(previous.textOffset) + previous.textLength != textLength_ ||
      length > UINT32_MAX - previous.textLength) {
    return PdfStatus::failure(PdfError::LimitExceeded, textLength_);
  }
  std::memcpy(workspace_.text + textLength_, text, length);
  textLength_ += length;
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
    textLength_ = pendingTextStart_;
    return PdfStatus::success();
  }
  if (runCount_ != 0) {
    PdfTextRun& previous = workspace_.runs[runCount_ - 1U];
    const PdfTextRun& current = workspace_.runs[runCount_];
    const int64_t baselineDifference = static_cast<int64_t>(current.baseline) - previous.baseline;
    const uint64_t previousTextEnd = static_cast<uint64_t>(previous.textOffset) + previous.textLength;
    const int64_t previousBaselineEnd = static_cast<int64_t>(previous.baselineX) + previous.baselineDx;
    const int64_t baselineGap = static_cast<int64_t>(current.baselineX) - previousBaselineEnd;
    const int64_t currentEndX = static_cast<int64_t>(current.baselineX) + current.baselineDx;
    const int64_t currentEndY = static_cast<int64_t>(current.baseline) + current.baselineDy;
    const int64_t lineHeight =
        std::max(static_cast<int64_t>(previous.yMax) - previous.yMin,
                 static_cast<int64_t>(current.yMax) - current.yMin);
    const int64_t maximumMergeGap = std::max<int64_t>(65536, lineHeight * 2);
    if (previous.fontId == current.fontId && previous.flags == current.flags &&
        baselineDifference >= -65536 && baselineDifference <= 65536 &&
        baselineGap <= maximumMergeGap &&
        previousTextEnd == current.textOffset && current.textLength <= UINT32_MAX - previous.textLength &&
        currentEndX - previous.baselineX >= INT32_MIN && currentEndX - previous.baselineX <= INT32_MAX &&
        currentEndY - previous.baseline >= INT32_MIN && currentEndY - previous.baseline <= INT32_MAX) {
      const int64_t wordGap = std::max<int64_t>(32768, lineHeight / 8);
      const bool insertSpace = baselineGap > wordGap && workspace_.text[current.textOffset - 1U] != ' ' &&
                               workspace_.text[current.textOffset] != ' ';
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
      previous.xMin = std::min(previous.xMin, current.xMin);
      previous.xMax = std::max(previous.xMax, current.xMax);
      previous.yMin = std::min(previous.yMin, current.yMin);
      previous.yMax = std::max(previous.yMax, current.yMax);
      previous.baselineDx = static_cast<int32_t>(currentEndX - previous.baselineX);
      previous.baselineDy = static_cast<int32_t>(currentEndY - previous.baseline);
      return PdfStatus::success();
    }
  }
  ++runCount_;
  return PdfStatus::success();
}

void PdfPageModel::abortTextRun() {
  if (!runPending_) {
    return;
  }
  textLength_ = pendingTextStart_;
  runPending_ = false;
}

PdfStatus PdfPageModel::appendImage(const PdfImagePlacement& image) {
  if (runPending_ || imageCount_ >= workspace_.imageCapacity) {
    return PdfStatus::failure(PdfError::LimitExceeded, imageCount_);
  }
  workspace_.images[imageCount_++] = image;
  return PdfStatus::success();
}
