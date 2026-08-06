#pragma once
#include <HalStorage.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

#include "Block.h"

struct PdfPixelCacheRenderWorkspace {
  static constexpr size_t READ_BUFFER_BYTES = 4096;
  static constexpr size_t PATH_BYTES = 160;

  alignas(uint32_t) uint8_t readBuffer[READ_BUFFER_BYTES] = {};
  char path[PATH_BYTES] = {};
  bool inUse = false;
};

static_assert(sizeof(PdfPixelCacheRenderWorkspace) == 4260,
              "PDF pixel-cache workspace must retain its bounded aligned layout");

class ImageBlock final : public Block {
 public:
  ImageBlock(std::string imagePath, std::string sourcePath, int16_t width, int16_t height);
  ~ImageBlock() override = default;

  const std::string& getImagePath() const { return imagePath; }
  int16_t getWidth() const { return width; }
  int16_t getHeight() const { return height; }

  bool imageExists() const;
  bool hasValidCache() const;
  bool needsDecode() const;
  void renderPlaceholder(GfxRenderer& renderer, int x, int y, bool foregroundBlack) const;
  static void clearSessionRenderFailures();

  // The section builder only reads image headers. The reader supplies this
  // allocation-free callback to extract a full image on its first render.
  using ExtractFn = bool (*)(void* context, const char* sourcePath, const char* destinationPath);
  static void setExtractor(void* context, ExtractFn fn);

  BlockType getType() override { return IMAGE_BLOCK; }
  bool isEmpty() override { return false; }

  void render(GfxRenderer& renderer, const int x, const int y);
  void render(GfxRenderer& renderer, int x, int y, PdfPixelCacheRenderWorkspace* pdfWorkspace);
  bool serialize(HalFile& file);
  static std::unique_ptr<ImageBlock> deserialize(HalFile& file);

 private:
  std::string imagePath;
  std::string sourcePath;
  int16_t width;
  int16_t height;

  static void* extractContext;
  static ExtractFn extractFn;
};
