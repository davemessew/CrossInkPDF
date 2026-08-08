#pragma once

#include <Arduino.h>

class HalDisplay {
 public:
  HalDisplay();
  ~HalDisplay();

  enum RefreshMode { FULL_REFRESH, HALF_REFRESH, FAST_REFRESH };

  void begin(bool seamless = false);

  static constexpr uint16_t DISPLAY_WIDTH = 800;
  static constexpr uint16_t DISPLAY_HEIGHT = 480;
  static constexpr uint16_t DISPLAY_WIDTH_BYTES = DISPLAY_WIDTH / 8;
  static constexpr uint32_t BUFFER_SIZE = DISPLAY_WIDTH_BYTES * DISPLAY_HEIGHT;

  void clearScreen(uint8_t color = 0xFF) const;
  void drawImage(const uint8_t* imageData, uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                 bool fromProgmem = false) const;
  void drawImageTransparent(const uint8_t* imageData, uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                            bool fromProgmem = false) const;
  void displayBuffer(RefreshMode mode = RefreshMode::FAST_REFRESH, bool turnOffScreen = false);
  void displayBufferAsync(RefreshMode mode = RefreshMode::FAST_REFRESH);
  void waitRefreshComplete();
  bool supportsAsyncRefresh() const;
  bool supportsAsyncGrayscaleBase() const;
  void refreshDisplay(RefreshMode mode = RefreshMode::FAST_REFRESH, bool turnOffScreen = false);
  void deepSleep();
  uint8_t* getFrameBuffer() const;
  uint8_t* lendFrameBufferStorage(uint32_t* size);
  void returnFrameBufferStorage();
  void preconditionGrayscale();
  void preconditionGrayscale(uint16_t x, uint16_t y, uint16_t w, uint16_t h);
  void displayGrayscaleBase(RefreshMode fallback = HALF_REFRESH, bool turnOffScreen = false);
  void copyGrayscaleBuffers(const uint8_t* lsbBuffer, const uint8_t* msbBuffer);
  void copyGrayscaleLsbBuffers(const uint8_t* lsbBuffer);
  void copyGrayscaleMsbBuffers(const uint8_t* msbBuffer);
  void cleanupGrayscaleBuffers(const uint8_t* bwBuffer);
  void displayGrayBuffer(bool turnOffScreen = false);
  void writeGrayscalePlaneStrip(bool lsbPlane, const uint8_t* rows, uint16_t yStart, uint16_t numRows);
  bool supportsStripGrayscale() const;
  uint16_t getDisplayWidth() const;
  uint16_t getDisplayHeight() const;
  uint16_t getDisplayWidthBytes() const;
  uint32_t getBufferSize() const;

 private:
  inline static uint8_t framebuffer[48000] = {};
};

extern HalDisplay display;
