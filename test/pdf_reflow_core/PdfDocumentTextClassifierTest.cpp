#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

#include "PdfDocumentTextClassifier.h"

TEST(PdfDocumentTextClassifierTest, SamplesForUiButWaitsForEveryPageBeforeNoReadableText) {
  PdfDocumentTextClassifier classifier;
  ASSERT_TRUE(classifier.begin(2).ok());
  ASSERT_TRUE(classifier.observePage(0, {0, 0, 1}).ok());
  EXPECT_EQ(classifier.sampledKind(), PdfDocumentTextKind::ImageOnlyCandidate);
  EXPECT_EQ(classifier.finish(PdfStatus::success()).error, PdfError::InvalidArgument);

  ASSERT_TRUE(classifier.observePage(1, {12, 0, 0}).ok());
  EXPECT_EQ(classifier.finish(PdfStatus::success()).error, PdfError::None);
}

TEST(PdfDocumentTextClassifierTest, DistinguishesBornDigitalOcrMixedAndImageOnly) {
  PdfDocumentTextClassifier bornDigital;
  ASSERT_TRUE(bornDigital.begin(1).ok());
  ASSERT_TRUE(bornDigital.observePage(0, {8, 0, 0}).ok());
  EXPECT_EQ(bornDigital.sampledKind(), PdfDocumentTextKind::BornDigital);

  PdfDocumentTextClassifier ocr;
  ASSERT_TRUE(ocr.begin(1).ok());
  ASSERT_TRUE(ocr.observePage(0, {0, 8, 1}).ok());
  EXPECT_EQ(ocr.sampledKind(), PdfDocumentTextKind::HiddenOcr);

  PdfDocumentTextClassifier mixed;
  ASSERT_TRUE(mixed.begin(1).ok());
  ASSERT_TRUE(mixed.observePage(0, {4, 7, 1}).ok());
  EXPECT_EQ(mixed.sampledKind(), PdfDocumentTextKind::Mixed);

  PdfDocumentTextClassifier image;
  ASSERT_TRUE(image.begin(1).ok());
  ASSERT_TRUE(image.observePage(0, {0, 0, 1}).ok());
  EXPECT_EQ(image.finish(PdfStatus::success()).error, PdfError::NoReadableText);
}

TEST(PdfDocumentTextClassifierTest, ScanOnlyFixtureReachesNoReadableTextAfterFullExtraction) {
  const std::filesystem::path path = std::filesystem::path(__FILE__).parent_path() / "fixtures" / "scan_only.pdf";
  std::ifstream input(path, std::ios::binary);
  const std::string bytes{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
  ASSERT_NE(bytes.find("/Subtype /Image"), std::string::npos);
  ASSERT_EQ(bytes.find(" Tj"), std::string::npos);

  PdfDocumentTextClassifier classifier;
  ASSERT_TRUE(classifier.begin(1).ok());
  ASSERT_TRUE(classifier.observePage(0, {0, 0, 1}).ok());
  EXPECT_EQ(classifier.finish(PdfStatus::success()).error, PdfError::NoReadableText);
}

TEST(PdfDocumentTextClassifierTest, PreservesSpecificExtractionFailures) {
  for (const PdfError error :
       {PdfError::UnsupportedEncoding, PdfError::UnsupportedFilter, PdfError::Malformed, PdfError::Encrypted}) {
    PdfDocumentTextClassifier classifier;
    ASSERT_TRUE(classifier.begin(3).ok());
    ASSERT_TRUE(classifier.observePage(0, {0, 0, 1}).ok());
    EXPECT_EQ(classifier.finish(PdfStatus::failure(error, 77)).error, error);
    EXPECT_EQ(classifier.finish(PdfStatus::failure(error, 77)).offset, 77u);
  }
}

TEST(PdfDocumentTextClassifierTest, RejectsOutOfOrderAndOverflowingObservations) {
  PdfDocumentTextClassifier classifier;
  ASSERT_TRUE(classifier.begin(2).ok());
  EXPECT_EQ(classifier.observePage(1, {1, 0, 0}).error, PdfError::InvalidArgument);
  ASSERT_TRUE(classifier.observePage(0, {UINT32_MAX, 0, 0}).ok());
  EXPECT_EQ(classifier.observePage(1, {1, 0, 0}).error, PdfError::LimitExceeded);
}
