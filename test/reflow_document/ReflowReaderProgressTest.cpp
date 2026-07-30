#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

#include "Reflow/ReflowCapabilityPolicy.h"
#include "Reflow/ReflowDocument.h"

class Print {};
class CssParser {};
class GfxRenderer {};

namespace {

class ReaderDocument final : public ReflowDocument {
 public:
  ReaderDocument(const ReflowDocumentFormat format, const ReflowCapabilitySet capabilities, const char* storeKey)
      : format_(format), capabilities_(capabilities), storeKey_(storeKey) {}

  ReflowDocumentFormat getFormat() const override { return format_; }
  const char* getStoreFormatKey() const override { return storeKey_; }
  ReflowCapabilitySet getCapabilities() const override { return capabilities_; }
  const std::string& getPath() const override { return path_; }
  const std::string& getCachePath() const override { return cachePath_; }
  const std::string& getTitle() const override { return title_; }
  const std::string& getAuthor() const override { return author_; }
  const std::string& getLanguage() const override { return language_; }
  std::string getCoverBmpPath(bool = false) const override { return {}; }
  bool generateCoverBmp(bool = false, const GfxRenderer* = nullptr, int = 0) const override { return false; }
  std::string getThumbBmpPath() const override { return {}; }
  std::string getThumbBmpPath(int, int) const override { return {}; }
  std::string getAdaptiveThumbBmpPath(int, int) const override { return {}; }
  bool generateThumbBmp(int, int, const GfxRenderer* = nullptr, int = 0) const override { return false; }
  bool generateAdaptiveThumbBmp(int, int, const GfxRenderer* = nullptr, int = 0) const override { return false; }
  int getSectionCount() const override { return 2; }
  ReflowSectionInfo getSectionInfo(const int sectionIndex) const override {
    if (sectionIndex == 0) {
      return {.href = "section-0.xhtml",
              .title = "Opening",
              .byteSize = 100,
              .cumulativeSize = 100,
              .firstWordOrdinal = 0,
              .wordCount = 40,
              .tocIndex = 0};
    }
    return {.href = "section-1.xhtml",
            .title = "Chapter",
            .byteSize = 300,
            .cumulativeSize = 400,
            .firstWordOrdinal = 40,
            .wordCount = 120,
            .tocIndex = 1};
  }
  bool getSectionSize(const int sectionIndex, size_t* size) const override {
    if (!size || sectionIndex < 0 || sectionIndex >= getSectionCount()) return false;
    *size = getSectionInfo(sectionIndex).byteSize;
    return true;
  }
  size_t getCumulativeSectionSize(const int sectionIndex) const override {
    return getSectionInfo(sectionIndex).cumulativeSize;
  }
  size_t getDocumentSize() const override { return 400; }
  int getSectionIndexForTextReference() const override { return 0; }
  int getTocEntryCount() const override { return 2; }
  ReflowTocEntry getTocEntry(const int tocIndex) const override {
    if (tocIndex == 0) {
      return {.title = "Opening",
              .href = "section-0.xhtml",
              .anchor = "",
              .level = 0,
              .sectionIndex = 0,
              .parentIndex = -1};
    }
    return {.title = "Chapter",
            .href = "section-1.xhtml#start",
            .anchor = "start",
            .level = 1,
            .sectionIndex = 1,
            .parentIndex = 0};
  }
  int getSectionIndexForTocIndex(const int tocIndex) const override { return getTocEntry(tocIndex).sectionIndex; }
  int getTocIndexForSectionIndex(const int sectionIndex) const override {
    return sectionIndex >= 0 && sectionIndex < getSectionCount() ? sectionIndex : -1;
  }
  int resolveHrefToSectionIndex(const std::string& href) const override {
    return href == "section-1.xhtml#start" ? 1 : -1;
  }
  float calculateSizeProgress(const int sectionIndex, const float sectionProgress) const override {
    const size_t before = sectionIndex > 0 ? getCumulativeSectionSize(sectionIndex - 1) : 0;
    return (static_cast<float>(before) + static_cast<float>(getSectionInfo(sectionIndex).byteSize) * sectionProgress) /
           static_cast<float>(getDocumentSize());
  }
  float calculateProgress(const int sectionIndex, const float sectionProgress) const override {
    const auto section = getSectionInfo(sectionIndex);
    return (static_cast<float>(section.firstWordOrdinal) + static_cast<float>(section.wordCount) * sectionProgress) /
           static_cast<float>(getTotalWordCount());
  }
  bool resolveProgressPercentToSection(const int percent, int& sectionIndex, float& sectionProgress) const override {
    if (percent < 0 || percent > 100) return false;
    const uint32_t word = static_cast<uint32_t>(percent) * getTotalWordCount() / 100U;
    sectionIndex = word < 40 ? 0 : 1;
    const auto section = getSectionInfo(sectionIndex);
    sectionProgress = static_cast<float>(word - section.firstWordOrdinal) / static_cast<float>(section.wordCount);
    return true;
  }
  bool hasStableReferencePages() const override { return false; }
  bool resolveReferencePage(int, float, uint32_t&, uint32_t&) const override { return false; }
  uint32_t getTotalWordCount() const override { return 160; }
  bool loadReadingPosition(ReflowReadingPosition& position) const override {
    if (!hasPosition_) return false;
    position = position_;
    return true;
  }
  bool saveReadingPosition(const ReflowReadingPosition& position) const override {
    position_ = position;
    hasPosition_ = true;
    return true;
  }
  bool getLocalSectionPath(int, ReflowResource&) const override { return false; }
  bool streamSection(const int sectionIndex, Print&, const size_t chunkSize) const override {
    lastStreamedSection_ = sectionIndex;
    return sectionIndex >= 0 && sectionIndex < getSectionCount() && chunkSize == 1024;
  }
  bool resolveResource(int, const std::string&, ReflowResource&) const override { return false; }
  bool streamResource(int, const std::string&, Print&, size_t) const override { return false; }
  bool getResourceSize(int, const std::string&, size_t*) const override { return false; }
  CssParser* getCssParser() const override { return nullptr; }

  int lastStreamedSection() const { return lastStreamedSection_; }

 private:
  ReflowDocumentFormat format_;
  ReflowCapabilitySet capabilities_;
  const char* storeKey_;
  std::string path_ = "/books/book";
  std::string cachePath_ = "/.crosspoint/cache";
  std::string title_ = "Book";
  std::string author_ = "Author";
  std::string language_ = "en";
  mutable ReflowReadingPosition position_;
  mutable bool hasPosition_ = false;
  mutable int lastStreamedSection_ = -1;
};

TEST(ReflowReaderProgress, UsesSharedSectionTocAndInternalHrefContracts) {
  ReaderDocument document(ReflowDocumentFormat::Pdf, 0, "pdf");
  Print output;

  ASSERT_TRUE(document.streamSection(1, output, 1024));
  EXPECT_EQ(document.lastStreamedSection(), 1);
  EXPECT_EQ(document.getTocEntry(1).sectionIndex, 1);
  EXPECT_EQ(document.getTocEntry(1).anchor, "start");
  EXPECT_EQ(document.resolveHrefToSectionIndex("section-1.xhtml#start"), 1);
  EXPECT_STREQ(document.getStoreFormatKey(), "pdf");
}

TEST(ReflowReaderProgress, PreservesSemanticPositionAndUsesIntegerRelayoutMapping) {
  ReaderDocument document(ReflowDocumentFormat::Pdf, 0, "pdf");
  ReflowReadingPosition saved;
  saved.sectionIndex = 1;
  saved.pageNumber = 3;
  saved.pageCount = 10;
  saved.hasPageCount = true;
  saved.hasSemanticPosition = true;
  saved.globalWordOrdinal = 77;
  saved.blockWordOffset = 4;
  std::memcpy(saved.blockAnchor, "block-8", sizeof("block-8"));

  ASSERT_TRUE(document.saveReadingPosition(saved));
  ReflowReadingPosition loaded;
  ASSERT_TRUE(document.loadReadingPosition(loaded));
  EXPECT_EQ(loaded.sectionIndex, 1);
  EXPECT_EQ(loaded.globalWordOrdinal, 77U);
  EXPECT_EQ(loaded.blockWordOffset, 4U);
  EXPECT_STREQ(loaded.blockAnchor, "block-8");
  EXPECT_EQ(reflowPageForRelayout(loaded.pageNumber, loaded.pageCount, 20), 6);
  EXPECT_EQ(reflowPageForRelayout(99, 10, 4), 3);
  EXPECT_EQ(document.calculateProgress(1, 0.5F), 0.625F);
}

TEST(ReflowReaderProgress, AppliesFormatCapabilitiesWithoutInspectingConcreteType) {
  const ReaderDocument epub(
      ReflowDocumentFormat::Epub,
      ReflowCapability::PublisherRenderModes | ReflowCapability::EmbeddedStyles | ReflowCapability::SavedItems, "epub");
  const ReaderDocument pdf(ReflowDocumentFormat::Pdf, 0, "pdf");

  EXPECT_TRUE(reflowUsesPublisherRenderModes(epub.getCapabilities()));
  EXPECT_TRUE(reflowUsesEmbeddedStyles(epub.getCapabilities()));
  EXPECT_TRUE(reflowSupportsSavedItems(epub.getCapabilities()));
  EXPECT_FALSE(reflowUsesPublisherRenderModes(pdf.getCapabilities()));
  EXPECT_FALSE(reflowUsesEmbeddedStyles(pdf.getCapabilities()));
  EXPECT_FALSE(reflowSupportsSavedItems(pdf.getCapabilities()));
}

}  // namespace
