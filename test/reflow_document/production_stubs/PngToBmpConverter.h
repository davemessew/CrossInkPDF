#pragma once

class PngToBmpConverter {
 public:
  template <typename... Args>
  static bool pngFileToBmpStream(Args&&...) {
    return false;
  }
  template <typename... Args>
  static bool pngFileTo1BitBmpStreamWithSize(Args&&...) {
    return false;
  }
};
