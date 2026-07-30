#include "PdfUnicode.h"

#include <limits>

namespace {

struct UnicodeRange {
  uint32_t first;
  uint32_t last;
};

static constexpr UnicodeRange LETTER_OR_DIGIT_RANGES[] = {
    {0x0030, 0x0039}, {0x0041, 0x005A}, {0x0061, 0x007A}, {0x00C0, 0x02AF}, {0x0370, 0x052F}, {0x0531, 0x0588},
    {0x0590, 0x08FF}, {0x0900, 0x1FFF}, {0x2C00, 0x2DFF}, {0xA640, 0xA69F}, {0xA720, 0xA7FF},
};

static constexpr UnicodeRange CJK_READING_RANGES[] = {
    {0x3040, 0x30FF}, {0x3400, 0x4DBF}, {0x4E00, 0x9FFF}, {0xAC00, 0xD7AF}, {0xF900, 0xFAFF}, {0x20000, 0x2FA1F},
};

template <size_t RangeCount>
bool inRanges(const uint32_t scalar, const UnicodeRange (&ranges)[RangeCount]) {
  for (const UnicodeRange& range : ranges) {
    if (scalar >= range.first && scalar <= range.last) {
      return true;
    }
  }
  return false;
}

}  // namespace

bool pdfIsUnicodeScalar(const uint32_t scalar) { return scalar <= 0x10FFFF && (scalar < 0xD800 || scalar > 0xDFFF); }

bool pdfIsUnicodeLetterOrDigit(const uint32_t scalar) {
  return inRanges(scalar, LETTER_OR_DIGIT_RANGES) || inRanges(scalar, CJK_READING_RANGES);
}

bool pdfIsCjkReadingUnit(const uint32_t scalar) { return inRanges(scalar, CJK_READING_RANGES); }

PdfStatus pdfDecodeUtf8Scalar(const uint8_t* const source, const size_t sourceLength, size_t* const offset,
                              uint32_t* const scalar) {
  if (source == nullptr || offset == nullptr || scalar == nullptr || *offset >= sourceLength) {
    return PdfStatus::failure(PdfError::InvalidArgument, offset == nullptr ? 0 : *offset);
  }
  const size_t start = *offset;
  const uint8_t first = source[start];
  size_t required = 1;
  uint32_t value = first;
  uint32_t minimum = 0;
  if ((first & 0x80U) == 0) {
    required = 1;
  } else if ((first & 0xE0U) == 0xC0U) {
    required = 2;
    value = first & 0x1FU;
    minimum = 0x80;
  } else if ((first & 0xF0U) == 0xE0U) {
    required = 3;
    value = first & 0x0FU;
    minimum = 0x800;
  } else if ((first & 0xF8U) == 0xF0U) {
    required = 4;
    value = first & 0x07U;
    minimum = 0x10000;
  } else {
    return PdfStatus::failure(PdfError::Malformed, start);
  }
  if (required > sourceLength - start) {
    return PdfStatus::failure(PdfError::Malformed, start);
  }
  for (size_t index = 1; index < required; ++index) {
    const uint8_t continuation = source[start + index];
    if ((continuation & 0xC0U) != 0x80U) {
      return PdfStatus::failure(PdfError::Malformed, start + index);
    }
    value = (value << 6U) | (continuation & 0x3FU);
  }
  if (value < minimum || !pdfIsUnicodeScalar(value)) {
    return PdfStatus::failure(PdfError::Malformed, start);
  }
  *offset = start + required;
  *scalar = value;
  return PdfStatus::success();
}

PdfStatus pdfAppendUtf8Scalar(const uint32_t scalar, uint8_t* const destination, const size_t capacity,
                              size_t* const length) {
  if (destination == nullptr || length == nullptr || *length > capacity || !pdfIsUnicodeScalar(scalar)) {
    return PdfStatus::failure(PdfError::Malformed, scalar);
  }
  size_t required = 1;
  if (scalar > 0x7F) {
    required = scalar <= 0x7FF ? 2 : (scalar <= 0xFFFF ? 3 : 4);
  }
  if (required > capacity - *length) {
    return PdfStatus::failure(PdfError::LimitExceeded, *length);
  }
  uint8_t* output = destination + *length;
  if (required == 1) {
    output[0] = static_cast<uint8_t>(scalar);
  } else if (required == 2) {
    output[0] = static_cast<uint8_t>(0xC0 | (scalar >> 6));
    output[1] = static_cast<uint8_t>(0x80 | (scalar & 0x3F));
  } else if (required == 3) {
    output[0] = static_cast<uint8_t>(0xE0 | (scalar >> 12));
    output[1] = static_cast<uint8_t>(0x80 | ((scalar >> 6) & 0x3F));
    output[2] = static_cast<uint8_t>(0x80 | (scalar & 0x3F));
  } else {
    output[0] = static_cast<uint8_t>(0xF0 | (scalar >> 18));
    output[1] = static_cast<uint8_t>(0x80 | ((scalar >> 12) & 0x3F));
    output[2] = static_cast<uint8_t>(0x80 | ((scalar >> 6) & 0x3F));
    output[3] = static_cast<uint8_t>(0x80 | (scalar & 0x3F));
  }
  *length += required;
  return PdfStatus::success();
}

PdfStatus pdfDecodeUtf16Be(const uint8_t* const source, const size_t sourceLength, PdfUtf8Value* const value) {
  if (source == nullptr || value == nullptr || sourceLength == 0 || (sourceLength & 1U) != 0) {
    return PdfStatus::failure(PdfError::Malformed, sourceLength);
  }
  value->length = 0;
  size_t sourceOffset = 0;
  size_t outputLength = 0;
  while (sourceOffset < sourceLength) {
    uint32_t scalar = static_cast<uint32_t>(source[sourceOffset]) << 8 | source[sourceOffset + 1];
    sourceOffset += 2;
    if (scalar >= 0xD800 && scalar <= 0xDBFF) {
      if (sourceOffset + 1 >= sourceLength) {
        return PdfStatus::failure(PdfError::Malformed, sourceOffset);
      }
      const uint32_t low = static_cast<uint32_t>(source[sourceOffset]) << 8 | source[sourceOffset + 1];
      sourceOffset += 2;
      if (low < 0xDC00 || low > 0xDFFF) {
        return PdfStatus::failure(PdfError::Malformed, sourceOffset - 2);
      }
      scalar = 0x10000 + ((scalar - 0xD800) << 10) + (low - 0xDC00);
    } else if (scalar >= 0xDC00 && scalar <= 0xDFFF) {
      return PdfStatus::failure(PdfError::Malformed, sourceOffset - 2);
    }
    const PdfStatus status = pdfAppendUtf8Scalar(scalar, value->bytes, sizeof(value->bytes), &outputLength);
    if (!status.ok()) {
      return status;
    }
  }
  value->length = static_cast<uint8_t>(outputLength);
  return PdfStatus::success();
}

PdfStatus pdfDecodeSingleUtf16BeScalar(const uint8_t* const source, const size_t sourceLength, uint32_t* const scalar) {
  if (source == nullptr || scalar == nullptr || (sourceLength != 2 && sourceLength != 4)) {
    return PdfStatus::failure(PdfError::Malformed, sourceLength);
  }
  const uint32_t first = static_cast<uint32_t>(source[0]) << 8 | source[1];
  if (sourceLength == 2) {
    if (!pdfIsUnicodeScalar(first)) {
      return PdfStatus::failure(PdfError::Malformed, first);
    }
    *scalar = first;
    return PdfStatus::success();
  }
  const uint32_t second = static_cast<uint32_t>(source[2]) << 8 | source[3];
  if (first < 0xD800 || first > 0xDBFF || second < 0xDC00 || second > 0xDFFF) {
    return PdfStatus::failure(PdfError::Malformed, first);
  }
  *scalar = 0x10000 + ((first - 0xD800) << 10) + (second - 0xDC00);
  return PdfStatus::success();
}
