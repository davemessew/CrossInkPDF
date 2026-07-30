#include "PdfXref.h"

#include <algorithm>
#include <cstring>
#include <limits>

#include "PdfCheckedMath.h"
#include "PdfLimits.h"

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

bool findLastStartXref(const uint8_t* bytes, const size_t length, uint64_t* offset) {
  constexpr char MARKER[] = "startxref";
  constexpr char EOF_MARKER[] = "%%EOF\n";
  if (bytes == nullptr || offset == nullptr || length < sizeof(EOF_MARKER) - 1 ||
      std::memcmp(bytes + length - (sizeof(EOF_MARKER) - 1), EOF_MARKER, sizeof(EOF_MARKER) - 1) != 0) {
    return false;
  }
  for (size_t candidate = length - (sizeof(EOF_MARKER) - 1); candidate-- > 0;) {
    if (candidate + sizeof(MARKER) - 1 > length || std::memcmp(bytes + candidate, MARKER, sizeof(MARKER) - 1) != 0) {
      continue;
    }
    size_t position = candidate + sizeof(MARKER) - 1;
    while (position < length &&
           (bytes[position] == ' ' || bytes[position] == '\r' || bytes[position] == '\n' || bytes[position] == '\t')) {
      ++position;
    }
    if (position == length || bytes[position] < '0' || bytes[position] > '9') {
      continue;
    }
    uint64_t parsed = 0;
    while (position < length && bytes[position] >= '0' && bytes[position] <= '9') {
      const uint8_t digit = static_cast<uint8_t>(bytes[position] - '0');
      if (parsed > (std::numeric_limits<uint64_t>::max() - digit) / 10) {
        return false;
      }
      parsed = parsed * 10 + digit;
      ++position;
    }
    *offset = parsed;
    return true;
  }
  return false;
}

}  // namespace

void PdfXrefTable::reset() {
  entryCount_ = 0;
  root_ = {};
  hasRoot_ = false;
}

PdfStatus PdfXrefTable::appendNewest(const PdfXrefEntry& entry) {
  if (!records_.valid() || records_.recordSize != sizeof(PdfXrefEntry)) {
    return PdfStatus::failure(PdfError::InvalidArgument, entry.objectNumber);
  }
  for (uint32_t ordinal = 0; ordinal < entryCount_; ++ordinal) {
    PdfXrefEntry existing;
    const PdfStatus status = pdfReadRecord(records_, ordinal, &existing);
    if (!status.ok()) {
      return status;
    }
    if (existing.objectNumber == entry.objectNumber) {
      return PdfStatus::success();
    }
  }
  if (entryCount_ >= records_.capacity) {
    return PdfStatus::failure(PdfError::LimitExceeded, entry.objectNumber);
  }
  const PdfStatus status = pdfWriteRecord(records_, entryCount_, &entry);
  if (status.ok()) {
    ++entryCount_;
  }
  return status;
}

PdfStatus PdfXrefTable::find(const uint32_t objectNumber, PdfXrefEntry* entry) const {
  if (entry == nullptr || !records_.valid() || records_.recordSize != sizeof(PdfXrefEntry)) {
    return PdfStatus::failure(PdfError::InvalidArgument, objectNumber);
  }
  for (uint32_t ordinal = 0; ordinal < entryCount_; ++ordinal) {
    PdfXrefEntry candidate;
    const PdfStatus status = pdfReadRecord(records_, ordinal, &candidate);
    if (!status.ok()) {
      return status;
    }
    if (candidate.objectNumber == objectNumber) {
      *entry = candidate;
      return PdfStatus::success();
    }
  }
  return PdfStatus::failure(PdfError::InvalidOffset, objectNumber);
}

bool PdfXrefTable::root(PdfObjectReference* root) const {
  if (!hasRoot_ || root == nullptr) {
    return false;
  }
  *root = root_;
  return true;
}

PdfXrefParser::PdfXrefParser(const PdfByteSource& source, uint8_t* sourceBuffer, const size_t sourceBufferSize,
                             PdfObjectArena& trailerArena, PdfXrefTable& table)
    : source_(source),
      sourceBuffer_(sourceBuffer),
      sourceBufferSize_(sourceBufferSize),
      lexer_(source, sourceBuffer, sourceBufferSize),
      trailerArena_(trailerArena),
      trailerParser_(lexer_, trailerArena),
      table_(table) {}

void PdfXrefParser::begin() {
  table_.reset();
  phase_ = Phase::FindStartXref;
  visitedCount_ = 0;
  subsectionStart_ = 0;
  subsectionCount_ = 0;
  subsectionIndex_ = 0;
  entryOffset_ = 0;
  entryGeneration_ = 0;
  const size_t tailLength = source_.size > sourceBufferSize_ ? sourceBufferSize_ : static_cast<size_t>(source_.size);
  tailRead_ = {source_.size - tailLength, sourceBuffer_, tailLength, 0};
}

PdfStepResult PdfXrefParser::step(PdfWorkBudget& budget) {
  if (phase_ == Phase::Done) {
    return PdfStepResult::completed();
  }
  if (phase_ == Phase::Failed) {
    return PdfStepResult::failure(PdfStatus::failure(PdfError::Malformed, lexer_.position()));
  }
  if (!source_.valid() || sourceBuffer_ == nullptr || sourceBufferSize_ == 0 ||
      sourceBufferSize_ > PdfLimits::SourceBufferBytes) {
    phase_ = Phase::Failed;
    return PdfStepResult::failure(PdfStatus::failure(PdfError::InvalidArgument));
  }

  auto fail = [this](const PdfStatus status) {
    phase_ = Phase::Failed;
    return PdfStepResult::failure(status);
  };

  while (true) {
    if (phase_ == Phase::FindStartXref) {
      const PdfStepResult readResult = pdfStepReadExact(source_, tailRead_, budget);
      if (!readResult.complete()) {
        if (readResult.failed()) {
          phase_ = Phase::Failed;
        }
        return readResult;
      }
      uint64_t xrefOffset = 0;
      if (!findLastStartXref(sourceBuffer_, tailRead_.length, &xrefOffset)) {
        return fail(
            PdfStatus::failure(source_.size == 0 ? PdfError::UnexpectedEof : PdfError::Malformed, source_.size));
      }
      const PdfStatus sectionStatus = enterSection(xrefOffset);
      if (!sectionStatus.ok()) {
        return fail(sectionStatus);
      }
      continue;
    }

    if (phase_ == Phase::ParseTrailer) {
      const PdfStepResult trailerResult = trailerParser_.step(budget);
      if (!trailerResult.complete()) {
        if (trailerResult.failed()) {
          phase_ = Phase::Failed;
        }
        return trailerResult;
      }
      const PdfStatus trailerStatus = consumeTrailer();
      if (!trailerStatus.ok()) {
        return fail(trailerStatus);
      }
      if (phase_ == Phase::Done) {
        return PdfStepResult::completed();
      }
      continue;
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
      case Phase::ExpectXref:
        if (token.kind != PdfTokenKind::Keyword || !tokenEquals(token, "xref")) {
          return fail(PdfStatus::failure(PdfError::Malformed, lexer_.tokenOffset()));
        }
        phase_ = Phase::SectionStartOrTrailer;
        break;

      case Phase::SectionStartOrTrailer:
        if (token.kind == PdfTokenKind::Keyword && tokenEquals(token, "trailer")) {
          trailerParser_.begin();
          phase_ = Phase::ParseTrailer;
          break;
        }
        if (!parseUnsigned(token, &value) || value > PdfLimits::MaxIndirectObjects) {
          return fail(PdfStatus::failure(PdfError::Malformed, lexer_.tokenOffset()));
        }
        subsectionStart_ = static_cast<uint32_t>(value);
        phase_ = Phase::SectionCount;
        break;

      case Phase::SectionCount: {
        if (!parseUnsigned(token, &value) || value == 0 || value > PdfLimits::MaxIndirectObjects + 1ULL) {
          return fail(PdfStatus::failure(PdfError::LimitExceeded, lexer_.tokenOffset()));
        }
        uint64_t end = 0;
        if (!pdfCheckedAdd(subsectionStart_, value, &end) || end > PdfLimits::MaxIndirectObjects + 1ULL) {
          return fail(PdfStatus::failure(PdfError::LimitExceeded, lexer_.tokenOffset()));
        }
        subsectionCount_ = static_cast<uint32_t>(value);
        subsectionIndex_ = 0;
        phase_ = Phase::EntryOffset;
        break;
      }

      case Phase::EntryOffset:
        if (!parseUnsigned(token, &entryOffset_)) {
          return fail(PdfStatus::failure(PdfError::Malformed, lexer_.tokenOffset()));
        }
        phase_ = Phase::EntryGeneration;
        break;

      case Phase::EntryGeneration:
        if (!parseUnsigned(token, &value) || value > UINT16_MAX) {
          return fail(PdfStatus::failure(PdfError::Malformed, lexer_.tokenOffset()));
        }
        entryGeneration_ = static_cast<uint16_t>(value);
        phase_ = Phase::EntryState;
        break;

      case Phase::EntryState: {
        if (token.kind != PdfTokenKind::Keyword || (!tokenEquals(token, "n") && !tokenEquals(token, "f"))) {
          return fail(PdfStatus::failure(PdfError::Malformed, lexer_.tokenOffset()));
        }
        PdfXrefEntry entry;
        entry.objectNumber = subsectionStart_ + subsectionIndex_;
        entry.generation = entryGeneration_;
        entry.type = tokenEquals(token, "n") ? PdfXrefEntryType::Uncompressed : PdfXrefEntryType::Free;
        entry.offset = entryOffset_;
        if (entry.type == PdfXrefEntryType::Uncompressed && entry.offset >= source_.size) {
          return fail(PdfStatus::failure(PdfError::InvalidOffset, entry.offset));
        }
        const PdfStatus appendStatus = table_.appendNewest(entry);
        if (!appendStatus.ok()) {
          return fail(appendStatus);
        }
        ++subsectionIndex_;
        phase_ = subsectionIndex_ == subsectionCount_ ? Phase::SectionStartOrTrailer : Phase::EntryOffset;
        break;
      }

      default:
        return fail(PdfStatus::failure(PdfError::Malformed, lexer_.position()));
    }
  }
}

PdfStatus PdfXrefParser::enterSection(const uint64_t offset) {
  if (offset >= source_.size) {
    return PdfStatus::failure(PdfError::InvalidOffset, offset);
  }
  if (visitedCount_ >= PdfLimits::MaxTrailerDepth) {
    return PdfStatus::failure(PdfError::LimitExceeded, offset);
  }
  for (uint8_t index = 0; index < visitedCount_; ++index) {
    if (visitedOffsets_[index] == offset) {
      return PdfStatus::failure(PdfError::Malformed, offset);
    }
  }
  visitedOffsets_[visitedCount_++] = offset;
  lexer_.reset(offset);
  phase_ = Phase::ExpectXref;
  return PdfStatus::success();
}

PdfStatus PdfXrefParser::consumeTrailer() {
  const uint16_t rootIndex = trailerParser_.rootIndex();
  if (rootIndex == PDF_INVALID_INDEX || rootIndex >= trailerArena_.valueCount ||
      trailerArena_.values[rootIndex].kind != PdfValueKind::Dictionary) {
    return PdfStatus::failure(PdfError::Malformed, lexer_.tokenOffset());
  }

  uint16_t valueIndex = PDF_INVALID_INDEX;
  if (pdfDictionaryFind(trailerArena_, rootIndex, "Encrypt", &valueIndex)) {
    return PdfStatus::failure(PdfError::Encrypted, lexer_.tokenOffset());
  }
  if (pdfDictionaryFind(trailerArena_, rootIndex, "Size", &valueIndex)) {
    const PdfValue& size = trailerArena_.values[valueIndex];
    if (size.kind != PdfValueKind::Integer || size.integerValue < 1 ||
        static_cast<uint64_t>(size.integerValue) > PdfLimits::MaxIndirectObjects + 1ULL) {
      return PdfStatus::failure(PdfError::LimitExceeded, lexer_.tokenOffset());
    }
  }

  PdfObjectReference existingRoot;
  if (!table_.root(&existingRoot) && pdfDictionaryFind(trailerArena_, rootIndex, "Root", &valueIndex)) {
    const PdfValue& root = trailerArena_.values[valueIndex];
    if (root.kind != PdfValueKind::Reference) {
      return PdfStatus::failure(PdfError::Malformed, lexer_.tokenOffset());
    }
    table_.setRoot({root.objectNumber, root.generation});
  }

  if (pdfDictionaryFind(trailerArena_, rootIndex, "Prev", &valueIndex)) {
    const PdfValue& previous = trailerArena_.values[valueIndex];
    if (previous.kind != PdfValueKind::Integer || previous.integerValue < 0) {
      return PdfStatus::failure(PdfError::Malformed, lexer_.tokenOffset());
    }
    return enterSection(static_cast<uint64_t>(previous.integerValue));
  }

  if (!table_.root(&existingRoot)) {
    return PdfStatus::failure(PdfError::Malformed, lexer_.tokenOffset());
  }
  phase_ = Phase::Done;
  return PdfStatus::success();
}
