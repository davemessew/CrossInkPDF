#include "PdfLayoutWordIndex.h"

#include <algorithm>
#include <cstring>
#include <limits>

#include "PdfCacheFormat.h"
#include "PdfIo.h"

namespace {

constexpr uint8_t kHeaderMagic[] = {'P', 'W', 'I', 'H'};
constexpr uint8_t kFooterMagic[] = {'P', 'W', 'I', 'F'};
constexpr uint8_t kBindingTrailerMagic[] = {'P', 'W', 'I', 'B'};
constexpr uint16_t kVersion = 3;
constexpr uint8_t kRangeValid = 1U;
constexpr uint16_t kReadBatchRecords = 4;

void putU16(uint8_t* const destination, const uint16_t value) {
  destination[0] = static_cast<uint8_t>(value);
  destination[1] = static_cast<uint8_t>(value >> 8U);
}

void putU32(uint8_t* const destination, const uint32_t value) {
  destination[0] = static_cast<uint8_t>(value);
  destination[1] = static_cast<uint8_t>(value >> 8U);
  destination[2] = static_cast<uint8_t>(value >> 16U);
  destination[3] = static_cast<uint8_t>(value >> 24U);
}

uint16_t getU16(const uint8_t* const source) {
  return static_cast<uint16_t>(source[0]) | static_cast<uint16_t>(source[1]) << 8U;
}

uint32_t getU32(const uint8_t* const source) {
  return static_cast<uint32_t>(source[0]) | static_cast<uint32_t>(source[1]) << 8U |
         static_cast<uint32_t>(source[2]) << 16U | static_cast<uint32_t>(source[3]) << 24U;
}

size_t boundedLength(const char* const value, const size_t capacity) {
  if (value == nullptr) {
    return 0;
  }
  size_t length = 0;
  while (length < capacity && value[length] != '\0') {
    ++length;
  }
  return length;
}

PdfStatus encodeHeader(const uint16_t sectionIndex, const uint32_t firstGlobalWordOrdinal,
                       const uint32_t sectionWordCount, const PdfLayoutCacheBinding& binding,
                       uint8_t output[PDF_LAYOUT_WORD_INDEX_HEADER_BYTES]) {
  if (sectionWordCount > std::numeric_limits<uint32_t>::max() - firstGlobalWordOrdinal) {
    return PdfStatus::failure(PdfError::LimitExceeded);
  }
  std::memset(output, 0, PDF_LAYOUT_WORD_INDEX_HEADER_BYTES);
  std::memcpy(output, kHeaderMagic, sizeof(kHeaderMagic));
  putU16(output + 4, kVersion);
  putU16(output + 6, PDF_LAYOUT_WORD_INDEX_HEADER_BYTES);
  putU16(output + 8, sectionIndex);
  putU16(output + 10, PDF_LAYOUT_WORD_INDEX_RECORD_BYTES);
  putU32(output + 12, firstGlobalWordOrdinal);
  putU32(output + 16, sectionWordCount);
  putU32(output + 20, binding.length);
  putU32(output + 24, binding.token);
  putU32(output + 28, pdfCacheCrc32(output, 28));
  return PdfStatus::success();
}

PdfStatus decodeHeader(const uint8_t input[PDF_LAYOUT_WORD_INDEX_HEADER_BYTES], PdfLayoutWordIndexInfo* const info) {
  if (std::memcmp(input, kHeaderMagic, sizeof(kHeaderMagic)) != 0 || getU16(input + 4) != kVersion ||
      getU16(input + 6) != PDF_LAYOUT_WORD_INDEX_HEADER_BYTES ||
      getU16(input + 10) != PDF_LAYOUT_WORD_INDEX_RECORD_BYTES || getU32(input + 28) != pdfCacheCrc32(input, 28)) {
    return PdfStatus::failure(PdfError::Malformed);
  }
  info->sectionIndex = getU16(input + 8);
  info->firstGlobalWordOrdinal = getU32(input + 12);
  info->sectionWordCount = getU32(input + 16);
  info->sectionCacheLength = getU32(input + 20);
  info->sectionCacheToken = getU32(input + 24);
  if (info->sectionWordCount > std::numeric_limits<uint32_t>::max() - info->firstGlobalWordOrdinal) {
    return PdfStatus::failure(PdfError::Malformed);
  }
  return PdfStatus::success();
}

PdfStatus encodeRange(const PdfLayoutWordRange& range, const uint32_t cursor, const PdfLayoutPageRecord& page,
                      uint8_t output[PDF_LAYOUT_WORD_INDEX_RECORD_BYTES]) {
  if (page.fileOffset == 0) {
    return PdfStatus::failure(PdfError::InvalidOffset);
  }
  std::memset(output, 0, PDF_LAYOUT_WORD_INDEX_RECORD_BYTES);
  const size_t anchorLength = boundedLength(range.blockAnchor, sizeof(range.blockAnchor));
  if (anchorLength == sizeof(range.blockAnchor)) {
    return PdfStatus::failure(PdfError::LimitExceeded);
  }
  if (range.valid) {
    if (range.lastGlobalWordOrdinal < range.firstGlobalWordOrdinal) {
      return PdfStatus::failure(PdfError::Malformed);
    }
    putU32(output, range.firstGlobalWordOrdinal);
    putU32(output + 4, range.lastGlobalWordOrdinal);
    putU32(output + 8, range.firstBlockWordOffset);
    std::memcpy(output + 12, range.blockAnchor, anchorLength);
    output[22] = kRangeValid;
    output[23] = static_cast<uint8_t>(anchorLength);
  } else {
    // Empty rendered pages retain the current ordinal as a search cursor but do not
    // claim that a semantic word was reached.
    putU32(output, cursor);
    putU32(output + 4, cursor);
  }
  putU32(output + 28, page.fileOffset);
  putU16(output + 32, page.paragraphIndex);
  putU16(output + 34, page.listItemIndex);
  putU32(output + 36, pdfCacheCrc32(output, 36));
  return PdfStatus::success();
}

PdfStatus decodeRange(const uint8_t input[PDF_LAYOUT_WORD_INDEX_RECORD_BYTES], PdfLayoutWordRange* const range,
                      uint32_t* const cursor, PdfLayoutPageRecord* const page = nullptr) {
  if (getU32(input + 24) != 0 || getU32(input + 36) != pdfCacheCrc32(input, 36) ||
      (input[22] & static_cast<uint8_t>(~kRangeValid)) != 0 || input[23] >= PDF_LAYOUT_WORD_ANCHOR_BYTES) {
    return PdfStatus::failure(PdfError::Malformed);
  }
  const uint32_t fileOffset = getU32(input + 28);
  if (fileOffset == 0) {
    return PdfStatus::failure(PdfError::Malformed);
  }
  if (page != nullptr) {
    *page = {fileOffset, getU16(input + 32), getU16(input + 34)};
  }
  const uint32_t first = getU32(input);
  const uint32_t last = getU32(input + 4);
  if (cursor != nullptr) {
    *cursor = first;
  }
  *range = {};
  range->wordCursor = first;
  if ((input[22] & kRangeValid) == 0) {
    if (first != last || input[23] != 0) {
      return PdfStatus::failure(PdfError::Malformed);
    }
    for (size_t index = 12; index < 22; ++index) {
      if (input[index] != 0) {
        return PdfStatus::failure(PdfError::Malformed);
      }
    }
    return PdfStatus::success();
  }
  if (last < first || input[12 + input[23]] != '\0') {
    return PdfStatus::failure(PdfError::Malformed);
  }
  range->firstGlobalWordOrdinal = first;
  range->lastGlobalWordOrdinal = last;
  range->firstBlockWordOffset = getU32(input + 8);
  range->wordCursor = last + 1U;
  std::memcpy(range->blockAnchor, input + 12, input[23]);
  range->valid = true;
  return PdfStatus::success();
}

PdfStatus readHeaderAndFooter(const PdfByteSource& source, PdfLayoutWordIndexInfo* const info,
                              uint32_t* const expectedAggregateCrc, uint32_t* const initialAggregateCrc = nullptr) {
  if (!source.valid() || info == nullptr ||
      source.size < PDF_LAYOUT_WORD_INDEX_HEADER_BYTES + PDF_LAYOUT_WORD_INDEX_FOOTER_BYTES) {
    return PdfStatus::failure(PdfError::InvalidArgument);
  }
  uint8_t header[PDF_LAYOUT_WORD_INDEX_HEADER_BYTES];
  PdfStatus status = pdfReadExact(source, 0, header, sizeof(header));
  if (!status) {
    return status;
  }
  status = decodeHeader(header, info);
  if (!status) {
    return status;
  }
  uint8_t footer[PDF_LAYOUT_WORD_INDEX_FOOTER_BYTES];
  status = pdfReadExact(source, source.size - sizeof(footer), footer, sizeof(footer));
  if (!status) {
    return status;
  }
  if (std::memcmp(footer, kFooterMagic, sizeof(kFooterMagic)) != 0 ||
      getU16(footer + 6) != PDF_LAYOUT_WORD_INDEX_FOOTER_BYTES || getU32(footer + 12) != pdfCacheCrc32(footer, 12)) {
    return PdfStatus::failure(PdfError::Malformed);
  }
  info->pageCount = getU16(footer + 4);
  const uint64_t expectedSize = PDF_LAYOUT_WORD_INDEX_HEADER_BYTES +
                                static_cast<uint64_t>(info->pageCount) * PDF_LAYOUT_WORD_INDEX_RECORD_BYTES +
                                PDF_LAYOUT_WORD_INDEX_FOOTER_BYTES;
  if (source.size != expectedSize) {
    return PdfStatus::failure(PdfError::Malformed);
  }
  if (expectedAggregateCrc != nullptr) {
    *expectedAggregateCrc = getU32(footer + 8);
  }
  if (initialAggregateCrc != nullptr) {
    *initialAggregateCrc = pdfCacheCrc32(header, sizeof(header));
  }
  return PdfStatus::success();
}

PdfStatus readEncodedRangeBatch(const PdfByteSource& source, const uint16_t firstPage, const uint16_t count,
                                uint8_t* const encoded) {
  if (encoded == nullptr || count == 0 || count > kReadBatchRecords) {
    return PdfStatus::failure(PdfError::InvalidArgument);
  }
  const uint64_t offset =
      PDF_LAYOUT_WORD_INDEX_HEADER_BYTES + static_cast<uint64_t>(firstPage) * PDF_LAYOUT_WORD_INDEX_RECORD_BYTES;
  return pdfReadExact(source, offset, encoded, static_cast<size_t>(count) * PDF_LAYOUT_WORD_INDEX_RECORD_BYTES);
}

PdfStatus writeAtExact(const PdfLayoutWordIndexPatchSink& patch, const uint64_t offset, const uint8_t* const source,
                       const size_t length) {
  if (!patch.valid() || source == nullptr) {
    return PdfStatus::failure(PdfError::InvalidArgument, offset);
  }
  size_t written = 0;
  const PdfStatus status = patch.writeAt(patch.context, offset, source, length, &written);
  if (!status) {
    return status;
  }
  return written == length ? PdfStatus::success() : PdfStatus::failure(PdfError::IoFailure, offset + written);
}

bool rangeFollowsCursor(const PdfLayoutWordRange& range, const uint32_t sectionStart, const uint32_t nextOrdinal,
                        const uint64_t sectionEnd) {
  if (!range.valid || range.firstGlobalWordOrdinal < sectionStart ||
      static_cast<uint64_t>(range.lastGlobalWordOrdinal) >= sectionEnd) {
    return false;
  }
  const bool startsAtCursor = range.firstGlobalWordOrdinal == nextOrdinal;
  const bool continuesOnePriorWord = nextOrdinal > sectionStart && range.firstGlobalWordOrdinal == nextOrdinal - 1U;
  return startsAtCursor || continuesOnePriorWord;
}

}  // namespace

PdfStatus PdfLayoutWordIndexWriter::fail(const PdfStatus status) {
  if (status_.ok()) {
    status_ = status;
  }
  return status_;
}

PdfStatus PdfLayoutWordIndexWriter::begin(const PdfByteSink destination, const uint16_t sectionIndex,
                                          const uint32_t firstGlobalWordOrdinal, const uint32_t sectionWordCount,
                                          const PdfLayoutCacheBinding& binding) {
  *this = {};
  if (!destination.valid()) {
    return fail(PdfStatus::failure(PdfError::InvalidArgument));
  }
  uint8_t header[PDF_LAYOUT_WORD_INDEX_HEADER_BYTES];
  PdfStatus status = encodeHeader(sectionIndex, firstGlobalWordOrdinal, sectionWordCount, binding, header);
  if (status) {
    status = pdfWriteExact(destination, header, sizeof(header));
  }
  if (!status) {
    return fail(status);
  }
  destination_ = destination;
  firstGlobalWordOrdinal_ = firstGlobalWordOrdinal;
  sectionWordCount_ = sectionWordCount;
  nextGlobalWordOrdinal_ = firstGlobalWordOrdinal;
  aggregateCrc_ = pdfCacheCrc32(header, sizeof(header));
  initialized_ = true;
  return PdfStatus::success();
}

PdfStatus PdfLayoutWordIndexWriter::append(const PdfLayoutWordRange& range) {
  return append(range, {static_cast<uint32_t>(pageCount_) + 1U, 0, 0});
}

PdfStatus PdfLayoutWordIndexWriter::append(const PdfLayoutWordRange& range, const PdfLayoutPageRecord& page) {
  if (!status_ || !initialized_ || finished_) {
    return fail(PdfStatus::failure(PdfError::InvalidArgument));
  }
  if (pageCount_ == UINT16_MAX) {
    return fail(PdfStatus::failure(PdfError::LimitExceeded));
  }
  if (page.fileOffset == 0 || (pageCount_ != 0 && page.fileOffset <= lastFileOffset_)) {
    return fail(PdfStatus::failure(PdfError::Malformed, pageCount_));
  }
  const uint64_t sectionEnd = static_cast<uint64_t>(firstGlobalWordOrdinal_) + static_cast<uint64_t>(sectionWordCount_);
  if (range.valid && !rangeFollowsCursor(range, firstGlobalWordOrdinal_, nextGlobalWordOrdinal_, sectionEnd)) {
    return fail(PdfStatus::failure(PdfError::Malformed, pageCount_));
  }
  uint8_t encoded[PDF_LAYOUT_WORD_INDEX_RECORD_BYTES];
  PdfStatus status = encodeRange(range, nextGlobalWordOrdinal_, page, encoded);
  if (status) {
    status = pdfWriteExact(destination_, encoded, sizeof(encoded));
  }
  if (!status) {
    return fail(status);
  }
  // Aggregate the record payload, not the payload plus its local CRC. CRCs
  // have a fixed residue when their own checksum is appended, so hashing the
  // complete record would not detect a payload whose local CRC was rewritten.
  aggregateCrc_ = pdfCacheCrc32(encoded, 36, aggregateCrc_);
  if (range.valid) {
    nextGlobalWordOrdinal_ = std::max(nextGlobalWordOrdinal_, range.lastGlobalWordOrdinal + 1U);
  }
  lastFileOffset_ = page.fileOffset;
  ++pageCount_;
  return PdfStatus::success();
}

PdfStatus PdfLayoutWordIndexWriter::finish() {
  if (!status_ || !initialized_ || finished_) {
    return fail(PdfStatus::failure(PdfError::InvalidArgument));
  }
  const uint64_t sectionEnd = static_cast<uint64_t>(firstGlobalWordOrdinal_) + static_cast<uint64_t>(sectionWordCount_);
  if (nextGlobalWordOrdinal_ != sectionEnd) {
    return fail(PdfStatus::failure(PdfError::Malformed, nextGlobalWordOrdinal_));
  }
  uint8_t footer[PDF_LAYOUT_WORD_INDEX_FOOTER_BYTES]{};
  std::memcpy(footer, kFooterMagic, sizeof(kFooterMagic));
  putU16(footer + 4, pageCount_);
  putU16(footer + 6, PDF_LAYOUT_WORD_INDEX_FOOTER_BYTES);
  putU32(footer + 8, aggregateCrc_);
  putU32(footer + 12, pdfCacheCrc32(footer, 12));
  const PdfStatus status = pdfWriteExact(destination_, footer, sizeof(footer));
  if (!status) {
    return fail(status);
  }
  finished_ = true;
  return PdfStatus::success();
}

PdfStatus pdfInspectLayoutWordIndex(const PdfByteSource& source, PdfLayoutWordIndexInfo* const info) {
  uint32_t expectedAggregateCrc = 0;
  uint32_t aggregateCrc = 0;
  PdfStatus status = readHeaderAndFooter(source, info, &expectedAggregateCrc, &aggregateCrc);
  if (!status) {
    return status;
  }
  uint32_t nextOrdinal = info->firstGlobalWordOrdinal;
  uint32_t lastFileOffset = 0;
  const uint64_t sectionEnd =
      static_cast<uint64_t>(info->firstGlobalWordOrdinal) + static_cast<uint64_t>(info->sectionWordCount);
  uint8_t encoded[PDF_LAYOUT_WORD_INDEX_RECORD_BYTES * kReadBatchRecords];
  for (uint16_t firstPage = 0; firstPage < info->pageCount;) {
    const uint16_t count = std::min<uint16_t>(kReadBatchRecords, static_cast<uint16_t>(info->pageCount - firstPage));
    status = readEncodedRangeBatch(source, firstPage, count, encoded);
    if (!status) {
      return status;
    }
    for (uint16_t index = 0; index < count; ++index) {
      const uint8_t* const record = encoded + static_cast<size_t>(index) * PDF_LAYOUT_WORD_INDEX_RECORD_BYTES;
      PdfLayoutWordRange range;
      PdfLayoutPageRecord page;
      uint32_t cursor = 0;
      status = decodeRange(record, &range, &cursor, &page);
      if (!status) {
        return status;
      }
      aggregateCrc = pdfCacheCrc32(record, 36, aggregateCrc);
      if ((!range.valid && cursor != nextOrdinal) ||
          (range.valid && !rangeFollowsCursor(range, info->firstGlobalWordOrdinal, nextOrdinal, sectionEnd)) ||
          page.fileOffset <= lastFileOffset) {
        return PdfStatus::failure(PdfError::Malformed, firstPage + index);
      }
      lastFileOffset = page.fileOffset;
      if (range.valid) {
        nextOrdinal = std::max(nextOrdinal, range.lastGlobalWordOrdinal + 1U);
      }
    }
    firstPage = static_cast<uint16_t>(firstPage + count);
  }
  if (nextOrdinal != sectionEnd || aggregateCrc != expectedAggregateCrc) {
    return PdfStatus::failure(PdfError::Malformed);
  }
  return PdfStatus::success();
}

PdfStatus pdfEncodeLayoutCacheBindingTrailer(
    const PdfLayoutCacheBinding& binding,
    uint8_t output[PDF_LAYOUT_CACHE_BINDING_TRAILER_BYTES]) {
  if (binding.length == 0 || binding.token == 0 || output == nullptr) {
    return PdfStatus::failure(PdfError::InvalidArgument);
  }
  std::memset(output, 0, PDF_LAYOUT_CACHE_BINDING_TRAILER_BYTES);
  std::memcpy(output, kBindingTrailerMagic, sizeof(kBindingTrailerMagic));
  putU32(output + 4, binding.length);
  putU32(output + 8, binding.token);
  putU32(output + 12, pdfCacheCrc32(output, 12));
  return PdfStatus::success();
}

PdfStatus pdfComputeLayoutCacheBinding(const PdfByteSource& sectionCache, PdfLayoutCacheBinding* const binding) {
  constexpr uint64_t kMaximumBoundCacheBytes =
      static_cast<uint64_t>(UINT32_MAX) + PDF_LAYOUT_CACHE_BINDING_TRAILER_BYTES;
  if (!sectionCache.valid() || binding == nullptr ||
      sectionCache.size < PDF_LAYOUT_CACHE_BINDING_TRAILER_BYTES || sectionCache.size > kMaximumBoundCacheBytes) {
    return PdfStatus::failure(PdfError::InvalidArgument);
  }
  *binding = {};
  uint8_t trailer[PDF_LAYOUT_CACHE_BINDING_TRAILER_BYTES];
  const PdfStatus status =
      pdfReadExact(sectionCache, sectionCache.size - sizeof(trailer), trailer, sizeof(trailer));
  if (!status) {
    return status;
  }
  const uint32_t length = getU32(trailer + 4);
  const uint32_t token = getU32(trailer + 8);
  if (std::memcmp(trailer, kBindingTrailerMagic, sizeof(kBindingTrailerMagic)) != 0 || length == 0 || token == 0 ||
      getU32(trailer + 12) != pdfCacheCrc32(trailer, 12) ||
      static_cast<uint64_t>(length) + sizeof(trailer) != sectionCache.size) {
    return PdfStatus::failure(PdfError::Malformed, sectionCache.size - sizeof(trailer));
  }
  binding->length = length;
  binding->token = token;
  return PdfStatus::success();
}

PdfStatus pdfBindLayoutWordIndex(const PdfByteSource& source, const PdfLayoutWordIndexPatchSink& patch,
                                 const PdfLayoutCacheBinding& binding) {
  if (!source.valid() || !patch.valid() || binding.length == 0 || binding.token == 0) {
    return PdfStatus::failure(PdfError::InvalidArgument);
  }
  PdfLayoutWordIndexInfo info;
  PdfStatus status = pdfInspectLayoutWordIndex(source, &info);
  if (!status) {
    return status;
  }

  uint8_t header[PDF_LAYOUT_WORD_INDEX_HEADER_BYTES];
  status = encodeHeader(info.sectionIndex, info.firstGlobalWordOrdinal, info.sectionWordCount, binding, header);
  if (!status) {
    return status;
  }
  uint32_t aggregateCrc = pdfCacheCrc32(header, sizeof(header));
  uint8_t encoded[PDF_LAYOUT_WORD_INDEX_RECORD_BYTES * kReadBatchRecords];
  for (uint16_t firstPage = 0; firstPage < info.pageCount;) {
    const uint16_t count = std::min<uint16_t>(kReadBatchRecords, static_cast<uint16_t>(info.pageCount - firstPage));
    status = readEncodedRangeBatch(source, firstPage, count, encoded);
    if (!status) {
      return status;
    }
    for (uint16_t index = 0; index < count; ++index) {
      aggregateCrc = pdfCacheCrc32(encoded + static_cast<size_t>(index) * PDF_LAYOUT_WORD_INDEX_RECORD_BYTES, 36,
                                  aggregateCrc);
    }
    firstPage = static_cast<uint16_t>(firstPage + count);
  }

  uint8_t footer[PDF_LAYOUT_WORD_INDEX_FOOTER_BYTES]{};
  std::memcpy(footer, kFooterMagic, sizeof(kFooterMagic));
  putU16(footer + 4, info.pageCount);
  putU16(footer + 6, PDF_LAYOUT_WORD_INDEX_FOOTER_BYTES);
  putU32(footer + 8, aggregateCrc);
  putU32(footer + 12, pdfCacheCrc32(footer, 12));
  status = writeAtExact(patch, 0, header, sizeof(header));
  if (!status) {
    return status;
  }
  return writeAtExact(patch, source.size - sizeof(footer), footer, sizeof(footer));
}

bool pdfLayoutWordIndexMatchesSectionCache(const PdfLayoutWordIndexInfo& info,
                                           const PdfLayoutCacheBinding& binding) {
  return binding.length != 0 && binding.token != 0 && info.sectionCacheLength == binding.length &&
         info.sectionCacheToken == binding.token;
}

PdfStatus pdfReadLayoutWordRanges(const PdfByteSource& source, const uint16_t firstPage, const uint16_t count,
                                  PdfLayoutWordRange* const ranges) {
  if (ranges == nullptr || count == 0) {
    return PdfStatus::failure(PdfError::InvalidArgument);
  }
  PdfLayoutWordIndexInfo info;
  PdfStatus status = readHeaderAndFooter(source, &info, nullptr);
  if (!status) {
    return status;
  }
  if (firstPage >= info.pageCount || count > static_cast<uint16_t>(info.pageCount - firstPage)) {
    return PdfStatus::failure(PdfError::InvalidOffset, firstPage);
  }
  uint8_t encoded[PDF_LAYOUT_WORD_INDEX_RECORD_BYTES * kReadBatchRecords];
  for (uint16_t decoded = 0; decoded < count;) {
    const uint16_t batchCount = std::min<uint16_t>(kReadBatchRecords, static_cast<uint16_t>(count - decoded));
    status = readEncodedRangeBatch(source, static_cast<uint16_t>(firstPage + decoded), batchCount, encoded);
    if (!status) {
      return status;
    }
    for (uint16_t index = 0; index < batchCount; ++index) {
      status = decodeRange(encoded + static_cast<size_t>(index) * PDF_LAYOUT_WORD_INDEX_RECORD_BYTES,
                           &ranges[decoded + index], nullptr);
      if (!status) {
        return status;
      }
    }
    decoded = static_cast<uint16_t>(decoded + batchCount);
  }
  return PdfStatus::success();
}

PdfStatus pdfReadValidatedLayoutWordRanges(const PdfByteSource& source, const PdfLayoutWordIndexInfo& info,
                                           const uint16_t firstPage, const uint16_t count,
                                           PdfLayoutWordRange* const ranges) {
  if (!source.valid() || ranges == nullptr || count == 0 || firstPage >= info.pageCount ||
      count > static_cast<uint16_t>(info.pageCount - firstPage)) {
    return PdfStatus::failure(PdfError::InvalidArgument, firstPage);
  }

  uint8_t encoded[PDF_LAYOUT_WORD_INDEX_RECORD_BYTES * kReadBatchRecords];
  for (uint16_t decoded = 0; decoded < count;) {
    const uint16_t batchCount = std::min<uint16_t>(kReadBatchRecords, static_cast<uint16_t>(count - decoded));
    PdfStatus status =
        readEncodedRangeBatch(source, static_cast<uint16_t>(firstPage + decoded), batchCount, encoded);
    if (!status) {
      return status;
    }
    for (uint16_t index = 0; index < batchCount; ++index) {
      status = decodeRange(encoded + static_cast<size_t>(index) * PDF_LAYOUT_WORD_INDEX_RECORD_BYTES,
                           &ranges[decoded + index], nullptr);
      if (!status) {
        return status;
      }
    }
    decoded = static_cast<uint16_t>(decoded + batchCount);
  }
  return PdfStatus::success();
}

PdfStatus pdfReadValidatedLayoutPageRecords(const PdfByteSource& source, const PdfLayoutWordIndexInfo& info,
                                            const uint16_t firstPage, const uint16_t count,
                                            PdfLayoutPageRecord* const pages) {
  if (!source.valid() || pages == nullptr || count == 0 || count > kReadBatchRecords || firstPage >= info.pageCount ||
      count > static_cast<uint16_t>(info.pageCount - firstPage)) {
    return PdfStatus::failure(PdfError::InvalidArgument, firstPage);
  }
  uint8_t encoded[PDF_LAYOUT_WORD_INDEX_RECORD_BYTES * kReadBatchRecords];
  PdfStatus status = readEncodedRangeBatch(source, firstPage, count, encoded);
  if (!status) {
    return status;
  }
  uint32_t priorOffset = 0;
  for (uint16_t index = 0; index < count; ++index) {
    PdfLayoutWordRange ignored;
    status = decodeRange(encoded + static_cast<size_t>(index) * PDF_LAYOUT_WORD_INDEX_RECORD_BYTES, &ignored,
                         nullptr, &pages[index]);
    if (!status || pages[index].fileOffset <= priorOffset) {
      return status ? PdfStatus::failure(PdfError::Malformed, firstPage + index) : status;
    }
    priorOffset = pages[index].fileOffset;
  }
  return PdfStatus::success();
}

PdfStatus pdfReadLayoutWordRange(const PdfByteSource& source, const uint16_t page, PdfLayoutWordRange* const range) {
  return pdfReadLayoutWordRanges(source, page, 1, range);
}

PdfStatus pdfFindLayoutPage(const PdfByteSource& source, const uint32_t globalWordOrdinal, uint16_t* const page,
                            PdfLayoutWordRange* const range) {
  if (page == nullptr) {
    return PdfStatus::failure(PdfError::InvalidArgument);
  }
  PdfLayoutWordIndexInfo info;
  PdfStatus status = readHeaderAndFooter(source, &info, nullptr);
  if (!status) {
    return status;
  }
  const uint64_t sectionEnd =
      static_cast<uint64_t>(info.firstGlobalWordOrdinal) + static_cast<uint64_t>(info.sectionWordCount);
  if (globalWordOrdinal < info.firstGlobalWordOrdinal || globalWordOrdinal >= sectionEnd) {
    return PdfStatus::failure(PdfError::InvalidOffset, globalWordOrdinal);
  }

  // Resume is infrequent. A bounded fixed-record scan handles deliberately empty
  // rendered pages without keeping an 8 KiB second LUT in RAM.
  uint8_t encoded[PDF_LAYOUT_WORD_INDEX_RECORD_BYTES * kReadBatchRecords];
  for (uint16_t firstPage = 0; firstPage < info.pageCount;) {
    const uint16_t count = std::min<uint16_t>(kReadBatchRecords, static_cast<uint16_t>(info.pageCount - firstPage));
    status = readEncodedRangeBatch(source, firstPage, count, encoded);
    if (!status) {
      return status;
    }
    for (uint16_t index = 0; index < count; ++index) {
      PdfLayoutWordRange candidate;
      status =
          decodeRange(encoded + static_cast<size_t>(index) * PDF_LAYOUT_WORD_INDEX_RECORD_BYTES, &candidate, nullptr);
      if (!status) {
        return status;
      }
      if (candidate.valid && globalWordOrdinal >= candidate.firstGlobalWordOrdinal &&
          globalWordOrdinal <= candidate.lastGlobalWordOrdinal) {
        *page = static_cast<uint16_t>(firstPage + index);
        if (range != nullptr) {
          *range = candidate;
        }
        return PdfStatus::success();
      }
    }
    firstPage = static_cast<uint16_t>(firstPage + count);
  }
  return PdfStatus::failure(PdfError::Malformed, globalWordOrdinal);
}

PdfStatus pdfFindLayoutCursor(const PdfByteSource& source, const uint32_t wordCursor, uint16_t* const page,
                              PdfLayoutWordRange* const range) {
  if (page == nullptr) {
    return PdfStatus::failure(PdfError::InvalidArgument);
  }
  PdfLayoutWordIndexInfo info;
  PdfStatus status = readHeaderAndFooter(source, &info, nullptr);
  if (!status) {
    return status;
  }
  const uint64_t sectionEnd =
      static_cast<uint64_t>(info.firstGlobalWordOrdinal) + static_cast<uint64_t>(info.sectionWordCount);
  if (wordCursor < info.firstGlobalWordOrdinal || static_cast<uint64_t>(wordCursor) > sectionEnd) {
    return PdfStatus::failure(PdfError::InvalidOffset, wordCursor);
  }

  bool fallbackFound = false;
  uint16_t fallbackPage = 0;
  PdfLayoutWordRange fallbackRange;
  uint8_t encoded[PDF_LAYOUT_WORD_INDEX_RECORD_BYTES * kReadBatchRecords];
  for (uint16_t firstPage = 0; firstPage < info.pageCount;) {
    const uint16_t count = std::min<uint16_t>(kReadBatchRecords, static_cast<uint16_t>(info.pageCount - firstPage));
    status = readEncodedRangeBatch(source, firstPage, count, encoded);
    if (!status) {
      return status;
    }
    for (uint16_t index = 0; index < count; ++index) {
      PdfLayoutWordRange candidate;
      status =
          decodeRange(encoded + static_cast<size_t>(index) * PDF_LAYOUT_WORD_INDEX_RECORD_BYTES, &candidate, nullptr);
      if (!status) {
        return status;
      }
      if (!candidate.valid && candidate.wordCursor == wordCursor) {
        *page = static_cast<uint16_t>(firstPage + index);
        if (range != nullptr) {
          *range = candidate;
        }
        return PdfStatus::success();
      }
      if (!candidate.valid) {
        continue;
      }
      const uint16_t candidatePage = static_cast<uint16_t>(firstPage + index);
      if (static_cast<uint64_t>(wordCursor) == sectionEnd) {
        // With no exact trailing empty-page record, the document-end cursor
        // belongs to the final valid rendered page.
        fallbackFound = true;
        fallbackPage = candidatePage;
        fallbackRange = candidate;
      } else if (!fallbackFound && wordCursor >= candidate.firstGlobalWordOrdinal &&
                 wordCursor <= candidate.lastGlobalWordOrdinal) {
        // A middle cursor is the count of words already reached, so its next
        // global ordinal is numerically the cursor itself.
        fallbackFound = true;
        fallbackPage = candidatePage;
        fallbackRange = candidate;
      }
    }
    firstPage = static_cast<uint16_t>(firstPage + count);
  }
  if (fallbackFound) {
    *page = fallbackPage;
    if (range != nullptr) {
      *range = fallbackRange;
    }
    return PdfStatus::success();
  }
  return PdfStatus::failure(PdfError::InvalidOffset, wordCursor);
}

PdfStatus pdfFindLayoutAnchor(const PdfByteSource& source, const char* const blockAnchor,
                              const uint32_t blockWordOffset, uint16_t* const page, PdfLayoutWordRange* const range) {
  if (blockAnchor == nullptr || blockAnchor[0] == '\0' || page == nullptr ||
      boundedLength(blockAnchor, PDF_LAYOUT_WORD_ANCHOR_BYTES) == PDF_LAYOUT_WORD_ANCHOR_BYTES) {
    return PdfStatus::failure(PdfError::InvalidArgument);
  }
  PdfLayoutWordIndexInfo info;
  PdfStatus status = readHeaderAndFooter(source, &info, nullptr);
  if (!status) {
    return status;
  }
  bool found = false;
  PdfLayoutWordRange selected;
  uint16_t selectedPage = 0;
  uint8_t encoded[PDF_LAYOUT_WORD_INDEX_RECORD_BYTES * kReadBatchRecords];
  for (uint16_t firstPage = 0; firstPage < info.pageCount;) {
    const uint16_t count = std::min<uint16_t>(kReadBatchRecords, static_cast<uint16_t>(info.pageCount - firstPage));
    status = readEncodedRangeBatch(source, firstPage, count, encoded);
    if (!status) {
      return status;
    }
    for (uint16_t index = 0; index < count; ++index) {
      PdfLayoutWordRange candidate;
      status =
          decodeRange(encoded + static_cast<size_t>(index) * PDF_LAYOUT_WORD_INDEX_RECORD_BYTES, &candidate, nullptr);
      if (!status) {
        return status;
      }
      if (!candidate.valid || std::strcmp(candidate.blockAnchor, blockAnchor) != 0 ||
          candidate.firstBlockWordOffset > blockWordOffset) {
        continue;
      }
      if (!found || candidate.firstBlockWordOffset > selected.firstBlockWordOffset) {
        found = true;
        selected = candidate;
        selectedPage = static_cast<uint16_t>(firstPage + index);
      }
    }
    firstPage = static_cast<uint16_t>(firstPage + count);
  }
  if (!found) {
    return PdfStatus::failure(PdfError::InvalidOffset, blockWordOffset);
  }
  *page = selectedPage;
  if (range != nullptr) {
    *range = selected;
  }
  return PdfStatus::success();
}

bool pdfCalculateWordProgress(const uint32_t lastReachedWordOrdinal, const uint32_t totalWords, float* const progress) {
  if (lastReachedWordOrdinal >= totalWords) {
    return false;
  }
  return pdfCalculateWordCursorProgress(lastReachedWordOrdinal + 1U, totalWords, progress);
}

bool pdfCalculateWordCursorProgress(const uint32_t reachedWordCount, const uint32_t totalWords, float* const progress) {
  if (progress == nullptr || totalWords == 0 || reachedWordCount > totalWords) {
    return false;
  }
  *progress = static_cast<float>(reachedWordCount) / static_cast<float>(totalWords);
  return true;
}
