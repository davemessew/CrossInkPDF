#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>

#include "PdfResourceTracker.h"

namespace {

struct ResourceHarness {
  PdfResourceSnapshot snapshot{96U * 1024U, 64U * 1024U, 4U * 1024U};
  std::array<PdfResourceEvent, 16> events{};
  size_t eventCount = 0;

  static PdfResourceSnapshot measure(void* context) { return static_cast<ResourceHarness*>(context)->snapshot; }

  static void record(void* context, const PdfResourceEvent& event) {
    auto& self = *static_cast<ResourceHarness*>(context);
    ASSERT_LT(self.eventCount, self.events.size());
    self.events[self.eventCount++] = event;
  }
};

TEST(PdfResourceTracker, GatesFreeHeapAndLargestBlockBeforeAllocation) {
  ResourceHarness harness;
  PdfResourceTracker tracker({&harness, ResourceHarness::measure, ResourceHarness::record});

  EXPECT_TRUE(tracker.canStart());

  harness.snapshot.freeHeap = PDF_MIN_FREE_HEAP_BYTES - 1;
  EXPECT_FALSE(tracker.canStart());

  harness.snapshot.freeHeap = PDF_MIN_FREE_HEAP_BYTES;
  harness.snapshot.largestBlock = PDF_MIN_LARGEST_BLOCK_BYTES - 1;
  EXPECT_FALSE(tracker.canStart());
}

TEST(PdfResourceTracker, AccountsSixFixedWorkspacesAndReleasesInReverse) {
  ResourceHarness harness;
  PdfResourceTracker tracker({&harness, ResourceHarness::measure, ResourceHarness::record});

  ASSERT_TRUE(tracker.canStart());
  ASSERT_TRUE(tracker.acquire(PdfResourceKind::InflateDictionary, 32768));
  ASSERT_TRUE(tracker.acquire(PdfResourceKind::SourceWindow, 4096));
  ASSERT_TRUE(tracker.acquire(PdfResourceKind::DecoderOutput, 4096));
  ASSERT_TRUE(tracker.acquire(PdfResourceKind::PageText, 8192));
  ASSERT_TRUE(tracker.acquire(PdfResourceKind::RunRecords, 12288));
  ASSERT_TRUE(tracker.acquire(PdfResourceKind::OperandScratch, 2048));

  EXPECT_EQ(tracker.currentBytes(), 63488U);
  EXPECT_EQ(tracker.peakBytes(), 63488U);
  EXPECT_EQ(tracker.liveCount(), 6U);
  EXPECT_LE(tracker.peakBytes(), PDF_MAX_OWNED_HEAP_BYTES);

  EXPECT_FALSE(tracker.release(PdfResourceKind::SourceWindow));
  EXPECT_TRUE(tracker.release(PdfResourceKind::OperandScratch));
  EXPECT_TRUE(tracker.release(PdfResourceKind::RunRecords));
  EXPECT_TRUE(tracker.release(PdfResourceKind::PageText));
  EXPECT_TRUE(tracker.release(PdfResourceKind::DecoderOutput));
  EXPECT_TRUE(tracker.release(PdfResourceKind::SourceWindow));
  EXPECT_TRUE(tracker.release(PdfResourceKind::InflateDictionary));
  EXPECT_EQ(tracker.currentBytes(), 0U);
  EXPECT_EQ(tracker.liveCount(), 0U);
}

TEST(PdfResourceTracker, RejectsDuplicateOrOverBudgetAccountingAndLowStackMargin) {
  ResourceHarness harness;
  PdfResourceTracker tracker({&harness, ResourceHarness::measure, ResourceHarness::record});

  ASSERT_TRUE(tracker.acquire(PdfResourceKind::PageText, 8192));
  EXPECT_FALSE(tracker.acquire(PdfResourceKind::PageText, 8192));
  EXPECT_FALSE(tracker.acquire(PdfResourceKind::SourceWindow, PDF_MAX_OWNED_HEAP_BYTES));

  harness.snapshot.stackMargin = PDF_MIN_STACK_MARGIN_BYTES - 1;
  EXPECT_FALSE(tracker.runtimeWithinLimits());
  harness.snapshot.stackMargin = PDF_MIN_STACK_MARGIN_BYTES;
  EXPECT_TRUE(tracker.runtimeWithinLimits());
}

}  // namespace
