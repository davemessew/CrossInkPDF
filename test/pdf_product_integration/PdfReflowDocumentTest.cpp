#include <Print.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <string>
#include <vector>

#include "PdfCacheStore.h"
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
  EXPECT_EQ(document.getCapabilities(), 0U);
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
  EXPECT_FALSE(document.saveReadingPosition(position));
  EXPECT_EQ(fixture.storage.openCallsForPath(kSourcePath), sourceOpensBefore + 1);
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

}  // namespace
