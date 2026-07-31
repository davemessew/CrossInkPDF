#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

#include "Epub/blocks/ImageBlock.h"
#include "Epub/converters/ImageDecoderFactory.h"
#include "GfxRenderer.h"
#include "HalStorage.h"
#include "PdfPreparation.h"
#include "PdfReflowDocument.h"
#include "PdfTestCacheIo.h"
#include "Print.h"

namespace {

constexpr char kSourcePath[] = "/books/prepared-render.pdf";
constexpr char kCacheDirectory[] = "/.crosspoint";
constexpr char kSerializedBlockPath[] = "/layout/image.bin";

struct PreparationHarness {
  PdfTestCacheIo storage;
  PdfResourceSnapshot resources{128U * 1024U, 96U * 1024U, 8U * 1024U};
  uint32_t nowMs = 0;

  static uint32_t now(void* const context) { return static_cast<PreparationHarness*>(context)->nowMs; }
  static PdfResourceSnapshot measure(void* const context) {
    return static_cast<PreparationHarness*>(context)->resources;
  }

  PdfPreparationConfig config() {
    return {
        storage.io(), kSourcePath, kCacheDirectory, this, now, {this, measure, nullptr}, storage.renameCallback(),
        800,          480,
    };
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

std::vector<uint8_t> loadFixture(const char* const name) {
  const std::filesystem::path path =
      std::filesystem::path(__FILE__).parent_path().parent_path() / "pdf_reflow_core" / "fixtures" / name;
  std::ifstream input(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

PdfStepResult runToTerminal(PdfPreparation& preparation, PreparationHarness& harness) {
  for (uint32_t step = 0; step < 20000; ++step) {
    const PdfStepResult result = preparation.step();
    ++harness.nowMs;
    if (!result.yielded()) {
      return result;
    }
  }
  return PdfStepResult::failure(PdfStatus::failure(PdfError::BudgetExhausted));
}

uint64_t framebufferHash(const GfxRenderer& renderer) {
  uint64_t hash = 14695981039346656037ULL;
  for (const uint8_t byte : renderer.framebuffer()) {
    hash ^= byte;
    hash *= 1099511628211ULL;
  }
  return hash;
}

std::string findPixelCache(const PreparationHarness& harness, const std::string& cacheRoot) {
  const auto paths = harness.storage.paths();
  const auto found = std::find_if(paths.begin(), paths.end(), [&](const std::string& path) {
    return path.starts_with(cacheRoot + "/gen_") && path.find("/images/") != std::string::npos &&
           path.ends_with(".pxc");
  });
  return found == paths.end() ? std::string{} : *found;
}

std::string findJpegCache(const PreparationHarness& harness, const std::string& cacheRoot) {
  const auto paths = harness.storage.paths();
  const auto found = std::find_if(paths.begin(), paths.end(), [&](const std::string& path) {
    return path.starts_with(cacheRoot + "/gen_") && path.find("/images/") != std::string::npos &&
           path.ends_with(".jpg");
  });
  return found == paths.end() ? std::string{} : *found;
}

TEST(PdfPreparedCacheRendering, PreparedCacheReopensPaginatesSerializesAndRendersWithoutSourceReads) {
  PreparationHarness harness;
  harness.storage.addFile(kSourcePath, loadFixture("flate_gray_caption.pdf"), 1234, true);
  harness.storage.setMaximumReadHandles(1);

  std::string cacheRoot;
  {
    PdfPreparation preparation;
    ASSERT_TRUE(preparation.begin(harness.config()).ok());
    const PdfStepResult result = runToTerminal(preparation, harness);
    ASSERT_TRUE(result.complete()) << static_cast<unsigned>(result.status.error) << "@" << result.status.offset;
    cacheRoot = preparation.cacheRoot();
  }
  const std::string pixelPath = findPixelCache(harness, cacheRoot);
  ASSERT_FALSE(pixelPath.empty());

  ReflowResource image;
  std::vector<uint8_t> preparedPixels;
  uint64_t firstHash = 0;
  uint32_t sourceOpensAfterFirstBoundary = 0;
  {
    PdfReflowDocument document;
    ASSERT_TRUE(document.initialize(harness.storage.io(), kSourcePath, kCacheDirectory).ok());
    ASSERT_TRUE(document.loadCompletedCache().ok());
    sourceOpensAfterFirstBoundary = harness.storage.openCallsForPath(kSourcePath);

    BufferPrint paginationInput;
    ASSERT_TRUE(document.streamSection(0, paginationInput, 37));
    ASSERT_FALSE(paginationInput.output.empty());
    ASSERT_TRUE(document.getImmutableLocalResource(0, pixelPath, image));
    ASSERT_EQ(image.imageKind, ReflowImageKind::PixelCache);
    preparedPixels = harness.storage.bytes(image.localPath);

    Storage.reset();
    Storage.addFile(image.localPath, preparedPixels);
    FsFile serialized;
    ASSERT_TRUE(Storage.openFileForWrite("TEST", kSerializedBlockPath, serialized));
    ImageBlock block(image.localPath, static_cast<int16_t>(image.width), static_cast<int16_t>(image.height));
    ASSERT_TRUE(block.serialize(serialized));
    ASSERT_TRUE(serialized.close());

    FsFile reloadedFile;
    ASSERT_TRUE(Storage.openFileForRead("TEST", kSerializedBlockPath, reloadedFile));
    std::unique_ptr<ImageBlock> reloaded = ImageBlock::deserialize(reloadedFile);
    ASSERT_NE(reloaded, nullptr);
    ASSERT_TRUE(reloadedFile.close());

    GfxRenderer renderer;
    reloaded->render(renderer, 0, 0);
    firstHash = framebufferHash(renderer);
    ASSERT_EQ(firstHash, 0x9b9af024946a5e91ULL);
    EXPECT_EQ(harness.storage.openCallsForPath(kSourcePath), sourceOpensAfterFirstBoundary);

    std::vector<uint8_t> changedPixels = preparedPixels;
    ASSERT_GT(changedPixels.size(), 4U);
    changedPixels[4] ^= 0xc0U;
    Storage.addFile(image.localPath, changedPixels);
    renderer.clear();
    reloaded->render(renderer, 0, 0);
    EXPECT_NE(framebufferHash(renderer), firstHash)
        << "positive control must prove the framebuffer oracle sees changed prepared pixels";
    Storage.addFile(image.localPath, preparedPixels);
  }

  PdfReflowDocument reopened;
  ASSERT_TRUE(reopened.initialize(harness.storage.io(), kSourcePath, kCacheDirectory).ok());
  ASSERT_TRUE(reopened.loadCompletedCache().ok());
  const uint32_t sourceOpensAfterReopenBoundary = harness.storage.openCallsForPath(kSourcePath);
  ReflowResource reopenedImage;
  ASSERT_TRUE(reopened.getImmutableLocalResource(0, pixelPath, reopenedImage));
  EXPECT_EQ(reopenedImage.width, image.width);
  EXPECT_EQ(reopenedImage.height, image.height);

  GfxRenderer renderer;
  ImageBlock reopenedBlock(reopenedImage.localPath, static_cast<int16_t>(reopenedImage.width),
                           static_cast<int16_t>(reopenedImage.height));
  renderer.clear();
  reopenedBlock.render(renderer, 0, 0);
  EXPECT_EQ(framebufferHash(renderer), firstHash);
  EXPECT_EQ(harness.storage.openCallsForPath(kSourcePath), sourceOpensAfterReopenBoundary);
  EXPECT_EQ(harness.storage.openHandleCount(), 0U);
}

TEST(PdfPreparedCacheRendering, PreparedJpegIdentityAndDimensionsAreValidatedOnceBeforePagination) {
  PreparationHarness harness;
  harness.storage.addFile(kSourcePath, loadFixture("jpeg_cover_caption.pdf"), 1234, true);
  harness.storage.setMaximumReadHandles(1);

  std::string cacheRoot;
  {
    PdfPreparation preparation;
    ASSERT_TRUE(preparation.begin(harness.config()).ok());
    const PdfStepResult result = runToTerminal(preparation, harness);
    ASSERT_TRUE(result.complete()) << static_cast<unsigned>(result.status.error) << "@" << result.status.offset;
    cacheRoot = preparation.cacheRoot();
  }
  const std::string jpegPath = findJpegCache(harness, cacheRoot);
  ASSERT_FALSE(jpegPath.empty());
  const uint32_t jpegOpensBeforeConsumer = harness.storage.openCallsForPath(jpegPath);

  PdfReflowDocument document;
  ASSERT_TRUE(document.initialize(harness.storage.io(), kSourcePath, kCacheDirectory).ok());
  ASSERT_TRUE(document.loadCompletedCache().ok());
  ASSERT_EQ(harness.storage.openCallsForPath(jpegPath), jpegOpensBeforeConsumer + 1U);
  const uint32_t sourceOpensAfterIdentity = harness.storage.openCallsForPath(kSourcePath);

  ReflowResource image;
  for (int repetition = 0; repetition < 8; ++repetition) {
    ASSERT_TRUE(document.getImmutableLocalResource(0, jpegPath, image));
    EXPECT_EQ(image.imageKind, ReflowImageKind::EncodedImage);
    EXPECT_GT(image.width, 0U);
    EXPECT_GT(image.height, 0U);
  }
  EXPECT_EQ(harness.storage.openCallsForPath(jpegPath), jpegOpensBeforeConsumer + 1U);
  EXPECT_EQ(harness.storage.openCallsForPath(kSourcePath), sourceOpensAfterIdentity);
  EXPECT_EQ(harness.storage.openHandleCount(), 0U);
}

}  // namespace

ImageToFramebufferDecoder* ImageDecoderFactory::getDecoder(const std::string&) { return nullptr; }
bool ImageDecoderFactory::isFormatSupported(const std::string&) { return false; }
