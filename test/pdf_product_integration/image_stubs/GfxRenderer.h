#pragma once

#include <array>
#include <cstdint>

class FontCacheManager;

class GfxRenderer {
 public:
  enum Orientation : uint8_t {
    Portrait,
    LandscapeClockwise,
    PortraitInverted,
    LandscapeCounterClockwise,
  };

  enum RenderMode : uint8_t {
    BW,
    GRAYSCALE_MSB,
    GRAYSCALE_LSB,
  };

  GfxRenderer() { clear(); }

  void clear() { framebuffer_.fill(0xffU); }
  FontCacheManager* getFontCacheManager() { return nullptr; }
  int getScreenWidth() const { return 8; }
  int getScreenHeight() const { return 4; }
  int getDisplayWidth() const { return 8; }
  int getDisplayHeight() const { return 4; }
  uint16_t getDisplayWidthBytes() const { return 1; }
  Orientation getOrientation() const { return LandscapeCounterClockwise; }
  RenderMode getRenderMode() const { return BW; }
  uint8_t* getWriteTarget() { return framebuffer_.data(); }
  int getWriteOriginY() const { return 0; }
  int getWriteRows() const { return 4; }
  bool isStripTargetActive() const { return false; }
  bool glyphIntersectsStrip(int, int, int, int) const { return true; }
  const std::array<uint8_t, 4>& framebuffer() const { return framebuffer_; }

 private:
  std::array<uint8_t, 4> framebuffer_{};
};
