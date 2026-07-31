#include <gtest/gtest.h>

#include <array>
#include <cstdlib>
#include <new>
#include <string>
#include <vector>

#include "Epub/blocks/ImageBlock.h"
#include "Epub/converters/ImageDecoderFactory.h"
#include "GfxRenderer.h"
#include "HalStorage.h"

namespace {

size_t mallocCalls = 0;
size_t freeCalls = 0;
size_t nothrowArrayNewCalls = 0;
size_t arrayDeleteCalls = 0;
ImageBlock* nestedBlock = nullptr;
GfxRenderer* nestedRenderer = nullptr;
int nestedHookCalls = 0;

class AllocationScope {
 public:
  AllocationScope()
      : mallocBaseline_(mallocCalls),
        freeBaseline_(freeCalls),
        arrayNewBaseline_(nothrowArrayNewCalls),
        arrayDeleteBaseline_(arrayDeleteCalls) {}
  size_t mallocs() const { return mallocCalls - mallocBaseline_; }
  size_t frees() const { return freeCalls - freeBaseline_; }
  size_t arrayNews() const { return nothrowArrayNewCalls - arrayNewBaseline_; }
  size_t arrayDeletes() const { return arrayDeleteCalls - arrayDeleteBaseline_; }

 private:
  size_t mallocBaseline_;
  size_t freeBaseline_;
  size_t arrayNewBaseline_;
  size_t arrayDeleteBaseline_;
};

constexpr char kPdfPixelPath[] =
    "/.crosspoint/pdf_123/gen_7/images/0123456789abcdef-89abcdef.pxc";
constexpr char kEpubImagePath[] = "/.crosspoint/epub_123/image.jpg";
constexpr char kEpubPixelPath[] = "/.crosspoint/epub_123/image.pxc";

void renderNestedImage() {
  if (++nestedHookCalls < 3) {
    return;
  }
  Storage.setReadHook(nullptr);
  if (nestedBlock != nullptr && nestedRenderer != nullptr) {
    nestedBlock->render(*nestedRenderer, 0, 0);
  }
}

std::vector<uint8_t> pixels() {
  return {
      4, 0, 2, 0,
      0x1b,  // 0, 1, 2, 3
      0xe4,  // 3, 2, 1, 0
  };
}

uint64_t framebufferHash(const GfxRenderer& renderer) {
  uint64_t hash = 14695981039346656037ULL;
  for (const uint8_t byte : renderer.framebuffer()) {
    hash ^= byte;
    hash *= 1099511628211ULL;
  }
  return hash;
}

TEST(PdfCachedImageBlock, ScalesPreparedPixelCacheWithoutDecoding) {
  Storage.reset();
  Storage.addFile(kPdfPixelPath, pixels());
  GfxRenderer renderer;
  ImageBlock block(kPdfPixelPath, 2, 2);

  block.render(renderer, 0, 0);

  EXPECT_EQ(renderer.framebuffer()[0], 0x3fU);
  EXPECT_EQ(renderer.framebuffer()[1], 0xbfU);
  EXPECT_EQ(framebufferHash(renderer), 0xc7ad3025cfdc7751ULL);
  EXPECT_EQ(Storage.opens(), 1U);
}

TEST(PdfCachedImageBlock, ReusesStaticReadBufferAcrossPdfRenderPasses) {
  Storage.reset();
  Storage.addFile(kPdfPixelPath, pixels());
  GfxRenderer renderer;
  ImageBlock block(kPdfPixelPath, 4, 2);

  {
    AllocationScope allocations;
    for (int pass = 0; pass < 8; ++pass) {
      renderer.clear();
      block.render(renderer, 0, 0);
      EXPECT_EQ(framebufferHash(renderer), 0xa5ff812bfb471921ULL);
    }
    EXPECT_EQ(allocations.mallocs(), 0U);
    EXPECT_EQ(allocations.frees(), 0U);
    EXPECT_EQ(allocations.arrayNews(), 0U);
    EXPECT_EQ(allocations.arrayDeletes(), 0U);
  }

  AllocationScope positiveControl;
  void* allocation = std::malloc(16);
  ASSERT_NE(allocation, nullptr);
  std::free(allocation);
  EXPECT_EQ(positiveControl.mallocs(), 1U);
  EXPECT_EQ(positiveControl.frees(), 1U);
}

TEST(PdfCachedImageBlock, KeepsLegacyEpubCacheAllocationPath) {
  Storage.reset();
  Storage.addFile(kEpubImagePath, {'j', 'p', 'e', 'g'});
  Storage.addFile(kEpubPixelPath, pixels());
  GfxRenderer renderer;
  ImageBlock block(kEpubImagePath, 4, 2);

  AllocationScope allocations;
  block.render(renderer, 0, 0);

  EXPECT_EQ(allocations.mallocs(), 1U);
  EXPECT_EQ(allocations.frees(), 1U);
  EXPECT_EQ(allocations.arrayNews(), 0U);
  EXPECT_EQ(allocations.arrayDeletes(), 0U);
  EXPECT_EQ(renderer.framebuffer()[0], 0x1fU);
  EXPECT_EQ(renderer.framebuffer()[1], 0x8fU);
  EXPECT_EQ(framebufferHash(renderer), 0xa5ff812bfb471921ULL);
}

TEST(PdfCachedImageBlock, RejectsPdfCacheLookalikesAndKeepsThemOnTheLegacyPath) {
  struct Lookalike {
    const char* image;
    const char* pixelCache;
  };
  constexpr std::array<Lookalike, 4> lookalikes = {{
      {
          "/.crosspoint/epub_123/pdf_123/gen_7/images/0123456789abcdef-89abcdef.jpg",
          "/.crosspoint/epub_123/pdf_123/gen_7/images/0123456789abcdef-89abcdef.pxc",
      },
      {
          "/.crosspoint/pdf_0123/gen_7/images/0123456789abcdef-89abcdef.pxc",
          "/.crosspoint/pdf_0123/gen_7/images/0123456789abcdef-89abcdef.pxc",
      },
      {
          "/.crosspoint/pdf_123/gen_07/images/0123456789abcdef-89abcdef.pxc",
          "/.crosspoint/pdf_123/gen_07/images/0123456789abcdef-89abcdef.pxc",
      },
      {
          "/.crosspoint/pdf_123/gen_7/images/0123456789abcdeF-89abcdef.pxc",
          "/.crosspoint/pdf_123/gen_7/images/0123456789abcdeF-89abcdef.pxc",
      },
  }};

  for (const Lookalike& lookalike : lookalikes) {
    Storage.reset();
    Storage.addFile(lookalike.pixelCache, pixels());
    GfxRenderer renderer;
    ImageBlock block(lookalike.image, 4, 2);

    AllocationScope allocations;
    block.render(renderer, 0, 0);

    EXPECT_EQ(allocations.mallocs(), 1U) << lookalike.image;
    EXPECT_EQ(allocations.frees(), 1U) << lookalike.image;
    EXPECT_EQ(allocations.arrayNews(), 0U) << lookalike.image;
    EXPECT_EQ(allocations.arrayDeletes(), 0U) << lookalike.image;
    EXPECT_EQ(framebufferHash(renderer), 0xa5ff812bfb471921ULL) << lookalike.image;
  }
}

TEST(PdfCachedImageBlock, GuardsTheSharedPdfBufferAgainstNestedRendering) {
  Storage.reset();
  Storage.addFile(kPdfPixelPath, pixels());
  GfxRenderer renderer;
  ImageBlock block(kPdfPixelPath, 4, 2);
  nestedBlock = &block;
  nestedRenderer = &renderer;
  nestedHookCalls = 0;
  Storage.setReadHook(renderNestedImage);

  AllocationScope allocations;
  block.render(renderer, 0, 0);
  Storage.setReadHook(nullptr);
  nestedBlock = nullptr;
  nestedRenderer = nullptr;

  EXPECT_EQ(allocations.mallocs(), 0U);
  EXPECT_EQ(allocations.frees(), 0U);
  EXPECT_EQ(allocations.arrayNews(), 1U);
  EXPECT_EQ(allocations.arrayDeletes(), 1U);
  EXPECT_EQ(framebufferHash(renderer), 0xa5ff812bfb471921ULL);
  EXPECT_EQ(Storage.opens(), 2U);
}

}  // namespace

ImageToFramebufferDecoder* ImageDecoderFactory::getDecoder(const std::string&) { return nullptr; }
bool ImageDecoderFactory::isFormatSupported(const std::string&) { return false; }

extern "C" void* __real_malloc(size_t size);
extern "C" void __real_free(void* pointer);

void* operator new[](const size_t size, const std::nothrow_t&) noexcept {
  ++nothrowArrayNewCalls;
  return __real_malloc(size);
}

void operator delete[](void* const pointer) noexcept {
  ++arrayDeleteCalls;
  __real_free(pointer);
}

void operator delete[](void* const pointer, const size_t) noexcept {
  ++arrayDeleteCalls;
  __real_free(pointer);
}

extern "C" void* __wrap_malloc(const size_t size) {
  ++mallocCalls;
  return __real_malloc(size);
}

extern "C" void __wrap_free(void* const pointer) {
  ++freeCalls;
  __real_free(pointer);
}
