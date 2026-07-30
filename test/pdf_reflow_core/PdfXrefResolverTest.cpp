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
  std::array<uint8_t, 4096> sourceBuffer{};
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
};

PdfStepResult parseXref(const PdfByteSource& source, XrefHarness& harness, const bool budgetOne = false) {
  PdfXrefParser parser(source, harness.sourceBuffer.data(), harness.sourceBuffer.size(), harness.arena, harness.table);
  parser.begin();
  while (true) {
    PdfWorkBudget budget{budgetOne ? 1u : 32u, budgetOne ? 1u : 4096u};
    const PdfStepResult result = parser.step(budget);
    if (!result.yielded()) {
      return result;
    }
  }
}

PdfStepResult resolveObject(const PdfByteSource& source, XrefHarness& harness, const PdfObjectReference reference,
                            PdfResolvedObject* resolved) {
  PdfObjectResolver resolver(source, harness.table, harness.sourceBuffer.data(), harness.sourceBuffer.size(),
                             harness.arena);
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
