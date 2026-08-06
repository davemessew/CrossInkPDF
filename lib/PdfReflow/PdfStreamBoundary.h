#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

inline constexpr size_t PdfStreamBoundaryLookaheadBytes = 32;
inline constexpr size_t PdfMinimumStreamBoundaryBytes = 17;

inline bool pdfIsStreamBoundaryWhitespace(const uint8_t byte) {
  return byte == 0 || byte == '\t' || byte == '\n' || byte == '\f' || byte == '\r' || byte == ' ';
}

inline bool pdfStreamBoundaryHasKeyword(const uint8_t* const bytes, const size_t length, const size_t offset,
                                        const char* const keyword) {
  const size_t keywordLength = std::strlen(keyword);
  return offset <= length && keywordLength <= length - offset &&
         std::memcmp(bytes + offset, keyword, keywordLength) == 0;
}

inline bool pdfValidateStreamBoundary(const uint8_t* const bytes, const size_t length) {
  if (bytes == nullptr || length < PdfMinimumStreamBoundaryBytes) {
    return false;
  }
  size_t cursor = 0;
  if (bytes[cursor] == '\r') {
    ++cursor;
    if (cursor < length && bytes[cursor] == '\n') {
      ++cursor;
    }
  } else if (bytes[cursor] == '\n') {
    ++cursor;
  } else {
    return false;
  }
  if (!pdfStreamBoundaryHasKeyword(bytes, length, cursor, "endstream")) {
    return false;
  }
  cursor += 9;
  if (cursor >= length || !pdfIsStreamBoundaryWhitespace(bytes[cursor])) {
    return false;
  }
  while (cursor < length && pdfIsStreamBoundaryWhitespace(bytes[cursor])) {
    ++cursor;
  }
  if (!pdfStreamBoundaryHasKeyword(bytes, length, cursor, "endobj")) {
    return false;
  }
  cursor += 6;
  return cursor == length || pdfIsStreamBoundaryWhitespace(bytes[cursor]);
}
