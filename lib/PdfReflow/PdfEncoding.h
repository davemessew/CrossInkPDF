#pragma once

#include <cstddef>
#include <cstdint>

#include "PdfIo.h"
#include "PdfObjectParser.h"
#include "PdfUnicode.h"

enum class PdfBaseEncoding : uint8_t {
  Standard,
  WinAnsi,
  MacRoman,
  PdfDoc,
};

struct PdfEncodingDifference {
  uint32_t scalar = 0;
  uint8_t code = 0;
  uint8_t reserved[3]{};
};

struct PdfEncodingWorkspace {
  using SourceAccessFn = PdfStatus (*)(void* context, bool sourceRequired);

  PdfEncodingDifference* differences = nullptr;
  uint16_t differenceCapacity = 0;
  PdfFixedRecordStore spill{};
  void* sourceAccessContext = nullptr;
  SourceAccessFn setSourceAccess = nullptr;
};

class PdfSimpleEncoding {
 public:
  explicit PdfSimpleEncoding(PdfEncodingWorkspace workspace) : workspace_(workspace) {}

  PdfStatus begin(PdfBaseEncoding base);
  PdfStatus applyDifferences(const PdfObjectArena& arena, uint16_t differencesArrayIndex);
  PdfStatus decode(uint8_t code, PdfUtf8Value* value);

  uint16_t differenceCount() const { return differenceCount_; }
  bool fullyResident() const { return spillCount_ == 0; }

 private:
  PdfStatus addDifference(uint8_t code, uint32_t scalar);
  PdfStatus readDifference(uint16_t ordinal, PdfEncodingDifference* difference);
  PdfStatus setSourceAccess(bool required);

  PdfEncodingWorkspace workspace_{};
  PdfBaseEncoding base_ = PdfBaseEncoding::Standard;
  uint16_t differenceCount_ = 0;
  uint16_t spillCount_ = 0;
  bool sourceAccessRequired_ = false;
};

bool pdfGlyphNameToUnicode(const uint8_t* name, size_t length, uint32_t* scalar);
bool pdfConservativeLatinFallback(uint8_t code, uint32_t* scalar);
bool pdfWinAnsiFallback(uint8_t code, uint32_t* scalar);
PdfStatus pdfDecodePdfTextString(const uint8_t* source, size_t sourceLength, const PdfByteSink& sink);
