#pragma once

#include <cstddef>
#include <cstdint>

#include "PdfLimits.h"
#include "PdfRunStore.h"
#include "PdfTypes.h"

namespace PdfReadingOrderLimits {

inline constexpr uint16_t MaxSortableRuns = PdfLimits::PageRunCount;
inline constexpr uint8_t HistogramBins = 64;
inline constexpr uint8_t MaxColumnGaps = 2;
inline constexpr uint8_t MinColumnRuns = 2;
inline constexpr uint8_t MinColumnGapPageDivisor = 20;
inline constexpr uint8_t HeadingWidthPercent = 55;
inline constexpr uint8_t TableMaxTextBytes = 8;
inline constexpr uint8_t TableMaxRowGapHeightMultiplier = 2;
inline constexpr uint8_t RepeatedBandPageDivisor = 10;
inline constexpr uint8_t RepeatedBandMinPages = 3;
inline constexpr uint8_t MaxRepeatedBandSignatures = 32;

}  // namespace PdfReadingOrderLimits

enum PdfReadingOrderFlag : uint16_t {
  PdfOrderNewLine = 1U << 0,
  PdfOrderNewBlock = 1U << 1,
  PdfOrderHeading = 1U << 2,
  PdfOrderTableCell = 1U << 3,
  PdfOrderFallback = 1U << 4,
};

struct PdfReadingOrderItem {
  uint32_t runOrdinal = 0;
  uint16_t flags = 0;
  uint8_t sectionIndex = 0;
  uint8_t columnIndex = 0;
};

static_assert(sizeof(PdfReadingOrderItem) == 8, "256 reading-order entries must fit the 2 KiB workspace");

struct PdfReadingOrderWorkspace {
  PdfReadingOrderItem* items = nullptr;
  uint16_t capacity = 0;
};

struct PdfReadingOrderSink {
  using EmitFn = PdfStatus (*)(void* context, const PdfReadingOrderItem& item);

  void* context = nullptr;
  EmitFn emit = nullptr;

  constexpr bool valid() const { return emit != nullptr; }
};

class PdfRepeatedBandTracker {
 public:
  PdfStatus observe(uint32_t pageOrdinal, const PdfRectangle& page, const PdfTextRun& run, const uint8_t* text,
                    size_t textLength);
  bool hasRepeatedBands() const;
  bool shouldSuppress(const PdfRectangle& page, const PdfTextRun& run, uint64_t hash, uint32_t units) const;

 private:
  enum class Band : uint8_t {
    None,
    Top,
    Bottom,
  };

  struct Entry {
    uint64_t hash = 0;
    uint32_t lastPage = UINT32_MAX;
    uint16_t units = 0;
    uint8_t pageCount = 0;
    Band band = Band::None;
  };

  static Band classifyBand(const PdfRectangle& page, const PdfTextRun& run);
  bool shouldSuppressBand(Band band, uint64_t hash, uint32_t units) const;

  Entry entries_[PdfReadingOrderLimits::MaxRepeatedBandSignatures]{};
  uint8_t count_ = 0;
  uint32_t lastObservedPage_ = UINT32_MAX;
};

class PdfReadingOrderReducer {
 public:
  explicit PdfReadingOrderReducer(PdfReadingOrderWorkspace workspace) : workspace_(workspace) {}

  PdfStatus reduce(PdfRunStore& runs, const PdfRectangle& page, uint32_t pageOrdinal,
                   const PdfRepeatedBandTracker* repeatedBands, uint64_t continuationOffset,
                   const PdfReadingOrderSink& sink, uint32_t* emittedCount);

 private:
  PdfReadingOrderWorkspace workspace_{};
};
