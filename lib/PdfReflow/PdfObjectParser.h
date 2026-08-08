#pragma once

#include <cstddef>
#include <cstdint>

#include "PdfLexer.h"

inline constexpr uint16_t PDF_INVALID_INDEX = UINT16_MAX;

enum class PdfValueKind : uint8_t {
  Null,
  Boolean,
  Integer,
  Real,
  Name,
  String,
  Array,
  Dictionary,
  Reference,
};

struct PdfObjectReference {
  uint32_t objectNumber = 0;
  uint16_t generation = 0;

  constexpr bool operator==(const PdfObjectReference& other) const {
    return objectNumber == other.objectNumber && generation == other.generation;
  }
};

struct PdfValue {
  int64_t integerValue = 0;
  uint32_t objectNumber = 0;
  int32_t fixedValue = 0;
  PdfValueKind kind = PdfValueKind::Null;
  bool booleanValue = false;
  uint16_t firstLink = PDF_INVALID_INDEX;
  uint16_t lastLink = PDF_INVALID_INDEX;
  uint16_t count = 0;
  uint16_t textOffset = 0;
  uint16_t textLength = 0;
  uint16_t generation = 0;
};

static_assert(sizeof(PdfValue) == 32);

struct PdfDictionaryEntry {
  uint16_t next = PDF_INVALID_INDEX;
  uint16_t valueIndex = PDF_INVALID_INDEX;
  uint16_t keyOffset = 0;
  uint16_t keyLength = 0;
};

struct PdfArrayItem {
  uint16_t next = PDF_INVALID_INDEX;
  uint16_t valueIndex = PDF_INVALID_INDEX;
};

struct PdfObjectArena {
  PdfValue* values = nullptr;
  uint16_t valueCapacity = 0;
  PdfDictionaryEntry* dictionaryEntries = nullptr;
  uint16_t dictionaryCapacity = 0;
  PdfArrayItem* arrayItems = nullptr;
  uint16_t arrayCapacity = 0;
  uint8_t* text = nullptr;
  uint16_t textCapacity = 0;

  uint16_t valueCount = 0;
  uint16_t dictionaryCount = 0;
  uint16_t arrayCount = 0;
  uint16_t textLength = 0;

  void reset();
};

bool pdfDictionaryFind(const PdfObjectArena& arena, uint16_t dictionaryIndex, const char* key, uint16_t* valueIndex);
bool pdfArrayAt(const PdfObjectArena& arena, uint16_t arrayIndex, uint16_t ordinal, uint16_t* valueIndex);
bool pdfTextEquals(const PdfObjectArena& arena, const PdfValue& value, const char* expected);

enum class PdfNamedIntegerArrayEvent : uint8_t {
  Begin,
  Value,
  End,
};

using PdfNamedIntegerArrayCallback =
    PdfStatus (*)(void* context, PdfNamedIntegerArrayEvent event, int64_t value, uint64_t sourceOffset);

struct PdfNamedIntegerArraySink {
  const char* key = nullptr;
  uint16_t keyLength = 0;
  uint8_t dictionaryDepth = 0;
  void* context = nullptr;
  PdfNamedIntegerArrayCallback callback = nullptr;

  bool valid() const { return key != nullptr && keyLength != 0 && dictionaryDepth != 0 && callback != nullptr; }
};

class PdfObjectParser {
 public:
  PdfObjectParser(PdfLexer& lexer, PdfObjectArena& arena);

  void begin();
  PdfStepResult step(PdfWorkBudget& budget);
  uint16_t rootIndex() const { return rootIndex_; }
  void setStringTokenBuffer(uint8_t* buffer, size_t capacity) {
    stringTokenBuffer_ = buffer;
    stringTokenCapacity_ = capacity;
  }
  void setSkipUnusedPageResources(bool enabled) { skipUnusedPageResources_ = enabled; }
  // The sink is caller-owned and must outlive parsing. A pointer keeps the
  // generic parser small for users that do not stream named arrays.
  void setNamedIntegerArraySink(const PdfNamedIntegerArraySink* sink) { namedIntegerArraySink_ = sink; }

 private:
  struct Frame {
    uint16_t containerIndex = PDF_INVALID_INDEX;
    uint16_t pendingKeyOffset = 0;
    uint16_t pendingKeyLength = 0;
    bool dictionary = false;
    bool expectingKey = false;
  };

  PdfStatus addValue(const PdfValue& value, uint16_t* valueIndex);
  PdfStatus copyText(const PdfToken& token, uint16_t* offset, uint16_t* length);
  PdfStatus attachValue(uint16_t valueIndex);
  PdfStatus emitInteger(int64_t value);
  PdfStatus emitReference(int64_t objectNumber, int64_t generation);
  PdfStatus emitTokenValue(const PdfToken& token);
  PdfStatus openContainer(bool dictionary);
  PdfStatus openNamedIntegerArray();
  PdfStatus handleNamedIntegerArrayToken(const PdfToken& token);
  PdfStatus closeContainer(bool dictionary);
  PdfStatus handleToken(const PdfToken& token);
  bool shouldStreamNamedIntegerArray() const;

  PdfLexer& lexer_;
  PdfObjectArena& arena_;
  Frame frames_[32]{};
  uint8_t depth_ = 0;
  uint16_t rootIndex_ = PDF_INVALID_INDEX;
  int64_t firstInteger_ = 0;
  int64_t secondInteger_ = 0;
  uint8_t pendingIntegerStage_ = 0;
  bool complete_ = false;
  bool failed_ = false;
  bool skipDictionaryValue_ = false;
  bool skipUnusedPageResources_ = false;
  bool namedIntegerArrayActive_ = false;
  uint8_t skipDepth_ = 0;
  const PdfNamedIntegerArraySink* namedIntegerArraySink_ = nullptr;
  uint8_t* stringTokenBuffer_ = nullptr;
  size_t stringTokenCapacity_ = 0;
};
