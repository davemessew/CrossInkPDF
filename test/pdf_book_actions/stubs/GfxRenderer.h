#pragma once

class GfxRenderer {
 public:
  int getTextWidth(int, const char*) const { return 20; }
  int getLineHeight(int) const { return 10; }
  int getScreenWidth() const { return 480; }
  int getScreenHeight() const { return 800; }
  void fillRect(int, int, int, int, bool) const {}
  void drawText(int, int, int, const char*, bool) const {}
  void displayBuffer() const {}
};
