#pragma once

class GfxRenderer {
 public:
  bool isSdCardFont(int) const { return false; }
  bool releaseSdCardFontForLowMemory(int) const { return false; }
};
