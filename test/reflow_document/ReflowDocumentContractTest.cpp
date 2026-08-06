#include <gtest/gtest.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

#include "Reflow/ReflowDocument.h"

class Print {};
class CssParser {};
class GfxRenderer {};

namespace {

class FakeReflowDocument final : public ReflowDocument {
 public:
  explicit FakeReflowDocument(bool* destroyed) : destroyed_(destroyed) {}

  ~FakeReflowDocument() override {
    if (destroyed_ != nullptr) {
      *destroyed_ = true;
    }
  }

  ReflowDocumentFormat getFormat() const override { return ReflowDocumentFormat::Pdf; }

  const char* getStoreFormatKey() const override { return "pdf"; }

  ReflowCapabilitySet getCapabilities() const override {
    return ReflowCapability::NearbyProgressSync | ReflowCapability::EmbeddedStyles | ReflowCapability::SavedItems;
  }

  const std::string& getPath() const override { return path_; }

  const std::string& getCachePath() const override { return cachePath_; }

  const std::string& getTitle() const override { return title_; }

  const std::string& getAuthor() const override { return author_; }

  const std::string& getLanguage() const override { return language_; }

  std::string getCoverBmpPath(const bool cropped = false) const override {
    return cropped ? cachePath_ + "/cover_cropped.bmp" : cachePath_ + "/cover.bmp";
  }

  bool generateCoverBmp(const bool cropped = false, const GfxRenderer* = nullptr, const int = 0) const override {
    coverGenerationWasCropped_ = cropped;
    ++coverGenerationCount_;
    return true;
  }

  std::string getThumbBmpPath() const override { return cachePath_ + "/thumb.bmp"; }

  std::string getThumbBmpPath(const int width, const int height) const override {
    return cachePath_ + "/thumb_" + std::to_string(width) + "x" + std::to_string(height) + ".bmp";
  }

  std::string getAdaptiveThumbBmpPath(const int width, const int height) const override {
    return cachePath_ + "/thumb_adaptive_" + std::to_string(width) + "x" + std::to_string(height) + ".bmp";
  }

  bool generateThumbBmp(const int width, const int height, const GfxRenderer* = nullptr, const int = 0) const override {
    generatedThumbWidth_ = width;
    generatedThumbHeight_ = height;
    return true;
  }

  bool generateAdaptiveThumbBmp(const int width, const int height, const GfxRenderer* = nullptr,
                                const int = 0) const override {
    generatedAdaptiveThumbWidth_ = width;
    generatedAdaptiveThumbHeight_ = height;
    return true;
  }

  int getSectionCount() const override { return 2; }

  bool getSectionHref(const int sectionIndex, std::string& href) const override {
    ++sectionHrefQueries_;
    if (sectionIndex < 0 || sectionIndex >= getSectionCount()) {
      href.clear();
      return false;
    }
    href = sectionIndex == 0 ? "sections/0.xhtml" : "sections/1.xhtml";
    return true;
  }

  ReflowSectionInfo getSectionInfo(const int sectionIndex) const override {
    ++sectionInfoQueries_;
    if (sectionIndex == 0) {
      return {
          .href = "sections/0.xhtml",
          .title = "Part One",
          .byteSize = 1000,
          .cumulativeSize = 0,
          .firstWordOrdinal = 0,
          .wordCount = 277,
          .tocIndex = 0,
      };
    }

    return {
        .href = "sections/1.xhtml",
        .title = "Chapter Two",
        .byteSize = 3000,
        .cumulativeSize = 1000,
        .firstWordOrdinal = 277,
        .wordCount = 500,
        .tocIndex = 1,
    };
  }

  bool getSectionSize(const int sectionIndex, size_t* size) const override {
    if (size == nullptr || sectionIndex < 0 || sectionIndex >= getSectionCount()) {
      return false;
    }
    *size = getSectionInfo(sectionIndex).byteSize;
    return true;
  }

  size_t getCumulativeSectionSize(const int sectionIndex) const override {
    return getSectionInfo(sectionIndex).cumulativeSize;
  }

  size_t getDocumentSize() const override { return 4000; }

  int getSectionIndexForTextReference() const override { return 0; }

  int getTocEntryCount() const override { return 2; }

  ReflowTocEntry getTocEntry(const int tocIndex) const override {
    if (tocIndex == 0) {
      return {
          .title = "Part One",
          .href = "sections/0.xhtml",
          .anchor = "",
          .level = 0,
          .sectionIndex = 0,
          .parentIndex = -1,
      };
    }

    return {
        .title = "Chapter Two",
        .href = "sections/1.xhtml#start",
        .anchor = "start",
        .level = 1,
        .sectionIndex = 1,
        .parentIndex = 0,
    };
  }

  int getSectionIndexForTocIndex(const int tocIndex) const override { return getTocEntry(tocIndex).sectionIndex; }

  int getTocIndexForSectionIndex(const int sectionIndex) const override {
    return sectionIndex >= 0 && sectionIndex < getSectionCount() ? sectionIndex : -1;
  }

  int resolveHrefToSectionIndex(const std::string& href) const override {
    if (href == "sections/0.xhtml") {
      return 0;
    }
    if (href == "sections/1.xhtml" || href == "sections/1.xhtml#start") {
      return 1;
    }
    return -1;
  }

  float calculateSizeProgress(const int sectionIndex, const float sectionProgress) const override {
    const auto info = getSectionInfo(sectionIndex);
    return (static_cast<float>(info.cumulativeSize) + static_cast<float>(info.byteSize) * sectionProgress) /
           static_cast<float>(getDocumentSize());
  }

  float calculateProgress(const int sectionIndex, const float sectionProgress) const override {
    return calculateSizeProgress(sectionIndex, sectionProgress);
  }

  bool resolveProgressPercentToSection(const int percent, int& sectionIndex, float& sectionProgress) const override {
    if (percent < 0 || percent > 100) {
      return false;
    }

    const auto byteOffset = static_cast<size_t>(percent) * getDocumentSize() / 100u;
    if (byteOffset < 1000u) {
      sectionIndex = 0;
      sectionProgress = static_cast<float>(byteOffset) / 1000.0F;
    } else {
      sectionIndex = 1;
      sectionProgress = static_cast<float>(byteOffset - 1000u) / 3000.0F;
    }
    return true;
  }

  bool hasStableReferencePages() const override { return true; }

  bool resolveReferencePage(const int sectionIndex, const float sectionProgress, uint32_t& currentPage,
                            uint32_t& pageCount) const override {
    if (sectionIndex < 0 || sectionIndex >= getSectionCount()) {
      return false;
    }
    pageCount = 20;
    currentPage =
        static_cast<uint32_t>(sectionIndex * 10) + static_cast<uint32_t>(std::floor(sectionProgress * 10.0F)) + 1u;
    return true;
  }

  uint32_t getTotalWordCount() const override { return 777; }

  bool loadReadingPosition(ReflowReadingPosition& position) const override {
    if (!hasSavedPosition_) {
      return false;
    }
    position = savedPosition_;
    return true;
  }

  bool saveReadingPosition(const ReflowReadingPosition& position) const override {
    savedPosition_ = position;
    hasSavedPosition_ = true;
    return true;
  }

  bool getLocalSectionPath(const int sectionIndex, ReflowResource& out) const override {
    if (sectionIndex != 1) {
      return false;
    }
    ++localSectionDispatchCount_;
    out = ReflowResource::borrowedLocalFile(cachePath_ + "/section-1.xhtml", ReflowImageKind::EncodedImage);
    return true;
  }

  bool streamSection(const int sectionIndex, Print&, const size_t chunkSize) const override {
    if (sectionIndex < 0 || sectionIndex >= getSectionCount() || chunkSize == 0) {
      return false;
    }
    ++streamSectionDispatchCount_;
    lastChunkSize_ = chunkSize;
    return true;
  }

  bool resolveResource(const int sectionIndex, const std::string& href, ReflowResource& out) const override {
    if (sectionIndex != 1 || href != "images/figure.pxc") {
      return false;
    }
    ++resolveResourceDispatchCount_;
    out = ReflowResource::borrowedLocalFile(cachePath_ + "/images/figure.pxc", ReflowImageKind::PixelCache, 320, 200);
    return true;
  }

  bool streamResource(const int sectionIndex, const std::string& href, Print&, const size_t chunkSize) const override {
    if (sectionIndex != 1 || href != "images/figure.jpg" || chunkSize == 0) {
      return false;
    }
    ++streamResourceDispatchCount_;
    lastChunkSize_ = chunkSize;
    return true;
  }

  bool getResourceSize(const int sectionIndex, const std::string& href, size_t* size) const override {
    if (sectionIndex != 1 || href != "images/figure.jpg" || size == nullptr) {
      return false;
    }
    *size = 4096;
    return true;
  }

  CssParser* getCssParser() const override { return &cssParser_; }

  int localSectionDispatchCount() const { return localSectionDispatchCount_; }

  int streamSectionDispatchCount() const { return streamSectionDispatchCount_; }

  int resolveResourceDispatchCount() const { return resolveResourceDispatchCount_; }

  int streamResourceDispatchCount() const { return streamResourceDispatchCount_; }

  size_t lastChunkSize() const { return lastChunkSize_; }

  int coverGenerationCount() const { return coverGenerationCount_; }

  bool coverGenerationWasCropped() const { return coverGenerationWasCropped_; }

  int generatedThumbWidth() const { return generatedThumbWidth_; }

  int generatedThumbHeight() const { return generatedThumbHeight_; }

  int generatedAdaptiveThumbWidth() const { return generatedAdaptiveThumbWidth_; }

  int generatedAdaptiveThumbHeight() const { return generatedAdaptiveThumbHeight_; }

  int sectionHrefQueries() const { return sectionHrefQueries_; }

  int sectionInfoQueries() const { return sectionInfoQueries_; }

 private:
  bool* destroyed_;
  const std::string path_ = "/books/sample.pdf";
  const std::string cachePath_ = "/.crosspoint/pdf_42";
  const std::string title_ = "Sample";
  const std::string author_ = "Reader";
  const std::string language_ = "en";
  mutable CssParser cssParser_;
  mutable ReflowReadingPosition savedPosition_;
  mutable bool hasSavedPosition_ = false;
  mutable int localSectionDispatchCount_ = 0;
  mutable int streamSectionDispatchCount_ = 0;
  mutable int resolveResourceDispatchCount_ = 0;
  mutable int streamResourceDispatchCount_ = 0;
  mutable size_t lastChunkSize_ = 0;
  mutable int coverGenerationCount_ = 0;
  mutable bool coverGenerationWasCropped_ = false;
  mutable int generatedThumbWidth_ = 0;
  mutable int generatedThumbHeight_ = 0;
  mutable int generatedAdaptiveThumbWidth_ = 0;
  mutable int generatedAdaptiveThumbHeight_ = 0;
  mutable int sectionHrefQueries_ = 0;
  mutable int sectionInfoQueries_ = 0;
};

TEST(ReflowDocumentContract, CapabilityBitsComposeAndQuery) {
  EXPECT_EQ(reflowCapabilityMask(ReflowCapability::ExternalProgressSync), 1u << 0);
  EXPECT_EQ(reflowCapabilityMask(ReflowCapability::NearbyProgressSync), 1u << 1);
  EXPECT_EQ(reflowCapabilityMask(ReflowCapability::PublisherRenderModes), 1u << 2);
  EXPECT_EQ(reflowCapabilityMask(ReflowCapability::EmbeddedStyles), 1u << 3);
  EXPECT_EQ(reflowCapabilityMask(ReflowCapability::SavedItems), 1u << 4);

  const auto capabilities =
      ReflowCapability::ExternalProgressSync | ReflowCapability::EmbeddedStyles | ReflowCapability::SavedItems;
  EXPECT_TRUE(hasReflowCapability(capabilities, ReflowCapability::ExternalProgressSync));
  EXPECT_TRUE(hasReflowCapability(capabilities, ReflowCapability::EmbeddedStyles));
  EXPECT_TRUE(hasReflowCapability(capabilities, ReflowCapability::SavedItems));
  EXPECT_FALSE(hasReflowCapability(capabilities, ReflowCapability::NearbyProgressSync));
  EXPECT_FALSE(hasReflowCapability(capabilities, ReflowCapability::PublisherRenderModes));
}

TEST(ReflowDocumentContract, BorrowedResourcesAreImmutableAndStreamedResourcesStayOwned) {
  const auto borrowed = ReflowResource::borrowedLocalFile("/cache/image.pxc", ReflowImageKind::PixelCache, 320, 200);
  EXPECT_EQ(borrowed.kind, ReflowResourceKind::BorrowedLocalFile);
  EXPECT_EQ(borrowed.imageKind, ReflowImageKind::PixelCache);
  EXPECT_EQ(borrowed.localPath, "/cache/image.pxc");
  EXPECT_EQ(borrowed.width, 320);
  EXPECT_EQ(borrowed.height, 200);
  EXPECT_FALSE(borrowed.paginatorMayDelete);

  const auto streamed = ReflowResource::streamed(ReflowImageKind::EncodedImage);
  EXPECT_EQ(streamed.kind, ReflowResourceKind::Streamed);
  EXPECT_EQ(streamed.imageKind, ReflowImageKind::EncodedImage);
  EXPECT_TRUE(streamed.localPath.empty());
  EXPECT_FALSE(streamed.paginatorMayDelete);
}

TEST(ReflowDocumentContract, VirtualDispatchCoversMetadataNavigationResourcesAndPosition) {
  bool destroyed = false;
  {
    auto fake = std::make_unique<FakeReflowDocument>(&destroyed);
    FakeReflowDocument* const observed = fake.get();
    std::unique_ptr<ReflowDocument> document = std::move(fake);

    EXPECT_EQ(document->getFormat(), ReflowDocumentFormat::Pdf);
    EXPECT_STREQ(document->getStoreFormatKey(), "pdf");
    EXPECT_TRUE(document->hasCapability(ReflowCapability::NearbyProgressSync));
    EXPECT_TRUE(document->hasCapability(ReflowCapability::EmbeddedStyles));
    EXPECT_TRUE(document->hasCapability(ReflowCapability::SavedItems));
    EXPECT_FALSE(document->hasCapability(ReflowCapability::PublisherRenderModes));

    EXPECT_EQ(document->getPath(), "/books/sample.pdf");
    EXPECT_EQ(document->getCachePath(), "/.crosspoint/pdf_42");
    EXPECT_EQ(document->getTitle(), "Sample");
    EXPECT_EQ(document->getAuthor(), "Reader");
    EXPECT_EQ(document->getLanguage(), "en");
    EXPECT_EQ(document->getCoverBmpPath(), "/.crosspoint/pdf_42/cover.bmp");
    EXPECT_EQ(document->getCoverBmpPath(true), "/.crosspoint/pdf_42/cover_cropped.bmp");
    EXPECT_TRUE(document->generateCoverBmp(true));
    EXPECT_EQ(observed->coverGenerationCount(), 1);
    EXPECT_TRUE(observed->coverGenerationWasCropped());
    EXPECT_EQ(document->getThumbBmpPath(), "/.crosspoint/pdf_42/thumb.bmp");
    EXPECT_EQ(document->getThumbBmpPath(120, 90), "/.crosspoint/pdf_42/thumb_120x90.bmp");
    EXPECT_EQ(document->getAdaptiveThumbBmpPath(120, 90), "/.crosspoint/pdf_42/thumb_adaptive_120x90.bmp");
    EXPECT_TRUE(document->generateThumbBmp(120, 90));
    EXPECT_EQ(observed->generatedThumbWidth(), 120);
    EXPECT_EQ(observed->generatedThumbHeight(), 90);
    EXPECT_TRUE(document->generateAdaptiveThumbBmp(80, 60));
    EXPECT_EQ(observed->generatedAdaptiveThumbWidth(), 80);
    EXPECT_EQ(observed->generatedAdaptiveThumbHeight(), 60);

    std::string sectionHref;
    ASSERT_TRUE(document->getSectionHref(1, sectionHref));
    EXPECT_EQ(sectionHref, "sections/1.xhtml");
    EXPECT_EQ(observed->sectionHrefQueries(), 1);
    EXPECT_EQ(observed->sectionInfoQueries(), 0);

    EXPECT_EQ(document->getSectionCount(), 2);
    const auto section = document->getSectionInfo(1);
    EXPECT_EQ(section.href, "sections/1.xhtml");
    EXPECT_EQ(section.title, "Chapter Two");
    EXPECT_EQ(section.byteSize, 3000u);
    EXPECT_EQ(section.cumulativeSize, 1000u);
    EXPECT_EQ(section.firstWordOrdinal, 277u);
    EXPECT_EQ(section.wordCount, 500u);
    EXPECT_EQ(section.tocIndex, 1);
    size_t sectionSize = 0;
    EXPECT_TRUE(document->getSectionSize(1, &sectionSize));
    EXPECT_EQ(sectionSize, 3000u);
    EXPECT_EQ(document->getCumulativeSectionSize(1), 1000u);
    EXPECT_EQ(document->getDocumentSize(), 4000u);
    EXPECT_EQ(document->getSectionIndexForTextReference(), 0);

    EXPECT_EQ(document->getTocEntryCount(), 2);
    const auto tocEntry = document->getTocEntry(1);
    EXPECT_EQ(tocEntry.title, "Chapter Two");
    EXPECT_EQ(tocEntry.href, "sections/1.xhtml#start");
    EXPECT_EQ(tocEntry.anchor, "start");
    EXPECT_EQ(tocEntry.level, 1);
    EXPECT_EQ(tocEntry.sectionIndex, 1);
    EXPECT_EQ(tocEntry.parentIndex, 0);
    EXPECT_EQ(document->getSectionIndexForTocIndex(1), 1);
    EXPECT_EQ(document->getTocIndexForSectionIndex(1), 1);
    EXPECT_EQ(document->resolveHrefToSectionIndex("sections/1.xhtml#start"), 1);

    EXPECT_FLOAT_EQ(document->calculateSizeProgress(1, 0.5F), 0.625F);
    EXPECT_FLOAT_EQ(document->calculateProgress(1, 0.5F), 0.625F);
    int resolvedSection = -1;
    float resolvedProgress = -1.0F;
    EXPECT_TRUE(document->resolveProgressPercentToSection(50, resolvedSection, resolvedProgress));
    EXPECT_EQ(resolvedSection, 1);
    EXPECT_FLOAT_EQ(resolvedProgress, 1.0F / 3.0F);
    EXPECT_TRUE(document->hasStableReferencePages());
    uint32_t referencePage = 0;
    uint32_t referencePageCount = 0;
    EXPECT_TRUE(document->resolveReferencePage(1, 0.5F, referencePage, referencePageCount));
    EXPECT_EQ(referencePage, 16u);
    EXPECT_EQ(referencePageCount, 20u);
    EXPECT_EQ(document->getTotalWordCount(), 777u);

    Print output;
    ReflowResource localSection;
    EXPECT_TRUE(document->getLocalSectionPath(1, localSection));
    EXPECT_EQ(localSection.kind, ReflowResourceKind::BorrowedLocalFile);
    EXPECT_EQ(localSection.localPath, "/.crosspoint/pdf_42/section-1.xhtml");
    EXPECT_EQ(observed->localSectionDispatchCount(), 1);
    EXPECT_TRUE(document->streamSection(0, output, 512));
    EXPECT_EQ(observed->streamSectionDispatchCount(), 1);
    EXPECT_EQ(observed->lastChunkSize(), 512u);

    ReflowResource resource;
    EXPECT_TRUE(document->resolveResource(1, "images/figure.pxc", resource));
    EXPECT_EQ(resource.kind, ReflowResourceKind::BorrowedLocalFile);
    EXPECT_EQ(resource.imageKind, ReflowImageKind::PixelCache);
    EXPECT_EQ(resource.width, 320);
    EXPECT_EQ(resource.height, 200);
    EXPECT_EQ(observed->resolveResourceDispatchCount(), 1);
    EXPECT_TRUE(document->streamResource(1, "images/figure.jpg", output, 1024));
    EXPECT_EQ(observed->streamResourceDispatchCount(), 1);
    EXPECT_EQ(observed->lastChunkSize(), 1024u);
    size_t resourceSize = 0;
    EXPECT_TRUE(document->getResourceSize(1, "images/figure.jpg", &resourceSize));
    EXPECT_EQ(resourceSize, 4096u);
    EXPECT_NE(document->getCssParser(), nullptr);

    const ReflowReadingPosition saved = {
        .sectionIndex = 1,
        .pageNumber = 4,
        .pageCount = 11,
        .hasPageCount = true,
        .hasSemanticPosition = true,
        .globalWordOrdinal = 421,
        .blockWordOffset = 17,
        .blockAnchor = "block-23",
    };
    EXPECT_TRUE(document->saveReadingPosition(saved));
    ReflowReadingPosition loaded;
    EXPECT_TRUE(document->loadReadingPosition(loaded));
    EXPECT_EQ(loaded.sectionIndex, 1);
    EXPECT_EQ(loaded.pageNumber, 4);
    EXPECT_EQ(loaded.pageCount, 11);
    EXPECT_TRUE(loaded.hasPageCount);
    EXPECT_TRUE(loaded.hasSemanticPosition);
    EXPECT_EQ(loaded.globalWordOrdinal, 421u);
    EXPECT_EQ(loaded.blockWordOffset, 17u);
    EXPECT_STREQ(loaded.blockAnchor, "block-23");
  }
  EXPECT_TRUE(destroyed);
}

}  // namespace
