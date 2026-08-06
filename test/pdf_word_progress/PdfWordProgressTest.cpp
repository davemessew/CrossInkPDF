#include <gtest/gtest.h>

#include <algorithm>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "PdfCacheFormat.h"
#include "PdfLayoutWordIndex.h"
#include "PdfProgressStore.h"
#include "PdfReaderProgressState.h"
#include "PdfTestCacheIo.h"
#include "PdfTestIo.h"

namespace {

PdfLayoutWordRange range(const uint32_t first, const uint32_t last, const uint32_t blockOffset, const char* anchor) {
  PdfLayoutWordRange result;
  result.firstGlobalWordOrdinal = first;
  result.lastGlobalWordOrdinal = last;
  result.firstBlockWordOffset = blockOffset;
  result.valid = true;
  const size_t anchorLength = std::strlen(anchor);
  EXPECT_LT(anchorLength, sizeof(result.blockAnchor));
  std::memcpy(result.blockAnchor, anchor, anchorLength);
  return result;
}

PdfSourceIdentity identity() {
  PdfSourceIdentity result;
  result.size = 123456;
  result.modificationTime = {true, 987654};
  result.headFingerprint = 0x1122334455667788ULL;
  result.tailFingerprint = 0x8877665544332211ULL;
  return result;
}

void setAnchor(ReflowReadingPosition& position, const char* anchor) {
  ASSERT_LT(std::strlen(anchor), sizeof(position.blockAnchor));
  std::memset(position.blockAnchor, 0, sizeof(position.blockAnchor));
  std::memcpy(position.blockAnchor, anchor, std::strlen(anchor));
}

void putU32(std::vector<uint8_t>& bytes, const size_t offset, const uint32_t value) {
  ASSERT_LE(offset + sizeof(value), bytes.size());
  bytes[offset] = static_cast<uint8_t>(value);
  bytes[offset + 1] = static_cast<uint8_t>(value >> 8U);
  bytes[offset + 2] = static_cast<uint8_t>(value >> 16U);
  bytes[offset + 3] = static_cast<uint8_t>(value >> 24U);
}

void putU16(std::vector<uint8_t>& bytes, const size_t offset, const uint16_t value) {
  ASSERT_LE(offset + sizeof(uint16_t), bytes.size());
  bytes[offset] = static_cast<uint8_t>(value);
  bytes[offset + 1] = static_cast<uint8_t>(value >> 8U);
}

uint16_t getU16(const std::vector<uint8_t>& bytes, const size_t offset) {
  EXPECT_LE(offset + sizeof(uint16_t), bytes.size());
  return static_cast<uint16_t>(bytes[offset]) | static_cast<uint16_t>(bytes[offset + 1]) << 8U;
}

uint32_t getU32(const std::vector<uint8_t>& bytes, const size_t offset) {
  EXPECT_LE(offset + sizeof(uint32_t), bytes.size());
  return static_cast<uint32_t>(bytes[offset]) | static_cast<uint32_t>(bytes[offset + 1]) << 8U |
         static_cast<uint32_t>(bytes[offset + 2]) << 16U | static_cast<uint32_t>(bytes[offset + 3]) << 24U;
}

void repairRecordAndAggregateCrc(std::vector<uint8_t>& bytes) {
  ASSERT_GE(bytes.size(), PDF_LAYOUT_WORD_INDEX_HEADER_BYTES + PDF_LAYOUT_WORD_INDEX_FOOTER_BYTES);
  const size_t recordBytes = bytes.size() - PDF_LAYOUT_WORD_INDEX_HEADER_BYTES - PDF_LAYOUT_WORD_INDEX_FOOTER_BYTES;
  ASSERT_EQ(recordBytes % PDF_LAYOUT_WORD_INDEX_RECORD_BYTES, 0U);
  uint32_t aggregate = pdfCacheCrc32(bytes.data(), PDF_LAYOUT_WORD_INDEX_HEADER_BYTES);
  for (size_t offset = PDF_LAYOUT_WORD_INDEX_HEADER_BYTES; offset < PDF_LAYOUT_WORD_INDEX_HEADER_BYTES + recordBytes;
       offset += PDF_LAYOUT_WORD_INDEX_RECORD_BYTES) {
    putU32(bytes, offset + 36,
           pdfCacheCrc32(bytes.data() + offset, PDF_LAYOUT_WORD_INDEX_RECORD_BYTES - sizeof(uint32_t)));
    aggregate = pdfCacheCrc32(bytes.data() + offset, PDF_LAYOUT_WORD_INDEX_RECORD_BYTES - sizeof(uint32_t), aggregate);
  }
  const size_t footer = bytes.size() - PDF_LAYOUT_WORD_INDEX_FOOTER_BYTES;
  putU32(bytes, footer + 8, aggregate);
  putU32(bytes, footer + 12, pdfCacheCrc32(bytes.data() + footer, 12));
}

class CountingByteSource {
 public:
  explicit CountingByteSource(const std::vector<uint8_t>& bytes) : bytes_(bytes) {}

  PdfByteSource source() { return {this, bytes_.size(), readAt}; }
  uint32_t readCount() const { return readCount_; }
  const std::vector<std::pair<uint64_t, size_t>>& reads() const { return reads_; }
  void resetReadCount() {
    readCount_ = 0;
    reads_.clear();
  }

 private:
  static PdfStatus readAt(void* context, const uint64_t offset, uint8_t* destination, const size_t requested,
                          size_t* bytesRead) {
    if (context == nullptr || destination == nullptr || bytesRead == nullptr) {
      return PdfStatus::failure(PdfError::InvalidArgument, offset);
    }
    auto& source = *static_cast<CountingByteSource*>(context);
    ++source.readCount_;
    source.reads_.emplace_back(offset, requested);
    if (offset > source.bytes_.size()) {
      return PdfStatus::failure(PdfError::InvalidOffset, offset);
    }
    const size_t count = std::min(requested, source.bytes_.size() - static_cast<size_t>(offset));
    std::memcpy(destination, source.bytes_.data() + static_cast<size_t>(offset), count);
    *bytesRead = count;
    return PdfStatus::success();
  }

  std::vector<uint8_t> bytes_;
  uint32_t readCount_ = 0;
  std::vector<std::pair<uint64_t, size_t>> reads_;
};

class MutableByteFile {
 public:
  explicit MutableByteFile(std::vector<uint8_t> bytes) : bytes_(std::move(bytes)) {}

  PdfByteSource source() { return {this, bytes_.size(), readAt}; }
  PdfLayoutWordIndexPatchSink patch() { return {this, writeAt}; }
  const std::vector<uint8_t>& bytes() const { return bytes_; }

 private:
  static PdfStatus readAt(void* context, const uint64_t offset, uint8_t* destination, const size_t requested,
                          size_t* bytesRead) {
    if (context == nullptr || destination == nullptr || bytesRead == nullptr ||
        offset > static_cast<MutableByteFile*>(context)->bytes_.size()) {
      return PdfStatus::failure(PdfError::InvalidArgument, offset);
    }
    auto& file = *static_cast<MutableByteFile*>(context);
    const size_t count = std::min(requested, file.bytes_.size() - static_cast<size_t>(offset));
    std::memcpy(destination, file.bytes_.data() + static_cast<size_t>(offset), count);
    *bytesRead = count;
    return PdfStatus::success();
  }

  static PdfStatus writeAt(void* context, const uint64_t offset, const uint8_t* source, const size_t requested,
                           size_t* bytesWritten) {
    if (context == nullptr || source == nullptr || bytesWritten == nullptr ||
        offset > static_cast<MutableByteFile*>(context)->bytes_.size() ||
        requested > static_cast<MutableByteFile*>(context)->bytes_.size() - static_cast<size_t>(offset)) {
      return PdfStatus::failure(PdfError::InvalidArgument, offset);
    }
    auto& file = *static_cast<MutableByteFile*>(context);
    std::memcpy(file.bytes_.data() + static_cast<size_t>(offset), source, requested);
    *bytesWritten = requested;
    return PdfStatus::success();
  }

  std::vector<uint8_t> bytes_;
};

size_t requestedBytes(const CountingByteSource& source) {
  size_t total = 0;
  for (const auto& read : source.reads()) {
    total += read.second;
  }
  return total;
}

std::vector<uint8_t> finalizedSectionCache(std::vector<uint8_t> bytes, const uint32_t pairToken) {
  constexpr size_t kBindingTrailerBytes = 16;
  EXPECT_FALSE(bytes.empty());
  EXPECT_NE(pairToken, 0U);
  EXPECT_LE(bytes.size(), UINT32_MAX);
  const uint32_t contentLength = static_cast<uint32_t>(bytes.size());
  const size_t trailer = bytes.size();
  bytes.resize(bytes.size() + kBindingTrailerBytes, 0);
  std::memcpy(bytes.data() + trailer, "PWIB", 4);
  putU32(bytes, trailer + 4, contentLength);
  putU32(bytes, trailer + 8, pairToken);
  putU32(bytes, trailer + 12, pdfCacheCrc32(bytes.data() + trailer, 12));
  return bytes;
}

TEST(PdfLayoutWordIndexTest, StoresExactKnownPageOrdinalsAndFindsTheirPages) {
  PdfTestByteSink sink;
  PdfLayoutWordIndexWriter writer;
  ASSERT_TRUE(writer.begin(sink.sink(), 2, 100, 12));
  ASSERT_TRUE(writer.append(range(100, 103, 0, "b00000007")));
  ASSERT_TRUE(writer.append(range(104, 108, 4, "b00000007")));
  ASSERT_TRUE(writer.append(range(109, 111, 0, "b00000008")));
  ASSERT_TRUE(writer.finish());

  PdfTestByteSource bytes(sink.bytes());
  PdfLayoutWordIndexInfo info;
  ASSERT_TRUE(pdfInspectLayoutWordIndex(bytes.source(), &info));
  EXPECT_EQ(info.sectionIndex, 2);
  EXPECT_EQ(info.firstGlobalWordOrdinal, 100U);
  EXPECT_EQ(info.sectionWordCount, 12U);
  EXPECT_EQ(info.pageCount, 3U);

  PdfLayoutWordRange window[2];
  ASSERT_TRUE(pdfReadLayoutWordRanges(bytes.source(), 1, 2, window));
  EXPECT_EQ(window[0].firstGlobalWordOrdinal, 104U);
  EXPECT_EQ(window[0].lastGlobalWordOrdinal, 108U);
  EXPECT_EQ(window[1].firstGlobalWordOrdinal, 109U);
  EXPECT_EQ(window[1].lastGlobalWordOrdinal, 111U);

  uint16_t page = UINT16_MAX;
  PdfLayoutWordRange found;
  ASSERT_TRUE(pdfFindLayoutPage(bytes.source(), 108, &page, &found));
  EXPECT_EQ(page, 1);
  EXPECT_EQ(found.firstGlobalWordOrdinal, 104U);
  EXPECT_EQ(found.lastGlobalWordOrdinal, 108U);
  EXPECT_EQ(found.firstBlockWordOffset, 4U);
  EXPECT_STREQ(found.blockAnchor, "b00000007");

  ASSERT_TRUE(pdfFindLayoutAnchor(bytes.source(), "b00000007", 7, &page, &found));
  EXPECT_EQ(page, 1);
  EXPECT_EQ(found.firstBlockWordOffset, 4U);
}

TEST(PdfLayoutWordIndexTest, RejectsOverlapsGapsOutsideTheSectionAndCorruptRecords) {
  PdfTestByteSink sink;
  PdfLayoutWordIndexWriter writer;
  ASSERT_TRUE(writer.begin(sink.sink(), 0, 20, 4));
  ASSERT_TRUE(writer.append(range(20, 21, 0, "b00000000")));
  EXPECT_EQ(writer.append(range(23, 23, 2, "b00000000")).error, PdfError::Malformed);

  PdfTestByteSink validSink;
  PdfLayoutWordIndexWriter validWriter;
  ASSERT_TRUE(validWriter.begin(validSink.sink(), 0, 20, 4));
  ASSERT_TRUE(validWriter.append(range(20, 21, 0, "b00000000")));
  ASSERT_TRUE(validWriter.append(range(22, 23, 0, "b00000001")));
  ASSERT_TRUE(validWriter.finish());

  std::vector<uint8_t> corrupt = validSink.bytes();
  ASSERT_GT(corrupt.size(), PDF_LAYOUT_WORD_INDEX_HEADER_BYTES + 1U);
  corrupt[PDF_LAYOUT_WORD_INDEX_HEADER_BYTES + 1U] ^= 0x80U;
  PdfTestByteSource corruptSource(std::move(corrupt));
  PdfLayoutWordIndexInfo info;
  EXPECT_EQ(pdfInspectLayoutWordIndex(corruptSource.source(), &info).error, PdfError::Malformed);
}

TEST(PdfLayoutWordIndexTest, AggregateFooterRejectsRecordWhoseLocalCrcWasRepaired) {
  PdfTestByteSink sink;
  PdfLayoutWordIndexWriter writer;
  ASSERT_TRUE(writer.begin(sink.sink(), 0, 0, 2));
  ASSERT_TRUE(writer.append(range(0, 0, 0, "b00000000")));
  ASSERT_TRUE(writer.append(range(1, 1, 0, "b00000001")));
  ASSERT_TRUE(writer.finish());

  std::vector<uint8_t> aggregateMismatch = sink.bytes();
  const size_t secondRecord = PDF_LAYOUT_WORD_INDEX_HEADER_BYTES + PDF_LAYOUT_WORD_INDEX_RECORD_BYTES;
  aggregateMismatch[secondRecord + 12] ^= 0x01U;
  putU32(aggregateMismatch, secondRecord + 36,
         pdfCacheCrc32(aggregateMismatch.data() + secondRecord, PDF_LAYOUT_WORD_INDEX_RECORD_BYTES - sizeof(uint32_t)));

  PdfLayoutWordIndexInfo info;
  PdfTestByteSource mismatchSource(aggregateMismatch);
  EXPECT_EQ(pdfInspectLayoutWordIndex(mismatchSource.source(), &info).error, PdfError::Malformed);

  // Positive control: the same records are accepted when the aggregate and
  // footer CRCs are deliberately rebuilt too.
  repairRecordAndAggregateCrc(aggregateMismatch);
  PdfTestByteSource repairedSource(std::move(aggregateMismatch));
  EXPECT_TRUE(pdfInspectLayoutWordIndex(repairedSource.source(), &info));
}

TEST(PdfLayoutWordIndexTest, RejectsRangesBelowSectionStartAndMoreThanOneWordOfBackwardOverlap) {
  PdfTestByteSink belowStartSink;
  PdfLayoutWordIndexWriter belowStartWriter;
  ASSERT_TRUE(belowStartWriter.begin(belowStartSink.sink(), 0, 100, 4));
  EXPECT_EQ(belowStartWriter.append(range(0, 99, 0, "b00000000")).error, PdfError::Malformed);

  PdfTestByteSink overlapSink;
  PdfLayoutWordIndexWriter overlapWriter;
  ASSERT_TRUE(overlapWriter.begin(overlapSink.sink(), 0, 100, 4));
  ASSERT_TRUE(overlapWriter.append(range(100, 101, 0, "b00000000")));
  EXPECT_EQ(overlapWriter.append(range(100, 103, 0, "b00000000")).error, PdfError::Malformed);

  PdfTestByteSink validSink;
  PdfLayoutWordIndexWriter validWriter;
  ASSERT_TRUE(validWriter.begin(validSink.sink(), 0, 100, 4));
  ASSERT_TRUE(validWriter.append(range(100, 103, 0, "b00000000")));
  ASSERT_TRUE(validWriter.finish());

  std::vector<uint8_t> belowStartBytes = validSink.bytes();
  putU32(belowStartBytes, PDF_LAYOUT_WORD_INDEX_HEADER_BYTES, 0);
  putU32(belowStartBytes, PDF_LAYOUT_WORD_INDEX_HEADER_BYTES + 4, 99);
  repairRecordAndAggregateCrc(belowStartBytes);
  PdfTestByteSource belowStartSource(std::move(belowStartBytes));
  PdfLayoutWordIndexInfo info;
  EXPECT_EQ(pdfInspectLayoutWordIndex(belowStartSource.source(), &info).error, PdfError::Malformed);

  PdfTestByteSink twoPageSink;
  PdfLayoutWordIndexWriter twoPageWriter;
  ASSERT_TRUE(twoPageWriter.begin(twoPageSink.sink(), 0, 100, 4));
  ASSERT_TRUE(twoPageWriter.append(range(100, 101, 0, "b00000000")));
  ASSERT_TRUE(twoPageWriter.append(range(102, 103, 2, "b00000000")));
  ASSERT_TRUE(twoPageWriter.finish());
  std::vector<uint8_t> overlapBytes = twoPageSink.bytes();
  const size_t secondRecord = PDF_LAYOUT_WORD_INDEX_HEADER_BYTES + PDF_LAYOUT_WORD_INDEX_RECORD_BYTES;
  putU32(overlapBytes, secondRecord, 100);
  repairRecordAndAggregateCrc(overlapBytes);
  PdfTestByteSource overlapSource(std::move(overlapBytes));
  EXPECT_EQ(pdfInspectLayoutWordIndex(overlapSource.source(), &info).error, PdfError::Malformed);
}

TEST(PdfLayoutWordIndexTest, AcceptsOneSplitWordContinuationAcrossRepeatedPages) {
  PdfTestByteSink sink;
  PdfLayoutWordIndexWriter writer;
  ASSERT_TRUE(writer.begin(sink.sink(), 0, 100, 2));
  ASSERT_TRUE(writer.append(range(100, 100, 0, "b00000000")));
  ASSERT_TRUE(writer.append(range(100, 100, 0, "b00000000")));
  ASSERT_TRUE(writer.append(range(100, 101, 0, "b00000000")));
  ASSERT_TRUE(writer.finish());

  PdfTestByteSource source(sink.bytes());
  PdfLayoutWordIndexInfo info;
  ASSERT_TRUE(pdfInspectLayoutWordIndex(source.source(), &info));
  EXPECT_EQ(info.pageCount, 3U);
}

TEST(PdfLayoutWordIndexTest, ChoosesEarliestEqualAnchorOffsetAcrossPathologicalSplitWordPages) {
  PdfTestByteSink sink;
  PdfLayoutWordIndexWriter writer;
  ASSERT_TRUE(writer.begin(sink.sink(), 0, 0, 2));
  ASSERT_TRUE(writer.append(range(0, 0, 0, "b00000000")));
  ASSERT_TRUE(writer.append(range(0, 0, 0, "b00000000")));
  ASSERT_TRUE(writer.append(range(0, 1, 0, "b00000000")));
  ASSERT_TRUE(writer.finish());

  PdfTestByteSource source(sink.bytes());
  uint16_t page = UINT16_MAX;
  PdfLayoutWordRange found;
  ASSERT_TRUE(pdfFindLayoutAnchor(source.source(), "b00000000", 0, &page, &found));
  EXPECT_EQ(page, 0U);
  EXPECT_EQ(found.firstGlobalWordOrdinal, 0U);
  EXPECT_EQ(found.lastGlobalWordOrdinal, 0U);

  ASSERT_TRUE(pdfFindLayoutPage(source.source(), 1, &page, &found));
  EXPECT_EQ(page, 2U);
  EXPECT_EQ(found.lastGlobalWordOrdinal, 1U);
}

TEST(PdfLayoutWordIndexTest, AllowsPunctuationOnlyPagesWithoutCountingAWord) {
  PdfTestByteSink sink;
  PdfLayoutWordIndexWriter writer;
  ASSERT_TRUE(writer.begin(sink.sink(), 0, 0, 0));
  PdfLayoutWordRange empty;
  ASSERT_TRUE(writer.append(empty));
  ASSERT_TRUE(writer.finish());

  PdfTestByteSource bytes(sink.bytes());
  PdfLayoutWordRange decoded;
  ASSERT_TRUE(pdfReadLayoutWordRange(bytes.source(), 0, &decoded));
  EXPECT_FALSE(decoded.valid);
}

TEST(PdfLayoutWordIndexTest, PreservesLeadingMiddleAndTrailingEmptyPageWordCursors) {
  PdfTestByteSink sink;
  PdfLayoutWordIndexWriter writer;
  ASSERT_TRUE(writer.begin(sink.sink(), 0, 0, 4));
  ASSERT_TRUE(writer.append(PdfLayoutWordRange{}));
  ASSERT_TRUE(writer.append(range(0, 1, 0, "b00000000")));
  ASSERT_TRUE(writer.append(PdfLayoutWordRange{}));
  ASSERT_TRUE(writer.append(range(2, 3, 0, "b00000001")));
  ASSERT_TRUE(writer.append(PdfLayoutWordRange{}));
  ASSERT_TRUE(writer.finish());

  PdfTestByteSource bytes(sink.bytes());
  PdfLayoutWordRange decoded[5];
  ASSERT_TRUE(pdfReadLayoutWordRanges(bytes.source(), 0, 5, decoded));
  EXPECT_FALSE(decoded[0].valid);
  EXPECT_EQ(decoded[0].wordCursor, 0U);
  EXPECT_TRUE(decoded[1].valid);
  EXPECT_EQ(decoded[1].wordCursor, 2U);
  EXPECT_FALSE(decoded[2].valid);
  EXPECT_EQ(decoded[2].wordCursor, 2U);
  EXPECT_TRUE(decoded[3].valid);
  EXPECT_EQ(decoded[3].wordCursor, 4U);
  EXPECT_FALSE(decoded[4].valid);
  EXPECT_EQ(decoded[4].wordCursor, 4U);

  float progress = -1.0F;
  ASSERT_TRUE(pdfCalculateWordCursorProgress(decoded[0].wordCursor, 4, &progress));
  EXPECT_FLOAT_EQ(progress, 0.0F);
  ASSERT_TRUE(pdfCalculateWordCursorProgress(decoded[2].wordCursor, 4, &progress));
  EXPECT_FLOAT_EQ(progress, 0.5F);
  ASSERT_TRUE(pdfCalculateWordCursorProgress(decoded[4].wordCursor, 4, &progress));
  EXPECT_FLOAT_EQ(progress, 1.0F);

  uint16_t page = UINT16_MAX;
  PdfLayoutWordRange found;
  ASSERT_TRUE(pdfFindLayoutCursor(bytes.source(), 0, &page, &found));
  EXPECT_EQ(page, 0U);
  EXPECT_FALSE(found.valid);
  ASSERT_TRUE(pdfFindLayoutCursor(bytes.source(), 2, &page, &found));
  EXPECT_EQ(page, 2U);
  EXPECT_FALSE(found.valid);
  ASSERT_TRUE(pdfFindLayoutCursor(bytes.source(), 4, &page, &found));
  EXPECT_EQ(page, 4U);
  EXPECT_FALSE(found.valid);
}

TEST(PdfLayoutWordIndexTest, CursorPrefersExactEmptyPageThenMapsAcrossChangedPagination) {
  PdfTestByteSink originalSink;
  PdfLayoutWordIndexWriter originalWriter;
  ASSERT_TRUE(originalWriter.begin(originalSink.sink(), 0, 0, 4));
  ASSERT_TRUE(originalWriter.append(range(0, 1, 0, "b00000000")));
  ASSERT_TRUE(originalWriter.append(PdfLayoutWordRange{}));
  ASSERT_TRUE(originalWriter.append(range(2, 3, 0, "b00000001")));
  ASSERT_TRUE(originalWriter.finish());

  uint16_t page = UINT16_MAX;
  PdfLayoutWordRange found;
  PdfTestByteSource original(originalSink.bytes());
  ASSERT_TRUE(pdfFindLayoutCursor(original.source(), 2, &page, &found));
  EXPECT_EQ(page, 1U);
  EXPECT_FALSE(found.valid);

  PdfTestByteSink relayoutSink;
  PdfLayoutWordIndexWriter relayoutWriter;
  ASSERT_TRUE(relayoutWriter.begin(relayoutSink.sink(), 0, 0, 4));
  ASSERT_TRUE(relayoutWriter.append(range(0, 0, 0, "b00000000")));
  ASSERT_TRUE(relayoutWriter.append(range(1, 2, 1, "b00000000")));
  ASSERT_TRUE(relayoutWriter.append(range(3, 3, 0, "b00000001")));
  ASSERT_TRUE(relayoutWriter.finish());

  PdfTestByteSource relayout(relayoutSink.bytes());
  ASSERT_TRUE(pdfFindLayoutCursor(relayout.source(), 2, &page, &found));
  EXPECT_EQ(page, 1U);
  EXPECT_TRUE(found.valid);
  EXPECT_EQ(found.firstGlobalWordOrdinal, 1U);
  EXPECT_EQ(found.lastGlobalWordOrdinal, 2U);

  ASSERT_TRUE(pdfFindLayoutCursor(relayout.source(), 4, &page, &found));
  EXPECT_EQ(page, 2U);
  EXPECT_TRUE(found.valid);
  EXPECT_EQ(found.lastGlobalWordOrdinal, 3U);
}

TEST(PdfLayoutWordIndexTest, ReadsFourPageWindowsAndSearchesInFourRecordBatches) {
  PdfTestByteSink sink;
  PdfLayoutWordIndexWriter writer;
  ASSERT_TRUE(writer.begin(sink.sink(), 0, 0, 8));
  for (uint32_t ordinal = 0; ordinal < 8; ++ordinal) {
    ASSERT_TRUE(writer.append(range(ordinal, ordinal, ordinal, "b00000000")));
  }
  ASSERT_TRUE(writer.finish());

  CountingByteSource source(sink.bytes());
  PdfLayoutWordRange window[4];
  ASSERT_TRUE(pdfReadLayoutWordRanges(source.source(), 2, 4, window));
  EXPECT_EQ(source.readCount(), 3U);
  EXPECT_EQ(window[0].firstGlobalWordOrdinal, 2U);
  EXPECT_EQ(window[3].lastGlobalWordOrdinal, 5U);

  source.resetReadCount();
  uint16_t page = UINT16_MAX;
  ASSERT_TRUE(pdfFindLayoutPage(source.source(), 7, &page));
  EXPECT_EQ(page, 7U);
  EXPECT_EQ(source.readCount(), 4U);

  source.resetReadCount();
  ASSERT_TRUE(pdfFindLayoutAnchor(source.source(), "b00000000", 7, &page));
  EXPECT_EQ(page, 7U);
  EXPECT_EQ(source.readCount(), 4U);
}

TEST(PdfLayoutWordIndexTest, ReadsValidatedPageRecordsInOneBoundedFourRecordIo) {
  PdfTestByteSink sink;
  PdfLayoutWordIndexWriter writer;
  ASSERT_TRUE(writer.begin(sink.sink(), 0, 0, 4));
  for (uint32_t ordinal = 0; ordinal < 4; ++ordinal) {
    ASSERT_TRUE(writer.append(range(ordinal, ordinal, ordinal, "b00000000"),
                              PdfLayoutPageRecord{ordinal + 1U, static_cast<uint16_t>(ordinal),
                                                  static_cast<uint16_t>(ordinal + 10U)}));
  }
  ASSERT_TRUE(writer.finish());

  CountingByteSource source(sink.bytes());
  PdfLayoutWordIndexInfo info;
  ASSERT_TRUE(pdfInspectLayoutWordIndex(source.source(), &info));
  source.resetReadCount();

  PdfLayoutPageRecord pages[4];
  ASSERT_TRUE(pdfReadValidatedLayoutPageRecords(source.source(), info, 0, 4, pages));
  EXPECT_EQ(source.readCount(), 1U);
  EXPECT_EQ(source.reads(), (std::vector<std::pair<uint64_t, size_t>>{{PDF_LAYOUT_WORD_INDEX_HEADER_BYTES, 160}}));
  EXPECT_EQ(pages[3].fileOffset, 4U);

  source.resetReadCount();
  EXPECT_EQ(pdfReadValidatedLayoutPageRecords(source.source(), info, 0, 5, pages).error,
            PdfError::InvalidArgument);
  EXPECT_EQ(source.readCount(), 0U);
  EXPECT_TRUE(source.reads().empty());
}

TEST(PdfLayoutWordIndexTest, NinePageInspectAndThreeFieldReplayUseExactlyFourteenReads) {
  constexpr uint16_t pageCount = 9;
  PdfTestByteSink sink;
  PdfLayoutWordIndexWriter writer;
  ASSERT_TRUE(writer.begin(sink.sink(), 0, 0, pageCount));
  for (uint32_t ordinal = 0; ordinal < pageCount; ++ordinal) {
    ASSERT_TRUE(writer.append(range(ordinal, ordinal, ordinal, "b00000000"),
                              PdfLayoutPageRecord{ordinal + 1U, static_cast<uint16_t>(ordinal),
                                                  static_cast<uint16_t>(ordinal + 10U)}));
  }
  ASSERT_TRUE(writer.finish());

  CountingByteSource source(sink.bytes());
  PdfLayoutWordIndexInfo info;
  ASSERT_TRUE(pdfInspectLayoutWordIndex(source.source(), &info));
  PdfLayoutPageRecord pages[4];
  for (uint8_t field = 0; field < 3; ++field) {
    for (uint16_t first = 0; first < pageCount; first = static_cast<uint16_t>(first + 4U)) {
      const uint16_t count = std::min<uint16_t>(4, static_cast<uint16_t>(pageCount - first));
      ASSERT_TRUE(pdfReadValidatedLayoutPageRecords(source.source(), info, first, count, pages));
    }
  }
  EXPECT_EQ(source.readCount(), 14U);
}

TEST(PdfLayoutWordIndexTest, CalculatesExactStartMiddleAndFinalWordProgress) {
  float progress = -1.0F;
  EXPECT_FALSE(pdfCalculateWordProgress(0, 0, &progress));
  EXPECT_FALSE(pdfCalculateWordProgress(10, 10, &progress));
  EXPECT_FALSE(pdfCalculateWordProgress(0, 10, nullptr));

  ASSERT_TRUE(pdfCalculateWordProgress(0, 10, &progress));
  EXPECT_FLOAT_EQ(progress, 0.1F);
  ASSERT_TRUE(pdfCalculateWordProgress(4, 10, &progress));
  EXPECT_FLOAT_EQ(progress, 0.5F);
  ASSERT_TRUE(pdfCalculateWordProgress(9, 10, &progress));
  EXPECT_FLOAT_EQ(progress, 1.0F);
}

TEST(PdfLayoutWordIndexTest, RelayoutChangesPageIndexButPreservesSemanticPosition) {
  PdfTestByteSink compactSink;
  PdfLayoutWordIndexWriter compactWriter;
  ASSERT_TRUE(compactWriter.begin(compactSink.sink(), 0, 0, 8));
  ASSERT_TRUE(compactWriter.append(range(0, 3, 0, "b00000000")));
  ASSERT_TRUE(compactWriter.append(range(4, 7, 4, "b00000000")));
  ASSERT_TRUE(compactWriter.finish());

  PdfTestByteSink largeFontSink;
  PdfLayoutWordIndexWriter largeFontWriter;
  ASSERT_TRUE(largeFontWriter.begin(largeFontSink.sink(), 0, 0, 8));
  ASSERT_TRUE(largeFontWriter.append(range(0, 1, 0, "b00000000")));
  ASSERT_TRUE(largeFontWriter.append(range(2, 5, 2, "b00000000")));
  ASSERT_TRUE(largeFontWriter.append(range(6, 7, 6, "b00000000")));
  ASSERT_TRUE(largeFontWriter.finish());

  PdfTestByteSource compactBytes(compactSink.bytes());
  PdfTestByteSource largeFontBytes(largeFontSink.bytes());
  uint16_t compactPage = UINT16_MAX;
  uint16_t largeFontPage = UINT16_MAX;
  ASSERT_TRUE(pdfFindLayoutAnchor(compactBytes.source(), "b00000000", 3, &compactPage));
  ASSERT_TRUE(pdfFindLayoutAnchor(largeFontBytes.source(), "b00000000", 3, &largeFontPage));
  EXPECT_EQ(compactPage, 0);
  EXPECT_EQ(largeFontPage, 1);

  ASSERT_TRUE(pdfFindLayoutPage(compactBytes.source(), 3, &compactPage));
  ASSERT_TRUE(pdfFindLayoutPage(largeFontBytes.source(), 3, &largeFontPage));
  EXPECT_EQ(compactPage, 0);
  EXPECT_EQ(largeFontPage, 1);
}

TEST(PdfLayoutWordIndexTest, VersionThreeGoldenRecordPreservesSemanticPayloadAndAddsPageLocation) {
  PdfTestByteSink sink;
  PdfLayoutWordIndexWriter writer;
  ASSERT_TRUE(writer.begin(sink.sink(), 7, 100, 2));
  const PdfLayoutPageRecord location{0x12345678U, 0x2345U, 0x3456U};
  ASSERT_TRUE(writer.append(range(100, 101, 9, "b00000007"), location));
  ASSERT_TRUE(writer.finish());

  const std::vector<uint8_t>& bytes = sink.bytes();
  ASSERT_EQ(bytes.size(), PDF_LAYOUT_WORD_INDEX_HEADER_BYTES + PDF_LAYOUT_WORD_INDEX_RECORD_BYTES +
                              PDF_LAYOUT_WORD_INDEX_FOOTER_BYTES);
  EXPECT_EQ(getU16(bytes, 4), 3U);
  EXPECT_EQ(getU16(bytes, 10), 40U);
  const size_t record = PDF_LAYOUT_WORD_INDEX_HEADER_BYTES;
  EXPECT_EQ(getU32(bytes, record), 100U);
  EXPECT_EQ(getU32(bytes, record + 4), 101U);
  EXPECT_EQ(getU32(bytes, record + 8), 9U);
  EXPECT_EQ(std::memcmp(bytes.data() + record + 12, "b00000007", 9), 0);
  EXPECT_EQ(bytes[record + 22], 1U);
  EXPECT_EQ(bytes[record + 23], 9U);
  EXPECT_EQ(getU32(bytes, record + 24), 0U);
  EXPECT_EQ(getU32(bytes, record + 28), location.fileOffset);
  EXPECT_EQ(getU16(bytes, record + 32), location.paragraphIndex);
  EXPECT_EQ(getU16(bytes, record + 34), location.listItemIndex);
  EXPECT_EQ(getU32(bytes, record + 36), pdfCacheCrc32(bytes.data() + record, 36));

  PdfTestByteSource source(bytes);
  PdfLayoutWordIndexInfo info;
  ASSERT_TRUE(pdfInspectLayoutWordIndex(source.source(), &info));
  PdfLayoutPageRecord decoded;
  ASSERT_TRUE(pdfReadValidatedLayoutPageRecords(source.source(), info, 0, 1, &decoded));
  EXPECT_EQ(decoded.fileOffset, location.fileOffset);
  EXPECT_EQ(decoded.paragraphIndex, location.paragraphIndex);
  EXPECT_EQ(decoded.listItemIndex, location.listItemIndex);
}

TEST(PdfLayoutWordIndexTest, BindsSidecarToExactSameLengthSectionCacheAndRejectsCrossPair) {
  PdfTestByteSink sink;
  PdfLayoutWordIndexWriter writer;
  ASSERT_TRUE(writer.begin(sink.sink(), 7, 100, 2));
  ASSERT_TRUE(writer.append(range(100, 101, 9, "b00000007"), PdfLayoutPageRecord{123, 4, 5}));
  ASSERT_TRUE(writer.finish());

  MutableByteFile sidecar(sink.bytes());
  const std::vector<uint8_t> oldSection = finalizedSectionCache({44, 1, 2, 3, 4, 5, 6, 7}, 0x11223344U);
  const std::vector<uint8_t> newSection = finalizedSectionCache({44, 1, 2, 3, 4, 5, 6, 8}, 0x55667788U);
  PdfTestByteSource oldSource(oldSection);
  PdfTestByteSource newSource(newSection);
  PdfLayoutCacheBinding oldBinding;
  PdfLayoutCacheBinding newBinding;
  ASSERT_TRUE(pdfComputeLayoutCacheBinding(oldSource.source(), &oldBinding));
  ASSERT_TRUE(pdfComputeLayoutCacheBinding(newSource.source(), &newBinding));
  ASSERT_EQ(oldBinding.length, newBinding.length);
  ASSERT_NE(oldBinding.token, newBinding.token);

  ASSERT_TRUE(pdfBindLayoutWordIndex(sidecar.source(), sidecar.patch(), oldBinding));
  PdfLayoutWordIndexInfo info;
  ASSERT_TRUE(pdfInspectLayoutWordIndex(sidecar.source(), &info));
  EXPECT_TRUE(pdfLayoutWordIndexMatchesSectionCache(info, oldBinding));
  EXPECT_FALSE(pdfLayoutWordIndexMatchesSectionCache(info, newBinding));
  EXPECT_EQ(info.sectionCacheLength, oldSection.size() - PDF_LAYOUT_CACHE_BINDING_TRAILER_BYTES);
  EXPECT_EQ(info.sectionCacheToken, oldBinding.token);
}

TEST(PdfLayoutWordIndexTest, ComputesSectionBindingWithConstantSeekReadBudgetIndependentOfCacheLength) {
  // Each PdfByteSource read maps one-to-one to HalFile::seek64 + read on the
  // device. A section reopen must therefore use a fixed I/O budget rather than
  // walking the complete layout cache in small blocks.
  std::vector<uint8_t> smallBytes(4U * 1024U);
  std::vector<uint8_t> largeBytes(1024U * 1024U);
  for (size_t index = 0; index < smallBytes.size(); ++index) {
    smallBytes[index] = static_cast<uint8_t>((index * 29U + 7U) & 0xffU);
  }
  for (size_t index = 0; index < largeBytes.size(); ++index) {
    largeBytes[index] = static_cast<uint8_t>((index * 31U + 11U) & 0xffU);
  }

  smallBytes = finalizedSectionCache(std::move(smallBytes), 0x10203040U);
  largeBytes = finalizedSectionCache(std::move(largeBytes), 0x50607080U);

  CountingByteSource small(smallBytes);
  CountingByteSource large(largeBytes);
  PdfLayoutCacheBinding smallBinding;
  PdfLayoutCacheBinding largeBinding;
  ASSERT_TRUE(pdfComputeLayoutCacheBinding(small.source(), &smallBinding));
  ASSERT_TRUE(pdfComputeLayoutCacheBinding(large.source(), &largeBinding));

  constexpr uint32_t kExpectedReads = 1;
  constexpr size_t kExpectedBytes = 16;
  EXPECT_EQ(small.readCount(), kExpectedReads);
  EXPECT_EQ(large.readCount(), kExpectedReads);
  EXPECT_EQ(small.readCount(), large.readCount());
  EXPECT_EQ(requestedBytes(small), kExpectedBytes);
  EXPECT_EQ(requestedBytes(large), kExpectedBytes);
  ASSERT_FALSE(small.reads().empty());
  ASSERT_FALSE(large.reads().empty());
  EXPECT_EQ(small.reads().front().first, smallBytes.size() - kExpectedBytes);
  EXPECT_EQ(large.reads().front().first, largeBytes.size() - kExpectedBytes);
}

TEST(PdfLayoutWordIndexTest, RejectsMissingTruncatedCorruptAndZeroTokenBindingTrailers) {
  const std::vector<uint8_t> prefix = {44, 1, 2, 3, 4, 5, 6, 7};
  PdfLayoutCacheBinding binding;

  PdfTestByteSource missing(prefix);
  EXPECT_FALSE(pdfComputeLayoutCacheBinding(missing.source(), &binding));

  std::vector<uint8_t> truncated = finalizedSectionCache(prefix, 0x11223344U);
  truncated.pop_back();
  PdfTestByteSource truncatedSource(truncated);
  EXPECT_FALSE(pdfComputeLayoutCacheBinding(truncatedSource.source(), &binding));

  std::vector<uint8_t> corrupt = finalizedSectionCache(prefix, 0x11223344U);
  corrupt.back() ^= 0x80U;
  PdfTestByteSource corruptSource(corrupt);
  EXPECT_FALSE(pdfComputeLayoutCacheBinding(corruptSource.source(), &binding));

  std::vector<uint8_t> zeroToken = finalizedSectionCache(prefix, 0x11223344U);
  const size_t trailer = zeroToken.size() - PDF_LAYOUT_CACHE_BINDING_TRAILER_BYTES;
  putU32(zeroToken, trailer + 8, 0);
  putU32(zeroToken, trailer + 12, pdfCacheCrc32(zeroToken.data() + trailer, 12));
  PdfTestByteSource zeroTokenSource(zeroToken);
  EXPECT_FALSE(pdfComputeLayoutCacheBinding(zeroTokenSource.source(), &binding));
}

TEST(PdfLayoutWordIndexTest, RejectsPriorVersionTwoSidecarEvenWhenItsChecksumsAreRepaired) {
  PdfTestByteSink sink;
  PdfLayoutWordIndexWriter writer;
  ASSERT_TRUE(writer.begin(sink.sink(), 0, 0, 1));
  ASSERT_TRUE(writer.append(range(0, 0, 0, "b00000000")));
  ASSERT_TRUE(writer.finish());

  std::vector<uint8_t> versionTwo = sink.bytes();
  putU16(versionTwo, 4, 2);
  putU32(versionTwo, 28, pdfCacheCrc32(versionTwo.data(), 28));
  repairRecordAndAggregateCrc(versionTwo);
  PdfTestByteSource source(versionTwo);
  PdfLayoutWordIndexInfo info;
  EXPECT_EQ(pdfInspectLayoutWordIndex(source.source(), &info).error, PdfError::Malformed);
}

TEST(PdfLayoutWordIndexTest, RejectsVersionOneTruncationCountCrcAndNonmonotonicLocations) {
  PdfTestByteSink sink;
  PdfLayoutWordIndexWriter writer;
  ASSERT_TRUE(writer.begin(sink.sink(), 0, 0, 2));
  ASSERT_TRUE(writer.append(range(0, 0, 0, "b00000000"), PdfLayoutPageRecord{100, 3, 4}));
  ASSERT_TRUE(writer.append(range(1, 1, 0, "b00000001"), PdfLayoutPageRecord{200, 5, 6}));
  ASSERT_TRUE(writer.finish());
  PdfLayoutWordIndexInfo info;

  std::vector<uint8_t> versionOne = sink.bytes();
  versionOne[4] = 1;
  versionOne[5] = 0;
  putU32(versionOne, 28, pdfCacheCrc32(versionOne.data(), 28));
  PdfTestByteSource versionOneSource(versionOne);
  EXPECT_EQ(pdfInspectLayoutWordIndex(versionOneSource.source(), &info).error, PdfError::Malformed);

  std::vector<uint8_t> truncated = sink.bytes();
  truncated.pop_back();
  PdfTestByteSource truncatedSource(truncated);
  EXPECT_FALSE(pdfInspectLayoutWordIndex(truncatedSource.source(), &info));

  std::vector<uint8_t> badCount = sink.bytes();
  const size_t footer = badCount.size() - PDF_LAYOUT_WORD_INDEX_FOOTER_BYTES;
  badCount[footer + 4] = 3;
  putU32(badCount, footer + 12, pdfCacheCrc32(badCount.data() + footer, 12));
  PdfTestByteSource badCountSource(badCount);
  EXPECT_EQ(pdfInspectLayoutWordIndex(badCountSource.source(), &info).error, PdfError::Malformed);

  std::vector<uint8_t> badLocalCrc = sink.bytes();
  badLocalCrc[PDF_LAYOUT_WORD_INDEX_HEADER_BYTES + 28] ^= 1U;
  PdfTestByteSource badLocalCrcSource(badLocalCrc);
  EXPECT_EQ(pdfInspectLayoutWordIndex(badLocalCrcSource.source(), &info).error, PdfError::Malformed);

  std::vector<uint8_t> nonmonotonic = sink.bytes();
  const size_t second = PDF_LAYOUT_WORD_INDEX_HEADER_BYTES + PDF_LAYOUT_WORD_INDEX_RECORD_BYTES;
  putU32(nonmonotonic, second + 28, 100);
  putU32(nonmonotonic, second + 36, pdfCacheCrc32(nonmonotonic.data() + second, 36));
  uint32_t aggregate = pdfCacheCrc32(nonmonotonic.data(), PDF_LAYOUT_WORD_INDEX_HEADER_BYTES);
  aggregate = pdfCacheCrc32(nonmonotonic.data() + PDF_LAYOUT_WORD_INDEX_HEADER_BYTES, 36, aggregate);
  aggregate = pdfCacheCrc32(nonmonotonic.data() + second, 36, aggregate);
  putU32(nonmonotonic, footer + 8, aggregate);
  putU32(nonmonotonic, footer + 12, pdfCacheCrc32(nonmonotonic.data() + footer, 12));
  PdfTestByteSource nonmonotonicSource(nonmonotonic);
  EXPECT_EQ(pdfInspectLayoutWordIndex(nonmonotonicSource.source(), &info).error, PdfError::Malformed);
}

TEST(PdfLayoutWordIndexTest, SupportsMoreThanTheFormerOneThousandTwentyFourPageHeapBoundary) {
  constexpr uint16_t pageCount = 1025;
  PdfTestByteSink sink;
  PdfLayoutWordIndexWriter writer;
  ASSERT_TRUE(writer.begin(sink.sink(), 0, 0, 0));
  for (uint16_t page = 0; page < pageCount; ++page) {
    ASSERT_TRUE(writer.append(PdfLayoutWordRange{}, PdfLayoutPageRecord{static_cast<uint32_t>(page + 1U), page,
                                                                       static_cast<uint16_t>(page + 7U)}));
  }
  ASSERT_TRUE(writer.finish());
  PdfTestByteSource source(sink.bytes());
  PdfLayoutWordIndexInfo info;
  ASSERT_TRUE(pdfInspectLayoutWordIndex(source.source(), &info));
  EXPECT_EQ(info.pageCount, pageCount);
  PdfLayoutPageRecord decoded[4];
  ASSERT_TRUE(pdfReadValidatedLayoutPageRecords(source.source(), info, 1021, 4, decoded));
  EXPECT_EQ(decoded[3].fileOffset, 1025U);
  EXPECT_EQ(decoded[3].paragraphIndex, 1024U);
  EXPECT_EQ(decoded[3].listItemIndex, 1031U);
}

TEST(PdfProgressStoreTest, AlternatesPowerSafeSlotsAndLoadsExactSemanticPosition) {
  PdfTestCacheIo files;
  files.addDirectory("/cache");
  PdfProgressStore store;
  ASSERT_TRUE(store.initialize(files.io(), "/cache", identity(), 112));

  ReflowReadingPosition first;
  first.sectionIndex = 2;
  first.pageNumber = 4;
  first.pageCount = 8;
  first.hasPageCount = true;
  first.hasSemanticPosition = true;
  first.globalWordOrdinal = 104;
  first.blockWordOffset = 4;
  setAnchor(first, "b00000007");
  ASSERT_TRUE(store.save(first));
  EXPECT_TRUE(files.exists("/cache/progress.a"));
  EXPECT_FALSE(files.exists("/cache/progress.b"));

  ReflowReadingPosition second = first;
  second.pageNumber = 5;
  second.globalWordOrdinal = 109;
  second.blockWordOffset = 0;
  setAnchor(second, "b00000008");
  ASSERT_TRUE(store.save(second));
  EXPECT_TRUE(files.exists("/cache/progress.b"));

  ReflowReadingPosition loaded;
  ASSERT_TRUE(store.load(&loaded));
  EXPECT_EQ(loaded.sectionIndex, 2);
  EXPECT_EQ(loaded.pageNumber, 5);
  EXPECT_EQ(loaded.pageCount, 8);
  EXPECT_TRUE(loaded.hasPageCount);
  EXPECT_TRUE(loaded.hasSemanticPosition);
  EXPECT_EQ(loaded.globalWordOrdinal, 109U);
  EXPECT_EQ(loaded.blockWordOffset, 0U);
  EXPECT_STREQ(loaded.blockAnchor, "b00000008");
}

TEST(PdfProgressStoreTest, KeepsPreviousSlotWhenTheNextWriteTears) {
  PdfTestCacheIo files;
  files.addDirectory("/cache");
  PdfProgressStore store;
  ASSERT_TRUE(store.initialize(files.io(), "/cache", identity(), 112));

  ReflowReadingPosition stable;
  stable.sectionIndex = 1;
  stable.pageNumber = 3;
  stable.hasSemanticPosition = true;
  stable.globalWordOrdinal = 50;
  setAnchor(stable, "b00000003");
  ASSERT_TRUE(store.save(stable));

  files.setWriteAllowance(12);
  ReflowReadingPosition torn = stable;
  torn.pageNumber = 7;
  torn.globalWordOrdinal = 90;
  EXPECT_FALSE(store.save(torn));
  files.clearWriteAllowance();

  ReflowReadingPosition loaded;
  ASSERT_TRUE(store.load(&loaded));
  EXPECT_EQ(loaded.pageNumber, 3);
  EXPECT_EQ(loaded.globalWordOrdinal, 50U);
  EXPECT_EQ(files.openHandleCount(), 0U);
}

TEST(PdfProgressStoreTest, FailedCommitKeepsThePreviousTupleForTheCurrentStore) {
  for (const PdfTestFaultPoint fault :
       {PdfTestFaultPoint::Write, PdfTestFaultPoint::Flush, PdfTestFaultPoint::Sync, PdfTestFaultPoint::Close}) {
    SCOPED_TRACE(static_cast<int>(fault));
    PdfTestCacheIo files;
    files.addDirectory("/cache");
    PdfProgressStore store;
    ASSERT_TRUE(store.initialize(files.io(), "/cache", identity(), 112));

    ReflowReadingPosition stable;
    stable.sectionIndex = 1;
    stable.pageNumber = 3;
    stable.pageCount = 8;
    stable.hasPageCount = true;
    stable.hasWordCursor = true;
    stable.wordCursor = 50;
    ASSERT_TRUE(store.save(stable));

    ReflowReadingPosition attempted = stable;
    attempted.pageNumber = 7;
    attempted.wordCursor = 90;
    files.fail(fault, fault == PdfTestFaultPoint::Close ? 2U : 1U);
    EXPECT_FALSE(store.save(attempted));
    files.clearFault();
    EXPECT_EQ(files.openHandleCount(), 0U);

    ReflowReadingPosition loaded;
    ASSERT_TRUE(store.load(&loaded));
    EXPECT_EQ(loaded.sectionIndex, stable.sectionIndex);
    EXPECT_EQ(loaded.pageNumber, stable.pageNumber);
    EXPECT_EQ(loaded.pageCount, stable.pageCount);
    EXPECT_TRUE(loaded.hasWordCursor);
    EXPECT_EQ(loaded.wordCursor, stable.wordCursor);
  }
}

TEST(PdfProgressStoreTest, SyncFailureIsUnconfirmedAndMayBecomeVisibleAfterReboot) {
  PdfTestCacheIo files;
  files.addDirectory("/cache");
  PdfProgressStore store;
  ASSERT_TRUE(store.initialize(files.io(), "/cache", identity(), 112));

  ReflowReadingPosition stable;
  stable.sectionIndex = 1;
  stable.pageNumber = 3;
  stable.hasWordCursor = true;
  stable.wordCursor = 50;
  ASSERT_TRUE(store.save(stable));

  ReflowReadingPosition attempted = stable;
  attempted.pageNumber = 7;
  attempted.wordCursor = 90;
  files.fail(PdfTestFaultPoint::Sync);
  EXPECT_FALSE(store.save(attempted));
  files.clearFault();
  ASSERT_TRUE(files.exists("/cache/progress.b"));

  ReflowReadingPosition currentProcess;
  ASSERT_TRUE(store.load(&currentProcess));
  EXPECT_EQ(currentProcess.pageNumber, stable.pageNumber);
  EXPECT_EQ(currentProcess.wordCursor, stable.wordCursor);

  // A failed sync means "commit not confirmed", not "definitely absent".
  // After reboot, a complete CRC-valid inactive slot may be the exact attempted
  // tuple and is safe to resume from.
  PdfProgressStore rebooted;
  ASSERT_TRUE(rebooted.initialize(files.io(), "/cache", identity(), 112));
  ReflowReadingPosition afterReboot;
  ASSERT_TRUE(rebooted.load(&afterReboot));
  EXPECT_EQ(afterReboot.pageNumber, attempted.pageNumber);
  EXPECT_EQ(afterReboot.wordCursor, attempted.wordCursor);
}

TEST(PdfProgressStoreTest, RetryAfterAmbiguousCommitReusesTheInactiveSlotAndProtectsTheStableSlot) {
  PdfTestCacheIo files;
  files.addDirectory("/cache");
  PdfProgressStore store;
  ASSERT_TRUE(store.initialize(files.io(), "/cache", identity(), 112));

  ReflowReadingPosition stable;
  stable.sectionIndex = 1;
  stable.pageNumber = 3;
  stable.hasWordCursor = true;
  stable.wordCursor = 50;
  ASSERT_TRUE(store.save(stable));

  ReflowReadingPosition attempted = stable;
  attempted.pageNumber = 7;
  attempted.wordCursor = 90;
  files.fail(PdfTestFaultPoint::Sync);
  EXPECT_FALSE(store.save(attempted));
  files.clearFault();

  files.setWriteAllowance(12);
  EXPECT_FALSE(store.save(attempted));
  files.clearWriteAllowance();
  ASSERT_TRUE(files.exists("/cache/progress.a"));
  ASSERT_TRUE(files.exists("/cache/progress.b"));
  EXPECT_EQ(files.bytes("/cache/progress.b").size(), 12U);

  PdfProgressStore rebooted;
  ASSERT_TRUE(rebooted.initialize(files.io(), "/cache", identity(), 112));
  ReflowReadingPosition loaded;
  ASSERT_TRUE(rebooted.load(&loaded));
  EXPECT_EQ(loaded.pageNumber, stable.pageNumber);
  EXPECT_EQ(loaded.wordCursor, stable.wordCursor);
}

TEST(PdfProgressStoreTest, FirstFailedSaveRetriesTheSameFirstSlot) {
  PdfTestCacheIo files;
  files.addDirectory("/cache");
  PdfProgressStore store;
  ASSERT_TRUE(store.initialize(files.io(), "/cache", identity(), 112));

  ReflowReadingPosition attempted;
  attempted.sectionIndex = 0;
  attempted.pageNumber = 2;
  attempted.hasWordCursor = true;
  attempted.wordCursor = 20;
  files.fail(PdfTestFaultPoint::Sync);
  EXPECT_FALSE(store.save(attempted));
  files.clearFault();
  ASSERT_TRUE(files.exists("/cache/progress.a"));
  EXPECT_FALSE(files.exists("/cache/progress.b"));

  ReflowReadingPosition hiddenWhileUnconfirmed;
  EXPECT_FALSE(store.load(&hiddenWhileUnconfirmed));

  ASSERT_TRUE(store.save(attempted));
  EXPECT_TRUE(files.exists("/cache/progress.a"));
  EXPECT_FALSE(files.exists("/cache/progress.b"));
  ReflowReadingPosition loaded;
  ASSERT_TRUE(store.load(&loaded));
  EXPECT_EQ(loaded.pageNumber, attempted.pageNumber);
  EXPECT_EQ(loaded.wordCursor, attempted.wordCursor);
}

TEST(PdfProgressStoreTest, TransientSlotReadFailureNeverStartsAWrite) {
  PdfTestCacheIo files;
  files.addDirectory("/cache");
  PdfProgressStore store;
  ASSERT_TRUE(store.initialize(files.io(), "/cache", identity(), 112));

  ReflowReadingPosition stable;
  stable.sectionIndex = 0;
  stable.pageNumber = 1;
  stable.hasWordCursor = true;
  stable.wordCursor = 10;
  ASSERT_TRUE(store.save(stable));

  const uint32_t writesBefore = files.writeCalls();
  ReflowReadingPosition next = stable;
  next.pageNumber = 2;
  next.wordCursor = 20;
  files.fail(PdfTestFaultPoint::Read);
  EXPECT_EQ(store.save(next).error, PdfError::IoFailure);
  files.clearFault();
  EXPECT_EQ(files.writeCalls(), writesBefore);

  ReflowReadingPosition loaded;
  ASSERT_TRUE(store.load(&loaded));
  EXPECT_EQ(loaded.pageNumber, stable.pageNumber);
  EXPECT_EQ(loaded.wordCursor, stable.wordCursor);
}

TEST(PdfProgressStoreTest, IgnoresSlotsForAChangedSourceEvenWhenWordCountMatches) {
  PdfTestCacheIo files;
  files.addDirectory("/cache");
  PdfProgressStore oldStore;
  ASSERT_TRUE(oldStore.initialize(files.io(), "/cache", identity(), 112));
  ReflowReadingPosition position;
  position.hasSemanticPosition = true;
  position.globalWordOrdinal = 7;
  setAnchor(position, "b00000000");
  ASSERT_TRUE(oldStore.save(position));

  PdfSourceIdentity changed = identity();
  changed.tailFingerprint ^= 1U;
  PdfProgressStore changedStore;
  ASSERT_TRUE(changedStore.initialize(files.io(), "/cache", changed, 112));
  ReflowReadingPosition loaded;
  EXPECT_FALSE(changedStore.load(&loaded));
}

TEST(PdfProgressStoreTest, RoundTripsTrailingEmptyPageCursorAtDocumentEnd) {
  PdfTestCacheIo files;
  files.addDirectory("/cache");
  PdfProgressStore store;
  ASSERT_TRUE(store.initialize(files.io(), "/cache", identity(), 4));

  ReflowReadingPosition trailingEmptyPage;
  trailingEmptyPage.sectionIndex = 0;
  trailingEmptyPage.pageNumber = 4;
  trailingEmptyPage.pageCount = 5;
  trailingEmptyPage.hasPageCount = true;
  trailingEmptyPage.hasWordCursor = true;
  trailingEmptyPage.wordCursor = 4;
  ASSERT_TRUE(store.save(trailingEmptyPage));

  ReflowReadingPosition loaded;
  ASSERT_TRUE(store.load(&loaded));
  EXPECT_FALSE(loaded.hasSemanticPosition);
  EXPECT_TRUE(loaded.hasWordCursor);
  EXPECT_EQ(loaded.wordCursor, 4U);
  EXPECT_EQ(loaded.pageNumber, 4);
  EXPECT_EQ(loaded.pageCount, 5);
}

TEST(PdfProgressStoreTest, RejectsSemanticOrdinalsOutsideTheDocument) {
  PdfTestCacheIo files;
  files.addDirectory("/cache");
  PdfProgressStore store;
  ASSERT_TRUE(store.initialize(files.io(), "/cache", identity(), 10));
  ReflowReadingPosition position;
  position.hasSemanticPosition = true;
  position.globalWordOrdinal = 10;
  setAnchor(position, "b00000000");
  EXPECT_EQ(store.save(position).error, PdfError::InvalidOffset);

  position = {};
  position.hasWordCursor = true;
  position.wordCursor = 11;
  EXPECT_EQ(store.save(position).error, PdfError::InvalidOffset);
}

TEST(PdfReaderProgressStateTest, SeedsLoadedTupleWithoutWritingUntilRelayoutMovesIt) {
  PdfReaderProgressState state;
  state.seedPersistedPosition(2, 7);
  EXPECT_FALSE(state.shouldAttemptSave(2, 7, 100, false));

  state.markPositionDirty();
  EXPECT_TRUE(state.shouldAttemptSave(2, 7, 100, false));
}

TEST(PdfReaderProgressStateTest, ThrottlesFailedAttemptsAndMarksPersistedOnlyAfterSuccess) {
  PdfReaderProgressState state;
  EXPECT_TRUE(state.shouldAttemptSave(1, 3, 100, false));
  state.recordSaveAttempt(1, 3, 100, false);
  EXPECT_FALSE(state.shouldAttemptSave(1, 3, 101, false));
  EXPECT_TRUE(state.shouldAttemptSave(1, 3, 30100, false));

  state.recordSaveAttempt(1, 3, 30100, true);
  EXPECT_FALSE(state.shouldAttemptSave(1, 3, 60100, false));
  EXPECT_TRUE(state.shouldAttemptSave(1, 3, 60100, true));
}

TEST(PdfReaderProgressStateTest, PopulatesValidAndEmptyPagePositionsWithExactCursor) {
  ReflowPageSemanticRange valid;
  valid.valid = true;
  valid.firstGlobalWordOrdinal = 7;
  valid.lastGlobalWordOrdinal = 9;
  valid.firstBlockWordOffset = 2;
  valid.wordCursor = 10;
  std::memcpy(valid.blockAnchor, "b00000003", sizeof("b00000003"));

  ReflowReadingPosition position;
  ASSERT_TRUE(pdfPopulateReadingPositionFromRange(valid, &position));
  EXPECT_TRUE(position.hasSemanticPosition);
  EXPECT_TRUE(position.hasWordCursor);
  EXPECT_EQ(position.globalWordOrdinal, 7U);
  EXPECT_EQ(position.blockWordOffset, 2U);
  EXPECT_EQ(position.wordCursor, 10U);
  EXPECT_STREQ(position.blockAnchor, "b00000003");

  ReflowPageSemanticRange empty;
  empty.wordCursor = 12;
  ASSERT_TRUE(pdfPopulateReadingPositionFromRange(empty, &position));
  EXPECT_FALSE(position.hasSemanticPosition);
  EXPECT_TRUE(position.hasWordCursor);
  EXPECT_EQ(position.wordCursor, 12U);
  EXPECT_EQ(position.blockAnchor[0], '\0');
}

TEST(PdfReaderProgressStateTest, BoundsExactRestoreRetriesAndNeverRequestsLegacyApproximation) {
  PdfReaderProgressState state;
  state.seedPersistedPosition(1, 4);
  EXPECT_TRUE(state.shouldAttemptExactRestore());
  EXPECT_EQ(state.noteDeferredRestoreFailure(), PdfDeferredRestoreAction::RetryExact);
  EXPECT_TRUE(state.shouldDeferSaveForRestoreRetry());
  EXPECT_TRUE(state.shouldAttemptExactRestore());
  EXPECT_FALSE(state.shouldAttemptSave(1, 4, 100, false));

  EXPECT_EQ(state.noteDeferredRestoreFailure(), PdfDeferredRestoreAction::KeepSafePosition);
  EXPECT_TRUE(state.shouldDeferSaveForRestoreRetry());
  EXPECT_FALSE(state.shouldAttemptExactRestore());
  EXPECT_FALSE(state.shouldAttemptSave(1, 4, 30100, false));
  // Automatic forced saves on exit must not replace the exact tuple either.
  EXPECT_FALSE(state.shouldAttemptSave(1, 4, 30100, true));

  // A real page turn is explicit user intent and may replace the old tuple.
  bool pendingRelayout = true;
  state.acceptNavigation(&pendingRelayout);
  EXPECT_FALSE(pendingRelayout);
  EXPECT_TRUE(state.shouldAttemptSave(1, 5, 30100, true));
  EXPECT_FALSE(state.shouldDeferSaveForRestoreRetry());
  EXPECT_EQ(state.deferredRestoreAttempts(), 0U);
}

TEST(PdfReaderProgressStateTest, FailedCapturePreservesExactTupleAndSuppressionUntilAValidCapture) {
  PdfReaderProgressState state;
  ReflowPageSemanticRange original;
  original.valid = true;
  original.firstGlobalWordOrdinal = 17;
  original.firstBlockWordOffset = 3;
  original.wordCursor = 21;
  std::memcpy(original.blockAnchor, "b00000007", sizeof("b00000007"));
  ASSERT_TRUE(state.cacheExactRange(original));
  ASSERT_EQ(state.noteDeferredRestoreFailure(), PdfDeferredRestoreAction::RetryExact);

  ReflowPageSemanticRange corrupt = original;
  std::memset(corrupt.blockAnchor, 'x', sizeof(corrupt.blockAnchor));
  EXPECT_FALSE(state.cacheExactRange(corrupt));
  EXPECT_TRUE(state.cachedHasSemanticPosition);
  EXPECT_TRUE(state.cachedHasWordCursor);
  EXPECT_EQ(state.cachedGlobalWordOrdinal, 17U);
  EXPECT_EQ(state.cachedBlockWordOffset, 3U);
  EXPECT_EQ(state.cachedWordCursor, 21U);
  EXPECT_STREQ(state.cachedBlockAnchor, "b00000007");
  EXPECT_TRUE(state.shouldDeferSaveForRestoreRetry());
  EXPECT_EQ(state.deferredRestoreAttempts(), 1U);

  ReflowPageSemanticRange exactEmpty;
  exactEmpty.wordCursor = 29;
  ASSERT_TRUE(state.cacheExactRange(exactEmpty));
  EXPECT_FALSE(state.cachedHasSemanticPosition);
  EXPECT_TRUE(state.cachedHasWordCursor);
  EXPECT_EQ(state.cachedWordCursor, 29U);
  EXPECT_EQ(state.cachedBlockAnchor[0], '\0');
  EXPECT_FALSE(state.shouldDeferSaveForRestoreRetry());
  EXPECT_EQ(state.deferredRestoreAttempts(), 0U);
}

TEST(PdfReaderProgressStateTest, AcceptedNavigationClearsEveryRestoreAndCurrentPageCacheAtomically) {
  PdfReaderProgressState state;
  ReflowPageSemanticRange range;
  range.valid = true;
  range.firstGlobalWordOrdinal = 9;
  range.firstBlockWordOffset = 2;
  range.wordCursor = 12;
  std::memcpy(range.blockAnchor, "b00000004", sizeof("b00000004"));
  ASSERT_TRUE(state.cacheExactRange(range));
  state.currentPageSemanticRange = range;
  state.semanticRangeSectionIndex = 4;
  state.semanticRangePageNumber = 6;
  state.seedPersistedPosition(4, 6);
  ASSERT_EQ(state.noteDeferredRestoreFailure(), PdfDeferredRestoreAction::RetryExact);
  bool pendingRelayout = true;

  state.acceptNavigation(&pendingRelayout);

  EXPECT_FALSE(pendingRelayout);
  EXPECT_FALSE(state.cachedHasSemanticPosition);
  EXPECT_FALSE(state.cachedHasWordCursor);
  EXPECT_EQ(state.cachedGlobalWordOrdinal, 0U);
  EXPECT_EQ(state.cachedBlockWordOffset, 0U);
  EXPECT_EQ(state.cachedWordCursor, 0U);
  EXPECT_EQ(state.cachedBlockAnchor[0], '\0');
  EXPECT_FALSE(state.currentPageSemanticRange.valid);
  EXPECT_EQ(state.currentPageSemanticRange.wordCursor, 0U);
  EXPECT_EQ(state.semanticRangeSectionIndex, -1);
  EXPECT_EQ(state.semanticRangePageNumber, -1);
  EXPECT_FALSE(state.shouldDeferSaveForRestoreRetry());
  EXPECT_EQ(state.deferredRestoreAttempts(), 0U);
  EXPECT_TRUE(state.shouldAttemptSave(4, 7, 100, true));
}

TEST(PdfReaderProgressStateTest, SeedsAndComparesEveryPersistedExactField) {
  ReflowReadingPosition expected;
  expected.sectionIndex = 3;
  expected.pageNumber = 8;
  expected.pageCount = 13;
  expected.hasPageCount = true;
  expected.hasSemanticPosition = true;
  expected.hasWordCursor = true;
  expected.globalWordOrdinal = 144;
  expected.blockWordOffset = 5;
  expected.wordCursor = 149;
  std::memcpy(expected.blockAnchor, "b00000021", sizeof("b00000021"));

  PdfReaderProgressState state;
  ASSERT_TRUE(state.cacheExactPosition(expected));
  EXPECT_TRUE(state.cachedHasSemanticPosition);
  EXPECT_TRUE(state.cachedHasWordCursor);
  EXPECT_EQ(state.cachedGlobalWordOrdinal, expected.globalWordOrdinal);
  EXPECT_EQ(state.cachedBlockWordOffset, expected.blockWordOffset);
  EXPECT_EQ(state.cachedWordCursor, expected.wordCursor);
  EXPECT_STREQ(state.cachedBlockAnchor, expected.blockAnchor);

  ReflowReadingPosition actual = expected;
  EXPECT_TRUE(pdfReadingPositionsEqualExact(expected, actual));
  actual.sectionIndex++;
  EXPECT_FALSE(pdfReadingPositionsEqualExact(expected, actual));
  actual = expected;
  actual.pageNumber++;
  EXPECT_FALSE(pdfReadingPositionsEqualExact(expected, actual));
  actual = expected;
  actual.pageCount++;
  EXPECT_FALSE(pdfReadingPositionsEqualExact(expected, actual));
  actual = expected;
  actual.hasPageCount = false;
  EXPECT_FALSE(pdfReadingPositionsEqualExact(expected, actual));
  actual = expected;
  actual.hasSemanticPosition = false;
  EXPECT_FALSE(pdfReadingPositionsEqualExact(expected, actual));
  actual = expected;
  actual.hasWordCursor = false;
  EXPECT_FALSE(pdfReadingPositionsEqualExact(expected, actual));
  actual = expected;
  actual.globalWordOrdinal++;
  EXPECT_FALSE(pdfReadingPositionsEqualExact(expected, actual));
  actual = expected;
  actual.blockWordOffset++;
  EXPECT_FALSE(pdfReadingPositionsEqualExact(expected, actual));
  actual = expected;
  actual.wordCursor++;
  EXPECT_FALSE(pdfReadingPositionsEqualExact(expected, actual));
  actual = expected;
  actual.blockAnchor[1] = '9';
  EXPECT_FALSE(pdfReadingPositionsEqualExact(expected, actual));
}

TEST(PdfReaderProgressStateTest, CapturesBoundedEmptyAndSplitFootnoteOrigins) {
  ReflowPageSemanticRange split;
  split.valid = true;
  split.firstGlobalWordOrdinal = 11;
  split.lastGlobalWordOrdinal = 11;
  split.firstBlockWordOffset = 2;
  split.wordCursor = 12;
  std::memcpy(split.blockAnchor, "b00000005", sizeof("b00000005"));

  PdfExactReadingOrigin splitOrigin;
  ASSERT_TRUE(splitOrigin.capture(split, 2, 7, 19));
  EXPECT_TRUE(splitOrigin.valid);
  EXPECT_EQ(splitOrigin.position.sectionIndex, 2);
  EXPECT_EQ(splitOrigin.position.pageNumber, 7);
  EXPECT_EQ(splitOrigin.position.pageCount, 19);
  EXPECT_TRUE(splitOrigin.position.hasPageCount);
  EXPECT_TRUE(splitOrigin.position.hasSemanticPosition);
  EXPECT_TRUE(splitOrigin.position.hasWordCursor);
  EXPECT_EQ(splitOrigin.position.globalWordOrdinal, 11U);
  EXPECT_EQ(splitOrigin.position.blockWordOffset, 2U);
  EXPECT_EQ(splitOrigin.position.wordCursor, 12U);
  EXPECT_STREQ(splitOrigin.position.blockAnchor, "b00000005");

  ReflowPageSemanticRange empty;
  empty.wordCursor = 17;
  PdfExactReadingOrigin emptyOrigin;
  ASSERT_TRUE(emptyOrigin.capture(empty, 2, 8, 19));
  EXPECT_TRUE(emptyOrigin.valid);
  EXPECT_FALSE(emptyOrigin.position.hasSemanticPosition);
  EXPECT_TRUE(emptyOrigin.position.hasWordCursor);
  EXPECT_EQ(emptyOrigin.position.wordCursor, 17U);
  EXPECT_EQ(emptyOrigin.position.blockAnchor[0], '\0');
}

TEST(PdfReaderProgressStateTest, NestedPreviewFallbackCannotOverwriteRootOriginUsedByBackOrBuildFailure) {
  ReflowPageSemanticRange rootRange;
  rootRange.valid = true;
  rootRange.firstGlobalWordOrdinal = 31;
  rootRange.lastGlobalWordOrdinal = 35;
  rootRange.firstBlockWordOffset = 4;
  rootRange.wordCursor = 36;
  std::memcpy(rootRange.blockAnchor, "b00000012", sizeof("b00000012"));

  PdfExactReadingOrigin origins[3];
  ASSERT_TRUE(origins[0].capture(rootRange, 1, 9, 14));
  const ReflowReadingPosition expectedRoot = origins[0].position;
  // A nested link opened from a preview has only a transient preview page.
  // Leaving that slot invalid must not mutate the bounded root entry.
  origins[1] = {};
  EXPECT_FALSE(origins[1].valid);
  EXPECT_TRUE(pdfReadingPositionsEqualExact(origins[0].position, expectedRoot));

  PdfReaderProgressState backRestore;
  bool pendingRelayout = true;
  backRestore.acceptNavigation(&pendingRelayout);
  ASSERT_TRUE(backRestore.cacheExactPosition(origins[0].position));
  EXPECT_TRUE(backRestore.cachedHasSemanticPosition);
  EXPECT_EQ(backRestore.cachedWordCursor, expectedRoot.wordCursor);

  PdfReaderProgressState buildFailureRestore;
  buildFailureRestore.acceptNavigation(&pendingRelayout);
  ASSERT_TRUE(buildFailureRestore.cacheExactPosition(origins[0].position));
  EXPECT_TRUE(buildFailureRestore.cachedHasWordCursor);
  EXPECT_EQ(buildFailureRestore.cachedGlobalWordOrdinal, expectedRoot.globalWordOrdinal);
  EXPECT_STREQ(buildFailureRestore.cachedBlockAnchor, expectedRoot.blockAnchor);
}

TEST(PdfReaderProgressStateTest, DoubleSidecarReadFailureAbortsRelayoutWithoutResettingExactState) {
  ReflowReadingPosition original;
  original.hasSemanticPosition = true;
  original.hasWordCursor = true;
  original.globalWordOrdinal = 91;
  original.blockWordOffset = 5;
  original.wordCursor = 99;
  std::memcpy(original.blockAnchor, "b00000009", sizeof("b00000009"));

  PdfReaderProgressState state;
  ASSERT_TRUE(state.cacheExactPosition(original));
  ASSERT_EQ(state.noteDeferredRestoreFailure(), PdfDeferredRestoreAction::RetryExact);
  ASSERT_TRUE(state.shouldDeferSaveForRestoreRetry());

  // Both explicit reads failed and no matching current-page cache was supplied.
  // The false result is the caller's gate: a reindex must keep the live section.
  const bool mayResetLiveSection = state.cacheRelayoutCapture(nullptr, nullptr, nullptr);

  EXPECT_FALSE(mayResetLiveSection);
  EXPECT_TRUE(state.shouldDeferSaveForRestoreRetry());
  EXPECT_TRUE(state.cachedHasSemanticPosition);
  EXPECT_TRUE(state.cachedHasWordCursor);
  EXPECT_EQ(state.cachedGlobalWordOrdinal, original.globalWordOrdinal);
  EXPECT_EQ(state.cachedBlockWordOffset, original.blockWordOffset);
  EXPECT_EQ(state.cachedWordCursor, original.wordCursor);
  EXPECT_STREQ(state.cachedBlockAnchor, original.blockAnchor);
}

TEST(PdfReaderProgressStateTest, UnresolvedExactRestoreRejectsMatchingVisibleRangeWithoutMutatingTupleOrRetryState) {
  ReflowReadingPosition exactTupleA;
  exactTupleA.hasSemanticPosition = true;
  exactTupleA.hasWordCursor = true;
  exactTupleA.globalWordOrdinal = 91;
  exactTupleA.blockWordOffset = 5;
  exactTupleA.wordCursor = 99;
  std::memcpy(exactTupleA.blockAnchor, "b00000009", sizeof("b00000009"));

  PdfReaderProgressState state;
  ASSERT_TRUE(state.cacheExactPosition(exactTupleA));
  state.seedPersistedPosition(4, 7);
  state.lastProgressAttemptMs = 1234;
  state.hasProgressAttempt = true;
  ASSERT_EQ(state.noteDeferredRestoreFailure(), PdfDeferredRestoreAction::RetryExact);
  ASSERT_EQ(state.deferredRestoreAttempts(), 1U);
  ASSERT_TRUE(state.shouldDeferSaveForRestoreRetry());

  ReflowPageSemanticRange visibleRangeB;
  visibleRangeB.valid = true;
  visibleRangeB.firstGlobalWordOrdinal = 120;
  visibleRangeB.lastGlobalWordOrdinal = 124;
  visibleRangeB.firstBlockWordOffset = 2;
  visibleRangeB.wordCursor = 125;
  std::memcpy(visibleRangeB.blockAnchor, "b00000010", sizeof("b00000010"));
  state.currentPageSemanticRange = visibleRangeB;
  state.semanticRangeSectionIndex = 4;
  state.semanticRangePageNumber = 7;

  ReflowPageSemanticRange sidecarRangeC = visibleRangeB;
  sidecarRangeC.firstGlobalWordOrdinal = 130;
  sidecarRangeC.lastGlobalWordOrdinal = 134;
  sidecarRangeC.firstBlockWordOffset = 3;
  sidecarRangeC.wordCursor = 135;
  std::memcpy(sidecarRangeC.blockAnchor, "b00000011", sizeof("b00000011"));

  EXPECT_FALSE(
      state.cacheRelayoutCapture(&state.currentPageSemanticRange, &sidecarRangeC, &sidecarRangeC));

  EXPECT_TRUE(state.shouldDeferSaveForRestoreRetry());
  EXPECT_EQ(state.deferredRestoreAttempts(), 1U);
  EXPECT_EQ(state.lastProgressAttemptMs, 1234U);
  EXPECT_TRUE(state.hasProgressAttempt);
  EXPECT_EQ(state.lastPersistedSpine, 4);
  EXPECT_EQ(state.lastPersistedPage, 7);
  EXPECT_TRUE(state.cachedHasSemanticPosition);
  EXPECT_TRUE(state.cachedHasWordCursor);
  EXPECT_EQ(state.cachedGlobalWordOrdinal, exactTupleA.globalWordOrdinal);
  EXPECT_EQ(state.cachedBlockWordOffset, exactTupleA.blockWordOffset);
  EXPECT_EQ(state.cachedWordCursor, exactTupleA.wordCursor);
  EXPECT_STREQ(state.cachedBlockAnchor, exactTupleA.blockAnchor);
  EXPECT_EQ(state.semanticRangeSectionIndex, 4);
  EXPECT_EQ(state.semanticRangePageNumber, 7);
  EXPECT_EQ(state.currentPageSemanticRange.firstGlobalWordOrdinal, visibleRangeB.firstGlobalWordOrdinal);
  EXPECT_EQ(state.currentPageSemanticRange.firstBlockWordOffset, visibleRangeB.firstBlockWordOffset);
  EXPECT_EQ(state.currentPageSemanticRange.wordCursor, visibleRangeB.wordCursor);
  EXPECT_STREQ(state.currentPageSemanticRange.blockAnchor, visibleRangeB.blockAnchor);
}

}  // namespace
