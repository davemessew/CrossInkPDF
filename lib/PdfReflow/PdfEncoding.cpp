#include "PdfEncoding.h"

#include <cstring>

namespace {

struct EncodingPair {
  uint8_t code;
  uint32_t scalar;
};

struct GlyphNamePair {
  const char* name;
  uint32_t scalar;
};

static constexpr uint32_t WIN_ANSI_80_TO_9F[] = {
    // Several long-lived PDF producers place the embedded font's bullet at
    // 0x81 while declaring WinAnsi. Readers commonly preserve that glyph even
    // though the original Windows table left the slot undefined.
    0x20AC, 0x2022, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160,
    0x2039, 0x0152, 0,      0x017D, 0,      0,      0x2018, 0x2019, 0x201C, 0x201D, 0x2022,
    0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0,      0x017E, 0x0178,
};

static constexpr uint32_t MAC_ROMAN_80_TO_FF[] = {
    0x00C4, 0x00C5, 0x00C7, 0x00C9, 0x00D1, 0x00D6, 0x00DC, 0x00E1, 0x00E0, 0x00E2, 0x00E4, 0x00E3, 0x00E5,
    0x00E7, 0x00E9, 0x00E8, 0x00EA, 0x00EB, 0x00ED, 0x00EC, 0x00EE, 0x00EF, 0x00F1, 0x00F3, 0x00F2, 0x00F4,
    0x00F6, 0x00F5, 0x00FA, 0x00F9, 0x00FB, 0x00FC, 0x2020, 0x00B0, 0x00A2, 0x00A3, 0x00A7, 0x2022, 0x00B6,
    0x00DF, 0x00AE, 0x00A9, 0x2122, 0x00B4, 0x00A8, 0x2260, 0x00C6, 0x00D8, 0x221E, 0x00B1, 0x2264, 0x2265,
    0x00A5, 0x00B5, 0x2202, 0x2211, 0x220F, 0x03C0, 0x222B, 0x00AA, 0x00BA, 0x03A9, 0x00E6, 0x00F8, 0x00BF,
    0x00A1, 0x00AC, 0x221A, 0x0192, 0x2248, 0x2206, 0x00AB, 0x00BB, 0x2026, 0x00A0, 0x00C0, 0x00C3, 0x00D5,
    0x0152, 0x0153, 0x2013, 0x2014, 0x201C, 0x201D, 0x2018, 0x2019, 0x00F7, 0x25CA, 0x00FF, 0x0178, 0x2044,
    0x20AC, 0x2039, 0x203A, 0xFB01, 0xFB02, 0x2021, 0x00B7, 0x201A, 0x201E, 0x2030, 0x00C2, 0x00CA, 0x00C1,
    0x00CB, 0x00C8, 0x00CD, 0x00CE, 0x00CF, 0x00CC, 0x00D3, 0x00D4, 0xF8FF, 0x00D2, 0x00DA, 0x00DB, 0x00D9,
    0x0131, 0x02C6, 0x02DC, 0x00AF, 0x02D8, 0x02D9, 0x02DA, 0x00B8, 0x02DD, 0x02DB, 0x02C7,
};

static constexpr EncodingPair PDF_DOC_SPECIAL[] = {
    {0x18, 0x02D8}, {0x19, 0x02C7}, {0x1A, 0x02C6}, {0x1B, 0x02D9}, {0x1C, 0x02DD}, {0x1D, 0x02DB}, {0x1E, 0x02DA},
    {0x1F, 0x02DC}, {0x80, 0x2022}, {0x81, 0x2020}, {0x82, 0x2021}, {0x83, 0x2026}, {0x84, 0x2014}, {0x85, 0x2013},
    {0x86, 0x0192}, {0x87, 0x2044}, {0x88, 0x2039}, {0x89, 0x203A}, {0x8A, 0x2212}, {0x8B, 0x2030}, {0x8C, 0x201E},
    {0x8D, 0x201C}, {0x8E, 0x201D}, {0x8F, 0x2018}, {0x90, 0x2019}, {0x91, 0x201A}, {0x92, 0x2122}, {0x93, 0xFB01},
    {0x94, 0xFB02}, {0x95, 0x0141}, {0x96, 0x0152}, {0x97, 0x0160}, {0x98, 0x0178}, {0x99, 0x017D}, {0x9A, 0x0131},
    {0x9B, 0x0142}, {0x9C, 0x0153}, {0x9D, 0x0161}, {0x9E, 0x017E}, {0xA0, 0x20AC},
};

static constexpr EncodingPair STANDARD_SPECIAL[] = {
    {0x27, 0x2019}, {0x60, 0x2018}, {0xA1, 0x00A1}, {0xA2, 0x00A2}, {0xA3, 0x00A3}, {0xA4, 0x2044}, {0xA5, 0x00A5},
    {0xA6, 0x0192}, {0xA7, 0x00A7}, {0xA8, 0x00A4}, {0xA9, 0x0027}, {0xAA, 0x201C}, {0xAB, 0x00AB}, {0xAC, 0x2039},
    {0xAD, 0x203A}, {0xAE, 0xFB01}, {0xAF, 0xFB02}, {0xB1, 0x2013}, {0xB2, 0x2020}, {0xB3, 0x2021}, {0xB4, 0x00B7},
    {0xB6, 0x00B6}, {0xB7, 0x2022}, {0xB8, 0x201A}, {0xB9, 0x201E}, {0xBA, 0x201D}, {0xBB, 0x00BB}, {0xBC, 0x2026},
    {0xBD, 0x2030}, {0xBF, 0x00BF}, {0xC1, 0x0060}, {0xC2, 0x00B4}, {0xC3, 0x02C6}, {0xC4, 0x02DC}, {0xC5, 0x00AF},
    {0xC6, 0x02D8}, {0xC7, 0x02D9}, {0xC8, 0x00A8}, {0xCA, 0x02DA}, {0xCB, 0x00B8}, {0xCD, 0x02DD}, {0xCE, 0x02DB},
    {0xCF, 0x02C7}, {0xD0, 0x2014}, {0xE1, 0x00C6}, {0xE3, 0x00AA}, {0xE8, 0x0141}, {0xE9, 0x00D8}, {0xEA, 0x0152},
    {0xEB, 0x00BA}, {0xF1, 0x00E6}, {0xF5, 0x0131}, {0xF8, 0x0142}, {0xF9, 0x00F8}, {0xFA, 0x0153}, {0xFB, 0x00DF},
};

static constexpr GlyphNamePair GLYPH_NAMES[] = {
    {"space", 0x0020},
    {"nonbreakingspace", 0x00A0},
    {"sfthyphen", 0x00AD},
    {"exclam", 0x0021},
    {"quotedbl", 0x0022},
    {"numbersign", 0x0023},
    {"dollar", 0x0024},
    {"percent", 0x0025},
    {"ampersand", 0x0026},
    {"quotesingle", 0x0027},
    {"parenleft", 0x0028},
    {"parenright", 0x0029},
    {"asterisk", 0x002A},
    {"plus", 0x002B},
    {"comma", 0x002C},
    {"hyphen", 0x002D},
    {"period", 0x002E},
    {"slash", 0x002F},
    {"colon", 0x003A},
    {"semicolon", 0x003B},
    {"less", 0x003C},
    {"equal", 0x003D},
    {"greater", 0x003E},
    {"question", 0x003F},
    {"at", 0x0040},
    {"bracketleft", 0x005B},
    {"backslash", 0x005C},
    {"bracketright", 0x005D},
    {"asciicircum", 0x005E},
    {"underscore", 0x005F},
    {"grave", 0x0060},
    {"braceleft", 0x007B},
    {"bar", 0x007C},
    {"braceright", 0x007D},
    {"asciitilde", 0x007E},
    {"zero", 0x0030},
    {"one", 0x0031},
    {"two", 0x0032},
    {"three", 0x0033},
    {"four", 0x0034},
    {"five", 0x0035},
    {"six", 0x0036},
    {"seven", 0x0037},
    {"eight", 0x0038},
    {"nine", 0x0039},
    {"exclamdown", 0x00A1},
    {"cent", 0x00A2},
    {"sterling", 0x00A3},
    {"currency", 0x00A4},
    {"yen", 0x00A5},
    {"brokenbar", 0x00A6},
    {"section", 0x00A7},
    {"dieresis", 0x00A8},
    {"copyright", 0x00A9},
    {"ordfeminine", 0x00AA},
    {"guillemotleft", 0x00AB},
    {"logicalnot", 0x00AC},
    {"registered", 0x00AE},
    {"macron", 0x00AF},
    {"degree", 0x00B0},
    {"plusminus", 0x00B1},
    {"twosuperior", 0x00B2},
    {"threesuperior", 0x00B3},
    {"acute", 0x00B4},
    {"mu", 0x00B5},
    {"paragraph", 0x00B6},
    {"periodcentered", 0x00B7},
    {"cedilla", 0x00B8},
    {"onesuperior", 0x00B9},
    {"ordmasculine", 0x00BA},
    {"guillemotright", 0x00BB},
    {"onequarter", 0x00BC},
    {"onehalf", 0x00BD},
    {"threequarters", 0x00BE},
    {"questiondown", 0x00BF},
    {"Agrave", 0x00C0},
    {"Aacute", 0x00C1},
    {"Acircumflex", 0x00C2},
    {"Atilde", 0x00C3},
    {"Aring", 0x00C5},
    {"Ccedilla", 0x00C7},
    {"Egrave", 0x00C8},
    {"Eacute", 0x00C9},
    {"Ecircumflex", 0x00CA},
    {"Edieresis", 0x00CB},
    {"Igrave", 0x00CC},
    {"Iacute", 0x00CD},
    {"Icircumflex", 0x00CE},
    {"Idieresis", 0x00CF},
    {"Eth", 0x00D0},
    {"Ntilde", 0x00D1},
    {"Ograve", 0x00D2},
    {"Oacute", 0x00D3},
    {"Ocircumflex", 0x00D4},
    {"Otilde", 0x00D5},
    {"multiply", 0x00D7},
    {"Ugrave", 0x00D9},
    {"Uacute", 0x00DA},
    {"Ucircumflex", 0x00DB},
    {"Yacute", 0x00DD},
    {"Thorn", 0x00DE},
    {"agrave", 0x00E0},
    {"aacute", 0x00E1},
    {"acircumflex", 0x00E2},
    {"atilde", 0x00E3},
    {"aring", 0x00E5},
    {"ccedilla", 0x00E7},
    {"egrave", 0x00E8},
    {"eacute", 0x00E9},
    {"ecircumflex", 0x00EA},
    {"edieresis", 0x00EB},
    {"igrave", 0x00EC},
    {"iacute", 0x00ED},
    {"icircumflex", 0x00EE},
    {"idieresis", 0x00EF},
    {"eth", 0x00F0},
    {"ntilde", 0x00F1},
    {"ograve", 0x00F2},
    {"oacute", 0x00F3},
    {"ocircumflex", 0x00F4},
    {"otilde", 0x00F5},
    {"divide", 0x00F7},
    {"ugrave", 0x00F9},
    {"uacute", 0x00FA},
    {"ucircumflex", 0x00FB},
    {"yacute", 0x00FD},
    {"thorn", 0x00FE},
    {"ydieresis", 0x00FF},
    {"Euro", 0x20AC},
    {"quotesinglbase", 0x201A},
    {"florin", 0x0192},
    {"quotedblbase", 0x201E},
    {"bullet", 0x2022},
    {"dagger", 0x2020},
    {"daggerdbl", 0x2021},
    {"circumflex", 0x02C6},
    {"perthousand", 0x2030},
    {"Scaron", 0x0160},
    {"guilsinglleft", 0x2039},
    {"Zcaron", 0x017D},
    {"emdash", 0x2014},
    {"endash", 0x2013},
    {"quoteleft", 0x2018},
    {"quoteright", 0x2019},
    {"quotedblleft", 0x201C},
    {"quotedblright", 0x201D},
    {"tilde", 0x02DC},
    {"trademark", 0x2122},
    {"scaron", 0x0161},
    {"guilsinglright", 0x203A},
    {"zcaron", 0x017E},
    {"Ydieresis", 0x0178},
    {"ellipsis", 0x2026},
    {"fi", 0xFB01},
    {"fl", 0xFB02},
    {"Omega", 0x03A9},
    {"pi", 0x03C0},
    {"fraction", 0x2044},
    {"Lslash", 0x0141},
    {"lslash", 0x0142},
    {"breve", 0x02D8},
    {"dotaccent", 0x02D9},
    {"ring", 0x02DA},
    {"hungarumlaut", 0x02DD},
    {"ogonek", 0x02DB},
    {"caron", 0x02C7},
    {"AE", 0x00C6},
    {"ae", 0x00E6},
    {"OE", 0x0152},
    {"oe", 0x0153},
    {"Oslash", 0x00D8},
    {"oslash", 0x00F8},
    {"germandbls", 0x00DF},
    {"dotlessi", 0x0131},
    {"Adieresis", 0x00C4},
    {"adieresis", 0x00E4},
    {"Odieresis", 0x00D6},
    {"odieresis", 0x00F6},
    {"Udieresis", 0x00DC},
    {"udieresis", 0x00FC},
};

bool parseHexDigit(const uint8_t byte, uint8_t* value) {
  if (byte >= '0' && byte <= '9') {
    *value = static_cast<uint8_t>(byte - '0');
    return true;
  }
  if (byte >= 'A' && byte <= 'F') {
    *value = static_cast<uint8_t>(byte - 'A' + 10);
    return true;
  }
  if (byte >= 'a' && byte <= 'f') {
    *value = static_cast<uint8_t>(byte - 'a' + 10);
    return true;
  }
  return false;
}

bool lookupPdfDocSpecial(const uint8_t code, uint32_t* scalar) {
  for (const EncodingPair& pair : PDF_DOC_SPECIAL) {
    if (pair.code == code) {
      *scalar = pair.scalar;
      return true;
    }
  }
  return false;
}

template <size_t PairCount>
bool lookupEncodingPair(const EncodingPair (&pairs)[PairCount], const uint8_t code, uint32_t* scalar) {
  for (const EncodingPair& pair : pairs) {
    if (pair.code == code) {
      *scalar = pair.scalar;
      return true;
    }
  }
  return false;
}

bool baseEncodingScalar(const PdfBaseEncoding base, const uint8_t code, uint32_t* scalar) {
  if (scalar == nullptr) {
    return false;
  }
  if (base == PdfBaseEncoding::AdvPSMP10) {
    if (code == 'c') {
      *scalar = 0x03B3;
      return true;
    }
    if (code == 'd') {
      *scalar = 0x03B4;
      return true;
    }
    if (code == 'l') {
      *scalar = 0x03BB;
      return true;
    }
  }
  if (base == PdfBaseEncoding::Standard && lookupEncodingPair(STANDARD_SPECIAL, code, scalar)) {
    return true;
  }
  if (code >= 0x20 && code <= 0x7E) {
    *scalar = code;
    return true;
  }
  switch (base) {
    case PdfBaseEncoding::WinAnsi:
      if (code >= 0xA0) {
        *scalar = code;
        return true;
      }
      if (code >= 0x80 && WIN_ANSI_80_TO_9F[code - 0x80] != 0) {
        *scalar = WIN_ANSI_80_TO_9F[code - 0x80];
        return true;
      }
      return false;
    case PdfBaseEncoding::MacRoman:
      if (code >= 0x80) {
        *scalar = MAC_ROMAN_80_TO_FF[code - 0x80];
        return true;
      }
      return false;
    case PdfBaseEncoding::PdfDoc:
      if (lookupPdfDocSpecial(code, scalar)) {
        return true;
      }
      if (code >= 0xA1) {
        *scalar = code;
        return true;
      }
      return false;
    case PdfBaseEncoding::Standard:
      return lookupEncodingPair(STANDARD_SPECIAL, code, scalar);
    case PdfBaseEncoding::TeXMathSymbols:
      if (code == 0) {
        *scalar = 0x2212;
        return true;
      }
      return lookupEncodingPair(STANDARD_SPECIAL, code, scalar);
    case PdfBaseEncoding::AdvP4C4E74:
      if (code == 2) {
        *scalar = 0x00B0;
        return true;
      }
      if (code == 3) {
        *scalar = 0x223C;
        return true;
      }
      if (code == 4) {
        *scalar = 0x2248;
        return true;
      }
      return lookupEncodingPair(STANDARD_SPECIAL, code, scalar);
    case PdfBaseEncoding::AdvP4C4E59:
      if (code == 2) {
        *scalar = 0x030C;
        return true;
      }
      if (code == 3) {
        *scalar = 0x0301;
        return true;
      }
      return lookupEncodingPair(STANDARD_SPECIAL, code, scalar);
    case PdfBaseEncoding::AdvPSMP10:
      return lookupEncodingPair(STANDARD_SPECIAL, code, scalar);
  }
  return false;
}

}  // namespace

bool pdfGlyphNameToUnicode(const uint8_t* const name, const size_t length, uint32_t* const scalar) {
  if (name == nullptr || scalar == nullptr || length == 0) {
    return false;
  }
  size_t glyphLength = 0;
  while (glyphLength < length && name[glyphLength] != '.') {
    ++glyphLength;
  }
  if (glyphLength == 0) {
    return false;
  }
  if (glyphLength == 1 && ((name[0] >= 'A' && name[0] <= 'Z') || (name[0] >= 'a' && name[0] <= 'z') ||
                           (name[0] >= '0' && name[0] <= '9'))) {
    *scalar = name[0];
    return true;
  }
  for (const GlyphNamePair& pair : GLYPH_NAMES) {
    const size_t pairLength = std::strlen(pair.name);
    if (pairLength == glyphLength && std::memcmp(name, pair.name, glyphLength) == 0) {
      *scalar = pair.scalar;
      return true;
    }
  }
  const size_t prefix = glyphLength == 7 && std::memcmp(name, "uni", 3) == 0 ? 3 : 0;
  if ((prefix == 3 && glyphLength == 7) ||
      ((name[0] == 'u' || name[0] == 'U') && glyphLength >= 5 && glyphLength <= 7)) {
    const size_t start = prefix == 3 ? 3 : 1;
    uint32_t parsed = 0;
    for (size_t index = start; index < glyphLength; ++index) {
      uint8_t nibble = 0;
      if (!parseHexDigit(name[index], &nibble)) {
        return false;
      }
      parsed = parsed << 4 | nibble;
    }
    if (pdfIsUnicodeScalar(parsed)) {
      *scalar = parsed;
      return true;
    }
  }
  return false;
}

bool pdfConservativeLatinFallback(const uint8_t code, uint32_t* const scalar) {
  if (scalar == nullptr || code < 0x20 || code > 0x7E) {
    return false;
  }
  *scalar = code;
  return true;
}

bool pdfWinAnsiFallback(const uint8_t code, uint32_t* const scalar) {
  return baseEncodingScalar(PdfBaseEncoding::WinAnsi, code, scalar);
}

PdfStatus pdfDecodePdfTextString(const uint8_t* const source, const size_t sourceLength, const PdfByteSink& sink) {
  if (source == nullptr || !sink.valid()) {
    return PdfStatus::failure(PdfError::InvalidArgument);
  }
  if (sourceLength >= 2 && source[0] == 0xFE && source[1] == 0xFF) {
    size_t offset = 2;
    while (offset < sourceLength) {
      if (offset + 1 >= sourceLength) {
        return PdfStatus::failure(PdfError::Malformed, offset);
      }
      size_t scalarBytes = 2;
      const uint32_t first = static_cast<uint32_t>(source[offset]) << 8 | source[offset + 1];
      if (first >= 0xD800 && first <= 0xDBFF) {
        scalarBytes = 4;
      }
      if (scalarBytes > sourceLength - offset) {
        return PdfStatus::failure(PdfError::Malformed, offset);
      }
      uint32_t scalar = 0;
      const PdfStatus decodeStatus = pdfDecodeSingleUtf16BeScalar(source + offset, scalarBytes, &scalar);
      if (!decodeStatus.ok()) {
        return decodeStatus;
      }
      PdfUtf8Value value;
      size_t length = 0;
      const PdfStatus encodeStatus = pdfAppendUtf8Scalar(scalar, value.bytes, sizeof(value.bytes), &length);
      if (!encodeStatus.ok()) {
        return encodeStatus;
      }
      const PdfStatus writeStatus = pdfWriteExact(sink, value.bytes, length);
      if (!writeStatus.ok()) {
        return writeStatus;
      }
      offset += scalarBytes;
    }
    return PdfStatus::success();
  }
  for (size_t offset = 0; offset < sourceLength; ++offset) {
    uint32_t scalar = 0;
    if (!baseEncodingScalar(PdfBaseEncoding::PdfDoc, source[offset], &scalar) &&
        !pdfConservativeLatinFallback(source[offset], &scalar)) {
      return PdfStatus::failure(PdfError::UnsupportedEncoding, offset);
    }
    PdfUtf8Value value;
    size_t length = 0;
    const PdfStatus encodeStatus = pdfAppendUtf8Scalar(scalar, value.bytes, sizeof(value.bytes), &length);
    if (!encodeStatus.ok()) {
      return encodeStatus;
    }
    const PdfStatus writeStatus = pdfWriteExact(sink, value.bytes, length);
    if (!writeStatus.ok()) {
      return writeStatus;
    }
  }
  return PdfStatus::success();
}

PdfStatus PdfSimpleEncoding::setSourceAccess(const bool required) {
  if (workspace_.setSourceAccess != nullptr) {
    const PdfStatus status = workspace_.setSourceAccess(workspace_.sourceAccessContext, required);
    if (!status.ok()) {
      return status;
    }
  }
  sourceAccessRequired_ = required;
  return PdfStatus::success();
}

PdfStatus PdfSimpleEncoding::begin(const PdfBaseEncoding base) {
  if (workspace_.differences == nullptr || workspace_.differenceCapacity == 0) {
    return PdfStatus::failure(PdfError::InvalidArgument);
  }
  if (workspace_.spill.valid() && workspace_.spill.recordSize != sizeof(PdfEncodingDifference)) {
    return PdfStatus::failure(PdfError::InvalidArgument, workspace_.spill.recordSize);
  }
  base_ = base;
  differenceCount_ = 0;
  spillCount_ = 0;
  return setSourceAccess(true);
}

PdfStatus PdfSimpleEncoding::addDifference(const uint8_t code, const uint32_t scalar) {
  PdfEncodingDifference difference{};
  difference.code = code;
  difference.scalar = scalar;
  if (differenceCount_ < workspace_.differenceCapacity) {
    workspace_.differences[differenceCount_] = difference;
  } else {
    if (!workspace_.spill.valid() || spillCount_ >= workspace_.spill.capacity) {
      return PdfStatus::failure(PdfError::LimitExceeded, differenceCount_);
    }
    const PdfStatus status = pdfWriteRecord(workspace_.spill, spillCount_, &difference);
    if (!status.ok()) {
      return status;
    }
    ++spillCount_;
  }
  ++differenceCount_;
  return PdfStatus::success();
}

PdfStatus PdfSimpleEncoding::applyDifferences(const PdfObjectArena& arena, const uint16_t differencesArrayIndex) {
  if (differencesArrayIndex >= arena.valueCount || arena.values[differencesArrayIndex].kind != PdfValueKind::Array) {
    return PdfStatus::failure(PdfError::Malformed);
  }
  uint16_t code = 0;
  bool hasCode = false;
  const PdfValue& array = arena.values[differencesArrayIndex];
  for (uint16_t ordinal = 0; ordinal < array.count; ++ordinal) {
    uint16_t valueIndex = PDF_INVALID_INDEX;
    if (!pdfArrayAt(arena, differencesArrayIndex, ordinal, &valueIndex)) {
      return PdfStatus::failure(PdfError::Malformed, ordinal);
    }
    const PdfValue& value = arena.values[valueIndex];
    if (value.kind == PdfValueKind::Integer) {
      if (value.integerValue < 0 || value.integerValue > 255) {
        return PdfStatus::failure(PdfError::Malformed, ordinal);
      }
      code = static_cast<uint16_t>(value.integerValue);
      hasCode = true;
      continue;
    }
    if (!hasCode || value.kind != PdfValueKind::Name ||
        static_cast<uint32_t>(value.textOffset) + value.textLength > arena.textLength) {
      return PdfStatus::failure(PdfError::Malformed, ordinal);
    }
    uint32_t scalar = 0;
    if (!pdfGlyphNameToUnicode(arena.text + value.textOffset, value.textLength, &scalar)) {
      return PdfStatus::failure(PdfError::UnsupportedEncoding, ordinal);
    }
    const PdfStatus addStatus = addDifference(static_cast<uint8_t>(code), scalar);
    if (!addStatus.ok()) {
      return addStatus;
    }
    if (code == 255 && ordinal + 1 < array.count) {
      return PdfStatus::failure(PdfError::Malformed, ordinal);
    }
    ++code;
  }
  return PdfStatus::success();
}

PdfStatus PdfSimpleEncoding::readDifference(const uint16_t ordinal, PdfEncodingDifference* const difference) {
  if (difference == nullptr || ordinal >= differenceCount_) {
    return PdfStatus::failure(PdfError::InvalidOffset, ordinal);
  }
  if (ordinal < workspace_.differenceCapacity) {
    *difference = workspace_.differences[ordinal];
    return PdfStatus::success();
  }
  const PdfStatus accessStatus = setSourceAccess(false);
  if (!accessStatus.ok()) {
    return accessStatus;
  }
  return pdfReadRecord(workspace_.spill, ordinal - workspace_.differenceCapacity, difference);
}

PdfStatus PdfSimpleEncoding::decode(const uint8_t code, PdfUtf8Value* const value) {
  if (value == nullptr) {
    return PdfStatus::failure(PdfError::InvalidArgument);
  }
  uint32_t scalar = 0;
  for (uint16_t ordinal = differenceCount_; ordinal-- > 0;) {
    PdfEncodingDifference difference;
    const PdfStatus status = readDifference(ordinal, &difference);
    if (!status.ok()) {
      return status;
    }
    if (difference.code == code) {
      scalar = difference.scalar;
      break;
    }
  }
  if (scalar == 0 && !baseEncodingScalar(base_, code, &scalar) && !pdfConservativeLatinFallback(code, &scalar)) {
    return PdfStatus::failure(PdfError::UnsupportedEncoding, code);
  }
  *value = {};
  size_t length = 0;
  const PdfStatus status = pdfAppendUtf8Scalar(scalar, value->bytes, sizeof(value->bytes), &length);
  if (status.ok()) {
    value->length = static_cast<uint8_t>(length);
  }
  return status;
}
