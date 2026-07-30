#pragma once

#include <cstddef>
#include <cstdint>

#include "PdfIo.h"
#include "PdfObjectParser.h"
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

class PdfXrefTable {
 public:
  explicit PdfXrefTable(const PdfFixedRecordStore& records) : records_(records) {}

  void reset();
  PdfStatus appendNewest(const PdfXrefEntry& entry);
  // Both fixed-record stores must outlive the table because the finalized
  // records may reside in either store after the last merge pass.
  PdfStatus finalize(PdfFixedRecordStore scratch, PdfXrefEntry* mergeBuffer, uint16_t mergeCapacity);
  PdfStatus find(uint32_t objectNumber, PdfXrefEntry* entry) const;

  uint32_t entryCount() const { return entryCount_; }
  bool finalized() const { return finalized_; }
  void setRoot(PdfObjectReference root) {
    root_ = root;
    hasRoot_ = true;
  }
  bool root(PdfObjectReference* root) const;

 private:
  PdfFixedRecordStore records_{};
  uint32_t entryCount_ = 0;
  PdfObjectReference root_{};
  bool hasRoot_ = false;
  bool finalized_ = false;
};

class PdfXrefParser {
 public:
  PdfXrefParser(const PdfByteSource& source, uint8_t* sourceBuffer, size_t sourceBufferSize,
                PdfObjectArena& trailerArena, PdfXrefTable& table, PdfStreamDecoder* streamDecoder = nullptr);

  void begin();
  PdfStepResult step(PdfWorkBudget& budget);

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
  bool xrefUsesDefaultIndex_ = false;
};
