#pragma once

#include <PdfLayoutWordIndex.h>

#include <cstddef>
#include <cstdint>

class PdfWordCounter {
 public:
  void reset() { words_ = 0; }
  uint32_t words() const { return words_; }
  PdfStatus consume(const uint8_t*, size_t) { return {}; }
  PdfStatus finish() { return {}; }

 private:
  uint32_t words_ = 0;
};
