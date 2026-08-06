#pragma once

#include <PixelCache.h>

#include <cstddef>
#include <cstdint>

#include "PdfTypes.h"

// Allocation-free adapter from one unpacked 2-bit PDF image row at a time to
// the legacy .pxc byte stream. Rows must arrive exactly once in ascending order.
// The first error is sticky so a caller cannot accidentally continue a partial
// cache; the owner of the sink remains responsible for removing that output.
class PdfPixelCacheWriter {
 public:
  PdfStatus begin(PdfByteSink sink, size_t width, size_t height);
  PdfStatus writeRow(uint32_t rowIndex, const uint8_t* pixels, size_t pixelCount);
  PdfStatus finish();

  const pixel_cache::Layout& layout() const { return layout_; }
  uint32_t rowsWritten() const { return nextRow_; }
  uint64_t bytesWritten() const { return outputOffset_; }
  PdfStatus status() const { return status_; }

 private:
  // An 800-pixel device row becomes two writes while the stack buffer remains
  // well below the firmware's 256-byte local-frame guideline.
  static constexpr size_t kChunkBytes = 128;

  PdfStatus writeChunk(const uint8_t* bytes, size_t size);
  PdfStatus fail(PdfStatus status);

  PdfByteSink sink_{};
  pixel_cache::Layout layout_{};
  PdfStatus status_{};
  uint64_t outputOffset_ = 0;
  uint32_t nextRow_ = 0;
  bool initialized_ = false;
  bool finished_ = false;
};

static_assert(sizeof(PdfPixelCacheWriter) <= 96, "PDF pixel-cache writer must stay stack-bounded");
