#include <gtest/gtest.h>

#include <Memory.h>

#include "PdfReaderSessionAllocation.h"

namespace {
struct TestPdfReaderState {
  uint32_t marker = 0x504446U;
};

class PdfReaderSessionAllocationTest : public testing::Test {
 protected:
  void SetUp() override { PdfReaderAllocationProbe::reset(); }
};

TEST_F(PdfReaderSessionAllocationTest, EpubSkipsThePdfStateAllocator) {
  const auto state = allocatePdfReaderSessionState<TestPdfReaderState>(ReflowDocumentFormat::Epub);

  EXPECT_EQ(state, nullptr);
  EXPECT_EQ(PdfReaderAllocationProbe::calls, 0U);
}

TEST_F(PdfReaderSessionAllocationTest, PdfMakesExactlyOneFallibleStateAllocation) {
  const auto state = allocatePdfReaderSessionState<TestPdfReaderState>(ReflowDocumentFormat::Pdf);

  ASSERT_NE(state, nullptr);
  EXPECT_EQ(state->marker, 0x504446U);
  EXPECT_EQ(PdfReaderAllocationProbe::calls, 1U);
}

TEST_F(PdfReaderSessionAllocationTest, PdfAllocationFailureIsReturnedWithoutRetry) {
  PdfReaderAllocationProbe::fail = true;

  const auto state = allocatePdfReaderSessionState<TestPdfReaderState>(ReflowDocumentFormat::Pdf);

  EXPECT_EQ(state, nullptr);
  EXPECT_EQ(PdfReaderAllocationProbe::calls, 1U);
}
}  // namespace
