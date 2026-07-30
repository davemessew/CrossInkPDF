#pragma once

#include <ReflowDocument.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

#include "PdfCacheStore.h"

class PdfReflowDocument : public ReflowDocument {
 public:
  PdfStatus initialize(const PdfCacheIo& io, const char* sourcePath, const char* cacheDirectory);
  PdfStatus loadCompletedCache();
  PdfStatus lastStatus() const { return status_; }

  ReflowDocumentFormat getFormat() const override { return ReflowDocumentFormat::Pdf; }
  const char* getStoreFormatKey() const override { return "pdf"; }
  ReflowCapabilitySet getCapabilities() const override { return 0; }

  const std::string& getPath() const override { return sourcePath_; }
  const std::string& getCachePath() const override { return cacheRoot_; }
  const std::string& getTitle() const override { return title_; }
  const std::string& getAuthor() const override { return author_; }
  const std::string& getLanguage() const override { return language_; }

  std::string getCoverBmpPath(bool cropped = false) const override;
  bool generateCoverBmp(bool cropped = false, const GfxRenderer* renderer = nullptr,
                        int readerFontId = 0) const override;
  std::string getThumbBmpPath() const override;
  std::string getThumbBmpPath(int width, int height) const override;
  std::string getAdaptiveThumbBmpPath(int width, int height) const override;
  bool generateThumbBmp(int width, int height, const GfxRenderer* renderer = nullptr,
                        int readerFontId = 0) const override;
  bool generateAdaptiveThumbBmp(int width, int height, const GfxRenderer* renderer = nullptr,
                                int readerFontId = 0) const override;

  int getSectionCount() const override;
  ReflowSectionInfo getSectionInfo(int sectionIndex) const override;
  bool getSectionSize(int sectionIndex, size_t* size) const override;
  size_t getCumulativeSectionSize(int sectionIndex) const override;
  size_t getDocumentSize() const override;
  int getSectionIndexForTextReference() const override;

  int getTocEntryCount() const override;
  ReflowTocEntry getTocEntry(int tocIndex) const override;
  int getSectionIndexForTocIndex(int tocIndex) const override;
  int getTocIndexForSectionIndex(int sectionIndex) const override;
  int resolveHrefToSectionIndex(const std::string& href) const override;

  float calculateSizeProgress(int sectionIndex, float sectionProgress) const override;
  float calculateProgress(int sectionIndex, float sectionProgress) const override;
  bool resolveProgressPercentToSection(int percent, int& sectionIndex, float& sectionProgress) const override;
  bool hasStableReferencePages() const override { return false; }
  bool resolveReferencePage(int sectionIndex, float sectionProgress, uint32_t& currentPage,
                            uint32_t& pageCount) const override;
  uint32_t getTotalWordCount() const override;
  bool loadReadingPosition(ReflowReadingPosition& position) const override;
  bool saveReadingPosition(const ReflowReadingPosition& position) const override;

  bool getLocalSectionPath(int sectionIndex, ReflowResource& out) const override;
  bool streamSection(int sectionIndex, Print& out, size_t chunkSize) const override;
  bool resolveResource(int sectionIndex, const std::string& href, ReflowResource& out) const override;
  bool streamResource(int sectionIndex, const std::string& href, Print& out, size_t chunkSize) const override;
  bool getResourceSize(int sectionIndex, const std::string& href, size_t* size) const override;
  CssParser* getCssParser() const override { return nullptr; }

 protected:
  const PdfCacheIo& cacheIo() const { return io_; }

 private:
  struct ManifestSource {
    const PdfCacheIo* io = nullptr;
    PdfCacheHandle handle{};
    uint64_t size = 0;

    static PdfStatus read(void* context, uint64_t offset, uint8_t* destination, size_t requested, size_t* bytesRead);
    PdfByteSource source() { return {this, size, read}; }
  };

  static PdfStatus validateRequiredFile(void* context, const PdfRequiredFileRecord& record);
  PdfStatus validateFile(const PdfRequiredFileRecord& record);
  bool streamCachedFile(const std::string& path, Print& out, size_t chunkSize) const;
  void resetLoadedState();
  void deriveFallbackTitle();

  PdfCacheIo io_{};
  std::string sourcePath_;
  std::string cacheRoot_;
  std::string title_;
  std::string author_;
  std::string language_;
  std::string sectionPath_;
  PdfSourceIdentity sourceIdentity_{};
  PdfCacheManifest manifest_{};
  PdfRequiredFileRecord sectionRecord_{};
  std::unique_ptr<uint8_t[]> ioWorkspace_;
  uint32_t requiredFilesSeen_ = 0;
  uint32_t xhtmlFilesSeen_ = 0;
  bool loaded_ = false;
  PdfStatus status_{};
};
