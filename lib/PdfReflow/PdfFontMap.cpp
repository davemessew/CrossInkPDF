#include "PdfFontMap.h"

#include <cstring>
#include <limits>

namespace {

bool valueAsWidth(const PdfValue& value, int32_t* width) {
  if (width == nullptr) {
    return false;
  }
  if (value.kind == PdfValueKind::Integer && value.integerValue >= std::numeric_limits<int32_t>::min() &&
      value.integerValue <= std::numeric_limits<int32_t>::max()) {
    *width = static_cast<int32_t>(value.integerValue);
    return true;
  }
  if (value.kind == PdfValueKind::Real) {
    if (value.fixedValue < 0) {
      return false;
    }
    *width = value.fixedValue / 65536;
    return true;
  }
  return false;
}

bool valueAsCode(const PdfValue& value, uint32_t* code) {
  if (code == nullptr || value.kind != PdfValueKind::Integer || value.integerValue < 0 ||
      value.integerValue > std::numeric_limits<uint32_t>::max()) {
    return false;
  }
  *code = static_cast<uint32_t>(value.integerValue);
  return true;
}

uint64_t glyphKey(const uint32_t sourceCode, const uint8_t sourceLength) {
  return static_cast<uint64_t>(sourceLength) << 32U | sourceCode;
}

bool sourceCodeFitsLength(const uint32_t sourceCode, const uint8_t sourceLength) {
  return sourceLength >= 1U && sourceLength <= 4U &&
         (sourceLength == 4U || sourceCode < (uint32_t{1} << (sourceLength * 8U)));
}

bool sourceCodeIsPrefix(const uint32_t prefixCode, const uint8_t prefixLength, const uint32_t code,
                        const uint8_t codeLength) {
  if (prefixLength >= codeLength) {
    return false;
  }
  return code >> ((codeLength - prefixLength) * 8U) == prefixCode;
}

}  // namespace

PdfStatus PdfFontMap::setSourceAccess(const bool required) {
  if (workspace_.setSourceAccess != nullptr) {
    const PdfStatus status = workspace_.setSourceAccess(workspace_.sourceAccessContext, required);
    if (!status.ok()) {
      return status;
    }
  }
  sourceAccessRequired_ = required;
  return PdfStatus::success();
}

PdfStatus PdfFontMap::begin(const uint16_t fontId, const bool cid, PdfCMap* const toUnicode,
                            PdfSimpleEncoding* const encoding, const int32_t defaultWidth) {
  if (workspace_.widths == nullptr || workspace_.widthCapacity == 0 || defaultWidth < 0) {
    return PdfStatus::failure(PdfError::InvalidArgument);
  }
  if (workspace_.spill.valid() && workspace_.spill.recordSize != sizeof(PdfFontWidthRecord)) {
    return PdfStatus::failure(PdfError::InvalidArgument, workspace_.spill.recordSize);
  }
  fontId_ = fontId;
  cid_ = cid;
  toUnicode_ = toUnicode;
  encoding_ = encoding;
  defaultWidth_ = defaultWidth;
  widthCount_ = 0;
  spillCount_ = 0;
  previousWidthLast_ = 0;
  widthsSorted_ = true;
  hasPreviousWidth_ = false;
  hasCachedWidth_ = false;
  return setSourceAccess(true);
}

PdfStatus PdfFontMap::beginMaterialized(const uint16_t fontId, const bool cid) {
  if (workspace_.materializedGlyphs == nullptr || workspace_.materializedGlyphCapacity == 0) {
    return PdfStatus::failure(PdfError::InvalidArgument);
  }
  if (workspace_.materializedGlyphCapacity > PdfLimits::MaxPageUniqueGlyphs) {
    return PdfStatus::failure(PdfError::LimitExceeded, workspace_.materializedGlyphCapacity);
  }
  const PdfStatus accessStatus = setSourceAccess(false);
  if (!accessStatus.ok()) {
    return accessStatus;
  }
  fontId_ = fontId;
  cid_ = cid;
  toUnicode_ = nullptr;
  encoding_ = nullptr;
  defaultWidth_ = -1;
  widthCount_ = 0;
  spillCount_ = 0;
  previousWidthLast_ = 0;
  widthsSorted_ = true;
  hasPreviousWidth_ = false;
  hasCachedWidth_ = false;
  return PdfStatus::success();
}

PdfStatus PdfFontMap::addMaterializedGlyph(const PdfDecodedGlyph& glyph) {
  if (!materialized() || !sourceCodeFitsLength(glyph.sourceCode, glyph.sourceLength) || glyph.unicode.length == 0 ||
      glyph.unicode.length > sizeof(glyph.unicode.bytes) || glyph.width < 0) {
    return PdfStatus::failure(PdfError::Malformed, glyph.sourceCode);
  }

  const uint64_t key = glyphKey(glyph.sourceCode, glyph.sourceLength);
  uint16_t first = 0;
  uint16_t last = widthCount_;
  while (first < last) {
    const uint16_t middle = static_cast<uint16_t>(first + (last - first) / 2U);
    const PdfDecodedGlyph& candidate = workspace_.materializedGlyphs[middle];
    if (glyphKey(candidate.sourceCode, candidate.sourceLength) < key) {
      first = static_cast<uint16_t>(middle + 1U);
    } else {
      last = middle;
    }
  }
  if (first < widthCount_) {
    const PdfDecodedGlyph& existing = workspace_.materializedGlyphs[first];
    if (glyphKey(existing.sourceCode, existing.sourceLength) == key) {
      const bool identical = existing.width == glyph.width && existing.unicode.length == glyph.unicode.length &&
                             std::memcmp(existing.unicode.bytes, glyph.unicode.bytes, glyph.unicode.length) == 0;
      return identical ? PdfStatus::success() : PdfStatus::failure(PdfError::Malformed, glyph.sourceCode);
    }
  }

  for (uint16_t index = 0; index < widthCount_; ++index) {
    const PdfDecodedGlyph& existing = workspace_.materializedGlyphs[index];
    if (sourceCodeIsPrefix(existing.sourceCode, existing.sourceLength, glyph.sourceCode, glyph.sourceLength) ||
        sourceCodeIsPrefix(glyph.sourceCode, glyph.sourceLength, existing.sourceCode, existing.sourceLength)) {
      return PdfStatus::failure(PdfError::UnsupportedEncoding, glyph.sourceCode);
    }
  }
  if (widthCount_ >= workspace_.materializedGlyphCapacity) {
    return PdfStatus::failure(PdfError::LimitExceeded, widthCount_);
  }

  if (first < widthCount_) {
    std::memmove(workspace_.materializedGlyphs + first + 1U, workspace_.materializedGlyphs + first,
                 static_cast<size_t>(widthCount_ - first) * sizeof(PdfDecodedGlyph));
  }
  workspace_.materializedGlyphs[first] = glyph;
  ++widthCount_;
  return PdfStatus::success();
}

PdfStatus PdfFontMap::materializeString(PdfFontMap& sourceFont, const uint8_t* const source,
                                        const size_t sourceLength) {
  if (!materialized() || &sourceFont == this || sourceFont.fontId() != fontId_ || sourceFont.cid() != cid_ ||
      (source == nullptr && sourceLength != 0)) {
    return PdfStatus::failure(PdfError::InvalidArgument);
  }
  size_t offset = 0;
  while (offset < sourceLength) {
    PdfDecodedGlyph glyph;
    const PdfStatus decodeStatus = sourceFont.decodeNext(source + offset, sourceLength - offset, &glyph);
    if (!decodeStatus.ok()) {
      return decodeStatus;
    }
    if (glyph.sourceLength == 0 || glyph.sourceLength > sourceLength - offset) {
      return PdfStatus::failure(PdfError::Malformed, offset);
    }
    const PdfStatus addStatus = addMaterializedGlyph(glyph);
    if (!addStatus.ok()) {
      return addStatus;
    }
    offset += glyph.sourceLength;
  }
  return PdfStatus::success();
}

PdfStatus PdfFontMap::addWidth(const uint32_t firstCode, const uint32_t lastCode, const int32_t width) {
  if (materialized() || lastCode < firstCode || width < 0) {
    return PdfStatus::failure(PdfError::Malformed, firstCode);
  }
  if (widthCount_ >= PdfLimits::MaxCMapRanges) {
    return PdfStatus::failure(PdfError::LimitExceeded, widthCount_);
  }
  const bool ordered = !hasPreviousWidth_ || firstCode > previousWidthLast_;
  if ((!ordered || !widthsSorted_) && widthCount_ >= workspace_.widthCapacity) {
    // Keep an unsorted map in bounded RAM; never turn it into a per-glyph linear SD scan.
    return PdfStatus::failure(PdfError::LimitExceeded, widthCount_);
  }
  if (!ordered) {
    widthsSorted_ = false;
  }
  PdfFontWidthRecord record{firstCode, lastCode, width};
  if (widthCount_ < workspace_.widthCapacity) {
    workspace_.widths[widthCount_] = record;
  } else {
    if (!workspace_.spill.valid() || spillCount_ >= workspace_.spill.capacity) {
      return PdfStatus::failure(PdfError::LimitExceeded, widthCount_);
    }
    const PdfStatus status = pdfWriteRecord(workspace_.spill, spillCount_, &record);
    if (!status.ok()) {
      return status;
    }
    ++spillCount_;
  }
  previousWidthLast_ = lastCode;
  hasPreviousWidth_ = true;
  ++widthCount_;
  return PdfStatus::success();
}

PdfStatus PdfFontMap::loadSimpleWidths(const PdfObjectArena& arena, const uint32_t firstChar,
                                       const uint16_t widthsArrayIndex) {
  if (widthsArrayIndex >= arena.valueCount || arena.values[widthsArrayIndex].kind != PdfValueKind::Array) {
    return PdfStatus::failure(PdfError::Malformed);
  }
  const PdfValue& widths = arena.values[widthsArrayIndex];
  if (firstChar > 255 || widths.count > 256U - firstChar) {
    return PdfStatus::failure(PdfError::Malformed, firstChar);
  }
  for (uint16_t ordinal = 0; ordinal < widths.count; ++ordinal) {
    uint16_t valueIndex = PDF_INVALID_INDEX;
    int32_t width = 0;
    if (!pdfArrayAt(arena, widthsArrayIndex, ordinal, &valueIndex) || !valueAsWidth(arena.values[valueIndex], &width)) {
      return PdfStatus::failure(PdfError::Malformed, ordinal);
    }
    const PdfStatus status = addWidth(firstChar + ordinal, firstChar + ordinal, width);
    if (!status.ok()) {
      return status;
    }
  }
  return PdfStatus::success();
}

PdfStatus PdfFontMap::loadCidWidths(const PdfObjectArena& arena, const uint16_t widthsArrayIndex) {
  if (widthsArrayIndex >= arena.valueCount || arena.values[widthsArrayIndex].kind != PdfValueKind::Array) {
    return PdfStatus::failure(PdfError::Malformed);
  }
  const PdfValue& widths = arena.values[widthsArrayIndex];
  uint16_t ordinal = 0;
  while (ordinal < widths.count) {
    uint16_t firstIndex = PDF_INVALID_INDEX;
    uint32_t firstCode = 0;
    if (!pdfArrayAt(arena, widthsArrayIndex, ordinal++, &firstIndex) ||
        !valueAsCode(arena.values[firstIndex], &firstCode) || ordinal >= widths.count) {
      return PdfStatus::failure(PdfError::Malformed, ordinal);
    }
    uint16_t secondIndex = PDF_INVALID_INDEX;
    if (!pdfArrayAt(arena, widthsArrayIndex, ordinal++, &secondIndex)) {
      return PdfStatus::failure(PdfError::Malformed, ordinal);
    }
    const PdfValue& second = arena.values[secondIndex];
    if (second.kind == PdfValueKind::Array) {
      for (uint16_t widthOrdinal = 0; widthOrdinal < second.count; ++widthOrdinal) {
        uint16_t widthIndex = PDF_INVALID_INDEX;
        int32_t width = 0;
        if (!pdfArrayAt(arena, secondIndex, widthOrdinal, &widthIndex) ||
            !valueAsWidth(arena.values[widthIndex], &width)) {
          return PdfStatus::failure(PdfError::Malformed, widthOrdinal);
        }
        if (firstCode > std::numeric_limits<uint32_t>::max() - widthOrdinal) {
          return PdfStatus::failure(PdfError::LimitExceeded, firstCode);
        }
        const PdfStatus status = addWidth(firstCode + widthOrdinal, firstCode + widthOrdinal, width);
        if (!status.ok()) {
          return status;
        }
      }
      continue;
    }
    uint32_t lastCode = 0;
    if (!valueAsCode(second, &lastCode) || ordinal >= widths.count) {
      return PdfStatus::failure(PdfError::Malformed, ordinal);
    }
    uint16_t widthIndex = PDF_INVALID_INDEX;
    int32_t width = 0;
    if (!pdfArrayAt(arena, widthsArrayIndex, ordinal++, &widthIndex) ||
        !valueAsWidth(arena.values[widthIndex], &width)) {
      return PdfStatus::failure(PdfError::Malformed, ordinal);
    }
    const PdfStatus status = addWidth(firstCode, lastCode, width);
    if (!status.ok()) {
      return status;
    }
  }
  return PdfStatus::success();
}

PdfStatus PdfFontMap::readWidth(const uint16_t ordinal, PdfFontWidthRecord* const width) {
  if (width == nullptr || ordinal >= widthCount_) {
    return PdfStatus::failure(PdfError::InvalidOffset, ordinal);
  }
  if (ordinal < workspace_.widthCapacity) {
    *width = workspace_.widths[ordinal];
    return PdfStatus::success();
  }
  const PdfStatus accessStatus = setSourceAccess(false);
  if (!accessStatus.ok()) {
    return accessStatus;
  }
  return pdfReadRecord(workspace_.spill, ordinal - workspace_.widthCapacity, width);
}

PdfStatus PdfFontMap::widthFor(const uint32_t sourceCode, int32_t* const width) {
  if (width == nullptr || materialized()) {
    return PdfStatus::failure(PdfError::InvalidArgument);
  }
  if (hasCachedWidth_ && sourceCode >= cachedWidth_.firstCode && sourceCode <= cachedWidth_.lastCode) {
    *width = cachedWidth_.width;
    return PdfStatus::success();
  }
  if (widthsSorted_ && widthCount_ != 0) {
    uint16_t first = 0;
    uint16_t last = widthCount_;
    while (first < last) {
      const uint16_t middle = static_cast<uint16_t>(first + (last - first) / 2);
      PdfFontWidthRecord record;
      const PdfStatus status = readWidth(middle, &record);
      if (!status.ok()) {
        return status;
      }
      if (record.firstCode <= sourceCode) {
        first = static_cast<uint16_t>(middle + 1);
      } else {
        last = middle;
      }
    }
    if (first != 0) {
      PdfFontWidthRecord record;
      const PdfStatus status = readWidth(static_cast<uint16_t>(first - 1), &record);
      if (!status.ok()) {
        return status;
      }
      if (sourceCode <= record.lastCode) {
        cachedWidth_ = record;
        hasCachedWidth_ = true;
        *width = record.width;
        return PdfStatus::success();
      }
    }
    *width = defaultWidth_;
    return PdfStatus::success();
  }
  for (uint16_t ordinal = widthCount_; ordinal-- > 0;) {
    PdfFontWidthRecord record;
    const PdfStatus status = readWidth(ordinal, &record);
    if (!status.ok()) {
      return status;
    }
    if (sourceCode >= record.firstCode && sourceCode <= record.lastCode) {
      cachedWidth_ = record;
      hasCachedWidth_ = true;
      *width = record.width;
      return PdfStatus::success();
    }
  }
  *width = defaultWidth_;
  return PdfStatus::success();
}

PdfStatus PdfFontMap::findMaterializedGlyph(const uint8_t* const source, const size_t sourceLength,
                                            PdfDecodedGlyph* const glyph) const {
  if (!materialized() || source == nullptr || sourceLength == 0 || glyph == nullptr) {
    return PdfStatus::failure(PdfError::InvalidArgument);
  }
  uint32_t sourceCode = 0;
  const uint8_t maximumLength = static_cast<uint8_t>(sourceLength < 4U ? sourceLength : 4U);
  for (uint8_t sourceCodeLength = 1; sourceCodeLength <= maximumLength; ++sourceCodeLength) {
    sourceCode = sourceCode << 8U | source[sourceCodeLength - 1U];
    const uint64_t key = glyphKey(sourceCode, sourceCodeLength);
    uint16_t first = 0;
    uint16_t last = widthCount_;
    while (first < last) {
      const uint16_t middle = static_cast<uint16_t>(first + (last - first) / 2U);
      const PdfDecodedGlyph& candidate = workspace_.materializedGlyphs[middle];
      const uint64_t candidateKey = glyphKey(candidate.sourceCode, candidate.sourceLength);
      if (candidateKey < key) {
        first = static_cast<uint16_t>(middle + 1U);
      } else {
        last = middle;
      }
    }
    if (first < widthCount_) {
      const PdfDecodedGlyph& candidate = workspace_.materializedGlyphs[first];
      if (glyphKey(candidate.sourceCode, candidate.sourceLength) == key) {
        *glyph = candidate;
        return PdfStatus::success();
      }
    }
  }
  return PdfStatus::failure(PdfError::UnsupportedEncoding, sourceCode);
}

PdfStatus PdfFontMap::decodeNext(const uint8_t* const source, const size_t sourceLength, PdfDecodedGlyph* const glyph) {
  if (source == nullptr || sourceLength == 0 || glyph == nullptr) {
    return PdfStatus::failure(PdfError::InvalidArgument);
  }
  if (materialized()) {
    const PdfStatus status = findMaterializedGlyph(source, sourceLength, glyph);
    if (status.ok() || cid_ || status.error != PdfError::UnsupportedEncoding) {
      return status;
    }
    uint32_t scalar = 0;
    if (!pdfConservativeLatinFallback(source[0], &scalar) && !pdfWinAnsiFallback(source[0], &scalar)) {
      return status;
    }
    *glyph = {};
    glyph->sourceCode = source[0];
    glyph->sourceLength = 1;
    size_t encodedLength = 0;
    const PdfStatus encodeStatus =
        pdfAppendUtf8Scalar(scalar, glyph->unicode.bytes, sizeof(glyph->unicode.bytes), &encodedLength);
    if (!encodeStatus) {
      return encodeStatus;
    }
    glyph->unicode.length = static_cast<uint8_t>(encodedLength);
    glyph->width = 500;
    return PdfStatus::success();
  }
  *glyph = {};
  if (toUnicode_ != nullptr) {
    PdfCMapLookup lookup;
    const PdfStatus status = toUnicode_->lookup(source, sourceLength, &lookup);
    if (status.ok()) {
      glyph->sourceCode = lookup.sourceCode;
      glyph->sourceLength = lookup.sourceLength;
      glyph->unicode = lookup.unicode;
      return widthFor(glyph->sourceCode, &glyph->width);
    }
    if (cid_ || status.error != PdfError::UnsupportedEncoding) {
      return status;
    }
  }
  if (cid_) {
    return PdfStatus::failure(PdfError::UnsupportedEncoding);
  }
  glyph->sourceCode = source[0];
  glyph->sourceLength = 1;
  PdfStatus status = encoding_ != nullptr ? encoding_->decode(source[0], &glyph->unicode)
                                          : PdfStatus::failure(PdfError::UnsupportedEncoding, source[0]);
  if (!status.ok()) {
    if (status.error != PdfError::UnsupportedEncoding) {
      return status;
    }
    uint32_t scalar = 0;
    if (!pdfConservativeLatinFallback(source[0], &scalar) && !pdfWinAnsiFallback(source[0], &scalar)) {
      return status;
    }
    size_t length = 0;
    status = pdfAppendUtf8Scalar(scalar, glyph->unicode.bytes, sizeof(glyph->unicode.bytes), &length);
    glyph->unicode.length = static_cast<uint8_t>(length);
  }
  if (!status.ok()) {
    return status;
  }
  return widthFor(glyph->sourceCode, &glyph->width);
}
