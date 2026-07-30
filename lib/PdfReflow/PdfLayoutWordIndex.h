#pragma once

#include <cstddef>
#include <cstdint>

#include "PdfTypes.h"

inline constexpr size_t PDF_LAYOUT_WORD_ANCHOR_BYTES = 10;
inline constexpr size_t PDF_LAYOUT_WORD_INDEX_HEADER_BYTES = 32;
inline constexpr size_t PDF_LAYOUT_WORD_INDEX_RECORD_BYTES = 32;
inline constexpr size_t PDF_LAYOUT_WORD_INDEX_FOOTER_BYTES = 16;

struct PdfLayoutWordRange {
  uint32_t firstGlobalWordOrdinal = 0;
  uint32_t lastGlobalWordOrdinal = 0;
  uint32_t firstBlockWordOffset = 0;
  // Number of document words reached at the end of this rendered page.
  // For empty pages this preserves the cursor encoded in the fixed record.
  uint32_t wordCursor = 0;
  char blockAnchor[PDF_LAYOUT_WORD_ANCHOR_BYTES] = {};
  bool valid = false;
};

struct PdfLayoutWordIndexInfo {
  uint16_t sectionIndex = 0;
  uint16_t pageCount = 0;
  uint32_t firstGlobalWordOrdinal = 0;
  uint32_t sectionWordCount = 0;
};

class PdfLayoutWordIndexWriter {
 public:
  PdfStatus begin(PdfByteSink destination, uint16_t sectionIndex, uint32_t firstGlobalWordOrdinal,
                  uint32_t sectionWordCount);
  PdfStatus append(const PdfLayoutWordRange& range);
  PdfStatus finish();

  uint16_t pageCount() const { return pageCount_; }

 private:
  PdfStatus fail(PdfStatus status);

  PdfByteSink destination_{};
  PdfStatus status_{};
  uint32_t firstGlobalWordOrdinal_ = 0;
  uint32_t sectionWordCount_ = 0;
  uint32_t nextGlobalWordOrdinal_ = 0;
  uint32_t aggregateCrc_ = 0;
  uint16_t pageCount_ = 0;
  bool initialized_ = false;
  bool finished_ = false;
};

PdfStatus pdfInspectLayoutWordIndex(const PdfByteSource& source, PdfLayoutWordIndexInfo* info);
PdfStatus pdfReadLayoutWordRanges(const PdfByteSource& source, uint16_t firstPage, uint16_t count,
                                  PdfLayoutWordRange* ranges);
// The caller has already validated this exact source and retains the same
// open handle. This avoids re-reading the header/footer for every bounded
// record window during a saved-item lookup.
PdfStatus pdfReadValidatedLayoutWordRanges(const PdfByteSource& source, const PdfLayoutWordIndexInfo& info,
                                           uint16_t firstPage, uint16_t count, PdfLayoutWordRange* ranges);
PdfStatus pdfReadLayoutWordRange(const PdfByteSource& source, uint16_t page, PdfLayoutWordRange* range);
PdfStatus pdfFindLayoutPage(const PdfByteSource& source, uint32_t globalWordOrdinal, uint16_t* page,
                            PdfLayoutWordRange* range = nullptr);
PdfStatus pdfFindLayoutCursor(const PdfByteSource& source, uint32_t wordCursor, uint16_t* page,
                              PdfLayoutWordRange* range = nullptr);
PdfStatus pdfFindLayoutAnchor(const PdfByteSource& source, const char* blockAnchor, uint32_t blockWordOffset,
                              uint16_t* page, PdfLayoutWordRange* range = nullptr);
bool pdfCalculateWordProgress(uint32_t lastReachedWordOrdinal, uint32_t totalWords, float* progress);
bool pdfCalculateWordCursorProgress(uint32_t reachedWordCount, uint32_t totalWords, float* progress);
