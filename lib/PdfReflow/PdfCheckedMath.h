#pragma once

#include <cstdint>
#include <limits>

constexpr bool pdfCheckedAdd(const uint64_t left, const uint64_t right, uint64_t* result) {
  if (result == nullptr || right > std::numeric_limits<uint64_t>::max() - left) {
    return false;
  }
  *result = left + right;
  return true;
}

constexpr bool pdfCheckedMultiply(const uint64_t left, const uint64_t right, uint64_t* result) {
  if (result == nullptr || (left != 0 && right > std::numeric_limits<uint64_t>::max() / left)) {
    return false;
  }
  *result = left * right;
  return true;
}

constexpr bool pdfCheckedRange(const uint64_t offset, const uint64_t length, const uint64_t sourceSize,
                               uint64_t* end = nullptr) {
  uint64_t rangeEnd = 0;
  if (!pdfCheckedAdd(offset, length, &rangeEnd) || rangeEnd > sourceSize) {
    return false;
  }
  if (end != nullptr) {
    *end = rangeEnd;
  }
  return true;
}

struct PdfFixed16 {
  int32_t raw = 0;

  static constexpr PdfFixed16 fromInteger(const int16_t value) {
    return {static_cast<int32_t>(value) * 65536};
  }
};

constexpr bool pdfFixedMultiply(const PdfFixed16 left, const PdfFixed16 right, PdfFixed16* result) {
  if (result == nullptr) {
    return false;
  }
  const int64_t product = static_cast<int64_t>(left.raw) * static_cast<int64_t>(right.raw);
  const int64_t scaled =
      product >= 0 ? (product + 32768) / 65536 : -(((-product) + 32768) / 65536);
  if (scaled < std::numeric_limits<int32_t>::min() || scaled > std::numeric_limits<int32_t>::max()) {
    return false;
  }
  result->raw = static_cast<int32_t>(scaled);
  return true;
}

constexpr bool pdfFixedAdd(const PdfFixed16 left, const PdfFixed16 right, PdfFixed16* result) {
  if (result == nullptr) {
    return false;
  }
  const int64_t sum = static_cast<int64_t>(left.raw) + right.raw;
  if (sum < std::numeric_limits<int32_t>::min() || sum > std::numeric_limits<int32_t>::max()) {
    return false;
  }
  result->raw = static_cast<int32_t>(sum);
  return true;
}

struct PdfMatrix {
  PdfFixed16 a = PdfFixed16::fromInteger(1);
  PdfFixed16 b{};
  PdfFixed16 c{};
  PdfFixed16 d = PdfFixed16::fromInteger(1);
  PdfFixed16 e{};
  PdfFixed16 f{};
};

constexpr bool pdfTransformPoint(const PdfMatrix& matrix, const PdfFixed16 x, const PdfFixed16 y,
                                 PdfFixed16* transformedX, PdfFixed16* transformedY) {
  PdfFixed16 ax;
  PdfFixed16 cy;
  PdfFixed16 bx;
  PdfFixed16 dy;
  PdfFixed16 sumX;
  PdfFixed16 sumY;
  return transformedX != nullptr && transformedY != nullptr && pdfFixedMultiply(matrix.a, x, &ax) &&
         pdfFixedMultiply(matrix.c, y, &cy) && pdfFixedAdd(ax, cy, &sumX) &&
         pdfFixedAdd(sumX, matrix.e, transformedX) && pdfFixedMultiply(matrix.b, x, &bx) &&
         pdfFixedMultiply(matrix.d, y, &dy) && pdfFixedAdd(bx, dy, &sumY) &&
         pdfFixedAdd(sumY, matrix.f, transformedY);
}
