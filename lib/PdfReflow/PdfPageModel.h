#pragma once

#include <cstddef>
#include <cstdint>

#include "PdfCheckedMath.h"
#include "PdfObjectParser.h"
#include "PdfTypes.h"

enum class PdfPageWarning : uint16_t {
  None = 0,
  VectorArtOmitted = 1U << 0,
  OptionalImageOmitted = 1U << 1,
};

constexpr PdfPageWarning operator|(const PdfPageWarning left, const PdfPageWarning right) {
  return static_cast<PdfPageWarning>(static_cast<uint16_t>(left) | static_cast<uint16_t>(right));
}

struct PdfImagePlacement {
  PdfObjectReference reference{};
  uint32_t sourceOrder = 0;
  uint32_t pixelWidth = 0;
  uint32_t pixelHeight = 0;
  int32_t xMin = 0;
  int32_t yMin = 0;
  int32_t xMax = 0;
  int32_t yMax = 0;
  uint16_t flags = 0;
  uint8_t imageMaskPaintLuminance = 0;
  uint8_t reserved = 0;
};

enum PdfImagePlacementFlag : uint16_t {
  PdfImageInline = 1U << 0,
};

enum PdfTextRunFlag : uint16_t {
  PdfTextHidden = 1U << 0,
  PdfTextActualText = 1U << 1,
  PdfTextExplicitWhitespace = 1U << 2,
  PdfTextPositionReset = 1U << 3,
  PdfTextLight = 1U << 4,
  PdfTextBold = 1U << 5,
  PdfTextArrayExplicitGap = 1U << 6,
  PdfTextArrayTightContinuation = 1U << 7,
};

struct PdfPageModelWorkspace {
  using SpillTextFn = PdfStatus (*)(void* context, size_t logicalOffset, const uint8_t* text, size_t length);

  uint8_t* text = nullptr;
  size_t textCapacity = 0;
  PdfTextRun* runs = nullptr;
  uint16_t runCapacity = 0;
  PdfImagePlacement* images = nullptr;
  uint16_t imageCapacity = 0;
  void* spillTextContext = nullptr;
  SpillTextFn spillText = nullptr;
};

class PdfPageModel {
 public:
  explicit PdfPageModel(PdfPageModelWorkspace workspace) : workspace_(workspace) {}

  PdfStatus reset();
  PdfStatus beginTextRun(const PdfTextRun& run);
  PdfStatus appendText(const uint8_t* text, size_t length);
  PdfStatus beginOverflowTextRun(const PdfTextRun& run, uint16_t* runIndex);
  PdfStatus appendOverflowText(const uint8_t* text, size_t length);
  PdfStatus expandTextRunBounds(uint16_t runIndex, int32_t x, int32_t y);
  PdfStatus setTextRunBaselineEnd(uint16_t runIndex, int32_t x, int32_t y);
  PdfStatus finishTextRun();
  void abortTextRun();
  PdfStatus appendImage(const PdfImagePlacement& image);
  void addWarning(PdfPageWarning warning) { warnings_ = warnings_ | warning; }

  const uint8_t* text() const { return workspace_.text; }
  size_t textLength() const { return textLength_; }
  const PdfTextRun* runs() const { return workspace_.runs; }
  const PdfTextRun* pendingTextRun() const { return runPending_ ? workspace_.runs + runCount_ : nullptr; }
  uint16_t runCount() const { return runCount_; }
  uint16_t runCapacity() const { return workspace_.runCapacity; }
  const PdfImagePlacement* images() const { return workspace_.images; }
  uint16_t imageCount() const { return imageCount_; }
  PdfPageWarning warnings() const { return warnings_; }

 private:
  PdfStatus appendStoredText(const uint8_t* text, size_t length);
  void rememberTextTail(const uint8_t* text, size_t length);
  void rollbackPendingText();

  PdfPageModelWorkspace workspace_{};
  size_t textLength_ = 0;
  size_t pendingTextStart_ = 0;
  uint16_t runCount_ = 0;
  uint16_t imageCount_ = 0;
  PdfPageWarning warnings_ = PdfPageWarning::None;
  enum class OverflowSeparator : uint8_t { None, Inferred, Explicit };
  OverflowSeparator overflowSeparator_ = OverflowSeparator::None;
  uint16_t duplicateOverlayOffset_ = UINT16_MAX;
  uint8_t textTail_[3]{};
  uint8_t pendingTextTail_[3]{};
  uint8_t textTailLength_ = 0;
  uint8_t pendingTextTailLength_ = 0;
  bool runPending_ = false;
};
