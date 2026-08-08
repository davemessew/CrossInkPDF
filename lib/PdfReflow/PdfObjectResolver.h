#pragma once

#include <cstddef>
#include <cstdint>

#include "PdfStreamBoundary.h"
#include "PdfXref.h"

class PdfSecurity;

enum class PdfObjectResolverReader : uint8_t {
  Source,
  Xref,
  ObjectStoreWriter,
  ObjectStore,
};

struct PdfObjectResolverWorkspace {
  using SourceAccessFn = PdfStepResult (*)(void* context, PdfObjectResolverReader reader, PdfWorkBudget& budget);
  using CachedObjectStreamLookupFn = bool (*)(void* context, uint32_t objectStreamNumber,
                                               uint32_t* objectCount, uint64_t* first, uint64_t* size);
  using CachedObjectStreamPrepareFn = PdfStatus (*)(void* context, uint64_t requiredBytes,
                                                     bool* cacheCleared);
  using CachedObjectStreamPublishFn = void (*)(void* context, uint32_t objectStreamNumber,
                                                uint32_t objectCount, uint64_t first, uint64_t size);

  PdfStreamDecoder* streamDecoder = nullptr;
  PdfByteStore objectStreamStore{};
  void* sourceAccessContext = nullptr;
  SourceAccessFn setSourceAccess = nullptr;
  PdfStreamDecodeLimits decodeLimits{};
  PdfSecurity* security = nullptr;
  CachedObjectStreamLookupFn lookupCachedObjectStream = nullptr;
  CachedObjectStreamPrepareFn prepareCachedObjectStream = nullptr;
  CachedObjectStreamPublishFn publishCachedObjectStream = nullptr;
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
  void setStringTokenBuffer(uint8_t* buffer, size_t capacity) { parser_.setStringTokenBuffer(buffer, capacity); }
  void setSkipUnusedPageResources(bool enabled) { parser_.setSkipUnusedPageResources(enabled); }
  PdfStepResult step(PdfWorkBudget& budget);
  const PdfResolvedObject& result() const { return result_; }
  uint64_t currentStreamBytes() const;
  uint64_t takeCompletedStreamBytes();
  // External users of the single SD reader (for example the page-tree spool)
  // invalidate the cached source/xref selection. The next resolver step then
  // reasserts the required reader through the access callback.
  void invalidateSourceAccess() { sourceAccessKnown_ = false; }

 private:
  enum class Phase : uint8_t {
    Idle,
    SelectXref,
    LookupReference,
    SelectObjectStreamXref,
    LookupObjectStream,
    SelectUncompressedSource,
    SelectObjectStreamSource,
    ConfigureObjectStream,
    SelectObjectStoreWriter,
    ResetObjectStreamStore,
    BeginObjectStreamDecode,
    DecodeObjectStream,
    SelectObjectStore,
    SelectDirectObjectStream,
    ActivateObjectStream,
    ObjectNumber,
    Generation,
    ObjKeyword,
    ParseValue,
    AfterValue,
    StreamFirstEol,
    StreamSecondEol,
    ValidateStreamBoundary,
    ObjectStreamIndexNumber,
    ObjectStreamIndexOffset,
    ParseEmbeddedValue,
    Done,
    Failed,
  };

  PdfStatus beginUncompressed(PdfObjectReference reference, const PdfXrefEntry& entry);
  PdfStepResult stepSourceAccess(PdfObjectResolverReader reader, PdfWorkBudget& budget);
  PdfStatus configureObjectStream(uint64_t streamOffset);
  PdfStatus beginObjectStreamDecode();
  PdfStatus activateObjectStream();
  PdfStatus beginObjectStreamIndex();
  PdfStatus beginEmbeddedObject();
  PdfStatus beginIndirectLength(PdfObjectReference reference);
  PdfStatus finishIndirectLength();
  PdfObjectReference lookupTargetReference() const;
  bool resolvingIndirectLength() const;
  bool hasResolvedIndirectLength() const;
  void clearObjectStreamCache();

  PdfByteSource source_{};
  const PdfXrefTable& xref_;
  PdfLexer lexer_;
  PdfObjectArena& arena_;
  PdfObjectParser parser_;
  PdfObjectResolverWorkspace workspace_{};
  PdfStreamDecoder* streamDecoder_ = nullptr;
  PdfByteRange objectStreamSourceRange_{};
  PdfByteRange objectStreamSliceRange_{};
  PdfByteSource objectStoreSource_{};
  PdfResolvedObject result_{};
  PdfObjectReference activeReference_{};
  PdfXrefLookupState xrefLookup_{};
  Phase phase_ = Phase::Idle;
  uint64_t eolOffset_ = 0;
  uint64_t pendingObjectStreamOffset_ = 0;
  uint64_t objectStreamFirst_ = 0;
  uint64_t objectStreamDecodedSize_ = 0;
  uint64_t objectStreamCurrentOffset_ = 0;
  uint64_t objectStreamPreviousOffset_ = 0;
  uint64_t objectStreamTargetStart_ = 0;
  uint64_t objectStreamTargetEnd_ = 0;
  uint64_t completedStreamBytes_ = 0;
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
  bool objectStreamUsesStore_ = false;
  bool cachedObjectStreamUsesStore_ = false;
  bool publishObjectStream_ = false;
  PdfObjectResolverReader sourceAccess_ = PdfObjectResolverReader::Source;
  bool sourceAccessKnown_ = true;
  PdfReadExactState eolRead_{};
};
