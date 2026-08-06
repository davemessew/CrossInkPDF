#pragma once

#include <cstddef>
#include <cstdint>

#include "PdfIo.h"
#include "PdfObjectParser.h"
#include "PdfStreamBoundary.h"
#include "PdfStreamDecoder.h"

enum class PdfXrefEntryType : uint8_t {
  Free,
  Uncompressed,
  Compressed,
};

struct PdfXrefEntry {
  uint32_t objectNumber = 0;
  uint16_t generation = 0;
  PdfXrefEntryType type = PdfXrefEntryType::Free;
  uint8_t reserved = 0;
  uint64_t offset = 0;
  uint32_t objectStreamIndex = 0;
};

enum class PdfXrefLookupPhase : uint8_t {
  Idle,
  Linear,
  Binary,
  Verify,
  Done,
  Failed,
};

// Caller-owned state for a bounded, resumable xref lookup. Keeping this state
// outside PdfXrefTable allows independent resolvers without heap allocation.
struct PdfXrefLookupState {
  PdfXrefEntry entry{};
  PdfStatus status{};
  uint32_t objectNumber = 0;
  uint32_t first = 0;
  uint32_t last = 0;
  uint32_t cursor = 0;
  PdfXrefLookupPhase phase = PdfXrefLookupPhase::Idle;
};

class PdfXrefTable {
 public:
  explicit PdfXrefTable(const PdfFixedRecordStore& records) : records_(records) {}

  // Optional newest-first duplicate filter. The caller owns these phase-local
  // bitset spans; together they need one bit for every legal object number,
  // including zero. The second span may be null when the first is large enough.
  PdfStatus configureNewestObjectFilter(uint8_t* first, size_t firstBytes, uint8_t* second, size_t secondBytes);
  // Stops using the caller-owned spans without clearing them. Call this before
  // either span is repurposed by a later processing phase.
  void detachNewestObjectFilter();
  void reset();
  PdfStatus appendNewest(const PdfXrefEntry& entry);
  // Adopts caller-provided records that are already sorted by object number.
  // The store is not scanned; callers retain responsibility for ordering and
  // duplicate removal.
  PdfStatus adoptSortedRecords(uint32_t count);
  // Both fixed-record stores must outlive the table because the finalized
  // records may reside in either store after the last merge pass.
  PdfStatus finalize(PdfFixedRecordStore scratch, PdfXrefEntry* mergeBuffer, uint16_t mergeCapacity);
  PdfStatus beginFind(uint32_t objectNumber, PdfXrefLookupState* state) const;
  PdfStepResult stepFind(PdfXrefLookupState& state, PdfXrefEntry* entry, PdfWorkBudget& budget) const;
  PdfStatus find(uint32_t objectNumber, PdfXrefEntry* entry) const;

  uint32_t entryCount() const { return entryCount_; }
  bool finalized() const { return finalized_; }
  bool recordsAlreadySortedUnique() const { return appendOrderStrict_; }
  void setRoot(PdfObjectReference root) {
    root_ = root;
    hasRoot_ = true;
  }
  bool root(PdfObjectReference* root) const;
  void setInfo(PdfObjectReference info) {
    info_ = info;
    hasInfo_ = true;
  }
  bool info(PdfObjectReference* info) const;

 private:
  PdfFixedRecordStore records_{};
  uint32_t entryCount_ = 0;
  PdfObjectReference root_{};
  PdfObjectReference info_{};
  bool hasRoot_ = false;
  bool hasInfo_ = false;
  bool finalized_ = false;
  bool appendOrderStrict_ = true;
  uint32_t lastAppendedObject_ = 0;
  uint8_t* seenObjectsFirst_ = nullptr;
  uint8_t* seenObjectsSecond_ = nullptr;
  size_t seenObjectsFirstBytes_ = 0;
  size_t seenObjectsSecondBytes_ = 0;
};

class PdfXrefParser {
 public:
  PdfXrefParser(const PdfByteSource& source, uint8_t* sourceBuffer, size_t sourceBufferSize,
                PdfObjectArena& trailerArena, PdfXrefTable& table, PdfStreamDecoder* streamDecoder = nullptr,
                PdfStreamDecodeLimits decodeLimits = {});

  void begin();
  PdfStepResult step(PdfWorkBudget& budget);
  uint64_t currentDecodedBytes() const;
  uint64_t decodedBytes() const { return decodedBytes_; }

 private:
  enum class Phase : uint8_t {
    FindStartXref,
    ExpectXref,
    StreamGeneration,
    StreamObjKeyword,
    ParseStreamDictionary,
    StreamKeyword,
    StreamFirstEol,
    StreamSecondEol,
    ValidateStreamBoundary,
    DecodeXrefStream,
    SectionStartOrTrailer,
    SectionCount,
    EntryOffset,
    EntryGeneration,
    EntryState,
    ParseTrailer,
    Done,
    Failed,
  };

  PdfStatus enterSection(uint64_t offset);
  PdfStatus consumeTrailer();
  PdfStatus consumeCommonDictionary(uint16_t rootIndex);
  PdfStatus configureXrefStream(uint16_t rootIndex);
  PdfStatus beginXrefStreamDecode();
  PdfStatus finishXrefStream();
  PdfStatus advanceXrefRange();
  PdfStatus consumeXrefByte(uint8_t byte);
  static PdfStatus writeDecodedXref(void* context, const uint8_t* source, size_t requested, size_t* bytesWritten);

  PdfByteSource source_{};
  uint8_t* sourceBuffer_ = nullptr;
  size_t sourceBufferSize_ = 0;
  PdfLexer lexer_;
  PdfObjectArena& trailerArena_;
  PdfObjectParser trailerParser_;
  PdfXrefTable& table_;
  PdfStreamDecoder* streamDecoder_ = nullptr;
  PdfStreamDecodeLimits decodeLimits_{};
  PdfByteRange xrefStreamRange_{};
  PdfReadExactState tailRead_{};
  PdfReadExactState eolRead_{};
  Phase phase_ = Phase::FindStartXref;
  uint64_t visitedOffsets_[32]{};
  uint8_t visitedCount_ = 0;
  uint32_t subsectionStart_ = 0;
  uint32_t subsectionCount_ = 0;
  uint32_t subsectionIndex_ = 0;
  uint64_t entryOffset_ = 0;
  uint64_t sectionOffset_ = 0;
  uint64_t streamObjectNumber_ = 0;
  uint64_t streamLength_ = 0;
  uint64_t eolOffset_ = 0;
  uint64_t pendingPrev_ = 0;
  uint64_t decodedBytes_ = 0;
  uint64_t xrefFieldValues_[3]{};
  uint32_t xrefExpectedEntries_ = 0;
  uint32_t xrefDecodedEntries_ = 0;
  uint32_t xrefCurrentObject_ = 0;
  uint32_t xrefRangeRemaining_ = 0;
  uint16_t xrefIndexValue_ = PDF_INVALID_INDEX;
  uint16_t xrefIndexLink_ = PDF_INVALID_INDEX;
  uint16_t entryGeneration_ = 0;
  uint8_t eolByte_ = 0;
  uint8_t xrefWidths_[3]{};
  PdfStreamFilter streamFilters_[PdfLimits::MaxFiltersPerStream]{};
  uint8_t xrefFieldIndex_ = 0;
  uint8_t xrefFieldByteIndex_ = 0;
  uint8_t xrefIndexPairsRemaining_ = 0;
  uint8_t streamFilterCount_ = 0;
  bool hasPendingPrev_ = false;
  bool hasPendingEntry_ = false;
  bool pendingEntryFromStream_ = false;
  bool xrefUsesDefaultIndex_ = false;
};
