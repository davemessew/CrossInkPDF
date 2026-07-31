#include "PdfImagePreparation.h"

#include <algorithm>

#include "PdfLimits.h"

namespace {

constexpr int32_t kFixedOne = 1 << 16;
constexpr uint32_t kTinyDisplayPixels = 24;
constexpr uint32_t kRuleAspectRatio = 16;
constexpr uint32_t kCoverCoveragePercent = 35;
constexpr uint32_t kBackgroundCoveragePercent = 80;

uint32_t fixedExtent(const int32_t minimum, const int32_t maximum) {
  if (maximum <= minimum) {
    return 0;
  }
  const uint64_t raw = static_cast<uint64_t>(static_cast<int64_t>(maximum) - minimum);
  return static_cast<uint32_t>(std::min<uint64_t>((raw + kFixedOne - 1U) / kFixedOne, UINT32_MAX));
}

uint32_t clampedExtent(const int32_t minimum, const int32_t maximum, const uint16_t pageExtent) {
  const int64_t low = std::max<int64_t>(0, minimum);
  const int64_t high = std::min<int64_t>(static_cast<int64_t>(pageExtent) << 16, maximum);
  if (high <= low) {
    return 0;
  }
  return static_cast<uint32_t>((static_cast<uint64_t>(high - low) + kFixedOne - 1U) / kFixedOne);
}

}  // namespace

PdfStatus pdfClassifyMeaningfulImage(const PdfImageMeaningInput& input, PdfImageMeaningDecision* const decision) {
  if (decision == nullptr || input.pageWidth == 0 || input.pageHeight == 0 || input.placement.pixelWidth == 0 ||
      input.placement.pixelHeight == 0 || input.repetitionCount == 0) {
    return PdfStatus::failure(PdfError::InvalidArgument);
  }
  *decision = {};
  decision->anchorOrdinal = input.nearestAnchorOrdinal;
  const uint32_t displayWidth = fixedExtent(input.placement.xMin, input.placement.xMax);
  const uint32_t displayHeight = fixedExtent(input.placement.yMin, input.placement.yMax);
  if (displayWidth == 0 || displayHeight == 0) {
    decision->omitReason = PdfImageOmitReason::InvalidPlacement;
    return PdfStatus::success();
  }
  const uint32_t coveredWidth = clampedExtent(input.placement.xMin, input.placement.xMax, input.pageWidth);
  const uint32_t coveredHeight = clampedExtent(input.placement.yMin, input.placement.yMax, input.pageHeight);
  const uint64_t coveredArea = static_cast<uint64_t>(coveredWidth) * coveredHeight;
  const uint64_t pageArea = static_cast<uint64_t>(input.pageWidth) * input.pageHeight;
  const uint32_t coveragePercent =
      pageArea == 0 ? 0 : static_cast<uint32_t>(std::min<uint64_t>(100, coveredArea * 100U / pageArea));

  const uint32_t shortSide = std::min(displayWidth, displayHeight);
  const uint32_t longSide = std::max(displayWidth, displayHeight);
  if (shortSide <= 3 && longSide >= shortSide * kRuleAspectRatio) {
    decision->omitReason = PdfImageOmitReason::Rule;
    return PdfStatus::success();
  }
  if (displayWidth <= kTinyDisplayPixels && displayHeight <= kTinyDisplayPixels) {
    decision->omitReason = PdfImageOmitReason::TinyDecoration;
    return PdfStatus::success();
  }
  if (input.repetitionCount >= 3) {
    decision->omitReason = PdfImageOmitReason::RepeatedDecoration;
    return PdfStatus::success();
  }

  const bool earlyCover = input.sourcePageIndex < PdfLimits::MaxCoverScanPages &&
                          coveragePercent >= kCoverCoveragePercent && input.repetitionCount == 1 &&
                          input.firstMeaningfulEarlyImage;
  if (earlyCover) {
    decision->retain = true;
    decision->coverCandidate = true;
    return PdfStatus::success();
  }
  if (coveragePercent >= kBackgroundCoveragePercent) {
    decision->omitReason = PdfImageOmitReason::Background;
    return PdfStatus::success();
  }
  if (!input.nearbySemanticContent) {
    decision->omitReason = PdfImageOmitReason::NoNearbySemanticContent;
    return PdfStatus::success();
  }
  decision->retain = true;
  return PdfStatus::success();
}

PdfStatus pdfMakePreparationImageWorkspace(uint8_t* const pageText, const size_t pageTextBytes,
                                           uint8_t* const decoderOutput, const size_t decoderOutputBytes,
                                           PdfImageWorkspace* const workspace) {
  if (workspace == nullptr || pageText == nullptr || decoderOutput == nullptr) {
    return PdfStatus::failure(PdfError::InvalidArgument);
  }
  *workspace = {};
  if (pageTextBytes < PdfLimits::PageTextBytes || decoderOutputBytes < PdfLimits::DecoderOutputBytes) {
    return PdfStatus::failure(PdfError::InsufficientMemory);
  }
  *workspace = {
      pageText,
      PdfLimits::PageTextBytes,
      decoderOutput,
      PdfLimits::DecoderOutputBytes,
  };
  return PdfStatus::success();
}
