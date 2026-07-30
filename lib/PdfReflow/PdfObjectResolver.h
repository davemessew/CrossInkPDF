#pragma once

#include <cstddef>
#include <cstdint>

#include "PdfXref.h"

struct PdfObjectResolverWorkspace {
  using SourceAccessFn = PdfStatus (*)(void* context, bool sourceRequired);

  PdfStreamDecoder* streamDecoder = nullptr;
  PdfByteStore objectStreamStore{};
  void* sourceAccessContext = nullptr;
  SourceAccessFn setSourceAccess = nullptr;
};

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
                    size_t sourceBufferSize, PdfObjectArena& arena, PdfObjectResolverWorkspace workspace = {});

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
    DecodeObjectStream,
    ObjectStreamIndexNumber,
    ObjectStreamIndexOffset,
    ParseEmbeddedValue,
    Done,
    Failed,
  };

  PdfStatus beginUncompressed(PdfObjectReference reference, const PdfXrefEntry& entry);
  PdfStatus prepareObjectStream(uint64_t streamOffset);
  PdfStatus beginObjectStreamIndex();
  PdfStatus beginEmbeddedObject();
  PdfStatus setSourceAccess(bool sourceRequired);

  PdfByteSource source_{};
  const PdfXrefTable& xref_;
  PdfLexer lexer_;
  PdfObjectArena& arena_;
  PdfObjectParser parser_;
  PdfObjectResolverWorkspace workspace_{};
  PdfStreamDecoder* streamDecoder_ = nullptr;
  PdfByteRange objectStreamSourceRange_{};
  PdfByteRange objectStreamIndexRange_{};
  PdfByteRange objectStreamBodyRange_{};
  PdfByteSource objectStoreSource_{};
  PdfResolvedObject result_{};
  PdfObjectReference activeReference_{};
  Phase phase_ = Phase::Idle;
  uint64_t eolOffset_ = 0;
  uint64_t objectStreamFirst_ = 0;
  uint64_t objectStreamDecodedSize_ = 0;
  uint64_t objectStreamCurrentOffset_ = 0;
  uint64_t objectStreamPreviousOffset_ = 0;
  uint64_t objectStreamTargetStart_ = 0;
  uint64_t objectStreamTargetEnd_ = 0;
  uint32_t objectStreamNumber_ = 0;
  uint32_t objectStreamTargetIndex_ = 0;
  uint32_t objectStreamObjectCount_ = 0;
  uint32_t objectStreamIndexOrdinal_ = 0;
  uint32_t objectStreamIndexObjectNumber_ = 0;
  uint32_t cachedObjectStreamNumber_ = 0;
  uint32_t cachedObjectStreamCount_ = 0;
  uint64_t cachedObjectStreamFirst_ = 0;
  uint64_t cachedObjectStreamSize_ = 0;
  PdfStreamFilter streamFilters_[PdfLimits::MaxFiltersPerStream]{};
  uint8_t streamFilterCount_ = 0;
  uint8_t eolByte_ = 0;
  bool resolvingObjectStream_ = false;
  bool objectStreamTargetFound_ = false;
  bool sourceAccessRequired_ = true;
  PdfReadExactState eolRead_{};
};
