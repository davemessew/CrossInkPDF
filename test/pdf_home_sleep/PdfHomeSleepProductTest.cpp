#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "BookRouteSpy.h"
#include "util/BookMoveUtils.h"
#include "CrossPointSettings.h"
#include "GfxRenderer.h"
#include "HalStorage.h"
#include "Memory.h"
#include "PdfCacheFormat.h"
#include "PdfCacheManifest.h"
#include "PdfCachedProductState.h"
#include "PdfHalCacheIo.h"
#include "PdfLayoutWordIndex.h"
#include "PdfMetadataStore.h"
#include "PdfOutline.h"
#include "PdfProgressStore.h"
#include "PdfSleepPageCache.h"
#include "PdfSourceIdentity.h"
#include "PdfTestCacheIo.h"
#include "PdfTestIo.h"
#include "RecentBookProgress.h"
#include "RecentBooksStore.h"
#include "SleepCoverAssets.h"

namespace {

constexpr char kSourcePath[] = "/books/integration.pdf";
constexpr char kCacheDirectory[] = "/.crosspoint";

struct PdfSleepFallbackProbe {
  uint32_t* operationCounter = nullptr;
  uint32_t calls = 0;
  uint32_t order = 0;

  static void load(void* const context) {
    auto& probe = *static_cast<PdfSleepFallbackProbe*>(context);
    ++probe.calls;
    probe.order = ++*probe.operationCounter;
  }
};

struct RequiredRecords {
  std::vector<PdfRequiredFileRecord> values;

  static PdfStatus read(void* context, uint32_t index, PdfRequiredFileRecord* output) {
    if (context == nullptr || output == nullptr) {
      return PdfStatus::failure(PdfError::InvalidArgument);
    }
    auto& self = *static_cast<RequiredRecords*>(context);
    if (index >= self.values.size()) {
      return PdfStatus::failure(PdfError::InvalidOffset, index);
    }
    *output = self.values[index];
    return PdfStatus::success();
  }

  PdfRequiredFileTableSource source() { return {this, static_cast<uint32_t>(values.size()), read}; }
};

struct MetadataSections {
  std::array<PdfMetadataSection, 2> values{};

  static PdfStatus read(void* context, uint16_t index, PdfMetadataSection* output) {
    if (context == nullptr || output == nullptr || index >= 2) {
      return PdfStatus::failure(PdfError::InvalidArgument);
    }
    *output = static_cast<MetadataSections*>(context)->values[index];
    return PdfStatus::success();
  }

  PdfMetadataSectionSource source() { return {this, 2, read}; }
};

struct OutlineEntries {
  std::array<PdfOutlineEntry, 2> values{};

  static PdfStatus read(void* context, uint16_t index, PdfOutlineEntry* output) {
    if (context == nullptr || output == nullptr || index >= 2) {
      return PdfStatus::failure(PdfError::InvalidArgument);
    }
    *output = static_cast<OutlineEntries*>(context)->values[index];
    return PdfStatus::success();
  }

  PdfOutlineEntrySource source() { return {this, 2, read}; }
};

void putU16(std::vector<uint8_t>& bytes, size_t offset, uint16_t value) {
  bytes[offset] = static_cast<uint8_t>(value);
  bytes[offset + 1] = static_cast<uint8_t>(value >> 8U);
}

void putU32(std::vector<uint8_t>& bytes, size_t offset, uint32_t value) {
  bytes[offset] = static_cast<uint8_t>(value);
  bytes[offset + 1] = static_cast<uint8_t>(value >> 8U);
  bytes[offset + 2] = static_cast<uint8_t>(value >> 16U);
  bytes[offset + 3] = static_cast<uint8_t>(value >> 24U);
}

uint32_t getU32(const std::vector<uint8_t>& bytes, const size_t offset) {
  return static_cast<uint32_t>(bytes[offset]) | (static_cast<uint32_t>(bytes[offset + 1]) << 8U) |
         (static_cast<uint32_t>(bytes[offset + 2]) << 16U) |
         (static_cast<uint32_t>(bytes[offset + 3]) << 24U);
}

void appendU8(std::vector<uint8_t>& bytes, const uint8_t value) { bytes.push_back(value); }

void appendU16(std::vector<uint8_t>& bytes, const uint16_t value) {
  bytes.push_back(static_cast<uint8_t>(value));
  bytes.push_back(static_cast<uint8_t>(value >> 8U));
}

void appendI16(std::vector<uint8_t>& bytes, const int16_t value) {
  appendU16(bytes, static_cast<uint16_t>(value));
}

void appendU32(std::vector<uint8_t>& bytes, const uint32_t value) {
  bytes.push_back(static_cast<uint8_t>(value));
  bytes.push_back(static_cast<uint8_t>(value >> 8U));
  bytes.push_back(static_cast<uint8_t>(value >> 16U));
  bytes.push_back(static_cast<uint8_t>(value >> 24U));
}

void appendTextBlock(std::vector<uint8_t>& bytes, const char* const text, const int16_t x,
                     const uint8_t style = 0) {
  const size_t textLength = std::strlen(text) + 1U;
  ASSERT_LE(textLength, static_cast<size_t>(UINT16_MAX));
  appendU16(bytes, 1);  // word count
  appendU8(bytes, 0);   // bionic
  appendU8(bytes, 0);   // guide dots
  appendU8(bytes, 0);   // word flags
  appendU16(bytes, static_cast<uint16_t>(textLength));
  appendU16(bytes, 0);  // text offset
  appendI16(bytes, x);
  appendU8(bytes, style);
  bytes.insert(bytes.end(), text, text + textLength);
  bytes.insert(bytes.end(), 23, 0);  // BlockStyle v44
}

std::vector<uint8_t> monochromeBmp(uint32_t width, uint32_t height, uint8_t fill) {
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
  bytes[58] = 0xff;
  bytes[59] = 0xff;
  bytes[60] = 0xff;
  return bytes;
}

std::vector<uint8_t> pdfSleepLayout(const PdfSleepPageLayout& layout, const std::array<uint8_t, 3>& pageMarkers) {
  constexpr size_t headerBytes = 44;
  constexpr size_t pageCount = 3;
  constexpr size_t serializedPageBytes = 13;
  constexpr uint32_t lutOffset = headerBytes + pageCount * serializedPageBytes;
  constexpr uint32_t anchorMapOffset = lutOffset + pageCount * sizeof(uint32_t);
  constexpr uint32_t paragraphLutOffset = anchorMapOffset + sizeof(uint16_t);
  constexpr uint32_t liLutOffset = paragraphLutOffset + sizeof(uint16_t) + pageCount * sizeof(uint16_t);
  std::vector<uint8_t> bytes(liLutOffset + pageCount * sizeof(uint16_t), 0);
  putU32(bytes, 0, 0x535843FF);
  bytes[4] = 44;
  putU32(bytes, 5, static_cast<uint32_t>(layout.fontId));
  bytes[25] = 2;  // EpubRenderMode::Light
  putU16(bytes, 16, layout.viewportWidth);
  putU16(bytes, 18, layout.viewportHeight);
  putU16(bytes, 26, static_cast<uint16_t>(pageMarkers.size()));
  putU32(bytes, 28, lutOffset);
  putU32(bytes, 32, anchorMapOffset);
  putU32(bytes, 36, paragraphLutOffset);
  putU32(bytes, 40, liLutOffset);
  for (size_t index = 0; index < pageMarkers.size(); ++index) {
    const size_t pageOffset = headerBytes + index * serializedPageBytes;
    bytes[pageOffset] = 1;                 // element count
    bytes[pageOffset + 2] = 4;             // TAG_PageHorizontalRule
    bytes[pageOffset + 3] = pageMarkers[index];
    bytes[pageOffset + 7] = 1;             // rule width
    bytes[pageOffset + 9] = 1;             // rule thickness
    bytes[pageOffset + 10] = 0;            // footnote count
    bytes[pageOffset + 11] = 0;
    bytes[pageOffset + 12] = 0;            // publisher marker count
    putU32(bytes, lutOffset + index * sizeof(uint32_t), static_cast<uint32_t>(pageOffset));
  }
  putU16(bytes, anchorMapOffset, 0);
  putU16(bytes, paragraphLutOffset, static_cast<uint16_t>(pageMarkers.size()));
  return bytes;
}

std::vector<uint8_t> pdfSleepLayoutWithPages(
    const PdfSleepPageLayout& layout, const std::array<std::vector<uint8_t>, 3>& pages) {
  constexpr uint32_t headerBytes = 44;
  constexpr uint16_t pageCount = 3;
  size_t pageBytes = 0;
  for (const auto& page : pages) {
    pageBytes += page.size();
  }
  const uint32_t lutOffset = headerBytes + static_cast<uint32_t>(pageBytes);
  const uint32_t anchorMapOffset = lutOffset + pageCount * sizeof(uint32_t);
  const uint32_t paragraphLutOffset = anchorMapOffset + sizeof(uint16_t);
  const uint32_t liLutOffset = paragraphLutOffset + sizeof(uint16_t) + pageCount * sizeof(uint16_t);
  std::vector<uint8_t> bytes(liLutOffset + pageCount * sizeof(uint16_t), 0);
  putU32(bytes, 0, 0x535843FF);
  bytes[4] = 44;
  putU32(bytes, 5, static_cast<uint32_t>(layout.fontId));
  bytes[25] = 2;
  putU16(bytes, 16, layout.viewportWidth);
  putU16(bytes, 18, layout.viewportHeight);
  putU16(bytes, 26, pageCount);
  putU32(bytes, 28, lutOffset);
  putU32(bytes, 32, anchorMapOffset);
  putU32(bytes, 36, paragraphLutOffset);
  putU32(bytes, 40, liLutOffset);
  uint32_t pageOffset = headerBytes;
  for (size_t index = 0; index < pages.size(); ++index) {
    putU32(bytes, lutOffset + index * sizeof(uint32_t), pageOffset);
    std::copy(pages[index].begin(), pages[index].end(), bytes.begin() + pageOffset);
    pageOffset += static_cast<uint32_t>(pages[index].size());
  }
  putU16(bytes, anchorMapOffset, 0);
  putU16(bytes, paragraphLutOffset, pageCount);
  return bytes;
}

std::vector<uint8_t> pdfSleepTextTableImageRulePage() {
  std::vector<uint8_t> page;
  appendU16(page, 4);

  appendU8(page, 1);  // PageLine
  appendI16(page, 5);
  appendI16(page, 7);
  appendTextBlock(page, "Hello", 3, EpdFontFamily::BOLD);

  appendU8(page, 3);  // PageTableFragment
  appendI16(page, 10);
  appendI16(page, 20);
  appendU16(page, 100);
  appendU8(page, 2);
  appendU8(page, 2);
  appendU16(page, 14);
  appendU8(page, 1);
  appendU16(page, 20);
  appendU8(page, 0);
  appendU8(page, 2);
  appendU8(page, 0);
  appendU8(page, 1);
  appendTextBlock(page, "A", 0);
  appendU8(page, 0);
  appendU8(page, 1);
  appendTextBlock(page, "B", 0);

  constexpr char imagePath[] = "/never.jpg";
  appendU8(page, 2);  // PageImage, intentionally skipped by sleep rendering
  appendI16(page, 1);
  appendI16(page, 2);
  appendU32(page, sizeof(imagePath) - 1U);
  page.insert(page.end(), imagePath, imagePath + sizeof(imagePath) - 1U);
  appendI16(page, 40);
  appendI16(page, 50);

  appendU8(page, 4);  // PageHorizontalRule
  appendI16(page, 6);
  appendI16(page, 60);
  appendU16(page, 30);
  appendU8(page, 2);

  appendU16(page, 0);
  appendU8(page, 0);
  return page;
}

std::vector<uint8_t> pdfSleepRulePage(const int16_t x = 0) {
  std::vector<uint8_t> page;
  appendU16(page, 1);
  appendU8(page, 4);
  appendI16(page, x);
  appendI16(page, 0);
  appendU16(page, 1);
  appendU8(page, 1);
  appendU16(page, 0);
  appendU8(page, 0);
  return page;
}

void appendEmptyWordsTextBlock(std::vector<uint8_t>& bytes, const uint16_t words) {
  appendU16(bytes, words);
  appendU8(bytes, 0);
  appendU8(bytes, 0);
  appendU8(bytes, 0);
  appendU16(bytes, words);
  for (uint16_t index = 0; index < words; ++index) {
    appendU16(bytes, index);
  }
  for (uint16_t index = 0; index < words; ++index) {
    appendI16(bytes, 0);
  }
  bytes.insert(bytes.end(), words, 0);  // styles
  bytes.insert(bytes.end(), words, 0);  // one NUL per empty word
  bytes.insert(bytes.end(), 23, 0);
}

std::vector<uint8_t> pdfSleepTooManyWordsPage() {
  std::vector<uint8_t> page;
  appendU16(page, 9);
  for (uint8_t line = 0; line < 9; ++line) {
    appendU8(page, 1);
    appendI16(page, 0);
    appendI16(page, line);
    appendEmptyWordsTextBlock(page, line < 8 ? 512 : 1);
  }
  appendU16(page, 0);
  appendU8(page, 0);
  return page;
}

std::vector<uint8_t> pdfSleepWordIndex(const uint32_t firstWordOrdinal = 4, const uint32_t wordCount = 6) {
  PdfTestByteSink bytes;
  PdfLayoutWordIndexWriter writer;
  EXPECT_TRUE(writer.begin(bytes.sink(), 1, firstWordOrdinal, wordCount).ok());
  for (uint16_t page = 0; page < 3; ++page) {
    PdfLayoutWordRange range{};
    range.firstGlobalWordOrdinal = firstWordOrdinal + page * 2;
    range.lastGlobalWordOrdinal = range.firstGlobalWordOrdinal + 1;
    range.wordCursor = range.lastGlobalWordOrdinal + 1;
    std::memcpy(range.blockAnchor, "b00000004", 9);
    range.valid = true;
    EXPECT_TRUE(writer.append(range).ok());
  }
  EXPECT_TRUE(writer.finish().ok());
  return bytes.bytes();
}

PdfOutlineEntry outlineEntry(const char* title, uint16_t section) {
  PdfOutlineEntry entry{};
  const size_t length = std::strlen(title);
  std::memcpy(entry.title, title, length);
  entry.titleLength = static_cast<uint8_t>(length);
  entry.parentIndex = -1;
  entry.sectionIndex = section;
  entry.level = 1;
  entry.anchorOrdinal = section == 0 ? 0 : 4;
  entry.sourcePageIndex = section;
  return entry;
}

struct SourceBudgetIo {
  PdfCacheIo base{};
  std::string sourcePath;
  bool sourceHandles[256]{};
  uint32_t sourceOpens = 0;
  uint32_t sourceReads = 0;
  uint32_t sourceCloses = 0;
  size_t sourceBytesRequested = 0;
  size_t maximumSourceRead = 0;

  static SourceBudgetIo& self(void* context) { return *static_cast<SourceBudgetIo*>(context); }

  static PdfStatus open(void* context, const char* path, PdfCacheOpenMode mode, PdfCacheHandle* handle) {
    auto& io = self(context);
    const PdfStatus status = io.base.open(io.base.context, path, mode, handle);
    if (status && path != nullptr && io.sourcePath == path) {
      ++io.sourceOpens;
      io.sourceHandles[handle->value] = true;
    }
    return status;
  }

  static PdfStatus read(void* context, PdfCacheHandle handle, uint64_t offset, uint8_t* destination, size_t requested,
                        size_t* bytesRead) {
    auto& io = self(context);
    if (io.sourceHandles[handle.value]) {
      ++io.sourceReads;
      io.sourceBytesRequested += requested;
      io.maximumSourceRead = std::max(io.maximumSourceRead, requested);
    }
    return io.base.read(io.base.context, handle, offset, destination, requested, bytesRead);
  }

  static PdfStatus write(void* context, PdfCacheHandle handle, const uint8_t* source, size_t requested,
                         size_t* bytesWritten) {
    auto& io = self(context);
    return io.base.write(io.base.context, handle, source, requested, bytesWritten);
  }

  static PdfStatus flush(void* context, PdfCacheHandle handle) {
    auto& io = self(context);
    return io.base.flush(io.base.context, handle);
  }

  static PdfStatus sync(void* context, PdfCacheHandle handle) {
    auto& io = self(context);
    return io.base.sync(io.base.context, handle);
  }

  static PdfStatus close(void* context, PdfCacheHandle* handle) {
    auto& io = self(context);
    const bool wasSource = handle != nullptr && handle->valid() && io.sourceHandles[handle->value];
    if (wasSource) {
      io.sourceHandles[handle->value] = false;
      ++io.sourceCloses;
    }
    return io.base.close(io.base.context, handle);
  }

  static PdfStatus remove(void* context, const char* path, bool recursive) {
    auto& io = self(context);
    return io.base.remove(io.base.context, path, recursive);
  }

  static PdfStatus mkdir(void* context, const char* path) {
    auto& io = self(context);
    return io.base.mkdir(io.base.context, path);
  }

  static PdfStatus list(void* context, const char* path, PdfCacheListVisitor visitor, void* visitorContext) {
    auto& io = self(context);
    return io.base.list(io.base.context, path, visitor, visitorContext);
  }

  static PdfStatus capacity(void* context, PdfCacheCapacity* capacity) {
    auto& io = self(context);
    return io.base.capacity(io.base.context, capacity);
  }

  static PdfStatus metadata(void* context, PdfCacheHandle handle, PdfCacheFileMetadata* metadata) {
    auto& io = self(context);
    return io.base.metadata(io.base.context, handle, metadata);
  }

  PdfCacheIo io() { return {this, open, read, write, flush, sync, close, remove, mkdir, list, capacity, metadata}; }
};

struct ProductFixture {
  PdfTestCacheIo storage;
  PdfSourceIdentity identity{};
  std::string cacheRoot;
  SourceBudgetIo budget;

  void initialize(bool addManifest = true, bool stale = false, const char* sourcePath = kSourcePath,
                  const uint64_t* cacheHashOverride = nullptr) {
    BookMoveUtils::TEST_MIGRATION_CACHE_HASH.reset();
    std::vector<uint8_t> source(9000);
    for (size_t index = 0; index < source.size(); ++index) {
      source[index] = static_cast<uint8_t>((index * 37U) & 0xffU);
    }
    storage.addFile(sourcePath, source, 42, true);
    std::array<uint8_t, PDF_SOURCE_FINGERPRINT_BYTES> workspace{};
    ASSERT_TRUE(
        pdfComputeSourceIdentity(storage.io(), sourcePath, workspace.data(), workspace.size(), &identity).ok());
    std::array<char, PDF_CACHE_PATH_CAPACITY> root{};
    const PdfStatus rootStatus =
        cacheHashOverride == nullptr
            ? pdfFormatCacheRoot(kCacheDirectory, sourcePath, root.data(), root.size())
            : pdfFormatCacheRootForHash(kCacheDirectory, *cacheHashOverride, root.data(), root.size());
    ASSERT_TRUE(rootStatus.ok());
    cacheRoot = root.data();
    if (addManifest) {
      addGeneration(stale);
      saveProgress();
    }
    storage.clearEvents();
    budget.base = storage.io();
    budget.sourcePath = sourcePath;
    pdfTestSetHalCacheIo(budget.io());
  }

  std::string fullPath(const char* leaf) const { return cacheRoot + "/gen_7/" + leaf; }

  PdfRequiredFileRecord addRequired(const char* leaf, const std::vector<uint8_t>& bytes) {
    const std::string relative = std::string("gen_7/") + leaf;
    storage.addFile(cacheRoot + "/" + relative, bytes);
    PdfRequiredFileRecord record{};
    std::memcpy(record.path, relative.data(), relative.size());
    record.pathLength = static_cast<uint8_t>(relative.size());
    record.size = bytes.size();
    record.crc32 = pdfCacheCrc32(bytes.data(), bytes.size());
    return record;
  }

  void addGeneration(bool stale) {
    RequiredRecords required;
    const auto section0 = addRequired("sections/000000.xhtml", {'o', 'n', 'e'});
    const auto section1 = addRequired("sections/000001.xhtml", {'t', 'w', 'o'});
    required.values.push_back(section0);
    required.values.push_back(section1);
    required.values.push_back(addRequired("images/0123456789abcdef-89abcdef.pxc", {1, 2, 3}));
    required.values.push_back(addRequired("cover.bmp", monochromeBmp(240, 400, 0xaa)));
    required.values.push_back(addRequired("thumb.bmp", monochromeBmp(96, 160, 0x55)));

    PdfMetadataBuilder builder;
    ASSERT_TRUE(builder.begin(reinterpret_cast<const uint8_t*>("integration.pdf"), 15).ok());
    ASSERT_TRUE(
        builder.setTitle(PdfMetadataOrigin::Xmp, reinterpret_cast<const uint8_t*>("Cached Integration"), 18).ok());
    ASSERT_TRUE(builder.setAuthor(PdfMetadataOrigin::Xmp, reinterpret_cast<const uint8_t*>("Cached Author"), 13).ok());
    PdfMetadata metadata = builder.metadata();
    metadata.sectionCount = 2;
    metadata.outlineCount = 2;
    metadata.totalWords = 10;
    MetadataSections sections;
    sections.values[0] = {
        .byteSize = 3,
        .cumulativeSize = 3,
        .firstWordOrdinal = 0,
        .wordCount = 4,
        .firstAnchorOrdinal = 0,
        .tocIndex = 0,
    };
    sections.values[1] = {
        .byteSize = 3,
        .cumulativeSize = 6,
        .firstWordOrdinal = 4,
        .wordCount = 6,
        .firstAnchorOrdinal = 4,
        .tocIndex = 1,
    };
    PdfTestByteSink metadataBytes;
    ASSERT_TRUE(pdfEncodeMetadata(metadata, sections.source(), metadataBytes.sink()).ok());
    required.values.push_back(addRequired("metadata.bin", metadataBytes.bytes()));

    OutlineEntries outline;
    outline.values = {outlineEntry("Opening", 0), outlineEntry("Deep Chapter", 1)};
    PdfTestByteSink outlineBytes;
    ASSERT_TRUE(pdfEncodeOutline(outline.source(), outlineBytes.sink()).ok());
    required.values.push_back(addRequired("outline.bin", outlineBytes.bytes()));

    PdfCacheManifest manifest{};
    manifest.sequence = 3;
    manifest.completed = true;
    manifest.source = identity;
    if (stale) {
      ++manifest.source.size;
    }
    manifest.generation = 7;
    manifest.totalWords = 10;
    manifest.requiredFileCount = static_cast<uint32_t>(required.values.size());
    manifest.requiredFileLedger = PDF_CACHE_FNV64_OFFSET;
    for (const auto& record : required.values) {
      manifest.requiredFileBytes += record.size;
      manifest.requiredFileLedger = pdfUpdateRequiredFileLedger(manifest.requiredFileLedger, record);
    }
    PdfTestByteSink manifestBytes;
    ASSERT_TRUE(pdfEncodeCacheManifest(manifest, required.source(), manifestBytes.sink()).ok());
    storage.addFile(cacheRoot + "/manifest.a", manifestBytes.bytes());
  }

  void saveProgress() {
    PdfProgressStore progress;
    ASSERT_TRUE(progress.initialize(storage.io(), cacheRoot.c_str(), identity, 10).ok());
    ReflowReadingPosition position{};
    position.sectionIndex = 1;
    position.pageNumber = 2;
    position.pageCount = 5;
    position.hasPageCount = true;
    position.hasSemanticPosition = true;
    position.hasWordCursor = true;
    position.globalWordOrdinal = 6;
    position.wordCursor = 7;
    std::memcpy(position.blockAnchor, "b00000004", 9);
    ASSERT_TRUE(progress.save(position).ok());
  }
};

TEST(PdfHomeProduct, HydratesCachedMetadataAndReusesOneBoundedIdentityPass) {
  ProductFixture fixture;
  fixture.initialize();
  RecentBookProgress::PdfProductCache cache;
  ASSERT_TRUE(cache.initialize());

  RecentBook book{kSourcePath, "Stored title", "Stored author", "/stale/cover.bmp"};
  float progress = -1.0f;
  std::string chapter;
  std::string fullCover;
  ASSERT_TRUE(RecentBookProgress::hydratePdfBook(cache, book, &progress, &chapter, &fullCover));

  EXPECT_EQ(book.title, "Cached Integration");
  EXPECT_EQ(book.author, "Cached Author");
  EXPECT_EQ(book.coverBmpPath, fixture.fullPath("thumb.bmp"));
  EXPECT_EQ(fullCover, fixture.fullPath("cover.bmp"));
  EXPECT_EQ(chapter, "Deep Chapter");
  EXPECT_FLOAT_EQ(progress, 70.0f);
  EXPECT_EQ(fixture.budget.sourceOpens, 1U);
  EXPECT_EQ(fixture.budget.sourceReads, 2U);
  EXPECT_EQ(fixture.budget.sourceCloses, 1U);
  EXPECT_EQ(fixture.budget.sourceBytesRequested, 2U * PDF_SOURCE_FINGERPRINT_BYTES);
  EXPECT_EQ(fixture.budget.maximumSourceRead, PDF_SOURCE_FINGERPRINT_BYTES);
  EXPECT_EQ(fixture.storage.openCallsForPath(fixture.fullPath("sections/000000.xhtml")), 0U);
  EXPECT_EQ(fixture.storage.openCallsForPath(fixture.fullPath("images/0123456789abcdef-89abcdef.pxc")), 0U);

  const uint32_t opensBeforeReuse = fixture.storage.openCalls();
  RecentBook second{kSourcePath, "wrong", "wrong", "wrong"};
  ASSERT_TRUE(RecentBookProgress::hydratePdfBook(cache, second, nullptr, nullptr, nullptr));
  EXPECT_EQ(second.title, book.title);
  EXPECT_EQ(second.coverBmpPath, book.coverBmpPath);
  EXPECT_EQ(fixture.storage.openCalls(), opensBeforeReuse);
  EXPECT_EQ(fixture.budget.sourceOpens, 1U);

  const uint32_t opensBeforeGetters = fixture.storage.openCalls();
  EXPECT_EQ(cache.cacheRoot(), fixture.cacheRoot);
  EXPECT_EQ(cache.currentSection(), 1U);
  EXPECT_EQ(cache.currentWord(), 7U);
  EXPECT_EQ(cache.totalWords(), 10U);
  EXPECT_EQ(cache.currentSectionFirstWordOrdinal(), 4U);
  EXPECT_EQ(cache.currentSectionWordCount(), 6U);
  EXPECT_EQ(fixture.storage.openCalls(), opensBeforeGetters);
}

TEST(PdfSleepProduct, LoadsCompletedProductsWithoutOpeningThePdfSource) {
  ProductFixture fixture;
  fixture.initialize();
  PdfSleepProductCache cache;

  ASSERT_TRUE(cache.load(kSourcePath));
  EXPECT_STREQ(cache.title(), "Cached Integration");
  EXPECT_STREQ(cache.author(), "Cached Author");
  EXPECT_STREQ(cache.chapter(), "Deep Chapter");
  EXPECT_EQ(cache.currentSection(), 1U);
  EXPECT_EQ(cache.currentWord(), 7U);
  EXPECT_EQ(cache.totalWords(), 10U);
  EXPECT_EQ(fixture.budget.sourceOpens, 0U);
  EXPECT_EQ(fixture.budget.sourceReads, 0U);
  EXPECT_EQ(fixture.budget.sourceCloses, 0U);
}

TEST(PdfSleepProduct, RejectsMissingCachedProductsWithoutOpeningThePdfSource) {
  ProductFixture fixture;
  fixture.initialize(false);
  PdfSleepProductCache cache;

  EXPECT_FALSE(cache.load(kSourcePath));
  EXPECT_EQ(fixture.budget.sourceOpens, 0U);
  EXPECT_EQ(fixture.budget.sourceReads, 0U);
  EXPECT_EQ(fixture.budget.sourceCloses, 0U);
}

TEST(PdfSleepProduct, RejectsCorruptCachedProductsWithoutOpeningThePdfSource) {
  ProductFixture fixture;
  fixture.initialize();
  fixture.storage.corruptByte(fixture.cacheRoot + "/manifest.a", 0, 0xff);
  PdfSleepProductCache cache;

  EXPECT_FALSE(cache.load(kSourcePath));
  EXPECT_EQ(fixture.budget.sourceOpens, 0U);
  EXPECT_EQ(fixture.budget.sourceReads, 0U);
  EXPECT_EQ(fixture.budget.sourceCloses, 0U);
}

TEST(PdfSleepProduct, LoadsCompletedProductsAfterThePdfSourceWasDeleted) {
  ProductFixture fixture;
  fixture.initialize();
  const PdfCacheIo storageIo = fixture.storage.io();
  ASSERT_TRUE(storageIo.remove(storageIo.context, kSourcePath, false).ok());
  ASSERT_FALSE(fixture.storage.exists(kSourcePath));
  PdfSleepProductCache cache;

  ASSERT_TRUE(cache.load(kSourcePath));
  EXPECT_STREQ(cache.title(), "Cached Integration");
  EXPECT_EQ(cache.currentWord(), 7U);
  EXPECT_EQ(fixture.budget.sourceOpens, 0U);
  EXPECT_EQ(fixture.budget.sourceReads, 0U);
  EXPECT_EQ(fixture.budget.sourceCloses, 0U);
}

TEST(PdfSleepProduct, BindsProgressToTheSelectedCompletedManifestSource) {
  ProductFixture fixture;
  fixture.initialize();
  const PdfCacheIo storageIo = fixture.storage.io();
  for (const char* const slot : {"/progress.a", "/progress.b"}) {
    const std::string path = fixture.cacheRoot + slot;
    if (fixture.storage.exists(path)) {
      ASSERT_TRUE(storageIo.remove(storageIo.context, path.c_str(), false).ok());
    }
  }
  ++fixture.identity.size;
  fixture.saveProgress();
  PdfSleepProductCache cache;

  ASSERT_TRUE(cache.load(kSourcePath));
  EXPECT_EQ(cache.currentWord(), 0U);
  EXPECT_FLOAT_EQ(cache.progressPercent(), 0.0f);
  EXPECT_EQ(fixture.budget.sourceOpens, 0U);
}

TEST(PdfHomeProduct, UsesStrictFallbackForMissingStaleAndCorruptProducts) {
  enum class Scenario { Missing, Stale, Corrupt };
  for (const Scenario scenario : {Scenario::Missing, Scenario::Stale, Scenario::Corrupt}) {
    ProductFixture fixture;
    fixture.initialize(scenario != Scenario::Missing, scenario == Scenario::Stale);
    if (scenario == Scenario::Corrupt) {
      fixture.storage.corruptByte(fixture.fullPath("metadata.bin"), 0, 0x01);
    }
    RecentBookProgress::PdfProductCache cache;
    ASSERT_TRUE(cache.initialize());
    RecentBook book{kSourcePath, "Stored title", "Stored author", "/stale/cover.bmp"};
    float progress = -1.0f;
    std::string chapter = "stale";
    std::string fullCover = "stale";

    ASSERT_TRUE(RecentBookProgress::hydratePdfBook(cache, book, &progress, &chapter, &fullCover));
    EXPECT_EQ(book.title, "integration.pdf");
    EXPECT_TRUE(book.author.empty());
    EXPECT_TRUE(book.coverBmpPath.empty());
    EXPECT_FLOAT_EQ(progress, 0.0f);
    EXPECT_TRUE(chapter.empty());
    EXPECT_TRUE(fullCover.empty());
    EXPECT_EQ(fixture.budget.sourceOpens, 1U);
  }
}

TEST(PdfHomeProduct, ReadsOldCacheOnlyDuringPreActivationAndReloadsWhenMigrationClears) {
  constexpr char OLD_PATH[] = "/books/old-name.pdf";
  constexpr char NEW_PATH[] = "/books/new-name.pdf";
  const uint64_t oldHash = pdfPathHash64(OLD_PATH, sizeof(OLD_PATH) - 1);
  ProductFixture fixture;
  fixture.initialize(true, false, NEW_PATH, &oldHash);
  BookMoveUtils::TEST_MIGRATION_CACHE_HASH.useNormalHash = false;
  BookMoveUtils::TEST_MIGRATION_CACHE_HASH.resolvedHash = oldHash;
  BookMoveUtils::TEST_MIGRATION_CACHE_HASH.readOnlyFallback = true;
  RecentBookProgress::PdfProductCache cache;
  ASSERT_TRUE(cache.initialize());

  RecentBook migrated{NEW_PATH, "Stored title", "Stored author", "/stored/cover.bmp"};
  float progress = -1.0f;
  std::string chapter;
  std::string fullCover;
  ASSERT_TRUE(RecentBookProgress::hydratePdfBook(cache, migrated, &progress, &chapter, &fullCover));
  EXPECT_EQ(migrated.title, "Cached Integration");
  EXPECT_EQ(migrated.author, "Cached Author");
  EXPECT_EQ(migrated.coverBmpPath, fixture.fullPath("thumb.bmp"));
  EXPECT_FLOAT_EQ(progress, 70.0f);
  EXPECT_EQ(cache.cacheRoot(), fixture.cacheRoot);
  EXPECT_EQ(BookMoveUtils::TEST_MIGRATION_CACHE_HASH.calls, 1U);
  EXPECT_EQ(fixture.budget.sourceOpens, 1U);

  BookMoveUtils::TEST_MIGRATION_CACHE_HASH.useNormalHash = true;
  BookMoveUtils::TEST_MIGRATION_CACHE_HASH.readOnlyFallback = false;
  RecentBook activated{NEW_PATH, "Stale title", "Stale author", "/stale/cover.bmp"};
  ASSERT_TRUE(RecentBookProgress::hydratePdfBook(cache, activated, &progress, &chapter, &fullCover));
  EXPECT_EQ(activated.title, "new-name.pdf");
  EXPECT_TRUE(activated.author.empty());
  EXPECT_TRUE(activated.coverBmpPath.empty());
  EXPECT_FLOAT_EQ(progress, 0.0f);
  EXPECT_TRUE(chapter.empty());
  EXPECT_TRUE(fullCover.empty());
  EXPECT_EQ(BookMoveUtils::TEST_MIGRATION_CACHE_HASH.calls, 2U);
  EXPECT_EQ(fixture.budget.sourceOpens, 2U);
}

TEST(PdfHomeProduct, PreservesStoredDisplayWhenMigrationResolutionIsUnsafe) {
  ProductFixture fixture;
  fixture.initialize();
  BookMoveUtils::TEST_MIGRATION_CACHE_HASH.succeeds = false;
  BookMoveUtils::TEST_MIGRATION_CACHE_HASH.readOnlyFallback = true;
  RecentBookProgress::PdfProductCache cache;
  ASSERT_TRUE(cache.initialize());
  RecentBook book{kSourcePath, "Stored title", "Stored author", "/stored/cover.bmp"};
  float progress = 81.0f;
  std::string chapter = "stale";
  std::string fullCover = "stale";
  const uint32_t opensBeforeHydration = fixture.storage.openCalls();

  ASSERT_TRUE(RecentBookProgress::hydratePdfBook(cache, book, &progress, &chapter, &fullCover));
  EXPECT_EQ(book.title, "Stored title");
  EXPECT_EQ(book.author, "Stored author");
  EXPECT_EQ(book.coverBmpPath, "/stored/cover.bmp");
  EXPECT_FLOAT_EQ(progress, -1.0f);
  EXPECT_TRUE(chapter.empty());
  EXPECT_TRUE(fullCover.empty());
  EXPECT_EQ(BookMoveUtils::TEST_MIGRATION_CACHE_HASH.calls, 1U);
  EXPECT_EQ(fixture.budget.sourceOpens, 0U);
  EXPECT_EQ(fixture.storage.openCalls(), opensBeforeHydration);
}

TEST(PdfHomeProduct, PreservesStoredDisplayWhenReadOnlyMigrationCacheIsMissing) {
  constexpr char OLD_PATH[] = "/books/old-missing.pdf";
  constexpr char NEW_PATH[] = "/books/new-missing.pdf";
  const uint64_t oldHash = pdfPathHash64(OLD_PATH, sizeof(OLD_PATH) - 1);
  ProductFixture fixture;
  fixture.initialize(false, false, NEW_PATH, &oldHash);
  BookMoveUtils::TEST_MIGRATION_CACHE_HASH.useNormalHash = false;
  BookMoveUtils::TEST_MIGRATION_CACHE_HASH.resolvedHash = oldHash;
  BookMoveUtils::TEST_MIGRATION_CACHE_HASH.readOnlyFallback = true;
  RecentBookProgress::PdfProductCache cache;
  ASSERT_TRUE(cache.initialize());
  RecentBook book{NEW_PATH, "Stored title", "Stored author", "/stored/cover.bmp"};
  float progress = 81.0f;
  std::string chapter = "stale";
  std::string fullCover = "stale";

  ASSERT_TRUE(RecentBookProgress::hydratePdfBook(cache, book, &progress, &chapter, &fullCover));
  EXPECT_EQ(book.title, "Stored title");
  EXPECT_EQ(book.author, "Stored author");
  EXPECT_EQ(book.coverBmpPath, "/stored/cover.bmp");
  EXPECT_FLOAT_EQ(progress, -1.0f);
  EXPECT_TRUE(chapter.empty());
  EXPECT_TRUE(fullCover.empty());
  EXPECT_EQ(fixture.budget.sourceOpens, 1U);
}

TEST(PdfHomeProduct, LeavesEpubXtcTxtAndMarkdownProgressRoutesUnchanged) {
  BOOK_ROUTE_SPY.reset();
  Storage.reset();

  RecentBook epub{"/books/legacy.epub", "", "", ""};
  EXPECT_FLOAT_EQ(RecentBookProgress::loadPercent(epub), 42.0f);
  EXPECT_EQ(BOOK_ROUTE_SPY.epubLoads, 1);
  EXPECT_EQ(BOOK_ROUTE_SPY.epubProgressLoads, 1);

  Storage.addFile("/xtc/progress.bin", {4, 0, 0, 0});
  RecentBook xtc{"/books/legacy.xtc", "", "", ""};
  EXPECT_FLOAT_EQ(RecentBookProgress::loadPercent(xtc), 37.0f);
  EXPECT_EQ(BOOK_ROUTE_SPY.xtcLoads, 1);

  std::vector<uint8_t> index;
  const auto append = [&index](const auto value) {
    const auto* bytes = reinterpret_cast<const uint8_t*>(&value);
    index.insert(index.end(), bytes, bytes + sizeof(value));
  };
  const uint32_t magic = 0x54585449;
  const uint8_t version = 3;
  const uint32_t fileSize = 1234;
  const int32_t zero = 0;
  const uint8_t alignment = 0;
  const uint32_t pages = 8;
  append(magic);
  append(version);
  append(fileSize);
  append(zero);
  append(zero);
  append(zero);
  append(zero);
  append(alignment);
  append(pages);
  Storage.addFile("/txt/progress.bin", {3, 0, 0, 0});
  Storage.addFile("/txt/index.bin", index);
  RecentBook txt{"/books/legacy.txt", "", "", ""};
  RecentBook markdown{"/books/legacy.md", "", "", ""};
  EXPECT_FLOAT_EQ(RecentBookProgress::loadPercent(txt), 50.0f);
  EXPECT_FLOAT_EQ(RecentBookProgress::loadPercent(markdown), 50.0f);
  EXPECT_EQ(BOOK_ROUTE_SPY.txtLoads, 2);

  RecentBook untouched{"/books/not-a-pdf.epub", "Title", "Author", "cover"};
  RecentBookProgress::PdfProductCache cache;
  ASSERT_TRUE(cache.initialize());
  EXPECT_FALSE(RecentBookProgress::hydratePdfBook(cache, untouched, nullptr, nullptr, nullptr));
  EXPECT_EQ(untouched.title, "Title");
  EXPECT_EQ(untouched.author, "Author");
  EXPECT_EQ(untouched.coverBmpPath, "cover");
}

TEST(PdfSleepProduct, UsesOnlyValidatedCachedPdfCoversAndNeverPreparesPdf) {
  ProductFixture fixture;
  fixture.initialize();
  PdfSleepProductCache cache;
  ASSERT_TRUE(cache.load(kSourcePath));

  BOOK_ROUTE_SPY.reset();
  EXPECT_EQ(cache.coverPath(), fixture.fullPath("cover.bmp"));
  EXPECT_EQ(cache.thumbnailPath(), fixture.fullPath("thumb.bmp"));
  EXPECT_FALSE(SleepCoverAssets::prepareFullCoverForPath(kSourcePath, false));
  EXPECT_FALSE(SleepCoverAssets::prepareMinimalCoverForPath(kSourcePath));
  EXPECT_FALSE(SleepCoverAssets::prepareDashboardCoverForPath(kSourcePath));
  EXPECT_EQ(BOOK_ROUTE_SPY.epubLoads, 0);
  EXPECT_EQ(BOOK_ROUTE_SPY.xtcLoads, 0);
  EXPECT_EQ(BOOK_ROUTE_SPY.txtLoads, 0);
  EXPECT_EQ(fixture.budget.sourceOpens, 0U);
}

TEST(PdfSleepProduct, RetainsExistingEpubCoverPreparationRoute) {
  BOOK_ROUTE_SPY.reset();
  Storage.reset();
  SETTINGS.sleepScreen = CrossPointSettings::SLEEP_SCREEN_MODE::COVER;

  EXPECT_TRUE(SleepCoverAssets::prepareFullCoverForPath("/books/legacy.epub", false));
  EXPECT_EQ(BOOK_ROUTE_SPY.epubLoads, 1);
  EXPECT_EQ(BOOK_ROUTE_SPY.epubCoverGenerations, 1);
  EXPECT_EQ(BOOK_ROUTE_SPY.xtcLoads, 0);
  EXPECT_EQ(BOOK_ROUTE_SPY.txtLoads, 0);
}

TEST(PdfSleepProduct, LoadsOneValidatedTextPageWithoutSourceXhtmlOrImageIoAndReleasesBeforeOverlay) {
  ProductFixture fixture;
  fixture.initialize();
  PdfSleepProductCache product;
  ASSERT_TRUE(product.load(kSourcePath));

  PdfSleepPageLayout layout{};
  layout.fontId = 23;
  layout.marginLeft = 19;
  layout.marginTop = 27;
  layout.viewportWidth = 442;
  layout.viewportHeight = 731;
  layout.orientation = static_cast<uint8_t>(GfxRenderer::LandscapeClockwise);
  layout.backgroundColor = 0xff;
  layout.foregroundBlack = true;
  layout.valid = true;

  const std::string sectionPath = fixture.cacheRoot + "/sections/1_light.bin";
  const std::string wordIndexPath = sectionPath + ".pwi";
  fixture.storage.addFile(wordIndexPath, pdfSleepWordIndex());
  Storage.reset();
  auto persistedLayout = pdfSleepLayout(layout, {0xa0, 0xb1, 0xc2});
  putU32(persistedLayout, 5, 31);  // Actual bounded fallback font retained in the section header.
  Storage.addFile(sectionPath, persistedLayout);
  const uint32_t sourceOpensBefore = fixture.budget.sourceOpens;
  const uint32_t cacheOpensBefore = fixture.storage.openCalls();
  PdfSleepPageCache page;
  ASSERT_TRUE(page.load(product, layout));
  EXPECT_TRUE(page.available());
  EXPECT_EQ(fixture.storage.openCallsForPath(wordIndexPath), 1U);
  EXPECT_EQ(Storage.openCallsForPath(sectionPath), 1U);
  EXPECT_EQ(Storage.closeCalls(), 1U);
  EXPECT_EQ(Storage.activeReaders(), 0U);
  EXPECT_EQ(Storage.maximumActiveReaders(), 1U);
  EXPECT_EQ(fixture.budget.sourceOpens, sourceOpensBefore);
  EXPECT_EQ(fixture.budget.sourceOpens, 0U);
  EXPECT_EQ(fixture.budget.sourceReads, 0U);
  EXPECT_EQ(fixture.budget.sourceCloses, 0U);
  EXPECT_EQ(fixture.storage.openCallsForPath(fixture.fullPath("sections/000000.xhtml")), 0U);
  EXPECT_EQ(fixture.storage.openCallsForPath(fixture.fullPath("images/0123456789abcdef-89abcdef.pxc")), 0U);
  EXPECT_EQ(fixture.storage.openCalls(), cacheOpensBefore + 1U);

  GfxRenderer renderer;
  ASSERT_TRUE(page.renderTextAndRelease(renderer));
  EXPECT_FALSE(page.available());
  EXPECT_EQ(renderer.drawLineCalls, 1U);
  EXPECT_EQ(renderer.lastLineX1, layout.marginLeft + 0xb1);
  EXPECT_EQ(renderer.lastLineY1, layout.marginTop);
  EXPECT_EQ(renderer.lastLineX2, layout.marginLeft + 0xb1);
  EXPECT_EQ(renderer.lastLineY2, layout.marginTop);
  EXPECT_EQ(renderer.lastLineWidth, 1);
  EXPECT_TRUE(renderer.lastLineBlack);
  EXPECT_EQ(renderer.getOrientation(), GfxRenderer::LandscapeClockwise);
  EXPECT_EQ(renderer.clearCalls, 1U);
  EXPECT_EQ(renderer.lastClearColor, 0xff);
  EXPECT_FALSE(page.renderTextAndRelease(renderer));
}

TEST(PdfSleepProduct, RendersProductionTextTableAndRuleBytesAndSkipsSerializedImages) {
  ProductFixture fixture;
  fixture.initialize();
  PdfSleepProductCache product;
  ASSERT_TRUE(product.load(kSourcePath));

  PdfSleepPageLayout layout{};
  layout.fontId = 23;
  layout.marginLeft = 19;
  layout.marginTop = 27;
  layout.viewportWidth = 442;
  layout.viewportHeight = 731;
  layout.orientation = static_cast<uint8_t>(GfxRenderer::Portrait);
  layout.backgroundColor = 0xff;
  layout.foregroundBlack = true;
  layout.valid = true;

  const std::string sectionPath = fixture.cacheRoot + "/sections/1_light.bin";
  fixture.storage.addFile(sectionPath + ".pwi", pdfSleepWordIndex());
  const auto pageBytes = pdfSleepTextTableImageRulePage();
  auto persistedLayout =
      pdfSleepLayoutWithPages(layout, std::array<std::vector<uint8_t>, 3>{pageBytes, pageBytes, pageBytes});
  putU32(persistedLayout, 5, 31);
  Storage.reset();
  Storage.addFile(sectionPath, persistedLayout);

  PdfSleepPageCache page;
  ASSERT_TRUE(page.load(product, layout));
  EXPECT_EQ(Storage.activeReaders(), 0U);

  GfxRenderer renderer;
  ASSERT_TRUE(page.renderTextAndRelease(renderer));
  EXPECT_FALSE(page.available());
  EXPECT_EQ(renderer.drawTextCalls, 3U);
  EXPECT_STREQ(renderer.lastText, "B");
  EXPECT_EQ(renderer.lastFontId, 31);
  EXPECT_EQ(renderer.lastTextX, layout.marginLeft + 10 + 50 + 2);
  EXPECT_EQ(renderer.lastTextY, layout.marginTop + 20 + 2);
  EXPECT_EQ(renderer.drawRectCalls, 1U);
  EXPECT_EQ(renderer.drawLineCalls, 2U);  // one table column plus one horizontal rule
  EXPECT_EQ(renderer.lastLineX1, layout.marginLeft + 6);
  EXPECT_EQ(renderer.lastLineY1, layout.marginTop + 60);
  EXPECT_EQ(renderer.lastLineX2, layout.marginLeft + 6 + 29);
  EXPECT_EQ(renderer.lastLineWidth, 2);
  EXPECT_EQ(Storage.openCallsForPath("/never.jpg"), 0U);
}

TEST(PdfSleepProduct, SnapshotsBeforeFallbackAndSkipsEveryFallbackReadOnSuccess) {
  uint32_t operationCounter = 0;
  GfxRenderer renderer;
  renderer.operationCounter = &operationCounter;
  renderer.storeBwBufferResult = true;
  PdfSleepFallbackProbe fallback{&operationCounter};

  EXPECT_TRUE(pdfSnapshotBeforeFallback(renderer, {&fallback, PdfSleepFallbackProbe::load}));
  EXPECT_EQ(renderer.storeBwBufferCalls, 1U);
  EXPECT_EQ(renderer.storeBwBufferOrder, 1U);
  EXPECT_EQ(fallback.calls, 0U);

  operationCounter = 0;
  renderer.storeBwBufferOrder = 0;
  renderer.storeBwBufferResult = false;
  EXPECT_FALSE(pdfSnapshotBeforeFallback(renderer, {&fallback, PdfSleepFallbackProbe::load}));
  EXPECT_EQ(renderer.storeBwBufferCalls, 2U);
  EXPECT_EQ(renderer.storeBwBufferOrder, 1U);
  EXPECT_EQ(fallback.calls, 1U);
  EXPECT_EQ(fallback.order, 2U);
}

TEST(PdfSleepProduct, RejectsMissingCorruptMismatchedCrossingOversizedAndOomLayoutState) {
  enum class Scenario : uint8_t {
    MissingIndex,
    CorruptIndex,
    MismatchedWordRange,
    StaleViewport,
    CrossingDeclaredImagePath,
    OversizedPage,
    AllocatorFailure,
    CorruptTextOffsets,
    GlobalWordBudget
  };
  for (const Scenario scenario :
       {Scenario::MissingIndex, Scenario::CorruptIndex, Scenario::StaleViewport, Scenario::MismatchedWordRange,
        Scenario::CrossingDeclaredImagePath, Scenario::OversizedPage, Scenario::AllocatorFailure,
        Scenario::CorruptTextOffsets, Scenario::GlobalWordBudget}) {
    SCOPED_TRACE(static_cast<unsigned>(scenario));
    ProductFixture fixture;
    fixture.initialize();
    PdfSleepProductCache product;
    ASSERT_TRUE(product.load(kSourcePath));

    PdfSleepPageLayout layout{};
    layout.fontId = 23;
    layout.marginLeft = 19;
    layout.marginTop = 27;
    layout.viewportWidth = 442;
    layout.viewportHeight = 731;
    layout.orientation = static_cast<uint8_t>(GfxRenderer::Portrait);
    layout.backgroundColor = 0xff;
    layout.foregroundBlack = true;
    layout.valid = true;
    const std::string sectionPath = fixture.cacheRoot + "/sections/1_light.bin";
    const std::string wordIndexPath = sectionPath + ".pwi";
    if (scenario != Scenario::MissingIndex) {
      auto index = scenario == Scenario::MismatchedWordRange ? pdfSleepWordIndex(3, 6) : pdfSleepWordIndex();
      if (scenario == Scenario::CorruptIndex) {
        index[PDF_LAYOUT_WORD_INDEX_HEADER_BYTES + 3] ^= 0x80;
      }
      fixture.storage.addFile(wordIndexPath, index);
    }

    auto section = pdfSleepLayout(layout, {0xa0, 0xb1, 0xc2});
    if (scenario == Scenario::CorruptTextOffsets) {
      const auto complexPage = pdfSleepTextTableImageRulePage();
      section = pdfSleepLayoutWithPages(
          layout, std::array<std::vector<uint8_t>, 3>{pdfSleepRulePage(), complexPage, pdfSleepRulePage()});
      const uint32_t lutOffset = getU32(section, 28);
      const uint32_t pagePosition = getU32(section, lutOffset + sizeof(uint32_t));
      putU16(section, pagePosition + 14U, 1);  // first TextBlock offset must be zero
    } else if (scenario == Scenario::GlobalWordBudget) {
      section = pdfSleepLayoutWithPages(
          layout, std::array<std::vector<uint8_t>, 3>{
                      pdfSleepRulePage(), pdfSleepTooManyWordsPage(), pdfSleepRulePage()});
    }
    if (scenario == Scenario::StaleViewport) {
      putU16(section, 16, layout.viewportWidth + 1);
    } else if (scenario == Scenario::CrossingDeclaredImagePath) {
      const uint32_t lutOffset = getU32(section, 28);
      const uint32_t pagePosition = getU32(section, lutOffset + sizeof(uint32_t));
      const uint32_t nextPagePosition = getU32(section, lutOffset + 2 * sizeof(uint32_t));
      std::fill(section.begin() + pagePosition, section.begin() + nextPagePosition, 0);
      section[pagePosition] = 1;      // one element
      section[pagePosition + 2] = 2;  // TAG_PageImage
      putU32(section, pagePosition + 7, 32);  // Declared path crosses nextPagePosition.
    } else if (scenario == Scenario::OversizedPage) {
      constexpr size_t oversizedBytes = PdfSleepPageCache::MAX_SERIALIZED_PAGE_BYTES + 1;
      const uint32_t oldLut = getU32(section, 28);
      const uint32_t oldAnchor = getU32(section, 32);
      const uint32_t oldParagraph = getU32(section, 36);
      const uint32_t oldLi = getU32(section, 40);
      const uint32_t page0 = getU32(section, oldLut);
      const uint32_t page1 = getU32(section, oldLut + sizeof(uint32_t));
      const uint32_t page2 = getU32(section, oldLut + 2 * sizeof(uint32_t));
      section.insert(section.begin() + page2, oversizedBytes, 0xee);
      const uint32_t shiftedLut = static_cast<uint32_t>(oldLut + oversizedBytes);
      putU32(section, 28, shiftedLut);
      putU32(section, 32, oldAnchor + oversizedBytes);
      putU32(section, 36, oldParagraph + oversizedBytes);
      putU32(section, 40, oldLi + oversizedBytes);
      putU32(section, shiftedLut, page0);
      putU32(section, shiftedLut + sizeof(uint32_t), page1);
      putU32(section, shiftedLut + 2 * sizeof(uint32_t), page2 + oversizedBytes);
    }
    Storage.reset();
    Storage.addFile(sectionPath, section);
    MemoryTestHooks::reset();
    if (scenario == Scenario::AllocatorFailure) {
      MemoryTestHooks::failNextUint8Array(13);
    }

    const uint32_t sourceOpensBefore = fixture.budget.sourceOpens;
    PdfSleepPageCache page;
    EXPECT_FALSE(page.load(product, layout));
    EXPECT_FALSE(page.available());
    EXPECT_EQ(Storage.activeReaders(), 0U);
    EXPECT_EQ(fixture.budget.sourceOpens, sourceOpensBefore);
    EXPECT_EQ(fixture.storage.openCallsForPath(fixture.fullPath("sections/000000.xhtml")), 0U);
    EXPECT_EQ(fixture.storage.openCallsForPath(fixture.fullPath("images/0123456789abcdef-89abcdef.pxc")), 0U);
    if (scenario == Scenario::AllocatorFailure) {
      EXPECT_EQ(MemoryTestHooks::failedUint8ArrayAllocations, 1U);
    }
    MemoryTestHooks::reset();
  }
}

}  // namespace
