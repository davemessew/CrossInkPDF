#pragma once

#include <cstddef>
#include <cstdint>

#include "PdfXref.h"

struct PdfResolvedObject {
  PdfObjectReference reference{};
  uint16_t rootIndex = PDF_INVALID_INDEX;
  uint64_t streamOffset = 0;
  uint64_t streamLength = 0;
  bool hasStream = false;
};

class PdfObjectResolver {
 public:
  PdfObjectResolver(const PdfByteSource& source, const PdfXrefTable& xref, uint8_t* sourceBuffer,
                    size_t sourceBufferSize, PdfObjectArena& arena);

  PdfStatus begin(PdfObjectReference reference);
  PdfStepResult step(PdfWorkBudget& budget);
  const PdfResolvedObject& result() const { return result_; }

 private:
  enum class Phase : uint8_t {
    Idle,
    ObjectNumber,
    Generation,
    ObjKeyword,
    ParseValue,
    AfterValue,
    StreamFirstEol,
    StreamSecondEol,
    Done,
    Failed,
  };

  PdfByteSource source_{};
  const PdfXrefTable& xref_;
  PdfLexer lexer_;
  PdfObjectArena& arena_;
  PdfObjectParser parser_;
  PdfResolvedObject result_{};
  Phase phase_ = Phase::Idle;
  uint64_t eolOffset_ = 0;
  uint8_t eolByte_ = 0;
  PdfReadExactState eolRead_{};
};
