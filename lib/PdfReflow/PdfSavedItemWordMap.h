#pragma once

#include <cstdint>

#include "PdfLayoutWordIndex.h"
#include "PdfSavedItemsStore.h"

struct PdfSavedItemWordIndexSource {
  using InspectFn = PdfStatus (*)(void* context, uint16_t sectionIndex, PdfLayoutWordIndexInfo* info);
  using ReadRangesFn = PdfStatus (*)(void* context, uint16_t sectionIndex, uint16_t firstPage, uint16_t count,
                                     PdfLayoutWordRange* ranges);

  void* context = nullptr;
  InspectFn inspect = nullptr;
  ReadRangesFn readRanges = nullptr;
  // Exact nonzero fingerprint/generation of the current pagination. A stored
  // page tuple is never reused when this differs.
  uint32_t layoutFingerprint = 0;

  constexpr bool valid() const { return inspect != nullptr && readRanges != nullptr; }
};

struct PdfSavedItemPageRange {
  uint16_t sectionIndex = 0;
  uint16_t startPage = 0;
  uint16_t endPage = 0;
  uint16_t pageCount = 0;
  // False means an exact-layout fallback tuple was used because the semantic
  // record was not present in the current word index.
  bool exact = false;
};

struct PdfSavedItemPageWordRange {
  uint16_t startWord = 0;
  uint16_t endWord = 0;
};

// Allocation-free mapper for the already-rendered current page. Call addWord
// once for each visible word in page order, passing true for semantic
// continuation/attachment fragments. It deliberately never examines text, so
// repeated words cannot select the wrong clipping occurrence.
class PdfSavedItemPageWordMapper {
 public:
  PdfStatus begin(const PdfSavedItem& item, uint16_t sectionIndex, uint32_t pageFirstOrdinal,
                  uint32_t pageLastOrdinal);
  void addWord(uint16_t pageWordIndex, bool attaches);
  bool finish(PdfSavedItemPageWordRange* range) const;

 private:
  uint32_t pageFirstOrdinal_ = 0;
  uint32_t pageLastOrdinal_ = 0;
  uint32_t itemFirstOrdinal_ = 0;
  uint32_t itemLastOrdinal_ = 0;
  uint32_t currentOrdinal_ = 0;
  uint16_t startWord_ = 0;
  uint16_t endWord_ = 0;
  bool initialized_ = false;
  bool hasOrdinal_ = false;
  bool found_ = false;
  bool invalid_ = false;
};

PdfStatus pdfMapSavedItemWordRange(const PdfSavedItemWordIndexSource& source, const PdfSavedItem& item,
                                   PdfSavedItemPageRange* pages);
