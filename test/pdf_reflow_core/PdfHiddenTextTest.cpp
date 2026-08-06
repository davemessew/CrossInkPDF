#include <gtest/gtest.h>

#include <array>
#include <cstring>
#include <string>

#include "PdfCheckedMath.h"
#include "PdfHiddenText.h"

namespace {

constexpr int32_t fx(const int16_t value) { return PdfFixed16::fromInteger(value).raw; }

PdfTextRun runAt(const int16_t x, const int16_t y, const int16_t width, const int16_t height,
                 const uint16_t flags = PdfTextHidden) {
  PdfTextRun run{};
  run.xMin = fx(x);
  run.yMin = fx(y);
  run.xMax = fx(static_cast<int16_t>(x + width));
  run.yMax = fx(static_cast<int16_t>(y + height));
  run.baselineX = fx(x);
  run.baseline = fx(y);
  run.baselineDx = fx(width);
  run.flags = flags;
  return run;
}

PdfImagePlacement imageAt(const int16_t x, const int16_t y, const int16_t width, const int16_t height) {
  PdfImagePlacement image{};
  image.xMin = fx(x);
  image.yMin = fx(y);
  image.xMax = fx(static_cast<int16_t>(x + width));
  image.yMax = fx(static_cast<int16_t>(y + height));
  return image;
}

PdfHiddenTextContext contextFor(const PdfTextRun* runs, const uint16_t runCount, const uint8_t* text,
                                const size_t textLength, const PdfImagePlacement* images, const uint16_t imageCount) {
  return {{fx(0), fx(0), fx(612), fx(792)}, runs, runCount, text, textLength, images, imageCount};
}

}  // namespace

TEST(PdfHiddenTextTest, RequiresOnPageAreaTransformAndPlausibleImageOverlap) {
  const std::string text = "Readable OCR line";
  std::array<PdfTextRun, 1> runs{runAt(72, 620, 100, 12)};
  runs[0].textLength = static_cast<uint32_t>(text.size());
  const std::array<PdfImagePlacement, 1> images{imageAt(60, 560, 180, 160)};
  PdfHiddenTextContext context = contextFor(runs.data(), runs.size(), reinterpret_cast<const uint8_t*>(text.data()),
                                            text.size(), images.data(), images.size());

  EXPECT_EQ(pdfClassifyHiddenText(context, 0), PdfHiddenTextDecision::Qualified);

  runs[0].baselineDx = 0;
  EXPECT_EQ(pdfClassifyHiddenText(context, 0), PdfHiddenTextDecision::ZeroTransform);
  runs[0] = runAt(72, 620, 0, 12);
  runs[0].textLength = static_cast<uint32_t>(text.size());
  EXPECT_EQ(pdfClassifyHiddenText(context, 0), PdfHiddenTextDecision::ZeroArea);
  runs[0] = runAt(700, 620, 100, 12);
  runs[0].textLength = static_cast<uint32_t>(text.size());
  EXPECT_EQ(pdfClassifyHiddenText(context, 0), PdfHiddenTextDecision::OffPage);
  runs[0] = runAt(72, 400, 100, 12);
  runs[0].textLength = static_cast<uint32_t>(text.size());
  EXPECT_EQ(pdfClassifyHiddenText(context, 0), PdfHiddenTextDecision::NoImageOverlap);
}

TEST(PdfHiddenTextTest, RejectsMetadataNoiseEmptyAndUnmappableCandidates) {
  const std::array<PdfImagePlacement, 1> images{imageAt(60, 560, 180, 160)};
  std::array<PdfTextRun, 1> runs{runAt(72, 620, 100, 12)};

  const std::string metadata = "Producer: scanner-driver";
  runs[0].textLength = static_cast<uint32_t>(metadata.size());
  PdfHiddenTextContext context = contextFor(runs.data(), runs.size(), reinterpret_cast<const uint8_t*>(metadata.data()),
                                            metadata.size(), images.data(), images.size());
  EXPECT_EQ(pdfClassifyHiddenText(context, 0), PdfHiddenTextDecision::MetadataLike);

  const std::array<uint8_t, 2> malformed{0xC0, 0xAF};
  runs[0].textLength = malformed.size();
  context = contextFor(runs.data(), runs.size(), malformed.data(), malformed.size(), images.data(), images.size());
  EXPECT_EQ(pdfClassifyHiddenText(context, 0), PdfHiddenTextDecision::Unmappable);

  runs[0].textLength = 0;
  EXPECT_EQ(pdfClassifyHiddenText(context, 0), PdfHiddenTextDecision::Empty);
}

TEST(PdfHiddenTextTest, DeduplicatesNormalizedVisibleTextOnlyWhenGeometryIsPlausiblyRelated) {
  const std::string hidden = "  DUPLICATE   visible text. ";
  const std::string visible = "Duplicate visible text.";
  std::string text = hidden + visible;
  std::array<PdfTextRun, 2> runs{runAt(72, 620, 145, 12), runAt(72, 720, 145, 12, 0)};
  runs[0].textOffset = 0;
  runs[0].textLength = static_cast<uint32_t>(hidden.size());
  runs[1].textOffset = static_cast<uint32_t>(hidden.size());
  runs[1].textLength = static_cast<uint32_t>(visible.size());
  const std::array<PdfImagePlacement, 1> images{imageAt(60, 560, 180, 160)};
  PdfHiddenTextContext context = contextFor(runs.data(), runs.size(), reinterpret_cast<const uint8_t*>(text.data()),
                                            text.size(), images.data(), images.size());

  EXPECT_EQ(pdfClassifyHiddenText(context, 0), PdfHiddenTextDecision::DuplicateVisible);

  runs[1] = runAt(400, 100, 145, 12, 0);
  runs[1].textOffset = static_cast<uint32_t>(hidden.size());
  runs[1].textLength = static_cast<uint32_t>(visible.size());
  EXPECT_EQ(pdfClassifyHiddenText(context, 0), PdfHiddenTextDecision::Qualified);
}

TEST(PdfHiddenTextTest, DoesNotTreatDifferentVisibleContentAsDuplicate) {
  const std::string hidden = "OCR body line";
  const std::string visible = "Visible caption";
  std::string text = hidden + visible;
  std::array<PdfTextRun, 2> runs{runAt(72, 620, 120, 12), runAt(72, 620, 120, 12, 0)};
  runs[0].textLength = static_cast<uint32_t>(hidden.size());
  runs[1].textOffset = static_cast<uint32_t>(hidden.size());
  runs[1].textLength = static_cast<uint32_t>(visible.size());
  const std::array<PdfImagePlacement, 1> images{imageAt(60, 560, 180, 160)};
  const PdfHiddenTextContext context =
      contextFor(runs.data(), runs.size(), reinterpret_cast<const uint8_t*>(text.data()), text.size(), images.data(),
                 images.size());

  EXPECT_EQ(pdfClassifyHiddenText(context, 0), PdfHiddenTextDecision::Qualified);
}

TEST(PdfHiddenTextTest, KeepsFirstHiddenPeerAndOnlyDeduplicatesAgainstRetainedEarlierRuns) {
  const std::string first = "OCR body line";
  const std::string second = "  ocr   BODY line ";
  const std::string third = "OCR body line";
  const std::string distinct = "OCR body lime";
  const std::string text = first + second + third + distinct;
  std::array<PdfTextRun, 4> runs{
      runAt(72, 200, 100, 12),
      runAt(112, 200, 100, 12),
      runAt(152, 200, 100, 12),
      runAt(72, 200, 100, 12),
  };
  size_t offset = 0;
  const std::array<size_t, 4> lengths{first.size(), second.size(), third.size(), distinct.size()};
  for (size_t index = 0; index < runs.size(); ++index) {
    runs[index].textOffset = static_cast<uint32_t>(offset);
    runs[index].textLength = static_cast<uint32_t>(lengths[index]);
    offset += lengths[index];
  }
  const std::array<PdfImagePlacement, 1> images{imageAt(0, 0, 612, 792)};
  std::array<uint8_t, 1> retained{};
  PdfHiddenTextContext context =
      contextFor(runs.data(), runs.size(), reinterpret_cast<const uint8_t*>(text.data()), text.size(), images.data(),
                 images.size());
  context.retainedHidden = retained.data();
  context.retainedHiddenBytes = retained.size();

  EXPECT_EQ(pdfClassifyHiddenText(context, 0), PdfHiddenTextDecision::Qualified);
  EXPECT_EQ(pdfClassifyHiddenText(context, 1), PdfHiddenTextDecision::DuplicateHidden);
  // Run 1 was suppressed. Run 2 overlaps it, but not retained run 0, so it remains readable.
  EXPECT_EQ(pdfClassifyHiddenText(context, 2), PdfHiddenTextDecision::Qualified);
  EXPECT_EQ(pdfClassifyHiddenText(context, 3), PdfHiddenTextDecision::Qualified);
  EXPECT_EQ(retained[0] & 0x0fU, 0x0dU);
}
