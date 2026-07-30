#include "PdfReadingOrder.h"

#include <algorithm>
#include <cstring>
#include <limits>

namespace {

constexpr uint64_t FNV_OFFSET = 1469598103934665603ULL;
constexpr uint64_t FNV_PRIME = 1099511628211ULL;

bool validRect(const PdfRectangle& rect) { return rect.xMax > rect.xMin && rect.yMax > rect.yMin; }

uint64_t dimension(const int32_t minimum, const int32_t maximum) {
  return maximum > minimum ? static_cast<uint64_t>(static_cast<int64_t>(maximum) - minimum) : 0;
}

uint64_t runWidth(const PdfTextRun& run) { return dimension(run.xMin, run.xMax); }

uint64_t runHeight(const PdfTextRun& run) { return dimension(run.yMin, run.yMax); }

uint64_t absolute(const int64_t value) { return static_cast<uint64_t>(value < 0 ? -value : value); }

bool isRotatedNoise(const PdfTextRun& run) {
  const uint64_t dx = absolute(run.baselineDx);
  const uint64_t dy = absolute(run.baselineDy);
  return dy != 0 && (dx == 0 || dy * 2 > dx);
}

bool normalizedWhitespace(const uint8_t value) {
  return value == '\t' || value == '\n' || value == '\f' || value == '\r' || value == ' ';
}

void fingerprintByte(const uint8_t value, uint64_t* const hash, uint32_t* const units) {
  *hash ^= value;
  *hash *= FNV_PRIME;
  if (*units != std::numeric_limits<uint32_t>::max()) {
    ++*units;
  }
}

PdfStatus fingerprintMemory(const uint8_t* const text, const size_t length, uint64_t* const hash,
                            uint32_t* const units) {
  if (text == nullptr || hash == nullptr || units == nullptr) {
    return PdfStatus::failure(PdfError::InvalidArgument);
  }
  *hash = FNV_OFFSET;
  *units = 0;
  bool emitted = false;
  bool pendingSpace = false;
  for (size_t index = 0; index < length; ++index) {
    uint8_t value = text[index];
    if (normalizedWhitespace(value)) {
      pendingSpace = emitted;
      continue;
    }
    if (pendingSpace) {
      fingerprintByte(' ', hash, units);
      pendingSpace = false;
    }
    if (value >= 'A' && value <= 'Z') {
      value = static_cast<uint8_t>(value + ('a' - 'A'));
    }
    fingerprintByte(value, hash, units);
    emitted = true;
  }
  return PdfStatus::success();
}

PdfStatus fingerprintRun(const PdfRunStore& runs, const uint32_t ordinal, uint64_t* const hash, uint32_t* const units) {
  if (hash == nullptr || units == nullptr) {
    return PdfStatus::failure(PdfError::InvalidArgument, ordinal);
  }
  PdfTextRun run{};
  PdfStatus status = runs.readRun(ordinal, &run);
  if (!status.ok()) {
    return status;
  }
  *hash = FNV_OFFSET;
  *units = 0;
  bool emitted = false;
  bool pendingSpace = false;
  uint32_t offset = 0;
  uint8_t buffer[128]{};
  while (offset < run.textLength) {
    size_t bytesRead = 0;
    status =
        runs.readText(ordinal, offset, buffer, std::min<size_t>(sizeof(buffer), run.textLength - offset), &bytesRead);
    if (!status.ok()) {
      return status;
    }
    if (bytesRead == 0) {
      return PdfStatus::failure(PdfError::UnexpectedEof, offset);
    }
    for (size_t index = 0; index < bytesRead; ++index) {
      uint8_t value = buffer[index];
      if (normalizedWhitespace(value)) {
        pendingSpace = emitted;
        continue;
      }
      if (pendingSpace) {
        fingerprintByte(' ', hash, units);
        pendingSpace = false;
      }
      if (value >= 'A' && value <= 'Z') {
        value = static_cast<uint8_t>(value + ('a' - 'A'));
      }
      fingerprintByte(value, hash, units);
      emitted = true;
    }
    offset += static_cast<uint32_t>(bytesRead);
  }
  return PdfStatus::success();
}

PdfStatus shouldEmitRun(const PdfRunStore& runs, const uint32_t ordinal, const PdfRectangle& page,
                        const PdfRepeatedBandTracker* const repeatedBands, bool* const emit) {
  if (emit == nullptr) {
    return PdfStatus::failure(PdfError::InvalidArgument, ordinal);
  }
  PdfTextRun run{};
  PdfStatus status = runs.readRun(ordinal, &run);
  if (!status.ok()) {
    return status;
  }
  *emit = !isRotatedNoise(run);
  if (!*emit || repeatedBands == nullptr || !repeatedBands->hasRepeatedBands()) {
    return PdfStatus::success();
  }
  uint64_t hash = 0;
  uint32_t units = 0;
  status = fingerprintRun(runs, ordinal, &hash, &units);
  if (!status.ok()) {
    return status;
  }
  *emit = !repeatedBands->shouldSuppress(page, run, hash, units);
  return PdfStatus::success();
}

uint64_t pageWidth(const PdfRectangle& page) { return dimension(page.xMin, page.xMax); }

uint64_t pageHeight(const PdfRectangle& page) { return dimension(page.yMin, page.yMax); }

uint64_t approximateMedianHeight(const PdfRunStore& runs, const PdfReadingOrderItem* const items, const uint16_t count,
                                 const PdfRectangle& page, PdfStatus* const status) {
  uint16_t histogram[PdfReadingOrderLimits::HistogramBins]{};
  const uint64_t availableHeight = pageHeight(page);
  if (availableHeight == 0) {
    *status = PdfStatus::failure(PdfError::InvalidArgument);
    return 0;
  }
  for (uint16_t index = 0; index < count; ++index) {
    PdfTextRun run{};
    *status = runs.readRun(items[index].runOrdinal, &run);
    if (!status->ok()) {
      return 0;
    }
    const uint64_t height = std::min(runHeight(run), availableHeight);
    const uint8_t bin = static_cast<uint8_t>(std::min<uint64_t>(
        PdfReadingOrderLimits::HistogramBins - 1, (height * PdfReadingOrderLimits::HistogramBins) / availableHeight));
    ++histogram[bin];
  }
  const uint16_t target = static_cast<uint16_t>((count + 1) / 2);
  uint16_t seen = 0;
  for (uint8_t bin = 0; bin < PdfReadingOrderLimits::HistogramBins; ++bin) {
    seen = static_cast<uint16_t>(seen + histogram[bin]);
    if (seen >= target) {
      *status = PdfStatus::success();
      return std::max<uint64_t>(
          1, ((static_cast<uint64_t>(bin) + 1) * availableHeight) / PdfReadingOrderLimits::HistogramBins);
    }
  }
  *status = PdfStatus::success();
  return 1;
}

bool baselinesNear(const PdfTextRun& left, const PdfTextRun& right, const uint64_t tolerance) {
  return absolute(static_cast<int64_t>(left.baseline) - right.baseline) <= tolerance;
}

PdfStatus markTableCells(const PdfRunStore& runs, PdfReadingOrderItem* const items, const uint16_t count,
                         const uint64_t medianHeight) {
  constexpr uint16_t TABLE_CANDIDATE = 1U << 14;
  constexpr uint16_t TABLE_CONFIRMED_ROW = 1U << 15;
  const uint64_t lineTolerance = std::max<uint64_t>(1, medianHeight / 3);

  // First mark short, well-separated fragments that share a line. This is O(n^2)
  // and capped at 256 entries; it avoids the cubic/quartic probes that dense PDFs
  // could otherwise turn into excessive CPU wake time.
  for (uint16_t first = 0; first < count; ++first) {
    PdfTextRun firstRun{};
    PdfStatus status = runs.readRun(items[first].runOrdinal, &firstRun);
    if (!status.ok()) {
      return status;
    }
    if (firstRun.textLength > PdfReadingOrderLimits::TableMaxTextBytes) {
      continue;
    }
    for (uint16_t second = static_cast<uint16_t>(first + 1); second < count; ++second) {
      PdfTextRun secondRun{};
      status = runs.readRun(items[second].runOrdinal, &secondRun);
      if (!status.ok()) {
        return status;
      }
      if (secondRun.textLength > PdfReadingOrderLimits::TableMaxTextBytes ||
          !baselinesNear(firstRun, secondRun, lineTolerance)) {
        continue;
      }
      const uint64_t separation = absolute((static_cast<int64_t>(firstRun.xMin) + firstRun.xMax) -
                                           (static_cast<int64_t>(secondRun.xMin) + secondRun.xMax)) /
                                  2;
      if (separation < medianHeight * 4) {
        continue;
      }
      items[first].flags |= TABLE_CANDIDATE;
      items[second].flags |= TABLE_CANDIDATE;
    }
  }

  // A candidate line becomes a table row only when another nearby candidate
  // line has at least one aligned cell start.
  for (uint16_t first = 0; first < count; ++first) {
    if ((items[first].flags & TABLE_CANDIDATE) == 0) {
      continue;
    }
    PdfTextRun firstRun{};
    PdfStatus status = runs.readRun(items[first].runOrdinal, &firstRun);
    if (!status.ok()) {
      return status;
    }
    for (uint16_t second = static_cast<uint16_t>(first + 1); second < count; ++second) {
      if ((items[second].flags & TABLE_CANDIDATE) == 0) {
        continue;
      }
      PdfTextRun secondRun{};
      status = runs.readRun(items[second].runOrdinal, &secondRun);
      if (!status.ok()) {
        return status;
      }
      const uint64_t rowGap = absolute(static_cast<int64_t>(firstRun.baseline) - secondRun.baseline);
      if (rowGap <= lineTolerance || rowGap > medianHeight * PdfReadingOrderLimits::TableMaxRowGapHeightMultiplier ||
          absolute(static_cast<int64_t>(firstRun.xMin) - secondRun.xMin) > medianHeight) {
        continue;
      }
      items[first].flags |= TABLE_CONFIRMED_ROW;
      items[second].flags |= TABLE_CONFIRMED_ROW;
    }
  }

  // Promote every candidate on a confirmed baseline, then discard temporary bits.
  for (uint16_t candidate = 0; candidate < count; ++candidate) {
    if ((items[candidate].flags & TABLE_CANDIDATE) == 0) {
      continue;
    }
    PdfTextRun candidateRun{};
    PdfStatus status = runs.readRun(items[candidate].runOrdinal, &candidateRun);
    if (!status.ok()) {
      return status;
    }
    for (uint16_t confirmed = 0; confirmed < count; ++confirmed) {
      if ((items[confirmed].flags & TABLE_CONFIRMED_ROW) == 0) {
        continue;
      }
      PdfTextRun confirmedRun{};
      status = runs.readRun(items[confirmed].runOrdinal, &confirmedRun);
      if (!status.ok()) {
        return status;
      }
      if (baselinesNear(candidateRun, confirmedRun, lineTolerance)) {
        items[candidate].flags |= PdfOrderTableCell;
        break;
      }
    }
  }
  for (uint16_t index = 0; index < count; ++index) {
    items[index].flags &= static_cast<uint16_t>(~(TABLE_CANDIDATE | TABLE_CONFIRMED_ROW));
  }
  return PdfStatus::success();
}

PdfStatus markHeadingsAndSections(const PdfRunStore& runs, PdfReadingOrderItem* const items, const uint16_t count,
                                  const PdfRectangle& page) {
  const uint64_t headingWidth = (pageWidth(page) / 100) * PdfReadingOrderLimits::HeadingWidthPercent +
                                ((pageWidth(page) % 100) * PdfReadingOrderLimits::HeadingWidthPercent) / 100;
  for (uint16_t index = 0; index < count; ++index) {
    PdfTextRun run{};
    PdfStatus status = runs.readRun(items[index].runOrdinal, &run);
    if (!status.ok()) {
      return status;
    }
    if ((items[index].flags & PdfOrderTableCell) == 0 && runWidth(run) >= headingWidth) {
      items[index].flags |= PdfOrderHeading;
    }
  }
  for (uint16_t index = 0; index < count; ++index) {
    PdfTextRun run{};
    PdfStatus status = runs.readRun(items[index].runOrdinal, &run);
    if (!status.ok()) {
      return status;
    }
    uint16_t headingsAbove = 0;
    for (uint16_t candidate = 0; candidate < count; ++candidate) {
      if ((items[candidate].flags & PdfOrderHeading) == 0) {
        continue;
      }
      PdfTextRun heading{};
      status = runs.readRun(items[candidate].runOrdinal, &heading);
      if (!status.ok()) {
        return status;
      }
      if (heading.baseline > run.baseline) {
        ++headingsAbove;
      }
    }
    items[index].sectionIndex =
        static_cast<uint8_t>(std::min<uint16_t>(headingsAbove, std::numeric_limits<uint8_t>::max()));
  }

  int32_t tableTop = std::numeric_limits<int32_t>::min();
  int32_t tableBottom = std::numeric_limits<int32_t>::max();
  bool hasTable = false;
  for (uint16_t index = 0; index < count; ++index) {
    if ((items[index].flags & PdfOrderTableCell) == 0) {
      continue;
    }
    PdfTextRun run{};
    const PdfStatus status = runs.readRun(items[index].runOrdinal, &run);
    if (!status.ok()) {
      return status;
    }
    tableTop = std::max(tableTop, run.baseline);
    tableBottom = std::min(tableBottom, run.baseline);
    hasTable = true;
  }
  if (hasTable) {
    for (uint16_t index = 0; index < count; ++index) {
      PdfTextRun run{};
      const PdfStatus status = runs.readRun(items[index].runOrdinal, &run);
      if (!status.ok()) {
        return status;
      }
      const uint16_t base = items[index].sectionIndex;
      uint16_t phase = 1;
      if ((items[index].flags & PdfOrderTableCell) == 0) {
        phase = run.baseline > tableTop ? 0 : (run.baseline < tableBottom ? 2 : 1);
      }
      items[index].sectionIndex =
          static_cast<uint8_t>(std::min<uint16_t>(base * 3 + phase, std::numeric_limits<uint8_t>::max()));
    }
  }
  return PdfStatus::success();
}

uint8_t histogramBin(const int32_t x, const PdfRectangle& page) {
  if (x <= page.xMin) {
    return 0;
  }
  if (x >= page.xMax) {
    return PdfReadingOrderLimits::HistogramBins - 1;
  }
  const uint64_t offset = static_cast<uint64_t>(static_cast<int64_t>(x) - page.xMin);
  return static_cast<uint8_t>(std::min<uint64_t>(PdfReadingOrderLimits::HistogramBins - 1,
                                                 (offset * PdfReadingOrderLimits::HistogramBins) / pageWidth(page)));
}

struct ColumnGap {
  uint8_t first = 0;
  uint8_t last = 0;
  uint8_t center = 0;
  uint8_t width = 0;
};

PdfStatus assignColumns(const PdfRunStore& runs, PdfReadingOrderItem* const items, const uint16_t count,
                        const PdfRectangle& page, uint8_t* const columnCount) {
  uint16_t occupancy[PdfReadingOrderLimits::HistogramBins]{};
  uint8_t minBin = PdfReadingOrderLimits::HistogramBins - 1;
  uint8_t maxBin = 0;
  uint16_t eligible = 0;
  for (uint16_t index = 0; index < count; ++index) {
    if ((items[index].flags & (PdfOrderHeading | PdfOrderTableCell)) != 0) {
      continue;
    }
    PdfTextRun run{};
    const PdfStatus status = runs.readRun(items[index].runOrdinal, &run);
    if (!status.ok()) {
      return status;
    }
    const uint8_t first = histogramBin(run.xMin, page);
    const uint8_t last = histogramBin(run.xMax, page);
    minBin = std::min(minBin, first);
    maxBin = std::max(maxBin, last);
    for (uint8_t bin = first; bin <= last; ++bin) {
      if (occupancy[bin] != std::numeric_limits<uint16_t>::max()) {
        ++occupancy[bin];
      }
      if (bin == PdfReadingOrderLimits::HistogramBins - 1) {
        break;
      }
    }
    ++eligible;
  }
  *columnCount = 1;
  if (eligible < PdfReadingOrderLimits::MinColumnRuns * 2 || minBin >= maxBin) {
    return PdfStatus::success();
  }
  const uint8_t minGapBins = static_cast<uint8_t>(
      std::max<uint64_t>(2, PdfReadingOrderLimits::HistogramBins / PdfReadingOrderLimits::MinColumnGapPageDivisor));
  ColumnGap gaps[PdfReadingOrderLimits::MaxColumnGaps]{};
  uint8_t gapCount = 0;
  uint8_t bin = static_cast<uint8_t>(minBin + 1);
  while (bin < maxBin) {
    if (occupancy[bin] != 0) {
      ++bin;
      continue;
    }
    const uint8_t first = bin;
    while (bin < maxBin && occupancy[bin] == 0) {
      ++bin;
    }
    const uint8_t width = static_cast<uint8_t>(bin - first);
    if (width < minGapBins) {
      continue;
    }
    const ColumnGap candidate{first, static_cast<uint8_t>(bin - 1), static_cast<uint8_t>(first + width / 2), width};
    if (gapCount < PdfReadingOrderLimits::MaxColumnGaps) {
      gaps[gapCount++] = candidate;
    } else {
      uint8_t narrowest = gaps[0].width <= gaps[1].width ? 0 : 1;
      if (candidate.width > gaps[narrowest].width) {
        gaps[narrowest] = candidate;
      }
    }
  }
  if (gapCount == 0) {
    return PdfStatus::success();
  }
  if (gapCount == 2 && gaps[0].center > gaps[1].center) {
    std::swap(gaps[0], gaps[1]);
  }
  uint16_t perColumn[PdfReadingOrderLimits::MaxColumnGaps + 1]{};
  for (uint16_t index = 0; index < count; ++index) {
    if ((items[index].flags & (PdfOrderHeading | PdfOrderTableCell)) != 0) {
      continue;
    }
    PdfTextRun run{};
    const PdfStatus status = runs.readRun(items[index].runOrdinal, &run);
    if (!status.ok()) {
      return status;
    }
    const uint8_t center = histogramBin(static_cast<int32_t>((static_cast<int64_t>(run.xMin) + run.xMax) / 2), page);
    uint8_t column = 0;
    while (column < gapCount && center > gaps[column].center) {
      ++column;
    }
    items[index].columnIndex = column;
    ++perColumn[column];
  }
  for (uint8_t column = 0; column <= gapCount; ++column) {
    if (perColumn[column] < PdfReadingOrderLimits::MinColumnRuns) {
      for (uint16_t index = 0; index < count; ++index) {
        items[index].columnIndex = 0;
      }
      return PdfStatus::success();
    }
  }
  *columnCount = static_cast<uint8_t>(gapCount + 1);
  return PdfStatus::success();
}

PdfStatus compareItems(const PdfRunStore& runs, const PdfReadingOrderItem& left, const PdfReadingOrderItem& right,
                       const uint8_t columnCount, int* const result) {
  if (left.sectionIndex != right.sectionIndex) {
    *result = left.sectionIndex < right.sectionIndex ? -1 : 1;
    return PdfStatus::success();
  }
  PdfTextRun leftRun{};
  PdfTextRun rightRun{};
  PdfStatus status = runs.readRun(left.runOrdinal, &leftRun);
  if (!status.ok()) {
    return status;
  }
  status = runs.readRun(right.runOrdinal, &rightRun);
  if (!status.ok()) {
    return status;
  }
  const bool table = (left.flags & PdfOrderTableCell) != 0 || (right.flags & PdfOrderTableCell) != 0;
  if (!table && columnCount > 1 && left.columnIndex != right.columnIndex) {
    *result = left.columnIndex < right.columnIndex ? -1 : 1;
    return PdfStatus::success();
  }
  if (leftRun.baseline != rightRun.baseline) {
    *result = leftRun.baseline > rightRun.baseline ? -1 : 1;
  } else if (leftRun.xMin != rightRun.xMin) {
    *result = leftRun.xMin < rightRun.xMin ? -1 : 1;
  } else if (leftRun.sourceOrder != rightRun.sourceOrder) {
    *result = leftRun.sourceOrder < rightRun.sourceOrder ? -1 : 1;
  } else {
    *result = left.runOrdinal < right.runOrdinal ? -1 : (left.runOrdinal == right.runOrdinal ? 0 : 1);
  }
  return PdfStatus::success();
}

PdfStatus shellSort(const PdfRunStore& runs, PdfReadingOrderItem* const items, const uint16_t count,
                    const uint8_t columnCount) {
  for (uint16_t gap = static_cast<uint16_t>(count / 2); gap > 0; gap = static_cast<uint16_t>(gap / 2)) {
    for (uint16_t index = gap; index < count; ++index) {
      const PdfReadingOrderItem value = items[index];
      uint16_t position = index;
      while (position >= gap) {
        int comparison = 0;
        const PdfStatus status = compareItems(runs, value, items[position - gap], columnCount, &comparison);
        if (!status.ok()) {
          return status;
        }
        if (comparison >= 0) {
          break;
        }
        items[position] = items[position - gap];
        position = static_cast<uint16_t>(position - gap);
      }
      items[position] = value;
    }
  }
  return PdfStatus::success();
}

PdfStatus significantEdgeByte(const PdfRunStore& runs, const uint32_t ordinal, const bool fromEnd, uint8_t* const value,
                              bool* const found) {
  if (value == nullptr || found == nullptr) {
    return PdfStatus::failure(PdfError::InvalidArgument, ordinal);
  }
  PdfTextRun run{};
  PdfStatus status = runs.readRun(ordinal, &run);
  if (!status.ok()) {
    return status;
  }
  constexpr size_t EDGE_BYTES = 32;
  uint8_t buffer[EDGE_BYTES]{};
  const uint32_t offset =
      fromEnd && run.textLength > EDGE_BYTES ? run.textLength - static_cast<uint32_t>(EDGE_BYTES) : 0;
  const size_t requested = std::min<size_t>(EDGE_BYTES, run.textLength - offset);
  size_t bytesRead = 0;
  status = runs.readText(ordinal, offset, buffer, requested, &bytesRead);
  if (!status.ok()) {
    return status;
  }
  *found = false;
  if (fromEnd) {
    for (size_t index = bytesRead; index > 0; --index) {
      if (!normalizedWhitespace(buffer[index - 1])) {
        *value = buffer[index - 1];
        *found = true;
        break;
      }
    }
  } else {
    for (size_t index = 0; index < bytesRead; ++index) {
      if (!normalizedWhitespace(buffer[index])) {
        *value = buffer[index];
        *found = true;
        break;
      }
    }
  }
  return PdfStatus::success();
}

PdfStatus emitSorted(const PdfRunStore& runs, PdfReadingOrderItem* const items, const uint16_t count,
                     const uint64_t medianHeight, const PdfReadingOrderSink& sink, uint32_t* const emitted) {
  PdfTextRun previous{};
  bool hasPrevious = false;
  for (uint16_t index = 0; index < count; ++index) {
    PdfTextRun current{};
    PdfStatus status = runs.readRun(items[index].runOrdinal, &current);
    if (!status.ok()) {
      return status;
    }
    const bool newLine = !hasPrevious || items[index].sectionIndex != items[index - 1].sectionIndex ||
                         items[index].columnIndex != items[index - 1].columnIndex ||
                         !baselinesNear(previous, current, std::max<uint64_t>(1, medianHeight / 3));
    bool newBlock = !hasPrevious || items[index].sectionIndex != items[index - 1].sectionIndex ||
                    (items[index].flags & PdfOrderHeading) != 0;
    if (newLine && hasPrevious && !newBlock) {
      const uint64_t gap = absolute(static_cast<int64_t>(previous.baseline) - current.baseline);
      const uint64_t indent = absolute(static_cast<int64_t>(previous.xMin) - current.xMin);
      newBlock = gap > medianHeight * 2 || indent > medianHeight * 2 || (items[index].flags & PdfOrderTableCell) != 0;
      if (!newBlock && gap * 2 > medianHeight * 3) {
        uint8_t previousEnd = 0;
        uint8_t currentStart = 0;
        bool hasPreviousEnd = false;
        bool hasCurrentStart = false;
        status = significantEdgeByte(runs, items[index - 1].runOrdinal, true, &previousEnd, &hasPreviousEnd);
        if (!status.ok()) {
          return status;
        }
        status = significantEdgeByte(runs, items[index].runOrdinal, false, &currentStart, &hasCurrentStart);
        if (!status.ok()) {
          return status;
        }
        const bool strongPunctuation =
            hasPreviousEnd && (previousEnd == '.' || previousEnd == '!' || previousEnd == '?' || previousEnd == ':');
        const bool continuation = hasCurrentStart && currentStart >= 'a' && currentStart <= 'z';
        newBlock = strongPunctuation && !continuation;
      }
    }
    if (newLine) {
      items[index].flags |= PdfOrderNewLine;
    }
    if (newBlock) {
      items[index].flags |= PdfOrderNewBlock;
    }
    status = sink.emit(sink.context, items[index]);
    if (!status.ok()) {
      return status;
    }
    ++*emitted;
    previous = current;
    hasPrevious = true;
  }
  return PdfStatus::success();
}

PdfStatus emitFallback(const PdfRunStore& runs, const PdfRectangle& page, const PdfRepeatedBandTracker* repeatedBands,
                       const PdfReadingOrderSink& sink, uint32_t* const emitted) {
  for (uint32_t ordinal = 0; ordinal < runs.count(); ++ordinal) {
    bool include = false;
    PdfStatus status = shouldEmitRun(runs, ordinal, page, repeatedBands, &include);
    if (!status.ok()) {
      return status;
    }
    if (!include) {
      continue;
    }
    PdfReadingOrderItem item{};
    item.runOrdinal = ordinal;
    item.flags = PdfOrderNewLine | PdfOrderNewBlock | PdfOrderFallback;
    status = sink.emit(sink.context, item);
    if (!status.ok()) {
      return status;
    }
    ++*emitted;
  }
  return PdfStatus::success();
}

}  // namespace

PdfRepeatedBandTracker::Band PdfRepeatedBandTracker::classifyBand(const PdfRectangle& page, const PdfTextRun& run) {
  if (!validRect(page) || run.xMax <= run.xMin || run.yMax <= run.yMin) {
    return Band::None;
  }
  const uint64_t bandHeight = pageHeight(page) / PdfReadingOrderLimits::RepeatedBandPageDivisor;
  const int64_t topBoundary = static_cast<int64_t>(page.yMax) - static_cast<int64_t>(bandHeight);
  const int64_t bottomBoundary = static_cast<int64_t>(page.yMin) + static_cast<int64_t>(bandHeight);
  const int64_t center = (static_cast<int64_t>(run.yMin) + run.yMax) / 2;
  if (center >= topBoundary) {
    return Band::Top;
  }
  if (center <= bottomBoundary) {
    return Band::Bottom;
  }
  return Band::None;
}

PdfStatus PdfRepeatedBandTracker::observe(const uint32_t pageOrdinal, const PdfRectangle& page, const PdfTextRun& run,
                                          const uint8_t* const text, const size_t textLength) {
  if (lastObservedPage_ != UINT32_MAX && pageOrdinal < lastObservedPage_) {
    return PdfStatus::failure(PdfError::InvalidArgument, pageOrdinal);
  }
  lastObservedPage_ = pageOrdinal;
  const Band band = classifyBand(page, run);
  if (band == Band::None) {
    return PdfStatus::success();
  }
  uint64_t hash = 0;
  uint32_t units = 0;
  const PdfStatus status = fingerprintMemory(text, textLength, &hash, &units);
  if (!status.ok()) {
    return status;
  }
  if (units == 0) {
    return PdfStatus::success();
  }
  if (units > std::numeric_limits<uint16_t>::max()) {
    // A 64 KiB band is not a credible running header/footer.
    return PdfStatus::success();
  }
  for (uint8_t index = 0; index < count_; ++index) {
    Entry& entry = entries_[index];
    if (entry.band != band || entry.hash != hash || entry.units != units) {
      continue;
    }
    if (entry.lastPage != pageOrdinal) {
      entry.lastPage = pageOrdinal;
      if (entry.pageCount != std::numeric_limits<uint8_t>::max()) {
        ++entry.pageCount;
      }
    }
    return PdfStatus::success();
  }
  if (count_ == PdfReadingOrderLimits::MaxRepeatedBandSignatures) {
    // Losing a suppression hint is safer than rejecting an otherwise readable document.
    return PdfStatus::success();
  }
  entries_[count_++] = {hash, pageOrdinal, static_cast<uint16_t>(units), 1, band};
  return PdfStatus::success();
}

bool PdfRepeatedBandTracker::hasRepeatedBands() const {
  for (uint8_t index = 0; index < count_; ++index) {
    if (entries_[index].pageCount >= PdfReadingOrderLimits::RepeatedBandMinPages) {
      return true;
    }
  }
  return false;
}

bool PdfRepeatedBandTracker::shouldSuppressBand(const Band band, const uint64_t hash, const uint32_t units) const {
  if (band == Band::None || units == 0) {
    return false;
  }
  for (uint8_t index = 0; index < count_; ++index) {
    const Entry& entry = entries_[index];
    if (entry.band == band && entry.hash == hash && entry.units == units &&
        entry.pageCount >= PdfReadingOrderLimits::RepeatedBandMinPages) {
      return true;
    }
  }
  return false;
}

bool PdfRepeatedBandTracker::shouldSuppress(const PdfRectangle& page, const PdfTextRun& run, const uint64_t hash,
                                            const uint32_t units) const {
  return shouldSuppressBand(classifyBand(page, run), hash, units);
}

PdfStatus PdfReadingOrderReducer::reduce(PdfRunStore& runs, const PdfRectangle& page, const uint32_t,
                                         const PdfRepeatedBandTracker* const repeatedBands,
                                         const uint64_t continuationOffset, const PdfReadingOrderSink& sink,
                                         uint32_t* const emittedCount) {
  if (!validRect(page) || !sink.valid() || emittedCount == nullptr || workspace_.items == nullptr ||
      workspace_.capacity == 0) {
    return PdfStatus::failure(PdfError::InvalidArgument);
  }
  *emittedCount = 0;
  PdfStatus status = runs.beginReduction(continuationOffset);
  if (!status.ok()) {
    return status;
  }

  if (runs.count() > PdfReadingOrderLimits::MaxSortableRuns || runs.count() > workspace_.capacity) {
    status = emitFallback(runs, page, repeatedBands, sink, emittedCount);
  } else {
    uint16_t count = 0;
    for (uint32_t ordinal = 0; ordinal < runs.count(); ++ordinal) {
      bool include = false;
      status = shouldEmitRun(runs, ordinal, page, repeatedBands, &include);
      if (!status.ok()) {
        break;
      }
      if (include) {
        workspace_.items[count].runOrdinal = ordinal;
        workspace_.items[count].flags = 0;
        workspace_.items[count].sectionIndex = 0;
        workspace_.items[count].columnIndex = 0;
        ++count;
      }
    }
    if (status.ok() && count != 0) {
      uint64_t medianHeight = approximateMedianHeight(runs, workspace_.items, count, page, &status);
      if (status.ok()) {
        status = markTableCells(runs, workspace_.items, count, medianHeight);
      }
      if (status.ok()) {
        status = markHeadingsAndSections(runs, workspace_.items, count, page);
      }
      uint8_t columnCount = 1;
      if (status.ok()) {
        status = assignColumns(runs, workspace_.items, count, page, &columnCount);
      }
      if (status.ok()) {
        status = shellSort(runs, workspace_.items, count, columnCount);
      }
      if (status.ok()) {
        status = emitSorted(runs, workspace_.items, count, medianHeight, sink, emittedCount);
      }
    }
  }

  const PdfStatus lifecycleStatus = runs.endReduction();
  return lifecycleStatus.ok() ? status : lifecycleStatus;
}
