#include <Print.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

#include "PdfCacheStore.h"
#include "PdfLayoutWordIndex.h"
#include "PdfMetadataStore.h"
#include "PdfOutline.h"
#include "PdfReflowDocument.h"
#include "PdfTestCacheIo.h"
#include "PdfTestIo.h"
#include <Memory.h>

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

struct MetadataSections {
  std::vector<PdfMetadataSection> values;

  static PdfStatus read(void* context, const uint16_t index, PdfMetadataSection* output) {
    if (context == nullptr || output == nullptr) {
      return PdfStatus::failure(PdfError::InvalidArgument);
    }
    const auto& self = *static_cast<MetadataSections*>(context);
    if (index >= self.values.size()) {
      return PdfStatus::failure(PdfError::InvalidOffset, index);
    }
    *output = self.values[index];
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
  RequiredRecords records;
  std::string cacheRoot;
  std::string selectedManifestPath;
  std::string otherManifestPath;
  std::string sectionPath;

  void build(const char* const sourcePath = kSourcePath, const uint64_t* const cacheHashOverride = nullptr,
             const uint16_t sectionCount = 1) {
    ASSERT_GT(sectionCount, 0);
    ASSERT_LE(sectionCount, PdfMetadataLimits::MaxSections);
    const std::string source = "%PDF-host-fixture-with-enough-identity-bytes";
    storage.addFile(sourcePath, std::vector<uint8_t>(source.begin(), source.end()), 42, true);
    std::array<uint8_t, PDF_SOURCE_FINGERPRINT_BYTES> workspace{};
    ASSERT_TRUE(pdfComputeSourceIdentity(storage.io(), sourcePath, workspace.data(), workspace.size(), &identity).ok());

    std::array<char, PDF_CACHE_PATH_CAPACITY> root{};
    const PdfStatus rootStatus =
        cacheHashOverride == nullptr
            ? pdfFormatCacheRoot(kCacheDirectory, sourcePath, root.data(), root.size())
            : pdfFormatCacheRootForHash(kCacheDirectory, *cacheHashOverride, root.data(), root.size());
    ASSERT_TRUE(rootStatus.ok());
    cacheRoot = root.data();
    PdfCacheStore cache;
    ASSERT_TRUE(cache.initialize(storage.io(), cacheRoot.c_str()).ok());
    ASSERT_TRUE(cache.ensureGeneration(7).ok());
    records.values.clear();
    for (uint16_t index = 0; index < sectionCount; ++index) {
      char relative[48]{};
      ASSERT_GT(std::snprintf(relative, sizeof(relative), "gen_7/sections/%06u.xhtml", index), 0);
      const std::string fullPath = cacheRoot + "/" + relative;
      if (index == 0) {
        sectionPath = fullPath;
      }
      PdfCacheTrackedWriter writer;
      ASSERT_TRUE(pdfOpenTrackedCacheWriter(storage.io(), fullPath.c_str(), relative, PdfCacheFileKind::Required,
                                            sizeof(kSection), &writer)
                      .ok());
      ASSERT_TRUE(
          pdfWriteTrackedCacheFile(&writer, reinterpret_cast<const uint8_t*>(kSection), sizeof(kSection) - 1).ok());
      PdfRequiredFileRecord sectionRecord{};
      ASSERT_TRUE(pdfCloseTrackedCacheFile(&writer, &sectionRecord).ok());
      records.values.push_back(sectionRecord);
    }

    const auto addArtifact = [&](const std::string& relative, const std::string& bytes) {
      const std::string path = cacheRoot + "/" + relative;
      storage.addFile(path, bytes);
      PdfRequiredFileRecord record{};
      ASSERT_LT(relative.size(), sizeof(record.path));
      std::memcpy(record.path, relative.data(), relative.size());
      record.pathLength = static_cast<uint8_t>(relative.size());
      record.size = bytes.size();
      record.crc32 = pdfCacheCrc32(bytes.data(), bytes.size());
      records.values.push_back(record);
    };
    addArtifact("gen_7/cover.bmp", "BMcover");
    addArtifact("gen_7/thumb.bmp", "BMthumb");

    PdfMetadataBuilder metadataBuilder;
    ASSERT_TRUE(metadataBuilder.begin(reinterpret_cast<const uint8_t*>("minimal"), 7).ok());
    PdfMetadata metadata = metadataBuilder.metadata();
    metadata.sectionCount = sectionCount;
    metadata.outlineCount = 1;
    metadata.totalWords = static_cast<uint32_t>(sectionCount) * 2U;
    MetadataSections metadataSections;
    metadataSections.values.reserve(sectionCount);
    uint32_t cumulativeSize = 0;
    for (uint16_t index = 0; index < sectionCount; ++index) {
      const uint32_t byteSize = static_cast<uint32_t>(records.values[index].size);
      cumulativeSize += byteSize;
      metadataSections.values.push_back({
          .byteSize = byteSize,
          .cumulativeSize = cumulativeSize,
          .firstWordOrdinal = static_cast<uint32_t>(index) * 2U,
          .wordCount = 2,
          .firstAnchorOrdinal = index,
          .tocIndex = static_cast<int16_t>(index == 0 ? 0 : -1),
      });
    }
    PdfTestByteSink metadataBytes;
    ASSERT_TRUE(pdfEncodeMetadata(metadata, {&metadataSections, sectionCount, MetadataSections::read},
                                  metadataBytes.sink())
                    .ok());
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
    manifest.totalWords = metadata.totalWords;
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
    selectedManifestPath = cacheRoot + (committed.selectedSlot == PdfCacheSlot::A ? "/manifest.a" : "/manifest.b");
    otherManifestPath = cacheRoot + (committed.selectedSlot == PdfCacheSlot::A ? "/manifest.b" : "/manifest.a");
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

PdfStatus patchVector(void* const context, const uint64_t offset, const uint8_t* const source,
                      const size_t requested, size_t* const bytesWritten) {
  if (context == nullptr || source == nullptr || bytesWritten == nullptr) {
    return PdfStatus::failure(PdfError::InvalidArgument, offset);
  }
  auto& bytes = *static_cast<std::vector<uint8_t>*>(context);
  if (offset > bytes.size() || requested > bytes.size() - static_cast<size_t>(offset)) {
    return PdfStatus::failure(PdfError::InvalidOffset, offset);
  }
  std::copy_n(source, requested, bytes.begin() + static_cast<std::ptrdiff_t>(offset));
  *bytesWritten = requested;
  return PdfStatus::success();
}

std::vector<uint8_t> layoutWordIndex(const std::vector<PdfLayoutWordRange>& ranges, const uint32_t totalWords,
                                      uint32_t* const pairToken = nullptr) {
  PdfTestByteSink sink;
  PdfLayoutWordIndexWriter writer;
  EXPECT_TRUE(writer.begin(sink.sink(), 0, 0, totalWords));
  for (const auto& range : ranges) {
    EXPECT_TRUE(writer.append(range));
  }
  EXPECT_TRUE(writer.finish());
  if (pairToken != nullptr) {
    *pairToken = writer.pairToken();
  }
  return sink.bytes();
}

void addLayoutPair(CacheFixture& fixture, const std::string& sectionCachePath,
                    const std::vector<PdfLayoutWordRange>& ranges, const uint32_t totalWords,
                    std::vector<uint8_t> sectionBytes = {44, 1, 2, 3, 4, 5, 6, 7}) {
  uint32_t pairToken = 0;
  std::vector<uint8_t> sidecar = layoutWordIndex(ranges, totalWords, &pairToken);
  const PdfLayoutCacheBinding binding{static_cast<uint32_t>(sectionBytes.size()), pairToken};
  uint8_t trailer[PDF_LAYOUT_CACHE_BINDING_TRAILER_BYTES];
  ASSERT_TRUE(pdfEncodeLayoutCacheBindingTrailer(binding, trailer));
  sectionBytes.insert(sectionBytes.end(), trailer, trailer + sizeof(trailer));

  PdfTestByteSource source(sidecar);
  ASSERT_TRUE(pdfBindLayoutWordIndex(source.source(), {&sidecar, patchVector}, binding));
  fixture.storage.addFile(sectionCachePath, sectionBytes);
  fixture.storage.addFile(sectionCachePath + ".pwi", sidecar);
}

class ArrayAllocationProbe {
 public:
  explicit ArrayAllocationProbe(const size_t failBytes = 0) {
    TestMemory::resetArrayProbe();
    TestMemory::observeArrays = true;
    TestMemory::failArrayBytes = failBytes;
  }
  ~ArrayAllocationProbe() { TestMemory::resetArrayProbe(); }
};

TEST(PdfReflowDocumentStorage, AllocatesExactTwoSectionZeroResourceValidationBytesOnce) {
  CacheFixture fixture;
  fixture.build(kSourcePath, nullptr, 2);

  ArrayAllocationProbe allocations;
  const uint32_t selectedManifestOpens = fixture.storage.openCallsForPath(fixture.selectedManifestPath);
  const uint32_t otherManifestOpens = fixture.storage.openCallsForPath(fixture.otherManifestPath);
  PdfReflowDocument document;
  ASSERT_TRUE(document.initialize(fixture.storage.io(), kSourcePath, kCacheDirectory).ok());
  ASSERT_TRUE(document.loadCompletedCache().ok());

  EXPECT_EQ(document.validationStorageBytesForTest(), 17U);
  EXPECT_EQ(std::count(TestMemory::arraySizes, TestMemory::arraySizes + TestMemory::arrayCount, 17U), 1);
  EXPECT_EQ(document.getSectionCount(), 2);
  EXPECT_EQ(document.getTotalWordCount(), 4U);
  EXPECT_EQ(fixture.storage.openCallsForPath(fixture.selectedManifestPath), selectedManifestOpens + 2U);
  EXPECT_EQ(fixture.storage.openCallsForPath(fixture.otherManifestPath), otherManifestOpens + 1U);
  EXPECT_EQ(fixture.storage.openHandleCount(), 0U);
}

TEST(PdfReflowDocumentStorage, FailsClosedWhenExactValidationAllocationFails) {
  CacheFixture fixture;
  fixture.build(kSourcePath, nullptr, 2);

  ArrayAllocationProbe allocations(17);
  PdfReflowDocument document;
  ASSERT_TRUE(document.initialize(fixture.storage.io(), kSourcePath, kCacheDirectory).ok());
  const PdfStatus status = document.loadCompletedCache();

  EXPECT_EQ(status.error, PdfError::InsufficientMemory);
  EXPECT_TRUE(TestMemory::sawArrayBytes(17));
  EXPECT_EQ(document.validationStorageBytesForTest(), 0U);
  EXPECT_EQ(document.getSectionCount(), 0);
  EXPECT_EQ(document.getCapabilities(), 0U);
  EXPECT_EQ(fixture.storage.openHandleCount(), 0U);
}

TEST(PdfReflowDocumentStorage, ReleasesExactValidationStorageAfterRequiredFileCorruption) {
  CacheFixture fixture;
  fixture.build(kSourcePath, nullptr, 2);
  fixture.storage.corruptByte(fixture.sectionPath, 12, 0x40);

  ArrayAllocationProbe allocations;
  PdfReflowDocument document;
  ASSERT_TRUE(document.initialize(fixture.storage.io(), kSourcePath, kCacheDirectory).ok());
  const PdfStatus status = document.loadCompletedCache();

  EXPECT_EQ(status.error, PdfError::Malformed);
  EXPECT_TRUE(TestMemory::sawArrayBytes(17));
  EXPECT_EQ(document.validationStorageBytesForTest(), 0U);
  EXPECT_EQ(document.getSectionCount(), 0);
  EXPECT_EQ(document.getCapabilities(), 0U);
  EXPECT_EQ(fixture.storage.openHandleCount(), 0U);
}

TEST(PdfReflowDocument, FallsBackToOlderCompletedManifestWhenNewestGenerationFilesFailValidation) {
  CacheFixture fixture;
  fixture.build();

  PdfCacheStore cache;
  ASSERT_TRUE(cache.initialize(fixture.storage.io(), fixture.cacheRoot.c_str()).ok());
  PdfCacheManifestSelection prior;
  ASSERT_TRUE(cache.loadManifestSlots(fixture.identity, &prior).ok());
  ASSERT_TRUE(prior.selected);

  RequiredRecords badRecords = fixture.records;
  ASSERT_FALSE(badRecords.values.empty());
  badRecords.values[0].crc32 ^= 1U;
  PdfCacheManifest newest = prior.manifest;
  ++newest.sequence;
  newest.requiredFileLedger = PDF_CACHE_FNV64_OFFSET;
  for (const auto& record : badRecords.values) {
    newest.requiredFileLedger = pdfUpdateRequiredFileLedger(newest.requiredFileLedger, record);
  }
  const PdfCacheCommitEvidence evidence{
      true,
      newest.requiredFileCount,
      newest.requiredFileBytes,
      newest.requiredFileLedger,
  };
  PdfCacheManifestSelection committed;
  ASSERT_TRUE(cache.commitManifest(newest, badRecords.source(), evidence, prior, &committed).ok());
  ASSERT_TRUE(committed.selected);
  ASSERT_NE(committed.selectedSlot, prior.selectedSlot);

  PdfReflowDocument document;
  ASSERT_TRUE(document.initialize(fixture.storage.io(), kSourcePath, kCacheDirectory).ok());
  const PdfStatus status = document.loadCompletedCache();

  EXPECT_TRUE(status.ok()) << static_cast<int>(status.error) << "@" << status.offset;
  EXPECT_EQ(document.getSectionCount(), 1);
  EXPECT_EQ(document.getTotalWordCount(), 2U);
  EXPECT_EQ(fixture.storage.openHandleCount(), 0U);
}

TEST(PdfReflowDocumentStorage, BoundsZeroMaximumAndOverflowLayouts) {
  size_t bytes = 99;
  EXPECT_TRUE(PdfReflowDocument::validationStorageLayoutForTest(0, 0, &bytes).ok());
  EXPECT_EQ(bytes, 0U);
  EXPECT_TRUE(PdfReflowDocument::validationStorageLayoutForTest(2, 0, &bytes).ok());
  EXPECT_EQ(bytes, 17U);
  EXPECT_TRUE(PdfReflowDocument::validationStorageLayoutForTest(PdfMetadataLimits::MaxSections, 64, &bytes).ok());
  EXPECT_EQ(bytes, 4640U);
  EXPECT_EQ(PdfReflowDocument::validationStorageLayoutForTest(PdfMetadataLimits::MaxSections + 1U, 0, &bytes).error,
            PdfError::LimitExceeded);
  EXPECT_EQ(bytes, 0U);
  EXPECT_EQ(PdfReflowDocument::validationStorageLayoutForTest(1, 65, &bytes).error, PdfError::LimitExceeded);
  EXPECT_EQ(bytes, 0U);
  EXPECT_EQ(PdfReflowDocument::validationStorageLayoutForTest(std::numeric_limits<size_t>::max(), 1, &bytes).error,
            PdfError::LimitExceeded);
  EXPECT_EQ(bytes, 0U);
  EXPECT_EQ(PdfReflowDocument::validationStorageLayoutForTest(1, std::numeric_limits<size_t>::max(), &bytes).error,
            PdfError::LimitExceeded);
  EXPECT_EQ(bytes, 0U);
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
  EXPECT_EQ(loadedPosition.pageNumber, 0);
  EXPECT_EQ(loadedPosition.pageCount, 0);
  EXPECT_FALSE(loadedPosition.hasPageCount);
  EXPECT_TRUE(loadedPosition.hasSemanticPosition);
  EXPECT_EQ(loadedPosition.globalWordOrdinal, 1U);
  EXPECT_EQ(loadedPosition.blockWordOffset, 1U);
  EXPECT_STREQ(loadedPosition.blockAnchor, "b00000000");
  EXPECT_EQ(fixture.storage.openCallsForPath(kSourcePath), sourceOpensBefore + 1);
  EXPECT_EQ(fixture.storage.openHandleCount(), 0U);
}

TEST(PdfReflowDocument, LoadsReadOnlyCacheFromCallerSuppliedMigrationHash) {
  constexpr char oldPath[] = "/books/original.pdf";
  constexpr char newPath[] = "/books/migrated.pdf";
  const uint64_t oldHash = pdfPathHash64(oldPath, sizeof(oldPath) - 1U);
  CacheFixture fixture;
  fixture.build(newPath, &oldHash);

  PdfReflowDocument normal;
  ASSERT_TRUE(normal.initialize(fixture.storage.io(), newPath, kCacheDirectory).ok());
  EXPECT_FALSE(normal.loadCompletedCache().ok());

  fixture.storage.clearEvents();
  PdfReflowDocument migrated;
  ASSERT_TRUE(migrated.initialize(fixture.storage.io(), newPath, kCacheDirectory, &oldHash).ok());
  const PdfStatus migratedStatus = migrated.loadCompletedCache();
  ASSERT_TRUE(migratedStatus.ok())
      << "error=" << static_cast<unsigned>(migratedStatus.error) << " offset=" << migratedStatus.offset;

  EXPECT_EQ(migrated.getSectionCount(), 1U);
  ReflowResource section;
  ASSERT_TRUE(migrated.getImmutableLocalSection(0, section));
  EXPECT_EQ(section.localPath, fixture.sectionPath);
  EXPECT_EQ(fixture.storage.openHandleCount(), 0U);

  const auto& events = fixture.storage.events();
  const auto sourceClose = std::find(events.begin(), events.end(), std::string("close:") + newPath);
  ASSERT_NE(sourceClose, events.end());
  const auto firstCacheOpen = std::find_if(sourceClose + 1, events.end(), [&](const std::string& event) {
    return event.rfind(std::string("open:") + fixture.cacheRoot + "/", 0) == 0;
  });
  ASSERT_NE(firstCacheOpen, events.end());
  EXPECT_LT(sourceClose, firstCacheOpen);
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

TEST(PdfReflowDocument, RemapsStalePerPageProgressAtARegroupedSectionBoundaryWithoutAllocating) {
  CacheFixture fixture;
  fixture.storage.setMaximumReadHandles(1);
  fixture.build(kSourcePath, nullptr, 2);

  PdfReflowDocument document;
  ASSERT_TRUE(document.initialize(fixture.storage.io(), kSourcePath, kCacheDirectory).ok());
  ASSERT_TRUE(document.loadCompletedCache().ok());

  ReflowReadingPosition stale;
  stale.sectionIndex = 2;
  stale.pageNumber = 5;
  stale.pageCount = 9;
  stale.hasPageCount = true;
  stale.hasSemanticPosition = true;
  stale.hasWordCursor = true;
  stale.globalWordOrdinal = 2;
  stale.blockWordOffset = 7;
  stale.wordCursor = 1;
  setAnchor(stale, "b00000002");
  ASSERT_TRUE(document.saveReadingPosition(stale));

  ArrayAllocationProbe allocations;
  const size_t allocationsBefore = TestMemory::arrayCount;
  ReflowReadingPosition loaded;
  ASSERT_TRUE(document.loadReadingPosition(loaded));

  EXPECT_EQ(loaded.sectionIndex, 1);
  EXPECT_EQ(loaded.pageNumber, 0);
  EXPECT_EQ(loaded.pageCount, 0);
  EXPECT_FALSE(loaded.hasPageCount);
  EXPECT_TRUE(loaded.hasSemanticPosition);
  EXPECT_TRUE(loaded.hasWordCursor);
  EXPECT_EQ(loaded.globalWordOrdinal, 2U);
  EXPECT_EQ(loaded.blockWordOffset, 7U);
  EXPECT_EQ(loaded.wordCursor, 1U);
  EXPECT_STREQ(loaded.blockAnchor, "b00000002");
  EXPECT_EQ(TestMemory::arrayCount, allocationsBefore);
  EXPECT_EQ(fixture.storage.openHandleCount(), 0U);
}

TEST(PdfReflowDocument, RemapsCursorOnlyProgressAtDocumentEndToTheFinalLogicalSection) {
  CacheFixture fixture;
  fixture.storage.setMaximumReadHandles(1);
  fixture.build(kSourcePath, nullptr, 2);

  PdfReflowDocument document;
  ASSERT_TRUE(document.initialize(fixture.storage.io(), kSourcePath, kCacheDirectory).ok());
  ASSERT_TRUE(document.loadCompletedCache().ok());

  ReflowReadingPosition stale;
  stale.sectionIndex = 3;
  stale.pageNumber = 4;
  stale.pageCount = 8;
  stale.hasPageCount = true;
  stale.hasWordCursor = true;
  stale.wordCursor = 4;
  ASSERT_TRUE(document.saveReadingPosition(stale));

  ReflowReadingPosition loaded;
  ASSERT_TRUE(document.loadReadingPosition(loaded));
  EXPECT_EQ(loaded.sectionIndex, 1);
  EXPECT_EQ(loaded.pageNumber, 0);
  EXPECT_EQ(loaded.pageCount, 0);
  EXPECT_FALSE(loaded.hasPageCount);
  EXPECT_FALSE(loaded.hasSemanticPosition);
  EXPECT_TRUE(loaded.hasWordCursor);
  EXPECT_EQ(loaded.wordCursor, 4U);
  EXPECT_EQ(fixture.storage.openHandleCount(), 0U);
}

TEST(PdfReflowDocument, PreservesLegacyPageTupleWhenNoSemanticCoordinateExists) {
  CacheFixture fixture;
  fixture.storage.setMaximumReadHandles(1);
  fixture.build(kSourcePath, nullptr, 2);

  PdfReflowDocument document;
  ASSERT_TRUE(document.initialize(fixture.storage.io(), kSourcePath, kCacheDirectory).ok());
  ASSERT_TRUE(document.loadCompletedCache().ok());

  ReflowReadingPosition legacy;
  legacy.sectionIndex = 1;
  legacy.pageNumber = 3;
  legacy.pageCount = 8;
  legacy.hasPageCount = true;
  ASSERT_TRUE(document.saveReadingPosition(legacy));

  ReflowReadingPosition loaded;
  ASSERT_TRUE(document.loadReadingPosition(loaded));
  EXPECT_EQ(loaded.sectionIndex, 1);
  EXPECT_EQ(loaded.pageNumber, 3);
  EXPECT_EQ(loaded.pageCount, 8);
  EXPECT_TRUE(loaded.hasPageCount);
  EXPECT_FALSE(loaded.hasSemanticPosition);
  EXPECT_FALSE(loaded.hasWordCursor);
  EXPECT_EQ(fixture.storage.openHandleCount(), 0U);
}

TEST(PdfReflowDocument, FailsClosedForOutOfRangeProgressWithoutASemanticCoordinate) {
  CacheFixture fixture;
  fixture.storage.setMaximumReadHandles(1);
  fixture.build(kSourcePath, nullptr, 2);

  PdfReflowDocument document;
  ASSERT_TRUE(document.initialize(fixture.storage.io(), kSourcePath, kCacheDirectory).ok());
  ASSERT_TRUE(document.loadCompletedCache().ok());

  ReflowReadingPosition stale;
  stale.sectionIndex = 2;
  stale.pageNumber = 1;
  ASSERT_TRUE(document.saveReadingPosition(stale));

  ReflowReadingPosition loaded;
  EXPECT_FALSE(document.loadReadingPosition(loaded));
  EXPECT_EQ(fixture.storage.openHandleCount(), 0U);
}

TEST(PdfReflowDocument, RemapsSavedItemsBySemanticWordAndDropsStaleFallbackPagesWithoutAllocating) {
  CacheFixture fixture;
  fixture.storage.setMaximumReadHandles(1);
  fixture.build(kSourcePath, nullptr, 2);

  PdfReflowDocument document;
  ASSERT_TRUE(document.initialize(fixture.storage.io(), kSourcePath, kCacheDirectory).ok());
  ASSERT_TRUE(document.loadCompletedCache().ok());

  PdfSavedItem bookmark{};
  bookmark.itemId = 7;
  bookmark.kind = PdfSavedItemKind::Bookmark;
  bookmark.flags = PDF_SAVED_ITEM_HAS_START_SEMANTIC | PDF_SAVED_ITEM_HAS_FALLBACK_PAGES;
  bookmark.startGlobalWordOrdinal = 2;
  bookmark.startBlockWordOffset = 6;
  bookmark.sectionIndex = 2;
  bookmark.fallbackStartPage = 4;
  bookmark.fallbackEndPage = 4;
  bookmark.fallbackPageCount = 7;
  bookmark.fallbackLayoutFingerprint = 0x12345678U;
  std::memcpy(bookmark.startBlockAnchor, "b00000002", 9);
  ASSERT_TRUE(document.savePdfSavedItems(&bookmark, 1).ok());

  std::array<PdfSavedItem, PDF_SAVED_ITEMS_MAX_RECORDS> items{};
  PdfSavedItemsBuffer buffer{items.data(), static_cast<uint16_t>(items.size()), 0};
  ArrayAllocationProbe allocations;
  const size_t allocationsBefore = TestMemory::arrayCount;
  ASSERT_TRUE(document.loadPdfSavedItems(&buffer).ok());

  ASSERT_EQ(buffer.count, 1);
  EXPECT_EQ(buffer.items[0].sectionIndex, 1U);
  EXPECT_EQ(buffer.items[0].startGlobalWordOrdinal, 2U);
  EXPECT_EQ(buffer.items[0].startBlockWordOffset, 6U);
  EXPECT_STREQ(buffer.items[0].startBlockAnchor, "b00000002");
  EXPECT_EQ(buffer.items[0].flags & PDF_SAVED_ITEM_HAS_FALLBACK_PAGES, 0U);
  EXPECT_EQ(buffer.items[0].fallbackStartPage, 0U);
  EXPECT_EQ(buffer.items[0].fallbackEndPage, 0U);
  EXPECT_EQ(buffer.items[0].fallbackPageCount, 0U);
  EXPECT_EQ(buffer.items[0].fallbackLayoutFingerprint, 0U);
  EXPECT_EQ(TestMemory::arrayCount, allocationsBefore);
  EXPECT_EQ(fixture.storage.openHandleCount(), 0U);
}

TEST(PdfReflowDocument, PreservesSavedItemFallbackPagesWhenTheSemanticSectionIsUnchanged) {
  CacheFixture fixture;
  fixture.storage.setMaximumReadHandles(1);
  fixture.build(kSourcePath, nullptr, 2);

  PdfReflowDocument document;
  ASSERT_TRUE(document.initialize(fixture.storage.io(), kSourcePath, kCacheDirectory).ok());
  ASSERT_TRUE(document.loadCompletedCache().ok());

  PdfSavedItem bookmark{};
  bookmark.itemId = 8;
  bookmark.kind = PdfSavedItemKind::Bookmark;
  bookmark.flags = PDF_SAVED_ITEM_HAS_START_SEMANTIC | PDF_SAVED_ITEM_HAS_FALLBACK_PAGES;
  bookmark.startGlobalWordOrdinal = 2;
  bookmark.startBlockWordOffset = 6;
  bookmark.sectionIndex = 1;
  bookmark.fallbackStartPage = 4;
  bookmark.fallbackEndPage = 4;
  bookmark.fallbackPageCount = 7;
  bookmark.fallbackLayoutFingerprint = 0x12345678U;
  std::memcpy(bookmark.startBlockAnchor, "b00000002", 9);
  ASSERT_TRUE(document.savePdfSavedItems(&bookmark, 1).ok());

  std::array<PdfSavedItem, PDF_SAVED_ITEMS_MAX_RECORDS> items{};
  PdfSavedItemsBuffer buffer{items.data(), static_cast<uint16_t>(items.size()), 0};
  ASSERT_TRUE(document.loadPdfSavedItems(&buffer).ok());

  ASSERT_EQ(buffer.count, 1);
  EXPECT_EQ(buffer.items[0].sectionIndex, 1U);
  EXPECT_EQ(buffer.items[0].flags,
            PDF_SAVED_ITEM_HAS_START_SEMANTIC | PDF_SAVED_ITEM_HAS_FALLBACK_PAGES);
  EXPECT_EQ(buffer.items[0].fallbackStartPage, 4U);
  EXPECT_EQ(buffer.items[0].fallbackEndPage, 4U);
  EXPECT_EQ(buffer.items[0].fallbackPageCount, 7U);
  EXPECT_EQ(buffer.items[0].fallbackLayoutFingerprint, 0x12345678U);
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
  addLayoutPair(fixture, sectionCachePath,
                {layoutRange(0, 0, 0, "b00000000"), layoutRange(1, 1, 0, "b00000000")}, 2);

  PdfReflowDocument document;
  ASSERT_TRUE(document.initialize(fixture.storage.io(), kSourcePath, kCacheDirectory).ok());
  ASSERT_TRUE(document.loadCompletedCache().ok());
  ASSERT_TRUE(document.validateLayoutWordIndex(sectionCachePath, 0, 2));

  uint16_t page = UINT16_MAX;
  ASSERT_TRUE(document.findLayoutWordPage(sectionCachePath, "b00000000", 0, 1, page));
  EXPECT_EQ(page, 1U);
}

TEST(PdfReflowDocument, RejectsSameLengthSectionCacheFromAnotherBoundSidecarPair) {
  CacheFixture fixture;
  fixture.build();
  const std::string sectionCachePath = fixture.cacheRoot + "/sections/0.bin";
  const std::vector<uint8_t> oldSection = {44, 1, 2, 3, 4, 5, 6, 7};
  const std::vector<uint8_t> newSection = {44, 1, 2, 3, 4, 5, 6, 8};
  addLayoutPair(fixture, sectionCachePath,
                {layoutRange(0, 0, 0, "b00000000"), layoutRange(1, 1, 1, "b00000000")}, 2,
                oldSection);

  PdfReflowDocument document;
  ASSERT_TRUE(document.initialize(fixture.storage.io(), kSourcePath, kCacheDirectory).ok());
  ASSERT_TRUE(document.loadCompletedCache().ok());
  ASSERT_TRUE(document.validateLayoutWordIndex(sectionCachePath, 0, 2));

  const std::vector<uint8_t> oldSidecar = fixture.storage.bytes(sectionCachePath + ".pwi");
  addLayoutPair(fixture, sectionCachePath,
                {layoutRange(0, 0, 0, "b00000000"), layoutRange(1, 1, 0, "b00000000")}, 2,
                newSection);
  fixture.storage.addFile(sectionCachePath + ".pwi", oldSidecar);
  EXPECT_FALSE(document.validateLayoutWordIndex(sectionCachePath, 0, 2));
}

TEST(PdfReflowDocument, RetriesValidatedSidecarAfterTransientOpenFailure) {
  CacheFixture fixture;
  fixture.build();
  const std::string sectionCachePath = fixture.cacheRoot + "/sections/0.bin";
  addLayoutPair(fixture, sectionCachePath,
                {layoutRange(0, 0, 0, "b00000000"), layoutRange(1, 1, 1, "b00000000")}, 2);

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
  addLayoutPair(fixture, sectionCachePath, ranges, 2);

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
  addLayoutPair(fixture, sectionCachePath,
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
  addLayoutPair(fixture, sectionCachePath, {layoutRange(0, 1, 0, "b00000000")}, 2);

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
  addLayoutPair(fixture, sectionCachePath,
                {layoutRange(0, 0, 0, "b00000000"), layoutRange(1, 1, 1, "b00000000")}, 2);

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
  fixture.storage.addFile(sectionCachePath, std::vector<uint8_t>{44, 1, 2, 3, 4, 5, 6, 7});
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
