#pragma once

#include <cstddef>
#include <cstdint>

#include "PdfIo.h"
#include "PdfObjectParser.h"
#include "PdfSecurity.h"
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

static_assert(sizeof(PdfXrefEntry) == 24, "xref read-ahead storage assumes 24-byte records");

enum class PdfXrefLookupPhase : uint8_t {
  Idle,
  Linear,
  BuildSampleIndex,
  Binary,
  Verify,
  ReadAhead,
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
  uint32_t readAheadToken = 0;
  PdfXrefLookupPhase phase = PdfXrefLookupPhase::Idle;
};

class PdfXrefTable {
 public:
  explicit PdfXrefTable(const PdfFixedRecordStore& records) : records_(records) {}

  // Optional newest-first duplicate filter. The caller owns these phase-local
  // spans, which hold a bounded exact set independent of the object-number
  // domain. An unrepresentable collision requests exact external compaction
  // at the next revision boundary.
  PdfStatus configureNewestObjectFilter(uint8_t* first, size_t firstBytes, uint8_t* second, size_t secondBytes);
  // Stops using the caller-owned spans without clearing them. Call this before
  // either span is repurposed by a later processing phase.
  void detachNewestObjectFilter();
  void reset();
  PdfStatus preflightAppend(uint32_t count) const;
  PdfStatus preflightAppendRange(uint32_t firstObject, uint32_t count) const;
  void prepareNewestObjectRange(uint32_t firstObject, uint32_t count);
  PdfStatus appendNewest(const PdfXrefEntry& entry);
  PdfStatus flushPendingWrites();
  bool sectionCompactionRequired() const { return sectionCompactionRequired_; }
  PdfStatus adoptCompactedRecords(const PdfFixedRecordStore& records, uint32_t count,
                                  uint32_t lastObjectNumber);
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
  bool recordsAreTwoSortedRuns(uint32_t* secondRunStart) const;
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
  PdfStatus setSecurity(const PdfSecurityTrailer& security);
  bool security(PdfSecurityTrailer* security) const;

 private:
  static constexpr uint8_t kLookupWindowEntries = 16;
  // Sparse resource lookups often form accidental pairs. Require a sustained
  // forward run before paying for a full window; any cache hit restores trust.
  static constexpr uint8_t kLocalityStreakRequired = 3;
  static constexpr uint8_t kSampleIndexEntries = UINT8_MAX;
  static constexpr uint8_t kSampleIndexLookupThreshold = 4;
  static constexpr uint16_t kSampleIndexMinimumRecords = 128;
  static constexpr uint8_t kVictimEntries = 24;
  static constexpr uint8_t kAppendBatchEntries = kLookupWindowEntries + kVictimEntries;

  void initializeSampleIndex();
  void configureBinaryLookup(PdfXrefLookupState* state) const;
  void rememberVictim(const PdfXrefEntry& entry, uint32_t ordinal) const;
  bool newestObjectAlreadySeen(uint32_t objectNumber);
  PdfXrefEntry* lookupWindowEntries() const { return entryStorage_; }
  PdfXrefEntry* victimEntries() const { return entryStorage_ + kLookupWindowEntries; }

  PdfFixedRecordStore records_{};
  uint32_t entryCount_ = 0;
  PdfObjectReference root_{};
  PdfObjectReference info_{};
  PdfSecurityTrailer security_{};
  bool hasRoot_ = false;
  bool hasInfo_ = false;
  bool hasSecurity_ = false;
  bool finalized_ = false;
  bool appendOrderStrict_ = true;
  uint32_t lastAppendedObject_ = 0;
  uint32_t secondSortedRunStart_ = 0;
  uint8_t* seenObjectsFirst_ = nullptr;
  uint8_t* seenObjectsSecond_ = nullptr;
  size_t seenObjectsFirstBytes_ = 0;
  size_t seenObjectsSecondBytes_ = 0;
  // Parsing and lookup are phase-disjoint. During append, use the complete
  // cache storage as one larger SD write batch; after finalization, split it
  // back into the lookup window and victim cache without adding RAM.
  mutable PdfXrefEntry entryStorage_[kAppendBatchEntries]{};
  mutable uint32_t sampleObjectNumbers_[kSampleIndexEntries]{};
  mutable uint32_t victimOrdinals_[kVictimEntries]{};
  mutable uint32_t lookupWindowFirstOrdinal_ = 0;
  mutable uint32_t lastLookupOrdinal_ = 0;
  mutable uint32_t lookupWindowToken_ = 0;
  mutable uint32_t sampleStride_ = 0;
  mutable uint8_t lookupWindowCount_ = 0;
  mutable uint8_t localityStreak_ = 0;
  mutable uint8_t sampleCount_ = 0;
  mutable uint8_t sampleBuildCount_ = 0;
  mutable uint8_t lookupMissCount_ = 0;
  mutable uint8_t victimCount_ = 0;
  uint8_t appendBatchCount_ = 0;
  uint8_t sortedRunCount_ = 0;
  mutable bool hasLastLookupOrdinal_ = false;
  mutable bool sampleIndexReady_ = false;
  mutable bool sampleIndexDisabled_ = false;
  bool sectionCompactionRequired_ = false;
  bool newestObjectDense_ = false;
};

static_assert(sizeof(PdfXrefEntry) * 40U == 960U, "xref entry cache and append batch must stay at 960 bytes");
static_assert(sizeof(uint32_t) * UINT8_MAX + sizeof(PdfXrefEntry) * 24U + sizeof(uint32_t) * 24U == 1692U,
              "sampled and victim xref indexes must stay within 1692 bytes");

class PdfXrefParser {
 public:
  PdfXrefParser(const PdfByteSource& source, uint8_t* sourceBuffer, size_t sourceBufferSize,
                PdfObjectArena& trailerArena, PdfXrefTable& table, PdfStreamDecoder* streamDecoder = nullptr,
                PdfStreamDecodeLimits decodeLimits = {});

  void begin();
  PdfStepResult step(PdfWorkBudget& budget);
  uint64_t currentDecodedBytes() const;
  uint64_t decodedBytes() const { return decodedBytes_; }
  bool compactionRequested() const { return phase_ == Phase::AwaitCompaction; }
  PdfStatus resumeAfterCompaction();

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
    AwaitCompaction,
    Done,
    Failed,
  };

  PdfStatus enterSection(uint64_t offset);
  PdfStatus finishSection();
  PdfStatus consumeTrailer();
  PdfStatus consumeCommonDictionary(uint16_t rootIndex);
  PdfStatus configureXrefStream(uint16_t rootIndex);
  PdfStatus beginXrefStreamDecode();
  PdfStatus finishXrefStream();
  PdfStepResult stepAdvanceXrefRange(PdfWorkBudget& budget);
  PdfStatus consumeXrefByte(uint8_t byte);
  static PdfStatus consumeNamedIndex(void* context, PdfNamedIntegerArrayEvent event, int64_t value,
                                     uint64_t sourceOffset);
  static PdfStatus writeDecodedXref(void* context, const uint8_t* source, size_t requested, size_t* bytesWritten);

  PdfByteSource source_{};
  uint8_t* sourceBuffer_ = nullptr;
  size_t sourceBufferSize_ = 0;
  PdfLexer lexer_;
  PdfObjectArena& trailerArena_;
  PdfObjectParser trailerParser_;
  PdfNamedIntegerArraySink xrefIndexSink_{};
  PdfXrefTable& table_;
  PdfStreamDecoder* streamDecoder_ = nullptr;
  PdfStreamDecodeLimits decodeLimits_{};
  PdfByteRange xrefStreamRange_{};
  PdfReadExactState tailRead_{};
  PdfReadExactState eolRead_{};
  Phase phase_ = Phase::FindStartXref;
  uint64_t prevCycleAnchor_ = 0;
  uint32_t prevCyclePower_ = 0;
  uint32_t prevCycleLength_ = 0;
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
  uint64_t xrefIndexStartOffset_ = 0;
  uint64_t xrefIndexEndOffset_ = 0;
  uint64_t xrefIndexReadOffset_ = 0;
  uint64_t xrefIndexNumber_ = 0;
  uint64_t xrefFieldValues_[3]{};
  uint32_t xrefExpectedEntries_ = 0;
  uint32_t xrefDecodedEntries_ = 0;
  uint32_t xrefCurrentObject_ = 0;
  uint32_t xrefRangeRemaining_ = 0;
  uint32_t xrefIndexObservedFirst_ = 0;
  uint32_t xrefIndexPairsRemaining_ = 0;
  uint16_t entryGeneration_ = 0;
  uint8_t eolByte_ = 0;
  uint64_t xrefWidths_[3]{};
  uint8_t xrefIndexBuffer_[32]{};
  PdfStreamFilter streamFilters_[PdfLimits::MaxFiltersPerStream]{};
  PdfStreamDecodeParameters streamDecodeParameters_{};
  uint8_t xrefFieldIndex_ = 0;
  uint64_t xrefFieldByteIndex_ = 0;
  uint8_t xrefIndexBufferPosition_ = 0;
  uint8_t xrefIndexBufferLength_ = 0;
  uint8_t streamFilterCount_ = 0;
  bool hasPendingPrev_ = false;
  bool hasPrevCycleAnchor_ = false;
  bool hasPendingEntry_ = false;
  bool pendingEntryFromStream_ = false;
  bool xrefUsesDefaultIndex_ = false;
  bool xrefIndexSeen_ = false;
  bool xrefIndexHaveObservedFirst_ = false;
  bool xrefIndexNeedRange_ = false;
  bool xrefIndexScanHaveFirst_ = false;
  bool xrefIndexNumberStarted_ = false;
  bool xrefIndexNumberHasDigits_ = false;
  bool xrefIndexInComment_ = false;
};
