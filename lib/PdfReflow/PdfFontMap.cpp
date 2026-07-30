#include "PdfFontMap.h"

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

}  // namespace

PdfStatus PdfFontMap::setSourceAccess(const bool required) {
  if (sourceAccessRequired_ == required) {
    return PdfStatus::success();
  }
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

PdfStatus PdfFontMap::addWidth(const uint32_t firstCode, const uint32_t lastCode, const int32_t width) {
  if (lastCode < firstCode || width < 0) {
    return PdfStatus::failure(PdfError::Malformed, firstCode);
  }
  if (widthCount_ >= PdfLimits::MaxCMapRanges) {
    return PdfStatus::failure(PdfError::LimitExceeded, widthCount_);
  }
  if (hasPreviousWidth_ && firstCode <= previousWidthLast_) {
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
  if (width == nullptr) {
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

PdfStatus PdfFontMap::decodeNext(const uint8_t* const source, const size_t sourceLength, PdfDecodedGlyph* const glyph) {
  if (source == nullptr || sourceLength == 0 || glyph == nullptr) {
    return PdfStatus::failure(PdfError::InvalidArgument);
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
    if (!pdfConservativeLatinFallback(source[0], &scalar)) {
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
