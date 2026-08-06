#pragma once

#include <EpdFontFamily.h>

#include <cstdint>
#include <cstring>

namespace BidiUtils {
enum class BidiBaseDir : signed char { AUTO = -1, LTR = 0, RTL = 1 };
}

class GfxRenderer {
 public:
  enum Orientation : uint8_t {
    Portrait = 0,
    LandscapeClockwise = 1,
    PortraitInverted = 2,
    LandscapeCounterClockwise = 3,
  };

  void setOrientation(const Orientation orientation) { orientation_ = orientation; }
  Orientation getOrientation() const { return orientation_; }
  void clearScreen(const uint8_t color = 0xff) {
    ++clearCalls;
    lastClearColor = color;
  }
  bool isFontCacheScanning() const { return false; }
  int getFontAscenderSize(int) const { return 12; }
  int getSpaceWidth(int, EpdFontFamily::Style) const { return 4; }
  int getTextAdvanceX(int, const char* text, EpdFontFamily::Style) const {
    return text == nullptr ? 0 : static_cast<int>(std::strlen(text)) * 6;
  }
  int getTextWidth(int fontId, const char* text, EpdFontFamily::Style style,
                   BidiUtils::BidiBaseDir = BidiUtils::BidiBaseDir::LTR) const {
    return getTextAdvanceX(fontId, text, style);
  }
  void drawText(int fontId, int x, int y, const char* text, bool black,
                EpdFontFamily::Style style = EpdFontFamily::REGULAR,
                BidiUtils::BidiBaseDir = BidiUtils::BidiBaseDir::LTR) {
    ++drawTextCalls;
    lastFontId = fontId;
    lastTextX = x;
    lastTextY = y;
    lastTextBlack = black;
    lastTextStyle = style;
    if (text != nullptr) {
      std::strncpy(lastText, text, sizeof(lastText) - 1U);
      lastText[sizeof(lastText) - 1U] = '\0';
    }
  }
  void drawLine(int x1, int y1, int x2, int y2, bool black = true) {
    drawLine(x1, y1, x2, y2, 1, black);
  }
  void drawLine(int x1, int y1, int x2, int y2, int width, bool black) {
    ++drawLineCalls;
    lastLineX1 = x1;
    lastLineY1 = y1;
    lastLineX2 = x2;
    lastLineY2 = y2;
    lastLineWidth = width;
    lastLineBlack = black;
  }
  void drawRect(int, int, int, int, bool = true) { ++drawRectCalls; }
  void fillRect(int, int, int, int, bool = true) { ++fillRectCalls; }

  Orientation orientation_ = Portrait;
  uint32_t clearCalls = 0;
  uint32_t drawTextCalls = 0;
  uint32_t drawLineCalls = 0;
  uint32_t drawRectCalls = 0;
  uint32_t fillRectCalls = 0;
  int lastFontId = 0;
  int lastTextX = 0;
  int lastTextY = 0;
  int lastLineX1 = 0;
  int lastLineY1 = 0;
  int lastLineX2 = 0;
  int lastLineY2 = 0;
  int lastLineWidth = 0;
  EpdFontFamily::Style lastTextStyle = EpdFontFamily::REGULAR;
  uint8_t lastClearColor = 0xff;
  bool lastTextBlack = false;
  bool lastLineBlack = false;
  char lastText[64]{};
};
