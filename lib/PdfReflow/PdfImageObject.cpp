#include "PdfImageObject.h"

#include <cstring>

#include "PdfCheckedMath.h"
#include "PdfStreamDecoder.h"

namespace {

enum class ParsedFilter : uint8_t {
  ASCIIHex,
  ASCII85,
  Flate,
  Dct,
  Lzw,
  Jpx,
  Jbig2,
  Ccitt,
  Unknown,
};

bool findValue(const PdfObjectArena& arena, const uint16_t dictionaryIndex, const char* const key,
               const char* const alias, uint16_t* const valueIndex) {
  return pdfDictionaryFind(arena, dictionaryIndex, key, valueIndex) ||
         (alias != nullptr && pdfDictionaryFind(arena, dictionaryIndex, alias, valueIndex));
}

bool optionalNameEquals(const PdfObjectArena& arena, const uint16_t dictionaryIndex, const char* const key,
                        const char* const expected) {
  uint16_t valueIndex = PDF_INVALID_INDEX;
  return !pdfDictionaryFind(arena, dictionaryIndex, key, &valueIndex) ||
         (valueIndex < arena.valueCount && pdfTextEquals(arena, arena.values[valueIndex], expected));
}

void addUnresolved(PdfImageObjectDescriptor* const descriptor, const PdfImageUnresolved field) {
  descriptor->unresolved = descriptor->unresolved | field;
}

bool colorSpaceUnresolved(const PdfImageObjectDescriptor& descriptor) {
  return pdfImageHasUnresolved(descriptor.unresolved, PdfImageUnresolved::ColorSpace) ||
         pdfImageHasUnresolved(descriptor.unresolved, PdfImageUnresolved::IndexedBaseColorSpace);
}

void resetDescriptor(PdfImageObjectDescriptor* const descriptor) {
  descriptor->parameters.width = 0;
  descriptor->parameters.height = 0;
  descriptor->parameters.maximumOutputWidth = 0;
  descriptor->parameters.maximumOutputHeight = 0;
  descriptor->parameters.maximumOutputBytes = 0;
  descriptor->parameters.palette = nullptr;
  descriptor->parameters.paletteBytes = 0;
  descriptor->parameters.paletteEntries = 0;
  descriptor->parameters.bitsPerComponent = 0;
  descriptor->parameters.predictor = 0;
  descriptor->parameters.imageMaskPaintLuminance = 0;
  descriptor->parameters.colorSpace = PdfImageColorSpace::Gray;
  descriptor->parameters.decode = PdfImageDecode::Normal;
  descriptor->parameters.hasSoftMask = false;
  descriptor->parameters.softMaskDecode = PdfImageDecode::Normal;
  descriptor->stream.offset = 0;
  descriptor->stream.length = 0;
  for (PdfStreamFilter& filter : descriptor->stream.decoderFilters) {
    filter = PdfStreamFilter::ASCIIHex;
  }
  descriptor->stream.decoderFilterCount = 0;
  descriptor->stream.terminalCodec = PdfImageTerminalCodec::Raster;
  descriptor->stream.target = PdfImageStreamTarget::ExtractorDecoded;
  descriptor->colorSpaceReference = {};
  descriptor->indexedBaseColorSpaceReference = {};
  descriptor->paletteReference = {};
  descriptor->explicitMaskReference = {};
  descriptor->softMaskReference = {};
  descriptor->omitReason = PdfStatus::success();
  descriptor->predictorColumns = 0;
  descriptor->decodeUpperValue = 0;
  descriptor->paletteBytesRequired = 0;
  descriptor->predictorColors = 0;
  descriptor->predictorBitsPerComponent = 0;
  descriptor->decodeComponentPairs = 0;
  descriptor->unresolved = PdfImageUnresolved::None;
  descriptor->disposition = PdfImageDisposition::Ready;
  descriptor->hasExplicitMask = false;
  descriptor->hasSoftMaskReference = false;
}

PdfStatus omit(PdfImageObjectDescriptor* const descriptor, const PdfError reason, const uint64_t offset = 0) {
  descriptor->disposition = PdfImageDisposition::OmitUnsupported;
  descriptor->omitReason = PdfStatus::failure(reason, offset);
  descriptor->stream.decoderFilterCount = 0;
  descriptor->stream.terminalCodec = PdfImageTerminalCodec::UnsupportedOptional;
  descriptor->stream.target = PdfImageStreamTarget::None;
  return PdfStatus::success();
}

bool indexedColorSpace(const PdfImageColorSpace colorSpace) {
  return colorSpace == PdfImageColorSpace::IndexedGray || colorSpace == PdfImageColorSpace::IndexedRGB;
}

PdfStatus validateDecodeForColorSpace(PdfImageObjectDescriptor* const descriptor) {
  if (descriptor->decodeComponentPairs == 0U) {
    return PdfStatus::success();
  }
  const uint8_t expectedPairs = descriptor->parameters.colorSpace == PdfImageColorSpace::RGB ? 3U : 1U;
  if (descriptor->decodeComponentPairs != expectedPairs) {
    return omit(descriptor, PdfError::UnsupportedEncoding, descriptor->decodeComponentPairs);
  }

  uint32_t expectedUpper = 1U;
  if (indexedColorSpace(descriptor->parameters.colorSpace)) {
    const uint8_t bits = descriptor->parameters.bitsPerComponent;
    if (bits >= 32U) {
      return omit(descriptor, PdfError::UnsupportedEncoding, bits);
    }
    expectedUpper = (1U << bits) - 1U;
  }
  if (descriptor->decodeUpperValue != expectedUpper) {
    return omit(descriptor, PdfError::UnsupportedEncoding, descriptor->decodeUpperValue);
  }
  return PdfStatus::success();
}

PdfStatus readDimension(const PdfObjectArena& arena, const uint16_t dictionaryIndex, const char* const key,
                        const char* const alias, uint32_t* const dimension) {
  uint16_t valueIndex = PDF_INVALID_INDEX;
  if (dimension == nullptr || !findValue(arena, dictionaryIndex, key, alias, &valueIndex) ||
      valueIndex >= arena.valueCount) {
    return PdfStatus::failure(PdfError::Malformed, dictionaryIndex);
  }
  const PdfValue& value = arena.values[valueIndex];
  if (value.kind != PdfValueKind::Integer || value.integerValue <= 0) {
    return PdfStatus::failure(PdfError::Malformed, valueIndex);
  }
  if (value.integerValue > PdfLimits::MaxImageDimension) {
    return PdfStatus::failure(PdfError::LimitExceeded, static_cast<uint64_t>(value.integerValue));
  }
  *dimension = static_cast<uint32_t>(value.integerValue);
  return PdfStatus::success();
}

bool readInteger(const PdfObjectArena& arena, const uint16_t dictionaryIndex, const char* const key,
                 const int64_t defaultValue, int64_t* const result) {
  uint16_t valueIndex = PDF_INVALID_INDEX;
  if (!pdfDictionaryFind(arena, dictionaryIndex, key, &valueIndex)) {
    *result = defaultValue;
    return true;
  }
  if (valueIndex >= arena.valueCount || arena.values[valueIndex].kind != PdfValueKind::Integer) {
    return false;
  }
  *result = arena.values[valueIndex].integerValue;
  return true;
}

bool readExactUnsignedDecodeEndpoint(const PdfValue& value, uint32_t* const endpoint) {
  if (endpoint == nullptr) {
    return false;
  }
  if (value.kind == PdfValueKind::Integer) {
    if (value.integerValue < 0 || static_cast<uint64_t>(value.integerValue) > UINT32_MAX) {
      return false;
    }
    *endpoint = static_cast<uint32_t>(value.integerValue);
    return true;
  }
  if (value.kind != PdfValueKind::Real || value.fixedValue < 0 || value.fixedValue % 65536 != 0) {
    return false;
  }
  *endpoint = static_cast<uint32_t>(value.fixedValue / 65536);
  return true;
}

PdfObjectReference referenceOf(const PdfValue& value) { return {value.objectNumber, value.generation}; }

bool isName(const PdfObjectArena& arena, const PdfValue& value, const char* const longName,
            const char* const alias = nullptr) {
  return pdfTextEquals(arena, value, longName) || (alias != nullptr && pdfTextEquals(arena, value, alias));
}

PdfStatus parseIndexedColorSpace(const PdfObjectArena& arena, const uint16_t arrayIndex,
                                 const PdfImageObjectParseInput& input, PdfImageObjectDescriptor* const descriptor) {
  const PdfValue& array = arena.values[arrayIndex];
  if (array.count != 4) {
    return PdfStatus::failure(PdfError::Malformed, arrayIndex);
  }
  uint16_t familyIndex = PDF_INVALID_INDEX;
  uint16_t baseIndex = PDF_INVALID_INDEX;
  uint16_t highIndex = PDF_INVALID_INDEX;
  uint16_t paletteIndex = PDF_INVALID_INDEX;
  if (!pdfArrayAt(arena, arrayIndex, 0, &familyIndex) || !pdfArrayAt(arena, arrayIndex, 1, &baseIndex) ||
      !pdfArrayAt(arena, arrayIndex, 2, &highIndex) || !pdfArrayAt(arena, arrayIndex, 3, &paletteIndex) ||
      familyIndex >= arena.valueCount || baseIndex >= arena.valueCount || highIndex >= arena.valueCount ||
      paletteIndex >= arena.valueCount || arena.values[familyIndex].kind != PdfValueKind::Name ||
      !isName(arena, arena.values[familyIndex], "Indexed", "I")) {
    return PdfStatus::failure(PdfError::Malformed, arrayIndex);
  }
  const PdfValue& high = arena.values[highIndex];
  if (high.kind != PdfValueKind::Integer || high.integerValue < 0 || high.integerValue > 255) {
    return PdfStatus::failure(PdfError::Malformed, highIndex);
  }
  descriptor->parameters.paletteEntries = static_cast<uint16_t>(high.integerValue + 1);

  uint8_t paletteComponents = 0;
  const PdfValue& base = arena.values[baseIndex];
  if (base.kind == PdfValueKind::Reference) {
    descriptor->indexedBaseColorSpaceReference = referenceOf(base);
    addUnresolved(descriptor, PdfImageUnresolved::IndexedBaseColorSpace);
  } else if (base.kind != PdfValueKind::Name) {
    return PdfStatus::failure(PdfError::Malformed, baseIndex);
  } else if (isName(arena, base, "DeviceGray", "G")) {
    descriptor->parameters.colorSpace = PdfImageColorSpace::IndexedGray;
    paletteComponents = 1;
  } else if (isName(arena, base, "DeviceRGB", "RGB")) {
    descriptor->parameters.colorSpace = PdfImageColorSpace::IndexedRGB;
    paletteComponents = 3;
  } else {
    return omit(descriptor, PdfError::UnsupportedEncoding, baseIndex);
  }

  const PdfValue& palette = arena.values[paletteIndex];
  if (paletteComponents != 0) {
    const uint32_t required = static_cast<uint32_t>(descriptor->parameters.paletteEntries) * paletteComponents;
    descriptor->paletteBytesRequired = static_cast<uint16_t>(required);
  }
  if (palette.kind == PdfValueKind::Reference) {
    descriptor->paletteReference = referenceOf(palette);
    addUnresolved(descriptor, PdfImageUnresolved::IndexedPalette);
    return PdfStatus::success();
  }
  if (palette.kind != PdfValueKind::String) {
    return PdfStatus::failure(PdfError::Malformed, paletteIndex);
  }

  size_t required = descriptor->paletteBytesRequired;
  if (required == 0) {
    // Preserve the direct lookup while the base color space is unresolved.
    // DeviceGray needs one byte per entry and DeviceRGB needs three; the outer
    // phased resolver can decide which contract applies without retaining this
    // arena. The lookup itself remains bounded at 768 bytes.
    const size_t minimum = descriptor->parameters.paletteEntries;
    const size_t maximum = static_cast<size_t>(descriptor->parameters.paletteEntries) * 3U;
    if (palette.textLength < minimum || palette.textLength > maximum) {
      return PdfStatus::failure(PdfError::Malformed, paletteIndex);
    }
    required = palette.textLength;
  }
  if (palette.textLength < required || static_cast<uint32_t>(palette.textOffset) + required > arena.textLength) {
    return PdfStatus::failure(PdfError::Malformed, paletteIndex);
  }
  if (input.palette == nullptr || input.paletteCapacity < required) {
    return PdfStatus::failure(PdfError::InsufficientMemory, required);
  }
  std::memcpy(input.palette, arena.text + palette.textOffset, required);
  descriptor->parameters.palette = input.palette;
  descriptor->parameters.paletteBytes = required;
  return PdfStatus::success();
}

PdfStatus parseColorSpace(const PdfObjectArena& arena, const uint16_t valueIndex, const PdfImageObjectParseInput& input,
                          PdfImageObjectDescriptor* const descriptor) {
  if (valueIndex >= arena.valueCount) {
    return PdfStatus::failure(PdfError::Malformed, valueIndex);
  }
  const PdfValue& value = arena.values[valueIndex];
  if (value.kind == PdfValueKind::Reference) {
    descriptor->colorSpaceReference = referenceOf(value);
    addUnresolved(descriptor, PdfImageUnresolved::ColorSpace);
    return PdfStatus::success();
  }
  if (value.kind == PdfValueKind::Array) {
    uint16_t familyIndex = PDF_INVALID_INDEX;
    if (!pdfArrayAt(arena, valueIndex, 0, &familyIndex) || familyIndex >= arena.valueCount ||
        arena.values[familyIndex].kind != PdfValueKind::Name) {
      return PdfStatus::failure(PdfError::Malformed, valueIndex);
    }
    if (!isName(arena, arena.values[familyIndex], "Indexed", "I")) {
      return omit(descriptor, PdfError::UnsupportedEncoding, valueIndex);
    }
    return parseIndexedColorSpace(arena, valueIndex, input, descriptor);
  }
  if (value.kind != PdfValueKind::Name) {
    return PdfStatus::failure(PdfError::Malformed, valueIndex);
  }
  if (isName(arena, value, "DeviceGray", "G")) {
    descriptor->parameters.colorSpace = PdfImageColorSpace::Gray;
    return PdfStatus::success();
  }
  if (isName(arena, value, "DeviceRGB", "RGB")) {
    descriptor->parameters.colorSpace = PdfImageColorSpace::RGB;
    return PdfStatus::success();
  }
  return omit(descriptor, PdfError::UnsupportedEncoding, valueIndex);
}

uint8_t sampleComponents(const PdfImageColorSpace colorSpace) {
  return colorSpace == PdfImageColorSpace::RGB ? 3U : 1U;
}

bool supportedBits(const PdfImageColorSpace colorSpace, const uint8_t bits) {
  if (colorSpace == PdfImageColorSpace::RGB) {
    return bits == 8U;
  }
  return bits == 1U || bits == 2U || bits == 4U || bits == 8U;
}

PdfStatus parseDecode(const PdfObjectArena& arena, const uint16_t dictionaryIndex,
                      PdfImageObjectDescriptor* const descriptor) {
  uint16_t decodeIndex = PDF_INVALID_INDEX;
  if (!findValue(arena, dictionaryIndex, "Decode", "D", &decodeIndex)) {
    descriptor->parameters.decode = PdfImageDecode::Normal;
    return PdfStatus::success();
  }
  if (decodeIndex >= arena.valueCount || arena.values[decodeIndex].kind != PdfValueKind::Array) {
    return PdfStatus::failure(PdfError::Malformed, decodeIndex);
  }
  const PdfValue& decode = arena.values[decodeIndex];
  const uint16_t expectedCount = descriptor->parameters.colorSpace == PdfImageColorSpace::RGB ? 6U : 2U;
  if ((!colorSpaceUnresolved(*descriptor) && decode.count != expectedCount) ||
      (colorSpaceUnresolved(*descriptor) && decode.count != 2U && decode.count != 6U)) {
    return PdfStatus::failure(PdfError::Malformed, decodeIndex);
  }
  descriptor->decodeComponentPairs = static_cast<uint8_t>(decode.count / 2U);
  bool orientationSet = false;
  bool inverted = false;
  uint32_t upper = 0;
  for (uint16_t ordinal = 0; ordinal < decode.count; ordinal += 2U) {
    uint16_t firstIndex = PDF_INVALID_INDEX;
    uint16_t secondIndex = PDF_INVALID_INDEX;
    if (!pdfArrayAt(arena, decodeIndex, ordinal, &firstIndex) ||
        !pdfArrayAt(arena, decodeIndex, static_cast<uint16_t>(ordinal + 1U), &secondIndex) ||
        firstIndex >= arena.valueCount || secondIndex >= arena.valueCount) {
      return PdfStatus::failure(PdfError::Malformed, decodeIndex);
    }
    const PdfValue& first = arena.values[firstIndex];
    const PdfValue& second = arena.values[secondIndex];
    if ((first.kind != PdfValueKind::Integer && first.kind != PdfValueKind::Real) ||
        (second.kind != PdfValueKind::Integer && second.kind != PdfValueKind::Real)) {
      const uint16_t malformedIndex =
          first.kind != PdfValueKind::Integer && first.kind != PdfValueKind::Real ? firstIndex : secondIndex;
      return PdfStatus::failure(PdfError::Malformed, malformedIndex);
    }
    uint32_t firstEndpoint = 0;
    uint32_t secondEndpoint = 0;
    if (!readExactUnsignedDecodeEndpoint(first, &firstEndpoint) ||
        !readExactUnsignedDecodeEndpoint(second, &secondEndpoint)) {
      return omit(descriptor, PdfError::UnsupportedEncoding, firstIndex);
    }
    const bool pairNormal = firstEndpoint == 0U && secondEndpoint != 0U;
    const bool pairInverted = firstEndpoint != 0U && secondEndpoint == 0U;
    if (!pairNormal && !pairInverted) {
      return omit(descriptor, PdfError::UnsupportedEncoding, firstIndex);
    }
    const uint32_t pairUpper = pairInverted ? firstEndpoint : secondEndpoint;
    if (!orientationSet) {
      orientationSet = true;
      inverted = pairInverted;
      upper = pairUpper;
    } else if (inverted != pairInverted || upper != pairUpper) {
      return omit(descriptor, PdfError::UnsupportedEncoding, firstIndex);
    }
  }
  descriptor->decodeUpperValue = upper;
  descriptor->parameters.decode = inverted ? PdfImageDecode::Inverted : PdfImageDecode::Normal;
  return colorSpaceUnresolved(*descriptor) ? PdfStatus::success() : validateDecodeForColorSpace(descriptor);
}

ParsedFilter parseFilterName(const PdfObjectArena& arena, const PdfValue& value) {
  if (isName(arena, value, "ASCIIHexDecode", "AHx")) {
    return ParsedFilter::ASCIIHex;
  }
  if (isName(arena, value, "ASCII85Decode", "A85")) {
    return ParsedFilter::ASCII85;
  }
  if (isName(arena, value, "FlateDecode", "Fl")) {
    return ParsedFilter::Flate;
  }
  if (isName(arena, value, "DCTDecode", "DCT")) {
    return ParsedFilter::Dct;
  }
  if (isName(arena, value, "LZWDecode", "LZW")) {
    return ParsedFilter::Lzw;
  }
  if (isName(arena, value, "JPXDecode")) {
    return ParsedFilter::Jpx;
  }
  if (isName(arena, value, "JBIG2Decode")) {
    return ParsedFilter::Jbig2;
  }
  if (isName(arena, value, "CCITTFaxDecode", "CCF")) {
    return ParsedFilter::Ccitt;
  }
  return ParsedFilter::Unknown;
}

PdfStatus filterValueAt(const PdfObjectArena& arena, const uint16_t filterIndex, const uint16_t ordinal,
                        uint16_t* const valueIndex, uint16_t* const count) {
  if (filterIndex >= arena.valueCount || valueIndex == nullptr || count == nullptr) {
    return PdfStatus::failure(PdfError::Malformed, filterIndex);
  }
  const PdfValue& filter = arena.values[filterIndex];
  if (filter.kind == PdfValueKind::Name) {
    *count = 1;
    if (ordinal != 0) {
      return PdfStatus::failure(PdfError::Malformed, ordinal);
    }
    *valueIndex = filterIndex;
    return PdfStatus::success();
  }
  if (filter.kind != PdfValueKind::Array) {
    return PdfStatus::failure(PdfError::Malformed, filterIndex);
  }
  *count = filter.count;
  if (ordinal >= *count || !pdfArrayAt(arena, filterIndex, ordinal, valueIndex) || *valueIndex >= arena.valueCount ||
      arena.values[*valueIndex].kind != PdfValueKind::Name) {
    return PdfStatus::failure(PdfError::Malformed, filterIndex);
  }
  return PdfStatus::success();
}

PdfStatus parseFilters(const PdfObjectArena& arena, const uint16_t dictionaryIndex,
                       PdfImageObjectDescriptor* const descriptor, uint16_t* const filterValueIndex,
                       uint16_t* const filterCount) {
  if (!findValue(arena, dictionaryIndex, "Filter", "F", filterValueIndex)) {
    *filterValueIndex = PDF_INVALID_INDEX;
    *filterCount = 0;
    return PdfStatus::success();
  }
  if (*filterValueIndex >= arena.valueCount) {
    return PdfStatus::failure(PdfError::Malformed, *filterValueIndex);
  }
  if (arena.values[*filterValueIndex].kind == PdfValueKind::Array &&
      arena.values[*filterValueIndex].count == 0U) {
    *filterCount = 0;
    return PdfStatus::success();
  }

  uint16_t filterNameIndices[PdfLimits::MaxFiltersPerStream]{};
  uint16_t firstIndex = PDF_INVALID_INDEX;
  PdfStatus status = filterValueAt(arena, *filterValueIndex, 0, &firstIndex, filterCount);
  if (!status.ok()) {
    return status;
  }
  const bool overCap = *filterCount > PdfLimits::MaxFiltersPerStream;
  if (!overCap) {
    filterNameIndices[0] = firstIndex;
  }
  // Validate the entire declared pipeline before classifying a valid pipeline
  // as optional/unsupported. A supported first stage must not hide corrupt
  // later filter metadata.
  for (uint16_t ordinal = 1; ordinal < *filterCount; ++ordinal) {
    uint16_t nameIndex = PDF_INVALID_INDEX;
    uint16_t ignoredCount = 0;
    status = filterValueAt(arena, *filterValueIndex, ordinal, &nameIndex, &ignoredCount);
    if (!status.ok()) {
      return status;
    }
    if (!overCap) {
      filterNameIndices[ordinal] = nameIndex;
    }
  }
  if (overCap) {
    return omit(descriptor, PdfError::LimitExceeded, *filterCount);
  }

  if (*filterCount == 1U && parseFilterName(arena, arena.values[filterNameIndices[0]]) == ParsedFilter::Dct) {
    descriptor->stream.terminalCodec = PdfImageTerminalCodec::DctJpeg;
    descriptor->stream.target = PdfImageStreamTarget::JpegBytes;
    return PdfStatus::success();
  }

  bool flateSeen = false;
  for (uint16_t ordinal = 0; ordinal < *filterCount; ++ordinal) {
    const ParsedFilter parsed = parseFilterName(arena, arena.values[filterNameIndices[ordinal]]);
    const bool last = ordinal + 1U == *filterCount;
    if (parsed == ParsedFilter::Dct) {
      return omit(descriptor, PdfError::UnsupportedFilter, ordinal);
    }
    if (parsed == ParsedFilter::Lzw || parsed == ParsedFilter::Jpx || parsed == ParsedFilter::Jbig2 ||
        parsed == ParsedFilter::Ccitt || parsed == ParsedFilter::Unknown) {
      return omit(descriptor, PdfError::UnsupportedFilter, ordinal);
    }
    if (parsed == ParsedFilter::Flate) {
      if (flateSeen || !last) {
        return omit(descriptor, PdfError::UnsupportedFilter, ordinal);
      }
      flateSeen = true;
      descriptor->stream.decoderFilters[descriptor->stream.decoderFilterCount++] = PdfStreamFilter::Flate;
      continue;
    }
    if (flateSeen) {
      return omit(descriptor, PdfError::UnsupportedFilter, ordinal);
    }
    descriptor->stream.decoderFilters[descriptor->stream.decoderFilterCount++] =
        parsed == ParsedFilter::ASCIIHex ? PdfStreamFilter::ASCIIHex : PdfStreamFilter::ASCII85;
  }
  return PdfStatus::success();
}

PdfStatus decodeParametersAt(const PdfObjectArena& arena, const uint16_t decodeParametersIndex,
                             const uint16_t filterCount, const uint16_t ordinal, uint16_t* const parameterIndex) {
  if (decodeParametersIndex == PDF_INVALID_INDEX) {
    *parameterIndex = PDF_INVALID_INDEX;
    return PdfStatus::success();
  }
  if (decodeParametersIndex >= arena.valueCount) {
    return PdfStatus::failure(PdfError::Malformed, decodeParametersIndex);
  }
  const PdfValue& parameters = arena.values[decodeParametersIndex];
  if (parameters.kind == PdfValueKind::Dictionary) {
    if (filterCount != 1 || ordinal != 0) {
      return PdfStatus::failure(PdfError::Malformed, decodeParametersIndex);
    }
    *parameterIndex = decodeParametersIndex;
    return PdfStatus::success();
  }
  if (parameters.kind != PdfValueKind::Array || parameters.count != filterCount ||
      !pdfArrayAt(arena, decodeParametersIndex, ordinal, parameterIndex) || *parameterIndex >= arena.valueCount) {
    return PdfStatus::failure(PdfError::Malformed, decodeParametersIndex);
  }
  const PdfValueKind kind = arena.values[*parameterIndex].kind;
  if (kind != PdfValueKind::Dictionary && kind != PdfValueKind::Null) {
    return PdfStatus::failure(PdfError::Malformed, *parameterIndex);
  }
  return PdfStatus::success();
}

PdfStatus parseDecodeParameters(const PdfObjectArena& arena, const uint16_t dictionaryIndex,
                                 const uint16_t filterValueIndex, const uint16_t filterCount,
                                 PdfObjectReference* const decodeParametersReference,
                                 PdfImageObjectDescriptor* const descriptor) {
  uint16_t decodeParametersIndex = PDF_INVALID_INDEX;
  findValue(arena, dictionaryIndex, "DecodeParms", "DP", &decodeParametersIndex);
  if (filterCount == 0) {
    return decodeParametersIndex == PDF_INVALID_INDEX ? PdfStatus::success()
                                                      : PdfStatus::failure(PdfError::Malformed, decodeParametersIndex);
  }
  if (decodeParametersIndex == PDF_INVALID_INDEX) {
    return PdfStatus::success();
  }
  if (decodeParametersIndex >= arena.valueCount) {
    return PdfStatus::failure(PdfError::Malformed, decodeParametersIndex);
  }
  const PdfValue& decodeParameters = arena.values[decodeParametersIndex];
  if (decodeParameters.kind == PdfValueKind::Reference) {
    if (decodeParametersReference == nullptr) {
      return omit(descriptor, PdfError::UnsupportedEncoding, decodeParametersIndex);
    }
    *decodeParametersReference = referenceOf(decodeParameters);
    addUnresolved(descriptor, PdfImageUnresolved::DecodeParameters);
    return PdfStatus::success();
  }

  for (uint16_t ordinal = 0; ordinal < filterCount; ++ordinal) {
    uint16_t nameIndex = PDF_INVALID_INDEX;
    uint16_t ignored = 0;
    PdfStatus status = filterValueAt(arena, filterValueIndex, ordinal, &nameIndex, &ignored);
    if (!status.ok()) {
      return status;
    }
    uint16_t parameterIndex = PDF_INVALID_INDEX;
    status = decodeParametersAt(arena, decodeParametersIndex, filterCount, ordinal, &parameterIndex);
    if (!status.ok()) {
      return status;
    }
    const ParsedFilter filter = parseFilterName(arena, arena.values[nameIndex]);
    if (filter == ParsedFilter::Dct) {
      if (parameterIndex != PDF_INVALID_INDEX && arena.values[parameterIndex].kind != PdfValueKind::Null) {
        return omit(descriptor, PdfError::UnsupportedEncoding, parameterIndex);
      }
      continue;
    }
    if (filter != ParsedFilter::Flate || parameterIndex == PDF_INVALID_INDEX ||
        arena.values[parameterIndex].kind == PdfValueKind::Null) {
      continue;
    }

    int64_t predictor = 1;
    if (!readInteger(arena, parameterIndex, "Predictor", 1, &predictor) || predictor < 1 || predictor > 255) {
      return PdfStatus::failure(PdfError::Malformed, parameterIndex);
    }
    if (predictor != 1 && predictor != 2 && (predictor < 10 || predictor > 15)) {
      return omit(descriptor, PdfError::UnsupportedEncoding, static_cast<uint64_t>(predictor));
    }
    descriptor->parameters.predictor = static_cast<uint8_t>(predictor);
    if (predictor == 1) {
      continue;
    }

    int64_t colors = 1;
    int64_t bits = 8;
    int64_t columns = 1;
    if (!readInteger(arena, parameterIndex, "Colors", 1, &colors) ||
        !readInteger(arena, parameterIndex, "BitsPerComponent", 8, &bits) ||
        !readInteger(arena, parameterIndex, "Columns", 1, &columns) || colors <= 0 || bits <= 0 || columns <= 0) {
      return PdfStatus::failure(PdfError::Malformed, parameterIndex);
    }
    if (colors > UINT8_MAX || bits > UINT8_MAX || columns > UINT32_MAX) {
      return PdfStatus::failure(PdfError::Malformed, parameterIndex);
    }
    descriptor->predictorColors = static_cast<uint8_t>(colors);
    descriptor->predictorBitsPerComponent = static_cast<uint8_t>(bits);
    descriptor->predictorColumns = static_cast<uint32_t>(columns);
    if (!colorSpaceUnresolved(*descriptor) &&
        (colors != sampleComponents(descriptor->parameters.colorSpace) ||
         bits != descriptor->parameters.bitsPerComponent || columns != descriptor->parameters.width)) {
      return omit(descriptor, PdfError::UnsupportedEncoding, parameterIndex);
    }
  }
  return PdfStatus::success();
}

PdfStatus parseReferenceField(const PdfObjectArena& arena, const uint16_t dictionaryIndex, const char* const key,
                              const PdfImageUnresolved unresolvedField, PdfObjectReference* const reference,
                              bool* const present, PdfImageObjectDescriptor* const descriptor) {
  uint16_t valueIndex = PDF_INVALID_INDEX;
  if (!pdfDictionaryFind(arena, dictionaryIndex, key, &valueIndex)) {
    return PdfStatus::success();
  }
  if (valueIndex >= arena.valueCount) {
    return PdfStatus::failure(PdfError::Malformed, valueIndex);
  }
  const PdfValue& value = arena.values[valueIndex];
  if (value.kind == PdfValueKind::Name && pdfTextEquals(arena, value, "None")) {
    return PdfStatus::success();
  }
  if (value.kind == PdfValueKind::Array && std::strcmp(key, "Mask") == 0) {
    return omit(descriptor, PdfError::UnsupportedEncoding, valueIndex);
  }
  if (value.kind != PdfValueKind::Reference) {
    return PdfStatus::failure(PdfError::Malformed, valueIndex);
  }
  *reference = referenceOf(value);
  *present = true;
  addUnresolved(descriptor, unresolvedField);
  return PdfStatus::success();
}

PdfStatus validateRowAndBits(PdfImageObjectDescriptor* const descriptor) {
  if (colorSpaceUnresolved(*descriptor)) {
    return PdfStatus::success();
  }
  if (!supportedBits(descriptor->parameters.colorSpace, descriptor->parameters.bitsPerComponent)) {
    return omit(descriptor, PdfError::UnsupportedEncoding, descriptor->parameters.bitsPerComponent);
  }
  uint64_t rowBits = 0;
  if (!pdfCheckedMultiply(descriptor->parameters.width, sampleComponents(descriptor->parameters.colorSpace),
                          &rowBits) ||
      !pdfCheckedMultiply(rowBits, descriptor->parameters.bitsPerComponent, &rowBits)) {
    return omit(descriptor, PdfError::LimitExceeded);
  }
  const uint64_t rowBytes = rowBits / 8U + (rowBits % 8U == 0U ? 0U : 1U);
  if (rowBytes == 0 || rowBytes > PdfLimits::MaxDecodedImageRowBytes) {
    return omit(descriptor, PdfError::LimitExceeded, rowBytes);
  }
  return PdfStatus::success();
}

}  // namespace

PdfStatus pdfApplyResolvedImageColorSpace(PdfImageObjectDescriptor* const descriptor,
                                          const PdfImageColorSpace resolvedColorSpace) {
  if (descriptor == nullptr || descriptor->disposition != PdfImageDisposition::NeedsResolution) {
    return PdfStatus::failure(PdfError::InvalidArgument);
  }
  const bool resolvingColorSpace =
      pdfImageHasUnresolved(descriptor->unresolved, PdfImageUnresolved::ColorSpace);
  const bool resolvingIndexedBase =
      pdfImageHasUnresolved(descriptor->unresolved, PdfImageUnresolved::IndexedBaseColorSpace);
  if (resolvingColorSpace == resolvingIndexedBase) {
    return PdfStatus::failure(PdfError::InvalidArgument);
  }

  PdfImageColorSpace effectiveColorSpace = resolvedColorSpace;
  PdfImageUnresolved resolvedField = PdfImageUnresolved::ColorSpace;
  uint16_t paletteBytesRequired = descriptor->paletteBytesRequired;
  if (resolvingColorSpace) {
    // A standalone reference needs the referenced Indexed dictionary and
    // palette facts, not just its family name. This bounded apply step accepts
    // the complete DeviceGray/DeviceRGB resolutions only.
    if (resolvedColorSpace != PdfImageColorSpace::Gray && resolvedColorSpace != PdfImageColorSpace::RGB) {
      return PdfStatus::failure(PdfError::InvalidArgument);
    }
  } else {
    resolvedField = PdfImageUnresolved::IndexedBaseColorSpace;
    if (resolvedColorSpace == PdfImageColorSpace::Gray) {
      effectiveColorSpace = PdfImageColorSpace::IndexedGray;
    } else if (resolvedColorSpace == PdfImageColorSpace::RGB) {
      effectiveColorSpace = PdfImageColorSpace::IndexedRGB;
    } else {
      return PdfStatus::failure(PdfError::InvalidArgument);
    }
    const uint16_t components = effectiveColorSpace == PdfImageColorSpace::IndexedRGB ? 3U : 1U;
    const uint32_t required = static_cast<uint32_t>(descriptor->parameters.paletteEntries) * components;
    if (required == 0U || required > UINT16_MAX) {
      return PdfStatus::failure(PdfError::Malformed);
    }
    paletteBytesRequired = static_cast<uint16_t>(required);
    if (!pdfImageHasUnresolved(descriptor->unresolved, PdfImageUnresolved::IndexedPalette) &&
        (descriptor->parameters.palette == nullptr || descriptor->parameters.paletteBytes < required)) {
      return PdfStatus::failure(PdfError::Malformed, required);
    }
  }

  descriptor->parameters.colorSpace = effectiveColorSpace;
  descriptor->paletteBytesRequired = paletteBytesRequired;
  descriptor->unresolved = static_cast<PdfImageUnresolved>(
      static_cast<uint8_t>(descriptor->unresolved) & ~static_cast<uint8_t>(resolvedField));
  if (resolvingColorSpace) {
    descriptor->colorSpaceReference = {};
  } else {
    descriptor->indexedBaseColorSpaceReference = {};
  }

  PdfStatus status = validateRowAndBits(descriptor);
  if (!status.ok() || descriptor->disposition == PdfImageDisposition::OmitUnsupported) {
    return status;
  }
  status = validateDecodeForColorSpace(descriptor);
  if (!status.ok() || descriptor->disposition == PdfImageDisposition::OmitUnsupported) {
    return status;
  }
  if (descriptor->parameters.predictor != 1U &&
      (descriptor->predictorColors != sampleComponents(effectiveColorSpace) ||
       descriptor->predictorBitsPerComponent != descriptor->parameters.bitsPerComponent ||
       descriptor->predictorColumns != descriptor->parameters.width)) {
    return omit(descriptor, PdfError::UnsupportedEncoding);
  }
  if (descriptor->stream.terminalCodec == PdfImageTerminalCodec::DctJpeg &&
      (descriptor->parameters.bitsPerComponent != 8U ||
       (effectiveColorSpace != PdfImageColorSpace::Gray && effectiveColorSpace != PdfImageColorSpace::RGB) ||
       descriptor->parameters.decode != PdfImageDecode::Normal)) {
    return omit(descriptor, PdfError::UnsupportedEncoding);
  }

  descriptor->disposition =
      descriptor->unresolved == PdfImageUnresolved::None ? PdfImageDisposition::Ready
                                                         : PdfImageDisposition::NeedsResolution;
  return PdfStatus::success();
}

PdfStatus pdfApplyResolvedImageDecodeParameters(const PdfObjectArena& arena, const uint16_t valueIndex,
                                                 PdfImageObjectDescriptor* const descriptor) {
  if (descriptor == nullptr || valueIndex >= arena.valueCount ||
      !pdfImageHasUnresolved(descriptor->unresolved, PdfImageUnresolved::DecodeParameters)) {
    return PdfStatus::failure(PdfError::InvalidArgument, valueIndex);
  }

  const uint16_t filterCount = descriptor->stream.terminalCodec == PdfImageTerminalCodec::DctJpeg
                                   ? 1U
                                   : descriptor->stream.decoderFilterCount;
  if (filterCount == 0U) {
    return PdfStatus::failure(PdfError::Malformed, valueIndex);
  }

  for (uint16_t ordinal = 0; ordinal < filterCount; ++ordinal) {
    uint16_t parameterIndex = valueIndex;
    const PdfValue& root = arena.values[valueIndex];
    if (root.kind == PdfValueKind::Dictionary) {
      if (filterCount != 1U) {
        return PdfStatus::failure(PdfError::Malformed, valueIndex);
      }
    } else if (root.kind == PdfValueKind::Array) {
      if (root.count != filterCount || !pdfArrayAt(arena, valueIndex, ordinal, &parameterIndex) ||
          parameterIndex >= arena.valueCount) {
        return PdfStatus::failure(PdfError::Malformed, valueIndex);
      }
    } else {
      return PdfStatus::failure(PdfError::Malformed, valueIndex);
    }

    const PdfValueKind parameterKind = arena.values[parameterIndex].kind;
    if (parameterKind == PdfValueKind::Null) {
      continue;
    }
    if (parameterKind != PdfValueKind::Dictionary) {
      return PdfStatus::failure(PdfError::Malformed, parameterIndex);
    }

    const bool dct = descriptor->stream.terminalCodec == PdfImageTerminalCodec::DctJpeg;
    const bool flate = !dct && descriptor->stream.decoderFilters[ordinal] == PdfStreamFilter::Flate;
    if (dct) {
      return omit(descriptor, PdfError::UnsupportedEncoding, parameterIndex);
    }
    if (!flate) {
      continue;
    }

    int64_t predictor = 1;
    if (!readInteger(arena, parameterIndex, "Predictor", 1, &predictor) || predictor < 1 || predictor > 255) {
      return PdfStatus::failure(PdfError::Malformed, parameterIndex);
    }
    if (predictor != 1 && predictor != 2 && (predictor < 10 || predictor > 15)) {
      return omit(descriptor, PdfError::UnsupportedEncoding, static_cast<uint64_t>(predictor));
    }
    descriptor->parameters.predictor = static_cast<uint8_t>(predictor);
    if (predictor == 1) {
      continue;
    }

    int64_t colors = 1;
    int64_t bits = 8;
    int64_t columns = 1;
    if (!readInteger(arena, parameterIndex, "Colors", 1, &colors) ||
        !readInteger(arena, parameterIndex, "BitsPerComponent", 8, &bits) ||
        !readInteger(arena, parameterIndex, "Columns", 1, &columns) || colors <= 0 || bits <= 0 || columns <= 0 ||
        colors > UINT8_MAX || bits > UINT8_MAX || columns > UINT32_MAX) {
      return PdfStatus::failure(PdfError::Malformed, parameterIndex);
    }
    descriptor->predictorColors = static_cast<uint8_t>(colors);
    descriptor->predictorBitsPerComponent = static_cast<uint8_t>(bits);
    descriptor->predictorColumns = static_cast<uint32_t>(columns);
    if (!colorSpaceUnresolved(*descriptor) &&
        (colors != sampleComponents(descriptor->parameters.colorSpace) ||
         bits != descriptor->parameters.bitsPerComponent || columns != descriptor->parameters.width)) {
      return omit(descriptor, PdfError::UnsupportedEncoding, parameterIndex);
    }
  }

  descriptor->unresolved = static_cast<PdfImageUnresolved>(
      static_cast<uint8_t>(descriptor->unresolved) & ~static_cast<uint8_t>(PdfImageUnresolved::DecodeParameters));
  descriptor->disposition = descriptor->unresolved == PdfImageUnresolved::None ? PdfImageDisposition::Ready
                                                                                : PdfImageDisposition::NeedsResolution;
  return PdfStatus::success();
}

PdfStatus pdfApplyResolvedImageAuxiliary(PdfImageObjectDescriptor* const base,
                                         const PdfImageObjectDescriptor& auxiliary, const PdfImageAuxiliaryKind kind) {
  if (base == nullptr ||
      (kind != PdfImageAuxiliaryKind::ExplicitMask && kind != PdfImageAuxiliaryKind::SoftMask)) {
    return PdfStatus::failure(PdfError::InvalidArgument);
  }
  const PdfImageUnresolved field =
      kind == PdfImageAuxiliaryKind::ExplicitMask ? PdfImageUnresolved::ExplicitMask : PdfImageUnresolved::SoftMask;
  if (!pdfImageHasUnresolved(base->unresolved, field) ||
      (kind == PdfImageAuxiliaryKind::ExplicitMask && !base->hasExplicitMask) ||
      (kind == PdfImageAuxiliaryKind::SoftMask && !base->hasSoftMaskReference)) {
    return PdfStatus::failure(PdfError::InvalidArgument);
  }
  if (auxiliary.disposition == PdfImageDisposition::OmitUnsupported) {
    return omit(base, auxiliary.omitReason.error, auxiliary.omitReason.offset);
  }
  if (auxiliary.disposition != PdfImageDisposition::Ready ||
      auxiliary.unresolved != PdfImageUnresolved::None) {
    return PdfStatus::failure(PdfError::InvalidArgument);
  }
  if (base->parameters.width != auxiliary.parameters.width || base->parameters.height != auxiliary.parameters.height ||
      auxiliary.hasExplicitMask || auxiliary.hasSoftMaskReference || auxiliary.parameters.hasSoftMask) {
    return omit(base, PdfError::UnsupportedEncoding);
  }
  if (kind == PdfImageAuxiliaryKind::ExplicitMask) {
    if (auxiliary.parameters.colorSpace != PdfImageColorSpace::ImageMask ||
        auxiliary.parameters.bitsPerComponent != 1U) {
      return omit(base, PdfError::UnsupportedEncoding);
    }
  } else {
    if (auxiliary.parameters.colorSpace != PdfImageColorSpace::Gray || auxiliary.parameters.bitsPerComponent != 8U) {
      return omit(base, PdfError::UnsupportedEncoding);
    }
    base->parameters.hasSoftMask = true;
    base->parameters.softMaskDecode = auxiliary.parameters.decode;
  }
  base->unresolved =
      static_cast<PdfImageUnresolved>(static_cast<uint8_t>(base->unresolved) & ~static_cast<uint8_t>(field));
  base->disposition =
      base->unresolved == PdfImageUnresolved::None ? PdfImageDisposition::Ready : PdfImageDisposition::NeedsResolution;
  return PdfStatus::success();
}

PdfStatus pdfParseImageObject(const PdfObjectArena& arena, const PdfImageObjectParseInput& input,
                              PdfImageObjectDescriptor* const descriptor) {
  if (descriptor == nullptr || arena.values == nullptr || input.dictionaryIndex >= arena.valueCount ||
      arena.values[input.dictionaryIndex].kind != PdfValueKind::Dictionary || input.stream.length == 0) {
    return PdfStatus::failure(PdfError::InvalidArgument);
  }
  resetDescriptor(descriptor);
  descriptor->stream.offset = input.stream.offset;
  descriptor->stream.length = input.stream.length;
  descriptor->parameters.predictor = 1;

  uint64_t streamEnd = 0;
  if (!pdfCheckedAdd(input.stream.offset, input.stream.length, &streamEnd) || streamEnd > input.stream.sourceSize) {
    return PdfStatus::failure(PdfError::InvalidOffset, input.stream.offset);
  }
  if (!optionalNameEquals(arena, input.dictionaryIndex, "Type", "XObject") ||
      !optionalNameEquals(arena, input.dictionaryIndex, "Subtype", "Image")) {
    return PdfStatus::failure(PdfError::Malformed, input.dictionaryIndex);
  }

  PdfStatus status = readDimension(arena, input.dictionaryIndex, "Width", "W", &descriptor->parameters.width);
  if (!status.ok()) {
    return status.error == PdfError::LimitExceeded ? omit(descriptor, status.error, status.offset) : status;
  }
  status = readDimension(arena, input.dictionaryIndex, "Height", "H", &descriptor->parameters.height);
  if (!status.ok()) {
    return status.error == PdfError::LimitExceeded ? omit(descriptor, status.error, status.offset) : status;
  }
  uint64_t pixels = 0;
  if (!pdfCheckedMultiply(descriptor->parameters.width, descriptor->parameters.height, &pixels) ||
      pixels > PdfLimits::MaxImagePixels) {
    return omit(descriptor, PdfError::LimitExceeded, pixels);
  }

  bool imageMask = false;
  uint16_t imageMaskIndex = PDF_INVALID_INDEX;
  if (findValue(arena, input.dictionaryIndex, "ImageMask", "IM", &imageMaskIndex)) {
    if (imageMaskIndex >= arena.valueCount || arena.values[imageMaskIndex].kind != PdfValueKind::Boolean) {
      return PdfStatus::failure(PdfError::Malformed, imageMaskIndex);
    }
    imageMask = arena.values[imageMaskIndex].booleanValue;
  }

  uint16_t bitsIndex = PDF_INVALID_INDEX;
  const bool hasBits = findValue(arena, input.dictionaryIndex, "BitsPerComponent", "BPC", &bitsIndex);
  if (imageMask) {
    uint16_t ignoredColorSpace = PDF_INVALID_INDEX;
    if (findValue(arena, input.dictionaryIndex, "ColorSpace", "CS", &ignoredColorSpace)) {
      return PdfStatus::failure(PdfError::Malformed, ignoredColorSpace);
    }
    if (hasBits && (bitsIndex >= arena.valueCount || arena.values[bitsIndex].kind != PdfValueKind::Integer ||
                    arena.values[bitsIndex].integerValue != 1)) {
      return PdfStatus::failure(PdfError::Malformed, bitsIndex);
    }
    descriptor->parameters.colorSpace = PdfImageColorSpace::ImageMask;
    descriptor->parameters.bitsPerComponent = 1;
  } else {
    uint16_t colorSpaceIndex = PDF_INVALID_INDEX;
    if (!findValue(arena, input.dictionaryIndex, "ColorSpace", "CS", &colorSpaceIndex)) {
      return PdfStatus::failure(PdfError::Malformed, input.dictionaryIndex);
    }
    status = parseColorSpace(arena, colorSpaceIndex, input, descriptor);
    if (!status.ok() || descriptor->disposition == PdfImageDisposition::OmitUnsupported) {
      return status;
    }
    if (!hasBits || bitsIndex >= arena.valueCount || arena.values[bitsIndex].kind != PdfValueKind::Integer ||
        arena.values[bitsIndex].integerValue <= 0 || arena.values[bitsIndex].integerValue > 255) {
      return PdfStatus::failure(PdfError::Malformed, bitsIndex);
    }
    descriptor->parameters.bitsPerComponent = static_cast<uint8_t>(arena.values[bitsIndex].integerValue);
  }

  status = validateRowAndBits(descriptor);
  if (!status.ok() || descriptor->disposition == PdfImageDisposition::OmitUnsupported) {
    return status;
  }
  status = parseDecode(arena, input.dictionaryIndex, descriptor);
  if (!status.ok() || descriptor->disposition == PdfImageDisposition::OmitUnsupported) {
    return status;
  }

  uint16_t filterValueIndex = PDF_INVALID_INDEX;
  uint16_t filterCount = 0;
  status = parseFilters(arena, input.dictionaryIndex, descriptor, &filterValueIndex, &filterCount);
  if (!status.ok()) {
    return status;
  }
  status = parseDecodeParameters(arena, input.dictionaryIndex, filterValueIndex, filterCount,
                                 input.decodeParametersReference, descriptor);
  if (!status.ok()) {
    return status;
  }
  if (descriptor->disposition == PdfImageDisposition::OmitUnsupported) {
    return PdfStatus::success();
  }
  if (descriptor->stream.terminalCodec == PdfImageTerminalCodec::DctJpeg &&
      (descriptor->parameters.bitsPerComponent != 8U ||
       (descriptor->parameters.colorSpace != PdfImageColorSpace::Gray &&
        descriptor->parameters.colorSpace != PdfImageColorSpace::RGB &&
        !colorSpaceUnresolved(*descriptor)) ||
       descriptor->parameters.decode != PdfImageDecode::Normal)) {
    return omit(descriptor, PdfError::UnsupportedEncoding);
  }

  status = parseReferenceField(arena, input.dictionaryIndex, "Mask", PdfImageUnresolved::ExplicitMask,
                               &descriptor->explicitMaskReference, &descriptor->hasExplicitMask, descriptor);
  if (!status.ok() || descriptor->disposition == PdfImageDisposition::OmitUnsupported) {
    return status;
  }
  status = parseReferenceField(arena, input.dictionaryIndex, "SMask", PdfImageUnresolved::SoftMask,
                               &descriptor->softMaskReference, &descriptor->hasSoftMaskReference, descriptor);
  if (!status.ok() || descriptor->disposition == PdfImageDisposition::OmitUnsupported) {
    return status;
  }
  if (imageMask && (descriptor->hasExplicitMask || descriptor->hasSoftMaskReference)) {
    return omit(descriptor, PdfError::UnsupportedEncoding);
  }
  if (descriptor->hasExplicitMask && descriptor->hasSoftMaskReference) {
    return omit(descriptor, PdfError::UnsupportedEncoding);
  }
  if (descriptor->unresolved != PdfImageUnresolved::None) {
    descriptor->disposition = PdfImageDisposition::NeedsResolution;
  }
  return PdfStatus::success();
}
