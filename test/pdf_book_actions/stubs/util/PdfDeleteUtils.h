#pragma once

#include <cstdint>
#include <string>

#include "TestState.h"

namespace PdfDeleteUtils {

enum class Result : uint8_t {
  Complete,
  NoPendingDelete,
  Unsupported,
  Invalid,
  Conflict,
  Pending,
};

inline Result deletePdfBook(const std::string& sourcePath) {
  ++TEST_STATE.pdfDeleteCalls;
  TEST_STATE.pdfDeletePath = sourcePath;
  return static_cast<Result>(TEST_STATE.pdfDeleteResult);
}

}  // namespace PdfDeleteUtils
