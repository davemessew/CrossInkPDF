#include "PdfCMap.h"

#include <cstring>
#include <limits>

namespace {

constexpr uint8_t CMAP_SEQUENTIAL = 1U << 0;

bool tokenEquals(const PdfToken& token, const char* expected) {
  const size_t length = std::strlen(expected);
  return token.length == length && std::memcmp(token.bytes, expected, length) == 0;
}

bool parseCount(const PdfToken& token, uint32_t* count) {
  if (count == nullptr || token.kind != PdfTokenKind::Integer || token.length == 0) {
    return false;
  }
  uint32_t value = 0;
  for (uint32_t index = 0; index < token.length; ++index) {
    const char byte = token.bytes[index];
    if (byte < '0' || byte > '9') {
      return false;
    }
    const uint8_t digit = static_cast<uint8_t>(byte - '0');
    if (value > (std::numeric_limits<uint32_t>::max() - digit) / 10) {
      return false;
    }
    value = value * 10 + digit;
  }
  *count = value;
  return true;
}

bool decodeBigEndianCode(const PdfToken& token, uint32_t* code) {
  if (code == nullptr || token.kind != PdfTokenKind::HexString || token.length == 0 || token.length > 4) {
    return false;
  }
  uint32_t value = 0;
  for (uint32_t index = 0; index < token.length; ++index) {
    value = value << 8 | static_cast<uint8_t>(token.bytes[index]);
  }
  *code = value;
  return true;
}

}  // namespace

PdfCMap::PdfCMap(uint8_t* const sourceBuffer, const size_t sourceBufferSize, const PdfCMapWorkspace workspace)
    : workspace_(workspace), lexer_({}, sourceBuffer, sourceBufferSize) {}

bool PdfCMap::observingRecords() const {
  return workspace_.spill.write != nullptr && workspace_.spill.read == nullptr;
}

bool PdfCMap::codeSpaceOnly() const { return observingRecords() && workspace_.spill.capacity == 0; }

PdfStatus PdfCMap::setSourceAccess(const bool required) {
  if (workspace_.setSourceAccess != nullptr) {
    const PdfStatus status = workspace_.setSourceAccess(workspace_.sourceAccessContext, required);
    if (!status.ok()) {
      return status;
    }
  }
  sourceAccessRequired_ = required;
  return PdfStatus::success();
}

PdfStatus PdfCMap::begin(const PdfByteSource& source) {
  if (!source.valid() || workspace_.records == nullptr || workspace_.recordCapacity == 0) {
    return PdfStatus::failure(PdfError::InvalidArgument);
  }
  const bool hasSpillConfiguration = workspace_.spill.context != nullptr || workspace_.spill.capacity != 0 ||
                                     workspace_.spill.recordSize != 0 || workspace_.spill.read != nullptr ||
                                     workspace_.spill.write != nullptr;
  if (hasSpillConfiguration && !workspace_.spill.valid() && !observingRecords()) {
    return PdfStatus::failure(PdfError::InvalidArgument);
  }
  if ((workspace_.spill.valid() || observingRecords()) && workspace_.spill.recordSize != sizeof(PdfCMapRecord)) {
    return PdfStatus::failure(PdfError::InvalidArgument, workspace_.spill.recordSize);
  }
  const PdfStatus accessStatus = setSourceAccess(true);
  if (!accessStatus.ok()) {
    return accessStatus;
  }
  lexer_.setSource(source);
  for (CodeSpace& codeSpace : codeSpaces_) {
    codeSpace = {};
  }
  pendingRecord_ = {};
  failure_ = {};
  section_ = Section::Idle;
  pendingCount_ = 0;
  sectionRemaining_ = 0;
  mappingCount_ = 0;
  spillCount_ = 0;
  rangeArrayCode_ = 0;
  rangeArrayLast_ = 0;
  previousRecordLast_ = 0;
  previousRecordKey_ = 0;
  codeSpaceCount_ = 0;
  field_ = 0;
  hasPendingCount_ = false;
  recordsSorted_ = true;
  hasPreviousRecord_ = false;
  hasCachedRecord_ = false;
  return PdfStatus::success();
}

PdfStepResult PdfCMap::fail(const PdfStatus status) {
  failure_ = status;
  section_ = Section::Failed;
  return PdfStepResult::failure(status);
}

PdfStepResult PdfCMap::step(PdfWorkBudget& budget) {
  if (section_ == Section::Done) {
    return PdfStepResult::completed();
  }
  if (section_ == Section::Failed) {
    return PdfStepResult::failure(failure_);
  }
  while (budget.operationsRemaining != 0 && budget.bytesRemaining != 0 && !budget.stopRequested()) {
    PdfToken token;
    const PdfStepResult tokenResult = lexer_.next(token, budget);
    if (tokenResult.yielded()) {
      return tokenResult;
    }
    if (tokenResult.failed()) {
      return fail(tokenResult.status);
    }
    if (token.kind == PdfTokenKind::End) {
      if (section_ != Section::Idle || mappingCount_ == 0 || codeSpaceCount_ == 0) {
        return fail(PdfStatus::failure(PdfError::Malformed, lexer_.position()));
      }
      section_ = Section::Done;
      return PdfStepResult::completed();
    }
    const PdfStatus status = handleToken(token);
    if (!status.ok()) {
      return fail(status);
    }
    if (section_ == Section::Done) {
      return PdfStepResult::completed();
    }
  }
  return budget.cancelRequested()
             ? fail(PdfStatus::failure(PdfError::Cancelled, lexer_.position()))
             : PdfStepResult::paused();
}

PdfStatus PdfCMap::handleToken(const PdfToken& token) {
  switch (section_) {
    case Section::CodeSpace:
      return handleCodeSpace(token);
    case Section::BfChar:
      return handleBfChar(token);
    case Section::BfRange:
      return handleBfRange(token);
    case Section::BfRangeArray:
      return handleBfRangeArray(token);
    case Section::AwaitSectionEnd: {
      const char* expected = field_ == 1 ? "endcodespacerange" : (field_ == 2 ? "endbfchar" : "endbfrange");
      if (token.kind != PdfTokenKind::Keyword || !tokenEquals(token, expected)) {
        return PdfStatus::failure(PdfError::Malformed, lexer_.tokenOffset());
      }
      section_ = Section::Idle;
      field_ = 0;
      return PdfStatus::success();
    }
    case Section::Idle:
      break;
    case Section::Done:
    case Section::Failed:
      return PdfStatus::failure(PdfError::InvalidArgument, lexer_.tokenOffset());
  }

  if (token.kind == PdfTokenKind::Integer) {
    uint32_t count = 0;
    if (parseCount(token, &count)) {
      pendingCount_ = count;
      hasPendingCount_ = true;
    }
    return PdfStatus::success();
  }
  if (codeSpaceOnly() && token.kind == PdfTokenKind::Keyword && tokenEquals(token, "endcmap")) {
    if (mappingCount_ == 0 || codeSpaceCount_ == 0) {
      return PdfStatus::failure(PdfError::Malformed, lexer_.tokenOffset());
    }
    section_ = Section::Done;
    return PdfStatus::success();
  }
  if (token.kind != PdfTokenKind::Keyword || !hasPendingCount_) {
    return PdfStatus::success();
  }
  if (pendingCount_ > PdfLimits::MaxCMapRanges) {
    section_ = Section::Done;
    return PdfStatus::success();
  }
  sectionRemaining_ = pendingCount_;
  hasPendingCount_ = false;
  field_ = 0;
  if (tokenEquals(token, "begincodespacerange")) {
    section_ = Section::CodeSpace;
    return PdfStatus::success();
  }
  if (tokenEquals(token, "beginbfchar")) {
    section_ = Section::BfChar;
    return PdfStatus::success();
  }
  if (tokenEquals(token, "beginbfrange")) {
    section_ = Section::BfRange;
    return PdfStatus::success();
  }
  return PdfStatus::success();
}

PdfStatus PdfCMap::handleCodeSpace(const PdfToken& token) {
  if (sectionRemaining_ == 0) {
    section_ = Section::AwaitSectionEnd;
    field_ = 1;
    return handleToken(token);
  }
  uint32_t code = 0;
  if (!decodeBigEndianCode(token, &code)) {
    return PdfStatus::failure(PdfError::Malformed, lexer_.tokenOffset());
  }
  if (field_ == 0) {
    pendingRecord_ = {};
    pendingRecord_.sourceFirst = code;
    pendingRecord_.sourceLength = static_cast<uint8_t>(token.length);
    field_ = 1;
    return PdfStatus::success();
  }
  if (token.length != pendingRecord_.sourceLength || code < pendingRecord_.sourceFirst) {
    return PdfStatus::failure(PdfError::Malformed, lexer_.tokenOffset());
  }
  if (codeSpaceCount_ < static_cast<uint8_t>(sizeof(codeSpaces_) / sizeof(codeSpaces_[0]))) {
    codeSpaces_[codeSpaceCount_++] = {pendingRecord_.sourceFirst, code, pendingRecord_.sourceLength};
  }
  --sectionRemaining_;
  field_ = 0;
  return PdfStatus::success();
}

PdfStatus PdfCMap::handleBfChar(const PdfToken& token) {
  if (sectionRemaining_ == 0) {
    section_ = Section::AwaitSectionEnd;
    field_ = 2;
    return handleToken(token);
  }
  if (field_ == 0) {
    uint32_t code = 0;
    if (!decodeBigEndianCode(token, &code)) {
      return PdfStatus::failure(PdfError::Malformed, lexer_.tokenOffset());
    }
    pendingRecord_ = {};
    pendingRecord_.sourceFirst = code;
    pendingRecord_.sourceLength = static_cast<uint8_t>(token.length);
    field_ = 1;
    return PdfStatus::success();
  }
  if (token.kind != PdfTokenKind::HexString) {
    return PdfStatus::failure(PdfError::Malformed, lexer_.tokenOffset());
  }
  const PdfStatus status = addExact(pendingRecord_.sourceFirst, pendingRecord_.sourceLength,
                                    reinterpret_cast<const uint8_t*>(token.bytes), token.length);
  if (!status.ok()) {
    return status;
  }
  --sectionRemaining_;
  field_ = 0;
  return PdfStatus::success();
}

PdfStatus PdfCMap::handleBfRange(const PdfToken& token) {
  if (sectionRemaining_ == 0) {
    section_ = Section::AwaitSectionEnd;
    field_ = 3;
    return handleToken(token);
  }
  if (field_ < 2) {
    uint32_t code = 0;
    if (!decodeBigEndianCode(token, &code)) {
      return PdfStatus::failure(PdfError::Malformed, lexer_.tokenOffset());
    }
    if (field_ == 0) {
      pendingRecord_ = {};
      pendingRecord_.sourceFirst = code;
      pendingRecord_.sourceLength = static_cast<uint8_t>(token.length);
      field_ = 1;
      return PdfStatus::success();
    }
    if (token.length != pendingRecord_.sourceLength || code < pendingRecord_.sourceFirst) {
      return PdfStatus::failure(PdfError::Malformed, lexer_.tokenOffset());
    }
    pendingRecord_.sourceLast = code;
    field_ = 2;
    return PdfStatus::success();
  }
  if (token.kind == PdfTokenKind::ArrayBegin) {
    rangeArrayCode_ = pendingRecord_.sourceFirst;
    rangeArrayLast_ = pendingRecord_.sourceLast;
    section_ = Section::BfRangeArray;
    return PdfStatus::success();
  }
  if (token.kind != PdfTokenKind::HexString) {
    return PdfStatus::failure(PdfError::Malformed, lexer_.tokenOffset());
  }
  const PdfStatus status =
      addSequential(pendingRecord_.sourceFirst, pendingRecord_.sourceLast, pendingRecord_.sourceLength,
                    reinterpret_cast<const uint8_t*>(token.bytes), token.length);
  if (!status.ok()) {
    return status;
  }
  --sectionRemaining_;
  field_ = 0;
  return PdfStatus::success();
}

PdfStatus PdfCMap::handleBfRangeArray(const PdfToken& token) {
  if (rangeArrayCode_ <= rangeArrayLast_) {
    if (token.kind != PdfTokenKind::HexString) {
      return PdfStatus::failure(PdfError::Malformed, lexer_.tokenOffset());
    }
    const PdfStatus status = addExact(static_cast<uint32_t>(rangeArrayCode_), pendingRecord_.sourceLength,
                                      reinterpret_cast<const uint8_t*>(token.bytes), token.length);
    if (!status.ok()) {
      return status;
    }
    ++rangeArrayCode_;
    return PdfStatus::success();
  }
  if (token.kind != PdfTokenKind::ArrayEnd) {
    return PdfStatus::failure(PdfError::Malformed, lexer_.tokenOffset());
  }
  --sectionRemaining_;
  field_ = 0;
  section_ = Section::BfRange;
  return PdfStatus::success();
}

PdfStatus PdfCMap::addRecord(const PdfCMapRecord& record) {
  if (mappingCount_ >= PdfLimits::MaxCMapRanges) {
    return PdfStatus::success();
  }
  const uint64_t key = static_cast<uint64_t>(record.sourceLength) << 32 | record.sourceFirst;
  const bool ordered = !hasPreviousRecord_ ||
                       (key >= previousRecordKey_ &&
                        (record.sourceLength != static_cast<uint8_t>(previousRecordKey_ >> 32) ||
                         record.sourceFirst > previousRecordLast_));
  if (!observingRecords() && (!ordered || !recordsSorted_) && mappingCount_ >= workspace_.recordCapacity) {
    // An unsorted SD-backed map would require a linear record scan for every glyph.
    return PdfStatus::success();
  }
  if (!ordered) {
    recordsSorted_ = false;
  }
  if (!codeSpaceOnly()) {
    if (observingRecords()) {
      if (mappingCount_ >= workspace_.spill.capacity) {
        return PdfStatus::success();
      }
      const PdfStatus status =
          workspace_.spill.write(workspace_.spill.context, mappingCount_, &record, sizeof(record));
      if (!status.ok()) {
        return status.error == PdfError::LimitExceeded ? PdfStatus::success() : status;
      }
    } else if (mappingCount_ < workspace_.recordCapacity) {
      workspace_.records[mappingCount_] = record;
    } else {
      if (!workspace_.spill.valid() || spillCount_ >= workspace_.spill.capacity) {
        return PdfStatus::success();
      }
      const PdfStatus status = pdfWriteRecord(workspace_.spill, spillCount_, &record);
      if (!status.ok()) {
        return status.error == PdfError::LimitExceeded ? PdfStatus::success() : status;
      }
      ++spillCount_;
    }
  }
  previousRecordKey_ = key;
  previousRecordLast_ = record.sourceLast;
  hasPreviousRecord_ = true;
  ++mappingCount_;
  return PdfStatus::success();
}

PdfStatus PdfCMap::addExact(const uint32_t sourceCode, const uint8_t sourceLength, const uint8_t* const destination,
                            const size_t destinationLength) {
  PdfCMapRecord record{};
  record.sourceFirst = sourceCode;
  record.sourceLast = sourceCode;
  record.sourceLength = sourceLength;
  PdfUtf8Value value;
  const PdfStatus decodeStatus = pdfDecodeUtf16Be(destination, destinationLength, &value);
  if (!decodeStatus.ok()) {
    return decodeStatus;
  }
  record.utf8Length = value.length;
  std::memcpy(record.utf8, value.bytes, value.length);
  return addRecord(record);
}

PdfStatus PdfCMap::addSequential(const uint32_t first, const uint32_t last, const uint8_t sourceLength,
                                 const uint8_t* const destination, const size_t destinationLength) {
  if (destination != nullptr && destinationLength == 2) {
    const uint16_t firstCodeUnit = static_cast<uint16_t>(destination[0]) << 8U | destination[1];
    const uint64_t lastCodeUnit = static_cast<uint64_t>(firstCodeUnit) + last - first;
    if (firstCodeUnit >= 0xD800U && lastCodeUnit <= 0xDFFFU) {
      return PdfStatus::success();
    }
  }
  uint32_t scalar = 0;
  const PdfStatus decodeStatus = pdfDecodeSingleUtf16BeScalar(destination, destinationLength, &scalar);
  const uint64_t count = static_cast<uint64_t>(last) - first + 1;
  const uint32_t lastScalar = count - 1 > std::numeric_limits<uint32_t>::max() - scalar
                                  ? std::numeric_limits<uint32_t>::max()
                                  : scalar + static_cast<uint32_t>(count - 1);
  if (!decodeStatus.ok() || scalar > std::numeric_limits<uint32_t>::max() - (count - 1) ||
      !pdfIsUnicodeScalar(lastScalar) ||
      (scalar < 0xD800 && lastScalar >= 0xD800)) {
    return PdfStatus::failure(PdfError::Malformed, first);
  }
  PdfCMapRecord record{};
  record.sourceFirst = first;
  record.sourceLast = last;
  record.destinationFirst = scalar;
  record.sourceLength = sourceLength;
  record.flags = CMAP_SEQUENTIAL;
  return addRecord(record);
}

PdfStatus PdfCMap::readRecord(const uint32_t ordinal, PdfCMapRecord* const record) {
  if (record == nullptr || ordinal >= mappingCount_) {
    return PdfStatus::failure(PdfError::InvalidOffset, ordinal);
  }
  if (ordinal < workspace_.recordCapacity) {
    *record = workspace_.records[ordinal];
    return PdfStatus::success();
  }
  const PdfStatus accessStatus = setSourceAccess(false);
  if (!accessStatus.ok()) {
    return accessStatus;
  }
  return pdfReadRecord(workspace_.spill, ordinal - workspace_.recordCapacity, record);
}

PdfStatus PdfCMap::decodeCode(const uint8_t* const source, const size_t sourceLength, uint32_t* const code,
                              uint8_t* const codeLength) const {
  if (source == nullptr || code == nullptr || codeLength == nullptr) {
    return PdfStatus::failure(PdfError::InvalidArgument);
  }
  for (const CodeSpace& range : codeSpaces_) {
    if (range.length == 0 || range.length > sourceLength) {
      continue;
    }
    uint32_t value = 0;
    for (uint8_t index = 0; index < range.length; ++index) {
      value = value << 8 | source[index];
    }
    if (value >= range.first && value <= range.last) {
      *code = value;
      *codeLength = range.length;
      return PdfStatus::success();
    }
  }
  return PdfStatus::failure(PdfError::UnsupportedEncoding);
}

PdfStatus PdfCMap::applyRecord(const PdfCMapRecord& record, const uint32_t code, const uint8_t codeLength,
                               PdfCMapLookup* const result) {
  if (result == nullptr || record.sourceLength != codeLength || code < record.sourceFirst || code > record.sourceLast) {
    return PdfStatus::failure(PdfError::UnsupportedEncoding, code);
  }
  result->sourceCode = code;
  result->sourceLength = codeLength;
  result->unicode = {};
  if ((record.flags & CMAP_SEQUENTIAL) != 0) {
    size_t length = 0;
    const PdfStatus status = pdfAppendUtf8Scalar(record.destinationFirst + (code - record.sourceFirst),
                                                 result->unicode.bytes, sizeof(result->unicode.bytes), &length);
    if (!status.ok()) {
      return status;
    }
    result->unicode.length = static_cast<uint8_t>(length);
  } else {
    result->unicode.length = record.utf8Length;
    std::memcpy(result->unicode.bytes, record.utf8, record.utf8Length);
  }
  cachedRecord_ = record;
  hasCachedRecord_ = true;
  return PdfStatus::success();
}

PdfStatus PdfCMap::lookup(const uint8_t* const source, const size_t sourceLength, PdfCMapLookup* const result) {
  if (result == nullptr || section_ != Section::Done || observingRecords()) {
    return PdfStatus::failure(PdfError::InvalidArgument);
  }
  uint32_t code = 0;
  uint8_t codeLength = 0;
  const PdfStatus codeStatus = decodeCode(source, sourceLength, &code, &codeLength);
  if (!codeStatus.ok()) {
    return codeStatus;
  }
  if (hasCachedRecord_ && cachedRecord_.sourceLength == codeLength && code >= cachedRecord_.sourceFirst &&
      code <= cachedRecord_.sourceLast) {
    return applyRecord(cachedRecord_, code, codeLength, result);
  }
  if (recordsSorted_) {
    const uint64_t target = static_cast<uint64_t>(codeLength) << 32 | code;
    uint32_t first = 0;
    uint32_t last = mappingCount_;
    while (first < last) {
      const uint32_t middle = first + (last - first) / 2;
      PdfCMapRecord record;
      const PdfStatus status = readRecord(middle, &record);
      if (!status.ok()) {
        return status;
      }
      const uint64_t key = static_cast<uint64_t>(record.sourceLength) << 32 | record.sourceFirst;
      if (key <= target) {
        first = middle + 1;
      } else {
        last = middle;
      }
    }
    if (first != 0) {
      PdfCMapRecord record;
      const PdfStatus status = readRecord(first - 1, &record);
      if (!status.ok()) {
        return status;
      }
      return applyRecord(record, code, codeLength, result);
    }
    return PdfStatus::failure(PdfError::UnsupportedEncoding, code);
  }
  for (uint32_t ordinal = 0; ordinal < mappingCount_; ++ordinal) {
    PdfCMapRecord record;
    const PdfStatus readStatus = readRecord(ordinal, &record);
    if (!readStatus.ok()) {
      return readStatus;
    }
    if (record.sourceLength != codeLength || code < record.sourceFirst || code > record.sourceLast) {
      continue;
    }
    return applyRecord(record, code, codeLength, result);
  }
  return PdfStatus::failure(PdfError::UnsupportedEncoding, code);
}

PdfStatus PdfCMap::copyCodeSpaces(PdfCMapCodeSpace* const destination, const size_t capacity,
                                  uint8_t* const count) const {
  if (count == nullptr || section_ != Section::Done || (destination == nullptr && codeSpaceCount_ != 0) ||
      capacity < codeSpaceCount_) {
    return PdfStatus::failure(PdfError::InvalidArgument, codeSpaceCount_);
  }
  for (uint8_t index = 0; index < codeSpaceCount_; ++index) {
    destination[index] = {codeSpaces_[index].first, codeSpaces_[index].last, codeSpaces_[index].length};
  }
  *count = codeSpaceCount_;
  return PdfStatus::success();
}
