#include "PdfSavedItemWordMap.h"

#include <algorithm>
#include <cstring>

namespace {

constexpr uint16_t kReadBatchRecords = 4;
constexpr uint8_t kKnownItemFlags =
    PDF_SAVED_ITEM_HAS_START_SEMANTIC | PDF_SAVED_ITEM_HAS_END_SEMANTIC | PDF_SAVED_ITEM_HAS_FALLBACK_PAGES;

size_t boundedLength(const char* const value, const size_t capacity) {
  size_t length = 0;
  while (length < capacity && value[length] != '\0') {
    ++length;
  }
  return length;
}

struct SemanticMatch {
  bool anchorFound = false;
  bool ordinalFound = false;
  uint32_t bestBlockOffset = 0;
  uint16_t anchorPage = 0;
  uint16_t ordinalPage = 0;
};

void consider(const PdfLayoutWordRange& candidate, const uint16_t page, const char* const anchor,
              const uint32_t blockOffset, const uint32_t globalOrdinal, SemanticMatch* const match) {
  if (!candidate.valid || globalOrdinal < candidate.firstGlobalWordOrdinal ||
      globalOrdinal > candidate.lastGlobalWordOrdinal) {
    return;
  }
  if (!match->ordinalFound) {
    match->ordinalFound = true;
    match->ordinalPage = page;
  }
  if (anchor[0] == '\0' || std::strncmp(candidate.blockAnchor, anchor, PDF_SAVED_ITEM_ANCHOR_BYTES) != 0 ||
      candidate.firstBlockWordOffset > blockOffset) {
    return;
  }
  if (!match->anchorFound || candidate.firstBlockWordOffset > match->bestBlockOffset) {
    match->anchorFound = true;
    match->bestBlockOffset = candidate.firstBlockWordOffset;
    match->anchorPage = page;
  }
}

bool selectPage(const SemanticMatch& match, uint16_t* const page) {
  if (match.anchorFound) {
    *page = match.anchorPage;
    return true;
  }
  if (match.ordinalFound) {
    *page = match.ordinalPage;
    return true;
  }
  return false;
}

}  // namespace

PdfStatus PdfSavedItemPageWordMapper::begin(const PdfSavedItem& item, const uint16_t sectionIndex,
                                            const uint32_t pageFirstOrdinal, const uint32_t pageLastOrdinal) {
  *this = {};
  const bool hasStart = (item.flags & PDF_SAVED_ITEM_HAS_START_SEMANTIC) != 0;
  const bool hasEnd = (item.flags & PDF_SAVED_ITEM_HAS_END_SEMANTIC) != 0;
  if (item.kind != PdfSavedItemKind::Clipping || item.sectionIndex != sectionIndex || !hasStart || !hasEnd ||
      item.endGlobalWordOrdinal < item.startGlobalWordOrdinal || pageLastOrdinal < pageFirstOrdinal) {
    return PdfStatus::failure(PdfError::InvalidArgument);
  }
  if (item.endGlobalWordOrdinal < pageFirstOrdinal || item.startGlobalWordOrdinal > pageLastOrdinal) {
    return PdfStatus::failure(PdfError::InvalidOffset);
  }
  pageFirstOrdinal_ = pageFirstOrdinal;
  pageLastOrdinal_ = pageLastOrdinal;
  itemFirstOrdinal_ = item.startGlobalWordOrdinal;
  itemLastOrdinal_ = item.endGlobalWordOrdinal;
  initialized_ = true;
  return PdfStatus::success();
}

void PdfSavedItemPageWordMapper::addWord(const uint16_t pageWordIndex, const bool attaches) {
  if (!initialized_ || invalid_) {
    return;
  }
  if (!hasOrdinal_) {
    currentOrdinal_ = pageFirstOrdinal_;
    hasOrdinal_ = true;
  } else if (!attaches) {
    if (currentOrdinal_ == UINT32_MAX) {
      invalid_ = true;
      return;
    }
    ++currentOrdinal_;
  }
  if (currentOrdinal_ > pageLastOrdinal_) {
    invalid_ = true;
    return;
  }
  if (currentOrdinal_ < itemFirstOrdinal_ || currentOrdinal_ > itemLastOrdinal_) {
    return;
  }
  if (!found_) {
    startWord_ = pageWordIndex;
    found_ = true;
  }
  endWord_ = pageWordIndex;
}

bool PdfSavedItemPageWordMapper::finish(PdfSavedItemPageWordRange* const range) const {
  if (range == nullptr || !initialized_ || invalid_ || !hasOrdinal_ || currentOrdinal_ != pageLastOrdinal_ ||
      !found_) {
    return false;
  }
  range->startWord = startWord_;
  range->endWord = endWord_;
  return true;
}

PdfStatus pdfMapSavedItemWordRange(const PdfSavedItemWordIndexSource& source, const PdfSavedItem& item,
                                   PdfSavedItemPageRange* const pages) {
  if (!source.valid() || pages == nullptr ||
      boundedLength(item.startBlockAnchor, sizeof(item.startBlockAnchor)) == sizeof(item.startBlockAnchor) ||
      boundedLength(item.endBlockAnchor, sizeof(item.endBlockAnchor)) == sizeof(item.endBlockAnchor)) {
    return PdfStatus::failure(PdfError::InvalidArgument);
  }
  if ((item.flags & ~kKnownItemFlags) != 0) {
    return PdfStatus::failure(PdfError::Malformed);
  }
  const bool hasStart = (item.flags & PDF_SAVED_ITEM_HAS_START_SEMANTIC) != 0;
  const bool hasEnd = (item.flags & PDF_SAVED_ITEM_HAS_END_SEMANTIC) != 0;
  if (!hasStart || (item.kind != PdfSavedItemKind::Bookmark && item.kind != PdfSavedItemKind::Clipping) ||
      (item.kind == PdfSavedItemKind::Clipping &&
       (!hasEnd || item.endGlobalWordOrdinal < item.startGlobalWordOrdinal)) ||
      (item.kind == PdfSavedItemKind::Bookmark && hasEnd)) {
    return PdfStatus::failure(PdfError::InvalidOffset);
  }

  PdfLayoutWordIndexInfo info;
  PdfStatus status = source.inspect(source.context, item.sectionIndex, &info);
  if (!status) {
    return status;
  }
  if (info.sectionIndex != item.sectionIndex || info.pageCount == 0) {
    return PdfStatus::failure(PdfError::Malformed);
  }
  const uint64_t sectionEnd =
      static_cast<uint64_t>(info.firstGlobalWordOrdinal) + static_cast<uint64_t>(info.sectionWordCount);
  if (item.startGlobalWordOrdinal < info.firstGlobalWordOrdinal ||
      static_cast<uint64_t>(item.startGlobalWordOrdinal) >= sectionEnd ||
      (item.kind == PdfSavedItemKind::Clipping && (item.endGlobalWordOrdinal < info.firstGlobalWordOrdinal ||
                                                   static_cast<uint64_t>(item.endGlobalWordOrdinal) >= sectionEnd))) {
    return PdfStatus::failure(PdfError::InvalidOffset, item.startGlobalWordOrdinal);
  }

  SemanticMatch startMatch;
  SemanticMatch endMatch;
  PdfLayoutWordRange ranges[kReadBatchRecords];
  for (uint32_t firstPage = 0; firstPage < info.pageCount;) {
    const uint32_t remaining = static_cast<uint32_t>(info.pageCount) - firstPage;
    const uint16_t count = static_cast<uint16_t>(std::min<uint32_t>(kReadBatchRecords, remaining));
    const uint16_t firstPageIndex = static_cast<uint16_t>(firstPage);
    status = source.readRanges(source.context, item.sectionIndex, firstPageIndex, count, ranges);
    if (!status) {
      return status;
    }
    for (uint16_t index = 0; index < count; ++index) {
      const uint16_t page = static_cast<uint16_t>(firstPage + index);
      consider(ranges[index], page, item.startBlockAnchor, item.startBlockWordOffset, item.startGlobalWordOrdinal,
               &startMatch);
      if (item.kind == PdfSavedItemKind::Clipping) {
        consider(ranges[index], page, item.endBlockAnchor, item.endBlockWordOffset, item.endGlobalWordOrdinal,
                 &endMatch);
      }
    }
    firstPage += count;
  }

  *pages = {};
  pages->sectionIndex = item.sectionIndex;
  pages->pageCount = info.pageCount;
  const bool exactStart = selectPage(startMatch, &pages->startPage);
  const bool exactEnd = item.kind == PdfSavedItemKind::Bookmark ? exactStart : selectPage(endMatch, &pages->endPage);
  if (item.kind == PdfSavedItemKind::Bookmark && exactStart) {
    pages->endPage = pages->startPage;
  }

  if (!exactStart || !exactEnd) {
    const bool canUseFallback =
        (item.flags & PDF_SAVED_ITEM_HAS_FALLBACK_PAGES) != 0 && source.layoutFingerprint != 0 &&
        item.fallbackLayoutFingerprint == source.layoutFingerprint && item.fallbackPageCount == info.pageCount &&
        item.fallbackStartPage <= item.fallbackEndPage && item.fallbackEndPage < info.pageCount;
    if (!canUseFallback) {
      return PdfStatus::failure(PdfError::InvalidOffset);
    }
    if (!exactStart) {
      pages->startPage = item.fallbackStartPage;
    }
    if (!exactEnd) {
      pages->endPage = item.fallbackEndPage;
    }
  }
  if (pages->startPage > pages->endPage) {
    return PdfStatus::failure(PdfError::Malformed);
  }
  pages->exact = exactStart && exactEnd;
  return PdfStatus::success();
}
