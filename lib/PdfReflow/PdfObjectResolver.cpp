#include "PdfObjectResolver.h"

#include <cstring>
#include <limits>

#include "PdfCheckedMath.h"

namespace {

bool tokenEquals(const PdfToken& token, const char* expected) {
  const size_t length = std::strlen(expected);
  return token.length == length && std::memcmp(token.bytes, expected, length) == 0;
}

bool parseUnsigned(const PdfToken& token, uint64_t* value) {
  if (value == nullptr || token.kind != PdfTokenKind::Integer || token.length == 0) {
    return false;
  }
  uint64_t parsed = 0;
  for (uint32_t index = 0; index < token.length; ++index) {
    const char byte = token.bytes[index];
    if (byte < '0' || byte > '9') {
      return false;
    }
    const uint8_t digit = static_cast<uint8_t>(byte - '0');
    if (parsed > (std::numeric_limits<uint64_t>::max() - digit) / 10) {
      return false;
    }
    parsed = parsed * 10 + digit;
  }
  *value = parsed;
  return true;
}

}  // namespace

PdfObjectResolver::PdfObjectResolver(const PdfByteSource& source, const PdfXrefTable& xref, uint8_t* sourceBuffer,
                                     const size_t sourceBufferSize, PdfObjectArena& arena)
    : source_(source),
      xref_(xref),
      lexer_(source, sourceBuffer, sourceBufferSize),
      arena_(arena),
      parser_(lexer_, arena) {}

PdfStatus PdfObjectResolver::begin(const PdfObjectReference reference) {
  result_ = {};
  result_.reference = reference;
  PdfXrefEntry entry;
  PdfStatus status = xref_.find(reference.objectNumber, &entry);
  if (!status.ok()) {
    phase_ = Phase::Failed;
    return status;
  }
  if (entry.type == PdfXrefEntryType::Free || entry.generation != reference.generation) {
    phase_ = Phase::Failed;
    return PdfStatus::failure(PdfError::InvalidOffset, reference.objectNumber);
  }
  if (entry.type == PdfXrefEntryType::Compressed) {
    phase_ = Phase::Failed;
    return PdfStatus::failure(PdfError::Unsupported, reference.objectNumber);
  }
  if (entry.offset >= source_.size) {
    phase_ = Phase::Failed;
    return PdfStatus::failure(PdfError::InvalidOffset, entry.offset);
  }
  lexer_.reset(entry.offset);
  phase_ = Phase::ObjectNumber;
  return PdfStatus::success();
}

PdfStepResult PdfObjectResolver::step(PdfWorkBudget& budget) {
  if (phase_ == Phase::Done) {
    return PdfStepResult::completed();
  }
  if (phase_ == Phase::Idle || phase_ == Phase::Failed) {
    return PdfStepResult::failure(PdfStatus::failure(PdfError::Malformed, lexer_.position()));
  }

  auto fail = [this](const PdfStatus status) {
    phase_ = Phase::Failed;
    return PdfStepResult::failure(status);
  };
  auto finishStream = [this, &fail](const uint64_t offset) {
    if (!pdfCheckedRange(offset, result_.streamLength, source_.size)) {
      return fail(PdfStatus::failure(PdfError::InvalidOffset, offset));
    }
    result_.streamOffset = offset;
    result_.hasStream = true;
    phase_ = Phase::Done;
    return PdfStepResult::completed();
  };

  while (true) {
    if (phase_ == Phase::ParseValue) {
      const PdfStepResult parseResult = parser_.step(budget);
      if (!parseResult.complete()) {
        if (parseResult.failed()) {
          phase_ = Phase::Failed;
        }
        return parseResult;
      }
      result_.rootIndex = parser_.rootIndex();
      phase_ = Phase::AfterValue;
      continue;
    }

    if (phase_ == Phase::StreamFirstEol || phase_ == Phase::StreamSecondEol) {
      const PdfStepResult readResult = pdfStepReadExact(source_, eolRead_, budget);
      if (!readResult.complete()) {
        if (readResult.failed()) {
          phase_ = Phase::Failed;
        }
        return readResult;
      }
      if (phase_ == Phase::StreamFirstEol) {
        if (eolByte_ == '\n') {
          return finishStream(eolOffset_ + 1);
        }
        if (eolByte_ != '\r') {
          return fail(PdfStatus::failure(PdfError::Malformed, eolOffset_));
        }
        phase_ = Phase::StreamSecondEol;
        eolRead_ = {eolOffset_ + 1, &eolByte_, 1, 0};
        continue;
      }
      if (eolByte_ != '\n') {
        return fail(PdfStatus::failure(PdfError::Malformed, eolOffset_ + 1));
      }
      return finishStream(eolOffset_ + 2);
    }

    PdfToken token;
    const PdfStepResult tokenResult = lexer_.next(token, budget);
    if (!tokenResult.complete()) {
      if (tokenResult.failed()) {
        phase_ = Phase::Failed;
      }
      return tokenResult;
    }
    uint64_t value = 0;

    switch (phase_) {
      case Phase::ObjectNumber:
        if (!parseUnsigned(token, &value) || value != result_.reference.objectNumber) {
          return fail(PdfStatus::failure(PdfError::Malformed, lexer_.tokenOffset()));
        }
        phase_ = Phase::Generation;
        break;

      case Phase::Generation:
        if (!parseUnsigned(token, &value) || value != result_.reference.generation) {
          return fail(PdfStatus::failure(PdfError::Malformed, lexer_.tokenOffset()));
        }
        phase_ = Phase::ObjKeyword;
        break;

      case Phase::ObjKeyword:
        if (token.kind != PdfTokenKind::Keyword || !tokenEquals(token, "obj")) {
          return fail(PdfStatus::failure(PdfError::Malformed, lexer_.tokenOffset()));
        }
        parser_.begin();
        phase_ = Phase::ParseValue;
        break;

      case Phase::AfterValue:
        if (token.kind != PdfTokenKind::Keyword) {
          return fail(PdfStatus::failure(PdfError::Malformed, lexer_.tokenOffset()));
        }
        if (tokenEquals(token, "endobj")) {
          phase_ = Phase::Done;
          return PdfStepResult::completed();
        }
        if (!tokenEquals(token, "stream") || result_.rootIndex >= arena_.valueCount ||
            arena_.values[result_.rootIndex].kind != PdfValueKind::Dictionary) {
          return fail(PdfStatus::failure(PdfError::Malformed, lexer_.tokenOffset()));
        }
        {
          uint16_t lengthIndex = PDF_INVALID_INDEX;
          if (!pdfDictionaryFind(arena_, result_.rootIndex, "Length", &lengthIndex) ||
              lengthIndex >= arena_.valueCount) {
            return fail(PdfStatus::failure(PdfError::Malformed, lexer_.tokenOffset()));
          }
          const PdfValue& length = arena_.values[lengthIndex];
          if (length.kind == PdfValueKind::Reference) {
            return fail(PdfStatus::failure(PdfError::Unsupported, lexer_.tokenOffset()));
          }
          if (length.kind != PdfValueKind::Integer || length.integerValue < 0) {
            return fail(PdfStatus::failure(PdfError::Malformed, lexer_.tokenOffset()));
          }
          result_.streamLength = static_cast<uint64_t>(length.integerValue);
          eolOffset_ = lexer_.position();
          eolRead_ = {eolOffset_, &eolByte_, 1, 0};
          phase_ = Phase::StreamFirstEol;
        }
        break;

      default:
        return fail(PdfStatus::failure(PdfError::Malformed, lexer_.position()));
    }
  }
}
