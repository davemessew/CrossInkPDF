#pragma once

#include <cstdint>
#include <string>
#include <string_view>

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

class DirectoryDeleteSession {};

inline Result deletePdfBook(const std::string& sourcePath) {
  ++TEST_STATE.pdfDeleteCalls;
  TEST_STATE.pdfDeletePath = sourcePath;
  return static_cast<Result>(TEST_STATE.pdfDeleteResult);
}

inline Result deletePdfBookNoPathAlloc(DirectoryDeleteSession&,
                                       const std::string_view sourcePath) {
  ++TEST_STATE.pdfDirectoryDeleteCalls;
  TEST_STATE.pdfDeletePath.assign(sourcePath.data(), sourcePath.size());
  return static_cast<Result>(TEST_STATE.pdfDeleteResult);
}

}  // namespace PdfDeleteUtils
