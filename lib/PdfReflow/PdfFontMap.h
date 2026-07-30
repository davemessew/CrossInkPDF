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

struct PdfFontMapWorkspace {
  using SourceAccessFn = PdfStatus (*)(void* context, bool sourceRequired);

  PdfFontWidthRecord* widths = nullptr;
  uint16_t widthCapacity = 0;
  PdfFixedRecordStore spill{};
  void* sourceAccessContext = nullptr;
  SourceAccessFn setSourceAccess = nullptr;
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
                  int32_t defaultWidth = 500);
  PdfStatus addWidth(uint32_t firstCode, uint32_t lastCode, int32_t width);
  PdfStatus loadSimpleWidths(const PdfObjectArena& arena, uint32_t firstChar, uint16_t widthsArrayIndex);
  PdfStatus loadCidWidths(const PdfObjectArena& arena, uint16_t widthsArrayIndex);
  PdfStatus decodeNext(const uint8_t* source, size_t sourceLength, PdfDecodedGlyph* glyph);
  PdfStatus widthFor(uint32_t sourceCode, int32_t* width);

  uint16_t fontId() const { return fontId_; }
  bool cid() const { return cid_; }
  uint16_t widthCount() const { return widthCount_; }

 private:
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
};
