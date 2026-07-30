#include "PdfHiddenText.h"

#include <algorithm>
#include <cstring>
#include <limits>

#include "PdfUnicode.h"

namespace {

struct NormalizedIterator {
  const uint8_t* text = nullptr;
  size_t length = 0;
  size_t offset = 0;
  uint32_t pending = 0;
  bool hasPending = false;
  bool emitted = false;
};

bool isWhitespace(const uint32_t scalar) {
  return scalar == 0x09 || scalar == 0x0A || scalar == 0x0C || scalar == 0x0D || scalar == 0x20 || scalar == 0x00A0 ||
         scalar == 0x1680 || (scalar >= 0x2000 && scalar <= 0x200A) || scalar == 0x2028 || scalar == 0x2029 ||
         scalar == 0x202F || scalar == 0x205F || scalar == 0x3000;
}

uint32_t foldCase(const uint32_t scalar) {
  if (scalar >= 'A' && scalar <= 'Z') {
    return scalar + ('a' - 'A');
  }
  return scalar;
}

PdfStatus nextNormalized(NormalizedIterator& iterator, uint32_t* const scalar, bool* const atEnd) {
  if (scalar == nullptr || atEnd == nullptr || iterator.text == nullptr) {
    return PdfStatus::failure(PdfError::InvalidArgument);
  }
  *atEnd = false;
  if (iterator.hasPending) {
    iterator.hasPending = false;
    iterator.emitted = true;
    *scalar = iterator.pending;
    return PdfStatus::success();
  }

  bool skippedWhitespace = false;
  while (iterator.offset < iterator.length) {
    uint32_t value = 0;
    const PdfStatus status = pdfDecodeUtf8Scalar(iterator.text, iterator.length, &iterator.offset, &value);
    if (!status.ok()) {
      return status;
    }
    if (isWhitespace(value)) {
      skippedWhitespace = true;
      continue;
    }
    value = foldCase(value);
    if (skippedWhitespace && iterator.emitted) {
      iterator.pending = value;
      iterator.hasPending = true;
      *scalar = ' ';
    } else {
      iterator.emitted = true;
      *scalar = value;
    }
    return PdfStatus::success();
  }
  *atEnd = true;
  return PdfStatus::success();
}

bool validRect(const PdfRectangle& rect) { return rect.xMax > rect.xMin && rect.yMax > rect.yMin; }

PdfRectangle runRect(const PdfTextRun& run) { return {run.xMin, run.yMin, run.xMax, run.yMax}; }

PdfRectangle imageRect(const PdfImagePlacement& image) { return {image.xMin, image.yMin, image.xMax, image.yMax}; }

uint64_t width(const PdfRectangle& rect) {
  return validRect(rect) ? static_cast<uint64_t>(static_cast<int64_t>(rect.xMax) - rect.xMin) : 0;
}

uint64_t height(const PdfRectangle& rect) {
  return validRect(rect) ? static_cast<uint64_t>(static_cast<int64_t>(rect.yMax) - rect.yMin) : 0;
}

uint64_t area(const PdfRectangle& rect) { return width(rect) * height(rect); }

PdfRectangle intersection(const PdfRectangle& left, const PdfRectangle& right) {
  return {std::max(left.xMin, right.xMin), std::max(left.yMin, right.yMin), std::min(left.xMax, right.xMax),
          std::min(left.yMax, right.yMax)};
}

uint64_t requiredFraction(const uint64_t value, const uint8_t numerator, const uint8_t denominator) {
  const uint64_t whole = (value / denominator) * numerator;
  const uint64_t remainder = value % denominator;
  return whole + (remainder * numerator + denominator - 1) / denominator;
}

bool overlapsByFraction(const PdfRectangle& candidate, const PdfRectangle& other, const uint8_t numerator,
                        const uint8_t denominator) {
  const uint64_t candidateArea = area(candidate);
  return candidateArea != 0 &&
         area(intersection(candidate, other)) >= requiredFraction(candidateArea, numerator, denominator);
}

bool slice(const PdfHiddenTextContext& context, const PdfTextRun& run, const uint8_t** const text,
           size_t* const length) {
  if (text == nullptr || length == nullptr || context.text == nullptr || run.textOffset > context.textLength ||
      run.textLength > context.textLength - run.textOffset) {
    return false;
  }
  *text = context.text + run.textOffset;
  *length = run.textLength;
  return true;
}

bool prefixEquals(const char* text, const size_t length, const char* prefix) {
  const size_t prefixLength = std::strlen(prefix);
  return length >= prefixLength && std::memcmp(text, prefix, prefixLength) == 0;
}

PdfStatus inspectContent(const uint8_t* const text, const size_t length, bool* const meaningful,
                         bool* const metadataLike) {
  if (text == nullptr || meaningful == nullptr || metadataLike == nullptr) {
    return PdfStatus::failure(PdfError::InvalidArgument);
  }
  *meaningful = false;
  *metadataLike = false;
  NormalizedIterator iterator{text, length};
  char prefix[PdfHiddenTextLimits::MetadataPrefixBytes]{};
  size_t prefixLength = 0;
  while (true) {
    uint32_t scalar = 0;
    bool atEnd = false;
    const PdfStatus status = nextNormalized(iterator, &scalar, &atEnd);
    if (!status.ok()) {
      return status;
    }
    if (atEnd) {
      break;
    }
    if (pdfIsUnicodeLetterOrDigit(scalar)) {
      *meaningful = true;
    }
    if (scalar <= 0x7F && prefixLength < sizeof(prefix)) {
      prefix[prefixLength++] = static_cast<char>(scalar);
    }
  }
  static constexpr const char* METADATA_PREFIXES[] = {
      "producer:", "creator:", "creationdate:", "moddate:", "metadata:", "xmp:", "pdfaid:",
  };
  for (const char* candidate : METADATA_PREFIXES) {
    if (prefixEquals(prefix, prefixLength, candidate)) {
      *metadataLike = true;
      break;
    }
  }
  return PdfStatus::success();
}

PdfStatus normalizedEqual(const uint8_t* const left, const size_t leftLength, const uint8_t* const right,
                          const size_t rightLength, bool* const equal) {
  if (left == nullptr || right == nullptr || equal == nullptr) {
    return PdfStatus::failure(PdfError::InvalidArgument);
  }
  NormalizedIterator leftIterator{left, leftLength};
  NormalizedIterator rightIterator{right, rightLength};
  while (true) {
    uint32_t leftScalar = 0;
    uint32_t rightScalar = 0;
    bool leftEnd = false;
    bool rightEnd = false;
    PdfStatus status = nextNormalized(leftIterator, &leftScalar, &leftEnd);
    if (!status.ok()) {
      return status;
    }
    status = nextNormalized(rightIterator, &rightScalar, &rightEnd);
    if (!status.ok()) {
      return status;
    }
    if (leftEnd || rightEnd) {
      *equal = leftEnd && rightEnd;
      return PdfStatus::success();
    }
    if (leftScalar != rightScalar) {
      *equal = false;
      return PdfStatus::success();
    }
  }
}

uint64_t absoluteDistance(const int64_t left, const int64_t right) {
  return static_cast<uint64_t>(left >= right ? left - right : right - left);
}

bool duplicateGeometry(const PdfRectangle& page, const PdfTextRun& hidden, const PdfTextRun& visible) {
  const PdfRectangle hiddenRect = runRect(hidden);
  const PdfRectangle visibleRect = runRect(visible);
  if (!validRect(hiddenRect) || !validRect(visibleRect)) {
    return false;
  }
  const uint64_t overlapWidth =
      width(intersection({hiddenRect.xMin, 0, hiddenRect.xMax, 1}, {visibleRect.xMin, 0, visibleRect.xMax, 1}));
  const uint64_t smallerWidth = std::min(width(hiddenRect), width(visibleRect));
  if (overlapWidth < requiredFraction(smallerWidth, PdfHiddenTextLimits::DuplicateHorizontalOverlapNumerator,
                                      PdfHiddenTextLimits::DuplicateHorizontalOverlapDenominator)) {
    return false;
  }
  const int64_t hiddenCenter = static_cast<int64_t>(hiddenRect.yMin) + hiddenRect.yMax;
  const int64_t visibleCenter = static_cast<int64_t>(visibleRect.yMin) + visibleRect.yMax;
  const uint64_t pageThreshold = height(page) / PdfHiddenTextLimits::DuplicateVerticalPageDivisor;
  const uint64_t heightThreshold =
      std::max(height(hiddenRect), height(visibleRect)) * PdfHiddenTextLimits::DuplicateVerticalHeightMultiplier;
  // Centers are represented as twice the coordinate, so double the threshold too.
  return absoluteDistance(hiddenCenter, visibleCenter) <= 2 * std::max(pageThreshold, heightThreshold);
}

}  // namespace

PdfStatus pdfFingerprintNormalizedText(const uint8_t* const text, const size_t length,
                                       PdfTextFingerprint* const fingerprint) {
  if (text == nullptr || fingerprint == nullptr) {
    return PdfStatus::failure(PdfError::InvalidArgument);
  }
  PdfTextFingerprint result;
  NormalizedIterator iterator{text, length};
  while (true) {
    uint32_t scalar = 0;
    bool atEnd = false;
    const PdfStatus status = nextNormalized(iterator, &scalar, &atEnd);
    if (!status.ok()) {
      return status;
    }
    if (atEnd) {
      break;
    }
    for (uint8_t index = 0; index < 4; ++index) {
      result.hash ^= static_cast<uint8_t>(scalar >> (index * 8U));
      result.hash *= 1099511628211ULL;
    }
    if (result.units == std::numeric_limits<uint32_t>::max()) {
      return PdfStatus::failure(PdfError::LimitExceeded, result.units);
    }
    ++result.units;
  }
  *fingerprint = result;
  return PdfStatus::success();
}

PdfHiddenTextDecision pdfClassifyHiddenText(const PdfHiddenTextContext& context, const uint16_t candidateIndex) {
  if (context.runs == nullptr || candidateIndex >= context.runCount || context.text == nullptr ||
      !validRect(context.page)) {
    return PdfHiddenTextDecision::Unmappable;
  }
  const PdfTextRun& candidate = context.runs[candidateIndex];
  if ((candidate.flags & PdfTextHidden) == 0) {
    return PdfHiddenTextDecision::NotHidden;
  }
  if (candidate.textLength == 0) {
    return PdfHiddenTextDecision::Empty;
  }
  const uint8_t* candidateText = nullptr;
  size_t candidateLength = 0;
  if (!slice(context, candidate, &candidateText, &candidateLength)) {
    return PdfHiddenTextDecision::Unmappable;
  }
  bool meaningful = false;
  bool metadataLike = false;
  if (!inspectContent(candidateText, candidateLength, &meaningful, &metadataLike).ok() || !meaningful) {
    return PdfHiddenTextDecision::Unmappable;
  }
  if (metadataLike) {
    return PdfHiddenTextDecision::MetadataLike;
  }
  const PdfRectangle candidateRect = runRect(candidate);
  if (!validRect(candidateRect)) {
    return PdfHiddenTextDecision::ZeroArea;
  }
  if (candidate.baselineDx == 0 && candidate.baselineDy == 0) {
    return PdfHiddenTextDecision::ZeroTransform;
  }
  if (!overlapsByFraction(candidateRect, context.page, PdfHiddenTextLimits::MinPageOverlapNumerator,
                          PdfHiddenTextLimits::MinPageOverlapDenominator)) {
    return PdfHiddenTextDecision::OffPage;
  }
  bool overlapsImage = false;
  for (uint16_t imageIndex = 0; imageIndex < context.imageCount; ++imageIndex) {
    if (context.images != nullptr && overlapsByFraction(candidateRect, imageRect(context.images[imageIndex]),
                                                        PdfHiddenTextLimits::MinImageOverlapNumerator,
                                                        PdfHiddenTextLimits::MinImageOverlapDenominator)) {
      overlapsImage = true;
      break;
    }
  }
  if (!overlapsImage) {
    return PdfHiddenTextDecision::NoImageOverlap;
  }
  for (uint16_t runIndex = 0; runIndex < context.runCount; ++runIndex) {
    if (runIndex == candidateIndex || (context.runs[runIndex].flags & PdfTextHidden) != 0) {
      continue;
    }
    const PdfTextRun& visible = context.runs[runIndex];
    const uint8_t* visibleText = nullptr;
    size_t visibleLength = 0;
    if (!slice(context, visible, &visibleText, &visibleLength) || visibleLength == 0 ||
        !duplicateGeometry(context.page, candidate, visible)) {
      continue;
    }
    bool equal = false;
    if (normalizedEqual(candidateText, candidateLength, visibleText, visibleLength, &equal).ok() && equal) {
      return PdfHiddenTextDecision::DuplicateVisible;
    }
  }
  return PdfHiddenTextDecision::Qualified;
}
