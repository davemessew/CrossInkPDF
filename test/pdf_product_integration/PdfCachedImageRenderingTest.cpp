#include <PixelCache.h>
#include <Print.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
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

constexpr char kImageSourcePath[] = "/books/images.pdf";
constexpr char kImageCacheDirectory[] = "/.crosspoint";
constexpr uint32_t kGeneration = 7;
constexpr char kPixelLeaf[] = "0123456789abcdef-89abcdef.pxc";

struct RequiredFileRecords {
  std::vector<PdfRequiredFileRecord> values;

  static PdfStatus read(void* const context, const uint32_t index, PdfRequiredFileRecord* const output) {
    if (context == nullptr || output == nullptr) {
      return PdfStatus::failure(PdfError::InvalidArgument);
    }
    const auto& self = *static_cast<RequiredFileRecords*>(context);
    if (index >= self.values.size()) {
      return PdfStatus::failure(PdfError::InvalidOffset, index);
    }
    *output = self.values[index];
    return PdfStatus::success();
  }

  PdfRequiredFileTableSource source() { return {this, static_cast<uint32_t>(values.size()), read}; }
};

struct MetadataSectionSource {
  PdfMetadataSection value{};

  static PdfStatus read(void* const context, const uint16_t index, PdfMetadataSection* const output) {
    if (context == nullptr || output == nullptr || index != 0) {
      return PdfStatus::failure(PdfError::InvalidArgument, index);
    }
    *output = static_cast<MetadataSectionSource*>(context)->value;
    return PdfStatus::success();
  }
};

struct OutlineEntrySource {
  PdfOutlineEntry value{};

  static PdfStatus read(void* const context, const uint16_t index, PdfOutlineEntry* const output) {
    if (context == nullptr || output == nullptr || index != 0) {
      return PdfStatus::failure(PdfError::InvalidArgument, index);
    }
    *output = static_cast<OutlineEntrySource*>(context)->value;
    return PdfStatus::success();
  }
};

struct ResourceSpec {
  std::string leaf;
  std::vector<uint8_t> bytes;
};

std::vector<uint8_t> minimalJpeg(const uint16_t width = 3, const uint16_t height = 2) {
  return {
      0xff, 0xd8,                    // SOI
      0xff, 0xe0, 0x00, 0x04, 0, 0,  // APP0
      0xff, 0xc0, 0x00, 0x0b,        // baseline SOF, eleven-byte segment
      0x08,
      static_cast<uint8_t>(height >> 8U),
      static_cast<uint8_t>(height),
      static_cast<uint8_t>(width >> 8U),
      static_cast<uint8_t>(width),
      0x01, 0x01, 0x11, 0x00,
      0xff, 0xd9,  // EOI
  };
}

std::string jpegLeaf(const std::vector<uint8_t>& bytes) {
  char leaf[64]{};
  const int written =
      std::snprintf(leaf, sizeof(leaf), "%016llx-%08lx-%016llx.jpg",
                    static_cast<unsigned long long>(pdfCacheFnv64(bytes.data(), bytes.size())),
                    static_cast<unsigned long>(pdfCacheCrc32(bytes.data(), bytes.size())),
                    static_cast<unsigned long long>(bytes.size()));
  EXPECT_GT(written, 0);
  EXPECT_LT(static_cast<size_t>(written), sizeof(leaf));
  return leaf;
}

class ImageCacheFixture {
 public:
  void build(const std::vector<ResourceSpec>& resources) {
    const std::string source = "%PDF-cached-image-rendering-identity";
    storage.addFile(kImageSourcePath, std::vector<uint8_t>(source.begin(), source.end()), 42, true);
    std::array<uint8_t, PDF_SOURCE_FINGERPRINT_BYTES> workspace{};
    ASSERT_TRUE(pdfComputeSourceIdentity(storage.io(), kImageSourcePath, workspace.data(), workspace.size(), &identity)
                    .ok());

    std::array<char, PDF_CACHE_PATH_CAPACITY> root{};
    ASSERT_TRUE(
        pdfFormatCacheRoot(kImageCacheDirectory, kImageSourcePath, root.data(), root.size()).ok());
    cacheRoot = root.data();

    PdfCacheStore cache;
    ASSERT_TRUE(cache.initialize(storage.io(), cacheRoot.c_str()).ok());
    ASSERT_TRUE(cache.ensureGeneration(kGeneration).ok());

    const std::string sectionText = buildSection(resources);
    sectionPath = cacheRoot + "/gen_7/sections/000000.xhtml";
    addRequired("gen_7/sections/000000.xhtml",
                std::vector<uint8_t>(sectionText.begin(), sectionText.end()));

    for (const ResourceSpec& resource : resources) {
      const std::string relative = "gen_7/images/" + resource.leaf;
      imagePaths.push_back(cacheRoot + "/" + relative);
      addRequired(relative, resource.bytes);
    }

    coverPath = cacheRoot + "/gen_7/cover.bmp";
    thumbnailPath = cacheRoot + "/gen_7/thumb.bmp";
    addRequired("gen_7/cover.bmp", {'B', 'M', 'c', 'o', 'v', 'e', 'r'});
    addRequired("gen_7/thumb.bmp", {'B', 'M', 't', 'h', 'u', 'm', 'b'});

    PdfMetadataBuilder metadataBuilder;
    ASSERT_TRUE(metadataBuilder.begin(reinterpret_cast<const uint8_t*>("images"), 6).ok());
    PdfMetadata metadata = metadataBuilder.metadata();
    metadata.sectionCount = 1;
    metadata.outlineCount = 1;
    metadata.totalWords = 2;
    MetadataSectionSource section{{
        .byteSize = static_cast<uint32_t>(required.values.front().size),
        .cumulativeSize = static_cast<uint32_t>(required.values.front().size),
        .firstWordOrdinal = 0,
        .wordCount = 2,
        .firstAnchorOrdinal = 0,
        .tocIndex = 0,
    }};
    PdfTestByteSink metadataBytes;
    ASSERT_TRUE(pdfEncodeMetadata(metadata, {&section, 1, MetadataSectionSource::read}, metadataBytes.sink()).ok());
    addRequired("gen_7/metadata.bin", metadataBytes.bytes());

    std::array<PdfOutlineEntry, 1> outlineWorkspace{};
    PdfOutlineBuilder outlineBuilder({outlineWorkspace.data(), 1});
    ASSERT_TRUE(outlineBuilder.begin().ok());
    ASSERT_TRUE(outlineBuilder.finish(reinterpret_cast<const uint8_t*>("images"), 6).ok());
    OutlineEntrySource outline{outlineWorkspace[0]};
    PdfTestByteSink outlineBytes;
    ASSERT_TRUE(pdfEncodeOutline({&outline, 1, OutlineEntrySource::read}, outlineBytes.sink()).ok());
    addRequired("gen_7/outline.bin", outlineBytes.bytes());

    PdfCacheManifest manifest;
    manifest.sequence = 3;
    manifest.completed = true;
    manifest.source = identity;
    manifest.generation = kGeneration;
    manifest.totalWords = 2;
    manifest.requiredFileCount = static_cast<uint32_t>(required.values.size());
    manifest.requiredFileLedger = PDF_CACHE_FNV64_OFFSET;
    for (const PdfRequiredFileRecord& record : required.values) {
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
    ASSERT_TRUE(cache.commitManifest(manifest, required.source(), evidence, prior, &committed).ok());
    ASSERT_TRUE(committed.selected);
    storage.clearEvents();
    storage.clearReadObservations();
  }

  void buildStandard() {
    pxcBytes = {5, 0, 2, 0, 0x1b, 0x00, 0xe4, 0x00};
    jpegBytes = minimalJpeg();
    jpegResourceLeaf = jpegLeaf(jpegBytes);
    build({
        {kPixelLeaf, pxcBytes},
        {jpegResourceLeaf, jpegBytes},
    });
  }

  PdfTestCacheIo storage;
  PdfSourceIdentity identity{};
  RequiredFileRecords required;
  std::string cacheRoot;
  std::string sectionPath;
  std::string coverPath;
  std::string thumbnailPath;
  std::string jpegResourceLeaf;
  std::vector<std::string> imagePaths;
  std::vector<uint8_t> pxcBytes;
  std::vector<uint8_t> jpegBytes;

 private:
  static std::string buildSection(const std::vector<ResourceSpec>& resources) {
    std::string section =
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
        "<html xmlns=\"http://www.w3.org/1999/xhtml\"><body><p id=\"b00000000\">Images";
    for (const ResourceSpec& resource : resources) {
      section += "<img src=\"../images/";
      section += resource.leaf;
      section += "\" alt=\"\"/>";
    }
    section += " render</p></body></html>";
    return section;
  }

  void addRequired(const std::string& relative, const std::vector<uint8_t>& bytes) {
    ASSERT_LT(relative.size(), PDF_CACHE_REQUIRED_PATH_CAPACITY);
    storage.addFile(cacheRoot + "/" + relative, bytes);
    PdfRequiredFileRecord record{};
    std::memcpy(record.path, relative.data(), relative.size());
    record.pathLength = static_cast<uint8_t>(relative.size());
    record.size = bytes.size();
    record.crc32 = pdfCacheCrc32(bytes.data(), bytes.size());
    required.values.push_back(record);
  }
};

class BufferPrint final : public Print {
 public:
  size_t write(const uint8_t* const bytes, const size_t length) override {
    output.insert(output.end(), bytes, bytes + length);
    return length;
  }

  std::vector<uint8_t> output;
};

uint64_t grayscaleFramebufferHash(const std::vector<uint8_t>& pxc) {
  pixel_cache::Layout layout{};
  if (pixel_cache::decodeHeader(pxc.data(), pxc.size(), layout) != pixel_cache::Status::Ok ||
      pxc.size() != layout.fileBytes) {
    return 0;
  }
  std::array<uint8_t, 8 * 4> framebuffer{};
  for (uint16_t row = 0; row < layout.height; ++row) {
    for (uint16_t column = 0; column < layout.width; ++column) {
      const size_t byteIndex =
          pixel_cache::kHeaderSize + static_cast<size_t>(row) * layout.bytesPerRow + column / 4U;
      const uint8_t shift = static_cast<uint8_t>(6U - (column & 3U) * 2U);
      framebuffer[static_cast<size_t>(row) * 8U + column] =
          static_cast<uint8_t>((pxc[byteIndex] >> shift) & 0x03U);
    }
  }
  return pdfCacheFnv64(framebuffer.data(), framebuffer.size());
}

TEST(PdfCachedImageRendering, LoadsAndResolvesCanonicalResourcesWithOnlyOneReader) {
  ImageCacheFixture fixture;
  fixture.buildStandard();
  fixture.storage.setMaximumReadHandles(1);
  const uint32_t sourceOpensBefore = fixture.storage.openCallsForPath(kImageSourcePath);

  PdfReflowDocument document;
  ASSERT_TRUE(document.initialize(fixture.storage.io(), kImageSourcePath, kImageCacheDirectory).ok());
  const PdfStatus status = document.loadCompletedCache();
  ASSERT_TRUE(status.ok()) << static_cast<unsigned>(status.error) << "@" << status.offset;

  EXPECT_EQ(document.getCoverBmpPath(), fixture.coverPath);
  EXPECT_EQ(document.getCoverBmpPath(true), fixture.coverPath);
  EXPECT_EQ(document.getThumbBmpPath(), fixture.thumbnailPath);
  EXPECT_EQ(document.getThumbBmpPath(60, 80), fixture.thumbnailPath);
  EXPECT_EQ(document.getAdaptiveThumbBmpPath(60, 80), fixture.thumbnailPath);
  EXPECT_TRUE(document.generateCoverBmp());
  EXPECT_TRUE(document.generateThumbBmp(60, 80));
  EXPECT_TRUE(document.generateAdaptiveThumbBmp(60, 80));

  ReflowResource pixel;
  ASSERT_TRUE(document.getImmutableLocalResource(0, fixture.imagePaths[0], pixel));
  EXPECT_EQ(pixel.localPath, fixture.imagePaths[0]);
  EXPECT_EQ(pixel.imageKind, ReflowImageKind::PixelCache);
  EXPECT_EQ(pixel.width, 5);
  EXPECT_EQ(pixel.height, 2);

  ReflowResource jpeg;
  ASSERT_TRUE(document.getImmutableLocalResource(0, fixture.imagePaths[1], jpeg));
  EXPECT_EQ(jpeg.localPath, fixture.imagePaths[1]);
  EXPECT_EQ(jpeg.imageKind, ReflowImageKind::EncodedImage);
  EXPECT_EQ(jpeg.width, 3);
  EXPECT_EQ(jpeg.height, 2);

  size_t size = 0;
  EXPECT_TRUE(document.getResourceSize(0, fixture.imagePaths[0], &size));
  EXPECT_EQ(size, fixture.pxcBytes.size());
  EXPECT_TRUE(document.getResourceSize(0, fixture.imagePaths[1], &size));
  EXPECT_EQ(size, fixture.jpegBytes.size());

  PdfCacheHandle sectionHandle{};
  const PdfCacheIo io = fixture.storage.io();
  ASSERT_TRUE(io.open(io.context, fixture.sectionPath.c_str(), PdfCacheOpenMode::Read, &sectionHandle).ok());
  const uint32_t opensWithSectionHeld = fixture.storage.openCalls();
  ASSERT_TRUE(document.resolveResource(0, fixture.imagePaths[0], pixel));
  ASSERT_TRUE(document.resolveResource(0, fixture.imagePaths[1], jpeg));
  EXPECT_TRUE(document.getResourceSize(0, fixture.imagePaths[0], &size));
  EXPECT_EQ(fixture.storage.openCalls(), opensWithSectionHeld)
      << "resource metadata must be cached before the XHTML reader is opened";
  ASSERT_TRUE(io.close(io.context, &sectionHandle).ok());

  BufferPrint pixelOutput;
  ASSERT_TRUE(document.streamResource(0, fixture.imagePaths[0], pixelOutput, 3));
  EXPECT_EQ(pixelOutput.output, fixture.pxcBytes);
  BufferPrint jpegOutput;
  ASSERT_TRUE(document.streamResource(0, fixture.imagePaths[1], jpegOutput, 5));
  EXPECT_EQ(jpegOutput.output, fixture.jpegBytes);

  EXPECT_FALSE(document.resolveResource(1, fixture.imagePaths[0], pixel));
  EXPECT_FALSE(document.resolveResource(0, fixture.imagePaths[0] + "/alias", pixel));
  EXPECT_FALSE(document.resolveResource(0, "../images/" + std::string(kPixelLeaf), pixel));
  EXPECT_EQ(fixture.storage.openCallsForPath(kImageSourcePath), sourceOpensBefore + 1U);
  EXPECT_EQ(fixture.storage.openHandleCount(), 0U);
}

TEST(PdfCachedImageRendering, InspectsJpegDimensionsOnceDuringManifestValidation) {
  ImageCacheFixture fixture;
  fixture.buildStandard();
  fixture.storage.setMaximumReadHandles(1);
  const uint32_t sourceOpensBefore = fixture.storage.openCallsForPath(kImageSourcePath);

  PdfReflowDocument document;
  ASSERT_TRUE(document.initialize(fixture.storage.io(), kImageSourcePath, kImageCacheDirectory).ok());
  ASSERT_TRUE(document.loadCompletedCache().ok());
  ASSERT_EQ(fixture.storage.openCallsForPath(fixture.imagePaths[1]), 1U);

  ReflowResource resource;
  for (int repetition = 0; repetition < 8; ++repetition) {
    ASSERT_TRUE(document.resolveResource(0, fixture.imagePaths[1], resource));
    EXPECT_EQ(resource.width, 3);
    EXPECT_EQ(resource.height, 2);
  }
  EXPECT_EQ(fixture.storage.openCallsForPath(fixture.imagePaths[1]), 1U);
  EXPECT_EQ(fixture.storage.openCallsForPath(kImageSourcePath), sourceOpensBefore + 1U);
}

TEST(PdfCachedImageRendering, RejectsNonCanonicalAliasesDuplicatesAndResourceOverflow) {
  const std::vector<uint8_t> pxc = {1, 0, 1, 0, 0x00};
  const std::array<std::vector<ResourceSpec>, 3> invalidSets = {
      std::vector<ResourceSpec>{{"0123456789abcdeF-89abcdef.pxc", pxc}},
      std::vector<ResourceSpec>{{"0123456789abcdef-89abcdef.pxc", pxc},
                                {"0123456789abcdef-89abcdef.pxc", pxc}},
      std::vector<ResourceSpec>{{"0123456789abcdef-89abcdef-0000000000000005.jpg", minimalJpeg()}},
  };

  for (const auto& resources : invalidSets) {
    ImageCacheFixture fixture;
    fixture.build(resources);
    PdfReflowDocument document;
    ASSERT_TRUE(document.initialize(fixture.storage.io(), kImageSourcePath, kImageCacheDirectory).ok());
    EXPECT_EQ(document.loadCompletedCache().error, PdfError::Malformed);
    EXPECT_EQ(fixture.storage.openHandleCount(), 0U);
  }

  std::vector<ResourceSpec> tooMany;
  tooMany.reserve(65);
  for (uint64_t index = 0; index < 65; ++index) {
    char leaf[40]{};
    ASSERT_GT(std::snprintf(leaf, sizeof(leaf), "%016llx-%08x.pxc",
                            static_cast<unsigned long long>(index + 1U), static_cast<unsigned>(index + 1U)),
              0);
    tooMany.push_back({leaf, pxc});
  }
  ImageCacheFixture atLimit;
  atLimit.build(std::vector<ResourceSpec>(tooMany.begin(), tooMany.begin() + 64));
  PdfReflowDocument atLimitDocument;
  ASSERT_TRUE(atLimitDocument.initialize(atLimit.storage.io(), kImageSourcePath, kImageCacheDirectory).ok());
  ASSERT_TRUE(atLimitDocument.loadCompletedCache().ok());
  ReflowResource lastAllowed;
  EXPECT_TRUE(atLimitDocument.resolveResource(0, atLimit.imagePaths.back(), lastAllowed));

  ImageCacheFixture overflow;
  overflow.build(tooMany);
  PdfReflowDocument document;
  ASSERT_TRUE(document.initialize(overflow.storage.io(), kImageSourcePath, kImageCacheDirectory).ok());
  EXPECT_EQ(document.loadCompletedCache().error, PdfError::LimitExceeded);
  EXPECT_EQ(overflow.storage.openHandleCount(), 0U);
}

TEST(PdfCachedImageRendering, RejectsInvalidPixelLayoutAndJpegIdentityOrDimensions) {
  struct Case {
    ResourceSpec resource;
  };
  std::vector<uint8_t> invalidJpeg = minimalJpeg(0, 2);
  std::vector<uint8_t> fingerprintJpeg = minimalJpeg();
  const std::array<Case, 3> cases = {
      Case{{"0123456789abcdef-89abcdef.pxc", {5, 0, 2, 0, 0x00}}},
      Case{{jpegLeaf(invalidJpeg), invalidJpeg}},
      Case{{"0123456789abcdef-89abcdef-0000000000000017.jpg", fingerprintJpeg}},
  };

  for (const Case& testCase : cases) {
    ImageCacheFixture fixture;
    fixture.build({testCase.resource});
    PdfReflowDocument document;
    ASSERT_TRUE(document.initialize(fixture.storage.io(), kImageSourcePath, kImageCacheDirectory).ok());
    EXPECT_EQ(document.loadCompletedCache().error, PdfError::Malformed);
    EXPECT_EQ(fixture.storage.openHandleCount(), 0U);
  }
}

TEST(PdfCachedImageRendering, CachedPixelFramebufferHashSurvivesReopenAndLayoutRecordReload) {
  ImageCacheFixture fixture;
  fixture.buildStandard();
  fixture.storage.setMaximumReadHandles(1);
  const uint32_t sourceOpensBefore = fixture.storage.openCallsForPath(kImageSourcePath);

  std::vector<uint8_t> serializedLayoutRecord;
  uint64_t firstHash = 0;
  {
    PdfReflowDocument first;
    ASSERT_TRUE(first.initialize(fixture.storage.io(), kImageSourcePath, kImageCacheDirectory).ok());
    ASSERT_TRUE(first.loadCompletedCache().ok());
    ReflowResource resource;
    ASSERT_TRUE(first.resolveResource(0, fixture.imagePaths[0], resource));
    serializedLayoutRecord.push_back(static_cast<uint8_t>(resource.localPath.size()));
    serializedLayoutRecord.insert(serializedLayoutRecord.end(), resource.localPath.begin(), resource.localPath.end());
    serializedLayoutRecord.push_back(static_cast<uint8_t>(resource.width));
    serializedLayoutRecord.push_back(static_cast<uint8_t>(resource.width >> 8U));
    serializedLayoutRecord.push_back(static_cast<uint8_t>(resource.height));
    serializedLayoutRecord.push_back(static_cast<uint8_t>(resource.height >> 8U));
    BufferPrint output;
    ASSERT_TRUE(first.streamResource(0, resource.localPath, output, 2));
    firstHash = grayscaleFramebufferHash(output.output);
    ASSERT_NE(firstHash, 0U);
    EXPECT_EQ(fixture.storage.openCallsForPath(kImageSourcePath), sourceOpensBefore + 1U);
  }

  PdfReflowDocument reopened;
  ASSERT_TRUE(reopened.initialize(fixture.storage.io(), kImageSourcePath, kImageCacheDirectory).ok());
  ASSERT_TRUE(reopened.loadCompletedCache().ok());
  ASSERT_GE(serializedLayoutRecord.size(), 5U);
  const uint8_t pathLength = serializedLayoutRecord[0];
  ASSERT_EQ(serializedLayoutRecord.size(), static_cast<size_t>(pathLength) + 5U);
  const std::string reloadedPath(serializedLayoutRecord.begin() + 1,
                                 serializedLayoutRecord.begin() + 1 + pathLength);
  const size_t dimensions = static_cast<size_t>(pathLength) + 1U;
  const uint16_t reloadedWidth = static_cast<uint16_t>(serializedLayoutRecord[dimensions]) |
                                 static_cast<uint16_t>(serializedLayoutRecord[dimensions + 1U]) << 8U;
  const uint16_t reloadedHeight = static_cast<uint16_t>(serializedLayoutRecord[dimensions + 2U]) |
                                  static_cast<uint16_t>(serializedLayoutRecord[dimensions + 3U]) << 8U;
  ReflowResource resource;
  ASSERT_TRUE(reopened.resolveResource(0, reloadedPath, resource));
  EXPECT_EQ(resource.width, reloadedWidth);
  EXPECT_EQ(resource.height, reloadedHeight);
  BufferPrint output;
  ASSERT_TRUE(reopened.streamResource(0, reloadedPath, output, 2));
  EXPECT_EQ(grayscaleFramebufferHash(output.output), firstHash);
  EXPECT_EQ(fixture.storage.openCallsForPath(kImageSourcePath), sourceOpensBefore + 2U);

  std::vector<uint8_t> positiveControl = output.output;
  positiveControl[pixel_cache::kHeaderSize] ^= 0x40U;
  EXPECT_NE(grayscaleFramebufferHash(positiveControl), firstHash)
      << "positive control must prove the framebuffer hash detects changed pixels";

  fixture.storage.corruptByte(fixture.imagePaths[0], pixel_cache::kHeaderSize, 0x40U);
  PdfReflowDocument corrupted;
  ASSERT_TRUE(corrupted.initialize(fixture.storage.io(), kImageSourcePath, kImageCacheDirectory).ok());
  EXPECT_EQ(corrupted.loadCompletedCache().error, PdfError::Malformed);
  EXPECT_EQ(fixture.storage.openHandleCount(), 0U);
}

}  // namespace
