#pragma once

// Keep this dependency direct: PlatformIO's LDF does not discover PixelCache
// through PdfImageExtractor.h reliably in the firmware build.
#include <PixelCache.h>

#include <cstddef>
#include <cstdint>

#include "PdfImageExtractor.h"
#include "PdfLimits.h"
#include "PdfObjectParser.h"
#include "PdfTypes.h"

enum class PdfStreamFilter : uint8_t;

enum class PdfImageDisposition : uint8_t {
  Ready = 0,
  NeedsResolution,
  OmitUnsupported,
};

enum class PdfImageTerminalCodec : uint8_t {
  Raster = 0,
  DctJpeg,
  UnsupportedOptional,
};

enum class PdfImageStreamTarget : uint8_t {
  ExtractorDecoded = 0,
  JpegBytes,
  None,
};

enum class PdfImageAuxiliaryKind : uint8_t {
  ExplicitMask = 0,
  SoftMask,
};

enum class PdfImageUnresolved : uint8_t {
  None = 0,
  ColorSpace = 1U << 0U,
  IndexedBaseColorSpace = 1U << 1U,
  IndexedPalette = 1U << 2U,
  ExplicitMask = 1U << 3U,
  SoftMask = 1U << 4U,
};

constexpr PdfImageUnresolved operator|(const PdfImageUnresolved left, const PdfImageUnresolved right) {
  return static_cast<PdfImageUnresolved>(static_cast<uint8_t>(left) | static_cast<uint8_t>(right));
}

constexpr bool pdfImageHasUnresolved(const PdfImageUnresolved fields, const PdfImageUnresolved field) {
  return (static_cast<uint8_t>(fields) & static_cast<uint8_t>(field)) != 0;
}

struct PdfImageStreamRange {
  uint64_t offset = 0;
  uint64_t length = 0;
  uint64_t sourceSize = 0;
};

struct PdfImageStreamPlan {
  uint64_t offset = 0;
  uint64_t length = 0;
  PdfStreamFilter decoderFilters[PdfLimits::MaxFiltersPerStream]{};
  uint8_t decoderFilterCount = 0;
  PdfImageTerminalCodec terminalCodec = PdfImageTerminalCodec::Raster;
  PdfImageStreamTarget target = PdfImageStreamTarget::ExtractorDecoded;
};

struct PdfImageObjectParseInput {
  uint16_t dictionaryIndex = PDF_INVALID_INDEX;
  PdfImageStreamRange stream{};
  // Indexed palettes are copied into this caller-owned, activity-lifetime
  // buffer. The parser never allocates; Indexed RGB needs at most 768 bytes.
  uint8_t* palette = nullptr;
  size_t paletteCapacity = 0;
};

struct PdfImageObjectDescriptor {
  PdfImageParameters parameters{};
  PdfImageStreamPlan stream{};
  PdfObjectReference colorSpaceReference{};
  PdfObjectReference indexedBaseColorSpaceReference{};
  PdfObjectReference paletteReference{};
  PdfObjectReference explicitMaskReference{};
  PdfObjectReference softMaskReference{};
  PdfStatus omitReason{};
  uint32_t predictorColumns = 0;
  // Exact non-zero endpoint from an explicit /Decode pair. It is retained
  // while an indirect color space is unresolved so the phased resolver can
  // apply the color-space-specific endpoint contract without reopening the
  // object arena.
  uint32_t decodeUpperValue = 0;
  uint16_t paletteBytesRequired = 0;
  uint8_t predictorColors = 0;
  uint8_t predictorBitsPerComponent = 0;
  uint8_t decodeComponentPairs = 0;
  PdfImageUnresolved unresolved = PdfImageUnresolved::None;
  PdfImageDisposition disposition = PdfImageDisposition::Ready;
  bool hasExplicitMask = false;
  bool hasSoftMaskReference = false;
};

PdfStatus pdfParseImageObject(const PdfObjectArena& arena, const PdfImageObjectParseInput& input,
                              PdfImageObjectDescriptor* descriptor);
PdfStatus pdfApplyResolvedImageColorSpace(PdfImageObjectDescriptor* descriptor,
                                          PdfImageColorSpace resolvedColorSpace);
PdfStatus pdfApplyResolvedImageAuxiliary(PdfImageObjectDescriptor* base, const PdfImageObjectDescriptor& auxiliary,
                                         PdfImageAuxiliaryKind kind);

static_assert(sizeof(PdfImageObjectDescriptor) <= 256,
              "PDF image descriptors must remain small enough for bounded parser state");
