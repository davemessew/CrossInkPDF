#pragma once

#include <cstddef>
#include <cstdint>

#include "PdfCMap.h"
#include "PdfEncoding.h"

struct PdfFontWidthRecord {
  uint32_t firstCode = 0;
  uint32_t lastCode = 0;
  int32_t width = 0;
};

struct PdfDecodedGlyph;

struct PdfFontMapWorkspace {
  using SourceAccessFn = PdfStatus (*)(void* context, bool sourceRequired);

  PdfFontWidthRecord* widths = nullptr;
  uint16_t widthCapacity = 0;
  PdfFixedRecordStore spill{};
  void* sourceAccessContext = nullptr;
  SourceAccessFn setSourceAccess = nullptr;
  PdfDecodedGlyph* materializedGlyphs = nullptr;
  uint16_t materializedGlyphCapacity = 0;
};

struct PdfDecodedGlyph {
  uint32_t sourceCode = 0;
  uint8_t sourceLength = 0;
  PdfUtf8Value unicode{};
  int32_t width = 0;
};

class PdfFontMap {
 public:
  explicit PdfFontMap(PdfFontMapWorkspace workspace) : workspace_(workspace) {}

  PdfStatus begin(uint16_t fontId, bool cid, PdfCMap* toUnicode, PdfSimpleEncoding* encoding,
                  int32_t defaultWidth = 500, bool bold = false);
  PdfStatus beginMaterialized(uint16_t fontId, bool cid, bool bold = false);
  PdfStatus materializeString(PdfFontMap& sourceFont, const uint8_t* source, size_t sourceLength);
  PdfStatus addMaterializedGlyph(const PdfDecodedGlyph& glyph);
  PdfStatus addWidth(uint32_t firstCode, uint32_t lastCode, int32_t width);
  PdfStatus loadSimpleWidths(const PdfObjectArena& arena, uint32_t firstChar, uint16_t widthsArrayIndex);
  PdfStatus loadCidWidths(const PdfObjectArena& arena, uint16_t widthsArrayIndex);
  PdfStatus decodeNext(const uint8_t* source, size_t sourceLength, PdfDecodedGlyph* glyph);
  PdfStatus widthFor(uint32_t sourceCode, int32_t* width);

  uint16_t fontId() const { return fontId_; }
  bool cid() const { return cid_; }
  uint16_t widthCount() const { return widthCount_; }
  uint16_t materializedGlyphCount() const { return materialized() ? widthCount_ : 0; }
  bool materialized() const { return defaultWidth_ < 0; }
  bool hasExplicitWhitespace() const { return hasExplicitWhitespace_; }
  bool bold() const { return bold_; }
  bool fullyResident() const {
    return spillCount_ == 0 && (toUnicode_ == nullptr || toUnicode_->fullyResident()) &&
           (encoding_ == nullptr || encoding_->fullyResident());
  }

 private:
  PdfStatus findMaterializedGlyph(const uint8_t* source, size_t sourceLength, PdfDecodedGlyph* glyph) const;
  PdfStatus readWidth(uint16_t ordinal, PdfFontWidthRecord* width);
  PdfStatus setSourceAccess(bool required);

  PdfFontMapWorkspace workspace_{};
  PdfFontWidthRecord cachedWidth_{};
  PdfCMap* toUnicode_ = nullptr;
  PdfSimpleEncoding* encoding_ = nullptr;
  int32_t defaultWidth_ = 500;
  uint16_t fontId_ = 0;
  uint16_t widthCount_ = 0;
  uint16_t spillCount_ = 0;
  uint32_t previousWidthLast_ = 0;
  bool cid_ = false;
  bool sourceAccessRequired_ = false;
  bool widthsSorted_ = true;
  bool hasPreviousWidth_ = false;
  bool hasCachedWidth_ = false;
  bool hasExplicitWhitespace_ = false;
  bool bold_ = false;
};
