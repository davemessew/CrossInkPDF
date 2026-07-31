#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

#include "PdfCacheStore.h"
#include "PdfMetadataStore.h"
#include "PdfOutline.h"
#include "PdfPreparation.h"
#include "PdfTestCacheIo.h"
#include "PdfTestIo.h"

namespace {

std::vector<uint8_t> loadClassicFixture() {
  const std::filesystem::path path =
      std::filesystem::path(__FILE__).parent_path().parent_path() / "pdf_reflow_core" / "fixtures" / "classic_text.pdf";
  std::ifstream input(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

std::vector<uint8_t> loadNavigationFixture() {
  const std::filesystem::path path = std::filesystem::path(__FILE__).parent_path().parent_path() / "pdf_reflow_core" /
                                     "fixtures" / "navigation_outline.pdf";
  std::ifstream input(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

std::vector<uint8_t> loadFixture(const char* name) {
  const std::filesystem::path path =
      std::filesystem::path(__FILE__).parent_path().parent_path() / "pdf_reflow_core" / "fixtures" / name;
  std::ifstream input(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

struct PreparationHarness {
  PdfTestCacheIo storage;
  PdfResourceSnapshot resources{128U * 1024U, 96U * 1024U, 8U * 1024U};
  uint32_t nowMs = 0;
  size_t resourceEvents = 0;

  static uint32_t now(void* context) { return static_cast<PreparationHarness*>(context)->nowMs; }

  static PdfResourceSnapshot measure(void* context) { return static_cast<PreparationHarness*>(context)->resources; }

  static void resourceEvent(void* context, const PdfResourceEvent&) {
    ++static_cast<PreparationHarness*>(context)->resourceEvents;
  }

  PdfPreparationConfig config() {
    return {
        storage.io(), "/books/minimal.pdf", "/.crosspoint", this, now, {this, measure, resourceEvent},
        storage.renameCallback(),
        800,
        480,
    };
  }

  void addFixture() { storage.addFile("/books/minimal.pdf", loadClassicFixture(), 1234, true); }
};

PdfStepResult runToTerminal(PdfPreparation& preparation, PreparationHarness& harness, uint32_t maxSteps = 20000) {
  for (uint32_t step = 0; step < maxSteps; ++step) {
    const uint64_t readBefore = harness.storage.bytesReadTotal();
    const uint64_t writtenBefore = harness.storage.bytesWrittenTotal();
    const PdfStepResult result = preparation.step();
    EXPECT_LE(harness.storage.bytesReadTotal() - readBefore, 4096U);
    EXPECT_LE(harness.storage.bytesWrittenTotal() - writtenBefore, 4096U);
    ++harness.nowMs;
    if (!result.yielded()) {
      return result;
    }
  }
  return PdfStepResult::failure(PdfStatus::failure(PdfError::BudgetExhausted));
}

struct OneSectionCollector {
  PdfMetadataSection section{};
  uint16_t count = 0;

  static PdfStatus accept(void* context, const uint16_t index, const PdfMetadataSection& record) {
    if (context == nullptr || index != 0) {
      return PdfStatus::failure(PdfError::InvalidArgument);
    }
    auto& self = *static_cast<OneSectionCollector*>(context);
    self.section = record;
    ++self.count;
    return PdfStatus::success();
  }
};

struct OneOutlineCollector {
  PdfOutlineEntry entry{};
  uint16_t count = 0;

  static PdfStatus accept(void* context, const uint16_t index, const PdfOutlineEntry& record) {
    if (context == nullptr || index != 0) {
      return PdfStatus::failure(PdfError::InvalidArgument);
    }
    auto& self = *static_cast<OneOutlineCollector*>(context);
    self.entry = record;
    ++self.count;
    return PdfStatus::success();
  }
};

TEST(PdfPreparation, ConvertsMinimalFixtureIntoCommittedDeviceStyleXhtml) {
  PreparationHarness harness;
  harness.addFixture();

  PdfPreparation preparation;
  ASSERT_TRUE(preparation.begin(harness.config()).ok());
  const PdfStepResult result = runToTerminal(preparation, harness);

  ASSERT_TRUE(result.complete()) << static_cast<int>(result.status.error);
  EXPECT_EQ(preparation.progressPercent(), 100U);
  EXPECT_EQ(preparation.totalWords(), 2U);
  EXPECT_EQ(preparation.resourcePeakBytes(), 63488U);
  EXPECT_EQ(preparation.resourceCurrentBytes(), 0U);
  EXPECT_EQ(harness.storage.openHandleCount(), 0U);
  EXPECT_LE(harness.storage.maximumReadRequest(), 4096U);
  EXPECT_LE(harness.storage.maximumWriteRequest(), 4096U);
  EXPECT_TRUE(harness.storage.isDirectory("/.crosspoint"));

  const std::string sectionPath = std::string(preparation.cacheRoot()) + "/gen_" +
                                  std::to_string(preparation.generation()) + "/sections/000000.xhtml";
  ASSERT_TRUE(harness.storage.exists(sectionPath));
  const auto& sectionBytes = harness.storage.bytes(sectionPath);
  const std::string section(sectionBytes.begin(), sectionBytes.end());
  EXPECT_NE(section.find(">Hello PDF</p>"), std::string::npos);
  EXPECT_EQ(section.find("font-size"), std::string::npos);
  EXPECT_EQ(section.find("font-family"), std::string::npos);
  EXPECT_EQ(section.find("position:"), std::string::npos);

  const std::string generationRoot =
      std::string(preparation.cacheRoot()) + "/gen_" + std::to_string(preparation.generation());
  const std::string metadataPath = generationRoot + "/metadata.bin";
  const std::string outlinePath = generationRoot + "/outline.bin";
  ASSERT_TRUE(harness.storage.exists(metadataPath));
  ASSERT_TRUE(harness.storage.exists(outlinePath));

  PdfTestByteSource metadataSource(harness.storage.bytes(metadataPath));
  PdfMetadata metadata{};
  OneSectionCollector sectionCollector;
  ASSERT_TRUE(
      pdfDecodeMetadata(metadataSource.source(), &metadata, {&sectionCollector, OneSectionCollector::accept}).ok());
  EXPECT_STREQ(metadata.title, "minimal");
  EXPECT_EQ(metadata.totalWords, 2U);
  EXPECT_EQ(metadata.sectionCount, 1U);
  EXPECT_EQ(metadata.outlineCount, 1U);
  EXPECT_EQ(sectionCollector.count, 1U);
  EXPECT_EQ(sectionCollector.section.wordCount, 2U);

  PdfTestByteSource outlineSource(harness.storage.bytes(outlinePath));
  PdfOutlineHeader outlineHeader{};
  OneOutlineCollector outlineCollector;
  ASSERT_TRUE(
      pdfDecodeOutline(outlineSource.source(), &outlineHeader, {&outlineCollector, OneOutlineCollector::accept}).ok());
  EXPECT_EQ(outlineHeader.entryCount, 1U);
  EXPECT_STREQ(outlineCollector.entry.title, "minimal");
  EXPECT_STREQ(outlineCollector.entry.anchor, "b00000000");

  PdfCacheStore cache;
  ASSERT_TRUE(cache.initialize(harness.storage.io(), preparation.cacheRoot()).ok());
  PdfCacheManifestSelection selection;
  ASSERT_TRUE(cache.loadManifestSlots(preparation.sourceIdentity(), &selection).ok());
  ASSERT_TRUE(selection.selected);
  EXPECT_TRUE(selection.manifest.completed);
  EXPECT_EQ(selection.manifest.totalWords, 2U);
  EXPECT_EQ(selection.manifest.requiredFileCount, 3U);
}

TEST(PdfPreparation, PreservesGeneratedNavigationInCommittedReflowCache) {
  PreparationHarness harness;
  harness.storage.addFile("/books/minimal.pdf", loadNavigationFixture(), 1234, true);

  PdfPreparation preparation;
  ASSERT_TRUE(preparation.begin(harness.config()).ok());
  const PdfStepResult result = runToTerminal(preparation, harness);
  ASSERT_TRUE(result.complete()) << static_cast<int>(result.status.error);
  ASSERT_EQ(preparation.coverCandidateSourceCount(), 2U);
  PdfCoverCandidateSource coverSource{};
  ASSERT_TRUE(preparation.coverCandidateSource(0, &coverSource));
  EXPECT_EQ(coverSource.reference.objectNumber, 3U);
  EXPECT_EQ(coverSource.sourcePageIndex, 0U);
  EXPECT_FALSE(coverSource.referenceIsResourceDictionary);
  ASSERT_TRUE(preparation.coverCandidateSource(1, &coverSource));
  EXPECT_EQ(coverSource.reference.objectNumber, 6U);
  EXPECT_EQ(coverSource.sourcePageIndex, 1U);
  EXPECT_FALSE(coverSource.referenceIsResourceDictionary);
  EXPECT_FALSE(preparation.coverCandidateSource(2, &coverSource));

  const std::string generationRoot =
      std::string(preparation.cacheRoot()) + "/gen_" + std::to_string(preparation.generation());
  const std::string section0Path = generationRoot + "/sections/000000.xhtml";
  const std::string section1Path = generationRoot + "/sections/000001.xhtml";
  ASSERT_TRUE(harness.storage.exists(section0Path));
  ASSERT_TRUE(harness.storage.exists(section1Path));
  const std::string section0(harness.storage.bytes(section0Path).begin(), harness.storage.bytes(section0Path).end());
  const std::string section1(harness.storage.bytes(section1Path).begin(), harness.storage.bytes(section1Path).end());
  EXPECT_NE(section0.find("aria-label=\"i\""), std::string::npos);
  EXPECT_NE(section1.find("aria-label=\"A-1\""), std::string::npos);
  EXPECT_NE(section0.find(">Contents</h1>"), std::string::npos);
  EXPECT_NE(section0.find("<a href=\"sections/000001.xhtml#b00000003\">Chapter Two</a>"), std::string::npos);
  EXPECT_NE(section1.find(">Index</p>"), std::string::npos);

  PdfTestByteSource metadataSource(harness.storage.bytes(generationRoot + "/metadata.bin"));
  PdfMetadata metadata{};
  std::vector<PdfMetadataSection> sections;
  const PdfMetadataSectionVisitor sectionVisitor{
      &sections,
      [](void* context, uint16_t, const PdfMetadataSection& section) {
        static_cast<std::vector<PdfMetadataSection>*>(context)->push_back(section);
        return PdfStatus::success();
      },
  };
  ASSERT_TRUE(pdfDecodeMetadata(metadataSource.source(), &metadata, sectionVisitor).ok());
  EXPECT_STREQ(metadata.title, "XMP Navigation");
  EXPECT_STREQ(metadata.author, "XMP Author");
  EXPECT_STREQ(metadata.language, "de-CH");
  EXPECT_EQ(metadata.sectionCount, 2U);
  EXPECT_EQ(metadata.outlineCount, 3U);
  EXPECT_EQ(metadata.totalWords, 10U);
  ASSERT_EQ(sections.size(), 2U);
  EXPECT_EQ(sections[1].firstAnchorOrdinal, 3U);

  PdfTestByteSource outlineSource(harness.storage.bytes(generationRoot + "/outline.bin"));
  PdfOutlineHeader outlineHeader{};
  std::vector<PdfOutlineEntry> outline;
  const PdfOutlineEntryVisitor outlineVisitor{
      &outline,
      [](void* context, uint16_t, const PdfOutlineEntry& entry) {
        static_cast<std::vector<PdfOutlineEntry>*>(context)->push_back(entry);
        return PdfStatus::success();
      },
  };
  ASSERT_TRUE(pdfDecodeOutline(outlineSource.source(), &outlineHeader, outlineVisitor).ok());
  ASSERT_EQ(outline.size(), 3U);
  EXPECT_STREQ(outline[0].title, "Part One");
  EXPECT_EQ(outline[0].level, 1U);
  EXPECT_STREQ(outline[1].title, "Chapter One");
  EXPECT_EQ(outline[1].parentIndex, 0);
  EXPECT_STREQ(outline[2].title, "Chapter Two");
  EXPECT_EQ(outline[2].sectionIndex, 1U);
  EXPECT_STREQ(outline[2].anchor, "b00000003");

  PdfCacheStore cache;
  ASSERT_TRUE(cache.initialize(harness.storage.io(), preparation.cacheRoot()).ok());
  PdfCacheManifestSelection selection;
  ASSERT_TRUE(cache.loadManifestSlots(preparation.sourceIdentity(), &selection).ok());
  ASSERT_TRUE(selection.selected);
  EXPECT_EQ(selection.manifest.requiredFileCount, 4U);
}

TEST(PdfPreparation, DerivesSectionedOutlineFromHeadingsAndFallsBackToDocumentRoot) {
  for (const auto& fixture :
       {std::pair{"navigation_heading_fallback.pdf", true}, std::pair{"navigation_root_fallback.pdf", false}}) {
    PreparationHarness harness;
    harness.storage.addFile("/books/minimal.pdf", loadFixture(fixture.first), 1234, true);
    PdfPreparation preparation;
    ASSERT_TRUE(preparation.begin(harness.config()).ok());
    const PdfStepResult result = runToTerminal(preparation, harness);
    ASSERT_TRUE(result.complete()) << fixture.first << " " << static_cast<int>(result.status.error);

    const std::string generationRoot =
        std::string(preparation.cacheRoot()) + "/gen_" + std::to_string(preparation.generation());
    PdfTestByteSource outlineSource(harness.storage.bytes(generationRoot + "/outline.bin"));
    PdfOutlineHeader outlineHeader{};
    std::vector<PdfOutlineEntry> outline;
    const PdfOutlineEntryVisitor visitor{
        &outline,
        [](void* context, uint16_t, const PdfOutlineEntry& entry) {
          static_cast<std::vector<PdfOutlineEntry>*>(context)->push_back(entry);
          return PdfStatus::success();
        },
    };
    ASSERT_TRUE(pdfDecodeOutline(outlineSource.source(), &outlineHeader, visitor).ok());
    if (fixture.second) {
      ASSERT_EQ(outline.size(), 2U);
      EXPECT_STREQ(outline[0].title, "First Heading");
      EXPECT_EQ(outline[0].sectionIndex, 0U);
      EXPECT_STREQ(outline[0].anchor, "b00000000");
      EXPECT_STREQ(outline[1].title, "Second Heading");
      EXPECT_EQ(outline[1].sectionIndex, 1U);
      EXPECT_STREQ(outline[1].anchor, "b00000002");
      EXPECT_TRUE(harness.storage.exists(generationRoot + "/sections/000001.xhtml"));
    } else {
      ASSERT_EQ(outline.size(), 1U);
      EXPECT_STREQ(outline[0].title, "minimal");
      EXPECT_EQ(outline[0].sectionIndex, 0U);
    }
  }
}

TEST(PdfPreparation, RejectsCyclicOutlineWithoutCommittingPartialGeneration) {
  PreparationHarness harness;
  harness.storage.addFile("/books/minimal.pdf", loadFixture("navigation_outline_cycle.pdf"), 1234, true);
  PdfPreparation preparation;
  ASSERT_TRUE(preparation.begin(harness.config()).ok());
  const PdfStepResult result = runToTerminal(preparation, harness);
  ASSERT_TRUE(result.failed());
  EXPECT_EQ(result.status.error, PdfError::Malformed);

  PdfCacheStore cache;
  ASSERT_TRUE(cache.initialize(harness.storage.io(), preparation.cacheRoot()).ok());
  PdfCacheManifestSelection selection;
  ASSERT_TRUE(cache.loadManifestSlots(preparation.sourceIdentity(), &selection).ok());
  EXPECT_FALSE(selection.selected);
}

TEST(PdfPreparation, CancelsAtASliceBoundaryClosesEverythingAndResumesSafely) {
  PreparationHarness harness;
  harness.addFixture();

  {
    PdfPreparation preparation;
    ASSERT_TRUE(preparation.begin(harness.config()).ok());
    for (uint32_t step = 0; step < 10; ++step) {
      ASSERT_TRUE(preparation.step().yielded());
      ++harness.nowMs;
    }
    preparation.requestCancel();
    const PdfStepResult cancelled = preparation.step();
    ASSERT_TRUE(cancelled.failed());
    EXPECT_EQ(cancelled.status.error, PdfError::Cancelled);
    EXPECT_EQ(preparation.resourceCurrentBytes(), 0U);
    EXPECT_EQ(harness.storage.openHandleCount(), 0U);
  }

  PdfPreparation resumed;
  ASSERT_TRUE(resumed.begin(harness.config()).ok());
  const PdfStepResult result = runToTerminal(resumed, harness);
  ASSERT_TRUE(result.complete()) << static_cast<int>(result.status.error);
  EXPECT_TRUE(resumed.resumedFromCheckpoint());
  EXPECT_EQ(harness.storage.openHandleCount(), 0U);
}

TEST(PdfPreparation, RejectsLowHeapBeforeOpeningThePdfOrAllocatingWorkspaces) {
  PreparationHarness harness;
  harness.addFixture();
  harness.resources.freeHeap = PDF_MIN_FREE_HEAP_BYTES - 1;

  PdfPreparation preparation;
  ASSERT_TRUE(preparation.begin(harness.config()).ok());
  const PdfStepResult result = preparation.step();

  ASSERT_TRUE(result.failed());
  EXPECT_EQ(result.status.error, PdfError::InsufficientMemory);
  EXPECT_EQ(preparation.resourceCurrentBytes(), 0U);
  EXPECT_EQ(harness.storage.openCalls(), 0U);
}

TEST(PdfPreparationPaintGate, RequiresBothProgressAndElapsedTimeAndCapsIntermediatePaints) {
  PdfPreparationPaintGate gate;

  EXPECT_FALSE(gate.shouldPaint(10, 14999));
  EXPECT_TRUE(gate.shouldPaint(10, 15000));
  EXPECT_FALSE(gate.shouldPaint(20, 29999));
  EXPECT_TRUE(gate.shouldPaint(20, 30000));

  for (uint8_t paint = 3; paint <= 10; ++paint) {
    EXPECT_TRUE(gate.shouldPaint(static_cast<uint8_t>(paint * 10), static_cast<uint32_t>(paint) * 15000U));
  }
  EXPECT_EQ(gate.intermediatePaintCount(), 10U);
  EXPECT_FALSE(gate.shouldPaint(100, 165000));
}

}  // namespace
