#pragma once

#include <cstddef>
#include <cstdint>

namespace pixel_cache {

// Legacy .pxc layout: uint16_t width LE, uint16_t height LE, then row-major
// 2-bit pixels (four pixels per byte, most-significant pixel first).
constexpr size_t kHeaderSize = 4;

enum class Status : uint8_t {
  Ok = 0,
  InvalidArgument,
  InvalidDimensions,
  SizeOverflow,
};

struct Layout {
  uint16_t width;
  uint16_t height;
  size_t bytesPerRow;
  size_t payloadBytes;
  size_t fileBytes;
};

// Dimensions must be non-zero and encodable in the legacy uint16_t header.
// Arithmetic is checked before dimensions are narrowed.
Status calculateLayout(size_t width, size_t height, Layout& out) noexcept;
void encodeHeader(const Layout& layout, uint8_t* out) noexcept;
Status decodeHeader(const uint8_t* data, size_t size, Layout& out) noexcept;

}  // namespace pixel_cache
