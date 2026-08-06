#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <string>

#include "Reflow/ReflowCapabilityPolicy.h"
#include "Reflow/ReflowDocument.h"

class Print {};
class CssParser {};
class GfxRenderer {};

namespace {

class CapabilityDocument final : public ReflowDocument {
 public:
  CapabilityDocument(const ReflowDocumentFormat format, const ReflowCapabilitySet capabilities)
      : format_(format), capabilities_(capabilities) {}

  ReflowDocumentFormat getFormat() const override { return format_; }
  const char* getStoreFormatKey() const override { return format_ == ReflowDocumentFormat::Epub ? "epub" : "pdf"; }
  ReflowCapabilitySet getCapabilities() const override { return capabilities_; }
  const std::string& getPath() const override { return empty_; }
  const std::string& getCachePath() const override { return empty_; }
  const std::string& getTitle() const override { return empty_; }
  const std::string& getAuthor() const override { return empty_; }
  const std::string& getLanguage() const override { return empty_; }
  std::string getCoverBmpPath(bool = false) const override { return {}; }
  bool generateCoverBmp(bool = false, const GfxRenderer* = nullptr, int = 0) const override { return false; }
  std::string getThumbBmpPath() const override { return {}; }
  std::string getThumbBmpPath(int, int) const override { return {}; }
  std::string getAdaptiveThumbBmpPath(int, int) const override { return {}; }
  bool generateThumbBmp(int, int, const GfxRenderer* = nullptr, int = 0) const override { return false; }
  bool generateAdaptiveThumbBmp(int, int, const GfxRenderer* = nullptr, int = 0) const override { return false; }
  int getSectionCount() const override { return 0; }
  ReflowSectionInfo getSectionInfo(int) const override { return {}; }
  bool getSectionSize(int, size_t*) const override { return false; }
  size_t getCumulativeSectionSize(int) const override { return 0; }
  size_t getDocumentSize() const override { return 0; }
  int getSectionIndexForTextReference() const override { return -1; }
  int getTocEntryCount() const override { return 0; }
  ReflowTocEntry getTocEntry(int) const override { return {}; }
  int getSectionIndexForTocIndex(int) const override { return -1; }
  int getTocIndexForSectionIndex(int) const override { return -1; }
  int resolveHrefToSectionIndex(const std::string&) const override { return -1; }
  float calculateSizeProgress(int, float) const override { return 0.0F; }
  float calculateProgress(int, float) const override { return 0.0F; }
  bool resolveProgressPercentToSection(int, int&, float&) const override { return false; }
  bool hasStableReferencePages() const override { return false; }
  bool resolveReferencePage(int, float, uint32_t&, uint32_t&) const override { return false; }
  uint32_t getTotalWordCount() const override { return 0; }
  bool loadReadingPosition(ReflowReadingPosition&) const override { return false; }
  bool saveReadingPosition(const ReflowReadingPosition&) const override { return false; }
  bool getLocalSectionPath(int, ReflowResource&) const override { return false; }
  bool streamSection(int, Print&, size_t) const override { return false; }
  bool resolveResource(int, const std::string&, ReflowResource&) const override { return false; }
  bool streamResource(int, const std::string&, Print&, size_t) const override { return false; }
  bool getResourceSize(int, const std::string&, size_t*) const override { return false; }
  CssParser* getCssParser() const override { return nullptr; }

 private:
  ReflowDocumentFormat format_;
  ReflowCapabilitySet capabilities_;
  std::string empty_;
};

void expectSyncActionVisibility(const ReflowDocument& document, const ReflowReaderSyncAction action,
                                const bool expected) {
  EXPECT_EQ(expected, reflowSupportsMenuAction(document.getCapabilities(), action));
  EXPECT_EQ(expected, reflowSupportsQuickAction(document.getCapabilities(), action));
}

TEST(ReflowCapabilityPolicy, EpubCapabilitiesExposeBothSyncMenuAndQuickActions) {
  const CapabilityDocument document(ReflowDocumentFormat::Epub,
                                    ReflowCapability::ExternalProgressSync | ReflowCapability::NearbyProgressSync |
                                        ReflowCapability::PublisherRenderModes | ReflowCapability::EmbeddedStyles |
                                        ReflowCapability::SavedItems);

  expectSyncActionVisibility(document, ReflowReaderSyncAction::ExternalProgress, true);
  expectSyncActionVisibility(document, ReflowReaderSyncAction::NearbyProgress, true);
}

TEST(ReflowCapabilityPolicy, PdfCapabilitiesHideAllSyncMenuAndQuickActions) {
  const CapabilityDocument document(ReflowDocumentFormat::Pdf, 0);

  expectSyncActionVisibility(document, ReflowReaderSyncAction::ExternalProgress, false);
  expectSyncActionVisibility(document, ReflowReaderSyncAction::NearbyProgress, false);
}

}  // namespace
