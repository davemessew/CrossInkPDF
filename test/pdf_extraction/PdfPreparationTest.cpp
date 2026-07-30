#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "PdfCacheStore.h"
#include "PdfPreparation.h"
#include "PdfTestCacheIo.h"

namespace {

std::vector<uint8_t> loadClassicFixture() {
  const std::filesystem::path path =
      std::filesystem::path(__FILE__).parent_path().parent_path() / "pdf_reflow_core" / "fixtures" / "classic_text.pdf";
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

  const std::string sectionPath =
      std::string(preparation.cacheRoot()) + "/gen_" + std::to_string(preparation.generation()) + "/section.xhtml";
  ASSERT_TRUE(harness.storage.exists(sectionPath));
  const auto& sectionBytes = harness.storage.bytes(sectionPath);
  const std::string section(sectionBytes.begin(), sectionBytes.end());
  EXPECT_NE(section.find(">Hello PDF</p>"), std::string::npos);
  EXPECT_EQ(section.find("font-size"), std::string::npos);
  EXPECT_EQ(section.find("font-family"), std::string::npos);
  EXPECT_EQ(section.find("position:"), std::string::npos);

  PdfCacheStore cache;
  ASSERT_TRUE(cache.initialize(harness.storage.io(), preparation.cacheRoot()).ok());
  PdfCacheManifestSelection selection;
  ASSERT_TRUE(cache.loadManifestSlots(preparation.sourceIdentity(), &selection).ok());
  ASSERT_TRUE(selection.selected);
  EXPECT_TRUE(selection.manifest.completed);
  EXPECT_EQ(selection.manifest.totalWords, 2U);
  EXPECT_EQ(selection.manifest.requiredFileCount, 1U);
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
