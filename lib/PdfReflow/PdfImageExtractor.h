#pragma once

#include <cstddef>
#include <cstdint>

#include "PdfPixelCacheWriter.h"
#include "PdfTypes.h"

enum class PdfImageColorSpace : uint8_t {
  Gray = 0,
  RGB,
  IndexedGray,
  IndexedRGB,
  ImageMask,
};

enum class PdfImageDecode : uint8_t {
  Normal = 0,
  Inverted,
};

struct PdfImageParameters {
  uint32_t width = 0;
  uint32_t height = 0;
  uint16_t maximumOutputWidth = 0;
  uint16_t maximumOutputHeight = 0;
  uint32_t maximumOutputBytes = 0;
  const uint8_t* palette = nullptr;
  size_t paletteBytes = 0;
  uint16_t paletteEntries = 0;
  uint8_t bitsPerComponent = 0;
  uint8_t predictor = 1;
  // Current nonstroking color reduced to luminance for a 1-bit ImageMask.
  // The PDF graphics-state adapter supplies this later; black is the safe
  // default for callers that do not yet expose that state.
  uint8_t imageMaskPaintLuminance = 0;
  PdfImageColorSpace colorSpace = PdfImageColorSpace::Gray;
  PdfImageDecode decode = PdfImageDecode::Normal;
  bool hasSoftMask = false;
  PdfImageDecode softMaskDecode = PdfImageDecode::Normal;
};

// The source-row buffer is the caller-owned, phase-reused 8 KiB predictor
// workspace. The output row is unpacked luminance/2-bit pixels and must remain
// at or below 4 KiB. The extractor performs no allocation.
struct PdfImageWorkspace {
  uint8_t* sourceRow = nullptr;
  size_t sourceRowCapacity = 0;
  uint8_t* outputRow = nullptr;
  size_t outputRowCapacity = 0;
};

struct PdfImageInfo {
  uint32_t sourceWidth = 0;
  uint32_t sourceHeight = 0;
  uint16_t outputWidth = 0;
  uint16_t outputHeight = 0;
  uint16_t paletteEntries = 0;
  size_t sourceRowBytes = 0;
  uint64_t expectedDecodedBytes = 0;
  uint64_t outputBytes = 0;
};

// Converts an already-inflated PDF image byte stream directly to legacy .pxc.
// XObject and inline-image bytes share this same bounded contract.
// PdfStreamDecoder can write unmasked images to decodedSink(); this seam
// deliberately does not own an inflater dictionary or a source file. With a
// soft mask, callers feed one complete base row, then the corresponding
// unpredicted Gray8 mask row. The outer preparation layer must phase the
// separate PDF base and /SMask streams through a spool/row adapter so only one
// source reader is open; that single-reader adapter is intentionally outside
// this isolated conversion core. Sink calls may report a short successful
// consumption at a row boundary so that adapter can switch phases.
class PdfImageExtractor {
 public:
  PdfStatus begin(const PdfImageParameters& parameters, PdfByteSink output, const PdfImageWorkspace& workspace);
  PdfByteSink decodedSink();
  PdfByteSink softMaskSink();
  PdfStatus extractDecoded(const PdfByteSource& decodedSource);
  PdfStatus finish();

  const PdfImageInfo& info() const { return info_; }
  PdfStatus status() const { return status_; }
  uint32_t sourceRowsConsumed() const { return sourceRowIndex_; }
  uint16_t outputRowsWritten() const { return outputRowIndex_; }
  bool awaitingSoftMask() const { return awaitingSoftMask_; }

 private:
  static PdfStatus writeDecoded(void* context, const uint8_t* source, size_t requested, size_t* bytesWritten);
  static PdfStatus writeSoftMask(void* context, const uint8_t* source, size_t requested, size_t* bytesWritten);

  PdfStatus consumeDecoded(const uint8_t* source, size_t requested, size_t* bytesWritten);
  PdfStatus consumeSoftMask(const uint8_t* source, size_t requested, size_t* bytesWritten);
  PdfStatus completeSourceRow();
  PdfStatus writeCurrentOutputRow();
  PdfStatus fail(PdfStatus status);

  PdfImageParameters parameters_{};
  PdfImageWorkspace workspace_{};
  PdfImageInfo info_{};
  PdfPixelCacheWriter writer_{};
  PdfStatus status_{};
  uint64_t decodedBytes_ = 0;
  size_t sourceRowPosition_ = 0;
  size_t softMaskPosition_ = 0;
  uint32_t sourceRowIndex_ = 0;
  uint32_t nextSelectedSourceRow_ = 0;
  uint32_t maskSourceX_ = 0;
  uint32_t maskBaseStep_ = 0;
  uint32_t maskStepRemainder_ = 0;
  uint32_t maskError_ = 0;
  uint16_t outputRowIndex_ = 0;
  uint16_t nextMaskOutputX_ = 0;
  uint16_t maskDenominator_ = 0;
  uint8_t components_ = 0;
  uint8_t pngBytesPerPixel_ = 0;
  uint8_t pngFilter_ = 0;
  uint8_t pngUpLeft_[4]{};
  bool initialized_ = false;
  bool finished_ = false;
  bool readingPngFilter_ = false;
  bool awaitingSoftMask_ = false;
  bool currentRowSelected_ = false;
};
