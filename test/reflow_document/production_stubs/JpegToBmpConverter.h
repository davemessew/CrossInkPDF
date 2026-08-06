#pragma once

class JpegToBmpConverter {
 public:
  template <typename... Args>
  static bool jpegFileToBmpStream(Args&&...) {
    return false;
  }
  template <typename... Args>
  static bool jpegFileTo1BitBmpStreamWithSize(Args&&...) {
    return false;
  }
};
