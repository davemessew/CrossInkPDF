#include <gtest/gtest.h>

#include <GfxRenderer.h>

#include <algorithm>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "Epub/Epub.h"
#include "Epub/Epub/Section.h"
#include "production_stubs/EpubProductionTestState.h"
#include "production_stubs/PdfHalIo.h"
#include "production_stubs/PdfLayoutWordIndex.h"

namespace {

constexpr char kLogicalSectionHref[] = "sections/000000.xhtml";
constexpr char kCacheRoot[] = "/.crosspoint/pdf_123";
constexpr char kBorrowedSectionPath[] = "/.crosspoint/pdf_123/gen_7/sections/000000.xhtml";
constexpr char kPixelCachePath[] =
    "/.crosspoint/pdf_123/gen_7/images/0123456789abcdef-89abcdef.pxc";
constexpr char kLogicalPixelCacheHref[] = "images/0123456789abcdef-89abcdef.pxc";
constexpr char kAbsoluteEpubSectionHref[] = "/OEBPS/text/chapter.xhtml";
constexpr char kRootlessEpubImageHref[] = "OEBPS/images/0123456789abcdef-89abcdef.pxc";
constexpr char kSectionCachePath[] = "/.crosspoint/pdf_123/sections/0.bin";
constexpr char kSectionSidecarPath[] = "/.crosspoint/pdf_123/sections/0.bin.pwi";
constexpr size_t kLegacyPageLutBytes = 1024U * 8U;

class VectorByteSource {
 public:
  explicit VectorByteSource(const std::vector<uint8_t>& bytes) : bytes_(bytes) {}

  PdfByteSource source() { return {this, bytes_.size(), readAt}; }

 private:
  static PdfStatus readAt(void* context, const uint64_t offset, uint8_t* destination, const size_t requested,
                          size_t* bytesRead) {
    if (context == nullptr || destination == nullptr || bytesRead == nullptr) {
      return PdfStatus::failure(PdfError::InvalidArgument, offset);
    }
    auto& self = *static_cast<VectorByteSource*>(context);
    if (offset > self.bytes_.size()) return PdfStatus::failure(PdfError::InvalidOffset, offset);
    const size_t copied = std::min(requested, self.bytes_.size() - static_cast<size_t>(offset));
    std::memcpy(destination, self.bytes_.data() + static_cast<size_t>(offset), copied);
    *bytesRead = copied;
    return PdfStatus::success();
  }

  const std::vector<uint8_t>& bytes_;
};

bool validBoundPair(const std::vector<uint8_t>& section, const std::vector<uint8_t>& sidecar) {
  VectorByteSource sectionSource(section);
  VectorByteSource sidecarSource(sidecar);
  PdfLayoutCacheBinding binding;
  PdfLayoutWordIndexInfo info;
  return pdfComputeLayoutCacheBinding(sectionSource.source(), &binding) &&
         pdfInspectLayoutWordIndex(sidecarSource.source(), &info) &&
         pdfLayoutWordIndexMatchesSectionCache(info, binding);
}

enum class PairState : uint8_t { Old, New, Rejected };

std::optional<PairState> classifyPair(const epub_production_test::StorageBoundarySnapshot& snapshot,
                                      const std::vector<uint8_t>& oldSection,
                                      const std::vector<uint8_t>& oldSidecar,
                                      const std::vector<uint8_t>& newSection,
                                      const std::vector<uint8_t>& newSidecar) {
  const auto section = snapshot.files.find(kSectionCachePath);
  const auto sidecar = snapshot.files.find(kSectionSidecarPath);
  if (section == snapshot.files.end() || sidecar == snapshot.files.end()) return PairState::Rejected;
  if (!validBoundPair(section->second, sidecar->second)) return PairState::Rejected;
  if (section->second == oldSection && sidecar->second == oldSidecar) return PairState::Old;
  if (section->second == newSection && sidecar->second == newSidecar) return PairState::New;
  return std::nullopt;
}

class SectionImageDocument final : public Epub {
 public:
  SectionImageDocument(const ReflowDocumentFormat format, const bool borrowedSection,
                       const char* logicalSectionHref = kLogicalSectionHref,
                       const char* expectedEpubImageHref = kLogicalPixelCacheHref)
      : Epub("/books/fixture.epub", "/.crosspoint"),
        format_(format),
        borrowedSection_(borrowedSection),
        logicalSectionHref_(logicalSectionHref),
        expectedEpubImageHref_(expectedEpubImageHref) {}

  ReflowDocumentFormat getFormat() const override { return format_; }
  const std::string& getCachePath() const override { return cacheRoot_; }
  const std::string& getLanguage() const override { return language_; }
  CssParser* getCssParser() const override { return nullptr; }
  int getSectionCount() const override { return 1; }

  bool getSectionHref(const int sectionIndex, std::string& href) const override {
    href = sectionIndex == 0 ? logicalSectionHref_ : "";
    return !href.empty();
  }

  ReflowSectionInfo getSectionInfo(const int sectionIndex) const override {
    return sectionIndex == 0 ? ReflowSectionInfo{.href = logicalSectionHref_, .byteSize = 64, .wordCount = 0}
                             : ReflowSectionInfo{};
  }

  int getTocEntryCount() const override { return 0; }
  int getTocIndexForSectionIndex(int) const override { return -1; }

  bool getLocalSectionPath(const int sectionIndex, ReflowResource& out) const override {
    if (!borrowedSection_ || sectionIndex != 0) {
      out = ReflowResource::streamed();
      return false;
    }
    out = ReflowResource::borrowedLocalFile(kBorrowedSectionPath, ReflowImageKind::EncodedImage);
    return true;
  }

  bool streamSection(const int sectionIndex, Print& out, const size_t) const override {
    static constexpr char html[] = "<p>fixture</p>";
    return !borrowedSection_ && sectionIndex == 0 &&
           out.write(reinterpret_cast<const uint8_t*>(html), sizeof(html) - 1) == sizeof(html) - 1;
  }

  bool resolveResource(const int sectionIndex, const std::string& href, ReflowResource& out) const override {
    ++resolveResourceCalls_;
    lastResolvedHref_ = href;
    const char* const expectedHref = format_ == ReflowDocumentFormat::Pdf ? kPixelCachePath : expectedEpubImageHref_;
    if (sectionIndex != 0 || href != expectedHref) {
      out = ReflowResource::streamed();
      return false;
    }
    out = ReflowResource::borrowedLocalFile(kPixelCachePath, ReflowImageKind::PixelCache, 320, 200);
    return true;
  }

  int resolveResourceCalls() const { return resolveResourceCalls_; }
  const std::string& lastResolvedHref() const { return lastResolvedHref_; }
  bool validateLayoutWordIndex(const std::string& sectionCachePath, const int sectionIndex,
                               const uint16_t pageCount) const override {
    ++validationCalls_;
    if (sectionIndex != 0) return false;

    HalFile sectionFile;
    if (!Storage.openFileForRead("TEST", sectionCachePath.c_str(), sectionFile)) return false;
    PdfLayoutCacheBinding binding;
    const PdfStatus bindingStatus = pdfComputeLayoutCacheBinding(pdfHalByteSource(sectionFile), &binding);
    const bool sectionClosed = sectionFile.close();
    if (!bindingStatus || !sectionClosed) return false;

    HalFile sidecarFile;
    const std::string sidecarPath = sectionCachePath + ".pwi";
    if (!Storage.openFileForRead("TEST", sidecarPath.c_str(), sidecarFile)) return false;
    PdfLayoutWordIndexInfo info;
    const PdfStatus inspectStatus = pdfInspectLayoutWordIndex(pdfHalByteSource(sidecarFile), &info);
    const bool sidecarClosed = sidecarFile.close();
    const ReflowSectionInfo expected = getSectionInfo(sectionIndex);
    return inspectStatus && sidecarClosed && pdfLayoutWordIndexMatchesSectionCache(info, binding) &&
           info.sectionIndex == static_cast<uint16_t>(sectionIndex) && info.pageCount == pageCount &&
           info.firstGlobalWordOrdinal == expected.firstWordOrdinal && info.sectionWordCount == expected.wordCount;
  }

  bool removeLayoutWordIndex(const std::string& sectionCachePath) const override {
    const std::string path = sectionCachePath + ".pwi";
    return !Storage.exists(path.c_str()) || Storage.remove(path.c_str());
  }
  int validationCalls() const { return validationCalls_; }

 private:
  ReflowDocumentFormat format_;
  bool borrowedSection_;
  const char* logicalSectionHref_;
  const char* expectedEpubImageHref_;
  const std::string cacheRoot_ = kCacheRoot;
  const std::string language_ = "en";
  mutable int resolveResourceCalls_ = 0;
  mutable int validationCalls_ = 0;
  mutable std::string lastResolvedHref_;
};

SectionBuildOptions previewBuild() {
  return {
      .previewAnchor = "fixture",
      .previewMaxPages = 1,
  };
}

bool createTestSection(const std::shared_ptr<ReflowDocument>& document, GfxRenderer& renderer,
                       const bool preview = true) {
  Section section(document, 0, renderer);
  return section.createSectionFile(0, 1.0f, false, false, 0, 600, 800, false, false, 0, false, false, nullptr,
                                   nullptr, nullptr, EpubRenderMode::CrossInkDefault,
                                   preview ? previewBuild() : SectionBuildOptions{});
}

class EpubProductionPathTest : public testing::Test {
 protected:
  void SetUp() override {
    epub_production_test::resetAll();
  }
};

bool allocatedArrayBytes(const size_t bytes) {
  const auto& allocations = epub_production_test::arrayAllocationBytes;
  return std::find(allocations.begin(), allocations.end(), bytes) != allocations.end();
}

void addBorrowedPdfFixtures() {
  epub_production_test::parser.mode = epub_production_test::ParserMode::BorrowedPixelCachePage;
  epub_production_test::parser.paragraphIndex = 17;
  epub_production_test::parser.listItemIndex = 29;
  epub_production_test::storage.addFile(kBorrowedSectionPath, {'<', 'p', '/', '>'});
  epub_production_test::storage.addFile(kPixelCachePath, {0x40, 0x01, 0xC8, 0x00});
}

TEST_F(EpubProductionPathTest, RealLoadReadingPositionRejectsOversizedPhysicalFileBeforeReading) {
  Epub document("/books/fixture.epub", "/cache");
  epub_production_test::storage.reset();
  epub_production_test::storage.addFile(document.getCachePath() + "/progress.bin",
                                       {0x34, 0x12, 0x78, 0x56}, 4096);

  ReflowReadingPosition position;
  position.hasSemanticPosition = true;
  position.hasWordCursor = true;
  position.globalWordOrdinal = 99;
  position.blockWordOffset = 98;
  position.wordCursor = 97;
  std::strcpy(position.blockAnchor, "stale");
  EXPECT_FALSE(document.loadReadingPosition(position));
  EXPECT_EQ(position.sectionIndex, 0);
  EXPECT_EQ(position.pageNumber, 0);
  EXPECT_TRUE(position.hasSemanticPosition);
  EXPECT_TRUE(position.hasWordCursor);
  EXPECT_EQ(position.globalWordOrdinal, 99U);
  EXPECT_EQ(position.blockWordOffset, 98U);
  EXPECT_EQ(position.wordCursor, 97U);
  EXPECT_STREQ(position.blockAnchor, "stale");
  EXPECT_TRUE(epub_production_test::storage.requestedReadCapacities.empty());
  EXPECT_EQ(epub_production_test::storage.closeCount, 1);
}

TEST_F(EpubProductionPathTest, RealLoadReadingPositionRejectsUndersizedPhysicalFileAfterBoundedRead) {
  Epub document("/books/fixture.epub", "/cache");
  epub_production_test::storage.reset();
  epub_production_test::storage.addFile(document.getCachePath() + "/progress.bin",
                                       {0x34, 0x12, 0x78, 0x56, 0xBC, 0x9A}, 1);

  ReflowReadingPosition position;
  EXPECT_FALSE(document.loadReadingPosition(position));
  EXPECT_EQ(epub_production_test::storage.requestedReadCapacities, std::vector<size_t>({1}));
  EXPECT_EQ(epub_production_test::storage.closeCount, 1);
}

TEST_F(EpubProductionPathTest, RealLoadReadingPositionReadsTheExactValidatedLegacyFileSize) {
  Epub fourByteDocument("/books/fixture.epub", "/cache");
  epub_production_test::storage.reset();
  epub_production_test::storage.addFile(fourByteDocument.getCachePath() + "/progress.bin",
                                       {0x34, 0x12, 0x78, 0x56});

  ReflowReadingPosition position;
  ASSERT_TRUE(fourByteDocument.loadReadingPosition(position));
  EXPECT_EQ(position.sectionIndex, 0x1234);
  EXPECT_EQ(position.pageNumber, 0x5678);
  EXPECT_FALSE(position.hasPageCount);
  EXPECT_EQ(epub_production_test::storage.requestedReadCapacities, std::vector<size_t>({4}));
  EXPECT_EQ(epub_production_test::storage.closeCount, 1);

  Epub sixByteDocument("/books/fixture.epub", "/cache");
  epub_production_test::storage.reset();
  epub_production_test::storage.addFile(sixByteDocument.getCachePath() + "/progress.bin",
                                       {0x34, 0x12, 0x78, 0x56, 0xBC, 0x9A});

  ASSERT_TRUE(sixByteDocument.loadReadingPosition(position));
  EXPECT_EQ(position.sectionIndex, 0x1234);
  EXPECT_EQ(position.pageNumber, 0x5678);
  EXPECT_EQ(position.pageCount, 0x9ABC);
  EXPECT_TRUE(position.hasPageCount);
  EXPECT_EQ(epub_production_test::storage.requestedReadCapacities, std::vector<size_t>({6}));
  EXPECT_EQ(epub_production_test::storage.closeCount, 1);
}

TEST_F(EpubProductionPathTest, RealLoadReadingPositionRejectsValidTupleWhenCloseFails) {
  Epub document("/books/fixture.epub", "/cache");
  epub_production_test::storage.reset();
  epub_production_test::storage.addFile(document.getCachePath() + "/progress.bin",
                                       {0x34, 0x12, 0x78, 0x56, 0xBC, 0x9A},
                                       epub_production_test::kUseDataSize,
                                       epub_production_test::kUseDataSize, false);

  ReflowReadingPosition position;
  EXPECT_FALSE(document.loadReadingPosition(position));
  EXPECT_EQ(epub_production_test::storage.requestedReadCapacities, std::vector<size_t>({6}));
  EXPECT_EQ(epub_production_test::storage.closeCount, 1);
}

TEST_F(EpubProductionPathTest, RealLoadReadingPositionRejectsInvalidActualLengthAndRecoversBackup) {
  Epub document("/books/fixture.epub", "/cache");
  const std::string progressPath = document.getCachePath() + "/progress.bin";
  epub_production_test::storage.reset();
  epub_production_test::storage.addFile(progressPath, {0xA5, 0xA5, 0xA5, 0xA5, 0xA5});
  epub_production_test::storage.addFile(progressPath + ".bak", {0x02, 0x00, 0xFF, 0xFF, 0x04, 0x00});

  ReflowReadingPosition position;
  ASSERT_TRUE(document.loadReadingPosition(position));

  EXPECT_EQ(position.sectionIndex, 2);
  EXPECT_EQ(position.pageNumber, 0);
  EXPECT_EQ(position.pageCount, 4);
  EXPECT_TRUE(position.hasPageCount);
  EXPECT_EQ(epub_production_test::storage.requestedReadCapacities, std::vector<size_t>({5, 6}));
  EXPECT_EQ(epub_production_test::storage.closeCount, 2);
}

TEST_F(EpubProductionPathTest, RealSaveReadingPositionWritesSixBytesAndRotatesLegacyBackup) {
  Epub document("/books/fixture.epub", "/cache");
  const std::string progressPath = document.getCachePath() + "/progress.bin";
  const std::string backupPath = progressPath + ".bak";
  const std::string tempPath = progressPath + ".tmp";
  epub_production_test::storage.reset();
  epub_production_test::storage.addFile(progressPath, {0x01, 0x00, 0x02, 0x00});
  epub_production_test::storage.addFile(backupPath, {0xAA});
  epub_production_test::storage.addFile(tempPath, {0xBB});

  const ReflowReadingPosition position = {
      .sectionIndex = 0x1234,
      .pageNumber = 0x5678,
      .pageCount = 0x9ABC,
      .hasPageCount = true,
      .blockAnchor = {},
  };
  ASSERT_TRUE(document.saveReadingPosition(position));

  EXPECT_EQ(epub_production_test::storage.files.at(progressPath).data,
            (std::vector<uint8_t>{0x34, 0x12, 0x78, 0x56, 0xBC, 0x9A}));
  EXPECT_EQ(epub_production_test::storage.files.at(backupPath).data,
            (std::vector<uint8_t>{0x01, 0x00, 0x02, 0x00}));
  EXPECT_FALSE(epub_production_test::storage.files.contains(tempPath));
}

TEST_F(EpubProductionPathTest, RealSaveReadingPositionRestoresPrimaryWhenFinalRenameFails) {
  Epub document("/books/fixture.epub", "/cache");
  const std::string progressPath = document.getCachePath() + "/progress.bin";
  const std::string backupPath = progressPath + ".bak";
  const std::string tempPath = progressPath + ".tmp";
  epub_production_test::storage.reset();
  epub_production_test::storage.addFile(progressPath, {0x01, 0x00, 0x02, 0x00});
  epub_production_test::storage.failRenameOldPath = tempPath;

  const ReflowReadingPosition position = {
      .sectionIndex = 7,
      .pageNumber = 8,
      .pageCount = 9,
      .hasPageCount = true,
      .blockAnchor = {},
  };
  EXPECT_FALSE(document.saveReadingPosition(position));

  EXPECT_EQ(epub_production_test::storage.files.at(progressPath).data,
            (std::vector<uint8_t>{0x01, 0x00, 0x02, 0x00}));
  EXPECT_FALSE(epub_production_test::storage.files.contains(backupPath));
  EXPECT_FALSE(epub_production_test::storage.files.contains(tempPath));
}

TEST_F(EpubProductionPathTest, RealSectionCreationUsesHrefOnlyMetadataPath) {
  auto document = std::make_shared<Epub>("/books/fixture.epub", "/cache");
  ASSERT_TRUE(document->load(false, true));
  epub_production_test::metadata.resetQueryCounts();
  epub_production_test::storage.openedWritePaths.clear();
  epub_production_test::storage.failWrites = true;

  GfxRenderer renderer;
  Section section(document, 1, renderer);
  EXPECT_FALSE(section.createSectionFile(0, 1.0f, false, false, 0, 600, 800, false, false, 0, false, false));

  EXPECT_EQ(epub_production_test::metadata.spineEntryQueries, 1);
  EXPECT_EQ(epub_production_test::metadata.cumulativeSizeQueries, 0);
  EXPECT_EQ(epub_production_test::metadata.tocEntryQueries, 0);
  EXPECT_EQ(epub_production_test::storage.openedWritePaths.size(), 3u);
}

TEST_F(EpubProductionPathTest, BorrowedPdfSectionResolvesRelativePixelCacheFromPhysicalGenerationPath) {
  addBorrowedPdfFixtures();
  auto document = std::make_shared<SectionImageDocument>(ReflowDocumentFormat::Pdf, true);
  GfxRenderer renderer;

  const bool created = createTestSection(document, renderer, /*preview=*/false);

  EXPECT_TRUE(created);
  EXPECT_EQ(epub_production_test::parser.parsePath, kBorrowedSectionPath);
  EXPECT_EQ(epub_production_test::parser.contentBase, "/.crosspoint/pdf_123/gen_7/sections/");
  EXPECT_TRUE(epub_production_test::parser.preserveImagePathRoot);
  EXPECT_EQ(epub_production_test::parser.resolvedImageHref, kPixelCachePath);
  EXPECT_EQ(document->resolveResourceCalls(), 1);
  EXPECT_EQ(document->lastResolvedHref(), kPixelCachePath);
  EXPECT_TRUE(epub_production_test::parser.borrowedPixelCache);
  EXPECT_EQ(epub_production_test::parser.borrowedImagePath, kPixelCachePath);
  EXPECT_EQ(epub_production_test::parser.borrowedImageWidth, 320);
  EXPECT_EQ(epub_production_test::parser.borrowedImageHeight, 200);
  EXPECT_EQ(epub_production_test::parser.serializedPages, 1);
  EXPECT_TRUE(epub_production_test::parser.pageImageFound);
  EXPECT_EQ(epub_production_test::parser.pageImagePath, kPixelCachePath);
  EXPECT_EQ(epub_production_test::parser.pageImageWidth, 320);
  EXPECT_EQ(epub_production_test::parser.pageImageHeight, 200);
  EXPECT_FALSE(allocatedArrayBytes(kLegacyPageLutBytes));
  EXPECT_TRUE(epub_production_test::storage.files.contains(kSectionSidecarPath));
  EXPECT_EQ(document->validationCalls(), 1);
}

TEST_F(EpubProductionPathTest, BorrowedPdfPreviewKeepsCanonicalPixelCachePathWithoutSemanticHooks) {
  addBorrowedPdfFixtures();
  auto document = std::make_shared<SectionImageDocument>(ReflowDocumentFormat::Pdf, true);
  GfxRenderer renderer;

  const bool created = createTestSection(document, renderer, /*preview=*/true);

  EXPECT_TRUE(created);
  EXPECT_EQ(epub_production_test::parser.parsePath, kBorrowedSectionPath);
  EXPECT_EQ(epub_production_test::parser.contentBase, "/.crosspoint/pdf_123/gen_7/sections/");
  EXPECT_FALSE(epub_production_test::parser.semanticPaginationHooksPresent);
  EXPECT_TRUE(epub_production_test::parser.preserveImagePathRoot);
  EXPECT_EQ(epub_production_test::parser.resolvedImageHref, kPixelCachePath);
  EXPECT_EQ(document->lastResolvedHref(), kPixelCachePath);
  EXPECT_TRUE(epub_production_test::parser.borrowedPixelCache);
  EXPECT_EQ(epub_production_test::parser.borrowedImagePath, kPixelCachePath);
  EXPECT_EQ(epub_production_test::parser.borrowedImageWidth, 320);
  EXPECT_EQ(epub_production_test::parser.borrowedImageHeight, 200);
  EXPECT_EQ(epub_production_test::parser.serializedPages, 1);
  EXPECT_TRUE(epub_production_test::parser.pageImageFound);
  EXPECT_EQ(epub_production_test::parser.pageImagePath, kPixelCachePath);
  EXPECT_EQ(epub_production_test::parser.pageImageWidth, 320);
  EXPECT_EQ(epub_production_test::parser.pageImageHeight, 200);
  EXPECT_TRUE(allocatedArrayBytes(kLegacyPageLutBytes));
  EXPECT_FALSE(epub_production_test::storage.files.contains(kSectionSidecarPath));
}

TEST_F(EpubProductionPathTest, BorrowedEpubSectionKeepsLogicalHrefResolutionWithoutPdfHooks) {
  epub_production_test::parser.mode = epub_production_test::ParserMode::BorrowedPixelCachePage;
  epub_production_test::storage.addFile(kBorrowedSectionPath, {'<', 'p', '/', '>'});
  epub_production_test::storage.addFile(kPixelCachePath, {0x40, 0x01, 0xC8, 0x00});
  auto document = std::make_shared<SectionImageDocument>(ReflowDocumentFormat::Epub, true);
  GfxRenderer renderer;

  ASSERT_TRUE(createTestSection(document, renderer));

  EXPECT_EQ(epub_production_test::parser.contentBase, "sections/");
  EXPECT_FALSE(epub_production_test::parser.preserveImagePathRoot);
  EXPECT_EQ(epub_production_test::parser.resolvedImageHref, kLogicalPixelCacheHref);
  EXPECT_EQ(document->lastResolvedHref(), kLogicalPixelCacheHref);
  EXPECT_TRUE(epub_production_test::parser.pageImageFound);
  EXPECT_TRUE(allocatedArrayBytes(kLegacyPageLutBytes));
  EXPECT_FALSE(epub_production_test::storage.files.contains(kSectionSidecarPath));
}

TEST_F(EpubProductionPathTest, EpubPromotionStillAttemptsRenameWhenDestinationRemovalReportsFailure) {
  epub_production_test::parser.mode = epub_production_test::ParserMode::BorrowedPixelCachePage;
  epub_production_test::storage.addFile(kBorrowedSectionPath, {'<', 'p', '/', '>'});
  epub_production_test::storage.addFile(kPixelCachePath, {0x40, 0x01, 0xC8, 0x00});
  epub_production_test::storage.addFile(kSectionCachePath, {0xA5});
  epub_production_test::storage.failRemovePath = kSectionCachePath;
  auto document = std::make_shared<SectionImageDocument>(ReflowDocumentFormat::Epub, true);
  GfxRenderer renderer;

  EXPECT_TRUE(createTestSection(document, renderer));

  EXPECT_EQ(epub_production_test::storage.removeFaultsReached, 1U);
  EXPECT_TRUE(epub_production_test::storage.files.contains(kSectionCachePath));
  EXPECT_NE(epub_production_test::storage.files.at(kSectionCachePath).data, std::vector<uint8_t>({0xA5}));
}

TEST_F(EpubProductionPathTest, PdfExternalLutReplayKeepsVersionFortyFourSectionPrefixIdenticalToLegacyPath) {
  addBorrowedPdfFixtures();
  auto document = std::make_shared<SectionImageDocument>(ReflowDocumentFormat::Pdf, true);
  GfxRenderer renderer;
  ASSERT_TRUE(createTestSection(document, renderer, /*preview=*/false));
  ASSERT_TRUE(epub_production_test::storage.files.contains(kSectionCachePath));
  const std::vector<uint8_t> externalLutBytes = epub_production_test::storage.files.at(kSectionCachePath).data;
  ASSERT_GT(externalLutBytes.size(), 5U);
  EXPECT_EQ(externalLutBytes[4], 44U);

  epub_production_test::resetAll();
  addBorrowedPdfFixtures();
  document = std::make_shared<SectionImageDocument>(ReflowDocumentFormat::Pdf, true);
  ASSERT_TRUE(createTestSection(document, renderer, /*preview=*/true));
  ASSERT_TRUE(epub_production_test::storage.files.contains(kSectionCachePath));
  const std::vector<uint8_t>& legacyBytes = epub_production_test::storage.files.at(kSectionCachePath).data;
  ASSERT_GT(legacyBytes.size(), 5U);
  EXPECT_EQ(legacyBytes[4], 44U);
  ASSERT_EQ(externalLutBytes.size(), legacyBytes.size() + PDF_LAYOUT_CACHE_BINDING_TRAILER_BYTES);
  EXPECT_TRUE(std::equal(legacyBytes.begin(), legacyBytes.end(), externalLutBytes.begin()));
  EXPECT_EQ(std::memcmp(externalLutBytes.data() + legacyBytes.size(), "PWIB", 4), 0);
}

TEST_F(EpubProductionPathTest, PdfBindingCreationNeverRereadsSectionAndValidationReadsOnlyTrailer) {
  constexpr size_t kBindingTrailerBytes = 16;
  addBorrowedPdfFixtures();
  auto document = std::make_shared<SectionImageDocument>(ReflowDocumentFormat::Pdf, true);
  GfxRenderer renderer;

  ASSERT_TRUE(createTestSection(document, renderer, /*preview=*/false));

  const std::string sectionTemp = std::string(kSectionCachePath) + ".tmp";
  std::vector<epub_production_test::StorageReadObservation> temporaryReads;
  std::vector<epub_production_test::StorageReadObservation> finalReads;
  for (const auto& read : epub_production_test::storage.readObservations) {
    if (read.path == sectionTemp) temporaryReads.push_back(read);
    if (read.path == kSectionCachePath) finalReads.push_back(read);
  }
  EXPECT_TRUE(temporaryReads.empty());
  ASSERT_EQ(finalReads.size(), 1U);
  const size_t sectionBytes = epub_production_test::storage.files.at(kSectionCachePath).data.size();
  EXPECT_EQ(finalReads.front().offset, sectionBytes - kBindingTrailerBytes);
  EXPECT_EQ(finalReads.front().requested, kBindingTrailerBytes);
  EXPECT_EQ(finalReads.front().returned, kBindingTrailerBytes);
}

TEST_F(EpubProductionPathTest, PdfPairPublicationBoundarySnapshotsAreOldNewOrFailClosed) {
  addBorrowedPdfFixtures();
  epub_production_test::parser.serializedPageByte = 0x11;
  auto document = std::make_shared<SectionImageDocument>(ReflowDocumentFormat::Pdf, true);
  GfxRenderer renderer;
  ASSERT_TRUE(createTestSection(document, renderer, /*preview=*/false));
  const std::vector<uint8_t> oldSection = epub_production_test::storage.files.at(kSectionCachePath).data;
  const std::vector<uint8_t> oldSidecar = epub_production_test::storage.files.at(kSectionSidecarPath).data;
  ASSERT_TRUE(validBoundPair(oldSection, oldSidecar));

  epub_production_test::resetAll();
  addBorrowedPdfFixtures();
  epub_production_test::parser.serializedPageByte = 0x31;
  epub_production_test::parser.paragraphIndex = 18;
  document = std::make_shared<SectionImageDocument>(ReflowDocumentFormat::Pdf, true);
  ASSERT_TRUE(createTestSection(document, renderer, /*preview=*/false));
  const std::vector<uint8_t> newSection = epub_production_test::storage.files.at(kSectionCachePath).data;
  const std::vector<uint8_t> newSidecar = epub_production_test::storage.files.at(kSectionSidecarPath).data;
  ASSERT_TRUE(validBoundPair(newSection, newSidecar));
  ASSERT_EQ(oldSection.size(), newSection.size());
  ASSERT_NE(oldSection, newSection);
  ASSERT_NE(oldSidecar, newSidecar);
  EXPECT_FALSE(validBoundPair(oldSection, {0xa5U}));
  EXPECT_FALSE(validBoundPair(oldSection, newSidecar));
  EXPECT_FALSE(validBoundPair(newSection, oldSidecar));

  epub_production_test::resetAll();
  addBorrowedPdfFixtures();
  epub_production_test::parser.serializedPageByte = 0x31;
  epub_production_test::parser.paragraphIndex = 18;
  auto& storage = epub_production_test::storage;
  storage.installPriorFilesOnSyncPath = std::string(kSectionCachePath) + ".tmp";
  storage.priorFiles[kSectionCachePath] = oldSection;
  storage.priorFiles[kSectionSidecarPath] = oldSidecar;
  document = std::make_shared<SectionImageDocument>(ReflowDocumentFormat::Pdf, true);
  ASSERT_TRUE(createTestSection(document, renderer, /*preview=*/false));

  bool sawOld = false;
  bool sawRejected = false;
  bool sawNew = false;
  for (const auto& snapshot : storage.boundarySnapshots) {
    const std::optional<PairState> state = classifyPair(snapshot, oldSection, oldSidecar, newSection, newSidecar);
    ASSERT_TRUE(state.has_value()) << snapshot.operation;
    sawOld = sawOld || *state == PairState::Old;
    sawNew = sawNew || *state == PairState::New;
    sawRejected = sawRejected || *state == PairState::Rejected;
  }
  EXPECT_TRUE(sawOld);
  EXPECT_TRUE(sawRejected);
  EXPECT_TRUE(sawNew);
}

TEST_F(EpubProductionPathTest, PublicationFailuresNeverExposeMixedOrUnboundFinalPairs) {
  addBorrowedPdfFixtures();
  epub_production_test::parser.serializedPageByte = 0x11;
  auto document = std::make_shared<SectionImageDocument>(ReflowDocumentFormat::Pdf, true);
  GfxRenderer renderer;
  ASSERT_TRUE(createTestSection(document, renderer, /*preview=*/false));
  const std::vector<uint8_t> oldSection = epub_production_test::storage.files.at(kSectionCachePath).data;
  const std::vector<uint8_t> oldSidecar = epub_production_test::storage.files.at(kSectionSidecarPath).data;
  ASSERT_TRUE(validBoundPair(oldSection, oldSidecar));

  epub_production_test::resetAll();
  addBorrowedPdfFixtures();
  epub_production_test::parser.serializedPageByte = 0x31;
  epub_production_test::parser.paragraphIndex = 18;
  document = std::make_shared<SectionImageDocument>(ReflowDocumentFormat::Pdf, true);
  ASSERT_TRUE(createTestSection(document, renderer, /*preview=*/false));
  const std::vector<uint8_t> newSection = epub_production_test::storage.files.at(kSectionCachePath).data;
  const std::vector<uint8_t> newSidecar = epub_production_test::storage.files.at(kSectionSidecarPath).data;
  ASSERT_TRUE(validBoundPair(newSection, newSidecar));

  enum class Failure : uint8_t { Remove, Rename, Sync, ShortPatch, HeaderPatch, FooterPatch };
  const std::pair<Failure, const char*> failures[] = {
      {Failure::Remove, "remove"},           {Failure::Rename, "rename"},
      {Failure::Sync, "sync"},               {Failure::ShortPatch, "short-patch"},
      {Failure::HeaderPatch, "header-patch"}, {Failure::FooterPatch, "footer-patch"},
  };
  for (const auto& [failure, name] : failures) {
    SCOPED_TRACE(name);
    epub_production_test::resetAll();
    addBorrowedPdfFixtures();
    epub_production_test::parser.serializedPageByte = 0x31;
    epub_production_test::parser.paragraphIndex = 18;
    auto& storage = epub_production_test::storage;
    storage.installPriorFilesOnSyncPath = std::string(kSectionCachePath) + ".tmp";
    storage.priorFiles[kSectionCachePath] = oldSection;
    storage.priorFiles[kSectionSidecarPath] = oldSidecar;
    std::string expectedFaultOperation;
    switch (failure) {
      case Failure::Remove:
        storage.failRemovePath = kSectionSidecarPath;
        expectedFaultOperation = std::string("remove-failed:") + kSectionSidecarPath;
        break;
      case Failure::Rename:
        storage.failRenameOldPath = std::string(kSectionCachePath) + ".tmp";
        expectedFaultOperation = std::string("rename-failed:") + kSectionCachePath + ".tmp->" + kSectionCachePath;
        break;
      case Failure::Sync:
        storage.failSyncPath = std::string(kSectionSidecarPath) + ".tmp";
        storage.failSyncOccurrence = 2;
        expectedFaultOperation = std::string("sync-failed:") + kSectionSidecarPath + ".tmp";
        break;
      case Failure::ShortPatch:
        storage.failPatchWritePath = std::string(kSectionSidecarPath) + ".tmp";
        storage.failPatchWriteOffset = 0;
        storage.shortPatchWrite = true;
        expectedFaultOperation = std::string("patch-short:") + kSectionSidecarPath + ".tmp@0";
        break;
      case Failure::HeaderPatch:
        storage.failPatchWritePath = std::string(kSectionSidecarPath) + ".tmp";
        storage.failPatchWriteOffset = 0;
        expectedFaultOperation = std::string("patch-header-failed:") + kSectionSidecarPath + ".tmp@0";
        break;
      case Failure::FooterPatch:
        storage.failPatchWritePath = std::string(kSectionSidecarPath) + ".tmp";
        storage.failPatchWriteOffset = newSidecar.size() - PDF_LAYOUT_WORD_INDEX_FOOTER_BYTES;
        expectedFaultOperation = std::string("patch-footer-failed:") + kSectionSidecarPath + ".tmp@" +
                                 std::to_string(storage.failPatchWriteOffset);
        break;
    }

    document = std::make_shared<SectionImageDocument>(ReflowDocumentFormat::Pdf, true);
    EXPECT_FALSE(createTestSection(document, renderer, /*preview=*/false));

    bool priorInstalled = false;
    for (const auto& snapshot : storage.boundarySnapshots) {
      if (snapshot.operation == std::string("sync:") + kSectionCachePath + ".tmp") priorInstalled = true;
      const std::optional<PairState> state = classifyPair(snapshot, oldSection, oldSidecar, newSection, newSidecar);
      ASSERT_TRUE(state.has_value()) << snapshot.operation;
      EXPECT_NE(*state, PairState::New) << snapshot.operation;
    }
    EXPECT_TRUE(priorInstalled);
    EXPECT_EQ(std::count_if(storage.boundarySnapshots.begin(), storage.boundarySnapshots.end(),
                            [&](const auto& snapshot) { return snapshot.operation == expectedFaultOperation; }),
              1);
    EXPECT_EQ(storage.removeFaultsReached, failure == Failure::Remove ? 1U : 0U);
    EXPECT_EQ(storage.renameFaultsReached, failure == Failure::Rename ? 1U : 0U);
    EXPECT_EQ(storage.syncFaultsReached, failure == Failure::Sync ? 1U : 0U);
    EXPECT_EQ(storage.shortPatchFaultsReached, failure == Failure::ShortPatch ? 1U : 0U);
    EXPECT_EQ(storage.headerPatchFaultsReached, failure == Failure::HeaderPatch ? 1U : 0U);
    EXPECT_EQ(storage.footerPatchFaultsReached, failure == Failure::FooterPatch ? 1U : 0U);
  }
}

TEST_F(EpubProductionPathTest, NinePageProductionReplayUsesFourRecordPwiWindows) {
  addBorrowedPdfFixtures();
  epub_production_test::parser.pageCount = 9;
  auto document = std::make_shared<SectionImageDocument>(ReflowDocumentFormat::Pdf, true);
  GfxRenderer renderer;

  ASSERT_TRUE(createTestSection(document, renderer, /*preview=*/false));

  const std::string sidecarTemp = std::string(kSectionSidecarPath) + ".tmp";
  std::vector<std::pair<size_t, size_t>> pwiReadsBeforeBinding;
  for (const auto& read : epub_production_test::storage.readObservations) {
    if (read.path == sidecarTemp && pwiReadsBeforeBinding.size() < 14U) {
      pwiReadsBeforeBinding.emplace_back(read.offset, read.requested);
    }
  }
  const std::vector<std::pair<size_t, size_t>> expected = {
      {0, 32},   {392, 16}, {32, 160},  {192, 160}, {352, 40},
      {32, 160}, {192, 160}, {352, 40}, {32, 160},  {192, 160},
      {352, 40}, {32, 160}, {192, 160}, {352, 40},
  };
  EXPECT_EQ(pwiReadsBeforeBinding, expected);
}

TEST_F(EpubProductionPathTest, PostPromotionValidationRejectsCorruptedBoundSidecar) {
  addBorrowedPdfFixtures();
  auto& storage = epub_production_test::storage;
  storage.corruptRenameDestinationPath = kSectionSidecarPath;
  storage.corruptRenameOffset = PDF_LAYOUT_WORD_INDEX_HEADER_BYTES;
  storage.corruptRenameXor = 0x80U;
  auto document = std::make_shared<SectionImageDocument>(ReflowDocumentFormat::Pdf, true);
  GfxRenderer renderer;

  EXPECT_FALSE(createTestSection(document, renderer, /*preview=*/false));
  EXPECT_EQ(document->validationCalls(), 1);
  EXPECT_EQ(storage.renameCorruptionsReached, 1U);
  bool sawInvalidPromotedPair = false;
  const std::string sidecarPromotion =
      std::string("rename:") + kSectionSidecarPath + ".tmp->" + kSectionSidecarPath;
  for (const auto& snapshot : storage.boundarySnapshots) {
    if (snapshot.operation != sidecarPromotion) continue;
    const auto section = snapshot.files.find(kSectionCachePath);
    const auto sidecar = snapshot.files.find(kSectionSidecarPath);
    ASSERT_NE(section, snapshot.files.end());
    ASSERT_NE(sidecar, snapshot.files.end());
    EXPECT_FALSE(validBoundPair(section->second, sidecar->second));
    sawInvalidPromotedPair = true;
  }
  EXPECT_TRUE(sawInvalidPromotedPair);
  EXPECT_FALSE(storage.files.contains(kSectionCachePath));
  EXPECT_FALSE(storage.files.contains(kSectionSidecarPath));
}

TEST_F(EpubProductionPathTest, SidecarPromotionFailureLeavesNoUsableSectionPairOrTemps) {
  addBorrowedPdfFixtures();
  epub_production_test::storage.failRenameOldPath = std::string(kSectionSidecarPath) + ".tmp";
  auto document = std::make_shared<SectionImageDocument>(ReflowDocumentFormat::Pdf, true);
  GfxRenderer renderer;

  EXPECT_FALSE(createTestSection(document, renderer, /*preview=*/false));

  EXPECT_FALSE(epub_production_test::storage.files.contains(kSectionCachePath));
  EXPECT_FALSE(epub_production_test::storage.files.contains(std::string(kSectionCachePath) + ".tmp"));
  EXPECT_FALSE(epub_production_test::storage.files.contains(kSectionSidecarPath));
  EXPECT_FALSE(epub_production_test::storage.files.contains(std::string(kSectionSidecarPath) + ".tmp"));
}

TEST_F(EpubProductionPathTest, SectionPromotionFailureLeavesNoUsableSectionPairOrTemps) {
  addBorrowedPdfFixtures();
  epub_production_test::storage.failRenameOldPath = std::string(kSectionCachePath) + ".tmp";
  auto document = std::make_shared<SectionImageDocument>(ReflowDocumentFormat::Pdf, true);
  GfxRenderer renderer;

  EXPECT_FALSE(createTestSection(document, renderer, /*preview=*/false));

  EXPECT_FALSE(epub_production_test::storage.files.contains(kSectionCachePath));
  EXPECT_FALSE(epub_production_test::storage.files.contains(std::string(kSectionCachePath) + ".tmp"));
  EXPECT_FALSE(epub_production_test::storage.files.contains(kSectionSidecarPath));
  EXPECT_FALSE(epub_production_test::storage.files.contains(std::string(kSectionSidecarPath) + ".tmp"));
}

TEST_F(EpubProductionPathTest, PdfFootnoteRetainsLegacyLutAllocationAndCreatesNoSidecar) {
  addBorrowedPdfFixtures();
  auto document = std::make_shared<SectionImageDocument>(ReflowDocumentFormat::Pdf, true);
  GfxRenderer renderer;
  Section footnote(document, 0, renderer, "_fn_7");

  ASSERT_TRUE(footnote.createSectionFile(0, 1.0f, false, false, 0, 600, 800, false, false, 0, false, false));

  EXPECT_TRUE(allocatedArrayBytes(kLegacyPageLutBytes));
  EXPECT_TRUE(epub_production_test::storage.files.contains("/.crosspoint/pdf_123/sections/0_fn_7.bin"));
  EXPECT_FALSE(epub_production_test::storage.files.contains("/.crosspoint/pdf_123/sections/0_fn_7.bin.pwi"));
}

TEST_F(EpubProductionPathTest, AbsoluteEpubHrefStaysRootlessWithoutPdfHooks) {
  epub_production_test::parser.mode = epub_production_test::ParserMode::BorrowedPixelCachePage;
  epub_production_test::storage.addFile(kBorrowedSectionPath, {'<', 'p', '/', '>'});
  epub_production_test::storage.addFile(kPixelCachePath, {0x40, 0x01, 0xC8, 0x00});
  auto document = std::make_shared<SectionImageDocument>(ReflowDocumentFormat::Epub, true,
                                                         kAbsoluteEpubSectionHref, kRootlessEpubImageHref);
  GfxRenderer renderer;

  ASSERT_TRUE(createTestSection(document, renderer));

  EXPECT_EQ(epub_production_test::parser.parsePath, kBorrowedSectionPath);
  EXPECT_EQ(epub_production_test::parser.contentBase, "/OEBPS/text/");
  EXPECT_FALSE(epub_production_test::parser.preserveImagePathRoot);
  EXPECT_EQ(epub_production_test::parser.resolvedImageHref, kRootlessEpubImageHref);
  EXPECT_EQ(document->lastResolvedHref(), kRootlessEpubImageHref);
  EXPECT_TRUE(epub_production_test::parser.pageImageFound);
}

TEST_F(EpubProductionPathTest, NonBorrowedPdfSectionKeepsLogicalHrefAsContentBase) {
  epub_production_test::parser.mode = epub_production_test::ParserMode::CaptureOnly;
  auto document = std::make_shared<SectionImageDocument>(ReflowDocumentFormat::Pdf, false);
  GfxRenderer renderer;

  ASSERT_TRUE(createTestSection(document, renderer));

  EXPECT_EQ(epub_production_test::parser.contentBase, "sections/");
  EXPECT_EQ(document->resolveResourceCalls(), 0);
}

TEST_F(EpubProductionPathTest, EpubSectionKeepsLogicalHrefAsContentBase) {
  epub_production_test::parser.mode = epub_production_test::ParserMode::CaptureOnly;
  auto document = std::make_shared<SectionImageDocument>(ReflowDocumentFormat::Epub, false);
  GfxRenderer renderer;

  ASSERT_TRUE(createTestSection(document, renderer));

  EXPECT_EQ(epub_production_test::parser.contentBase, "sections/");
  EXPECT_EQ(document->resolveResourceCalls(), 0);
}

}  // namespace
