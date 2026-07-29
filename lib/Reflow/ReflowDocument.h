#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>

class CssParser;
class GfxRenderer;
class Print;

enum class ReflowDocumentFormat : uint8_t {
  Epub,
  Pdf,
};

enum class ReflowCapability : uint16_t {
  ExternalProgressSync = 1u << 0,
  NearbyProgressSync = 1u << 1,
  PublisherRenderModes = 1u << 2,
  EmbeddedStyles = 1u << 3,
  SavedItems = 1u << 4,
};

using ReflowCapabilitySet = uint16_t;

constexpr ReflowCapabilitySet reflowCapabilityMask(const ReflowCapability capability) {
  return static_cast<ReflowCapabilitySet>(capability);
}

constexpr ReflowCapabilitySet operator|(const ReflowCapability left, const ReflowCapability right) {
  return reflowCapabilityMask(left) | reflowCapabilityMask(right);
}

constexpr ReflowCapabilitySet operator|(const ReflowCapabilitySet left, const ReflowCapability right) {
  return left | reflowCapabilityMask(right);
}

constexpr bool hasReflowCapability(const ReflowCapabilitySet capabilities, const ReflowCapability capability) {
  return (capabilities & reflowCapabilityMask(capability)) != 0;
}

enum class ReflowResourceKind : uint8_t {
  Streamed,
  BorrowedLocalFile,
};

enum class ReflowImageKind : uint8_t {
  EncodedImage,
  PixelCache,
};

struct ReflowResource {
  ReflowResourceKind kind = ReflowResourceKind::Streamed;
  ReflowImageKind imageKind = ReflowImageKind::EncodedImage;
  std::string localPath;
  uint16_t width = 0;
  uint16_t height = 0;
  bool paginatorMayDelete = false;

  static ReflowResource borrowedLocalFile(std::string path, const ReflowImageKind imageKind, const uint16_t width = 0,
                                          const uint16_t height = 0) {
    ReflowResource resource;
    resource.kind = ReflowResourceKind::BorrowedLocalFile;
    resource.imageKind = imageKind;
    resource.localPath = std::move(path);
    resource.width = width;
    resource.height = height;
    resource.paginatorMayDelete = false;
    return resource;
  }

  static ReflowResource streamed(const ReflowImageKind imageKind = ReflowImageKind::EncodedImage) {
    ReflowResource resource;
    resource.imageKind = imageKind;
    return resource;
  }
};

struct ReflowSectionInfo {
  std::string href;
  std::string title;
  uint32_t byteSize = 0;
  uint32_t cumulativeSize = 0;
  uint32_t firstWordOrdinal = 0;
  uint32_t wordCount = 0;
  int tocIndex = -1;
};

struct ReflowTocEntry {
  std::string title;
  std::string href;
  std::string anchor;
  uint8_t level = 0;
  int sectionIndex = -1;
  int parentIndex = -1;
};

struct ReflowReadingPosition {
  int sectionIndex = 0;
  int pageNumber = 0;
  int pageCount = 0;
  bool hasPageCount = false;
  bool hasSemanticPosition = false;
  uint32_t globalWordOrdinal = 0;
  uint32_t blockWordOffset = 0;
  std::string blockAnchor;
};

class ReflowSectionSource {
 public:
  virtual ~ReflowSectionSource() = default;

  virtual bool getLocalSectionPath(int sectionIndex, ReflowResource& out) const = 0;
  virtual bool streamSection(int sectionIndex, Print& out, size_t chunkSize) const = 0;
  virtual bool resolveResource(int sectionIndex, const std::string& href, ReflowResource& out) const = 0;
  virtual bool streamResource(int sectionIndex, const std::string& href, Print& out, size_t chunkSize) const = 0;
  virtual bool getResourceSize(int sectionIndex, const std::string& href, size_t* size) const = 0;
  virtual CssParser* getCssParser() const = 0;

  bool getImmutableLocalSection(const int sectionIndex, ReflowResource& out) const {
    ReflowResource candidate;
    if (!getLocalSectionPath(sectionIndex, candidate) || candidate.kind != ReflowResourceKind::BorrowedLocalFile ||
        candidate.localPath.empty() || candidate.paginatorMayDelete) {
      out = ReflowResource::streamed();
      return false;
    }
    out = std::move(candidate);
    return true;
  }

  bool getImmutableLocalResource(const int sectionIndex, const std::string& href, ReflowResource& out) const {
    ReflowResource candidate;
    if (!resolveResource(sectionIndex, href, candidate) || candidate.kind != ReflowResourceKind::BorrowedLocalFile ||
        candidate.localPath.empty() || candidate.paginatorMayDelete ||
        (candidate.imageKind == ReflowImageKind::PixelCache && (candidate.width == 0 || candidate.height == 0))) {
      out = ReflowResource::streamed();
      return false;
    }
    out = std::move(candidate);
    return true;
  }
};

class ReflowDocument : public ReflowSectionSource {
 public:
  ~ReflowDocument() override = default;

  virtual ReflowDocumentFormat getFormat() const = 0;
  virtual const char* getStoreFormatKey() const = 0;
  virtual ReflowCapabilitySet getCapabilities() const = 0;

  bool hasCapability(const ReflowCapability capability) const {
    return hasReflowCapability(getCapabilities(), capability);
  }

  virtual const std::string& getPath() const = 0;
  virtual const std::string& getCachePath() const = 0;
  virtual const std::string& getTitle() const = 0;
  virtual const std::string& getAuthor() const = 0;
  virtual const std::string& getLanguage() const = 0;

  virtual std::string getCoverBmpPath(bool cropped = false) const = 0;
  virtual bool generateCoverBmp(bool cropped = false, const GfxRenderer* renderer = nullptr,
                                int readerFontId = 0) const = 0;
  virtual std::string getThumbBmpPath() const = 0;
  virtual std::string getThumbBmpPath(int width, int height) const = 0;
  virtual std::string getAdaptiveThumbBmpPath(int width, int height) const = 0;
  virtual bool generateThumbBmp(int width, int height, const GfxRenderer* renderer = nullptr,
                                int readerFontId = 0) const = 0;
  virtual bool generateAdaptiveThumbBmp(int width, int height, const GfxRenderer* renderer = nullptr,
                                        int readerFontId = 0) const = 0;

  virtual int getSectionCount() const = 0;
  virtual ReflowSectionInfo getSectionInfo(int sectionIndex) const = 0;
  virtual bool getSectionSize(int sectionIndex, size_t* size) const = 0;
  virtual size_t getCumulativeSectionSize(int sectionIndex) const = 0;
  virtual size_t getDocumentSize() const = 0;
  virtual int getSectionIndexForTextReference() const = 0;

  virtual int getTocEntryCount() const = 0;
  virtual ReflowTocEntry getTocEntry(int tocIndex) const = 0;
  virtual int getSectionIndexForTocIndex(int tocIndex) const = 0;
  virtual int getTocIndexForSectionIndex(int sectionIndex) const = 0;
  virtual int resolveHrefToSectionIndex(const std::string& href) const = 0;

  virtual float calculateSizeProgress(int sectionIndex, float sectionProgress) const = 0;
  virtual float calculateProgress(int sectionIndex, float sectionProgress) const = 0;
  virtual bool resolveProgressPercentToSection(int percent, int& sectionIndex, float& sectionProgress) const = 0;
  virtual bool hasStableReferencePages() const = 0;
  virtual bool resolveReferencePage(int sectionIndex, float sectionProgress, uint32_t& currentPage,
                                    uint32_t& pageCount) const = 0;
  virtual uint32_t getTotalWordCount() const = 0;
  virtual bool loadReadingPosition(ReflowReadingPosition& position) const = 0;
  virtual bool saveReadingPosition(const ReflowReadingPosition& position) const = 0;
};
