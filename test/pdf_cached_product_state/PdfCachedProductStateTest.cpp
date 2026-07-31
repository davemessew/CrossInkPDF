#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <new>
#include <string>
#include <vector>

#include "PdfCacheFormat.h"
#include "PdfCacheManifest.h"
#include "PdfCachedProductState.h"
#include "PdfMetadataStore.h"
#include "PdfOutline.h"
#include "PdfProgressStore.h"
#include "PdfSourceIdentity.h"
#include "PdfTestCacheIo.h"
#include "PdfTestIo.h"

namespace {

constexpr char kSourcePath[] = "/books/product.pdf";
constexpr char kCacheDirectory[] = "/.crosspoint";

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

  PdfMetadataSectionSource source() { return {this, static_cast<uint16_t>(values.size()), read}; }
};

struct OutlineEntries {
  std::vector<PdfOutlineEntry> values;

  static PdfStatus read(void* context, const uint16_t index, PdfOutlineEntry* output) {
    if (context == nullptr || output == nullptr) {
      return PdfStatus::failure(PdfError::InvalidArgument);
    }
    const auto& self = *static_cast<OutlineEntries*>(context);
    if (index >= self.values.size()) {
      return PdfStatus::failure(PdfError::InvalidOffset, index);
    }
    *output = self.values[index];
    return PdfStatus::success();
  }

  PdfOutlineEntrySource source() { return {this, static_cast<uint16_t>(values.size()), read}; }
};

void putU16(std::vector<uint8_t>& bytes, const size_t offset, const uint16_t value) {
  bytes[offset] = static_cast<uint8_t>(value);
  bytes[offset + 1] = static_cast<uint8_t>(value >> 8U);
}

void putU32(std::vector<uint8_t>& bytes, const size_t offset, const uint32_t value) {
  bytes[offset] = static_cast<uint8_t>(value);
  bytes[offset + 1] = static_cast<uint8_t>(value >> 8U);
  bytes[offset + 2] = static_cast<uint8_t>(value >> 16U);
  bytes[offset + 3] = static_cast<uint8_t>(value >> 24U);
}

std::vector<uint8_t> monochromeBmp(const uint32_t width, const uint32_t height, const uint8_t fill) {
  const uint32_t rowBytes = ((width + 31U) / 32U) * 4U;
  const uint32_t pixelBytes = rowBytes * height;
  std::vector<uint8_t> bytes(62U + pixelBytes, fill);
  std::fill(bytes.begin(), bytes.begin() + 62, 0);
  bytes[0] = 'B';
  bytes[1] = 'M';
  putU32(bytes, 2, static_cast<uint32_t>(bytes.size()));
  putU32(bytes, 10, 62);
  putU32(bytes, 14, 40);
  putU32(bytes, 18, width);
  putU32(bytes, 22, static_cast<uint32_t>(-static_cast<int32_t>(height)));
  putU16(bytes, 26, 1);
  putU16(bytes, 28, 1);
  putU32(bytes, 34, pixelBytes);
  putU32(bytes, 46, 2);
  bytes[54] = 0;
  bytes[55] = 0;
  bytes[56] = 0;
  bytes[57] = 0;
  bytes[58] = 0xff;
  bytes[59] = 0xff;
  bytes[60] = 0xff;
  bytes[61] = 0;
  return bytes;
}

PdfOutlineEntry outlineEntry(const char* title, const uint16_t section) {
  PdfOutlineEntry entry{};
  const size_t length = std::strlen(title);
  EXPECT_LT(length, sizeof(entry.title));
  std::memcpy(entry.title, title, length);
  entry.titleLength = static_cast<uint8_t>(length);
  entry.parentIndex = -1;
  entry.sectionIndex = section;
  entry.level = 1;
  entry.anchorOrdinal = section == 0 ? 0 : 4;
  entry.sourcePageIndex = section;
  return entry;
}

struct GenerationOptions {
  bool completed = true;
  bool misorderProductRecords = false;
  bool omitSectionRecords = false;
  bool includeRetainedImage = true;
  bool retainedImageBeforeSections = false;
  bool addUnknownRequiredPath = false;
  uint16_t metadataSectionCount = 2;
  const char* retainedImageLeaf = "images/0123456789abcdef-89abcdef.pxc";
};

struct ProductFixture {
  PdfTestCacheIo storage;
  PdfSourceIdentity identity{};
  std::string cacheRoot;

  void initialize(const char* const sourcePath = kSourcePath, const uint64_t* const cacheHashOverride = nullptr) {
    const std::string source = "%PDF-cached-product-state-source-identity";
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
    storage.clearEvents();
  }

  std::string fullPath(const uint32_t generation, const char* leaf) const {
    return cacheRoot + "/gen_" + std::to_string(generation) + "/" + leaf;
  }

  PdfRequiredFileRecord addRequired(const uint32_t generation, const char* leaf, const std::vector<uint8_t>& bytes) {
    const std::string relative = "gen_" + std::to_string(generation) + "/" + leaf;
    EXPECT_LT(relative.size(), PDF_CACHE_REQUIRED_PATH_CAPACITY);
    storage.addFile(cacheRoot + "/" + relative, bytes);
    PdfRequiredFileRecord record{};
    std::memcpy(record.path, relative.data(), relative.size());
    record.pathLength = static_cast<uint8_t>(relative.size());
    record.size = bytes.size();
    record.crc32 = pdfCacheCrc32(bytes.data(), bytes.size());
    return record;
  }

  void addGeneration(const uint32_t generation, const uint32_t sequence, const char slot,
                     const char* title = "Cached Book", const char* author = "Cache Author",
                     const PdfSourceIdentity* manifestIdentity = nullptr,
                     const GenerationOptions& options = GenerationOptions{}) {
    RequiredRecords required;
    const std::vector<uint8_t> section0{'<', 'p', '>', 'o', 'n', 'e', '<', '/', 'p', '>'};
    const std::vector<uint8_t> section1{'<', 'p', '>', 't', 'w', 'o', '<', '/', 'p', '>'};
    const PdfRequiredFileRecord section0Record = addRequired(generation, "sections/000000.xhtml", section0);
    const PdfRequiredFileRecord section1Record = addRequired(generation, "sections/000001.xhtml", section1);
    const PdfRequiredFileRecord imageRecord = addRequired(generation, options.retainedImageLeaf, {1, 2, 3, 4, 5});
    if (options.includeRetainedImage && options.retainedImageBeforeSections) {
      required.values.push_back(imageRecord);
    }
    if (!options.omitSectionRecords) {
      required.values.push_back(section0Record);
      required.values.push_back(section1Record);
    }
    if (options.includeRetainedImage && !options.retainedImageBeforeSections) {
      required.values.push_back(imageRecord);
    }
    if (options.addUnknownRequiredPath) {
      required.values.push_back(addRequired(generation, "unexpected.bin", {9, 8, 7}));
    }

    const std::vector<uint8_t> cover = monochromeBmp(240, 400, 0xaa);
    const std::vector<uint8_t> thumbnail = monochromeBmp(96, 160, 0x55);
    const PdfRequiredFileRecord coverRecord = addRequired(generation, "cover.bmp", cover);
    const PdfRequiredFileRecord thumbnailRecord = addRequired(generation, "thumb.bmp", thumbnail);
    if (options.misorderProductRecords) {
      required.values.push_back(thumbnailRecord);
      required.values.push_back(coverRecord);
    } else {
      required.values.push_back(coverRecord);
      required.values.push_back(thumbnailRecord);
    }

    PdfMetadataBuilder metadataBuilder;
    ASSERT_TRUE(metadataBuilder.begin(reinterpret_cast<const uint8_t*>("product.pdf"), 11).ok());
    ASSERT_TRUE(
        metadataBuilder.setTitle(PdfMetadataOrigin::Xmp, reinterpret_cast<const uint8_t*>(title), std::strlen(title))
            .ok());
    ASSERT_TRUE(
        metadataBuilder.setAuthor(PdfMetadataOrigin::Xmp, reinterpret_cast<const uint8_t*>(author), std::strlen(author))
            .ok());
    PdfMetadata metadata = metadataBuilder.metadata();
    metadata.sectionCount = options.metadataSectionCount;
    metadata.outlineCount = 2;
    metadata.totalWords = 10;
    MetadataSections sections;
    if (options.metadataSectionCount == 1) {
      sections.values = {{
          .byteSize = static_cast<uint32_t>(section0Record.size + section1Record.size),
          .cumulativeSize = static_cast<uint32_t>(section0Record.size + section1Record.size),
          .firstWordOrdinal = 0,
          .wordCount = 10,
          .firstAnchorOrdinal = 0,
          .tocIndex = 0,
      }};
    } else {
      sections.values = {
          {
              .byteSize = static_cast<uint32_t>(section0Record.size),
              .cumulativeSize = static_cast<uint32_t>(section0Record.size),
              .firstWordOrdinal = 0,
              .wordCount = 4,
              .firstAnchorOrdinal = 0,
              .tocIndex = 0,
          },
          {
              .byteSize = static_cast<uint32_t>(section1Record.size),
              .cumulativeSize = static_cast<uint32_t>(section0Record.size + section1Record.size),
              .firstWordOrdinal = 4,
              .wordCount = 6,
              .firstAnchorOrdinal = 4,
              .tocIndex = 1,
          },
      };
    }
    PdfTestByteSink metadataBytes;
    ASSERT_TRUE(pdfEncodeMetadata(metadata, sections.source(), metadataBytes.sink()).ok());
    required.values.push_back(addRequired(generation, "metadata.bin", metadataBytes.bytes()));

    OutlineEntries outline;
    outline.values = {outlineEntry("Opening", 0), outlineEntry("Deep Chapter", 1)};
    PdfTestByteSink outlineBytes;
    ASSERT_TRUE(pdfEncodeOutline(outline.source(), outlineBytes.sink()).ok());
    required.values.push_back(addRequired(generation, "outline.bin", outlineBytes.bytes()));

    PdfCacheManifest manifest{};
    manifest.sequence = sequence;
    manifest.completed = options.completed;
    manifest.source = manifestIdentity == nullptr ? identity : *manifestIdentity;
    manifest.generation = generation;
    manifest.totalWords = 10;
    manifest.requiredFileCount = static_cast<uint32_t>(required.values.size());
    manifest.requiredFileLedger = PDF_CACHE_FNV64_OFFSET;
    for (const PdfRequiredFileRecord& record : required.values) {
      manifest.requiredFileBytes += record.size;
      manifest.requiredFileLedger = pdfUpdateRequiredFileLedger(manifest.requiredFileLedger, record);
    }
    PdfTestByteSink manifestBytes;
    ASSERT_TRUE(pdfEncodeCacheManifest(manifest, required.source(), manifestBytes.sink()).ok());
    storage.addFile(cacheRoot + (slot == 'a' ? "/manifest.a" : "/manifest.b"), manifestBytes.bytes());
    storage.clearEvents();
  }

  void saveProgress(const uint16_t section, const uint32_t wordCursor) {
    PdfProgressStore store;
    ASSERT_TRUE(store.initialize(storage.io(), cacheRoot.c_str(), identity, 10).ok());
    ReflowReadingPosition position{};
    position.sectionIndex = section;
    position.pageNumber = 2;
    position.pageCount = 5;
    position.hasPageCount = true;
    position.hasSemanticPosition = true;
    position.hasWordCursor = true;
    position.globalWordOrdinal = wordCursor == 10 ? 9 : wordCursor;
    position.wordCursor = wordCursor;
    std::memcpy(position.blockAnchor, "b00000004", 9);
    ASSERT_TRUE(store.save(position).ok());
    storage.clearEvents();
  }
};

void expectEmpty(const PdfCachedProductState& state) {
  EXPECT_EQ(state.title[0], '\0');
  EXPECT_EQ(state.author[0], '\0');
  EXPECT_EQ(state.currentChapter[0], '\0');
  EXPECT_EQ(state.coverPath[0], '\0');
  EXPECT_EQ(state.thumbnailPath[0], '\0');
  EXPECT_EQ(state.generation, 0U);
  EXPECT_EQ(state.totalWords, 0U);
  EXPECT_EQ(state.currentWord, 0U);
  EXPECT_FALSE(state.hasProgress);
}

TEST(PdfCachedProductState, ExposesCompletedMetadataWithoutProgress) {
  ProductFixture fixture;
  fixture.initialize();
  fixture.addGeneration(7, 3, 'a');

  PdfCachedProductState state{};
  const auto result = pdfLoadCachedProductState(fixture.storage.io(), kSourcePath, kCacheDirectory, &state);

  ASSERT_TRUE(result.available());
  EXPECT_STREQ(state.title, "Cached Book");
  EXPECT_STREQ(state.author, "Cache Author");
  EXPECT_STREQ(state.currentChapter, "Opening");
  EXPECT_EQ(state.generation, 7U);
  EXPECT_EQ(state.totalWords, 10U);
  EXPECT_EQ(state.currentWord, 0U);
  EXPECT_EQ(state.currentSection, 0U);
  EXPECT_FALSE(state.hasProgress);
}

TEST(PdfCachedProductState, ExposesSelectedCoverThumbnailChapterAndRootWordProgress) {
  ProductFixture fixture;
  fixture.initialize();
  fixture.addGeneration(7, 3, 'a');
  fixture.saveProgress(1, 7);

  PdfCachedProductState state{};
  const auto result = pdfLoadCachedProductState(fixture.storage.io(), kSourcePath, kCacheDirectory, &state);

  ASSERT_TRUE(result.available());
  EXPECT_STREQ(state.coverPath, fixture.fullPath(7, "cover.bmp").c_str());
  EXPECT_STREQ(state.thumbnailPath, fixture.fullPath(7, "thumb.bmp").c_str());
  EXPECT_STREQ(state.currentChapter, "Deep Chapter");
  EXPECT_EQ(state.currentWord, 7U);
  EXPECT_EQ(state.currentSection, 1U);
  EXPECT_TRUE(state.hasProgress);
}

TEST(PdfCachedProductState, LoadsReadOnlyProductsFromCallerSuppliedMigrationHash) {
  constexpr char oldPath[] = "/books/original-product.pdf";
  constexpr char newPath[] = "/books/migrated-product.pdf";
  const uint64_t oldHash = pdfPathHash64(oldPath, sizeof(oldPath) - 1U);
  ProductFixture fixture;
  fixture.initialize(newPath, &oldHash);
  fixture.addGeneration(7, 3, 'a');
  fixture.saveProgress(1, 7);

  PdfCachedProductState normalState{};
  EXPECT_FALSE(
      pdfLoadCachedProductState(fixture.storage.io(), newPath, kCacheDirectory, &normalState).available());

  fixture.storage.clearEvents();
  fixture.storage.setMaximumReadHandles(1);
  PdfCachedProductState migratedState{};
  const auto migrated =
      pdfLoadCachedProductState(fixture.storage.io(), newPath, kCacheDirectory, &migratedState, &oldHash);

  ASSERT_TRUE(migrated.available());
  EXPECT_STREQ(migratedState.title, "Cached Book");
  EXPECT_STREQ(migratedState.currentChapter, "Deep Chapter");
  EXPECT_EQ(migratedState.currentWord, 7U);
  EXPECT_STREQ(migratedState.coverPath, fixture.fullPath(7, "cover.bmp").c_str());
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

TEST(PdfCachedProductState, NeverOpensXhtmlOrUnrelatedRetainedImages) {
  ProductFixture fixture;
  fixture.initialize();
  fixture.addGeneration(7, 3, 'a');
  fixture.saveProgress(1, 7);
  const std::string section = fixture.fullPath(7, "sections/000001.xhtml");
  const std::string image = fixture.fullPath(7, "images/0123456789abcdef-89abcdef.pxc");
  ASSERT_TRUE(fixture.storage.exists(image));

  PdfCacheIo imageProbeIo = fixture.storage.io();
  PdfCacheHandle imageProbeHandle{};
  const uint32_t imageOpensBeforeProbe = fixture.storage.openCallsForPath(image);
  ASSERT_TRUE(imageProbeIo.open(imageProbeIo.context, image.c_str(), PdfCacheOpenMode::Read, &imageProbeHandle).ok());
  ASSERT_EQ(fixture.storage.openCallsForPath(image), imageOpensBeforeProbe + 1U);
  ASSERT_TRUE(imageProbeIo.close(imageProbeIo.context, &imageProbeHandle).ok());
  fixture.storage.clearEvents();

  const uint32_t sectionOpens = fixture.storage.openCallsForPath(section);
  const uint32_t imageOpens = fixture.storage.openCallsForPath(image);

  PdfCachedProductState state{};
  ASSERT_TRUE(pdfLoadCachedProductState(fixture.storage.io(), kSourcePath, kCacheDirectory, &state).available());

  EXPECT_EQ(fixture.storage.openCallsForPath(section), sectionOpens);
  EXPECT_EQ(fixture.storage.openCallsForPath(image), imageOpens);
  EXPECT_EQ(fixture.storage.openHandleCount(), 0U);
  EXPECT_LE(fixture.storage.maximumReadRequest(), PDF_SOURCE_FINGERPRINT_BYTES);
  int openHandles = 0;
  int maximumOpenHandles = 0;
  for (const std::string& event : fixture.storage.events()) {
    if (event.rfind("open:", 0) == 0 && fixture.storage.exists(event.substr(5))) {
      ++openHandles;
      maximumOpenHandles = std::max(maximumOpenHandles, openHandles);
    } else if (event.rfind("close:", 0) == 0) {
      --openHandles;
    }
    EXPECT_GE(openHandles, 0);
  }
  EXPECT_EQ(openHandles, 0);
  EXPECT_EQ(maximumOpenHandles, 1);
}

TEST(PdfCachedProductState, AcceptsCanonicalRetainedJpegWithoutOpeningIt) {
  constexpr char jpeg[] = "images/0123456789abcdef-89abcdef-0011223344556677.jpg";
  ProductFixture fixture;
  fixture.initialize();
  GenerationOptions options;
  options.retainedImageLeaf = jpeg;
  fixture.addGeneration(7, 3, 'a', "Cached Book", "Cache Author", nullptr, options);
  const std::string jpegPath = fixture.fullPath(7, jpeg);
  const uint32_t opensBefore = fixture.storage.openCallsForPath(jpegPath);

  PdfCachedProductState state{};
  const auto loaded = pdfLoadCachedProductState(fixture.storage.io(), kSourcePath, kCacheDirectory, &state);

  ASSERT_TRUE(loaded.available());
  EXPECT_EQ(fixture.storage.openCallsForPath(jpegPath), opensBefore);
  EXPECT_STREQ(state.title, "Cached Book");
}

TEST(PdfCachedProductState, RejectsNoncanonicalRetainedJpegPath) {
  ProductFixture fixture;
  fixture.initialize();
  GenerationOptions options;
  options.retainedImageLeaf = "images/0123456789abcdef-89abcdef-001122334455667.jpg";
  fixture.addGeneration(7, 3, 'a', "Cached Book", "Cache Author", nullptr, options);

  PdfCachedProductState state{};
  const auto loaded = pdfLoadCachedProductState(fixture.storage.io(), kSourcePath, kCacheDirectory, &state);

  EXPECT_EQ(loaded.kind, PdfCachedProductStateKind::Corrupt);
  expectEmpty(state);
}

TEST(PdfCachedProductState, SelectsNewestMatchingCompletedManifestAcrossTwoSlots) {
  ProductFixture fixture;
  fixture.initialize();
  fixture.addGeneration(7, 3, 'a', "Older Cache");
  fixture.addGeneration(8, 4, 'b', "Newest Cache");
  fixture.saveProgress(1, 7);

  PdfCachedProductState state{};
  const auto result = pdfLoadCachedProductState(fixture.storage.io(), kSourcePath, kCacheDirectory, &state);

  ASSERT_TRUE(result.available());
  EXPECT_EQ(state.generation, 8U);
  EXPECT_STREQ(state.title, "Newest Cache");
  EXPECT_STREQ(state.coverPath, fixture.fullPath(8, "cover.bmp").c_str());
}

TEST(PdfCachedProductState, IgnoresNewerIncompleteOrStaleSlots) {
  ProductFixture incomplete;
  incomplete.initialize();
  incomplete.addGeneration(7, 3, 'a', "Completed Cache");
  GenerationOptions incompleteOptions;
  incompleteOptions.completed = false;
  incomplete.addGeneration(8, 4, 'b', "Incomplete Cache", "Cache Author", nullptr, incompleteOptions);
  PdfCachedProductState state{};
  ASSERT_TRUE(pdfLoadCachedProductState(incomplete.storage.io(), kSourcePath, kCacheDirectory, &state).available());
  EXPECT_EQ(state.generation, 7U);
  EXPECT_STREQ(state.title, "Completed Cache");

  ProductFixture stale;
  stale.initialize();
  stale.addGeneration(7, 3, 'a', "Matching Cache");
  PdfSourceIdentity oldIdentity = stale.identity;
  oldIdentity.tailFingerprint ^= 0x55aaU;
  stale.addGeneration(8, 4, 'b', "Stale Cache", "Cache Author", &oldIdentity);
  ASSERT_TRUE(pdfLoadCachedProductState(stale.storage.io(), kSourcePath, kCacheDirectory, &state).available());
  EXPECT_EQ(state.generation, 7U);
  EXPECT_STREQ(state.title, "Matching Cache");
}

TEST(PdfCachedProductState, FallsBackToOlderValidSlotWhenOtherSlotIsCorrupt) {
  ProductFixture fixture;
  fixture.initialize();
  fixture.addGeneration(7, 3, 'a', "Good Cache");
  fixture.addGeneration(8, 4, 'b', "Broken Cache");
  fixture.storage.corruptByte(fixture.cacheRoot + "/manifest.b", 8, 0x40);

  PdfCachedProductState state{};
  const auto result = pdfLoadCachedProductState(fixture.storage.io(), kSourcePath, kCacheDirectory, &state);

  ASSERT_TRUE(result.available());
  EXPECT_EQ(state.generation, 7U);
  EXPECT_STREQ(state.title, "Good Cache");
}

TEST(PdfCachedProductState, DistinguishesUncachedFromCorruptAndStale) {
  ProductFixture uncached;
  uncached.initialize();
  PdfCachedProductState state{};
  std::memcpy(state.title, "sentinel", 9);
  auto result = pdfLoadCachedProductState(uncached.storage.io(), kSourcePath, kCacheDirectory, &state);
  EXPECT_EQ(result.kind, PdfCachedProductStateKind::Missing);
  expectEmpty(state);

  ProductFixture corrupt;
  corrupt.initialize();
  corrupt.addGeneration(7, 3, 'a');
  corrupt.storage.corruptByte(corrupt.cacheRoot + "/manifest.a", 9, 0x80);
  std::memcpy(state.title, "sentinel", 9);
  result = pdfLoadCachedProductState(corrupt.storage.io(), kSourcePath, kCacheDirectory, &state);
  EXPECT_EQ(result.kind, PdfCachedProductStateKind::Corrupt);
  expectEmpty(state);

  ProductFixture stale;
  stale.initialize();
  PdfSourceIdentity oldIdentity = stale.identity;
  oldIdentity.headFingerprint ^= 0x1234U;
  stale.addGeneration(7, 3, 'a', "Stale Cache", "Old Author", &oldIdentity);
  std::memcpy(state.title, "sentinel", 9);
  result = pdfLoadCachedProductState(stale.storage.io(), kSourcePath, kCacheDirectory, &state);
  EXPECT_EQ(result.kind, PdfCachedProductStateKind::Stale);
  expectEmpty(state);
}

TEST(PdfCachedProductState, RejectsCorruptMinimalArtifactWithoutExposingPartialState) {
  ProductFixture fixture;
  fixture.initialize();
  fixture.addGeneration(7, 3, 'a');
  fixture.storage.corruptByte(fixture.fullPath(7, "cover.bmp"), 100, 0x01);

  PdfCachedProductState state{};
  std::memcpy(state.title, "sentinel", 9);
  const auto result = pdfLoadCachedProductState(fixture.storage.io(), kSourcePath, kCacheDirectory, &state);

  EXPECT_EQ(result.kind, PdfCachedProductStateKind::Corrupt);
  expectEmpty(state);
}

TEST(PdfCachedProductState, RejectsMissingMinimalArtifactAndMisorderedProductRecords) {
  ProductFixture missing;
  missing.initialize();
  missing.addGeneration(7, 3, 'a');
  PdfCacheIo missingIo = missing.storage.io();
  ASSERT_TRUE(missingIo.remove(missingIo.context, missing.fullPath(7, "cover.bmp").c_str(), false).ok());
  PdfCachedProductState state{};
  EXPECT_EQ(pdfLoadCachedProductState(missing.storage.io(), kSourcePath, kCacheDirectory, &state).kind,
            PdfCachedProductStateKind::Corrupt);
  expectEmpty(state);

  ProductFixture misordered;
  misordered.initialize();
  GenerationOptions misorderedOptions;
  misorderedOptions.misorderProductRecords = true;
  misordered.addGeneration(7, 3, 'a', "Cached Book", "Cache Author", nullptr, misorderedOptions);
  EXPECT_EQ(pdfLoadCachedProductState(misordered.storage.io(), kSourcePath, kCacheDirectory, &state).kind,
            PdfCachedProductStateKind::Corrupt);
  expectEmpty(state);
}

TEST(PdfCachedProductState, RejectsManifestWithZeroSectionRecords) {
  ProductFixture fixture;
  fixture.initialize();
  GenerationOptions options;
  options.omitSectionRecords = true;
  options.includeRetainedImage = false;
  fixture.addGeneration(7, 3, 'a', "Cached Book", "Cache Author", nullptr, options);
  PdfCachedProductState state{};

  EXPECT_EQ(pdfLoadCachedProductState(fixture.storage.io(), kSourcePath, kCacheDirectory, &state).kind,
            PdfCachedProductStateKind::Corrupt);
  expectEmpty(state);
}

TEST(PdfCachedProductState, RejectsUnknownRequiredFilePath) {
  ProductFixture fixture;
  fixture.initialize();
  GenerationOptions options;
  options.addUnknownRequiredPath = true;
  fixture.addGeneration(7, 3, 'a', "Cached Book", "Cache Author", nullptr, options);
  PdfCachedProductState state{};

  EXPECT_EQ(pdfLoadCachedProductState(fixture.storage.io(), kSourcePath, kCacheDirectory, &state).kind,
            PdfCachedProductStateKind::Corrupt);
  expectEmpty(state);
}

TEST(PdfCachedProductState, RejectsManifestAndMetadataSectionCountMismatch) {
  ProductFixture fixture;
  fixture.initialize();
  GenerationOptions options;
  options.metadataSectionCount = 1;
  fixture.addGeneration(7, 3, 'a', "Cached Book", "Cache Author", nullptr, options);
  PdfCachedProductState state{};

  EXPECT_EQ(pdfLoadCachedProductState(fixture.storage.io(), kSourcePath, kCacheDirectory, &state).kind,
            PdfCachedProductStateKind::Corrupt);
  expectEmpty(state);
}

TEST(PdfCachedProductState, RejectsRetainedImagesBeforeAnySection) {
  ProductFixture fixture;
  fixture.initialize();
  GenerationOptions options;
  options.omitSectionRecords = true;
  options.retainedImageBeforeSections = true;
  fixture.addGeneration(7, 3, 'a', "Cached Book", "Cache Author", nullptr, options);
  PdfCachedProductState state{};

  EXPECT_EQ(pdfLoadCachedProductState(fixture.storage.io(), kSourcePath, kCacheDirectory, &state).kind,
            PdfCachedProductStateKind::Corrupt);
  expectEmpty(state);
}

TEST(PdfCachedProductState, IgnoresCorruptOptionalProgressJournal) {
  ProductFixture fixture;
  fixture.initialize();
  fixture.addGeneration(7, 3, 'a');
  fixture.saveProgress(1, 7);
  fixture.storage.corruptByte(fixture.cacheRoot + "/progress.a", 12, 0x80);

  PdfCachedProductState state{};
  const auto result = pdfLoadCachedProductState(fixture.storage.io(), kSourcePath, kCacheDirectory, &state);

  ASSERT_TRUE(result.available());
  EXPECT_FALSE(state.hasProgress);
  EXPECT_EQ(state.currentWord, 0U);
  EXPECT_STREQ(state.currentChapter, "Opening");
}

void* denyAllocation(void*, size_t) { return nullptr; }
void ignoreRelease(void*, void*) {}

struct CountingAllocator {
  size_t allocations = 0;
  size_t releases = 0;
  size_t requestedBytes = 0;

  static void* allocate(void* context, const size_t size) {
    auto& self = *static_cast<CountingAllocator*>(context);
    ++self.allocations;
    self.requestedBytes = size;
    return ::operator new(size, std::nothrow);
  }

  static void release(void* context, void* allocation) {
    auto& self = *static_cast<CountingAllocator*>(context);
    ++self.releases;
    ::operator delete(allocation);
  }
};

TEST(PdfCachedProductState, UsesOneBoundedWorkspaceAllocation) {
  ProductFixture fixture;
  fixture.initialize();
  fixture.addGeneration(7, 3, 'a');
  CountingAllocator counting;
  const PdfCachedProductStateAllocator allocator{&counting, CountingAllocator::allocate, CountingAllocator::release};
  PdfCachedProductState state{};

  ASSERT_TRUE(
      pdfLoadCachedProductState(fixture.storage.io(), kSourcePath, kCacheDirectory, &state, allocator).available());

  EXPECT_EQ(counting.allocations, 1U);
  EXPECT_EQ(counting.releases, 1U);
  EXPECT_GT(counting.requestedBytes, PDF_SOURCE_FINGERPRINT_BYTES);
  EXPECT_LT(counting.requestedBytes, 8U * 1024U);
}

TEST(PdfCachedProductState, ReportsAllocationFailureBeforeOpeningAnyFile) {
  ProductFixture fixture;
  fixture.initialize();
  fixture.addGeneration(7, 3, 'a');
  const uint32_t opensBefore = fixture.storage.openCalls();
  PdfCachedProductState state{};
  std::memcpy(state.title, "sentinel", 9);
  const PdfCachedProductStateAllocator allocator{nullptr, denyAllocation, ignoreRelease};

  const auto result = pdfLoadCachedProductState(fixture.storage.io(), kSourcePath, kCacheDirectory, &state, allocator);

  EXPECT_EQ(result.kind, PdfCachedProductStateKind::Error);
  EXPECT_EQ(result.status.error, PdfError::InsufficientMemory);
  EXPECT_EQ(fixture.storage.openCalls(), opensBefore);
  expectEmpty(state);
}

TEST(PdfCachedProductState, RejectsPathsThatCannotFitBoundedCacheBuffers) {
  ProductFixture fixture;
  fixture.initialize();
  const std::string longCacheDirectory = "/" + std::string(PDF_CACHE_PATH_CAPACITY, 'c');
  PdfCachedProductState state{};

  const auto result = pdfLoadCachedProductState(fixture.storage.io(), kSourcePath, longCacheDirectory.c_str(), &state);

  EXPECT_EQ(result.kind, PdfCachedProductStateKind::Error);
  EXPECT_EQ(result.status.error, PdfError::LimitExceeded);
  expectEmpty(state);
}

}  // namespace
