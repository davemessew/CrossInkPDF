#pragma once

#include <cstddef>
#include <cstdint>

#include "PdfTypes.h"

inline constexpr size_t PDF_LAYOUT_WORD_ANCHOR_BYTES = 10;
inline constexpr size_t PDF_LAYOUT_WORD_INDEX_HEADER_BYTES = 32;
inline constexpr size_t PDF_LAYOUT_WORD_INDEX_RECORD_BYTES = 40;
inline constexpr size_t PDF_LAYOUT_WORD_INDEX_FOOTER_BYTES = 16;
inline constexpr size_t PDF_LAYOUT_CACHE_BINDING_TRAILER_BYTES = 16;

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
  uint32_t sectionCacheLength = 0;
  uint32_t sectionCacheToken = 0;
};

struct PdfLayoutCacheBinding {
  uint32_t length = 0;
  uint32_t token = 0;
};

struct PdfLayoutWordIndexPatchSink {
  using WriteAtFn = PdfStatus (*)(void* context, uint64_t offset, const uint8_t* source, size_t requested,
                                  size_t* bytesWritten);

  void* context = nullptr;
  WriteAtFn writeAt = nullptr;

  constexpr bool valid() const { return writeAt != nullptr; }
};

// PDF-only page-cache coordinates stored beside each semantic range. Keeping
// these fixed-width lets Section replay its unchanged v44 lookup-table tails
// without retaining the 8 KiB in-memory page LUT.
struct PdfLayoutPageRecord {
  uint32_t fileOffset = 0;
  uint16_t paragraphIndex = 0;
  uint16_t listItemIndex = 0;
};

static_assert(sizeof(PdfLayoutPageRecord) == 8, "PWI page records must remain fixed-width");

class PdfLayoutWordIndexWriter {
 public:
  PdfStatus begin(PdfByteSink destination, uint16_t sectionIndex, uint32_t firstGlobalWordOrdinal,
                  uint32_t sectionWordCount, const PdfLayoutCacheBinding& binding = {});
  PdfStatus append(const PdfLayoutWordRange& range);
  PdfStatus append(const PdfLayoutWordRange& range, const PdfLayoutPageRecord& page);
  PdfStatus finish();

  uint16_t pageCount() const { return pageCount_; }
  uint32_t pairToken() const { return finished_ ? (aggregateCrc_ == 0 ? 1U : aggregateCrc_) : 0; }

 private:
  PdfStatus fail(PdfStatus status);

  PdfByteSink destination_{};
  PdfStatus status_{};
  uint32_t firstGlobalWordOrdinal_ = 0;
  uint32_t sectionWordCount_ = 0;
  uint32_t nextGlobalWordOrdinal_ = 0;
  uint32_t aggregateCrc_ = 0;
  uint32_t lastFileOffset_ = 0;
  uint16_t pageCount_ = 0;
  bool initialized_ = false;
  bool finished_ = false;
};

PdfStatus pdfInspectLayoutWordIndex(const PdfByteSource& source, PdfLayoutWordIndexInfo* info);
PdfStatus pdfEncodeLayoutCacheBindingTrailer(
    const PdfLayoutCacheBinding& binding,
    uint8_t output[PDF_LAYOUT_CACHE_BINDING_TRAILER_BYTES]);
PdfStatus pdfComputeLayoutCacheBinding(const PdfByteSource& sectionCache, PdfLayoutCacheBinding* binding);
PdfStatus pdfBindLayoutWordIndex(const PdfByteSource& source, const PdfLayoutWordIndexPatchSink& patch,
                                 const PdfLayoutCacheBinding& binding);
bool pdfLayoutWordIndexMatchesSectionCache(const PdfLayoutWordIndexInfo& info,
                                           const PdfLayoutCacheBinding& binding);
PdfStatus pdfReadLayoutWordRanges(const PdfByteSource& source, uint16_t firstPage, uint16_t count,
                                  PdfLayoutWordRange* ranges);
// The caller has already validated this exact source and retains the same
// open handle. This avoids re-reading the header/footer for every bounded
// record window during a saved-item lookup.
PdfStatus pdfReadValidatedLayoutWordRanges(const PdfByteSource& source, const PdfLayoutWordIndexInfo& info,
                                           uint16_t firstPage, uint16_t count, PdfLayoutWordRange* ranges);
PdfStatus pdfReadValidatedLayoutPageRecords(const PdfByteSource& source, const PdfLayoutWordIndexInfo& info,
                                            uint16_t firstPage, uint16_t count, PdfLayoutPageRecord* pages);
PdfStatus pdfReadLayoutWordRange(const PdfByteSource& source, uint16_t page, PdfLayoutWordRange* range);
PdfStatus pdfFindLayoutPage(const PdfByteSource& source, uint32_t globalWordOrdinal, uint16_t* page,
                            PdfLayoutWordRange* range = nullptr);
PdfStatus pdfFindLayoutCursor(const PdfByteSource& source, uint32_t wordCursor, uint16_t* page,
                              PdfLayoutWordRange* range = nullptr);
PdfStatus pdfFindLayoutAnchor(const PdfByteSource& source, const char* blockAnchor, uint32_t blockWordOffset,
                              uint16_t* page, PdfLayoutWordRange* range = nullptr);
bool pdfCalculateWordProgress(uint32_t lastReachedWordOrdinal, uint32_t totalWords, float* progress);
bool pdfCalculateWordCursorProgress(uint32_t reachedWordCount, uint32_t totalWords, float* progress);
