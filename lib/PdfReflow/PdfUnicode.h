#pragma once

#include <cstddef>
#include <cstdint>

#include "PdfTypes.h"

struct PdfUtf8Value {
  uint8_t bytes[16]{};
  uint8_t length = 0;
};

bool pdfIsUnicodeScalar(uint32_t scalar);
bool pdfIsUnicodeLetterOrDigit(uint32_t scalar);
bool pdfIsCjkReadingUnit(uint32_t scalar);

PdfStatus pdfAppendUtf8Scalar(uint32_t scalar, uint8_t* destination, size_t capacity, size_t* length);
PdfStatus pdfDecodeUtf16Be(const uint8_t* source, size_t sourceLength, PdfUtf8Value* value);
PdfStatus pdfDecodeSingleUtf16BeScalar(const uint8_t* source, size_t sourceLength, uint32_t* scalar);
