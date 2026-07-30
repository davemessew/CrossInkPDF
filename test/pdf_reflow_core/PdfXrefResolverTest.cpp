#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "PdfObjectResolver.h"
#include "PdfTestIo.h"
#include "PdfXref.h"

namespace {

std::vector<uint8_t> loadFixture(const char* name) {
  const std::filesystem::path path = std::filesystem::path(__FILE__).parent_path() / "fixtures" / name;
  std::ifstream input(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

struct XrefHarness {
  XrefHarness()
      : streamDecoder({decoderSourceBuffer.data(), decoderSourceBuffer.size(), decoderOutputBuffer.data(),
                       decoderOutputBuffer.size(), inflateDictionary.data(), inflateDictionary.size()}) {
    objectStreamStorage.forbidReadsWhile(&sourceActive);
  }

  static PdfStatus setSourceAccess(void* context, const bool required) {
    if (context == nullptr) {
      return PdfStatus::failure(PdfError::InvalidArgument);
    }
    auto& harness = *static_cast<XrefHarness*>(context);
    harness.sourceActive = required;
    ++harness.sourceTransitions;
    return PdfStatus::success();
  }

  std::array<uint8_t, 4096> sourceBuffer{};
  std::array<uint8_t, 4096> decoderSourceBuffer{};
  std::array<uint8_t, 4096> decoderOutputBuffer{};
  std::array<uint8_t, 32768> inflateDictionary{};
  PdfStreamDecoder streamDecoder;
  std::array<PdfValue, 64> values{};
  std::array<PdfDictionaryEntry, 64> dictionaries{};
  std::array<PdfArrayItem, 64> arrays{};
  std::array<uint8_t, 1024> text{};
  PdfObjectArena arena{
      values.data(),       static_cast<uint16_t>(values.size()),
      dictionaries.data(), static_cast<uint16_t>(dictionaries.size()),
      arrays.data(),       static_cast<uint16_t>(arrays.size()),
      text.data(),         static_cast<uint16_t>(text.size()),
  };
  PdfTestRecordStore recordStorage{sizeof(PdfXrefEntry), 256};
  PdfXrefTable table{recordStorage.store()};
  bool sourceActive = true;
  uint32_t sourceTransitions = 0;
  PdfTestByteStore objectStreamStorage{64 * 1024};

  PdfObjectResolverWorkspace resolverWorkspace() {
    return {
        &streamDecoder,
        objectStreamStorage.store(),
        this,
        setSourceAccess,
    };
  }
};

PdfStepResult parseXref(const PdfByteSource& source, XrefHarness& harness, const bool budgetOne = false) {
  PdfXrefParser parserWithStreams(source, harness.sourceBuffer.data(), harness.sourceBuffer.size(), harness.arena,
                                  harness.table, &harness.streamDecoder);
  parserWithStreams.begin();
  while (true) {
    PdfWorkBudget budget{budgetOne ? 1u : 32u, budgetOne ? 1u : 4096u};
    const PdfStepResult result = parserWithStreams.step(budget);
    if (!result.yielded()) {
      return result;
    }
  }
}

PdfStepResult resolveObject(const PdfByteSource& source, XrefHarness& harness, const PdfObjectReference reference,
                            PdfResolvedObject* resolved) {
  PdfObjectResolver resolver(source, harness.table, harness.sourceBuffer.data(), harness.sourceBuffer.size(),
                             harness.arena, harness.resolverWorkspace());
  const PdfStatus beginStatus = resolver.begin(reference);
  if (!beginStatus.ok()) {
    return PdfStepResult::failure(beginStatus);
  }
  while (true) {
    PdfWorkBudget budget{1, 1};
    const PdfStepResult result = resolver.step(budget);
    if (!result.yielded()) {
      if (result.complete() && resolved != nullptr) {
        *resolved = resolver.result();
      }
      return result;
    }
  }
}

PdfStepResult resolveObject(PdfObjectResolver& resolver, const PdfObjectReference reference,
                            PdfResolvedObject* resolved) {
  const PdfStatus beginStatus = resolver.begin(reference);
  if (!beginStatus.ok()) {
    return PdfStepResult::failure(beginStatus);
  }
  while (true) {
    PdfWorkBudget budget{32, 4096};
    const PdfStepResult result = resolver.step(budget);
    if (!result.yielded()) {
      if (result.complete() && resolved != nullptr) {
        *resolved = resolver.result();
      }
      return result;
    }
  }
}

}  // namespace

TEST(PdfXrefTest, ParsesClassicTableAndRoot) {
  PdfTestByteSource memory(loadFixture("classic_text.pdf"));
  const PdfByteSource source = memory.source();
  XrefHarness harness;

  const PdfStepResult result = parseXref(source, harness);

  ASSERT_TRUE(result.complete());
  PdfObjectReference root;
  ASSERT_TRUE(harness.table.root(&root));
  EXPECT_EQ(root, (PdfObjectReference{1, 0}));
  PdfXrefEntry content;
  ASSERT_TRUE(harness.table.find(4, &content).ok());
  EXPECT_EQ(content.type, PdfXrefEntryType::Uncompressed);
  EXPECT_LT(content.offset, source.size);
}

TEST(PdfXrefTest, NewestIncrementalRevisionWinsWithBudgetOne) {
  PdfTestByteSource memory(loadFixture("incremental_update.pdf"));
  const PdfByteSource source = memory.source();
  XrefHarness harness;

  const PdfStepResult result = parseXref(source, harness, true);

  ASSERT_TRUE(result.complete());
  PdfXrefEntry page;
  ASSERT_TRUE(harness.table.find(3, &page).ok());
  std::array<uint8_t, 7> header{};
  ASSERT_TRUE(pdfReadExact(source, page.offset, header.data(), header.size()).ok());
  EXPECT_EQ(std::string(header.begin(), header.end()), "3 0 obj");
  EXPECT_GT(page.offset, source.size / 2);
}

TEST(PdfXrefTest, ParsesSparseXrefStreamWidthsAndCompressedEntries) {
  PdfTestByteSource memory(loadFixture("xref_stream_objstm.pdf"));
  const PdfByteSource source = memory.source();
  XrefHarness harness;

  const PdfStepResult result = parseXref(source, harness, true);

  ASSERT_TRUE(result.complete());
  PdfObjectReference root;
  ASSERT_TRUE(harness.table.root(&root));
  EXPECT_EQ(root, (PdfObjectReference{1, 0}));
  PdfXrefEntry catalog;
  ASSERT_TRUE(harness.table.find(1, &catalog).ok());
  EXPECT_EQ(catalog.type, PdfXrefEntryType::Compressed);
  EXPECT_EQ(catalog.offset, 6u);
  EXPECT_EQ(catalog.objectStreamIndex, 0u);
  PdfXrefEntry content;
  ASSERT_TRUE(harness.table.find(5, &content).ok());
  EXPECT_EQ(content.type, PdfXrefEntryType::Uncompressed);
  EXPECT_LT(content.offset, source.size);
}

TEST(PdfXrefTest, NewestXrefStreamRevisionWinsOverClassicPrev) {
  PdfTestByteSource memory(loadFixture("incremental_xref_stream.pdf"));
  const PdfByteSource source = memory.source();
  XrefHarness harness;

  ASSERT_TRUE(parseXref(source, harness, true).complete());
  PdfXrefEntry page;
  ASSERT_TRUE(harness.table.find(3, &page).ok());
  std::array<uint8_t, 7> header{};
  ASSERT_TRUE(pdfReadExact(source, page.offset, header.data(), header.size()).ok());
  EXPECT_EQ(std::string(header.begin(), header.end()), "3 0 obj");
  EXPECT_GT(page.offset, source.size / 2);
}

TEST(PdfXrefTest, RejectsMalformedXrefStreamWidthsAndIndexes) {
  const std::vector<uint8_t> complete = loadFixture("xref_stream_objstm.pdf");
  for (const auto& replacement : std::array<std::pair<std::string, std::string>, 2>{
           {std::make_pair("/W [1 4 2]", "/W [9 4 2]"),
            std::make_pair("/Index [0 2 2 3 5 3]", "/Index [0 2 2 3 5 4]")}}) {
    std::string mutated(complete.begin(), complete.end());
    const size_t position = mutated.find(replacement.first);
    ASSERT_NE(position, std::string::npos);
    mutated.replace(position, replacement.first.size(), replacement.second);
    PdfTestByteSource memory(std::vector<uint8_t>(mutated.begin(), mutated.end()));
    XrefHarness harness;
    const PdfStepResult result = parseXref(memory.source(), harness);
    EXPECT_TRUE(result.failed()) << replacement.second;
    EXPECT_TRUE(result.status.error == PdfError::LimitExceeded || result.status.error == PdfError::UnexpectedEof)
        << replacement.second;
  }
}

TEST(PdfXrefTest, ExternalMergeUsesFixedSixtyFourEntryBufferAndNewestDuplicateWins) {
  bool sourceActive = true;
  PdfTestRecordStore primary(sizeof(PdfXrefEntry), 140);
  PdfTestRecordStore scratch(sizeof(PdfXrefEntry), 140);
  primary.forbidReadsWhile(&sourceActive);
  scratch.forbidReadsWhile(&sourceActive);
  PdfXrefTable table(primary.store());
  table.reset();
  ASSERT_TRUE(table.appendNewest({5, 0, PdfXrefEntryType::Uncompressed, 0, 9999, 0}).ok());
  for (uint32_t object = 130; object > 0; --object) {
    ASSERT_TRUE(table.appendNewest({object, 0, PdfXrefEntryType::Uncompressed, 0, object * 10ULL, 0}).ok());
  }
  std::array<PdfXrefEntry, PdfLimits::XrefMergeEntries> mergeBuffer{};

  EXPECT_EQ(table.finalize(scratch.store(), mergeBuffer.data(), PdfLimits::XrefMergeEntries - 1).error,
            PdfError::InvalidArgument);
  EXPECT_EQ(table.finalize(scratch.store(), mergeBuffer.data(), mergeBuffer.size()).error, PdfError::IoFailure);

  sourceActive = false;
  ASSERT_TRUE(table.finalize(scratch.store(), mergeBuffer.data(), mergeBuffer.size()).ok());
  EXPECT_TRUE(table.finalized());
  EXPECT_EQ(table.entryCount(), 130u);
  PdfXrefEntry newest;
  ASSERT_TRUE(table.find(5, &newest).ok());
  EXPECT_EQ(newest.offset, 9999u);
  PdfXrefEntry last;
  ASSERT_TRUE(table.find(130, &last).ok());
  EXPECT_EQ(last.offset, 1300u);
}

TEST(PdfXrefTest, RejectsEncryptionBadStartxrefAndPrevCycle) {
  for (const auto& fixture :
       std::array<std::pair<const char*, PdfError>, 3>{{{"encrypted.pdf", PdfError::Encrypted},
                                                        {"bad_startxref.pdf", PdfError::InvalidOffset},
                                                        {"xref_prev_cycle.pdf", PdfError::Malformed}}}) {
    PdfTestByteSource memory(loadFixture(fixture.first));
    const PdfByteSource source = memory.source();
    XrefHarness harness;
    const PdfStepResult result = parseXref(source, harness);
    EXPECT_TRUE(result.failed()) << fixture.first;
    EXPECT_EQ(result.status.error, fixture.second) << fixture.first;
  }
}

TEST(PdfXrefTest, EveryClassicTruncationFailsWithoutOutOfRangeRead) {
  const std::vector<uint8_t> complete = loadFixture("classic_text.pdf");
  for (size_t length = 0; length < complete.size(); ++length) {
    PdfTestByteSource memory(std::vector<uint8_t>(complete.begin(), complete.begin() + length));
    const PdfByteSource source = memory.source();
    XrefHarness harness;
    const PdfStepResult result = parseXref(source, harness);
    EXPECT_TRUE(result.failed()) << "length=" << length;
    EXPECT_TRUE(result.status.error == PdfError::UnexpectedEof || result.status.error == PdfError::InvalidOffset ||
                result.status.error == PdfError::Malformed)
        << "length=" << length;
  }
}

TEST(PdfObjectResolverTest, ResolvesCatalogPageAndContentStreamEndToEnd) {
  PdfTestByteSource memory(loadFixture("classic_text.pdf"));
  const PdfByteSource source = memory.source();
  XrefHarness harness;
  ASSERT_TRUE(parseXref(source, harness).complete());

  PdfObjectReference root;
  ASSERT_TRUE(harness.table.root(&root));
  PdfResolvedObject catalog;
  ASSERT_TRUE(resolveObject(source, harness, root, &catalog).complete());
  uint16_t pagesIndex = PDF_INVALID_INDEX;
  ASSERT_TRUE(pdfDictionaryFind(harness.arena, catalog.rootIndex, "Pages", &pagesIndex));
  const PdfValue pages = harness.arena.values[pagesIndex];
  ASSERT_EQ(pages.kind, PdfValueKind::Reference);

  PdfResolvedObject pagesObject;
  ASSERT_TRUE(resolveObject(source, harness, {pages.objectNumber, pages.generation}, &pagesObject).complete());
  uint16_t kidsIndex = PDF_INVALID_INDEX;
  ASSERT_TRUE(pdfDictionaryFind(harness.arena, pagesObject.rootIndex, "Kids", &kidsIndex));
  uint16_t pageIndex = PDF_INVALID_INDEX;
  ASSERT_TRUE(pdfArrayAt(harness.arena, kidsIndex, 0, &pageIndex));
  const PdfValue pageReference = harness.arena.values[pageIndex];

  PdfResolvedObject page;
  ASSERT_TRUE(resolveObject(source, harness, {pageReference.objectNumber, pageReference.generation}, &page).complete());
  uint16_t contentIndex = PDF_INVALID_INDEX;
  ASSERT_TRUE(pdfDictionaryFind(harness.arena, page.rootIndex, "Contents", &contentIndex));
  const PdfValue contentReference = harness.arena.values[contentIndex];

  PdfResolvedObject content;
  ASSERT_TRUE(resolveObject(source, harness, {contentReference.objectNumber, contentReference.generation}, &content)
                  .complete());
  ASSERT_TRUE(content.hasStream);
  std::vector<uint8_t> bytes(static_cast<size_t>(content.streamLength));
  ASSERT_TRUE(pdfReadExact(source, content.streamOffset, bytes.data(), bytes.size()).ok());
  EXPECT_NE(std::string(bytes.begin(), bytes.end()).find("(Hello PDF)"), std::string::npos);
}

TEST(PdfObjectResolverTest, ResolvesCompressedObjectsFromOneCachedFlateObjectStream) {
  PdfTestByteSource memory(loadFixture("xref_stream_objstm.pdf"));
  const PdfByteSource source = memory.source();
  XrefHarness harness;
  ASSERT_TRUE(parseXref(source, harness).complete());
  PdfObjectResolver resolver(source, harness.table, harness.sourceBuffer.data(), harness.sourceBuffer.size(),
                             harness.arena, harness.resolverWorkspace());

  PdfResolvedObject catalog;
  ASSERT_TRUE(resolveObject(resolver, {1, 0}, &catalog).complete());
  uint16_t pagesIndex = PDF_INVALID_INDEX;
  ASSERT_TRUE(pdfDictionaryFind(harness.arena, catalog.rootIndex, "Pages", &pagesIndex));
  const PdfObjectReference pagesRef{harness.arena.values[pagesIndex].objectNumber,
                                    harness.arena.values[pagesIndex].generation};
  EXPECT_EQ(harness.objectStreamStorage.resetCount(), 1u);

  PdfResolvedObject pages;
  ASSERT_TRUE(resolveObject(resolver, pagesRef, &pages).complete());
  uint16_t kidsIndex = PDF_INVALID_INDEX;
  ASSERT_TRUE(pdfDictionaryFind(harness.arena, pages.rootIndex, "Kids", &kidsIndex));
  uint16_t pageItem = PDF_INVALID_INDEX;
  ASSERT_TRUE(pdfArrayAt(harness.arena, kidsIndex, 0, &pageItem));
  const PdfObjectReference pageRef{harness.arena.values[pageItem].objectNumber,
                                   harness.arena.values[pageItem].generation};

  PdfResolvedObject page;
  ASSERT_TRUE(resolveObject(resolver, pageRef, &page).complete());
  EXPECT_EQ(harness.objectStreamStorage.resetCount(), 1u);
  EXPECT_FALSE(harness.sourceActive);
  EXPECT_GE(harness.sourceTransitions, 1u);
  uint16_t contentsIndex = PDF_INVALID_INDEX;
  ASSERT_TRUE(pdfDictionaryFind(harness.arena, page.rootIndex, "Contents", &contentsIndex));
  const PdfObjectReference contentsRef{harness.arena.values[contentsIndex].objectNumber,
                                       harness.arena.values[contentsIndex].generation};

  PdfResolvedObject content;
  ASSERT_TRUE(resolveObject(resolver, contentsRef, &content).complete());
  EXPECT_TRUE(harness.sourceActive);
  ASSERT_TRUE(content.hasStream);
  std::vector<uint8_t> contentBytes(static_cast<size_t>(content.streamLength));
  ASSERT_TRUE(pdfReadExact(source, content.streamOffset, contentBytes.data(), contentBytes.size()).ok());
  EXPECT_NE(std::string(contentBytes.begin(), contentBytes.end()).find("Compressed object stream text."),
            std::string::npos);
}

TEST(PdfObjectResolverTest, SingleReaderGuardRejectsStoreReadUntilSourceBoundaryCloses) {
  bool sourceActive = true;
  PdfTestByteStore storage(16);
  storage.forbidReadsWhile(&sourceActive);
  PdfByteStore store = storage.store();
  ASSERT_TRUE(store.reset(store.context).ok());
  const std::array<uint8_t, 3> expected{1, 2, 3};
  ASSERT_TRUE(pdfWriteExact(pdfByteStoreSink(store), expected.data(), expected.size()).ok());
  PdfByteSource source = pdfByteStoreSource(store);
  std::array<uint8_t, 3> output{};

  EXPECT_EQ(pdfReadExact(source, 0, output.data(), output.size()).error, PdfError::IoFailure);
  sourceActive = false;
  ASSERT_TRUE(pdfReadExact(source, 0, output.data(), output.size()).ok());
  EXPECT_EQ(output, expected);
}

TEST(PdfObjectResolverTest, RejectsCompressedObjectStreamRecursionAndCycle) {
  PdfTestByteSource memory({1, 2, 3, 4});
  XrefHarness harness;
  harness.table.reset();
  ASSERT_TRUE(harness.table.appendNewest({7, 0, PdfXrefEntryType::Compressed, 0, 7, 0}).ok());
  PdfObjectResolver cycle(memory.source(), harness.table, harness.sourceBuffer.data(), harness.sourceBuffer.size(),
                          harness.arena, harness.resolverWorkspace());
  EXPECT_EQ(cycle.begin({7, 0}).error, PdfError::Malformed);

  harness.table.reset();
  ASSERT_TRUE(harness.table.appendNewest({7, 0, PdfXrefEntryType::Compressed, 0, 8, 0}).ok());
  ASSERT_TRUE(harness.table.appendNewest({8, 0, PdfXrefEntryType::Compressed, 0, 9, 0}).ok());
  PdfObjectResolver recursive(memory.source(), harness.table, harness.sourceBuffer.data(), harness.sourceBuffer.size(),
                              harness.arena, harness.resolverWorkspace());
  EXPECT_EQ(recursive.begin({7, 0}).error, PdfError::Malformed);
}

TEST(PdfObjectResolverTest, RejectsBadOffsetBeforeReading) {
  PdfTestByteSource memory({1, 2, 3, 4});
  memory.setFailureOffset(0);
  const PdfByteSource source = memory.source();
  XrefHarness harness;
  harness.table.reset();
  ASSERT_TRUE(harness.table.appendNewest({7, 0, PdfXrefEntryType::Uncompressed, 0, source.size, 0}).ok());
  PdfObjectResolver resolver(source, harness.table, harness.sourceBuffer.data(), harness.sourceBuffer.size(),
                             harness.arena);

  const PdfStatus status = resolver.begin({7, 0});

  EXPECT_EQ(status.error, PdfError::InvalidOffset);
  EXPECT_EQ(status.offset, source.size);
}
