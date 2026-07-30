#include "PdfObjectParser.h"

#include <climits>
#include <cstring>

namespace {

bool parseInteger(const PdfToken& token, int64_t* value) {
  if (value == nullptr || token.kind != PdfTokenKind::Integer || token.length == 0) {
    return false;
  }
  size_t position = 0;
  bool negative = false;
  if (token.bytes[position] == '+' || token.bytes[position] == '-') {
    negative = token.bytes[position] == '-';
    ++position;
  }
  if (position == token.length) {
    return false;
  }
  uint64_t magnitude = 0;
  const uint64_t maximum = negative ? static_cast<uint64_t>(INT64_MAX) + 1ULL : static_cast<uint64_t>(INT64_MAX);
  for (; position < token.length; ++position) {
    const char byte = token.bytes[position];
    if (byte < '0' || byte > '9') {
      return false;
    }
    const uint8_t digit = static_cast<uint8_t>(byte - '0');
    if (magnitude > (maximum - digit) / 10) {
      return false;
    }
    magnitude = magnitude * 10 + digit;
  }
  if (negative) {
    *value = magnitude == static_cast<uint64_t>(INT64_MAX) + 1ULL ? INT64_MIN : -static_cast<int64_t>(magnitude);
  } else {
    *value = static_cast<int64_t>(magnitude);
  }
  return true;
}

bool parseFixed(const PdfToken& token, int32_t* value) {
  if (value == nullptr || token.kind != PdfTokenKind::Real || token.length == 0) {
    return false;
  }
  size_t position = 0;
  bool negative = false;
  if (token.bytes[position] == '+' || token.bytes[position] == '-') {
    negative = token.bytes[position] == '-';
    ++position;
  }
  int64_t whole = 0;
  int64_t fraction = 0;
  int64_t scale = 1;
  bool decimal = false;
  bool digitSeen = false;
  for (; position < token.length; ++position) {
    const char byte = token.bytes[position];
    if (byte == '.' && !decimal) {
      decimal = true;
      continue;
    }
    if (byte < '0' || byte > '9') {
      return false;
    }
    digitSeen = true;
    const uint8_t digit = static_cast<uint8_t>(byte - '0');
    if (!decimal) {
      if (whole > 32768) {
        return false;
      }
      whole = whole * 10 + digit;
    } else if (scale < 1000000) {
      fraction = fraction * 10 + digit;
      scale *= 10;
    }
  }
  if (!digitSeen) {
    return false;
  }
  int64_t raw = whole * 65536 + (fraction * 65536 + scale / 2) / scale;
  if (negative) {
    raw = -raw;
  }
  if (raw < INT32_MIN || raw > INT32_MAX) {
    return false;
  }
  *value = static_cast<int32_t>(raw);
  return true;
}

bool tokenEquals(const PdfToken& token, const char* expected) {
  const size_t length = std::strlen(expected);
  return token.length == length && std::memcmp(token.bytes, expected, length) == 0;
}

}  // namespace

void PdfObjectArena::reset() {
  valueCount = 0;
  dictionaryCount = 0;
  arrayCount = 0;
  textLength = 0;
}

bool pdfDictionaryFind(const PdfObjectArena& arena, const uint16_t dictionaryIndex, const char* key,
                       uint16_t* valueIndex) {
  if (key == nullptr || valueIndex == nullptr || dictionaryIndex >= arena.valueCount || arena.values == nullptr ||
      arena.dictionaryEntries == nullptr || arena.text == nullptr) {
    return false;
  }
  const PdfValue& dictionary = arena.values[dictionaryIndex];
  if (dictionary.kind != PdfValueKind::Dictionary) {
    return false;
  }
  const size_t keyLength = std::strlen(key);
  if (keyLength > UINT16_MAX) {
    return false;
  }
  uint16_t entryIndex = dictionary.firstLink;
  for (uint16_t visited = 0; visited < dictionary.count; ++visited) {
    if (entryIndex >= arena.dictionaryCount) {
      return false;
    }
    const PdfDictionaryEntry& entry = arena.dictionaryEntries[entryIndex];
    if (entry.keyLength == keyLength && static_cast<uint32_t>(entry.keyOffset) + entry.keyLength <= arena.textLength &&
        std::memcmp(arena.text + entry.keyOffset, key, keyLength) == 0) {
      *valueIndex = entry.valueIndex;
      return entry.valueIndex < arena.valueCount;
    }
    entryIndex = entry.next;
  }
  return false;
}

bool pdfArrayAt(const PdfObjectArena& arena, const uint16_t arrayIndex, const uint16_t ordinal, uint16_t* valueIndex) {
  if (valueIndex == nullptr || arrayIndex >= arena.valueCount || arena.values == nullptr ||
      arena.arrayItems == nullptr) {
    return false;
  }
  const PdfValue& array = arena.values[arrayIndex];
  if (array.kind != PdfValueKind::Array || ordinal >= array.count) {
    return false;
  }
  uint16_t itemIndex = array.firstLink;
  for (uint16_t current = 0; current <= ordinal; ++current) {
    if (itemIndex >= arena.arrayCount) {
      return false;
    }
    const PdfArrayItem& item = arena.arrayItems[itemIndex];
    if (current == ordinal) {
      *valueIndex = item.valueIndex;
      return item.valueIndex < arena.valueCount;
    }
    itemIndex = item.next;
  }
  return false;
}

bool pdfTextEquals(const PdfObjectArena& arena, const PdfValue& value, const char* expected) {
  if (expected == nullptr || arena.text == nullptr ||
      (value.kind != PdfValueKind::Name && value.kind != PdfValueKind::String)) {
    return false;
  }
  const size_t expectedLength = std::strlen(expected);
  return value.textLength == expectedLength &&
         static_cast<uint32_t>(value.textOffset) + value.textLength <= arena.textLength &&
         std::memcmp(arena.text + value.textOffset, expected, expectedLength) == 0;
}

PdfObjectParser::PdfObjectParser(PdfLexer& lexer, PdfObjectArena& arena) : lexer_(lexer), arena_(arena) {}

void PdfObjectParser::begin() {
  arena_.reset();
  depth_ = 0;
  rootIndex_ = PDF_INVALID_INDEX;
  pendingIntegerStage_ = 0;
  complete_ = false;
  failed_ = false;
}

PdfStatus PdfObjectParser::addValue(const PdfValue& value, uint16_t* valueIndex) {
  if (valueIndex == nullptr || arena_.values == nullptr || arena_.valueCount >= arena_.valueCapacity) {
    return PdfStatus::failure(PdfError::LimitExceeded, lexer_.tokenOffset());
  }
  *valueIndex = arena_.valueCount;
  arena_.values[arena_.valueCount++] = value;
  return PdfStatus::success();
}

PdfStatus PdfObjectParser::copyText(const PdfToken& token, uint16_t* offset, uint16_t* length) {
  if (offset == nullptr || length == nullptr || arena_.text == nullptr || token.length > UINT16_MAX ||
      token.length > static_cast<uint32_t>(arena_.textCapacity - arena_.textLength)) {
    return PdfStatus::failure(PdfError::LimitExceeded, lexer_.tokenOffset());
  }
  *offset = arena_.textLength;
  *length = static_cast<uint16_t>(token.length);
  if (token.length != 0) {
    std::memcpy(arena_.text + arena_.textLength, token.bytes, token.length);
  }
  arena_.textLength = static_cast<uint16_t>(arena_.textLength + token.length);
  return PdfStatus::success();
}

PdfStatus PdfObjectParser::attachValue(const uint16_t valueIndex) {
  if (valueIndex >= arena_.valueCount) {
    return PdfStatus::failure(PdfError::Malformed, lexer_.tokenOffset());
  }
  if (depth_ == 0) {
    if (rootIndex_ != PDF_INVALID_INDEX) {
      return PdfStatus::failure(PdfError::Malformed, lexer_.tokenOffset());
    }
    rootIndex_ = valueIndex;
    return PdfStatus::success();
  }

  Frame& frame = frames_[depth_ - 1];
  PdfValue& container = arena_.values[frame.containerIndex];
  if (frame.dictionary) {
    if (frame.expectingKey || arena_.dictionaryEntries == nullptr ||
        arena_.dictionaryCount >= arena_.dictionaryCapacity) {
      return PdfStatus::failure(frame.expectingKey ? PdfError::Malformed : PdfError::LimitExceeded,
                                lexer_.tokenOffset());
    }
    const uint16_t entryIndex = arena_.dictionaryCount++;
    PdfDictionaryEntry& entry = arena_.dictionaryEntries[entryIndex];
    entry = {PDF_INVALID_INDEX, valueIndex, frame.pendingKeyOffset, frame.pendingKeyLength};
    if (container.firstLink == PDF_INVALID_INDEX) {
      container.firstLink = entryIndex;
    } else if (container.lastLink < arena_.dictionaryCount) {
      arena_.dictionaryEntries[container.lastLink].next = entryIndex;
    } else {
      return PdfStatus::failure(PdfError::Malformed, lexer_.tokenOffset());
    }
    container.lastLink = entryIndex;
    ++container.count;
    frame.expectingKey = true;
    return PdfStatus::success();
  }

  if (arena_.arrayItems == nullptr || arena_.arrayCount >= arena_.arrayCapacity) {
    return PdfStatus::failure(PdfError::LimitExceeded, lexer_.tokenOffset());
  }
  const uint16_t itemIndex = arena_.arrayCount++;
  PdfArrayItem& item = arena_.arrayItems[itemIndex];
  item = {PDF_INVALID_INDEX, valueIndex};
  if (container.firstLink == PDF_INVALID_INDEX) {
    container.firstLink = itemIndex;
  } else if (container.lastLink < arena_.arrayCount) {
    arena_.arrayItems[container.lastLink].next = itemIndex;
  } else {
    return PdfStatus::failure(PdfError::Malformed, lexer_.tokenOffset());
  }
  container.lastLink = itemIndex;
  ++container.count;
  return PdfStatus::success();
}

PdfStatus PdfObjectParser::emitInteger(const int64_t value) {
  PdfValue parsed;
  parsed.kind = PdfValueKind::Integer;
  parsed.integerValue = value;
  uint16_t valueIndex = PDF_INVALID_INDEX;
  PdfStatus status = addValue(parsed, &valueIndex);
  if (status.ok()) {
    status = attachValue(valueIndex);
  }
  if (status.ok() && depth_ == 0) {
    complete_ = true;
  }
  return status;
}

PdfStatus PdfObjectParser::emitReference(const int64_t objectNumber, const int64_t generation) {
  if (objectNumber < 0 || objectNumber > UINT32_MAX || generation < 0 || generation > UINT16_MAX) {
    return PdfStatus::failure(PdfError::Malformed, lexer_.tokenOffset());
  }
  PdfValue parsed;
  parsed.kind = PdfValueKind::Reference;
  parsed.objectNumber = static_cast<uint32_t>(objectNumber);
  parsed.generation = static_cast<uint16_t>(generation);
  uint16_t valueIndex = PDF_INVALID_INDEX;
  PdfStatus status = addValue(parsed, &valueIndex);
  if (status.ok()) {
    status = attachValue(valueIndex);
  }
  if (status.ok() && depth_ == 0) {
    complete_ = true;
  }
  return status;
}

PdfStatus PdfObjectParser::emitTokenValue(const PdfToken& token) {
  PdfValue value;
  switch (token.kind) {
    case PdfTokenKind::Name:
      value.kind = PdfValueKind::Name;
      break;
    case PdfTokenKind::String:
    case PdfTokenKind::HexString:
      value.kind = PdfValueKind::String;
      break;
    case PdfTokenKind::Real:
      value.kind = PdfValueKind::Real;
      if (!parseFixed(token, &value.fixedValue)) {
        return PdfStatus::failure(PdfError::Malformed, lexer_.tokenOffset());
      }
      break;
    case PdfTokenKind::Keyword:
      if (tokenEquals(token, "null")) {
        value.kind = PdfValueKind::Null;
      } else if (tokenEquals(token, "true") || tokenEquals(token, "false")) {
        value.kind = PdfValueKind::Boolean;
        value.booleanValue = tokenEquals(token, "true");
      } else {
        return PdfStatus::failure(PdfError::Malformed, lexer_.tokenOffset());
      }
      break;
    default:
      return PdfStatus::failure(PdfError::Malformed, lexer_.tokenOffset());
  }

  if (value.kind == PdfValueKind::Name || value.kind == PdfValueKind::String) {
    const PdfStatus textStatus = copyText(token, &value.textOffset, &value.textLength);
    if (!textStatus.ok()) {
      return textStatus;
    }
  }
  uint16_t valueIndex = PDF_INVALID_INDEX;
  PdfStatus status = addValue(value, &valueIndex);
  if (status.ok()) {
    status = attachValue(valueIndex);
  }
  if (status.ok() && depth_ == 0) {
    complete_ = true;
  }
  return status;
}

PdfStatus PdfObjectParser::openContainer(const bool dictionary) {
  if (depth_ >= 32) {
    return PdfStatus::failure(PdfError::LimitExceeded, lexer_.tokenOffset());
  }
  PdfValue value;
  value.kind = dictionary ? PdfValueKind::Dictionary : PdfValueKind::Array;
  uint16_t valueIndex = PDF_INVALID_INDEX;
  PdfStatus status = addValue(value, &valueIndex);
  if (!status.ok()) {
    return status;
  }
  status = attachValue(valueIndex);
  if (!status.ok()) {
    return status;
  }
  frames_[depth_++] = {valueIndex, 0, 0, dictionary, dictionary};
  return PdfStatus::success();
}

PdfStatus PdfObjectParser::closeContainer(const bool dictionary) {
  if (depth_ == 0 || frames_[depth_ - 1].dictionary != dictionary ||
      (dictionary && !frames_[depth_ - 1].expectingKey)) {
    return PdfStatus::failure(PdfError::Malformed, lexer_.tokenOffset());
  }
  --depth_;
  if (depth_ == 0) {
    complete_ = true;
  }
  return PdfStatus::success();
}

PdfStatus PdfObjectParser::handleToken(const PdfToken& token) {
  if (token.kind == PdfTokenKind::DictionaryBegin) {
    return openContainer(true);
  }
  if (token.kind == PdfTokenKind::ArrayBegin) {
    return openContainer(false);
  }
  if (token.kind == PdfTokenKind::DictionaryEnd) {
    return closeContainer(true);
  }
  if (token.kind == PdfTokenKind::ArrayEnd) {
    return closeContainer(false);
  }
  if (depth_ != 0 && frames_[depth_ - 1].dictionary && frames_[depth_ - 1].expectingKey) {
    if (token.kind != PdfTokenKind::Name) {
      return PdfStatus::failure(PdfError::Malformed, lexer_.tokenOffset());
    }
    Frame& frame = frames_[depth_ - 1];
    const PdfStatus status = copyText(token, &frame.pendingKeyOffset, &frame.pendingKeyLength);
    if (status.ok()) {
      frame.expectingKey = false;
    }
    return status;
  }
  return emitTokenValue(token);
}

PdfStepResult PdfObjectParser::step(PdfWorkBudget& budget) {
  if (failed_) {
    return PdfStepResult::failure(PdfStatus::failure(PdfError::Malformed, lexer_.position()));
  }
  if (complete_) {
    return PdfStepResult::completed();
  }

  while (true) {
    PdfToken token;
    const PdfStepResult lexResult = lexer_.next(token, budget);
    if (!lexResult.complete()) {
      if (lexResult.failed()) {
        failed_ = true;
      }
      return lexResult;
    }

    PdfStatus status;
    if (pendingIntegerStage_ == 1) {
      if (token.kind == PdfTokenKind::Integer) {
        if (!parseInteger(token, &secondInteger_)) {
          failed_ = true;
          return PdfStepResult::failure(PdfStatus::failure(PdfError::Malformed, lexer_.tokenOffset()));
        }
        pendingIntegerStage_ = 2;
        continue;
      }
      status = emitInteger(firstInteger_);
      pendingIntegerStage_ = 0;
      if (!status.ok()) {
        failed_ = true;
        return PdfStepResult::failure(status);
      }
      if (complete_) {
        if (!lexer_.unread(token)) {
          failed_ = true;
          return PdfStepResult::failure(PdfStatus::failure(PdfError::Malformed, lexer_.tokenOffset()));
        }
        return PdfStepResult::completed();
      }
    } else if (pendingIntegerStage_ == 2) {
      if (token.kind == PdfTokenKind::Keyword && tokenEquals(token, "R")) {
        status = emitReference(firstInteger_, secondInteger_);
        pendingIntegerStage_ = 0;
        if (!status.ok()) {
          failed_ = true;
          return PdfStepResult::failure(status);
        }
        if (complete_) {
          return PdfStepResult::completed();
        }
        continue;
      }
      status = emitInteger(firstInteger_);
      if (status.ok()) {
        status = emitInteger(secondInteger_);
      }
      pendingIntegerStage_ = 0;
      if (!status.ok()) {
        failed_ = true;
        return PdfStepResult::failure(status);
      }
      if (complete_) {
        failed_ = true;
        return PdfStepResult::failure(PdfStatus::failure(PdfError::Malformed, lexer_.tokenOffset()));
      }
    }

    if (token.kind == PdfTokenKind::Integer) {
      if (!parseInteger(token, &firstInteger_)) {
        failed_ = true;
        return PdfStepResult::failure(PdfStatus::failure(PdfError::Malformed, lexer_.tokenOffset()));
      }
      pendingIntegerStage_ = 1;
      continue;
    }

    if (token.kind == PdfTokenKind::End) {
      failed_ = true;
      return PdfStepResult::failure(PdfStatus::failure(PdfError::UnexpectedEof, lexer_.position()));
    }

    status = handleToken(token);
    if (!status.ok()) {
      failed_ = true;
      return PdfStepResult::failure(status);
    }
    if (complete_) {
      return PdfStepResult::completed();
    }
  }
}
