#pragma once

#include <cstddef>
#include <cstdint>

#include "PdfPageModel.h"
#include "PdfTypes.h"

namespace PdfHiddenTextLimits {

inline constexpr uint8_t MinPageOverlapNumerator = 1;
inline constexpr uint8_t MinPageOverlapDenominator = 2;
inline constexpr uint8_t MinImageOverlapNumerator = 1;
inline constexpr uint8_t MinImageOverlapDenominator = 4;
inline constexpr uint8_t DuplicateHorizontalOverlapNumerator = 1;
inline constexpr uint8_t DuplicateHorizontalOverlapDenominator = 2;
inline constexpr uint8_t DuplicateVerticalPageDivisor = 6;
inline constexpr uint8_t DuplicateVerticalHeightMultiplier = 8;
inline constexpr size_t MetadataPrefixBytes = 32;

}  // namespace PdfHiddenTextLimits

enum class PdfHiddenTextDecision : uint8_t {
  NotHidden,
  Qualified,
  Empty,
  Unmappable,
  ZeroTransform,
  ZeroArea,
  OffPage,
  NoImageOverlap,
  MetadataLike,
  DuplicateVisible,
};

struct PdfTextFingerprint {
  uint64_t hash = 1469598103934665603ULL;
  uint32_t units = 0;

  constexpr bool operator==(const PdfTextFingerprint& other) const {
    return hash == other.hash && units == other.units;
  }
};

struct PdfHiddenTextContext {
  PdfRectangle page{};
  const PdfTextRun* runs = nullptr;
  uint16_t runCount = 0;
  const uint8_t* text = nullptr;
  size_t textLength = 0;
  const PdfImagePlacement* images = nullptr;
  uint16_t imageCount = 0;
};

PdfStatus pdfFingerprintNormalizedText(const uint8_t* text, size_t length, PdfTextFingerprint* fingerprint);
PdfHiddenTextDecision pdfClassifyHiddenText(const PdfHiddenTextContext& context, uint16_t candidateIndex);
