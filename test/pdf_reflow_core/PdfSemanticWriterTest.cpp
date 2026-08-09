#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

#include "PdfSemanticWriter.h"
#include "PdfTestIo.h"

namespace {

struct BlockCollector {
  std::vector<PdfSemanticBlockRecord> records;

  static PdfStatus emit(void* context, const PdfSemanticBlockRecord& record) {
    auto& collector = *static_cast<BlockCollector*>(context);
    collector.records.push_back(record);
    return PdfStatus::success();
  }

  PdfSemanticBlockSink sink() { return {this, emit}; }
};

struct WriterHarness {
  std::array<uint8_t, PdfSemanticWriterLimits::MinimumOutputBufferBytes> workspace{};
  PdfTestByteSink bytes;
  BlockCollector blocks;
  PdfSemanticWriter writer;

  PdfStatus begin(const uint32_t initialWords = 0) {
    return writer.begin(bytes.sink(), blocks.sink(), {workspace.data(), workspace.size()}, initialWords);
  }

  std::string output() const { return {bytes.bytes().begin(), bytes.bytes().end()}; }
};

PdfStatus write(PdfSemanticWriter& writer, const char* const text) {
  return writer.writeText(reinterpret_cast<const uint8_t*>(text), std::strlen(text));
}

}  // namespace

TEST(SemanticWriterTest, EmitsMinimalEscapedSemanticXhtmlAndWordRecords) {
  WriterHarness harness;
  harness.bytes.setMaximumWrite(3);
  ASSERT_TRUE(harness.begin().ok());

  ASSERT_TRUE(harness.writer.beginBlock({PdfSemanticBlockKind::Heading, 0, 2}).ok());
  ASSERT_TRUE(write(harness.writer, "Chapter & One").ok());
  ASSERT_TRUE(harness.writer.endBlock().ok());

  ASSERT_TRUE(harness.writer.beginBlock({PdfSemanticBlockKind::Paragraph, 1, 0}).ok());
  ASSERT_TRUE(write(harness.writer, "Use <contents> & ").ok());
  static constexpr char HREF[] = "000001.xhtml#b00000002?x=1&y=\"2\"";
  ASSERT_TRUE(harness.writer.beginInternalLink(reinterpret_cast<const uint8_t*>(HREF), sizeof(HREF) - 1).ok());
  ASSERT_TRUE(write(harness.writer, "open index").ok());
  ASSERT_TRUE(harness.writer.endInternalLink().ok());
  ASSERT_TRUE(harness.writer.endBlock().ok());

  static constexpr char LONG_LABEL[] = "1234567890123\xC3\xA9Z";
  ASSERT_TRUE(
      harness.writer.writePublisherPageBreak(reinterpret_cast<const uint8_t*>(LONG_LABEL), sizeof(LONG_LABEL) - 1)
          .ok());

  ASSERT_TRUE(harness.writer.beginTable().ok());
  ASSERT_TRUE(harness.writer.beginTableRow().ok());
  ASSERT_TRUE(harness.writer.beginBlock({PdfSemanticBlockKind::TableCell, 2, 0}).ok());
  ASSERT_TRUE(write(harness.writer, "Name").ok());
  ASSERT_TRUE(harness.writer.endBlock().ok());
  ASSERT_TRUE(harness.writer.beginBlock({PdfSemanticBlockKind::TableCell, 3, 0}).ok());
  ASSERT_TRUE(write(harness.writer, "10").ok());
  ASSERT_TRUE(harness.writer.endBlock().ok());
  ASSERT_TRUE(harness.writer.endTableRow().ok());
  ASSERT_TRUE(harness.writer.endTable().ok());
  ASSERT_TRUE(harness.writer.finish().ok());

  const std::string expected =
      "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
      "<html xmlns=\"http://www.w3.org/1999/xhtml\"><head><meta charset=\"UTF-8\"/></head><body>"
      "<h2 id=\"b00000000\">Chapter &amp; One</h2>"
      "<p id=\"b00000001\">Use &lt;contents&gt; &amp; "
      "<a href=\"000001.xhtml#b00000002?x=1&amp;y=&quot;2&quot;\">open index</a></p>"
      "<span role=\"doc-pagebreak\" aria-label=\"1234567890123\xC3\xA9\"></span>"
      "<table><tbody><tr><td id=\"b00000002\">Name</td><td id=\"b00000003\">10</td></tr></tbody></table>"
      "</body></html>";
  EXPECT_EQ(harness.output(), expected);
  ASSERT_EQ(harness.blocks.records.size(), 4u);
  EXPECT_STREQ(harness.blocks.records[0].anchor, "b00000000");
  EXPECT_EQ(harness.blocks.records[0].cumulativeWordStart, 0u);
  EXPECT_EQ(harness.blocks.records[0].wordCount, 2u);
  EXPECT_STREQ(harness.blocks.records[1].anchor, "b00000001");
  EXPECT_EQ(harness.blocks.records[1].cumulativeWordStart, 2u);
  EXPECT_EQ(harness.blocks.records[1].wordCount, 4u);
  EXPECT_EQ(harness.blocks.records[2].cumulativeWordStart, 6u);
  EXPECT_EQ(harness.blocks.records[2].wordCount, 1u);
  EXPECT_EQ(harness.blocks.records[3].cumulativeWordStart, 7u);
  EXPECT_EQ(harness.blocks.records[3].wordCount, 1u);
  EXPECT_EQ(harness.writer.totalWords(), 8u);
  EXPECT_EQ(harness.output().find("font-size"), std::string::npos);
  EXPECT_EQ(harness.output().find("font-family"), std::string::npos);
  EXPECT_EQ(harness.output().find("position:"), std::string::npos);
}

TEST(SemanticWriterTest, FormatsMaximumAnchorAndRejectsAnchorCollision) {
  WriterHarness maximum;
  ASSERT_TRUE(maximum.begin().ok());
  ASSERT_TRUE(
      maximum.writer.beginBlock({PdfSemanticBlockKind::Paragraph, std::numeric_limits<uint32_t>::max(), 0}).ok());
  ASSERT_TRUE(write(maximum.writer, "last").ok());
  ASSERT_TRUE(maximum.writer.endBlock().ok());
  ASSERT_TRUE(maximum.writer.finish().ok());
  ASSERT_EQ(maximum.blocks.records.size(), 1u);
  EXPECT_STREQ(maximum.blocks.records[0].anchor, "bffffffff");

  WriterHarness collision;
  ASSERT_TRUE(collision.begin().ok());
  ASSERT_TRUE(collision.writer.beginBlock({PdfSemanticBlockKind::Paragraph, 7, 0}).ok());
  ASSERT_TRUE(write(collision.writer, "first").ok());
  ASSERT_TRUE(collision.writer.endBlock().ok());
  EXPECT_EQ(collision.writer.beginBlock({PdfSemanticBlockKind::Paragraph, 7, 0}).error, PdfError::Malformed);
}

TEST(SemanticWriterTest, RejectsCumulativeWordRolloverButAllowsZeroWordBlockAtMaximum) {
  WriterHarness zero;
  ASSERT_TRUE(zero.begin(std::numeric_limits<uint32_t>::max()).ok());
  ASSERT_TRUE(zero.writer.beginBlock({PdfSemanticBlockKind::Paragraph, 0, 0}).ok());
  ASSERT_TRUE(write(zero.writer, "...").ok());
  ASSERT_TRUE(zero.writer.endBlock().ok());
  ASSERT_TRUE(zero.writer.finish().ok());
  EXPECT_EQ(zero.writer.totalWords(), std::numeric_limits<uint32_t>::max());

  WriterHarness overflow;
  ASSERT_TRUE(overflow.begin(std::numeric_limits<uint32_t>::max()).ok());
  ASSERT_TRUE(overflow.writer.beginBlock({PdfSemanticBlockKind::Paragraph, 0, 0}).ok());
  ASSERT_TRUE(write(overflow.writer, "word").ok());
  EXPECT_EQ(overflow.writer.endBlock().error, PdfError::LimitExceeded);
}

TEST(SemanticWriterTest, TruncatesPublisherLabelAtUtf8BoundaryAndRejectsMalformedAttributes) {
  char label[PdfSemanticWriterLimits::PublisherLabelBytes]{};
  size_t labelLength = 0;
  const std::string value = "12345678901234\xC3\xA9";
  ASSERT_TRUE(
      pdfTruncatePublisherLabel(reinterpret_cast<const uint8_t*>(value.data()), value.size(), label, &labelLength)
          .ok());
  EXPECT_EQ(labelLength, 14u);
  EXPECT_STREQ(label, "12345678901234");

  WriterHarness malformed;
  ASSERT_TRUE(malformed.begin().ok());
  ASSERT_TRUE(malformed.writer.beginBlock({PdfSemanticBlockKind::Paragraph, 0, 0}).ok());
  static constexpr uint8_t BAD_UTF8[] = {0xC3, '('};
  EXPECT_EQ(malformed.writer.beginInternalLink(BAD_UTF8, sizeof(BAD_UTF8)).error, PdfError::Malformed);
}

TEST(SemanticWriterTest, BatchesOneByteTextWritesIntoCallerOwnedOutputBuffer) {
  WriterHarness harness;
  ASSERT_TRUE(harness.begin().ok());
  ASSERT_TRUE(harness.writer.beginBlock({PdfSemanticBlockKind::Paragraph, 0, 0}).ok());
  for (size_t index = 0; index < 96; ++index) {
    ASSERT_TRUE(harness.writer.writeText(reinterpret_cast<const uint8_t*>("a"), 1).ok());
  }
  ASSERT_TRUE(harness.writer.endBlock().ok());
  ASSERT_TRUE(harness.writer.finish().ok());
  EXPECT_LT(harness.bytes.writeCount(), 20u);
}

TEST(SemanticWriterTest, CountsAWordSplitAcrossExtractionChunksExactlyOnce) {
  WriterHarness harness;
  ASSERT_TRUE(harness.begin().ok());
  ASSERT_TRUE(harness.writer.beginBlock({PdfSemanticBlockKind::Paragraph, 0, 0}).ok());
  ASSERT_TRUE(write(harness.writer, "inter").ok());
  ASSERT_TRUE(write(harness.writer, "-").ok());
  ASSERT_TRUE(write(harness.writer, "national").ok());
  ASSERT_TRUE(harness.writer.endBlock().ok());
  ASSERT_TRUE(harness.writer.finish().ok());
  ASSERT_EQ(harness.blocks.records.size(), 1u);
  EXPECT_EQ(harness.blocks.records[0].wordCount, 1u);
  EXPECT_EQ(harness.writer.totalWords(), 1u);
  EXPECT_NE(harness.output().find(">inter-national</p>"), std::string::npos);
}

TEST(SemanticWriterTest, NormalizesUnsupportedPhoneticsAndNoBreakSpacesForDeviceFonts) {
  WriterHarness harness;
  ASSERT_TRUE(harness.begin().ok());
  ASSERT_TRUE(harness.writer.beginBlock({PdfSemanticBlockKind::Paragraph, 0, 0}).ok());
  static constexpr uint8_t TEXT[] = {
      'w', 'o', 'r', 'd', 0xc2, 0xa0, 'w', 'r', 'a', 'p', ' ',
      0xc9, 0x99, 0xcb, 0x88, 't', 0xc3, 0xa4, 'm', 'i', 'k',
  };
  ASSERT_TRUE(harness.writer.writeText(TEXT, sizeof(TEXT)).ok());
  ASSERT_TRUE(harness.writer.endBlock().ok());
  ASSERT_TRUE(harness.writer.finish().ok());

  EXPECT_NE(harness.output().find(">word wrap uh-t\xC3\xA4mik</p>"), std::string::npos);
  ASSERT_EQ(harness.blocks.records.size(), 1U);
  EXPECT_EQ(harness.blocks.records[0].wordCount, 3U);
}
