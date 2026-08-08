#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <limits>
#include <vector>

#include "PdfCheckedMath.h"
#include "PdfIo.h"
#include "PdfLimits.h"
#include "PdfTestIo.h"

TEST(PdfByteSourceTest, CompletesAcrossShortReads) {
  PdfTestByteSource memory({0x25, 0x50, 0x44, 0x46, 0x2D, 0x31, 0x2E, 0x37});
  memory.setMaximumRead(2);
  const PdfByteSource source = memory.source();
  std::array<uint8_t, 8> output{};

  const PdfStatus status = pdfReadExact(source, 0, output.data(), output.size());

  EXPECT_EQ(status.error, PdfError::None);
  EXPECT_EQ(output, (std::array<uint8_t, 8>{0x25, 0x50, 0x44, 0x46, 0x2D, 0x31, 0x2E, 0x37}));
}

TEST(PdfByteSourceTest, RejectsOutOfRangeSliceBeforeCallback) {
  PdfTestByteSource memory({1, 2, 3, 4});
  const PdfByteSource source = memory.source();
  std::array<uint8_t, 2> output{};

  const PdfStatus status = pdfReadExact(source, 3, output.data(), output.size());

  EXPECT_EQ(status.error, PdfError::InvalidOffset);
  EXPECT_EQ(status.offset, 3u);
}

TEST(PdfByteSourceTest, RejectsCheckedAdditionOverflow) {
  PdfTestByteSource memory({1, 2, 3, 4});
  const PdfByteSource source = memory.source(std::numeric_limits<uint64_t>::max());
  uint8_t output = 0;

  const PdfStatus status = pdfReadExact(source, std::numeric_limits<uint64_t>::max(), &output, static_cast<size_t>(2));

  EXPECT_EQ(status.error, PdfError::InvalidOffset);
  EXPECT_EQ(status.offset, std::numeric_limits<uint64_t>::max());
}

TEST(PdfByteSourceTest, ReportsUnexpectedEofAtExactMissingOffset) {
  PdfTestByteSource memory({10, 20, 30});
  const PdfByteSource source = memory.source(5);
  std::array<uint8_t, 5> output{};

  const PdfStatus status = pdfReadExact(source, 0, output.data(), output.size());

  EXPECT_EQ(status.error, PdfError::UnexpectedEof);
  EXPECT_EQ(status.offset, 3u);
}

TEST(PdfByteSourceTest, BudgetOneProducesSameOutputAsUnboundedRead) {
  const std::vector<uint8_t> expected{1, 3, 5, 7, 9, 11, 13};
  PdfTestByteSource memory(expected);
  const PdfByteSource source = memory.source();
  std::vector<uint8_t> output(expected.size());
  PdfReadExactState state{0, output.data(), output.size(), 0};

  PdfStepResult result;
  do {
    PdfWorkBudget budget{1, 1};
    result = pdfStepReadExact(source, state, budget);
  } while (result.yielded());

  EXPECT_TRUE(result.complete());
  EXPECT_EQ(result.status.error, PdfError::None);
  EXPECT_EQ(output, expected);
}

TEST(PdfByteSourceTest, BoundedRangeTranslatesOffsetsWithoutEscapingParent) {
  PdfTestByteSource memory({0, 1, 2, 3, 4, 5});
  const PdfByteSource parent = memory.source();
  PdfByteRange range;
  ASSERT_TRUE(pdfInitializeByteRange(parent, 2, 3, &range).ok());
  const PdfByteSource source = pdfByteRangeSource(range);
  std::array<uint8_t, 3> output{};
  EXPECT_TRUE(pdfReadExact(source, 0, output.data(), output.size()).ok());
  EXPECT_EQ(output, (std::array<uint8_t, 3>{2, 3, 4}));
  uint8_t byte = 0;
  EXPECT_EQ(pdfReadExact(source, 3, &byte, 1).error, PdfError::InvalidOffset);
}

TEST(PdfCheckedMathTest, RejectsOverflowAndTransformsFixedPoint) {
  uint64_t result = 0;
  EXPECT_FALSE(pdfCheckedAdd(std::numeric_limits<uint64_t>::max(), 1, &result));
  EXPECT_FALSE(pdfCheckedMultiply(std::numeric_limits<uint64_t>::max(), 2, &result));
  EXPECT_TRUE(pdfCheckedRange(4, 6, 10, &result));
  EXPECT_EQ(result, 10u);

  PdfMatrix matrix;
  matrix.e = PdfFixed16::fromInteger(2);
  matrix.f = PdfFixed16::fromInteger(-3);
  PdfFixed16 x;
  PdfFixed16 y;
  ASSERT_TRUE(pdfTransformPoint(matrix, PdfFixed16::fromInteger(4), PdfFixed16::fromInteger(7), &x, &y));
  EXPECT_EQ(x.raw, PdfFixed16::fromInteger(6).raw);
  EXPECT_EQ(y.raw, PdfFixed16::fromInteger(4).raw);
}

TEST(PdfLimitsTest, ProductionBoundsMatchApprovedEnvelope) {
  EXPECT_EQ(PdfLimits::MaxIndirectObjectNumber, 8'388'607u);
  EXPECT_EQ(PdfLimits::MaxXrefRecords, PdfLimits::MaxIndirectObjectNumber + 1U);
  EXPECT_EQ(PdfLimits::MaxPages, static_cast<uint32_t>(UINT16_MAX));
  EXPECT_EQ(PdfLimits::MaxOperatorsPerPage, 250000u);
  EXPECT_EQ(PdfLimits::MaxOperatorsPerDocument, 10000000u);
  EXPECT_EQ(PdfLimits::MaxFormDepth, 16u);
  EXPECT_EQ(PdfLimits::MaxExpandedRequiredStreamBytes, 64ULL * 1024ULL * 1024ULL);
  EXPECT_EQ(PdfLimits::MaxExpansionRatio, 200u);
  EXPECT_EQ(PdfLimits::MaxFiltersPerStream, 4u);
  EXPECT_EQ(PdfLimits::MaxXrefFieldBytes, 8u);
  EXPECT_EQ(PdfLimits::MaxXrefEntryBytes, 24u);
  EXPECT_EQ(PdfLimits::XrefMergeEntries, 64u);
  EXPECT_EQ(PdfLimits::MaxImagePixels, 16000000u);
  EXPECT_EQ(PdfLimits::MaxDecodedImageRowBytes, 8192u);
  EXPECT_LE(PdfLimits::TotalWorkspaceBytes, 63488u);
  EXPECT_LE(PdfLimits::MaxIndividualWorkspaceBytes, 32768u);
}
