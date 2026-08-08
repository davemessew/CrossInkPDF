#include "HalDisplay.h"

#include <algorithm>
#include <cstring>

#include "QemuHalControl.h"

HalDisplay display;

namespace {
uint8_t imageByte(const uint8_t* data, size_t offset, bool fromProgmem) {
  (void)fromProgmem;
  return data[offset];
}

void drawMonochrome(const uint8_t* imageData, uint16_t x, uint16_t y, uint16_t w, uint16_t h, bool fromProgmem,
                    bool transparent) {
  if (imageData == nullptr) {
    return;
  }
  const size_t sourceStride = (static_cast<size_t>(w) + 7U) / 8U;
  uint8_t* const destination = display.getFrameBuffer();
  for (uint16_t row = 0; row < h && static_cast<uint32_t>(y) + row < HalDisplay::DISPLAY_HEIGHT; ++row) {
    for (uint16_t column = 0; column < w && static_cast<uint32_t>(x) + column < HalDisplay::DISPLAY_WIDTH; ++column) {
      const uint8_t source =
          imageByte(imageData, static_cast<size_t>(row) * sourceStride + column / 8U, fromProgmem);
      const bool white = (source & (0x80U >> (column % 8U))) != 0;
      if (transparent && white) {
        continue;
      }
      const size_t destinationOffset =
          static_cast<size_t>(y + row) * HalDisplay::DISPLAY_WIDTH_BYTES + (x + column) / 8U;
      const uint8_t mask = static_cast<uint8_t>(0x80U >> ((x + column) % 8U));
      if (white) {
        destination[destinationOffset] |= mask;
      } else {
        destination[destinationOffset] &= static_cast<uint8_t>(~mask);
      }
    }
  }
}
}  // namespace

HalDisplay::HalDisplay() = default;

HalDisplay::~HalDisplay() = default;

void HalDisplay::begin(bool seamless) {
  (void)seamless;
  clearScreen();
}

void HalDisplay::clearScreen(uint8_t color) const { std::memset(framebuffer, color, BUFFER_SIZE); }

void HalDisplay::drawImage(const uint8_t* imageData, uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                           bool fromProgmem) const {
  drawMonochrome(imageData, x, y, w, h, fromProgmem, false);
}

void HalDisplay::drawImageTransparent(const uint8_t* imageData, uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                                      bool fromProgmem) const {
  drawMonochrome(imageData, x, y, w, h, fromProgmem, true);
}

void HalDisplay::displayBuffer(RefreshMode mode, bool turnOffScreen) {
  (void)mode;
  (void)turnOffScreen;
}

void HalDisplay::displayBufferAsync(RefreshMode mode) { displayBuffer(mode); }

void HalDisplay::waitRefreshComplete() {}

bool HalDisplay::supportsAsyncRefresh() const { return false; }

bool HalDisplay::supportsAsyncGrayscaleBase() const { return false; }

void HalDisplay::refreshDisplay(RefreshMode mode, bool turnOffScreen) {
  displayBuffer(mode, turnOffScreen);
}

void HalDisplay::deepSleep() {}

uint8_t* HalDisplay::getFrameBuffer() const { return framebuffer; }

uint8_t* HalDisplay::lendFrameBufferStorage(uint32_t* size) {
  if (size != nullptr) {
    *size = 0;
  }
  return nullptr;
}

void HalDisplay::returnFrameBufferStorage() {}

void HalDisplay::preconditionGrayscale() {}

void HalDisplay::preconditionGrayscale(uint16_t x, uint16_t y, uint16_t w, uint16_t h) {
  (void)x;
  (void)y;
  (void)w;
  (void)h;
}

void HalDisplay::displayGrayscaleBase(RefreshMode fallback, bool turnOffScreen) {
  displayBuffer(fallback, turnOffScreen);
}

void HalDisplay::copyGrayscaleBuffers(const uint8_t* lsbBuffer, const uint8_t* msbBuffer) {
  (void)msbBuffer;
  copyGrayscaleLsbBuffers(lsbBuffer);
}

void HalDisplay::copyGrayscaleLsbBuffers(const uint8_t* lsbBuffer) {
  if (lsbBuffer != nullptr) {
    std::memcpy(framebuffer, lsbBuffer, BUFFER_SIZE);
  }
}

void HalDisplay::copyGrayscaleMsbBuffers(const uint8_t* msbBuffer) {
  if (msbBuffer != nullptr) {
    std::memcpy(framebuffer, msbBuffer, BUFFER_SIZE);
  }
}

void HalDisplay::cleanupGrayscaleBuffers(const uint8_t* bwBuffer) {
  if (bwBuffer != nullptr) {
    std::memcpy(framebuffer, bwBuffer, BUFFER_SIZE);
  }
}

void HalDisplay::displayGrayBuffer(bool turnOffScreen) { (void)turnOffScreen; }

void HalDisplay::writeGrayscalePlaneStrip(bool lsbPlane, const uint8_t* rows, uint16_t yStart, uint16_t numRows) {
  (void)lsbPlane;
  if (rows == nullptr || yStart >= DISPLAY_HEIGHT) {
    return;
  }
  const uint16_t rowsToCopy = std::min<uint16_t>(numRows, DISPLAY_HEIGHT - yStart);
  std::memcpy(framebuffer + static_cast<size_t>(yStart) * DISPLAY_WIDTH_BYTES, rows,
              static_cast<size_t>(rowsToCopy) * DISPLAY_WIDTH_BYTES);
}

bool HalDisplay::supportsStripGrayscale() const { return true; }

uint16_t HalDisplay::getDisplayWidth() const { return DISPLAY_WIDTH; }

uint16_t HalDisplay::getDisplayHeight() const { return DISPLAY_HEIGHT; }

uint16_t HalDisplay::getDisplayWidthBytes() const { return DISPLAY_WIDTH_BYTES; }

uint32_t HalDisplay::getBufferSize() const { return BUFFER_SIZE; }

uint32_t QemuHalControl::frameCrc32() {
  const uint8_t* const bytes = display.getFrameBuffer();
  uint32_t crc = 0xFFFFFFFFU;
  for (size_t index = 0; index < HalDisplay::BUFFER_SIZE; ++index) {
    crc ^= bytes[index];
    for (uint8_t bit = 0; bit < 8; ++bit) {
      const uint32_t mask = 0U - (crc & 1U);
      crc = (crc >> 1U) ^ (0xEDB88320U & mask);
    }
  }
  return ~crc;
}
