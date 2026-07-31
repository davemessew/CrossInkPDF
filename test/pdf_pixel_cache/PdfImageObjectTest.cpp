#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <new>

#include "PdfImageObject.h"
#include "PdfLexer.h"
#include "PdfObjectParser.h"
#include "PdfStreamDecoder.h"

#if defined(PDF_IMAGE_OBJECT_WRAP_MALLOC)
extern "C" void* __real_malloc(size_t size);
extern "C" void __real_free(void* pointer);
#endif

namespace {

std::atomic<bool> gTrackAllocations{false};
std::atomic<size_t> gNewCount{0};
std::atomic<size_t> gMallocCount{0};
std::atomic<size_t> gFreeCount{0};

void* trackedMalloc(const size_t size) {
#if defined(PDF_IMAGE_OBJECT_WRAP_MALLOC)
  return __real_malloc(size);
#else
  return std::malloc(size);
#endif
}

void trackedFree(void* const pointer) {
#if defined(PDF_IMAGE_OBJECT_WRAP_MALLOC)
  __real_free(pointer);
#else
  std::free(pointer);
#endif
}

struct FixedSource {
  const uint8_t* bytes = nullptr;
  size_t size = 0;

  static PdfStatus readAt(void* context, const uint64_t offset, uint8_t* destination, const size_t requested,
                          size_t* bytesRead) {
    if (context == nullptr || destination == nullptr || bytesRead == nullptr) {
      return PdfStatus::failure(PdfError::InvalidArgument);
    }
    auto& source = *static_cast<FixedSource*>(context);
    if (offset > source.size) {
      return PdfStatus::failure(PdfError::InvalidOffset, offset);
    }
    const size_t available = source.size - static_cast<size_t>(offset);
    const size_t count = requested < available ? requested : available;
    if (count != 0) {
      std::memcpy(destination, source.bytes + offset, count);
    }
    *bytesRead = count;
    return PdfStatus::success();
  }

  PdfByteSource source() { return {this, size, readAt}; }
};

struct ArenaStorage {
  std::array<PdfValue, 96> values{};
  std::array<PdfDictionaryEntry, 64> dictionaries{};
  std::array<PdfArrayItem, 64> arrays{};
  std::array<uint8_t, 1024> text{};

  PdfObjectArena arena() {
    return {
        values.data(),       static_cast<uint16_t>(values.size()),
        dictionaries.data(), static_cast<uint16_t>(dictionaries.size()),
        arrays.data(),       static_cast<uint16_t>(arrays.size()),
        text.data(),         static_cast<uint16_t>(text.size()),
    };
  }
};

template <size_t Size>
PdfStatus parseDictionary(const char (&sourceText)[Size], ArenaStorage& storage, PdfObjectArena* arena,
                          uint16_t* rootIndex) {
  FixedSource input{reinterpret_cast<const uint8_t*>(sourceText), Size - 1U};
  std::array<uint8_t, 128> sourceBuffer{};
  *arena = storage.arena();
  PdfLexer lexer(input.source(), sourceBuffer.data(), sourceBuffer.size());
  PdfObjectParser parser(lexer, *arena);
  parser.begin();
  PdfStepResult result;
  do {
    PdfWorkBudget budget{64, 1024};
    result = parser.step(budget);
  } while (result.yielded());
  if (!result.complete()) {
    return result.status;
  }
  *rootIndex = parser.rootIndex();
  return PdfStatus::success();
}

void expectReference(const PdfObjectReference& reference, const uint32_t objectNumber, const uint16_t generation = 0) {
  EXPECT_EQ(reference.objectNumber, objectNumber);
  EXPECT_EQ(reference.generation, generation);
}

TEST(PdfImageObjectParse, MapsAFlateGrayDictionaryToTheExtractorAndExactStreamRange) {
  static constexpr char dictionary[] =
      "<< /Type /XObject /Subtype /Image /Width 640 /Height 480 "
      "/ColorSpace /DeviceGray /BitsPerComponent 8 /Filter /FlateDecode >>";
  ArenaStorage storage;
  PdfObjectArena arena;
  uint16_t rootIndex = PDF_INVALID_INDEX;
  ASSERT_TRUE(parseDictionary(dictionary, storage, &arena, &rootIndex).ok());
  std::array<uint8_t, 768> palette{};
  PdfImageObjectDescriptor descriptor;

  const PdfStatus status =
      pdfParseImageObject(arena, {rootIndex, {4096, 1234, 8192}, palette.data(), palette.size()}, &descriptor);

  ASSERT_TRUE(status.ok());
  EXPECT_EQ(descriptor.disposition, PdfImageDisposition::Ready);
  EXPECT_EQ(descriptor.parameters.width, 640U);
  EXPECT_EQ(descriptor.parameters.height, 480U);
  EXPECT_EQ(descriptor.parameters.bitsPerComponent, 8U);
  EXPECT_EQ(descriptor.parameters.colorSpace, PdfImageColorSpace::Gray);
  EXPECT_EQ(descriptor.parameters.predictor, 1U);
  EXPECT_EQ(descriptor.stream.offset, 4096U);
  EXPECT_EQ(descriptor.stream.length, 1234U);
  EXPECT_EQ(descriptor.stream.target, PdfImageStreamTarget::ExtractorDecoded);
  EXPECT_EQ(descriptor.stream.terminalCodec, PdfImageTerminalCodec::Raster);
  ASSERT_EQ(descriptor.stream.decoderFilterCount, 1U);
  EXPECT_EQ(descriptor.stream.decoderFilters[0], PdfStreamFilter::Flate);
}

TEST(PdfImageObjectParse, AcceptsInlineAliasesRgbAndFullDecodeInversion) {
  static constexpr char dictionary[] = "<< /W 10 /H 2 /CS /RGB /BPC 8 /F /Fl /D [1 0 1 0 1 0] >>";
  ArenaStorage storage;
  PdfObjectArena arena;
  uint16_t rootIndex = PDF_INVALID_INDEX;
  ASSERT_TRUE(parseDictionary(dictionary, storage, &arena, &rootIndex).ok());
  PdfImageObjectDescriptor descriptor;

  const PdfStatus status = pdfParseImageObject(arena, {rootIndex, {0, 90, 90}}, &descriptor);

  ASSERT_TRUE(status.ok());
  EXPECT_EQ(descriptor.disposition, PdfImageDisposition::Ready);
  EXPECT_EQ(descriptor.parameters.colorSpace, PdfImageColorSpace::RGB);
  EXPECT_EQ(descriptor.parameters.decode, PdfImageDecode::Inverted);
  EXPECT_EQ(descriptor.parameters.bitsPerComponent, 8U);
}

TEST(PdfImageObjectParse, CopiesDirectIndexedGrayAndRgbPalettesIntoCallerStorage) {
  static constexpr char indexedGray[] =
      "<< /W 4 /H 1 /CS [/Indexed /DeviceGray 3 <0055AAFF>] /BPC 2 /F /FlateDecode >>";
  static constexpr char indexedRgb[] = "<< /W 4 /H 1 /CS [/I /RGB 3 <000000555555AAAAAAFFFFFF>] /BPC 2 /F /Fl >>";
  for (const bool rgb : {false, true}) {
    ArenaStorage storage;
    PdfObjectArena arena;
    uint16_t rootIndex = PDF_INVALID_INDEX;
    std::array<uint8_t, 768> palette{};
    PdfImageObjectDescriptor descriptor;
    const PdfStatus parseStatus = rgb ? parseDictionary(indexedRgb, storage, &arena, &rootIndex)
                                      : parseDictionary(indexedGray, storage, &arena, &rootIndex);
    ASSERT_TRUE(parseStatus.ok());

    const PdfStatus status =
        pdfParseImageObject(arena, {rootIndex, {12, 40, 64}, palette.data(), palette.size()}, &descriptor);

    ASSERT_TRUE(status.ok());
    EXPECT_EQ(descriptor.disposition, PdfImageDisposition::Ready);
    EXPECT_EQ(descriptor.parameters.colorSpace, rgb ? PdfImageColorSpace::IndexedRGB : PdfImageColorSpace::IndexedGray);
    EXPECT_EQ(descriptor.parameters.palette, palette.data());
    EXPECT_EQ(descriptor.parameters.paletteEntries, 4U);
    EXPECT_EQ(descriptor.parameters.paletteBytes, rgb ? 12U : 4U);
    EXPECT_EQ(descriptor.paletteBytesRequired, rgb ? 12U : 4U);
    EXPECT_EQ(palette[0], 0U);
    EXPECT_EQ(palette[rgb ? 11U : 3U], 255U);
  }
}

TEST(PdfImageObjectParse, CapturesColorSpaceIndexedBaseAndPaletteReferencesForPhasedResolution) {
  static constexpr char directReference[] = "<< /W 8 /H 8 /CS 21 2 R /BPC 8 /F /Fl >>";
  static constexpr char baseReference[] = "<< /W 8 /H 8 /CS [/Indexed 22 0 R 3 <00010203>] /BPC 2 /F /Fl >>";
  static constexpr char paletteReference[] = "<< /W 8 /H 8 /CS [/Indexed /DeviceRGB 3 23 1 R] /BPC 2 /F /Fl >>";

  {
    ArenaStorage storage;
    PdfObjectArena arena;
    uint16_t rootIndex = PDF_INVALID_INDEX;
    ASSERT_TRUE(parseDictionary(directReference, storage, &arena, &rootIndex).ok());
    PdfImageObjectDescriptor descriptor;
    ASSERT_TRUE(pdfParseImageObject(arena, {rootIndex, {0, 1, 1}}, &descriptor).ok());
    EXPECT_EQ(descriptor.disposition, PdfImageDisposition::NeedsResolution);
    EXPECT_TRUE(pdfImageHasUnresolved(descriptor.unresolved, PdfImageUnresolved::ColorSpace));
    expectReference(descriptor.colorSpaceReference, 21, 2);
  }
  {
    ArenaStorage storage;
    PdfObjectArena arena;
    uint16_t rootIndex = PDF_INVALID_INDEX;
    ASSERT_TRUE(parseDictionary(baseReference, storage, &arena, &rootIndex).ok());
    std::array<uint8_t, 768> palette{};
    PdfImageObjectDescriptor descriptor;
    ASSERT_TRUE(pdfParseImageObject(arena, {rootIndex, {0, 1, 1}, palette.data(), palette.size()}, &descriptor).ok());
    EXPECT_EQ(descriptor.disposition, PdfImageDisposition::NeedsResolution);
    EXPECT_TRUE(pdfImageHasUnresolved(descriptor.unresolved, PdfImageUnresolved::IndexedBaseColorSpace));
    expectReference(descriptor.indexedBaseColorSpaceReference, 22);
  }
  {
    ArenaStorage storage;
    PdfObjectArena arena;
    uint16_t rootIndex = PDF_INVALID_INDEX;
    ASSERT_TRUE(parseDictionary(paletteReference, storage, &arena, &rootIndex).ok());
    PdfImageObjectDescriptor descriptor;
    ASSERT_TRUE(pdfParseImageObject(arena, {rootIndex, {0, 1, 1}}, &descriptor).ok());
    EXPECT_EQ(descriptor.disposition, PdfImageDisposition::NeedsResolution);
    EXPECT_TRUE(pdfImageHasUnresolved(descriptor.unresolved, PdfImageUnresolved::IndexedPalette));
    expectReference(descriptor.paletteReference, 23, 1);
    EXPECT_EQ(descriptor.paletteBytesRequired, 12U);
  }
}

TEST(PdfImageObjectParse, RetainsRgbDecodeAndPredictorFactsWhileColorSpaceIsIndirect) {
  static constexpr char dictionary[] =
      "<< /W 10 /H 2 /CS 21 0 R /BPC 8 /F /Fl "
      "/D [1 0 1 0 1 0] "
      "/DP << /Predictor 12 /Colors 3 /BitsPerComponent 8 /Columns 10 >> >>";
  ArenaStorage storage;
  PdfObjectArena arena;
  uint16_t rootIndex = PDF_INVALID_INDEX;
  ASSERT_TRUE(parseDictionary(dictionary, storage, &arena, &rootIndex).ok());
  PdfImageObjectDescriptor descriptor;

  ASSERT_TRUE(pdfParseImageObject(arena, {rootIndex, {0, 1, 1}}, &descriptor).ok());

  EXPECT_EQ(descriptor.disposition, PdfImageDisposition::NeedsResolution);
  EXPECT_EQ(descriptor.parameters.decode, PdfImageDecode::Inverted);
  EXPECT_EQ(descriptor.decodeComponentPairs, 3U);
  EXPECT_EQ(descriptor.parameters.predictor, 12U);
  EXPECT_EQ(descriptor.predictorColors, 3U);
  EXPECT_EQ(descriptor.predictorBitsPerComponent, 8U);
  EXPECT_EQ(descriptor.predictorColumns, 10U);
}

TEST(PdfImageObjectParse, CapturesExplicitAndSoftMaskReferencesWithoutOpeningThem) {
  static constexpr char explicitMask[] = "<< /W 4 /H 4 /CS /DeviceGray /BPC 8 /Filter /FlateDecode /Mask 30 0 R >>";
  static constexpr char softMask[] = "<< /W 4 /H 4 /CS /DeviceGray /BPC 8 /Filter /FlateDecode /SMask 31 3 R >>";
  for (const bool soft : {false, true}) {
    ArenaStorage storage;
    PdfObjectArena arena;
    uint16_t rootIndex = PDF_INVALID_INDEX;
    ASSERT_TRUE((soft ? parseDictionary(softMask, storage, &arena, &rootIndex)
                      : parseDictionary(explicitMask, storage, &arena, &rootIndex))
                    .ok());
    PdfImageObjectDescriptor descriptor;

    ASSERT_TRUE(pdfParseImageObject(arena, {rootIndex, {5, 6, 20}}, &descriptor).ok());

    EXPECT_EQ(descriptor.disposition, PdfImageDisposition::NeedsResolution);
    EXPECT_EQ(descriptor.hasExplicitMask, !soft);
    EXPECT_EQ(descriptor.hasSoftMaskReference, soft);
    EXPECT_TRUE(pdfImageHasUnresolved(descriptor.unresolved,
                                      soft ? PdfImageUnresolved::SoftMask : PdfImageUnresolved::ExplicitMask));
    expectReference(soft ? descriptor.softMaskReference : descriptor.explicitMaskReference, soft ? 31U : 30U,
                    soft ? 3U : 0U);
    EXPECT_FALSE(descriptor.parameters.hasSoftMask);
  }
}

TEST(PdfImageObjectParse, MapsImageMaskDefaultsAndPolarity) {
  static constexpr char dictionary[] = "<< /W 8 /H 2 /IM true /F /Fl /D [1 0] >>";
  ArenaStorage storage;
  PdfObjectArena arena;
  uint16_t rootIndex = PDF_INVALID_INDEX;
  ASSERT_TRUE(parseDictionary(dictionary, storage, &arena, &rootIndex).ok());
  PdfImageObjectDescriptor descriptor;

  ASSERT_TRUE(pdfParseImageObject(arena, {rootIndex, {0, 4, 4}}, &descriptor).ok());

  EXPECT_EQ(descriptor.parameters.colorSpace, PdfImageColorSpace::ImageMask);
  EXPECT_EQ(descriptor.parameters.bitsPerComponent, 1U);
  EXPECT_EQ(descriptor.parameters.decode, PdfImageDecode::Inverted);
}

TEST(PdfImageObjectDecode, AcceptsSampleDomainDecodeEndpointsForTwoBitIndexedImages) {
  static constexpr char normal[] =
      "<< /W 4 /H 1 /CS [/Indexed /DeviceGray 3 <0055AAFF>] /BPC 2 /F /Fl /D [0 3] >>";
  static constexpr char inverted[] =
      "<< /W 4 /H 1 /CS [/Indexed /DeviceGray 3 <0055AAFF>] /BPC 2 /F /Fl /D [3 0] >>";
  for (const bool invert : {false, true}) {
    ArenaStorage storage;
    PdfObjectArena arena;
    uint16_t rootIndex = PDF_INVALID_INDEX;
    std::array<uint8_t, 768> palette{};
    ASSERT_TRUE((invert ? parseDictionary(inverted, storage, &arena, &rootIndex)
                        : parseDictionary(normal, storage, &arena, &rootIndex))
                    .ok());
    PdfImageObjectDescriptor descriptor;

    ASSERT_TRUE(
        pdfParseImageObject(arena, {rootIndex, {0, 1, 1}, palette.data(), palette.size()}, &descriptor).ok());

    EXPECT_EQ(descriptor.disposition, PdfImageDisposition::Ready);
    EXPECT_EQ(descriptor.parameters.decode, invert ? PdfImageDecode::Inverted : PdfImageDecode::Normal);
  }
}

TEST(PdfImageObjectDecode, EnforcesColorSpaceSpecificDecodeUpperEndpoints) {
  static constexpr char graySampleMaximum[] =
      "<< /W 1 /H 1 /CS /DeviceGray /BPC 8 /F /Fl /D [0 255] >>";
  static constexpr char rgbSampleMaximum[] =
      "<< /W 1 /H 1 /CS /DeviceRGB /BPC 8 /F /Fl /D [255 0 255 0 255 0] >>";
  static constexpr char indexedUnitNormal[] =
      "<< /W 4 /H 1 /CS [/Indexed /DeviceGray 3 <0055AAFF>] /BPC 2 /F /Fl /D [0 1] >>";
  static constexpr char indexedUnitInverted[] =
      "<< /W 4 /H 1 /CS [/Indexed /DeviceGray 3 <0055AAFF>] /BPC 2 /F /Fl /D [1 0] >>";

  for (const uint8_t fixture : {0U, 1U, 2U, 3U}) {
    ArenaStorage storage;
    PdfObjectArena arena;
    uint16_t rootIndex = PDF_INVALID_INDEX;
    PdfStatus parsed;
    if (fixture == 0U) {
      parsed = parseDictionary(graySampleMaximum, storage, &arena, &rootIndex);
    } else if (fixture == 1U) {
      parsed = parseDictionary(rgbSampleMaximum, storage, &arena, &rootIndex);
    } else if (fixture == 2U) {
      parsed = parseDictionary(indexedUnitNormal, storage, &arena, &rootIndex);
    } else {
      parsed = parseDictionary(indexedUnitInverted, storage, &arena, &rootIndex);
    }
    ASSERT_TRUE(parsed.ok());
    std::array<uint8_t, 768> palette{};
    PdfImageObjectDescriptor descriptor;

    ASSERT_TRUE(
        pdfParseImageObject(arena, {rootIndex, {0, 1, 1}, palette.data(), palette.size()}, &descriptor).ok());

    EXPECT_EQ(descriptor.disposition, PdfImageDisposition::OmitUnsupported);
    EXPECT_EQ(descriptor.omitReason.error, PdfError::UnsupportedEncoding);
  }
}

TEST(PdfImageObjectDecode, KeepsMalformedDecodeMembersFatal) {
  static constexpr char nonNumeric[] =
      "<< /W 1 /H 1 /CS /DeviceGray /BPC 8 /F /Fl /D [0 /NotANumber] >>";
  static constexpr char wrongShape[] =
      "<< /W 1 /H 1 /CS /DeviceGray /BPC 8 /F /Fl /D [0 1 0] >>";
  for (const bool shape : {false, true}) {
    ArenaStorage storage;
    PdfObjectArena arena;
    uint16_t rootIndex = PDF_INVALID_INDEX;
    ASSERT_TRUE((shape ? parseDictionary(wrongShape, storage, &arena, &rootIndex)
                       : parseDictionary(nonNumeric, storage, &arena, &rootIndex))
                    .ok());
    PdfImageObjectDescriptor descriptor;

    const PdfStatus status = pdfParseImageObject(arena, {rootIndex, {0, 1, 1}}, &descriptor);

    EXPECT_EQ(status.error, PdfError::Malformed);
  }
}

TEST(PdfImageObjectDecode, DefersRawDecodeEndpointValidationUntilIndirectColorSpaceResolution) {
  static constexpr char unsupportedRgb[] =
      "<< /W 1 /H 1 /CS 21 0 R /BPC 8 /F /Fl /D [255 0 255 0 255 0] >>";
  static constexpr char supportedRgb[] =
      "<< /W 1 /H 1 /CS 21 0 R /BPC 8 /F /Fl /D [1 0 1 0 1 0] >>";

  for (const bool supported : {false, true}) {
    ArenaStorage storage;
    PdfObjectArena arena;
    uint16_t rootIndex = PDF_INVALID_INDEX;
    ASSERT_TRUE((supported ? parseDictionary(supportedRgb, storage, &arena, &rootIndex)
                           : parseDictionary(unsupportedRgb, storage, &arena, &rootIndex))
                    .ok());
    PdfImageObjectDescriptor descriptor;
    ASSERT_TRUE(pdfParseImageObject(arena, {rootIndex, {0, 1, 1}}, &descriptor).ok());
    ASSERT_EQ(descriptor.disposition, PdfImageDisposition::NeedsResolution);
    EXPECT_EQ(descriptor.decodeUpperValue, supported ? 1U : 255U);
    EXPECT_EQ(descriptor.decodeComponentPairs, 3U);
    EXPECT_EQ(descriptor.parameters.decode, PdfImageDecode::Inverted);

    ASSERT_TRUE(pdfApplyResolvedImageColorSpace(&descriptor, PdfImageColorSpace::RGB).ok());

    EXPECT_EQ(descriptor.disposition,
              supported ? PdfImageDisposition::Ready : PdfImageDisposition::OmitUnsupported);
    if (supported) {
      EXPECT_FALSE(pdfImageHasUnresolved(descriptor.unresolved, PdfImageUnresolved::ColorSpace));
      EXPECT_EQ(descriptor.parameters.colorSpace, PdfImageColorSpace::RGB);
    } else {
      EXPECT_EQ(descriptor.omitReason.error, PdfError::UnsupportedEncoding);
    }
  }
}

TEST(PdfImageObjectDecode, ValidatesIndexedDecodeAfterIndirectBaseColorSpaceResolution) {
  static constexpr char dictionary[] =
      "<< /W 4 /H 1 /CS [/Indexed 22 0 R 3 <0055AAFF>] /BPC 2 /F /Fl /D [0 3] >>";
  ArenaStorage storage;
  PdfObjectArena arena;
  uint16_t rootIndex = PDF_INVALID_INDEX;
  std::array<uint8_t, 768> palette{};
  ASSERT_TRUE(parseDictionary(dictionary, storage, &arena, &rootIndex).ok());
  PdfImageObjectDescriptor descriptor;
  ASSERT_TRUE(
      pdfParseImageObject(arena, {rootIndex, {0, 1, 1}, palette.data(), palette.size()}, &descriptor).ok());
  ASSERT_EQ(descriptor.disposition, PdfImageDisposition::NeedsResolution);
  EXPECT_EQ(descriptor.decodeUpperValue, 3U);

  ASSERT_TRUE(pdfApplyResolvedImageColorSpace(&descriptor, PdfImageColorSpace::Gray).ok());

  EXPECT_EQ(descriptor.disposition, PdfImageDisposition::Ready);
  EXPECT_EQ(descriptor.parameters.colorSpace, PdfImageColorSpace::IndexedGray);
  EXPECT_EQ(descriptor.paletteBytesRequired, 4U);
}

TEST(PdfImageObjectDecode, DoesNotConstrainIndexedHighValueToTheOneBitSampleCardinality) {
  static constexpr char dictionary[] =
      "<< /W 2 /H 1 /CS [/Indexed /DeviceGray 3 <0055AAFF>] /BPC 1 /F /Fl /D [0 1] >>";
  ArenaStorage storage;
  PdfObjectArena arena;
  uint16_t rootIndex = PDF_INVALID_INDEX;
  std::array<uint8_t, 768> palette{};
  ASSERT_TRUE(parseDictionary(dictionary, storage, &arena, &rootIndex).ok());
  PdfImageObjectDescriptor descriptor;

  ASSERT_TRUE(pdfParseImageObject(arena, {rootIndex, {0, 1, 1}, palette.data(), palette.size()}, &descriptor).ok());

  EXPECT_EQ(descriptor.disposition, PdfImageDisposition::Ready);
  EXPECT_EQ(descriptor.parameters.paletteEntries, 4U);
}

TEST(PdfImageObjectFilters, PreservesOnlyPlainTerminalEightBitDct) {
  static constexpr char dictionary[] = "<< /W 100 /H 100 /CS /DeviceRGB /BPC 8 /F /DCT >>";
  ArenaStorage storage;
  PdfObjectArena arena;
  uint16_t rootIndex = PDF_INVALID_INDEX;
  ASSERT_TRUE(parseDictionary(dictionary, storage, &arena, &rootIndex).ok());
  PdfImageObjectDescriptor descriptor;

  ASSERT_TRUE(pdfParseImageObject(arena, {rootIndex, {100, 500, 1000}}, &descriptor).ok());

  EXPECT_EQ(descriptor.disposition, PdfImageDisposition::Ready);
  EXPECT_EQ(descriptor.stream.terminalCodec, PdfImageTerminalCodec::DctJpeg);
  EXPECT_EQ(descriptor.stream.target, PdfImageStreamTarget::JpegBytes);
  EXPECT_EQ(descriptor.stream.decoderFilterCount, 0U);
}

TEST(PdfImageObjectFilters, OmitsDctWithAnyPrecedingFilters) {
  static constexpr char dictionary[] =
      "<< /W 100 /H 100 /CS /DeviceRGB /BPC 8 "
      "/F [/AHx /A85 /DCT] /DP [null null null] >>";
  ArenaStorage storage;
  PdfObjectArena arena;
  uint16_t rootIndex = PDF_INVALID_INDEX;
  ASSERT_TRUE(parseDictionary(dictionary, storage, &arena, &rootIndex).ok());
  PdfImageObjectDescriptor descriptor;

  ASSERT_TRUE(pdfParseImageObject(arena, {rootIndex, {100, 500, 1000}}, &descriptor).ok());

  EXPECT_EQ(descriptor.disposition, PdfImageDisposition::OmitUnsupported);
  EXPECT_EQ(descriptor.stream.terminalCodec, PdfImageTerminalCodec::UnsupportedOptional);
  EXPECT_EQ(descriptor.stream.target, PdfImageStreamTarget::None);
  EXPECT_EQ(descriptor.omitReason.error, PdfError::UnsupportedFilter);
}

TEST(PdfImageObjectFilters, OmitsFlatePredictorBeforeDct) {
  static constexpr char dictionary[] =
      "<< /W 10 /H 10 /CS /DeviceRGB /BPC 8 "
      "/F [/Fl /DCT] "
      "/DP [<< /Predictor 12 /Colors 3 /BitsPerComponent 8 /Columns 10 >> null] >>";
  ArenaStorage storage;
  PdfObjectArena arena;
  uint16_t rootIndex = PDF_INVALID_INDEX;
  ASSERT_TRUE(parseDictionary(dictionary, storage, &arena, &rootIndex).ok());
  PdfImageObjectDescriptor descriptor;

  ASSERT_TRUE(pdfParseImageObject(arena, {rootIndex, {0, 1, 1}}, &descriptor).ok());

  EXPECT_EQ(descriptor.disposition, PdfImageDisposition::OmitUnsupported);
  EXPECT_EQ(descriptor.omitReason.error, PdfError::UnsupportedFilter);
}

TEST(PdfImageObjectFilters, OmitsValidButUnsupportedMultiFilterPipelines) {
  static constexpr char dctThenAscii[] = "<< /W 1 /H 1 /CS /DeviceRGB /BPC 8 /Filter [/DCTDecode /ASCII85Decode] >>";
  static constexpr char flateThenAscii[] =
      "<< /W 1 /H 1 /CS /DeviceGray /BPC 8 /Filter [/FlateDecode /ASCIIHexDecode] >>";
  for (const bool dct : {false, true}) {
    ArenaStorage storage;
    PdfObjectArena arena;
    uint16_t rootIndex = PDF_INVALID_INDEX;
    ASSERT_TRUE((dct ? parseDictionary(dctThenAscii, storage, &arena, &rootIndex)
                     : parseDictionary(flateThenAscii, storage, &arena, &rootIndex))
                    .ok());
    PdfImageObjectDescriptor descriptor;
    const PdfStatus status = pdfParseImageObject(arena, {rootIndex, {0, 1, 1}}, &descriptor);
    ASSERT_TRUE(status.ok());
    EXPECT_EQ(descriptor.disposition, PdfImageDisposition::OmitUnsupported);
    EXPECT_EQ(descriptor.omitReason.error, PdfError::UnsupportedFilter);
  }
}

TEST(PdfImageObjectFilters, StructurallyValidatesOverCapPipelinesBeforeOmittingWithoutStorage) {
  static constexpr char validOverCap[] =
      "<< /W 1 /H 1 /CS /DeviceGray /BPC 8 /Filter [/AHx /A85 /AHx /A85 /Fl] >>";
  static constexpr char malformedOverCap[] =
      "<< /W 1 /H 1 /CS /DeviceGray /BPC 8 /Filter [/AHx /A85 /AHx /A85 7] >>";

  {
    ArenaStorage storage;
    PdfObjectArena arena;
    uint16_t rootIndex = PDF_INVALID_INDEX;
    ASSERT_TRUE(parseDictionary(validOverCap, storage, &arena, &rootIndex).ok());
    PdfImageObjectDescriptor descriptor;

    const PdfStatus status = pdfParseImageObject(arena, {rootIndex, {0, 1, 1}}, &descriptor);

    ASSERT_TRUE(status.ok());
    EXPECT_EQ(descriptor.disposition, PdfImageDisposition::OmitUnsupported);
    EXPECT_EQ(descriptor.omitReason.error, PdfError::LimitExceeded);
    EXPECT_EQ(descriptor.stream.decoderFilterCount, 0U);
  }
  {
    ArenaStorage storage;
    PdfObjectArena arena;
    uint16_t rootIndex = PDF_INVALID_INDEX;
    ASSERT_TRUE(parseDictionary(malformedOverCap, storage, &arena, &rootIndex).ok());
    PdfImageObjectDescriptor descriptor;

    const PdfStatus status = pdfParseImageObject(arena, {rootIndex, {0, 1, 1}}, &descriptor);

    EXPECT_EQ(status.error, PdfError::Malformed);
  }
}

TEST(PdfImageObjectFilters, OmitsDctDecodeParametersAndNonEightBitSamples) {
  static constexpr char colorTransform[] =
      "<< /W 10 /H 10 /CS /DeviceRGB /BPC 8 /F /DCT /DP << /ColorTransform 0 >> >>";
  static constexpr char nonEightBit[] = "<< /W 10 /H 10 /CS /DeviceGray /BPC 4 /F /DCT >>";
  for (const bool nonEight : {false, true}) {
    ArenaStorage storage;
    PdfObjectArena arena;
    uint16_t rootIndex = PDF_INVALID_INDEX;
    ASSERT_TRUE((nonEight ? parseDictionary(nonEightBit, storage, &arena, &rootIndex)
                          : parseDictionary(colorTransform, storage, &arena, &rootIndex))
                    .ok());
    PdfImageObjectDescriptor descriptor;

    ASSERT_TRUE(pdfParseImageObject(arena, {rootIndex, {0, 1, 1}}, &descriptor).ok());

    EXPECT_EQ(descriptor.disposition, PdfImageDisposition::OmitUnsupported);
    EXPECT_EQ(descriptor.omitReason.error, PdfError::UnsupportedEncoding);
  }
}

TEST(PdfImageObjectFilters, OmitsOptionalUnsupportedTerminalCodecsAndLzw) {
  static constexpr char jpx[] = "<< /W 1 /H 1 /CS /DeviceRGB /BPC 8 /F /JPXDecode >>";
  static constexpr char jbig[] = "<< /W 1 /H 1 /CS /DeviceGray /BPC 1 /F /JBIG2Decode >>";
  static constexpr char ccitt[] = "<< /W 1 /H 1 /CS /DeviceGray /BPC 1 /F /CCITTFaxDecode >>";
  static constexpr char lzw[] = "<< /W 1 /H 1 /CS /DeviceGray /BPC 8 /F /LZW >>";
  for (const uint8_t fixture : {0U, 1U, 2U, 3U}) {
    ArenaStorage storage;
    PdfObjectArena arena;
    uint16_t rootIndex = PDF_INVALID_INDEX;
    PdfStatus parsed;
    if (fixture == 0U) {
      parsed = parseDictionary(jpx, storage, &arena, &rootIndex);
    } else if (fixture == 1U) {
      parsed = parseDictionary(jbig, storage, &arena, &rootIndex);
    } else if (fixture == 2U) {
      parsed = parseDictionary(ccitt, storage, &arena, &rootIndex);
    } else {
      parsed = parseDictionary(lzw, storage, &arena, &rootIndex);
    }
    ASSERT_TRUE(parsed.ok());
    PdfImageObjectDescriptor descriptor;

    ASSERT_TRUE(pdfParseImageObject(arena, {rootIndex, {0, 1, 1}}, &descriptor).ok());

    EXPECT_EQ(descriptor.disposition, PdfImageDisposition::OmitUnsupported);
    EXPECT_EQ(descriptor.stream.terminalCodec, PdfImageTerminalCodec::UnsupportedOptional);
    EXPECT_EQ(descriptor.stream.target, PdfImageStreamTarget::None);
    EXPECT_EQ(descriptor.omitReason.error, PdfError::UnsupportedFilter);
  }
}

TEST(PdfImageObjectPredictors, MapsTiffAndPngParametersOnlyWhenTheyMatchTheRaster) {
  static constexpr char tiff[] =
      "<< /W 32 /H 2 /CS /DeviceGray /BPC 4 /F /Fl "
      "/DP << /Predictor 2 /Colors 1 /BitsPerComponent 4 /Columns 32 >> >>";
  static constexpr char png[] =
      "<< /W 10 /H 2 /CS /DeviceRGB /BPC 8 /F [/A85 /Fl] "
      "/DP [null << /Predictor 15 /Colors 3 /BitsPerComponent 8 /Columns 10 >>] >>";
  for (const bool isPng : {false, true}) {
    ArenaStorage storage;
    PdfObjectArena arena;
    uint16_t rootIndex = PDF_INVALID_INDEX;
    ASSERT_TRUE(
        (isPng ? parseDictionary(png, storage, &arena, &rootIndex) : parseDictionary(tiff, storage, &arena, &rootIndex))
            .ok());
    PdfImageObjectDescriptor descriptor;

    ASSERT_TRUE(pdfParseImageObject(arena, {rootIndex, {0, 100, 100}}, &descriptor).ok());

    EXPECT_EQ(descriptor.parameters.predictor, isPng ? 15U : 2U);
    EXPECT_EQ(descriptor.disposition, PdfImageDisposition::Ready);
  }
}

TEST(PdfImageObjectPredictors, AcceptsTheCompleteBoundedPredictorSetAndRetainsConsistencyInputs) {
  static constexpr char dictionary[] =
      "<< /W 10 /H 2 /CS /DeviceRGB /BPC 8 /F /Fl "
      "/DP << /Predictor 10 /Colors 3 /BitsPerComponent 8 /Columns 10 >> >>";
  ArenaStorage storage;
  PdfObjectArena arena;
  uint16_t rootIndex = PDF_INVALID_INDEX;
  ASSERT_TRUE(parseDictionary(dictionary, storage, &arena, &rootIndex).ok());
  uint16_t decodeParametersIndex = PDF_INVALID_INDEX;
  uint16_t predictorIndex = PDF_INVALID_INDEX;
  ASSERT_TRUE(pdfDictionaryFind(arena, rootIndex, "DP", &decodeParametersIndex));
  ASSERT_TRUE(pdfDictionaryFind(arena, decodeParametersIndex, "Predictor", &predictorIndex));

  for (const uint8_t predictor : {1U, 2U, 10U, 11U, 12U, 13U, 14U, 15U}) {
    arena.values[predictorIndex].integerValue = predictor;
    PdfImageObjectDescriptor descriptor;

    ASSERT_TRUE(pdfParseImageObject(arena, {rootIndex, {0, 1, 1}}, &descriptor).ok());

    EXPECT_EQ(descriptor.disposition, PdfImageDisposition::Ready);
    EXPECT_EQ(descriptor.parameters.predictor, predictor);
    if (predictor != 1U) {
      EXPECT_EQ(descriptor.predictorColors, 3U);
      EXPECT_EQ(descriptor.predictorBitsPerComponent, 8U);
      EXPECT_EQ(descriptor.predictorColumns, 10U);
    }
  }
}

TEST(PdfImageObjectPredictors, OmitsUnsupportedOrInconsistentPredictorParameters) {
  static constexpr char unsupported[] =
      "<< /W 10 /H 2 /CS /DeviceGray /BPC 8 /F /Fl "
      "/DP << /Predictor 9 /Colors 1 /BitsPerComponent 8 /Columns 10 >> >>";
  static constexpr char columnsMismatch[] =
      "<< /W 10 /H 2 /CS /DeviceRGB /BPC 8 /F /Fl "
      "/DP << /Predictor 12 /Colors 3 /BitsPerComponent 8 /Columns 9 >> >>";
  for (const bool mismatch : {false, true}) {
    ArenaStorage storage;
    PdfObjectArena arena;
    uint16_t rootIndex = PDF_INVALID_INDEX;
    ASSERT_TRUE((mismatch ? parseDictionary(columnsMismatch, storage, &arena, &rootIndex)
                          : parseDictionary(unsupported, storage, &arena, &rootIndex))
                    .ok());
    PdfImageObjectDescriptor descriptor;

    ASSERT_TRUE(pdfParseImageObject(arena, {rootIndex, {0, 1, 1}}, &descriptor).ok());

    EXPECT_EQ(descriptor.disposition, PdfImageDisposition::OmitUnsupported);
    EXPECT_EQ(descriptor.omitReason.error, PdfError::UnsupportedEncoding);
  }
}

TEST(PdfImageObjectDecode, OmitsPartialInversionButAcceptsExactRealNormalRanges) {
  static constexpr char partial[] = "<< /W 1 /H 1 /CS /DeviceRGB /BPC 8 /F /Fl /D [1 0 0 1 1 0] >>";
  static constexpr char realNormal[] = "<< /W 1 /H 1 /CS /DeviceGray /BPC 8 /F /Fl /D [0.0 1.0] >>";
  {
    ArenaStorage storage;
    PdfObjectArena arena;
    uint16_t rootIndex = PDF_INVALID_INDEX;
    ASSERT_TRUE(parseDictionary(partial, storage, &arena, &rootIndex).ok());
    PdfImageObjectDescriptor descriptor;
    ASSERT_TRUE(pdfParseImageObject(arena, {rootIndex, {0, 1, 1}}, &descriptor).ok());
    EXPECT_EQ(descriptor.disposition, PdfImageDisposition::OmitUnsupported);
    EXPECT_EQ(descriptor.omitReason.error, PdfError::UnsupportedEncoding);
  }
  {
    ArenaStorage storage;
    PdfObjectArena arena;
    uint16_t rootIndex = PDF_INVALID_INDEX;
    ASSERT_TRUE(parseDictionary(realNormal, storage, &arena, &rootIndex).ok());
    PdfImageObjectDescriptor descriptor;
    ASSERT_TRUE(pdfParseImageObject(arena, {rootIndex, {0, 1, 1}}, &descriptor).ok());
    EXPECT_EQ(descriptor.disposition, PdfImageDisposition::Ready);
    EXPECT_EQ(descriptor.parameters.decode, PdfImageDecode::Normal);
  }
}

TEST(PdfImageObjectResolution, AppliesOnlySameSizeOneBitAndGrayEightAuxiliaryMasks) {
  PdfImageObjectDescriptor base;
  base.parameters.width = 100;
  base.parameters.height = 50;
  base.unresolved = PdfImageUnresolved::ExplicitMask | PdfImageUnresolved::SoftMask;
  base.disposition = PdfImageDisposition::NeedsResolution;
  base.hasExplicitMask = true;
  base.hasSoftMaskReference = true;

  PdfImageObjectDescriptor explicitMask;
  explicitMask.parameters.width = 100;
  explicitMask.parameters.height = 50;
  explicitMask.parameters.bitsPerComponent = 1;
  explicitMask.parameters.colorSpace = PdfImageColorSpace::ImageMask;
  explicitMask.parameters.decode = PdfImageDecode::Inverted;
  ASSERT_TRUE(pdfApplyResolvedImageAuxiliary(&base, explicitMask, PdfImageAuxiliaryKind::ExplicitMask).ok());
  EXPECT_FALSE(pdfImageHasUnresolved(base.unresolved, PdfImageUnresolved::ExplicitMask));
  EXPECT_EQ(base.disposition, PdfImageDisposition::NeedsResolution);

  PdfImageObjectDescriptor softMask;
  softMask.parameters.width = 100;
  softMask.parameters.height = 50;
  softMask.parameters.bitsPerComponent = 8;
  softMask.parameters.colorSpace = PdfImageColorSpace::Gray;
  softMask.parameters.decode = PdfImageDecode::Inverted;
  ASSERT_TRUE(pdfApplyResolvedImageAuxiliary(&base, softMask, PdfImageAuxiliaryKind::SoftMask).ok());
  EXPECT_FALSE(pdfImageHasUnresolved(base.unresolved, PdfImageUnresolved::SoftMask));
  EXPECT_EQ(base.disposition, PdfImageDisposition::Ready);
  EXPECT_TRUE(base.parameters.hasSoftMask);
  EXPECT_EQ(base.parameters.softMaskDecode, PdfImageDecode::Inverted);
}

TEST(PdfImageObjectResolution, OmitsValidButUnsupportedResolvedMasks) {
  PdfImageObjectDescriptor base;
  base.parameters.width = 10;
  base.parameters.height = 10;
  base.unresolved = PdfImageUnresolved::SoftMask;
  base.disposition = PdfImageDisposition::NeedsResolution;
  base.hasSoftMaskReference = true;

  PdfImageObjectDescriptor wrongSize;
  wrongSize.parameters.width = 9;
  wrongSize.parameters.height = 10;
  wrongSize.parameters.bitsPerComponent = 8;
  wrongSize.parameters.colorSpace = PdfImageColorSpace::Gray;
  ASSERT_TRUE(pdfApplyResolvedImageAuxiliary(&base, wrongSize, PdfImageAuxiliaryKind::SoftMask).ok());
  EXPECT_EQ(base.disposition, PdfImageDisposition::OmitUnsupported);
  EXPECT_EQ(base.omitReason.error, PdfError::UnsupportedEncoding);
  EXPECT_EQ(base.stream.target, PdfImageStreamTarget::None);

  base.disposition = PdfImageDisposition::NeedsResolution;
  base.unresolved = PdfImageUnresolved::SoftMask;
  PdfImageObjectDescriptor wrongBits = wrongSize;
  wrongBits.parameters.width = 10;
  wrongBits.parameters.bitsPerComponent = 4;
  ASSERT_TRUE(pdfApplyResolvedImageAuxiliary(&base, wrongBits, PdfImageAuxiliaryKind::SoftMask).ok());
  EXPECT_EQ(base.disposition, PdfImageDisposition::OmitUnsupported);
  EXPECT_EQ(base.omitReason.error, PdfError::UnsupportedEncoding);

  PdfImageObjectDescriptor explicitBase;
  explicitBase.parameters.width = 10;
  explicitBase.parameters.height = 10;
  explicitBase.unresolved = PdfImageUnresolved::ExplicitMask;
  explicitBase.disposition = PdfImageDisposition::NeedsResolution;
  explicitBase.hasExplicitMask = true;
  PdfImageObjectDescriptor wrongExplicitSize;
  wrongExplicitSize.parameters.width = 10;
  wrongExplicitSize.parameters.height = 9;
  wrongExplicitSize.parameters.bitsPerComponent = 1;
  wrongExplicitSize.parameters.colorSpace = PdfImageColorSpace::ImageMask;

  ASSERT_TRUE(
      pdfApplyResolvedImageAuxiliary(&explicitBase, wrongExplicitSize, PdfImageAuxiliaryKind::ExplicitMask).ok());
  EXPECT_EQ(explicitBase.disposition, PdfImageDisposition::OmitUnsupported);
  EXPECT_EQ(explicitBase.omitReason.error, PdfError::UnsupportedEncoding);
}

TEST(PdfImageObjectLimits, OmitsSixteenMegapixelAndEightKibibyteRowViolationsWithCheckedMath) {
  static constexpr char pixels[] = "<< /W 5000 /H 4000 /CS /DeviceGray /BPC 8 /F /Fl >>";
  static constexpr char row[] = "<< /W 3000 /H 1 /CS /DeviceRGB /BPC 8 /F /Fl >>";
  for (const bool rowLimit : {false, true}) {
    ArenaStorage storage;
    PdfObjectArena arena;
    uint16_t rootIndex = PDF_INVALID_INDEX;
    ASSERT_TRUE((rowLimit ? parseDictionary(row, storage, &arena, &rootIndex)
                          : parseDictionary(pixels, storage, &arena, &rootIndex))
                    .ok());
    PdfImageObjectDescriptor descriptor;
    ASSERT_TRUE(pdfParseImageObject(arena, {rootIndex, {0, 1, 1}}, &descriptor).ok());
    EXPECT_EQ(descriptor.disposition, PdfImageDisposition::OmitUnsupported);
    EXPECT_EQ(descriptor.omitReason.error, PdfError::LimitExceeded);
  }
}

TEST(PdfImageObjectLimits, AcceptsExactPixelRowAndPaletteBoundaries) {
  static constexpr char boundary[] = "<< /W 4000 /H 4000 /CS /DeviceGray /BPC 8 /F /Fl >>";
  {
    ArenaStorage storage;
    PdfObjectArena arena;
    uint16_t rootIndex = PDF_INVALID_INDEX;
    ASSERT_TRUE(parseDictionary(boundary, storage, &arena, &rootIndex).ok());
    PdfImageObjectDescriptor descriptor;
    ASSERT_TRUE(pdfParseImageObject(arena, {rootIndex, {0, 1, 1}}, &descriptor).ok());
    EXPECT_EQ(descriptor.disposition, PdfImageDisposition::Ready);
  }

  static constexpr char indexed[] = "<< /W 1 /H 1 /CS [/Indexed /DeviceRGB 3 <000000000000>] /BPC 8 /F /Fl >>";
  ArenaStorage storage;
  PdfObjectArena arena;
  uint16_t rootIndex = PDF_INVALID_INDEX;
  ASSERT_TRUE(parseDictionary(indexed, storage, &arena, &rootIndex).ok());
  uint16_t colorSpaceIndex = PDF_INVALID_INDEX;
  uint16_t highIndex = PDF_INVALID_INDEX;
  uint16_t paletteIndex = PDF_INVALID_INDEX;
  ASSERT_TRUE(pdfDictionaryFind(arena, rootIndex, "CS", &colorSpaceIndex));
  ASSERT_TRUE(pdfArrayAt(arena, colorSpaceIndex, 2, &highIndex));
  ASSERT_TRUE(pdfArrayAt(arena, colorSpaceIndex, 3, &paletteIndex));
  arena.values[highIndex].integerValue = 255;
  ASSERT_LE(static_cast<size_t>(arena.textLength) + 768U, storage.text.size());
  arena.values[paletteIndex].textOffset = arena.textLength;
  arena.values[paletteIndex].textLength = 768;
  for (uint16_t index = 0; index < 768U; ++index) {
    arena.text[arena.textLength + index] = static_cast<uint8_t>(index);
  }
  arena.textLength = static_cast<uint16_t>(arena.textLength + 768U);
  std::array<uint8_t, 768> palette{};
  PdfImageObjectDescriptor descriptor;

  ASSERT_TRUE(pdfParseImageObject(arena, {rootIndex, {0, 1, 1}, palette.data(), palette.size()}, &descriptor).ok());

  EXPECT_EQ(descriptor.disposition, PdfImageDisposition::Ready);
  EXPECT_EQ(descriptor.parameters.paletteEntries, 256U);
  EXPECT_EQ(descriptor.parameters.paletteBytes, 768U);
  EXPECT_EQ(palette[767], static_cast<uint8_t>(767));
}

TEST(PdfImageObjectValidation, RejectsMalformedValuesPaletteCapacityAndInvalidStreamRanges) {
  static constexpr char missingWidth[] = "<< /H 1 /CS /DeviceGray /BPC 8 /F /Fl >>";
  static constexpr char shortPalette[] = "<< /W 1 /H 1 /CS [/Indexed /DeviceRGB 255 <00>] /BPC 8 /F /Fl >>";
  {
    ArenaStorage storage;
    PdfObjectArena arena;
    uint16_t rootIndex = PDF_INVALID_INDEX;
    ASSERT_TRUE(parseDictionary(missingWidth, storage, &arena, &rootIndex).ok());
    PdfImageObjectDescriptor descriptor;
    EXPECT_EQ(pdfParseImageObject(arena, {rootIndex, {0, 1, 1}}, &descriptor).error, PdfError::Malformed);
  }
  {
    ArenaStorage storage;
    PdfObjectArena arena;
    uint16_t rootIndex = PDF_INVALID_INDEX;
    ASSERT_TRUE(parseDictionary(shortPalette, storage, &arena, &rootIndex).ok());
    std::array<uint8_t, 8> palette{};
    PdfImageObjectDescriptor descriptor;
    const PdfStatus status =
        pdfParseImageObject(arena, {rootIndex, {0, 1, 1}, palette.data(), palette.size()}, &descriptor);
    EXPECT_EQ(status.error, PdfError::Malformed);
  }
  {
    static constexpr char valid[] = "<< /W 1 /H 1 /CS /DeviceGray /BPC 8 /F /Fl >>";
    ArenaStorage storage;
    PdfObjectArena arena;
    uint16_t rootIndex = PDF_INVALID_INDEX;
    ASSERT_TRUE(parseDictionary(valid, storage, &arena, &rootIndex).ok());
    PdfImageObjectDescriptor descriptor;
    EXPECT_EQ(pdfParseImageObject(arena, {rootIndex, {9, 2, 10}}, &descriptor).error, PdfError::InvalidOffset);
    EXPECT_EQ(pdfParseImageObject(arena, {rootIndex, {UINT64_MAX, 2, UINT64_MAX}}, &descriptor).error,
              PdfError::InvalidOffset);
  }
}

TEST(PdfImageObjectValidation, KeepsSyntacticallyCorruptFilterMetadataFatal) {
  static constexpr char nonNameFilter[] =
      "<< /W 1 /H 1 /CS /DeviceGray /BPC 8 /F [/FlateDecode 42] >>";
  static constexpr char nonDictionaryParameters[] =
      "<< /W 1 /H 1 /CS /DeviceRGB /BPC 8 /F /DCT /DP 7 >>";
  static constexpr char unsupportedPipelineBadParameters[] =
      "<< /W 1 /H 1 /CS /DeviceRGB /BPC 8 /F [/Fl /DCT] /DP 7 >>";
  for (const uint8_t fixture : {0U, 1U, 2U}) {
    ArenaStorage storage;
    PdfObjectArena arena;
    uint16_t rootIndex = PDF_INVALID_INDEX;
    PdfStatus parsed;
    if (fixture == 0U) {
      parsed = parseDictionary(nonNameFilter, storage, &arena, &rootIndex);
    } else if (fixture == 1U) {
      parsed = parseDictionary(nonDictionaryParameters, storage, &arena, &rootIndex);
    } else {
      parsed = parseDictionary(unsupportedPipelineBadParameters, storage, &arena, &rootIndex);
    }
    ASSERT_TRUE(parsed.ok());
    PdfImageObjectDescriptor descriptor;

    const PdfStatus status = pdfParseImageObject(arena, {rootIndex, {0, 1, 1}}, &descriptor);

    EXPECT_EQ(status.error, PdfError::Malformed);
  }
}

TEST(PdfImageObjectAllocationWitness, ParsesAResolvedIndexedObjectWithoutHeapChurn) {
  static constexpr char dictionary[] = "<< /W 4 /H 1 /CS [/Indexed /DeviceGray 3 <0055AAFF>] /BPC 2 /F /Fl >>";
  ArenaStorage storage;
  PdfObjectArena arena;
  uint16_t rootIndex = PDF_INVALID_INDEX;
  ASSERT_TRUE(parseDictionary(dictionary, storage, &arena, &rootIndex).ok());
  std::array<uint8_t, 768> palette{};
  PdfImageObjectDescriptor descriptor;
  gNewCount.store(0, std::memory_order_relaxed);
  gMallocCount.store(0, std::memory_order_relaxed);
  gFreeCount.store(0, std::memory_order_relaxed);
  gTrackAllocations.store(true, std::memory_order_relaxed);

  const PdfStatus status =
      pdfParseImageObject(arena, {rootIndex, {0, 4, 4}, palette.data(), palette.size()}, &descriptor);

  gTrackAllocations.store(false, std::memory_order_relaxed);
  EXPECT_TRUE(status.ok());
  EXPECT_EQ(gNewCount.load(std::memory_order_relaxed), 0U);
  EXPECT_EQ(gMallocCount.load(std::memory_order_relaxed), 0U);
  EXPECT_EQ(gFreeCount.load(std::memory_order_relaxed), 0U);
}

}  // namespace

void* operator new(const size_t size) {
  if (gTrackAllocations.load(std::memory_order_relaxed)) {
    gNewCount.fetch_add(1, std::memory_order_relaxed);
  }
  if (void* const pointer = trackedMalloc(size)) {
    return pointer;
  }
  throw std::bad_alloc();
}

void* operator new[](const size_t size) { return ::operator new(size); }

void operator delete(void* const pointer) noexcept { trackedFree(pointer); }
void operator delete[](void* const pointer) noexcept { trackedFree(pointer); }
void operator delete(void* const pointer, size_t) noexcept { trackedFree(pointer); }
void operator delete[](void* const pointer, size_t) noexcept { trackedFree(pointer); }

#if defined(PDF_IMAGE_OBJECT_WRAP_MALLOC)
extern "C" void* __wrap_malloc(const size_t size) {
  if (gTrackAllocations.load(std::memory_order_relaxed)) {
    gMallocCount.fetch_add(1, std::memory_order_relaxed);
  }
  return __real_malloc(size);
}

extern "C" void __wrap_free(void* const pointer) {
  if (gTrackAllocations.load(std::memory_order_relaxed)) {
    gFreeCount.fetch_add(1, std::memory_order_relaxed);
  }
  __real_free(pointer);
}
#endif
