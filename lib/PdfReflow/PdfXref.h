#pragma once

#include <cstddef>
#include <cstdint>

#include "PdfIo.h"
#include "PdfObjectParser.h"

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
  explicit PdfXrefTable(PdfFixedRecordStore records) : records_(records) {}

  void reset();
  PdfStatus appendNewest(const PdfXrefEntry& entry);
  PdfStatus find(uint32_t objectNumber, PdfXrefEntry* entry) const;

  uint32_t entryCount() const { return entryCount_; }
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
};

class PdfXrefParser {
 public:
  PdfXrefParser(const PdfByteSource& source, uint8_t* sourceBuffer, size_t sourceBufferSize,
                PdfObjectArena& trailerArena, PdfXrefTable& table);

  void begin();
  PdfStepResult step(PdfWorkBudget& budget);

 private:
  enum class Phase : uint8_t {
    FindStartXref,
    ExpectXref,
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

  PdfByteSource source_{};
  uint8_t* sourceBuffer_ = nullptr;
  size_t sourceBufferSize_ = 0;
  PdfLexer lexer_;
  PdfObjectArena& trailerArena_;
  PdfObjectParser trailerParser_;
  PdfXrefTable& table_;
  PdfReadExactState tailRead_{};
  Phase phase_ = Phase::FindStartXref;
  uint64_t visitedOffsets_[32]{};
  uint8_t visitedCount_ = 0;
  uint32_t subsectionStart_ = 0;
  uint32_t subsectionCount_ = 0;
  uint32_t subsectionIndex_ = 0;
  uint64_t entryOffset_ = 0;
  uint16_t entryGeneration_ = 0;
};
