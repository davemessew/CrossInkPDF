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
