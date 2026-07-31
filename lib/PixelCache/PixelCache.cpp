#include "PixelCache.h"

#include <limits>

namespace pixel_cache {
namespace {

void clearLayout(Layout& layout) noexcept { layout = Layout{}; }

}  // namespace

Status calculateLayout(const size_t width, const size_t height, Layout& out) noexcept {
  clearLayout(out);
  if (width == 0 || height == 0) {
    return Status::InvalidDimensions;
  }

  const size_t bytesPerRow = width / 4U + (width % 4U == 0U ? 0U : 1U);
  constexpr size_t maxSize = std::numeric_limits<size_t>::max();
  if (height > (maxSize - kHeaderSize) / bytesPerRow) {
    return Status::SizeOverflow;
  }

  if (width > std::numeric_limits<uint16_t>::max() || height > std::numeric_limits<uint16_t>::max()) {
    return Status::InvalidDimensions;
  }

  const size_t payloadBytes = bytesPerRow * height;
  out.width = static_cast<uint16_t>(width);
  out.height = static_cast<uint16_t>(height);
  out.bytesPerRow = bytesPerRow;
  out.payloadBytes = payloadBytes;
  out.fileBytes = kHeaderSize + payloadBytes;
  return Status::Ok;
}

void encodeHeader(const Layout& layout, uint8_t* const out) noexcept {
  out[0] = static_cast<uint8_t>(layout.width & 0xffU);
  out[1] = static_cast<uint8_t>(layout.width >> 8U);
  out[2] = static_cast<uint8_t>(layout.height & 0xffU);
  out[3] = static_cast<uint8_t>(layout.height >> 8U);
}

Status decodeHeader(const uint8_t* const data, const size_t size, Layout& out) noexcept {
  clearLayout(out);
  if (data == nullptr || size < kHeaderSize) {
    return Status::InvalidArgument;
  }

  const uint16_t width = static_cast<uint16_t>(data[0]) | static_cast<uint16_t>(data[1]) << 8U;
  const uint16_t height = static_cast<uint16_t>(data[2]) | static_cast<uint16_t>(data[3]) << 8U;
  return calculateLayout(width, height, out);
}

}  // namespace pixel_cache
