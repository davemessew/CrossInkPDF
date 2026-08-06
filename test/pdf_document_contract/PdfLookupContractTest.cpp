#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "PdfObjectResolver.h"
#include "ContractCacheIo.h"
#include "PdfFixedRecordSpool.h"
#include "PdfPageTree.h"
#include "PdfTestIo.h"
#include "PdfXref.h"

namespace {

struct SliceIoMetrics {
  uint64_t operations = 0;
  uint64_t bytes = 0;
  uint32_t nowMs = 0;
  uint32_t sliceStartedAtMs = 0;
  uint32_t cancelAfterWrites = UINT32_MAX;
  uint32_t writes = 0;

  static bool sliceExpired(void* context) {
    const auto& metrics = *static_cast<SliceIoMetrics*>(context);
    return metrics.nowMs - metrics.sliceStartedAtMs >= 5U;
  }

  static bool cancelRequested(void* context) {
    const auto& metrics = *static_cast<SliceIoMetrics*>(context);
    return metrics.writes >= metrics.cancelAfterWrites;
  }

  void record(const size_t transferred) {
    ++operations;
    bytes += transferred;
    ++nowMs;
  }
};

struct CountingByteSource {
  explicit CountingByteSource(std::vector<uint8_t> data, SliceIoMetrics* const observed)
      : bytes(std::move(data)), metrics(observed) {}

  static PdfStatus readAt(void* context, const uint64_t offset, uint8_t* destination, const size_t requested,
                          size_t* bytesRead) {
    if (context == nullptr || destination == nullptr || bytesRead == nullptr) {
      return PdfStatus::failure(PdfError::InvalidArgument, offset);
    }
    auto& source = *static_cast<CountingByteSource*>(context);
    if (offset >= source.bytes.size()) {
      *bytesRead = 0;
      source.metrics->record(0);
      return PdfStatus::success();
    }
    *bytesRead = std::min(requested, source.bytes.size() - static_cast<size_t>(offset));
    std::memcpy(destination, source.bytes.data() + offset, *bytesRead);
    source.metrics->record(*bytesRead);
    return PdfStatus::success();
  }

  PdfByteSource source() { return {this, bytes.size(), readAt}; }

  std::vector<uint8_t> bytes;
  SliceIoMetrics* metrics = nullptr;
};

struct LogicalXrefStore {
  static PdfStatus read(void* context, const uint32_t ordinal, void* record, const size_t recordSize) {
    if (context == nullptr || record == nullptr || recordSize != sizeof(PdfXrefEntry) || ordinal >= 100001U) {
      return PdfStatus::failure(PdfError::InvalidArgument, ordinal);
    }
    auto& store = *static_cast<LogicalXrefStore*>(context);
    ++store.reads;
    store.bytes += recordSize;
    *static_cast<PdfXrefEntry*>(record) = {ordinal, 0, PdfXrefEntryType::Uncompressed, 0, ordinal + 1U, 0};
    return PdfStatus::success();
  }

  static PdfStatus write(void*, uint32_t, const void*, size_t) {
    return PdfStatus::failure(PdfError::InvalidArgument);
  }

  PdfFixedRecordStore fixed() { return {this, 100001U, sizeof(PdfXrefEntry), read, write}; }

  uint32_t reads = 0;
  uint64_t bytes = 0;
};

struct CountingXrefStore {
  explicit CountingXrefStore(const uint32_t capacity, SliceIoMetrics* const observed = nullptr)
      : entries(capacity), metrics(observed) {}

  static PdfStatus read(void* context, const uint32_t ordinal, void* record, const size_t recordSize) {
    if (context == nullptr || record == nullptr || recordSize != sizeof(PdfXrefEntry)) {
      return PdfStatus::failure(PdfError::InvalidArgument, ordinal);
    }
    auto& store = *static_cast<CountingXrefStore*>(context);
    if (ordinal >= store.entries.size()) {
      return PdfStatus::failure(PdfError::InvalidArgument, ordinal);
    }
    std::memcpy(record, &store.entries[ordinal], recordSize);
    ++store.reads;
    if (store.metrics != nullptr) {
      store.metrics->record(recordSize);
    }
    return PdfStatus::success();
  }

  static PdfStatus write(void* context, const uint32_t ordinal, const void* record, const size_t recordSize) {
    if (context == nullptr || record == nullptr || recordSize != sizeof(PdfXrefEntry)) {
      return PdfStatus::failure(PdfError::InvalidArgument, ordinal);
    }
    auto& store = *static_cast<CountingXrefStore*>(context);
    if (ordinal >= store.entries.size()) {
      return PdfStatus::failure(PdfError::InvalidArgument, ordinal);
    }
    std::memcpy(&store.entries[ordinal], record, recordSize);
    ++store.writes;
    if (store.metrics != nullptr) {
      ++store.metrics->writes;
      store.metrics->record(recordSize);
    }
    return PdfStatus::success();
  }

  PdfFixedRecordStore fixed() {
    return {this, static_cast<uint32_t>(entries.size()), sizeof(PdfXrefEntry), read, write};
  }

  std::vector<PdfXrefEntry> entries;
  uint32_t reads = 0;
  uint32_t writes = 0;
  SliceIoMetrics* metrics = nullptr;
};

std::vector<uint8_t> loadXrefStreamFixture() {
  const std::filesystem::path path = std::filesystem::path(__FILE__).parent_path().parent_path() /
                                     "pdf_reflow_core" / "fixtures" / "xref_stream_objstm.pdf";
  std::ifstream input(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

std::vector<uint8_t> oneObjectClassicPdf() {
  std::string pdf = "%PDF-1.4\n";
  const size_t objectOffset = pdf.size();
  pdf += "1 0 obj\n<< /Type /Catalog >>\nendobj\n";
  const size_t xrefOffset = pdf.size();
  char xref[256]{};
  const int written = std::snprintf(xref, sizeof(xref),
                                    "xref\n0 2\n0000000000 65535 f\n%010llu 00000 n\n"
                                    "trailer\n<< /Size 2 /Root 1 0 R >>\nstartxref\n%llu\n%%%%EOF\n",
                                    static_cast<unsigned long long>(objectOffset),
                                    static_cast<unsigned long long>(xrefOffset));
  EXPECT_GT(written, 0);
  EXPECT_LT(static_cast<size_t>(written), sizeof(xref));
  pdf.append(xref, static_cast<size_t>(written));
  return {pdf.begin(), pdf.end()};
}

struct ResolverHarness {
  ResolverHarness()
      : decoder({decoderSource.data(), decoderSource.size(), decoderOutput.data(), decoderOutput.size(),
                 inflateDictionary.data(), inflateDictionary.size()}) {
    records.forbidReadsWhile(&xrefBlocked);
    objectStream.forbidReadsWhile(&externalReaderOpen);
  }

  static PdfStatus setSourceAccess(void* context, const PdfObjectResolverReader reader) {
    if (context == nullptr) {
      return PdfStatus::failure(PdfError::InvalidArgument);
    }
    auto& harness = *static_cast<ResolverHarness*>(context);
    harness.reader = reader;
    harness.sourceOpen = reader == PdfObjectResolverReader::Source;
    harness.xrefBlocked = reader != PdfObjectResolverReader::Xref;
    harness.externalReaderOpen = reader != PdfObjectResolverReader::ObjectStore;
    harness.transitions.push_back(harness.sourceOpen);
    return PdfStatus::success();
  }

  static PdfStatus setTraversalAccess(void* context, const bool required) {
    if (context == nullptr) {
      return PdfStatus::failure(PdfError::InvalidArgument);
    }
    auto& harness = *static_cast<ResolverHarness*>(context);
    harness.traversalOpen = required;
    // Opening the traversal store evicts either resolver reader. Closing it
    // leaves no reader selected until the resolver explicitly reasserts one.
    harness.sourceOpen = false;
    harness.xrefBlocked = true;
    harness.externalReaderOpen = false;
    return PdfStatus::success();
  }

  static PdfStatus capturePage(void* context, const PdfPageInfo&) {
    if (context == nullptr) {
      return PdfStatus::failure(PdfError::InvalidArgument);
    }
    ++static_cast<ResolverHarness*>(context)->pageCount;
    return PdfStatus::success();
  }

  PdfObjectResolverWorkspace workspace(const bool withDecoder = false) {
    return {withDecoder ? &decoder : nullptr, objectStream.store(), this, setSourceAccess};
  }

  std::array<uint8_t, 4096> sourceBuffer{};
  std::array<uint8_t, 4096> decoderSource{};
  std::array<uint8_t, 4096> decoderOutput{};
  std::array<uint8_t, 32768> inflateDictionary{};
  PdfStreamDecoder decoder;
  std::array<PdfValue, 64> values{};
  std::array<PdfDictionaryEntry, 64> dictionaries{};
  std::array<PdfArrayItem, 64> arrays{};
  std::array<uint8_t, 1024> text{};
  PdfObjectArena arena{values.data(),       static_cast<uint16_t>(values.size()),
                       dictionaries.data(), static_cast<uint16_t>(dictionaries.size()),
                       arrays.data(),       static_cast<uint16_t>(arrays.size()),
                       text.data(),         static_cast<uint16_t>(text.size())};
  PdfTestRecordStore records{sizeof(PdfXrefEntry), 16};
  PdfXrefTable xref{records.store()};
  PdfTestByteStore objectStream{4096};
  PdfObjectResolverReader reader = PdfObjectResolverReader::Source;
  bool sourceOpen = true;
  bool xrefBlocked = true;
  bool externalReaderOpen = true;
  bool traversalOpen = false;
  uint32_t pageCount = 0;
  std::vector<bool> transitions;
};

struct XrefStreamWorkspace {
  XrefStreamWorkspace()
      : decoder({decoderSource.data(), decoderSource.size(), decoderOutput.data(), decoderOutput.size(),
                 inflateDictionary.data(), inflateDictionary.size()}) {}

  PdfObjectArena arena() {
    return {values.data(),       static_cast<uint16_t>(values.size()),
            dictionaries.data(), static_cast<uint16_t>(dictionaries.size()),
            arrays.data(),       static_cast<uint16_t>(arrays.size()),
            text.data(),         static_cast<uint16_t>(text.size())};
  }

  std::array<uint8_t, 4096> sourceBuffer{};
  std::array<uint8_t, 4096> decoderSource{};
  std::array<uint8_t, 4096> decoderOutput{};
  std::array<uint8_t, 32768> inflateDictionary{};
  PdfStreamDecoder decoder;
  std::array<PdfValue, 64> values{};
  std::array<PdfDictionaryEntry, 64> dictionaries{};
  std::array<PdfArrayItem, 64> arrays{};
  std::array<uint8_t, 1024> text{};
};

PdfStepResult runResolver(PdfObjectResolver& resolver) {
  for (uint16_t step = 0; step < 256; ++step) {
    PdfWorkBudget budget{32, 4096};
    const PdfStepResult result = resolver.step(budget);
    if (!result.yielded()) {
      return result;
    }
  }
  return PdfStepResult::failure(PdfStatus::failure(PdfError::BudgetExhausted));
}

TEST(PdfXrefAppendBudgetContract, ClassicEntryWaitsForOneRecordOperationAndExactRecordBytes) {
  PdfTestByteSource memory(oneObjectClassicPdf());
  const PdfByteSource source = memory.source();
  CountingXrefStore records(2);
  PdfXrefTable table(records.fixed());
  std::array<uint8_t, 4096> sourceBuffer{};
  std::array<PdfValue, 64> values{};
  std::array<PdfDictionaryEntry, 64> dictionaries{};
  std::array<PdfArrayItem, 64> arrays{};
  std::array<uint8_t, 1024> text{};
  PdfObjectArena arena{values.data(),       static_cast<uint16_t>(values.size()),
                       dictionaries.data(), static_cast<uint16_t>(dictionaries.size()),
                       arrays.data(),       static_cast<uint16_t>(arrays.size()),
                       text.data(),         static_cast<uint16_t>(text.size())};
  PdfXrefParser parser(source, sourceBuffer.data(), sourceBuffer.size(), arena, table, nullptr);
  parser.begin();

  bool waitingForRecordBudget = false;
  for (uint16_t step = 0; step < 512; ++step) {
    PdfWorkBudget shortRecord{32, sizeof(PdfXrefEntry) - 1U};
    const PdfStepResult result = parser.step(shortRecord);
    ASSERT_TRUE(result.yielded()) << "step=" << step << " error=" << static_cast<int>(result.status.error);
    ASSERT_EQ(records.writes, 0U);
    if (shortRecord.operationsRemaining == 32U && shortRecord.bytesRemaining == sizeof(PdfXrefEntry) - 1U) {
      waitingForRecordBudget = true;
      break;
    }
  }
  ASSERT_TRUE(waitingForRecordBudget);

  PdfWorkBudget noOperation{0, sizeof(PdfXrefEntry)};
  EXPECT_TRUE(parser.step(noOperation).yielded());
  EXPECT_EQ(records.writes, 0U);

  PdfWorkBudget exactRecord{1, sizeof(PdfXrefEntry)};
  const PdfStepResult appendResult = parser.step(exactRecord);
  EXPECT_FALSE(appendResult.failed());
  EXPECT_EQ(records.writes, 1U);
  EXPECT_EQ(exactRecord.operationsRemaining, 0U);
  EXPECT_EQ(exactRecord.bytesRemaining, 0U);
}

TEST(PdfXrefAppendBudgetContract, SparseStreamAcceptsShortPrefixesWithoutDropsInsideBoundedSlices) {
  std::vector<uint8_t> fixture = loadXrefStreamFixture();
  ASSERT_FALSE(fixture.empty());
  SliceIoMetrics metrics;
  CountingByteSource input(std::move(fixture), &metrics);
  const PdfByteSource source = input.source();
  CountingXrefStore records(16, &metrics);
  PdfXrefTable table(records.fixed());
  XrefStreamWorkspace workspace;
  PdfObjectArena arena = workspace.arena();
  PdfXrefParser parser(source, workspace.sourceBuffer.data(), workspace.sourceBuffer.size(), arena, table,
                       &workspace.decoder);
  parser.begin();

  uint32_t maximumWritesPerSlice = 0;
  PdfStepResult result = PdfStepResult::paused();
  for (uint16_t step = 0; step < 4096U && result.yielded(); ++step) {
    const uint64_t operationsBefore = metrics.operations;
    const uint64_t bytesBefore = metrics.bytes;
    const uint32_t writesBefore = records.writes;
    const uint32_t timeBefore = metrics.nowMs;
    metrics.sliceStartedAtMs = timeBefore;
    PdfWorkBudget budget{32, 4096, nullptr, nullptr, &metrics, SliceIoMetrics::sliceExpired};
    result = parser.step(budget);
    const uint64_t actualOperations = metrics.operations - operationsBefore;
    const uint64_t actualBytes = metrics.bytes - bytesBefore;
    const uint32_t elapsed = metrics.nowMs - timeBefore;
    const uint32_t writes = records.writes - writesBefore;
    maximumWritesPerSlice = std::max(maximumWritesPerSlice, writes);
    EXPECT_LE(32U - budget.operationsRemaining, 32U) << "step=" << step;
    EXPECT_LE(4096U - budget.bytesRemaining, 4096U) << "step=" << step;
    EXPECT_LE(actualOperations, 5U) << "step=" << step;
    EXPECT_LE(actualBytes, 4096U) << "step=" << step;
    EXPECT_LE(elapsed, 5U) << "step=" << step;
  }

  ASSERT_TRUE(result.complete()) << static_cast<int>(result.status.error);
  ASSERT_GT(table.entryCount(), 1U);
  EXPECT_GT(maximumWritesPerSlice, 1U);
  ASSERT_EQ(records.writes, table.entryCount());
  for (uint32_t left = 0; left < table.entryCount(); ++left) {
    for (uint32_t right = left + 1U; right < table.entryCount(); ++right) {
      EXPECT_NE(records.entries[left].objectNumber, records.entries[right].objectNumber);
    }
  }
  PdfXrefEntry catalog{};
  ASSERT_TRUE(table.find(1, &catalog).ok());
  EXPECT_EQ(catalog.type, PdfXrefEntryType::Compressed);
  EXPECT_EQ(catalog.offset, 6U);
  EXPECT_EQ(catalog.objectStreamIndex, 0U);
}

TEST(PdfXrefAppendBudgetContract, SparseStreamCancellationResetsAndReplaysFromTheBeginning) {
  std::vector<uint8_t> fixture = loadXrefStreamFixture();
  ASSERT_FALSE(fixture.empty());
  SliceIoMetrics metrics;
  metrics.cancelAfterWrites = 1;
  CountingByteSource input(std::move(fixture), &metrics);
  const PdfByteSource source = input.source();
  CountingXrefStore records(16, &metrics);
  PdfXrefTable table(records.fixed());
  XrefStreamWorkspace workspace;
  PdfObjectArena arena = workspace.arena();
  PdfXrefParser parser(source, workspace.sourceBuffer.data(), workspace.sourceBuffer.size(), arena, table,
                       &workspace.decoder);
  parser.begin();

  PdfStepResult cancelled = PdfStepResult::paused();
  for (uint16_t step = 0; step < 4096U && cancelled.yielded(); ++step) {
    PdfWorkBudget budget{32, 4096, &metrics, SliceIoMetrics::cancelRequested};
    cancelled = parser.step(budget);
  }
  ASSERT_TRUE(cancelled.failed());
  EXPECT_EQ(cancelled.status.error, PdfError::Cancelled);
  EXPECT_EQ(table.entryCount(), 1U);

  metrics.cancelAfterWrites = UINT32_MAX;
  parser.begin();
  PdfStepResult replay = PdfStepResult::paused();
  for (uint16_t step = 0; step < 4096U && replay.yielded(); ++step) {
    PdfWorkBudget budget{32, 4096};
    replay = parser.step(budget);
  }
  ASSERT_TRUE(replay.complete()) << static_cast<int>(replay.status.error);
  EXPECT_GT(table.entryCount(), 1U);
  PdfXrefEntry content{};
  ASSERT_TRUE(table.find(5, &content).ok());
  EXPECT_EQ(content.type, PdfXrefEntryType::Uncompressed);
  EXPECT_LT(content.offset, source.size);
}

TEST(PdfXrefNewestObjectFilterContract, SegmentedBoundariesResetAndDetachStayWithinCallerOwnedSpans) {
  constexpr size_t kPrimaryBytes = 12'288U;
  constexpr size_t kTailBytes = 213U;
  std::array<uint8_t, kPrimaryBytes> primary{};
  std::array<uint8_t, kTailBytes + 1U> tail{};
  primary.fill(0xa5U);
  tail.fill(0xa5U);

  PdfTestRecordStore records(sizeof(PdfXrefEntry), 2);
  PdfXrefTable table(records.store());
  ASSERT_TRUE(table.configureNewestObjectFilter(primary.data(), primary.size(), tail.data(), kTailBytes).ok());
  EXPECT_TRUE(std::all_of(primary.begin(), primary.end(), [](const uint8_t value) { return value == 0; }));
  EXPECT_TRUE(std::all_of(tail.begin(), tail.begin() + kTailBytes, [](const uint8_t value) { return value == 0; }));
  EXPECT_EQ(tail.back(), 0xa5U);

  const PdfXrefEntry objectZero{0, 65'535U, PdfXrefEntryType::Free, 0, 0, 0};
  const PdfXrefEntry objectMaximum{100'000U, 0, PdfXrefEntryType::Uncompressed, 0, 77, 0};
  ASSERT_TRUE(table.appendNewest(objectZero).ok());
  ASSERT_TRUE(table.appendNewest(objectMaximum).ok());
  ASSERT_TRUE(table.appendNewest({0, 0, PdfXrefEntryType::Free, 0, 1, 0}).ok());
  ASSERT_TRUE(table.appendNewest({100'000U, 0, PdfXrefEntryType::Uncompressed, 0, 88, 0}).ok());
  EXPECT_EQ(table.entryCount(), 2U);
  PdfXrefEntry found{};
  ASSERT_TRUE(table.find(100'000U, &found).ok());
  EXPECT_EQ(found.offset, 77U);

  table.reset();
  EXPECT_TRUE(std::all_of(primary.begin(), primary.end(), [](const uint8_t value) { return value == 0; }));
  EXPECT_TRUE(std::all_of(tail.begin(), tail.begin() + kTailBytes, [](const uint8_t value) { return value == 0; }));
  EXPECT_EQ(tail.back(), 0xa5U);
  EXPECT_EQ(table.appendNewest({100'001U, 0, PdfXrefEntryType::Free, 0, 0, 0}).error,
            PdfError::InvalidArgument);
  ASSERT_TRUE(table.appendNewest(objectZero).ok());
  ASSERT_TRUE(table.appendNewest(objectMaximum).ok());
  EXPECT_EQ(table.entryCount(), 2U);

  primary.fill(0x3cU);
  tail.fill(0x3cU);
  table.detachNewestObjectFilter();
  table.reset();
  EXPECT_TRUE(std::all_of(primary.begin(), primary.end(), [](const uint8_t value) { return value == 0x3cU; }));
  EXPECT_TRUE(std::all_of(tail.begin(), tail.end(), [](const uint8_t value) { return value == 0x3cU; }));
}

TEST(PdfXrefNewestObjectFilterContract, RejectsAggregateSpanSmallerThanExactObjectDomain) {
  std::array<uint8_t, 12'288U> primary{};
  std::array<uint8_t, 212U> shortTail{};
  PdfTestRecordStore records(sizeof(PdfXrefEntry), 1);
  PdfXrefTable table(records.store());

  EXPECT_EQ(table.configureNewestObjectFilter(primary.data(), primary.size(), shortTail.data(), shortTail.size()).error,
            PdfError::InvalidArgument);
  EXPECT_EQ(table.entryCount(), 0U);
}

TEST(PdfXrefAdoptContract, FindsExternallySortedFirstMiddleAndLastWithoutScanningOnAdopt) {
  PdfTestRecordStore records(sizeof(PdfXrefEntry), 8);
  const PdfFixedRecordStore store = records.store();
  const std::array<PdfXrefEntry, 3> sorted{{
      {1, 0, PdfXrefEntryType::Uncompressed, 0, 10, 0},
      {25, 2, PdfXrefEntryType::Uncompressed, 0, 250, 0},
      {99, 0, PdfXrefEntryType::Compressed, 0, 50, 3},
  }};
  for (uint32_t index = 0; index < sorted.size(); ++index) {
    ASSERT_TRUE(pdfWriteRecord(store, index, &sorted[index]).ok());
  }
  PdfXrefTable table(store);
  table.setRoot({1, 0});
  table.setInfo({25, 2});

  ASSERT_TRUE(table.adoptSortedRecords(static_cast<uint32_t>(sorted.size())).ok());
  EXPECT_EQ(records.readCount(), 0U);
  EXPECT_TRUE(table.finalized());
  EXPECT_EQ(table.entryCount(), sorted.size());

  for (const PdfXrefEntry& expected : sorted) {
    PdfXrefEntry actual{};
    ASSERT_TRUE(table.find(expected.objectNumber, &actual).ok());
    EXPECT_EQ(actual.objectNumber, expected.objectNumber);
    EXPECT_EQ(actual.generation, expected.generation);
    EXPECT_EQ(actual.type, expected.type);
    EXPECT_EQ(actual.offset, expected.offset);
  }
  PdfObjectReference reference{};
  ASSERT_TRUE(table.root(&reference));
  EXPECT_EQ(reference, (PdfObjectReference{1, 0}));
  ASSERT_TRUE(table.info(&reference));
  EXPECT_EQ(reference, (PdfObjectReference{25, 2}));
}

TEST(PdfXrefAdoptContract, RejectsZeroOverflowPopulatedAndFinalizedMisuse) {
  PdfTestRecordStore records(sizeof(PdfXrefEntry), 2);
  const PdfFixedRecordStore store = records.store();

  PdfXrefTable zero(store);
  EXPECT_EQ(zero.adoptSortedRecords(0).error, PdfError::InvalidArgument);

  PdfXrefTable overflow(store);
  EXPECT_EQ(overflow.adoptSortedRecords(3).error, PdfError::InvalidArgument);

  PdfXrefTable populated(store);
  ASSERT_TRUE(populated.appendNewest({1, 0, PdfXrefEntryType::Uncompressed, 0, 1, 0}).ok());
  EXPECT_EQ(populated.adoptSortedRecords(1).error, PdfError::InvalidArgument);

  PdfXrefTable finalized(store);
  ASSERT_TRUE(finalized.adoptSortedRecords(1).ok());
  EXPECT_EQ(finalized.adoptSortedRecords(1).error, PdfError::InvalidArgument);
}

TEST(PdfXrefAdoptContract, MaximumLogicalCapacityLookupUsesAtMostEighteenExactRecords) {
  LogicalXrefStore records;
  PdfXrefTable table(records.fixed());
  ASSERT_TRUE(table.adoptSortedRecords(100001U).ok());

  PdfXrefEntry entry{};
  ASSERT_TRUE(table.find(100000U, &entry).ok());
  EXPECT_EQ(entry.objectNumber, 100000U);
  EXPECT_LE(records.reads, 18U);
  EXPECT_LE(records.bytes, 18U * sizeof(PdfXrefEntry));
}

TEST(PdfXrefLookupBudgetContract, MaximumLookupYieldsAfterOneExactRecordWithoutExceedingBudget) {
  LogicalXrefStore records;
  PdfXrefTable table(records.fixed());
  ASSERT_TRUE(table.adoptSortedRecords(100'001U).ok());
  PdfXrefLookupState state{};
  ASSERT_TRUE(table.beginFind(100'000U, &state).ok());

  PdfXrefEntry entry{};
  PdfStepResult result = PdfStepResult::paused();
  uint32_t steps = 0;
  while (result.yielded() && steps++ < 32U) {
    const uint32_t readsBefore = records.reads;
    const uint64_t bytesBefore = records.bytes;
    PdfWorkBudget budget{1, sizeof(PdfXrefEntry)};
    result = table.stepFind(state, &entry, budget);
    EXPECT_LE(records.reads - readsBefore, 1U);
    EXPECT_LE(records.bytes - bytesBefore, sizeof(PdfXrefEntry));
  }

  ASSERT_TRUE(result.complete());
  EXPECT_EQ(entry.objectNumber, 100'000U);
  EXPECT_LE(steps, 18U);
  EXPECT_LE(records.reads, 18U);
}

TEST(PdfXrefLookupBudgetContract, LinearLookupDoesNotAdvancePastRecordWhenBudgetYields) {
  PdfTestRecordStore records(sizeof(PdfXrefEntry), 3);
  PdfXrefTable table(records.store());
  ASSERT_TRUE(table.appendNewest({7, 0, PdfXrefEntryType::Uncompressed, 0, 7, 0}).ok());
  ASSERT_TRUE(table.appendNewest({2, 0, PdfXrefEntryType::Uncompressed, 0, 2, 0}).ok());
  ASSERT_TRUE(table.appendNewest({9, 0, PdfXrefEntryType::Uncompressed, 0, 9, 0}).ok());
  PdfXrefLookupState state{};
  ASSERT_TRUE(table.beginFind(2, &state).ok());
  PdfXrefEntry entry{};

  PdfWorkBudget empty{0, 0};
  EXPECT_TRUE(table.stepFind(state, &entry, empty).yielded());
  EXPECT_EQ(records.readCount(), 0U);
  PdfWorkBudget first{1, sizeof(PdfXrefEntry)};
  EXPECT_TRUE(table.stepFind(state, &entry, first).yielded());
  EXPECT_EQ(records.readCount(), 1U);
  PdfWorkBudget second{1, sizeof(PdfXrefEntry)};
  ASSERT_TRUE(table.stepFind(state, &entry, second).complete());
  EXPECT_EQ(records.readCount(), 2U);
  EXPECT_EQ(entry.objectNumber, 2U);
}

TEST(PdfFixedRecordSpoolContract, GenericReadWriteOpenIsRejectedWithoutHandleOrMutation) {
  static_assert(!std::is_copy_constructible_v<PdfFixedRecordSpool>);
  static_assert(!std::is_move_constructible_v<PdfFixedRecordSpool>);
  ContractCacheIo storage;
  const PdfCacheIo io = storage.io();
  PdfFixedRecordSpool spool;
  ASSERT_TRUE(spool.configure(&io, sizeof(PdfXrefEntry), 4).ok());
  ASSERT_TRUE(spool.open("/spool", PdfCacheOpenMode::WriteTruncate).ok());
  const PdfXrefEntry first{1, 0, PdfXrefEntryType::Uncompressed, 0, 12, 0};
  ASSERT_TRUE(pdfWriteRecord(spool.store(), 0, &first).ok());
  ASSERT_TRUE(spool.close().ok());

  const std::vector<uint8_t> before = storage.bytes("/spool");
  EXPECT_EQ(spool.open("/spool", PdfCacheOpenMode::ReadWrite, 1).error, PdfError::InvalidArgument);
  EXPECT_EQ(storage.openHandleCount(), 0U);
  EXPECT_EQ(storage.bytes("/spool"), before);
}

TEST(PdfFixedRecordSpoolContract, BatchAppendUsesOneHalWriteAndPreservesEveryRecord) {
  ContractCacheIo storage;
  const PdfCacheIo io = storage.io();
  PdfFixedRecordSpool spool;
  ASSERT_TRUE(spool.configure(&io, sizeof(PdfXrefEntry), 4).ok());
  ASSERT_TRUE(spool.open("/batch", PdfCacheOpenMode::WriteTruncate).ok());
  const std::array<PdfXrefEntry, 3> expected{{
      {1, 0, PdfXrefEntryType::Uncompressed, 0, 12, 0},
      {2, 0, PdfXrefEntryType::Compressed, 0, 7, 3},
      {3, 2, PdfXrefEntryType::Free, 0, 0, 0},
  }};
  const uint32_t writesBefore = storage.metrics().writes;

  ASSERT_TRUE(spool.appendRecords(expected.data(), static_cast<uint32_t>(expected.size())).ok());
  EXPECT_EQ(storage.metrics().writes - writesBefore, 1U);
  EXPECT_EQ(spool.recordCount(), expected.size());
  EXPECT_EQ(spool.writeOperations(), expected.size());
  ASSERT_TRUE(spool.close().ok());
  ASSERT_TRUE(spool.open("/batch", PdfCacheOpenMode::Read, static_cast<uint32_t>(expected.size())).ok());
  for (uint32_t index = 0; index < expected.size(); ++index) {
    PdfXrefEntry observed{};
    ASSERT_TRUE(pdfReadRecord(spool.store(), index, &observed).ok());
    EXPECT_EQ(observed.objectNumber, expected[index].objectNumber);
    EXPECT_EQ(observed.generation, expected[index].generation);
    EXPECT_EQ(observed.type, expected[index].type);
    EXPECT_EQ(observed.offset, expected[index].offset);
    EXPECT_EQ(observed.objectStreamIndex, expected[index].objectStreamIndex);
  }
}

TEST(PdfFixedRecordSpoolContract, UpdateModeReadsAndRewritesExactlyOneExistingMiddleRecord) {
  ContractCacheIo storage;
  const PdfCacheIo io = storage.io();
  PdfFixedRecordSpool spool;
  ASSERT_TRUE(spool.configure(&io, sizeof(PdfXrefEntry), 4).ok());
  ASSERT_TRUE(spool.open("/updates", PdfCacheOpenMode::WriteTruncate).ok());
  const std::array<PdfXrefEntry, 3> original{{
      {1, 0, PdfXrefEntryType::Uncompressed, 0, 12, 0},
      {2, 0, PdfXrefEntryType::Uncompressed, 0, 24, 0},
      {3, 0, PdfXrefEntryType::Uncompressed, 0, 36, 0},
  }};
  for (uint32_t index = 0; index < original.size(); ++index) {
    ASSERT_TRUE(pdfWriteRecord(spool.store(), index, &original[index]).ok());
  }
  ASSERT_TRUE(spool.close().ok());
  ASSERT_TRUE(spool.openForUpdates("/updates", static_cast<uint32_t>(original.size())).ok());

  PdfXrefEntry observed{};
  ASSERT_TRUE(pdfReadRecord(spool.store(), 1, &observed).ok());
  EXPECT_EQ(observed.objectNumber, 2U);
  const PdfXrefEntry replacement{20, 0, PdfXrefEntryType::Uncompressed, 0, 240, 0};
  ASSERT_TRUE(spool.rewriteExisting(1, &replacement, sizeof(replacement)).ok());
  EXPECT_EQ(pdfWriteRecord(spool.store(), 3, &replacement).error, PdfError::InvalidOffset);
  ASSERT_TRUE(spool.close().ok());
  ASSERT_TRUE(spool.open("/updates", PdfCacheOpenMode::Read, 3).ok());

  std::array<PdfXrefEntry, 3> final{};
  for (uint32_t index = 0; index < final.size(); ++index) {
    ASSERT_TRUE(pdfReadRecord(spool.store(), index, &final[index]).ok());
  }
  EXPECT_EQ(final[0].objectNumber, original[0].objectNumber);
  EXPECT_EQ(final[1].objectNumber, replacement.objectNumber);
  EXPECT_EQ(final[1].offset, replacement.offset);
  EXPECT_EQ(final[2].objectNumber, original[2].objectNumber);
}

TEST(PdfFixedRecordSpoolContract, UpdateModeRejectsOutOfRangeRewriteWithoutMutation) {
  ContractCacheIo storage;
  const PdfCacheIo io = storage.io();
  PdfFixedRecordSpool spool;
  ASSERT_TRUE(spool.configure(&io, sizeof(PdfXrefEntry), 2).ok());
  ASSERT_TRUE(spool.open("/bounded-updates", PdfCacheOpenMode::WriteTruncate).ok());
  const PdfXrefEntry original{1, 0, PdfXrefEntryType::Uncompressed, 0, 12, 0};
  ASSERT_TRUE(pdfWriteRecord(spool.store(), 0, &original).ok());
  ASSERT_TRUE(spool.close().ok());
  const std::vector<uint8_t> before = storage.bytes("/bounded-updates");
  ASSERT_TRUE(spool.openForUpdates("/bounded-updates", 1).ok());
  const PdfXrefEntry replacement{2, 0, PdfXrefEntryType::Uncompressed, 0, 24, 0};

  EXPECT_EQ(spool.rewriteExisting(1, &replacement, sizeof(replacement)).error, PdfError::InvalidOffset);
  EXPECT_TRUE(spool.isOpen());
  EXPECT_EQ(storage.bytes("/bounded-updates"), before);
}

TEST(PdfFixedRecordSpoolContract, PartialUpdateWriteErrorAbortClosesHandle) {
  ContractCacheIo storage;
  const PdfCacheIo io = storage.io();
  PdfFixedRecordSpool spool;
  ASSERT_TRUE(spool.configure(&io, sizeof(PdfXrefEntry), 2).ok());
  ASSERT_TRUE(spool.open("/failed-update", PdfCacheOpenMode::WriteTruncate).ok());
  const PdfXrefEntry original{1, 0, PdfXrefEntryType::Uncompressed, 0, 12, 0};
  ASSERT_TRUE(pdfWriteRecord(spool.store(), 0, &original).ok());
  ASSERT_TRUE(spool.close().ok());
  ASSERT_TRUE(spool.openForUpdates("/failed-update", 1).ok());
  storage.failNextWriteAfter(3);
  const PdfXrefEntry replacement{2, 0, PdfXrefEntryType::Uncompressed, 0, 24, 0};

  EXPECT_EQ(spool.rewriteExisting(0, &replacement, sizeof(replacement)).error, PdfError::IoFailure);
  EXPECT_FALSE(spool.isOpen());
  EXPECT_EQ(storage.openHandleCount(), 0U);
}

TEST(PdfFixedRecordSpoolContract, PartialHalWriteErrorAbortClosesWriteTruncateHandle) {
  ContractCacheIo storage;
  const PdfCacheIo io = storage.io();
  PdfFixedRecordSpool spool;
  ASSERT_TRUE(spool.configure(&io, sizeof(PdfXrefEntry), 4).ok());
  ASSERT_TRUE(spool.open("/failed-spool", PdfCacheOpenMode::WriteTruncate).ok());
  ASSERT_EQ(storage.openHandleCount(), 1U);
  storage.failNextWriteAfter(3);
  const PdfXrefEntry entry{1, 0, PdfXrefEntryType::Uncompressed, 0, 12, 0};

  EXPECT_EQ(pdfWriteRecord(spool.store(), 0, &entry).error, PdfError::IoFailure);
  EXPECT_FALSE(spool.isOpen());
  EXPECT_EQ(storage.openHandleCount(), 0U);
}

TEST(PdfFixedRecordSpoolContract, SuccessfulShortAppendIsRejectedAndAbortClosesHandle) {
  ContractCacheIo storage;
  const PdfCacheIo io = storage.io();
  PdfFixedRecordSpool spool;
  ASSERT_TRUE(spool.configure(&io, sizeof(PdfXrefEntry), 2).ok());
  ASSERT_TRUE(spool.open("/short-append", PdfCacheOpenMode::WriteTruncate).ok());
  storage.shortNextWriteAfter(3);
  const PdfXrefEntry entry{1, 0, PdfXrefEntryType::Uncompressed, 0, 12, 0};

  EXPECT_EQ(pdfWriteRecord(spool.store(), 0, &entry).error, PdfError::InsufficientStorage);
  EXPECT_FALSE(spool.isOpen());
  EXPECT_EQ(spool.recordCount(), 0U);
  EXPECT_EQ(storage.openHandleCount(), 0U);
}

TEST(PdfFixedRecordSpoolContract, SuccessfulShortBatchAppendIsRejectedAndAbortClosesHandle) {
  ContractCacheIo storage;
  const PdfCacheIo io = storage.io();
  PdfFixedRecordSpool spool;
  ASSERT_TRUE(spool.configure(&io, sizeof(PdfXrefEntry), 2).ok());
  ASSERT_TRUE(spool.open("/short-batch", PdfCacheOpenMode::WriteTruncate).ok());
  const std::array<PdfXrefEntry, 2> entries{{
      {1, 0, PdfXrefEntryType::Uncompressed, 0, 12, 0},
      {2, 0, PdfXrefEntryType::Uncompressed, 0, 24, 0},
  }};
  storage.shortNextWriteAfter(sizeof(PdfXrefEntry) + 1U);

  EXPECT_EQ(spool.appendRecords(entries.data(), static_cast<uint32_t>(entries.size())).error,
            PdfError::InsufficientStorage);
  EXPECT_FALSE(spool.isOpen());
  EXPECT_EQ(spool.recordCount(), 0U);
  EXPECT_EQ(storage.openHandleCount(), 0U);
}

TEST(PdfFixedRecordSpoolContract, SuccessfulShortRewriteIsRejectedAndAbortClosesHandle) {
  ContractCacheIo storage;
  const PdfCacheIo io = storage.io();
  PdfFixedRecordSpool spool;
  ASSERT_TRUE(spool.configure(&io, sizeof(PdfXrefEntry), 2).ok());
  ASSERT_TRUE(spool.open("/short-rewrite", PdfCacheOpenMode::WriteTruncate).ok());
  const PdfXrefEntry original{1, 0, PdfXrefEntryType::Uncompressed, 0, 12, 0};
  ASSERT_TRUE(pdfWriteRecord(spool.store(), 0, &original).ok());
  ASSERT_TRUE(spool.close().ok());
  ASSERT_TRUE(spool.openForUpdates("/short-rewrite", 1).ok());
  storage.shortNextWriteAfter(3);
  const PdfXrefEntry replacement{2, 0, PdfXrefEntryType::Uncompressed, 0, 24, 0};

  EXPECT_EQ(spool.rewriteExisting(0, &replacement, sizeof(replacement)).error, PdfError::InsufficientStorage);
  EXPECT_FALSE(spool.isOpen());
  EXPECT_EQ(storage.openHandleCount(), 0U);
}

TEST(PdfFixedRecordSpoolContract, SuccessfulShortReadIsRejectedAsUnexpectedEof) {
  ContractCacheIo storage;
  const PdfCacheIo io = storage.io();
  PdfFixedRecordSpool spool;
  ASSERT_TRUE(spool.configure(&io, sizeof(PdfXrefEntry), 2).ok());
  ASSERT_TRUE(spool.open("/short-read", PdfCacheOpenMode::WriteTruncate).ok());
  const PdfXrefEntry expected{1, 0, PdfXrefEntryType::Uncompressed, 0, 12, 0};
  ASSERT_TRUE(pdfWriteRecord(spool.store(), 0, &expected).ok());
  ASSERT_TRUE(spool.close().ok());
  ASSERT_TRUE(spool.open("/short-read", PdfCacheOpenMode::Read, 1).ok());
  storage.shortNextReadAfter(3);
  PdfXrefEntry observed{};

  EXPECT_EQ(pdfReadRecord(spool.store(), 0, &observed).error, PdfError::UnexpectedEof);
}

TEST(PdfFixedRecordSpoolContract, WriteOnlyResumeAppendPreservesExistingPrefix) {
  ContractCacheIo storage;
  const PdfCacheIo io = storage.io();
  PdfFixedRecordSpool spool;
  ASSERT_TRUE(spool.configure(&io, sizeof(PdfXrefEntry), 2).ok());
  ASSERT_TRUE(spool.open("/resume-append", PdfCacheOpenMode::WriteTruncate).ok());
  const PdfXrefEntry first{1, 0, PdfXrefEntryType::Uncompressed, 0, 12, 0};
  const PdfXrefEntry second{2, 0, PdfXrefEntryType::Uncompressed, 0, 24, 0};
  ASSERT_TRUE(pdfWriteRecord(spool.store(), 0, &first).ok());
  ASSERT_TRUE(spool.close().ok());
  ASSERT_TRUE(spool.openForAppend("/resume-append", 1).ok());
  ASSERT_TRUE(pdfWriteRecord(spool.store(), 1, &second).ok());
  ASSERT_TRUE(spool.close().ok());
  ASSERT_TRUE(spool.open("/resume-append", PdfCacheOpenMode::Read, 2).ok());
  std::array<PdfXrefEntry, 2> observed{};
  ASSERT_TRUE(pdfReadRecord(spool.store(), 0, &observed[0]).ok());
  ASSERT_TRUE(pdfReadRecord(spool.store(), 1, &observed[1]).ok());
  EXPECT_EQ(observed[0].objectNumber, 1U);
  EXPECT_EQ(observed[1].objectNumber, 2U);
}

TEST(PdfMutableRecordSpoolContract, BatchAppendUsesOneHalWriteAndLeavesDurabilityControlToCaller) {
  static_assert(!std::is_copy_constructible_v<PdfMutableRecordSpool>);
  ContractCacheIo storage;
  const PdfCacheIo io = storage.io();
  PdfMutableRecordSpool spool;
  ASSERT_TRUE(spool.configure(&io, sizeof(PdfXrefEntry), 4).ok());
  ASSERT_TRUE(spool.create("/mutable-batch").ok());
  ASSERT_TRUE(spool.openSession("/mutable-batch").ok());
  storage.resetMetrics();
  const std::array<PdfXrefEntry, 3> expected{{
      {1, 0, PdfXrefEntryType::Uncompressed, 0, 12, 0},
      {2, 0, PdfXrefEntryType::Compressed, 0, 7, 3},
      {3, 2, PdfXrefEntryType::Free, 0, 0, 0},
  }};

  ASSERT_TRUE(spool.appendRecords(expected.data(), static_cast<uint32_t>(expected.size())).ok());
  EXPECT_EQ(storage.metrics().writes, 1U);
  EXPECT_EQ(storage.metrics().flushes, 0U);
  EXPECT_EQ(storage.metrics().syncs, 0U);
  EXPECT_EQ(storage.metrics().closes, 0U);
  EXPECT_TRUE(spool.isOpen());
  EXPECT_EQ(spool.recordCount(), expected.size());

  ASSERT_TRUE(spool.closeSession().ok());
  ASSERT_TRUE(spool.openSession("/mutable-batch").ok());
  for (uint32_t index = 0; index < expected.size(); ++index) {
    PdfXrefEntry observed{};
    ASSERT_TRUE(pdfReadRecord(spool.store(), index, &observed).ok());
    EXPECT_EQ(observed.objectNumber, expected[index].objectNumber);
    EXPECT_EQ(observed.generation, expected[index].generation);
    EXPECT_EQ(observed.type, expected[index].type);
    EXPECT_EQ(observed.offset, expected[index].offset);
    EXPECT_EQ(observed.objectStreamIndex, expected[index].objectStreamIndex);
  }
}

TEST(PdfMutableRecordSpoolContract, BatchAppendSeeksPastRecordsAfterInPlacePatch) {
  ContractCacheIo storage;
  const PdfCacheIo io = storage.io();
  PdfMutableRecordSpool spool;
  ASSERT_TRUE(spool.configure(&io, sizeof(PdfXrefEntry), 3).ok());
  ASSERT_TRUE(spool.create("/mutable-seek").ok());
  ASSERT_TRUE(spool.openSession("/mutable-seek").ok());
  const std::array<PdfXrefEntry, 2> prefix{{
      {1, 0, PdfXrefEntryType::Uncompressed, 0, 12, 0},
      {2, 0, PdfXrefEntryType::Uncompressed, 0, 24, 0},
  }};
  ASSERT_TRUE(spool.appendRecords(prefix.data(), static_cast<uint32_t>(prefix.size())).ok());
  const PdfXrefEntry replacement{10, 0, PdfXrefEntryType::Uncompressed, 0, 120, 0};
  ASSERT_TRUE(pdfWriteRecord(spool.store(), 0, &replacement).ok());
  const PdfXrefEntry appended{3, 0, PdfXrefEntryType::Uncompressed, 0, 36, 0};

  ASSERT_TRUE(spool.appendRecords(&appended, 1).ok());
  ASSERT_TRUE(spool.closeSession().ok());
  ASSERT_TRUE(spool.openSession("/mutable-seek").ok());
  std::array<PdfXrefEntry, 3> observed{};
  for (uint32_t index = 0; index < observed.size(); ++index) {
    ASSERT_TRUE(pdfReadRecord(spool.store(), index, &observed[index]).ok());
  }
  EXPECT_EQ(observed[0].objectNumber, replacement.objectNumber);
  EXPECT_EQ(observed[1].objectNumber, prefix[1].objectNumber);
  EXPECT_EQ(observed[2].objectNumber, appended.objectNumber);
}

TEST(PdfMutableRecordSpoolContract, BatchAppendRejectsCapacityAndInvalidInputWithoutMutation) {
  ContractCacheIo storage;
  const PdfCacheIo io = storage.io();
  PdfMutableRecordSpool spool;
  ASSERT_TRUE(spool.configure(&io, sizeof(PdfXrefEntry), 2).ok());
  ASSERT_TRUE(spool.create("/mutable-bounded").ok());
  ASSERT_TRUE(spool.openSession("/mutable-bounded").ok());
  const std::array<PdfXrefEntry, 2> records{{
      {1, 0, PdfXrefEntryType::Uncompressed, 0, 12, 0},
      {2, 0, PdfXrefEntryType::Uncompressed, 0, 24, 0},
  }};
  ASSERT_TRUE(spool.appendRecords(records.data(), static_cast<uint32_t>(records.size())).ok());
  const std::vector<uint8_t> before = storage.bytes("/mutable-bounded");
  const uint32_t writesBefore = storage.metrics().writes;

  EXPECT_EQ(spool.appendRecords(records.data(), 1).error, PdfError::InvalidOffset);
  EXPECT_EQ(spool.appendRecords(nullptr, 1).error, PdfError::InvalidOffset);
  EXPECT_EQ(spool.appendRecords(records.data(), 0).error, PdfError::InvalidOffset);
  EXPECT_EQ(storage.metrics().writes, writesBefore);
  EXPECT_EQ(storage.bytes("/mutable-bounded"), before);
  EXPECT_EQ(spool.recordCount(), records.size());
  EXPECT_TRUE(spool.isOpen());
}

TEST(PdfMutableRecordSpoolContract, SuccessfulShortBatchAppendIsRejectedAndAbortClosesHandle) {
  ContractCacheIo storage;
  const PdfCacheIo io = storage.io();
  PdfMutableRecordSpool spool;
  ASSERT_TRUE(spool.configure(&io, sizeof(PdfXrefEntry), 2).ok());
  ASSERT_TRUE(spool.create("/mutable-short").ok());
  ASSERT_TRUE(spool.openSession("/mutable-short").ok());
  const std::array<PdfXrefEntry, 2> records{{
      {1, 0, PdfXrefEntryType::Uncompressed, 0, 12, 0},
      {2, 0, PdfXrefEntryType::Uncompressed, 0, 24, 0},
  }};
  storage.shortNextWriteAfter(sizeof(PdfXrefEntry) + 1U);

  const PdfStatus status = spool.appendRecords(records.data(), static_cast<uint32_t>(records.size()));
  EXPECT_EQ(status.error, PdfError::InsufficientStorage);
  EXPECT_EQ(status.offset, sizeof(PdfXrefEntry) + 1U);
  EXPECT_FALSE(spool.isOpen());
  EXPECT_EQ(spool.recordCount(), 0U);
  EXPECT_EQ(storage.openHandleCount(), 0U);
}

TEST(PdfMutableRecordSpoolContract, PartialHalBatchWriteErrorAbortClosesHandle) {
  ContractCacheIo storage;
  const PdfCacheIo io = storage.io();
  PdfMutableRecordSpool spool;
  ASSERT_TRUE(spool.configure(&io, sizeof(PdfXrefEntry), 2).ok());
  ASSERT_TRUE(spool.create("/mutable-error").ok());
  ASSERT_TRUE(spool.openSession("/mutable-error").ok());
  const std::array<PdfXrefEntry, 2> records{{
      {1, 0, PdfXrefEntryType::Uncompressed, 0, 12, 0},
      {2, 0, PdfXrefEntryType::Uncompressed, 0, 24, 0},
  }};
  storage.failNextWriteAfter(3);

  EXPECT_EQ(spool.appendRecords(records.data(), static_cast<uint32_t>(records.size())).error,
            PdfError::IoFailure);
  EXPECT_FALSE(spool.isOpen());
  EXPECT_EQ(spool.recordCount(), 0U);
  EXPECT_EQ(storage.openHandleCount(), 0U);
}

TEST(PdfResolverSourceAccessContract, UncompressedLookupClosesXrefReaderThenReopensSource) {
  const std::string object = "1 0 obj\n42\nendobj\n";
  PdfTestByteSource bytes({object.begin(), object.end()});
  ResolverHarness harness;
  const PdfXrefEntry entry{1, 0, PdfXrefEntryType::Uncompressed, 0, 0, 0};
  ASSERT_TRUE(pdfWriteRecord(harness.records.store(), 0, &entry).ok());
  ASSERT_TRUE(harness.xref.adoptSortedRecords(1).ok());
  PdfObjectResolver resolver(bytes.source(), harness.xref, harness.sourceBuffer.data(), harness.sourceBuffer.size(),
                             harness.arena, harness.workspace());

  ASSERT_TRUE(resolver.begin({1, 0}).ok());
  EXPECT_TRUE(harness.transitions.empty());
  EXPECT_EQ(harness.records.readCount(), 0U);
  ASSERT_TRUE(runResolver(resolver).complete());
  EXPECT_EQ(harness.transitions, (std::vector<bool>{false, true}));
  EXPECT_TRUE(harness.sourceOpen);
}

TEST(PdfResolverSourceAccessContract, CompressedUncachedKeepsSourceClosedAcrossBothXrefLookups) {
  const std::string objectStream = "10 0 obj\n<<>>\nendobj\n";
  PdfTestByteSource bytes({objectStream.begin(), objectStream.end()});
  ResolverHarness harness;
  const std::array<PdfXrefEntry, 2> entries{{
      {2, 0, PdfXrefEntryType::Compressed, 0, 10, 0},
      {10, 0, PdfXrefEntryType::Uncompressed, 0, 0, 0},
  }};
  for (uint32_t index = 0; index < entries.size(); ++index) {
    ASSERT_TRUE(pdfWriteRecord(harness.records.store(), index, &entries[index]).ok());
  }
  ASSERT_TRUE(harness.xref.adoptSortedRecords(static_cast<uint32_t>(entries.size())).ok());
  PdfObjectResolver resolver(bytes.source(), harness.xref, harness.sourceBuffer.data(), harness.sourceBuffer.size(),
                             harness.arena, harness.workspace());

  ASSERT_TRUE(resolver.begin({2, 0}).ok());
  EXPECT_TRUE(runResolver(resolver).failed());
  EXPECT_EQ(harness.transitions, (std::vector<bool>{false, true}));
  EXPECT_TRUE(harness.sourceOpen);
}

TEST(PdfResolverSourceAccessContract, CachedCompressedLookupNeverReopensTheSource) {
  const std::string objectStream =
      "10 0 obj\n<< /Type /ObjStm /N 1 /First 4 /Length 6 >>\nstream\n2 0 42\nendstream\nendobj\n";
  PdfTestByteSource bytes({objectStream.begin(), objectStream.end()});
  ResolverHarness harness;
  const std::array<PdfXrefEntry, 2> entries{{
      {2, 0, PdfXrefEntryType::Compressed, 0, 10, 0},
      {10, 0, PdfXrefEntryType::Uncompressed, 0, 0, 0},
  }};
  for (uint32_t index = 0; index < entries.size(); ++index) {
    ASSERT_TRUE(pdfWriteRecord(harness.records.store(), index, &entries[index]).ok());
  }
  ASSERT_TRUE(harness.xref.adoptSortedRecords(static_cast<uint32_t>(entries.size())).ok());
  PdfObjectResolver resolver(bytes.source(), harness.xref, harness.sourceBuffer.data(), harness.sourceBuffer.size(),
                             harness.arena, harness.workspace(true));

  ASSERT_TRUE(resolver.begin({2, 0}).ok());
  ASSERT_TRUE(runResolver(resolver).complete());
  ASSERT_FALSE(harness.sourceOpen);
  harness.transitions.clear();

  ASSERT_TRUE(resolver.begin({2, 0}).ok());
  EXPECT_TRUE(harness.transitions.empty());
  EXPECT_FALSE(harness.sourceOpen);
  EXPECT_TRUE(runResolver(resolver).complete());
}

TEST(PdfResolverSourceAccessContract, PageTreeReassertsXrefAfterTraversalEvictsCachedCompressedReader) {
  const std::string pages = "<< /Type /Pages /Count 1 /Kids [3 0 R] >>";
  const std::string page = "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] >>";
  const std::string index = "2 0 3 " + std::to_string(pages.size() + 1U) + " ";
  const std::string decoded = index + pages + " " + page;
  const std::string objectStream =
      "10 0 obj\n<< /Type /ObjStm /N 2 /First " + std::to_string(index.size()) + " /Length " +
      std::to_string(decoded.size()) + " >>\nstream\n" + decoded + "\nendstream\nendobj\n";
  PdfTestByteSource bytes({objectStream.begin(), objectStream.end()});
  ResolverHarness harness;
  const std::array<PdfXrefEntry, 3> entries{{
      {2, 0, PdfXrefEntryType::Compressed, 0, 10, 0},
      {3, 0, PdfXrefEntryType::Compressed, 0, 10, 1},
      {10, 0, PdfXrefEntryType::Uncompressed, 0, 0, 0},
  }};
  for (uint32_t indexOrdinal = 0; indexOrdinal < entries.size(); ++indexOrdinal) {
    ASSERT_TRUE(pdfWriteRecord(harness.records.store(), indexOrdinal, &entries[indexOrdinal]).ok());
  }
  ASSERT_TRUE(harness.xref.adoptSortedRecords(static_cast<uint32_t>(entries.size())).ok());
  PdfObjectResolver resolver(bytes.source(), harness.xref, harness.sourceBuffer.data(), harness.sourceBuffer.size(),
                             harness.arena, harness.workspace(true));
  PdfTestRecordStore traversal(sizeof(PdfPageTreeRecord), 8);
  PdfPageInfo pageWorkspace{};
  PdfPageTreeWalker walker(resolver, harness.arena, traversal.store(), ResolverHarness::capturePage, &harness,
                           ResolverHarness::setTraversalAccess, &harness, &pageWorkspace, 1);

  ASSERT_TRUE(walker.begin({2, 0}).ok());
  PdfStepResult result = PdfStepResult::paused();
  for (uint16_t step = 0; step < 256U && result.yielded(); ++step) {
    PdfWorkBudget budget{32, 4096};
    result = walker.step(budget);
  }

  ASSERT_TRUE(result.complete()) << static_cast<unsigned>(result.status.error) << "@" << result.status.offset;
  EXPECT_EQ(harness.pageCount, 1U);
  EXPECT_GE(static_cast<uint32_t>(std::count(harness.transitions.begin(), harness.transitions.end(), false)), 3U);
}

TEST(PdfResolverSourceAccessContract, LookupFailureLeavesOnlyTheXrefReaderSelected) {
  const std::string object = "1 0 obj\n42\nendobj\n";
  PdfTestByteSource bytes({object.begin(), object.end()});
  ResolverHarness harness;
  const PdfXrefEntry entry{1, 0, PdfXrefEntryType::Uncompressed, 0, 0, 0};
  ASSERT_TRUE(pdfWriteRecord(harness.records.store(), 0, &entry).ok());
  ASSERT_TRUE(harness.xref.adoptSortedRecords(1).ok());
  PdfObjectResolver resolver(bytes.source(), harness.xref, harness.sourceBuffer.data(), harness.sourceBuffer.size(),
                             harness.arena, harness.workspace());

  ASSERT_TRUE(resolver.begin({2, 0}).ok());
  const PdfStepResult result = runResolver(resolver);
  ASSERT_TRUE(result.failed());
  EXPECT_EQ(result.status.error, PdfError::InvalidOffset);
  EXPECT_EQ(harness.transitions, (std::vector<bool>{false}));
  EXPECT_FALSE(harness.sourceOpen);
}

TEST(PdfResolverSourceAccessContract, ResolverWithoutCallbackPreservesUncompressedBehavior) {
  const std::string object = "1 0 obj\n42\nendobj\n";
  PdfTestByteSource bytes({object.begin(), object.end()});
  ResolverHarness harness;
  const PdfXrefEntry entry{1, 0, PdfXrefEntryType::Uncompressed, 0, 0, 0};
  ASSERT_TRUE(pdfWriteRecord(harness.records.store(), 0, &entry).ok());
  ASSERT_TRUE(harness.xref.adoptSortedRecords(1).ok());
  harness.records.forbidReadsWhile(nullptr);
  PdfObjectResolver resolver(bytes.source(), harness.xref, harness.sourceBuffer.data(), harness.sourceBuffer.size(),
                             harness.arena);

  ASSERT_TRUE(resolver.begin({1, 0}).ok());
  EXPECT_TRUE(runResolver(resolver).complete());
}

}  // namespace
