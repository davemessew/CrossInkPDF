#include <Print.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <string>
#include <vector>

#include "PdfCacheStore.h"
#include "PdfLayoutWordIndex.h"
#include "PdfMetadataStore.h"
#include "PdfOutline.h"
#include "PdfReflowDocument.h"
#include "PdfTestCacheIo.h"
#include "PdfTestIo.h"

namespace {

constexpr char kSourcePath[] = "/books/minimal.pdf";
constexpr char kCacheDirectory[] = "/.crosspoint";
constexpr char kSection[] =
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
    "<html xmlns=\"http://www.w3.org/1999/xhtml\"><head><meta charset=\"UTF-8\"/></head>"
    "<body><p id=\"b00000000\">Hello PDF</p></body></html>";

void setAnchor(ReflowReadingPosition& position, const char* anchor) {
  ASSERT_LT(std::strlen(anchor), sizeof(position.blockAnchor));
  std::memcpy(position.blockAnchor, anchor, std::strlen(anchor));
}

struct RequiredRecords {
  std::vector<PdfRequiredFileRecord> values;

  static PdfStatus read(void* context, const uint32_t index, PdfRequiredFileRecord* output) {
    if (context == nullptr || output == nullptr) {
      return PdfStatus::failure(PdfError::InvalidArgument);
    }
    const auto& self = *static_cast<RequiredRecords*>(context);
    if (index >= self.values.size()) {
      return PdfStatus::failure(PdfError::InvalidOffset, index);
    }
    *output = self.values[index];
    return PdfStatus::success();
  }

  PdfRequiredFileTableSource source() { return {this, static_cast<uint32_t>(values.size()), read}; }
};

struct OneMetadataSection {
  PdfMetadataSection value{};

  static PdfStatus read(void* context, const uint16_t index, PdfMetadataSection* output) {
    if (context == nullptr || output == nullptr || index != 0) {
      return PdfStatus::failure(PdfError::InvalidArgument);
    }
    *output = static_cast<OneMetadataSection*>(context)->value;
    return PdfStatus::success();
  }
};

struct OneOutlineEntry {
  PdfOutlineEntry value{};

  static PdfStatus read(void* context, const uint16_t index, PdfOutlineEntry* output) {
    if (context == nullptr || output == nullptr || index != 0) {
      return PdfStatus::failure(PdfError::InvalidArgument);
    }
    *output = static_cast<OneOutlineEntry*>(context)->value;
    return PdfStatus::success();
  }
};

struct CacheFixture {
  PdfTestCacheIo storage;
  PdfSourceIdentity identity{};
  std::string cacheRoot;
  std::string sectionPath;

  void build() {
    const std::string source = "%PDF-host-fixture-with-enough-identity-bytes";
    storage.addFile(kSourcePath, std::vector<uint8_t>(source.begin(), source.end()), 42, true);
    std::array<uint8_t, PDF_SOURCE_FINGERPRINT_BYTES> workspace{};
    ASSERT_TRUE(
        pdfComputeSourceIdentity(storage.io(), kSourcePath, workspace.data(), workspace.size(), &identity).ok());

    std::array<char, PDF_CACHE_PATH_CAPACITY> root{};
    ASSERT_TRUE(pdfFormatCacheRoot(kCacheDirectory, kSourcePath, root.data(), root.size()).ok());
    cacheRoot = root.data();
    sectionPath = cacheRoot + "/gen_7/sections/000000.xhtml";

    PdfCacheStore cache;
    ASSERT_TRUE(cache.initialize(storage.io(), cacheRoot.c_str()).ok());
    ASSERT_TRUE(cache.ensureGeneration(7).ok());
    PdfCacheTrackedWriter writer;
    ASSERT_TRUE(pdfOpenTrackedCacheWriter(storage.io(), sectionPath.c_str(), "gen_7/sections/000000.xhtml",
                                          PdfCacheFileKind::Required, sizeof(kSection), &writer)
                    .ok());
    ASSERT_TRUE(
        pdfWriteTrackedCacheFile(&writer, reinterpret_cast<const uint8_t*>(kSection), sizeof(kSection) - 1).ok());
    RequiredRecords records;
    PdfRequiredFileRecord sectionRecord{};
    ASSERT_TRUE(pdfCloseTrackedCacheFile(&writer, &sectionRecord).ok());
    records.values.push_back(sectionRecord);

    PdfMetadataBuilder metadataBuilder;
    ASSERT_TRUE(metadataBuilder.begin(reinterpret_cast<const uint8_t*>("minimal"), 7).ok());
    PdfMetadata metadata = metadataBuilder.metadata();
    metadata.sectionCount = 1;
    metadata.outlineCount = 1;
    metadata.totalWords = 2;
    OneMetadataSection metadataSection{{
        .byteSize = static_cast<uint32_t>(sectionRecord.size),
        .cumulativeSize = static_cast<uint32_t>(sectionRecord.size),
        .firstWordOrdinal = 0,
        .wordCount = 2,
        .firstAnchorOrdinal = 0,
        .tocIndex = 0,
    }};
    PdfTestByteSink metadataBytes;
    ASSERT_TRUE(
        pdfEncodeMetadata(metadata, {&metadataSection, 1, OneMetadataSection::read}, metadataBytes.sink()).ok());
    const std::string metadataRelative = "gen_7/metadata.bin";
    const std::string metadataPath = cacheRoot + "/" + metadataRelative;
    storage.addFile(metadataPath, metadataBytes.bytes());
    PdfRequiredFileRecord metadataRecord{};
    std::memcpy(metadataRecord.path, metadataRelative.data(), metadataRelative.size());
    metadataRecord.pathLength = static_cast<uint8_t>(metadataRelative.size());
    metadataRecord.size = metadataBytes.bytes().size();
    metadataRecord.crc32 = pdfCacheCrc32(metadataBytes.bytes().data(), metadataBytes.bytes().size());
    records.values.push_back(metadataRecord);

    std::array<PdfOutlineEntry, 1> outlineWorkspace{};
    PdfOutlineBuilder outlineBuilder({outlineWorkspace.data(), 1});
    ASSERT_TRUE(outlineBuilder.begin().ok());
    ASSERT_TRUE(outlineBuilder.finish(reinterpret_cast<const uint8_t*>("minimal"), 7).ok());
    OneOutlineEntry outlineEntry{outlineWorkspace[0]};
    PdfTestByteSink outlineBytes;
    ASSERT_TRUE(pdfEncodeOutline({&outlineEntry, 1, OneOutlineEntry::read}, outlineBytes.sink()).ok());
    const std::string outlineRelative = "gen_7/outline.bin";
    const std::string outlinePath = cacheRoot + "/" + outlineRelative;
    storage.addFile(outlinePath, outlineBytes.bytes());
    PdfRequiredFileRecord outlineRecord{};
    std::memcpy(outlineRecord.path, outlineRelative.data(), outlineRelative.size());
    outlineRecord.pathLength = static_cast<uint8_t>(outlineRelative.size());
    outlineRecord.size = outlineBytes.bytes().size();
    outlineRecord.crc32 = pdfCacheCrc32(outlineBytes.bytes().data(), outlineBytes.bytes().size());
    records.values.push_back(outlineRecord);

    PdfCacheManifest manifest;
    manifest.sequence = 3;
    manifest.completed = true;
    manifest.source = identity;
    manifest.generation = 7;
    manifest.totalWords = 2;
    manifest.requiredFileCount = static_cast<uint32_t>(records.values.size());
    manifest.requiredFileLedger = PDF_CACHE_FNV64_OFFSET;
    for (const auto& record : records.values) {
      manifest.requiredFileBytes += record.size;
      manifest.requiredFileLedger = pdfUpdateRequiredFileLedger(manifest.requiredFileLedger, record);
    }
    const PdfCacheCommitEvidence evidence{
        true,
        manifest.requiredFileCount,
        manifest.requiredFileBytes,
        manifest.requiredFileLedger,
    };
    PdfCacheManifestSelection committed;
    const PdfCacheManifestSelection prior{};
    ASSERT_TRUE(cache.commitManifest(manifest, records.source(), evidence, prior, &committed).ok());
    ASSERT_TRUE(committed.selected);
    storage.clearEvents();
  }
};

class BufferPrint final : public Print {
 public:
  size_t write(const uint8_t* bytes, const size_t length) override {
    output.insert(output.end(), bytes, bytes + length);
    return length;
  }

  std::vector<uint8_t> output;
};

PdfLayoutWordRange layoutRange(const uint32_t first, const uint32_t last, const uint32_t blockOffset,
                               const char* const anchor) {
  PdfLayoutWordRange result;
  result.firstGlobalWordOrdinal = first;
  result.lastGlobalWordOrdinal = last;
  result.firstBlockWordOffset = blockOffset;
  result.valid = true;
  const size_t anchorLength = std::strlen(anchor);
  EXPECT_LT(anchorLength, sizeof(result.blockAnchor));
  std::memcpy(result.blockAnchor, anchor, anchorLength);
  return result;
}

std::vector<uint8_t> layoutWordIndex(const std::vector<PdfLayoutWordRange>& ranges, const uint32_t totalWords) {
  PdfTestByteSink sink;
  PdfLayoutWordIndexWriter writer;
  EXPECT_TRUE(writer.begin(sink.sink(), 0, 0, totalWords));
  for (const auto& range : ranges) {
    EXPECT_TRUE(writer.append(range));
  }
  EXPECT_TRUE(writer.finish());
  return sink.bytes();
}

TEST(PdfReflowDocument, ClosesSourceBeforeValidatingCacheAndNeverReopensItForReading) {
  CacheFixture fixture;
  fixture.build();
  const uint32_t sourceOpensBefore = fixture.storage.openCallsForPath(kSourcePath);

  PdfReflowDocument document;
  ASSERT_TRUE(document.initialize(fixture.storage.io(), kSourcePath, kCacheDirectory).ok());
  ASSERT_TRUE(document.loadCompletedCache().ok());

  const auto& events = fixture.storage.events();
  const auto sourceOpen = std::find(events.begin(), events.end(), std::string("open:") + kSourcePath);
  const auto sourceClose = std::find(events.begin(), events.end(), std::string("close:") + kSourcePath);
  ASSERT_NE(sourceOpen, events.end());
  ASSERT_NE(sourceClose, events.end());
  EXPECT_LT(sourceOpen, sourceClose);
  const auto firstCacheOpen = std::find_if(sourceClose + 1, events.end(), [](const std::string& event) {
    return event.rfind("open:/.crosspoint/", 0) == 0;
  });
  ASSERT_NE(firstCacheOpen, events.end());
  EXPECT_LT(sourceClose, firstCacheOpen);

  EXPECT_EQ(document.getFormat(), ReflowDocumentFormat::Pdf);
  EXPECT_STREQ(document.getStoreFormatKey(), "pdf");
  EXPECT_TRUE(hasReflowCapability(document.getCapabilities(), ReflowCapability::SavedItems));
  EXPECT_EQ(document.getSectionCount(), 1);
  EXPECT_EQ(document.getTotalWordCount(), 2U);
  EXPECT_EQ(document.getSectionInfo(0).wordCount, 2U);

  ReflowResource local;
  ASSERT_TRUE(document.getImmutableLocalSection(0, local));
  EXPECT_EQ(local.localPath, fixture.sectionPath);

  BufferPrint output;
  ASSERT_TRUE(document.streamSection(0, output, 17));
  EXPECT_EQ(std::string(output.output.begin(), output.output.end()), kSection);

  ReflowReadingPosition position;
  EXPECT_FALSE(document.loadReadingPosition(position));
  position.sectionIndex = 0;
  position.pageNumber = 2;
  position.pageCount = 4;
  position.hasPageCount = true;
  position.hasSemanticPosition = true;
  position.globalWordOrdinal = 1;
  position.blockWordOffset = 1;
  setAnchor(position, "b00000000");
  ASSERT_TRUE(document.saveReadingPosition(position));

  ReflowReadingPosition loadedPosition;
  ASSERT_TRUE(document.loadReadingPosition(loadedPosition));
  EXPECT_EQ(loadedPosition.sectionIndex, 0);
  EXPECT_EQ(loadedPosition.pageNumber, 2);
  EXPECT_EQ(loadedPosition.pageCount, 4);
  EXPECT_TRUE(loadedPosition.hasPageCount);
  EXPECT_TRUE(loadedPosition.hasSemanticPosition);
  EXPECT_EQ(loadedPosition.globalWordOrdinal, 1U);
  EXPECT_EQ(loadedPosition.blockWordOffset, 1U);
  EXPECT_STREQ(loadedPosition.blockAnchor, "b00000000");
  EXPECT_EQ(fixture.storage.openCallsForPath(kSourcePath), sourceOpensBefore + 1);
  EXPECT_EQ(fixture.storage.openHandleCount(), 0U);
}

TEST(PdfReflowDocument, MasksSavedItemsUntilMatchingCompletedCacheInitializesPersistence) {
  CacheFixture fixture;
  fixture.build();

  PdfReflowDocument document;
  ASSERT_TRUE(document.initialize(fixture.storage.io(), kSourcePath, kCacheDirectory).ok());
  EXPECT_FALSE(hasReflowCapability(document.getCapabilities(), ReflowCapability::SavedItems));

  ASSERT_TRUE(document.loadCompletedCache().ok());
  EXPECT_TRUE(hasReflowCapability(document.getCapabilities(), ReflowCapability::SavedItems));

  fixture.storage.corruptByte(fixture.sectionPath, 12, 0x40);
  EXPECT_FALSE(document.loadCompletedCache().ok());
  EXPECT_FALSE(hasReflowCapability(document.getCapabilities(), ReflowCapability::SavedItems));
}

TEST(PdfReflowDocument, ExposesNarrowSavedItemPersistenceOnlyWhileReady) {
  CacheFixture fixture;
  fixture.build();

  PdfReflowDocument document;
  ASSERT_TRUE(document.initialize(fixture.storage.io(), kSourcePath, kCacheDirectory).ok());
  std::array<PdfSavedItem, PDF_SAVED_ITEMS_MAX_RECORDS> items{};
  PdfSavedItemsBuffer buffer{items.data(), static_cast<uint16_t>(items.size()), 0};
  EXPECT_EQ(document.loadPdfSavedItems(&buffer).error, PdfError::InvalidArgument);

  ASSERT_TRUE(document.loadCompletedCache().ok());
  EXPECT_EQ(document.loadPdfSavedItems(&buffer).error, PdfError::InvalidOffset);
  EXPECT_EQ(buffer.count, 0);

  PdfSavedItem bookmark{};
  bookmark.itemId = 7;
  bookmark.kind = PdfSavedItemKind::Bookmark;
  bookmark.flags = PDF_SAVED_ITEM_HAS_START_SEMANTIC;
  bookmark.startGlobalWordOrdinal = 1;
  bookmark.startBlockWordOffset = 1;
  bookmark.sectionIndex = 0;
  std::memcpy(bookmark.startBlockAnchor, "b00000000", 9);
  ASSERT_TRUE(document.validatePdfSavedItem(bookmark).ok());
  ASSERT_TRUE(document.savePdfSavedItems(&bookmark, 1).ok());

  ASSERT_TRUE(document.loadPdfSavedItems(&buffer).ok());
  ASSERT_EQ(buffer.count, 1);
  EXPECT_EQ(buffer.items[0].itemId, 7);
  EXPECT_EQ(buffer.items[0].startGlobalWordOrdinal, 1U);
}

TEST(PdfReflowDocument, ReloadsSemanticProgressAfterDocumentRestart) {
  CacheFixture fixture;
  fixture.build();

  ReflowReadingPosition saved;
  saved.sectionIndex = 0;
  saved.pageNumber = 1;
  saved.pageCount = 3;
  saved.hasPageCount = true;
  saved.hasSemanticPosition = true;
  saved.globalWordOrdinal = 1;
  saved.blockWordOffset = 1;
  setAnchor(saved, "b00000000");

  {
    PdfReflowDocument first;
    ASSERT_TRUE(first.initialize(fixture.storage.io(), kSourcePath, kCacheDirectory).ok());
    ASSERT_TRUE(first.loadCompletedCache().ok());
    ASSERT_TRUE(first.saveReadingPosition(saved));
  }

  PdfReflowDocument restarted;
  ASSERT_TRUE(restarted.initialize(fixture.storage.io(), kSourcePath, kCacheDirectory).ok());
  ASSERT_TRUE(restarted.loadCompletedCache().ok());
  ReflowReadingPosition loaded;
  ASSERT_TRUE(restarted.loadReadingPosition(loaded));
  EXPECT_TRUE(loaded.hasSemanticPosition);
  EXPECT_EQ(loaded.globalWordOrdinal, 1U);
  EXPECT_EQ(loaded.blockWordOffset, 1U);
  EXPECT_STREQ(loaded.blockAnchor, "b00000000");
  EXPECT_EQ(fixture.storage.openHandleCount(), 0U);
}

TEST(PdfReflowDocument, RejectsCorruptRequiredFileBeforeExposingSections) {
  CacheFixture fixture;
  fixture.build();
  fixture.storage.corruptByte(fixture.sectionPath, 12, 0x40);

  PdfReflowDocument document;
  ASSERT_TRUE(document.initialize(fixture.storage.io(), kSourcePath, kCacheDirectory).ok());
  const PdfStatus status = document.loadCompletedCache();

  EXPECT_EQ(status.error, PdfError::Malformed);
  EXPECT_EQ(document.getSectionCount(), 0);
  EXPECT_EQ(fixture.storage.openHandleCount(), 0U);
}

TEST(PdfReflowDocument, FallsBackToGlobalOrdinalWhenEarliestAnchorContinuationDoesNotContainIt) {
  CacheFixture fixture;
  fixture.build();
  const std::string sectionCachePath = fixture.cacheRoot + "/sections/0.bin";
  fixture.storage.addFile(sectionCachePath + ".pwi", layoutWordIndex(
                                                         {
                                                             layoutRange(0, 0, 0, "b00000000"),
                                                             layoutRange(1, 1, 0, "b00000000"),
                                                         },
                                                         2));

  PdfReflowDocument document;
  ASSERT_TRUE(document.initialize(fixture.storage.io(), kSourcePath, kCacheDirectory).ok());
  ASSERT_TRUE(document.loadCompletedCache().ok());
  ASSERT_TRUE(document.validateLayoutWordIndex(sectionCachePath, 0, 2));

  uint16_t page = UINT16_MAX;
  ASSERT_TRUE(document.findLayoutWordPage(sectionCachePath, "b00000000", 0, 1, page));
  EXPECT_EQ(page, 1U);
}

TEST(PdfReflowDocument, RetriesValidatedSidecarAfterTransientOpenFailure) {
  CacheFixture fixture;
  fixture.build();
  const std::string sectionCachePath = fixture.cacheRoot + "/sections/0.bin";
  fixture.storage.addFile(sectionCachePath + ".pwi", layoutWordIndex(
                                                         {
                                                             layoutRange(0, 0, 0, "b00000000"),
                                                             layoutRange(1, 1, 1, "b00000000"),
                                                         },
                                                         2));

  PdfReflowDocument document;
  ASSERT_TRUE(document.initialize(fixture.storage.io(), kSourcePath, kCacheDirectory).ok());
  ASSERT_TRUE(document.loadCompletedCache().ok());
  ASSERT_TRUE(document.validateLayoutWordIndex(sectionCachePath, 0, 2));

  ReflowPageSemanticRange range;
  fixture.storage.fail(PdfTestFaultPoint::Open);
  EXPECT_FALSE(document.readLayoutWordRange(sectionCachePath, 2, 1, range));
  fixture.storage.clearFault();
  ASSERT_TRUE(document.readLayoutWordRange(sectionCachePath, 2, 1, range));
  EXPECT_TRUE(range.valid);
  EXPECT_EQ(range.firstGlobalWordOrdinal, 1U);
}

TEST(PdfReflowDocument, InvalidatesCachedWindowBeforeAPartialRefillCanOverwriteIt) {
  CacheFixture fixture;
  fixture.build();
  const std::string sectionCachePath = fixture.cacheRoot + "/sections/0.bin";
  const std::string sidecarPath = sectionCachePath + ".pwi";
  std::vector<PdfLayoutWordRange> ranges = {
      layoutRange(0, 0, 0, "b00000000"),
      PdfLayoutWordRange{},
      PdfLayoutWordRange{},
      PdfLayoutWordRange{},
      PdfLayoutWordRange{},
      PdfLayoutWordRange{},
      PdfLayoutWordRange{},
      layoutRange(1, 1, 0, "b00000001"),
  };
  fixture.storage.addFile(sidecarPath, layoutWordIndex(ranges, 2));

  PdfReflowDocument document;
  ASSERT_TRUE(document.initialize(fixture.storage.io(), kSourcePath, kCacheDirectory).ok());
  ASSERT_TRUE(document.loadCompletedCache().ok());
  ASSERT_TRUE(document.validateLayoutWordIndex(sectionCachePath, 0, 8));

  ReflowPageSemanticRange range;
  ASSERT_TRUE(document.readLayoutWordRange(sectionCachePath, 8, 0, range));
  ASSERT_TRUE(range.valid);
  EXPECT_EQ(range.firstGlobalWordOrdinal, 0U);

  // Page 4 decodes into scratch before page 5's local CRC fails.
  constexpr size_t corruptPage = 5;
  fixture.storage.corruptByte(
      sidecarPath,
      PDF_LAYOUT_WORD_INDEX_HEADER_BYTES + corruptPage * PDF_LAYOUT_WORD_INDEX_RECORD_BYTES,
      0x01);
  EXPECT_FALSE(document.readLayoutWordRange(sectionCachePath, 8, 4, range));

  // If the old page-0 metadata survived, this would return overwritten page-4
  // scratch without touching storage. A real refill must observe this fault.
  fixture.storage.fail(PdfTestFaultPoint::Read);
  EXPECT_FALSE(document.readLayoutWordRange(sectionCachePath, 8, 0, range));
  fixture.storage.clearFault();
  EXPECT_FALSE(range.valid);
}

TEST(PdfReflowDocument, PublishesARefilledWindowOnlyAfterCloseSucceeds) {
  CacheFixture fixture;
  fixture.build();
  const std::string sectionCachePath = fixture.cacheRoot + "/sections/0.bin";
  const std::string sidecarPath = sectionCachePath + ".pwi";
  fixture.storage.addFile(sidecarPath,
                          layoutWordIndex(
                              {
                                  layoutRange(0, 0, 0, "b00000000"),
                                  PdfLayoutWordRange{},
                                  PdfLayoutWordRange{},
                                  PdfLayoutWordRange{},
                                  PdfLayoutWordRange{},
                                  PdfLayoutWordRange{},
                                  PdfLayoutWordRange{},
                                  layoutRange(1, 1, 0, "b00000001"),
                              },
                              2));

  PdfReflowDocument document;
  ASSERT_TRUE(document.initialize(fixture.storage.io(), kSourcePath, kCacheDirectory).ok());
  ASSERT_TRUE(document.loadCompletedCache().ok());
  ASSERT_TRUE(document.validateLayoutWordIndex(sectionCachePath, 0, 8));

  ReflowPageSemanticRange range;
  ASSERT_TRUE(document.readLayoutWordRange(sectionCachePath, 8, 0, range));
  fixture.storage.fail(PdfTestFaultPoint::Close);
  EXPECT_FALSE(document.readLayoutWordRange(sectionCachePath, 8, 4, range));
  fixture.storage.clearFault();

  fixture.storage.fail(PdfTestFaultPoint::Read);
  EXPECT_FALSE(document.readLayoutWordRange(sectionCachePath, 8, 0, range));
  fixture.storage.clearFault();
  EXPECT_EQ(fixture.storage.openHandleCount(), 0U);
}

TEST(PdfReflowDocument, RemovesSidecarDirectlyAndKeepsItUsableWhenRemoveFails) {
  CacheFixture fixture;
  fixture.build();
  const std::string sectionCachePath = fixture.cacheRoot + "/sections/0.bin";
  const std::string sidecarPath = sectionCachePath + ".pwi";
  fixture.storage.addFile(sidecarPath, layoutWordIndex({layoutRange(0, 1, 0, "b00000000")}, 2));

  PdfReflowDocument document;
  ASSERT_TRUE(document.initialize(fixture.storage.io(), kSourcePath, kCacheDirectory).ok());
  ASSERT_TRUE(document.loadCompletedCache().ok());
  ASSERT_TRUE(document.validateLayoutWordIndex(sectionCachePath, 0, 1));

  const uint32_t opensBeforeRemove = fixture.storage.openCallsForPath(sidecarPath);
  fixture.storage.fail(PdfTestFaultPoint::Remove);
  EXPECT_FALSE(document.removeLayoutWordIndex(sectionCachePath));
  EXPECT_TRUE(fixture.storage.exists(sidecarPath));
  EXPECT_EQ(fixture.storage.openCallsForPath(sidecarPath), opensBeforeRemove);
  fixture.storage.clearFault();

  ReflowPageSemanticRange range;
  ASSERT_TRUE(document.readLayoutWordRange(sectionCachePath, 1, 0, range));
  EXPECT_TRUE(range.valid);
  EXPECT_TRUE(document.removeLayoutWordIndex(sectionCachePath));
  EXPECT_FALSE(fixture.storage.exists(sidecarPath));
  EXPECT_TRUE(document.removeLayoutWordIndex(sectionCachePath));
}

TEST(PdfReflowDocument, MapsSavedItemsThroughTheCurrentSectionSidecar) {
  CacheFixture fixture;
  fixture.build();
  const std::string sectionCachePath = fixture.cacheRoot + "/sections/0.bin";
  fixture.storage.addFile(sectionCachePath + ".pwi",
                          layoutWordIndex({layoutRange(0, 0, 0, "b00000000"),
                                           layoutRange(1, 1, 1, "b00000000")},
                                          2));

  PdfReflowDocument document;
  ASSERT_TRUE(document.initialize(fixture.storage.io(), kSourcePath, kCacheDirectory).ok());
  ASSERT_TRUE(document.loadCompletedCache().ok());
  ASSERT_TRUE(document.validateLayoutWordIndex(sectionCachePath, 0, 2));

  PdfSavedItem bookmark{};
  bookmark.itemId = 7;
  bookmark.kind = PdfSavedItemKind::Bookmark;
  bookmark.flags = PDF_SAVED_ITEM_HAS_START_SEMANTIC;
  bookmark.startGlobalWordOrdinal = 1;
  bookmark.startBlockWordOffset = 1;
  bookmark.sectionIndex = 0;
  std::memcpy(bookmark.startBlockAnchor, "b00000000", 9);

  PdfSavedItemPageRange pages;
  ASSERT_TRUE(document.mapPdfSavedItem(sectionCachePath, 55, bookmark, &pages).ok());
  EXPECT_EQ(pages.startPage, 1);
  EXPECT_EQ(pages.endPage, 1);
  EXPECT_EQ(pages.pageCount, 2);
  EXPECT_TRUE(pages.exact);
}

TEST(PdfReflowDocument, MapsLongSavedItemWithOneOpenAndOneLinearPassBeyondValidation) {
  CacheFixture fixture;
  fixture.build();
  const std::string sectionCachePath = fixture.cacheRoot + "/sections/0.bin";
  const std::string sidecarPath = sectionCachePath + ".pwi";
  const std::vector<uint8_t> sidecar =
      layoutWordIndex(
          {
              layoutRange(0, 0, 0, "b00000000"),
              PdfLayoutWordRange{},
              PdfLayoutWordRange{},
              PdfLayoutWordRange{},
              PdfLayoutWordRange{},
              PdfLayoutWordRange{},
              PdfLayoutWordRange{},
              layoutRange(1, 1, 0, "b00000001"),
          },
          2);
  fixture.storage.addFile(sidecarPath, sidecar);

  PdfReflowDocument document;
  ASSERT_TRUE(document.initialize(fixture.storage.io(), kSourcePath, kCacheDirectory).ok());
  ASSERT_TRUE(document.loadCompletedCache().ok());

  PdfSavedItem bookmark{};
  bookmark.itemId = 7;
  bookmark.kind = PdfSavedItemKind::Bookmark;
  bookmark.flags = PDF_SAVED_ITEM_HAS_START_SEMANTIC;
  bookmark.startGlobalWordOrdinal = 1;
  bookmark.sectionIndex = 0;
  std::memcpy(bookmark.startBlockAnchor, "b00000001", 9);

  const uint32_t opensBefore = fixture.storage.openCallsForPath(sidecarPath);
  const uint32_t readsBefore = fixture.storage.readCalls();
  const uint64_t bytesBefore = fixture.storage.bytesReadTotal();
  PdfSavedItemPageRange pages;
  ASSERT_TRUE(document.mapPdfSavedItem(sectionCachePath, 55, bookmark, &pages).ok());

  EXPECT_EQ(pages.startPage, 7);
  EXPECT_EQ(fixture.storage.openCallsForPath(sidecarPath) - opensBefore, 1U);
  // One validation pass reads the exact sidecar once. The semantic lookup then
  // reads each fixed record once more without nested header/footer inspections.
  EXPECT_EQ(fixture.storage.readCalls() - readsBefore, 6U);
  EXPECT_EQ(fixture.storage.bytesReadTotal() - bytesBefore,
            sidecar.size() + 8U * PDF_LAYOUT_WORD_INDEX_RECORD_BYTES);
  EXPECT_EQ(fixture.storage.openHandleCount(), 0U);
}

TEST(PdfReflowDocument, LayoutFingerprintChangesWhenSamePageCountLayoutChanges) {
  CacheFixture fixture;
  fixture.build();
  const std::string sectionCachePath = fixture.cacheRoot + "/sections/0.bin";
  const std::string sidecarPath = sectionCachePath + ".pwi";
  fixture.storage.addFile(sidecarPath,
                          layoutWordIndex({layoutRange(0, 0, 0, "b00000000"),
                                           layoutRange(1, 1, 1, "b00000000")},
                                          2));

  PdfReflowDocument document;
  ASSERT_TRUE(document.initialize(fixture.storage.io(), kSourcePath, kCacheDirectory).ok());
  ASSERT_TRUE(document.loadCompletedCache().ok());
  uint32_t firstFingerprint = 0;
  ASSERT_TRUE(document.pdfSavedItemsLayoutFingerprint(sectionCachePath, &firstFingerprint).ok());
  EXPECT_NE(firstFingerprint, 0U);

  fixture.storage.addFile(sidecarPath,
                          layoutWordIndex({layoutRange(0, 0, 0, "b00000000"),
                                           layoutRange(1, 1, 0, "b00000001")},
                                          2));
  uint32_t secondFingerprint = 0;
  ASSERT_TRUE(document.pdfSavedItemsLayoutFingerprint(sectionCachePath, &secondFingerprint).ok());
  EXPECT_NE(secondFingerprint, 0U);
  EXPECT_NE(firstFingerprint, secondFingerprint);
}

}  // namespace
