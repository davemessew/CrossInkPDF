#include <gtest/gtest.h>

#include <array>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

#include "PdfPageTree.h"
#include "PdfTestIo.h"

namespace {

std::vector<uint8_t> loadPageTreeFixture(const char* name) {
  const std::filesystem::path path = std::filesystem::path(__FILE__).parent_path() / "fixtures" / name;
  std::ifstream input(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

struct PageTreeHarness {
  PageTreeHarness() { traversalStorage.forbidReadsWhile(&traversalReadForbidden); }

  std::array<uint8_t, 4096> sourceBuffer{};
  std::array<PdfValue, 128> values{};
  std::array<PdfDictionaryEntry, 128> dictionaries{};
  std::array<PdfArrayItem, 128> arrays{};
  std::array<uint8_t, 2048> text{};
  PdfObjectArena arena{
      values.data(),       static_cast<uint16_t>(values.size()),
      dictionaries.data(), static_cast<uint16_t>(dictionaries.size()),
      arrays.data(),       static_cast<uint16_t>(arrays.size()),
      text.data(),         static_cast<uint16_t>(text.size()),
  };
  PdfTestRecordStore xrefStorage{sizeof(PdfXrefEntry), 256};
  PdfXrefTable xref{xrefStorage.store()};
  PdfTestRecordStore traversalStorage{sizeof(PdfPageTreeRecord), 256};
  PdfTestRecordStore annotationOverflowStorage{sizeof(PdfObjectReference), 256};
  PdfTestRecordStore contentOverflowStorage{sizeof(PdfPageContentOverflowRecord), 256};
  PdfPageInfo pageScratch{};
  std::vector<PdfPageInfo> pages;
  bool sourceOpen = true;
  bool xrefBlocked = true;
  bool traversalOpen = false;
  bool traversalReadForbidden = true;
  uint32_t traversalOpenCount = 0;
  uint32_t traversalCloseCount = 0;
};

PdfStepResult runParser(PdfXrefParser& parser) {
  while (true) {
    PdfWorkBudget budget{32, 4096};
    const PdfStepResult result = parser.step(budget);
    if (!result.yielded()) {
      return result;
    }
  }
}

PdfStepResult runResolver(PdfObjectResolver& resolver) {
  while (true) {
    PdfWorkBudget budget{32, 4096};
    const PdfStepResult result = resolver.step(budget);
    if (!result.yielded()) {
      return result;
    }
  }
}

PdfStatus capturePage(void* context, const PdfPageInfo& page) {
  static_cast<PageTreeHarness*>(context)->pages.push_back(page);
  return PdfStatus::success();
}

PdfStatus setTraversalAccess(void* context, const bool required) {
  if (context == nullptr) {
    return PdfStatus::failure(PdfError::InvalidArgument);
  }
  auto& harness = *static_cast<PageTreeHarness*>(context);
  harness.traversalOpen = required;
  harness.traversalReadForbidden = !required;
  harness.sourceOpen = false;
  harness.xrefBlocked = true;
  required ? ++harness.traversalOpenCount : ++harness.traversalCloseCount;
  return PdfStatus::success();
}

PdfStepResult walkBytes(std::vector<uint8_t> bytes, PageTreeHarness& harness,
                        const uint32_t maxPages = PdfLimits::MaxPages) {
  PdfTestByteSource memory(std::move(bytes));
  const PdfByteSource source = memory.source();
  PdfXrefParser xrefParser(source, harness.sourceBuffer.data(), harness.sourceBuffer.size(), harness.arena,
                           harness.xref);
  xrefParser.begin();
  PdfStepResult result = runParser(xrefParser);
  if (!result.complete()) {
    return result;
  }
  PdfObjectReference catalogReference;
  if (!harness.xref.root(&catalogReference)) {
    return PdfStepResult::failure(PdfStatus::failure(PdfError::Malformed));
  }
  PdfObjectResolver resolver(source, harness.xref, harness.sourceBuffer.data(), harness.sourceBuffer.size(),
                             harness.arena);
  PdfStatus status = resolver.begin(catalogReference);
  if (!status.ok()) {
    return PdfStepResult::failure(status);
  }
  result = runResolver(resolver);
  if (!result.complete()) {
    return result;
  }
  uint16_t pagesIndex = PDF_INVALID_INDEX;
  if (!pdfDictionaryFind(harness.arena, resolver.result().rootIndex, "Pages", &pagesIndex)) {
    return PdfStepResult::failure(PdfStatus::failure(PdfError::Malformed));
  }
  const PdfValue pages = harness.arena.values[pagesIndex];
  if (pages.kind != PdfValueKind::Reference) {
    return PdfStepResult::failure(PdfStatus::failure(PdfError::Malformed));
  }

  PdfPageTreeWalker walker(resolver, harness.arena,
                           harness.traversalStorage.store(), capturePage,
                           &harness, setTraversalAccess, &harness,
                           &harness.pageScratch, harness.annotationOverflowStorage.store(), maxPages,
                           harness.contentOverflowStorage.store());
  status = walker.begin({pages.objectNumber, pages.generation});
  if (!status.ok()) {
    return PdfStepResult::failure(status);
  }
  for (uint16_t step = 0; step < 4096U; ++step) {
    PdfWorkBudget budget{32, 4096};
    result = walker.step(budget);
    if (!result.yielded()) {
      return result;
    }
  }
  return PdfStepResult::failure(PdfStatus::failure(PdfError::BudgetExhausted));
}

PdfStepResult walkFixture(const char* fixture, PageTreeHarness& harness,
                          const uint32_t maxPages = PdfLimits::MaxPages) {
  return walkBytes(loadPageTreeFixture(fixture), harness, maxPages);
}

std::vector<uint8_t> makeManyContentsFixture(const uint16_t contentCount) {
  const uint32_t objectCount = static_cast<uint32_t>(contentCount) + 3U;
  std::vector<size_t> offsets(objectCount + 1U, 0);
  std::string pdf = "%PDF-1.7\n";
  const auto appendObject = [&pdf, &offsets](const uint32_t number, const std::string& body) {
    offsets[number] = pdf.size();
    pdf += std::to_string(number) + " 0 obj\n" + body + "\nendobj\n";
  };
  appendObject(1, "<< /Type /Catalog /Pages 2 0 R >>");
  appendObject(2, "<< /Type /Pages /Kids [3 0 R] /Count 1 /MediaBox [0 0 600 800] >>");
  std::string contents = "<< /Type /Page /Parent 2 0 R /Contents [";
  for (uint16_t ordinal = 0; ordinal < contentCount; ++ordinal) {
    contents += std::to_string(static_cast<uint32_t>(ordinal) + 4U) + " 0 R ";
  }
  contents += "] >>";
  appendObject(3, contents);
  for (uint32_t number = 4; number <= objectCount; ++number) {
    appendObject(number, "<< /Length 0 >>\nstream\n\nendstream");
  }
  const size_t xrefOffset = pdf.size();
  pdf += "xref\n0 " + std::to_string(objectCount + 1U) + "\n0000000000 65535 f \n";
  char entry[32]{};
  for (uint32_t number = 1; number <= objectCount; ++number) {
    const int written = std::snprintf(entry, sizeof(entry), "%010llu 00000 n \n",
                                      static_cast<unsigned long long>(offsets[number]));
    EXPECT_GT(written, 0);
    pdf.append(entry, static_cast<size_t>(written));
  }
  pdf += "trailer\n<< /Size " + std::to_string(objectCount + 1U) + " /Root 1 0 R >>\nstartxref\n" +
         std::to_string(xrefOffset) + "\n%%EOF\n";
  return {pdf.begin(), pdf.end()};
}

}  // namespace

TEST(PdfPageTreeTest, PreservesOrderInheritedResourcesAndSingleOrArrayContents) {
  PageTreeHarness harness;

  const PdfStepResult result = walkFixture("page_tree_inherited.pdf", harness);

  ASSERT_TRUE(result.complete());
  ASSERT_EQ(harness.pages.size(), 2u);
  EXPECT_EQ(harness.traversalOpenCount, 4U);
  EXPECT_EQ(harness.traversalOpenCount, harness.traversalCloseCount);
  EXPECT_FALSE(harness.traversalOpen);
  EXPECT_TRUE(harness.traversalReadForbidden);
  EXPECT_FALSE(harness.sourceOpen);
  EXPECT_TRUE(harness.xrefBlocked);
  EXPECT_EQ(harness.pages[0].pageReference, (PdfObjectReference{3, 0}));
  EXPECT_EQ(harness.pages[0].contentCount, 2u);
  EXPECT_EQ(harness.pages[0].contents[0], (PdfObjectReference{4, 0}));
  EXPECT_EQ(harness.pages[0].contents[1], (PdfObjectReference{5, 0}));
  EXPECT_TRUE(harness.pages[0].hasResources);
  EXPECT_TRUE(harness.pages[0].resourcesIndirect);
  EXPECT_EQ(harness.pages[0].resourceOwner, (PdfObjectReference{2, 0}));
  EXPECT_EQ(harness.pages[0].resourceReference, (PdfObjectReference{10, 0}));
  EXPECT_EQ(harness.pages[0].pageWidth, 740U);
  EXPECT_EQ(harness.pages[0].pageHeight, 600U);
  EXPECT_EQ(harness.pages[0].rotation, 270U);
  EXPECT_EQ(harness.pages[0].viewXMin, 10 << 16);
  EXPECT_EQ(harness.pages[0].viewYMin, 40 << 16);
  EXPECT_EQ(harness.pages[1].pageReference, (PdfObjectReference{6, 0}));
  EXPECT_EQ(harness.pages[1].contentCount, 1u);
  EXPECT_EQ(harness.pages[1].contents[0], (PdfObjectReference{7, 0}));
  EXPECT_EQ(harness.pages[1].pageWidth, 400U);
  EXPECT_EQ(harness.pages[1].pageHeight, 250U);
  EXPECT_EQ(harness.pages[1].rotation, 90U);
  EXPECT_EQ(harness.pages[1].viewXMin, 50 << 16);
  EXPECT_EQ(harness.pages[1].viewYMin, 0);
  EXPECT_TRUE(harness.pages[1].hasResources);
  EXPECT_EQ(harness.pages[1].resourceReference, (PdfObjectReference{10, 0}));
}

TEST(PdfPageTreeTest, SpoolsAnnotationsBeyondInlinePageCapacity) {
  PageTreeHarness harness;

  const PdfStepResult result = walkFixture("page_tree_many_annotations.pdf", harness);

  ASSERT_TRUE(result.complete());
  ASSERT_EQ(harness.pages.size(), 1U);
  const PdfPageInfo& page = harness.pages[0];
  ASSERT_EQ(page.annotationCount, PdfLimits::MaxLinkAnnotationsPerPage);
  EXPECT_EQ(page.overflowAnnotationCount, 41U);
  for (uint8_t index = 0; index < page.annotationCount; ++index) {
    EXPECT_EQ(page.annotations[index], (PdfObjectReference{static_cast<uint32_t>(10U + index), 0}));
  }
  for (uint32_t index = 0; index < page.overflowAnnotationCount; ++index) {
    PdfObjectReference reference{};
    ASSERT_TRUE(pdfReadRecord(harness.annotationOverflowStorage.store(), index, &reference));
    EXPECT_EQ(reference, (PdfObjectReference{static_cast<uint32_t>(26U + index), 0}));
  }
}

TEST(PdfPageTreeTest, SpoolsContentStreamsBeyondInlineCapacityInSourceOrder) {
  constexpr uint16_t contentCount = PdfLimits::MaxContentStreamsPerPage + 4U;
  PageTreeHarness harness;

  const PdfStepResult result = walkBytes(makeManyContentsFixture(contentCount), harness);

  ASSERT_TRUE(result.complete()) << static_cast<int>(result.status.error) << '@' << result.status.offset;
  ASSERT_EQ(harness.pages.size(), 1U);
  const PdfPageInfo& page = harness.pages[0];
  ASSERT_EQ(page.contentCount, PdfLimits::MaxContentStreamsPerPage);
  ASSERT_EQ(page.contentOverflowStart, 0U);
  ASSERT_EQ(page.overflowContentCount, 4U);
  for (uint16_t ordinal = 0; ordinal < page.contentCount; ++ordinal) {
    EXPECT_EQ(page.contents[ordinal], (PdfObjectReference{static_cast<uint32_t>(ordinal) + 4U, 0}));
  }
  for (uint16_t overflow = 0; overflow < page.overflowContentCount; ++overflow) {
    PdfPageContentOverflowRecord record{};
    ASSERT_TRUE(pdfReadRecord(harness.contentOverflowStorage.store(), page.contentOverflowStart + overflow,
                              &record));
    const uint16_t ordinal = static_cast<uint16_t>(PdfLimits::MaxContentStreamsPerPage + overflow);
    EXPECT_EQ(record.pageIndex, page.pageIndex);
    EXPECT_EQ(record.ordinal, ordinal);
    EXPECT_EQ(record.reserved, 0U);
    EXPECT_EQ(record.reference, (PdfObjectReference{static_cast<uint32_t>(ordinal) + 4U, 0}));
  }
}

TEST(PdfPageTreeTest,
     NormalizesAllQuarterTurnsAndIntersectsOrFallsBackFromCropBox) {
  PageTreeHarness harness;

  const PdfStepResult result = walkFixture("page_tree_geometry.pdf", harness);

  ASSERT_TRUE(result.complete());
  ASSERT_EQ(harness.pages.size(), 4U);
  EXPECT_EQ(harness.pages[0].pageWidth, 200U);
  EXPECT_EQ(harness.pages[0].pageHeight, 100U);
  EXPECT_EQ(harness.pages[0].rotation, 0U);
  EXPECT_EQ(harness.pages[1].pageWidth, 100U);
  EXPECT_EQ(harness.pages[1].pageHeight, 200U);
  EXPECT_EQ(harness.pages[1].rotation, 90U);
  EXPECT_EQ(harness.pages[2].pageWidth, 150U);
  EXPECT_EQ(harness.pages[2].pageHeight, 80U);
  EXPECT_EQ(harness.pages[2].rotation, 180U);
  EXPECT_EQ(harness.pages[3].pageWidth, 100U);
  EXPECT_EQ(harness.pages[3].pageHeight, 200U);
  EXPECT_EQ(harness.pages[3].rotation, 270U);
}

TEST(PdfPageTreeTest, RejectsMalformedInheritedPageGeometry) {
  for (const char* fixture :
       {"page_tree_bad_media_box.pdf", "page_tree_bad_crop_box.pdf",
        "page_tree_bad_rotate.pdf"}) {
    PageTreeHarness harness;
    const PdfStepResult result = walkFixture(fixture, harness);
    EXPECT_TRUE(result.failed()) << fixture;
    EXPECT_EQ(result.status.error, PdfError::Malformed) << fixture;
  }
}

TEST(PdfPageTreeTest, RejectsCycleAndDepth) {
  for (const auto& fixture :
       std::array<std::pair<const char*, PdfError>, 2>{{{"page_tree_cycle.pdf", PdfError::Malformed},
                                                        {"page_tree_deep.pdf", PdfError::LimitExceeded}}}) {
    PageTreeHarness harness;
    const PdfStepResult result = walkFixture(fixture.first, harness);
    EXPECT_TRUE(result.failed()) << fixture.first;
    EXPECT_EQ(result.status.error, fixture.second) << fixture.first;
  }
}

TEST(PdfPageTreeTest, EnforcesConfiguredCapAtNMinusOne) {
  PageTreeHarness passing;
  EXPECT_TRUE(walkFixture("page_tree_inherited.pdf", passing, 2).complete());
  PageTreeHarness failing;
  const PdfStepResult result = walkFixture("page_tree_inherited.pdf", failing, 1);
  EXPECT_TRUE(result.failed());
  EXPECT_EQ(result.status.error, PdfError::LimitExceeded);
  EXPECT_EQ(PdfLimits::MaxPages, static_cast<uint32_t>(UINT16_MAX));
}
