#pragma once

#include <cstddef>
#include <cstdint>

#include "PdfIo.h"
#include "PdfLexer.h"
#include "PdfLimits.h"
#include "PdfUnicode.h"

struct PdfCMapRecord {
  uint32_t sourceFirst = 0;
  uint32_t sourceLast = 0;
  uint32_t destinationFirst = 0;
  uint8_t sourceLength = 0;
  uint8_t utf8Length = 0;
  uint8_t flags = 0;
  uint8_t reserved = 0;
  uint8_t utf8[16]{};
};

static_assert(sizeof(PdfCMapRecord) <= 32, "CMap spill records must remain compact");

struct PdfCMapLookup {
  uint32_t sourceCode = 0;
  uint8_t sourceLength = 0;
  PdfUtf8Value unicode{};
};

struct PdfCMapCodeSpace {
  uint32_t first = 0;
  uint32_t last = 0;
  uint8_t length = 0;
};

struct PdfCMapWorkspace {
  using SourceAccessFn = PdfStatus (*)(void* context, bool sourceRequired);

  PdfCMapRecord* records = nullptr;
  uint16_t recordCapacity = 0;
  PdfFixedRecordStore spill{};
  void* sourceAccessContext = nullptr;
  SourceAccessFn setSourceAccess = nullptr;
};

class PdfCMap {
 public:
  PdfCMap(uint8_t* sourceBuffer, size_t sourceBufferSize, PdfCMapWorkspace workspace);

  PdfStatus begin(const PdfByteSource& source);
  PdfStepResult step(PdfWorkBudget& budget);
  PdfStatus lookup(const uint8_t* source, size_t sourceLength, PdfCMapLookup* result);
  PdfStatus copyCodeSpaces(PdfCMapCodeSpace* destination, size_t capacity, uint8_t* count) const;
  PdfStatus applyRecord(const PdfCMapRecord& record, uint32_t code, uint8_t codeLength, PdfCMapLookup* result);

  uint32_t mappingCount() const { return mappingCount_; }
  uint8_t codeSpaceCount() const { return codeSpaceCount_; }
  bool fullyResident() const { return spillCount_ == 0; }

 private:
  struct CodeSpace {
    uint32_t first = 0;
    uint32_t last = 0;
    uint8_t length = 0;
  };

  enum class Section : uint8_t {
    Idle,
    CodeSpace,
    BfChar,
    BfRange,
    BfRangeArray,
    AwaitSectionEnd,
    Done,
    Failed,
  };

  PdfStatus setSourceAccess(bool required);
  PdfStatus handleToken(const PdfToken& token);
  PdfStatus handleCodeSpace(const PdfToken& token);
  PdfStatus handleBfChar(const PdfToken& token);
  PdfStatus handleBfRange(const PdfToken& token);
  PdfStatus handleBfRangeArray(const PdfToken& token);
  PdfStatus addRecord(const PdfCMapRecord& record);
  PdfStatus addExact(uint32_t sourceCode, uint8_t sourceLength, const uint8_t* destination, size_t destinationLength);
  PdfStatus addSequential(uint32_t first, uint32_t last, uint8_t sourceLength, const uint8_t* destination,
                          size_t destinationLength);
  PdfStatus readRecord(uint32_t ordinal, PdfCMapRecord* record);
  PdfStatus decodeCode(const uint8_t* source, size_t sourceLength, uint32_t* code, uint8_t* codeLength) const;
  PdfStepResult fail(PdfStatus status);
  bool observingRecords() const;
  bool codeSpaceOnly() const;

  PdfCMapWorkspace workspace_{};
  PdfLexer lexer_;
  CodeSpace codeSpaces_[16]{};
  PdfCMapRecord pendingRecord_{};
  PdfCMapRecord cachedRecord_{};
  PdfStatus failure_{};
  Section section_ = Section::Idle;
  uint32_t pendingCount_ = 0;
  uint32_t sectionRemaining_ = 0;
  uint32_t mappingCount_ = 0;
  uint32_t spillCount_ = 0;
  uint64_t rangeArrayCode_ = 0;
  uint64_t rangeArrayLast_ = 0;
  uint32_t previousRecordLast_ = 0;
  uint64_t previousRecordKey_ = 0;
  uint8_t codeSpaceCount_ = 0;
  uint8_t field_ = 0;
  bool hasPendingCount_ = false;
  bool sourceAccessRequired_ = false;
  bool recordsSorted_ = true;
  bool hasPreviousRecord_ = false;
  bool hasCachedRecord_ = false;
};
