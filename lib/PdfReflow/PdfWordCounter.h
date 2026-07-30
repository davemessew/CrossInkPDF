#pragma once

#include <cstddef>
#include <cstdint>

#include "PdfTypes.h"

class PdfWordCounter {
 public:
  PdfStatus reset(uint32_t initialWords = 0);
  PdfStatus consume(const uint8_t* text, size_t length);
  PdfStatus finish();

  uint32_t words() const { return words_; }
  uint64_t bytesConsumed() const { return byteOffset_; }

 private:
  PdfStatus consumeScalar(uint32_t scalar, uint64_t offset);
  PdfStatus addWord(uint64_t offset);
  PdfStatus fail(PdfStatus status);

  PdfStatus status_{};
  uint64_t byteOffset_ = 0;
  uint64_t sequenceStart_ = 0;
  uint32_t words_ = 0;
  uint32_t scalar_ = 0;
  uint32_t minimumScalar_ = 0;
  uint8_t continuationBytes_ = 0;
  bool inWord_ = false;
  bool pendingConnector_ = false;
  bool finished_ = false;
};
