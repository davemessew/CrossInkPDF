#include "PdfWordCounter.h"

#include <limits>

#include "PdfUnicode.h"

namespace {

bool isInternalConnector(const uint32_t scalar) {
  return scalar == '\'' || scalar == '-' || scalar == 0x2010 || scalar == 0x2011 || scalar == 0x2019;
}

bool isCombiningMark(const uint32_t scalar) {
  return (scalar >= 0x0300 && scalar <= 0x036F) || (scalar >= 0x1DC0 && scalar <= 0x1DFF) ||
         (scalar >= 0x20D0 && scalar <= 0x20FF) || (scalar >= 0xFE20 && scalar <= 0xFE2F);
}

bool isXmlScalar(const uint32_t scalar) {
  return scalar == 0x09 || scalar == 0x0A || scalar == 0x0D || (scalar >= 0x20 && scalar <= 0xD7FF) ||
         (scalar >= 0xE000 && scalar <= 0xFFFD) || (scalar >= 0x10000 && scalar <= 0x10FFFF);
}

}  // namespace

PdfStatus PdfWordCounter::reset(const uint32_t initialWords) {
  status_ = PdfStatus::success();
  byteOffset_ = 0;
  sequenceStart_ = 0;
  words_ = initialWords;
  scalar_ = 0;
  minimumScalar_ = 0;
  continuationBytes_ = 0;
  inWord_ = false;
  pendingConnector_ = false;
  finished_ = false;
  return status_;
}

PdfStatus PdfWordCounter::fail(const PdfStatus status) {
  if (status_.ok()) {
    status_ = status;
  }
  return status_;
}

PdfStatus PdfWordCounter::addWord(const uint64_t offset) {
  if (words_ == std::numeric_limits<uint32_t>::max()) {
    return fail(PdfStatus::failure(PdfError::LimitExceeded, offset));
  }
  ++words_;
  return PdfStatus::success();
}

PdfStatus PdfWordCounter::consumeScalar(const uint32_t scalar, const uint64_t offset) {
  if (!pdfIsUnicodeScalar(scalar) || !isXmlScalar(scalar)) {
    return fail(PdfStatus::failure(PdfError::Malformed, offset));
  }
  if (isCombiningMark(scalar)) {
    return PdfStatus::success();
  }
  if (pdfIsCjkReadingUnit(scalar)) {
    inWord_ = false;
    pendingConnector_ = false;
    return addWord(offset);
  }
  if (pdfIsUnicodeLetterOrDigit(scalar)) {
    pendingConnector_ = false;
    if (!inWord_) {
      const PdfStatus status = addWord(offset);
      if (!status.ok()) {
        return status;
      }
      inWord_ = true;
    }
    return PdfStatus::success();
  }
  if (isInternalConnector(scalar) && inWord_ && !pendingConnector_) {
    pendingConnector_ = true;
    return PdfStatus::success();
  }
  inWord_ = false;
  pendingConnector_ = false;
  return PdfStatus::success();
}

PdfStatus PdfWordCounter::consume(const uint8_t* const text, const size_t length) {
  if (!status_.ok()) {
    return status_;
  }
  if (finished_ || (text == nullptr && length != 0)) {
    return fail(PdfStatus::failure(PdfError::InvalidArgument, byteOffset_));
  }
  for (size_t index = 0; index < length; ++index) {
    if (byteOffset_ == std::numeric_limits<uint64_t>::max()) {
      return fail(PdfStatus::failure(PdfError::LimitExceeded, byteOffset_));
    }
    const uint8_t byte = text[index];
    const uint64_t currentOffset = byteOffset_;
    ++byteOffset_;
    if (continuationBytes_ == 0) {
      if ((byte & 0x80U) == 0) {
        const PdfStatus status = consumeScalar(byte, currentOffset);
        if (!status.ok()) {
          return status;
        }
        continue;
      }
      sequenceStart_ = currentOffset;
      if ((byte & 0xE0U) == 0xC0U) {
        scalar_ = byte & 0x1FU;
        minimumScalar_ = 0x80;
        continuationBytes_ = 1;
      } else if ((byte & 0xF0U) == 0xE0U) {
        scalar_ = byte & 0x0FU;
        minimumScalar_ = 0x800;
        continuationBytes_ = 2;
      } else if ((byte & 0xF8U) == 0xF0U) {
        scalar_ = byte & 0x07U;
        minimumScalar_ = 0x10000;
        continuationBytes_ = 3;
      } else {
        return fail(PdfStatus::failure(PdfError::Malformed, currentOffset));
      }
      continue;
    }
    if ((byte & 0xC0U) != 0x80U) {
      return fail(PdfStatus::failure(PdfError::Malformed, currentOffset));
    }
    scalar_ = (scalar_ << 6U) | (byte & 0x3FU);
    --continuationBytes_;
    if (continuationBytes_ == 0) {
      if (scalar_ < minimumScalar_) {
        return fail(PdfStatus::failure(PdfError::Malformed, sequenceStart_));
      }
      const PdfStatus status = consumeScalar(scalar_, sequenceStart_);
      if (!status.ok()) {
        return status;
      }
    }
  }
  return PdfStatus::success();
}

PdfStatus PdfWordCounter::finish() {
  if (!status_.ok()) {
    return status_;
  }
  if (continuationBytes_ != 0) {
    return fail(PdfStatus::failure(PdfError::Malformed, sequenceStart_));
  }
  finished_ = true;
  return PdfStatus::success();
}
