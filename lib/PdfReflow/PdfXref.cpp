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

void stableSortXrefRun(PdfXrefEntry* entries, const uint32_t count) {
  for (uint32_t index = 1; index < count; ++index) {
    const PdfXrefEntry current = entries[index];
    uint32_t position = index;
    while (position != 0 && entries[position - 1].objectNumber > current.objectNumber) {
      entries[position] = entries[position - 1];
      --position;
    }
    entries[position] = current;
  }
}

}  // namespace

void PdfXrefTable::reset() {
  entryCount_ = 0;
  root_ = {};
  hasRoot_ = false;
  finalized_ = false;
}

PdfStatus PdfXrefTable::appendNewest(const PdfXrefEntry& entry) {
  if (finalized_ || !records_.valid() || records_.recordSize != sizeof(PdfXrefEntry)) {
    return PdfStatus::failure(PdfError::InvalidArgument, entry.objectNumber);
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

PdfStatus PdfXrefTable::finalize(const PdfFixedRecordStore scratch, PdfXrefEntry* mergeBuffer,
                                 const uint16_t mergeCapacity) {
  if (finalized_) {
    return PdfStatus::success();
  }
  if (!records_.valid() || records_.recordSize != sizeof(PdfXrefEntry) || mergeBuffer == nullptr ||
      mergeCapacity < PdfLimits::XrefMergeEntries) {
    return PdfStatus::failure(PdfError::InvalidArgument, mergeCapacity);
  }
  if (entryCount_ == 0) {
    finalized_ = true;
    return PdfStatus::success();
  }

  const uint32_t runLength = PdfLimits::XrefMergeEntries;
  for (uint32_t start = 0; start < entryCount_; start += runLength) {
    const uint32_t count = std::min<uint32_t>(runLength, entryCount_ - start);
    for (uint32_t index = 0; index < count; ++index) {
      const PdfStatus status = pdfReadRecord(records_, start + index, &mergeBuffer[index]);
      if (!status.ok()) {
        return status;
      }
    }
    stableSortXrefRun(mergeBuffer, count);
    for (uint32_t index = 0; index < count; ++index) {
      const PdfStatus status = pdfWriteRecord(records_, start + index, &mergeBuffer[index]);
      if (!status.ok()) {
        return status;
      }
    }
  }

  PdfFixedRecordStore input = records_;
  PdfFixedRecordStore output = scratch;
  uint32_t mergedRunLength = runLength;
  while (mergedRunLength < entryCount_) {
    if (!output.valid() || output.recordSize != sizeof(PdfXrefEntry) || output.capacity < entryCount_) {
      return PdfStatus::failure(PdfError::InvalidArgument, entryCount_);
    }
    for (uint32_t start = 0; start < entryCount_; start += mergedRunLength * 2) {
      const uint32_t middle = std::min<uint32_t>(start + mergedRunLength, entryCount_);
      const uint32_t end = std::min<uint32_t>(middle + mergedRunLength, entryCount_);
      uint32_t left = start;
      uint32_t right = middle;
      PdfXrefEntry leftEntry{};
      PdfXrefEntry rightEntry{};
      bool haveLeft = false;
      bool haveRight = false;
      for (uint32_t destination = start; destination < end; ++destination) {
        if (!haveLeft && left < middle) {
          const PdfStatus status = pdfReadRecord(input, left, &leftEntry);
          if (!status.ok()) {
            return status;
          }
          haveLeft = true;
        }
        if (!haveRight && right < end) {
          const PdfStatus status = pdfReadRecord(input, right, &rightEntry);
          if (!status.ok()) {
            return status;
          }
          haveRight = true;
        }
        const bool takeLeft = haveLeft && (!haveRight || leftEntry.objectNumber <= rightEntry.objectNumber);
        const PdfXrefEntry& selected = takeLeft ? leftEntry : rightEntry;
        const PdfStatus writeStatus = pdfWriteRecord(output, destination, &selected);
        if (!writeStatus.ok()) {
          return writeStatus;
        }
        if (takeLeft) {
          haveLeft = false;
          ++left;
        } else {
          haveRight = false;
          ++right;
        }
      }
    }
    const PdfFixedRecordStore swap = input;
    input = output;
    output = swap;
    if (mergedRunLength > UINT32_MAX / 2) {
      return PdfStatus::failure(PdfError::LimitExceeded, mergedRunLength);
    }
    mergedRunLength *= 2;
  }
  records_ = input;

  uint32_t uniqueCount = 0;
  uint32_t previousObject = UINT32_MAX;
  for (uint32_t ordinal = 0; ordinal < entryCount_; ++ordinal) {
    PdfXrefEntry entry;
    const PdfStatus readStatus = pdfReadRecord(records_, ordinal, &entry);
    if (!readStatus.ok()) {
      return readStatus;
    }
    if (uniqueCount != 0 && entry.objectNumber == previousObject) {
      continue;
    }
    previousObject = entry.objectNumber;
    const PdfStatus writeStatus = pdfWriteRecord(records_, uniqueCount, &entry);
    if (!writeStatus.ok()) {
      return writeStatus;
    }
    ++uniqueCount;
  }
  entryCount_ = uniqueCount;
  finalized_ = true;
  return PdfStatus::success();
}

PdfStatus PdfXrefTable::find(const uint32_t objectNumber, PdfXrefEntry* entry) const {
  if (entry == nullptr || !records_.valid() || records_.recordSize != sizeof(PdfXrefEntry)) {
    return PdfStatus::failure(PdfError::InvalidArgument, objectNumber);
  }
  if (finalized_) {
    uint32_t first = 0;
    uint32_t last = entryCount_;
    while (first < last) {
      const uint32_t middle = first + (last - first) / 2;
      PdfXrefEntry candidate;
      const PdfStatus status = pdfReadRecord(records_, middle, &candidate);
      if (!status.ok()) {
        return status;
      }
      if (candidate.objectNumber < objectNumber) {
        first = middle + 1;
      } else {
        last = middle;
      }
    }
    if (first < entryCount_) {
      PdfXrefEntry candidate;
      const PdfStatus status = pdfReadRecord(records_, first, &candidate);
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
                             PdfObjectArena& trailerArena, PdfXrefTable& table, PdfStreamDecoder* const streamDecoder)
    : source_(source),
      sourceBuffer_(sourceBuffer),
      sourceBufferSize_(sourceBufferSize),
      lexer_(source, sourceBuffer, sourceBufferSize),
      trailerArena_(trailerArena),
      trailerParser_(lexer_, trailerArena),
      table_(table),
      streamDecoder_(streamDecoder) {}

void PdfXrefParser::begin() {
  table_.reset();
  phase_ = Phase::FindStartXref;
  visitedCount_ = 0;
  subsectionStart_ = 0;
  subsectionCount_ = 0;
  subsectionIndex_ = 0;
  entryOffset_ = 0;
  sectionOffset_ = 0;
  streamObjectNumber_ = 0;
  streamLength_ = 0;
  eolOffset_ = 0;
  pendingPrev_ = 0;
  xrefExpectedEntries_ = 0;
  xrefDecodedEntries_ = 0;
  xrefCurrentObject_ = 0;
  xrefRangeRemaining_ = 0;
  xrefIndexValue_ = PDF_INVALID_INDEX;
  xrefIndexLink_ = PDF_INVALID_INDEX;
  entryGeneration_ = 0;
  eolByte_ = 0;
  xrefFieldIndex_ = 0;
  xrefFieldByteIndex_ = 0;
  xrefIndexPairsRemaining_ = 0;
  streamFilterCount_ = 0;
  hasPendingPrev_ = false;
  xrefUsesDefaultIndex_ = false;
  std::memset(xrefFieldValues_, 0, sizeof(xrefFieldValues_));
  std::memset(xrefWidths_, 0, sizeof(xrefWidths_));
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

    if (phase_ == Phase::ParseTrailer || phase_ == Phase::ParseStreamDictionary) {
      const PdfStepResult trailerResult = trailerParser_.step(budget);
      if (!trailerResult.complete()) {
        if (trailerResult.failed()) {
          phase_ = Phase::Failed;
        }
        return trailerResult;
      }
      if (phase_ == Phase::ParseTrailer) {
        const PdfStatus trailerStatus = consumeTrailer();
        if (!trailerStatus.ok()) {
          return fail(trailerStatus);
        }
        if (phase_ == Phase::Done) {
          return PdfStepResult::completed();
        }
      } else {
        const uint16_t rootIndex = trailerParser_.rootIndex();
        const PdfStatus commonStatus = consumeCommonDictionary(rootIndex);
        if (!commonStatus.ok()) {
          return fail(commonStatus);
        }
        const PdfStatus streamStatus = configureXrefStream(rootIndex);
        if (!streamStatus.ok()) {
          return fail(streamStatus);
        }
        phase_ = Phase::StreamKeyword;
      }
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
      uint64_t streamOffset = 0;
      if (phase_ == Phase::StreamFirstEol) {
        if (eolByte_ == '\n') {
          streamOffset = eolOffset_ + 1;
        } else if (eolByte_ == '\r') {
          phase_ = Phase::StreamSecondEol;
          eolRead_ = {eolOffset_ + 1, &eolByte_, 1, 0};
          continue;
        } else {
          return fail(PdfStatus::failure(PdfError::Malformed, eolOffset_));
        }
      } else {
        if (eolByte_ != '\n') {
          return fail(PdfStatus::failure(PdfError::Malformed, eolOffset_ + 1));
        }
        streamOffset = eolOffset_ + 2;
      }
      const PdfStatus rangeStatus = pdfInitializeByteRange(source_, streamOffset, streamLength_, &xrefStreamRange_);
      if (!rangeStatus.ok()) {
        return fail(rangeStatus);
      }
      const PdfByteSource streamSource = pdfByteRangeSource(xrefStreamRange_);
      const PdfByteSink streamSink{this, writeDecodedXref};
      if (streamDecoder_ == nullptr) {
        return fail(PdfStatus::failure(PdfError::UnsupportedFilter, sectionOffset_));
      }
      const PdfStatus decoderStatus =
          streamDecoder_->begin(streamSource, streamSink, streamFilters_, streamFilterCount_);
      if (!decoderStatus.ok()) {
        return fail(decoderStatus);
      }
      phase_ = Phase::DecodeXrefStream;
      continue;
    }

    if (phase_ == Phase::DecodeXrefStream) {
      const PdfStepResult decodeResult = streamDecoder_->step(budget);
      if (!decodeResult.complete()) {
        if (decodeResult.failed()) {
          phase_ = Phase::Failed;
        }
        return decodeResult;
      }
      const PdfStatus finishStatus = finishXrefStream();
      if (!finishStatus.ok()) {
        return fail(finishStatus);
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
        if (token.kind == PdfTokenKind::Keyword && tokenEquals(token, "xref")) {
          phase_ = Phase::SectionStartOrTrailer;
          break;
        }
        if (!parseUnsigned(token, &streamObjectNumber_) || streamObjectNumber_ > PdfLimits::MaxIndirectObjects) {
          return fail(PdfStatus::failure(PdfError::Malformed, lexer_.tokenOffset()));
        }
        phase_ = Phase::StreamGeneration;
        break;

      case Phase::StreamGeneration:
        if (!parseUnsigned(token, &value) || value > UINT16_MAX) {
          return fail(PdfStatus::failure(PdfError::Malformed, lexer_.tokenOffset()));
        }
        entryGeneration_ = static_cast<uint16_t>(value);
        phase_ = Phase::StreamObjKeyword;
        break;

      case Phase::StreamObjKeyword:
        if (token.kind != PdfTokenKind::Keyword || !tokenEquals(token, "obj")) {
          return fail(PdfStatus::failure(PdfError::Malformed, lexer_.tokenOffset()));
        }
        trailerParser_.begin();
        phase_ = Phase::ParseStreamDictionary;
        break;

      case Phase::StreamKeyword: {
        if (token.kind != PdfTokenKind::Keyword || !tokenEquals(token, "stream")) {
          return fail(PdfStatus::failure(PdfError::Malformed, lexer_.tokenOffset()));
        }
        eolOffset_ = lexer_.position();
        eolRead_ = {eolOffset_, &eolByte_, 1, 0};
        phase_ = Phase::StreamFirstEol;
        break;
      }

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
  sectionOffset_ = offset;
  lexer_.reset(offset);
  phase_ = Phase::ExpectXref;
  return PdfStatus::success();
}

PdfStatus PdfXrefParser::consumeTrailer() {
  const uint16_t rootIndex = trailerParser_.rootIndex();
  const PdfStatus status = consumeCommonDictionary(rootIndex);
  if (!status.ok()) {
    return status;
  }
  PdfObjectReference existingRoot;
  if (hasPendingPrev_) {
    return enterSection(pendingPrev_);
  }
  if (!table_.root(&existingRoot)) {
    return PdfStatus::failure(PdfError::Malformed, lexer_.tokenOffset());
  }
  phase_ = Phase::Done;
  return PdfStatus::success();
}

PdfStatus PdfXrefParser::consumeCommonDictionary(const uint16_t rootIndex) {
  if (rootIndex == PDF_INVALID_INDEX || rootIndex >= trailerArena_.valueCount ||
      trailerArena_.values[rootIndex].kind != PdfValueKind::Dictionary) {
    return PdfStatus::failure(PdfError::Malformed, lexer_.tokenOffset());
  }

  hasPendingPrev_ = false;
  pendingPrev_ = 0;
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
    pendingPrev_ = static_cast<uint64_t>(previous.integerValue);
    hasPendingPrev_ = true;
  }
  return PdfStatus::success();
}

PdfStatus PdfXrefParser::configureXrefStream(const uint16_t rootIndex) {
  uint16_t valueIndex = PDF_INVALID_INDEX;
  if (!pdfDictionaryFind(trailerArena_, rootIndex, "Type", &valueIndex) || valueIndex >= trailerArena_.valueCount ||
      !pdfTextEquals(trailerArena_, trailerArena_.values[valueIndex], "XRef")) {
    return PdfStatus::failure(PdfError::Malformed, sectionOffset_);
  }
  if (!pdfDictionaryFind(trailerArena_, rootIndex, "Length", &valueIndex) || valueIndex >= trailerArena_.valueCount) {
    return PdfStatus::failure(PdfError::Malformed, sectionOffset_);
  }
  const PdfValue& length = trailerArena_.values[valueIndex];
  if (length.kind != PdfValueKind::Integer || length.integerValue < 0) {
    return PdfStatus::failure(length.kind == PdfValueKind::Reference ? PdfError::Unsupported : PdfError::Malformed,
                              sectionOffset_);
  }
  streamLength_ = static_cast<uint64_t>(length.integerValue);

  if (!pdfDictionaryFind(trailerArena_, rootIndex, "W", &valueIndex) || valueIndex >= trailerArena_.valueCount ||
      trailerArena_.values[valueIndex].kind != PdfValueKind::Array || trailerArena_.values[valueIndex].count != 3) {
    return PdfStatus::failure(PdfError::Malformed, sectionOffset_);
  }
  uint16_t totalWidth = 0;
  for (uint16_t ordinal = 0; ordinal < 3; ++ordinal) {
    uint16_t itemIndex = PDF_INVALID_INDEX;
    if (!pdfArrayAt(trailerArena_, valueIndex, ordinal, &itemIndex) || itemIndex >= trailerArena_.valueCount) {
      return PdfStatus::failure(PdfError::Malformed, sectionOffset_);
    }
    const PdfValue& width = trailerArena_.values[itemIndex];
    if (width.kind != PdfValueKind::Integer || width.integerValue < 0 ||
        width.integerValue > PdfLimits::MaxXrefFieldBytes) {
      return PdfStatus::failure(PdfError::LimitExceeded, sectionOffset_);
    }
    xrefWidths_[ordinal] = static_cast<uint8_t>(width.integerValue);
    totalWidth = static_cast<uint16_t>(totalWidth + width.integerValue);
  }
  if (totalWidth == 0 || totalWidth > PdfLimits::MaxXrefEntryBytes) {
    return PdfStatus::failure(PdfError::LimitExceeded, sectionOffset_);
  }

  uint16_t sizeIndex = PDF_INVALID_INDEX;
  if (!pdfDictionaryFind(trailerArena_, rootIndex, "Size", &sizeIndex) || sizeIndex >= trailerArena_.valueCount) {
    return PdfStatus::failure(PdfError::Malformed, sectionOffset_);
  }
  const PdfValue& size = trailerArena_.values[sizeIndex];
  if (size.kind != PdfValueKind::Integer || size.integerValue < 1 ||
      static_cast<uint64_t>(size.integerValue) > PdfLimits::MaxIndirectObjects + 1ULL) {
    return PdfStatus::failure(PdfError::LimitExceeded, sectionOffset_);
  }

  xrefExpectedEntries_ = 0;
  xrefDecodedEntries_ = 0;
  xrefFieldIndex_ = 0;
  xrefFieldByteIndex_ = 0;
  std::memset(xrefFieldValues_, 0, sizeof(xrefFieldValues_));
  xrefIndexValue_ = PDF_INVALID_INDEX;
  xrefIndexLink_ = PDF_INVALID_INDEX;
  xrefIndexPairsRemaining_ = 0;
  xrefUsesDefaultIndex_ = true;
  if (pdfDictionaryFind(trailerArena_, rootIndex, "Index", &valueIndex)) {
    if (valueIndex >= trailerArena_.valueCount || trailerArena_.values[valueIndex].kind != PdfValueKind::Array ||
        trailerArena_.values[valueIndex].count == 0 || (trailerArena_.values[valueIndex].count & 1U) != 0 ||
        trailerArena_.values[valueIndex].count / 2 > PdfLimits::MaxXrefIndexPairs) {
      return PdfStatus::failure(PdfError::LimitExceeded, sectionOffset_);
    }
    xrefUsesDefaultIndex_ = false;
    xrefIndexValue_ = valueIndex;
    xrefIndexLink_ = trailerArena_.values[valueIndex].firstLink;
    xrefIndexPairsRemaining_ = static_cast<uint8_t>(trailerArena_.values[valueIndex].count / 2);

    uint16_t link = xrefIndexLink_;
    for (uint8_t pair = 0; pair < xrefIndexPairsRemaining_; ++pair) {
      if (link >= trailerArena_.arrayCount) {
        return PdfStatus::failure(PdfError::Malformed, sectionOffset_);
      }
      const PdfArrayItem& firstItem = trailerArena_.arrayItems[link];
      link = firstItem.next;
      if (link >= trailerArena_.arrayCount || firstItem.valueIndex >= trailerArena_.valueCount) {
        return PdfStatus::failure(PdfError::Malformed, sectionOffset_);
      }
      const PdfArrayItem& countItem = trailerArena_.arrayItems[link];
      link = countItem.next;
      if (countItem.valueIndex >= trailerArena_.valueCount) {
        return PdfStatus::failure(PdfError::Malformed, sectionOffset_);
      }
      const PdfValue& first = trailerArena_.values[firstItem.valueIndex];
      const PdfValue& count = trailerArena_.values[countItem.valueIndex];
      if (first.kind != PdfValueKind::Integer || first.integerValue < 0 || count.kind != PdfValueKind::Integer ||
          count.integerValue <= 0) {
        return PdfStatus::failure(PdfError::Malformed, sectionOffset_);
      }
      uint64_t rangeEnd = 0;
      if (!pdfCheckedAdd(static_cast<uint64_t>(first.integerValue), static_cast<uint64_t>(count.integerValue),
                         &rangeEnd) ||
          rangeEnd > PdfLimits::MaxIndirectObjects + 1ULL ||
          static_cast<uint64_t>(xrefExpectedEntries_) + static_cast<uint64_t>(count.integerValue) >
              PdfLimits::MaxIndirectObjects + 1ULL) {
        return PdfStatus::failure(PdfError::LimitExceeded, sectionOffset_);
      }
      xrefExpectedEntries_ += static_cast<uint32_t>(count.integerValue);
    }
    const PdfStatus rangeStatus = advanceXrefRange();
    if (!rangeStatus.ok()) {
      return rangeStatus;
    }
  } else {
    xrefExpectedEntries_ = static_cast<uint32_t>(size.integerValue);
    xrefCurrentObject_ = 0;
    xrefRangeRemaining_ = xrefExpectedEntries_;
  }

  return pdfStreamFiltersFromDictionary(trailerArena_, rootIndex, streamFilters_, PdfLimits::MaxFiltersPerStream,
                                        &streamFilterCount_);
}

PdfStatus PdfXrefParser::advanceXrefRange() {
  if (xrefUsesDefaultIndex_ || xrefIndexPairsRemaining_ == 0) {
    xrefRangeRemaining_ = 0;
    return PdfStatus::success();
  }
  if (xrefIndexLink_ >= trailerArena_.arrayCount) {
    return PdfStatus::failure(PdfError::Malformed, sectionOffset_);
  }
  const PdfArrayItem& firstItem = trailerArena_.arrayItems[xrefIndexLink_];
  xrefIndexLink_ = firstItem.next;
  if (xrefIndexLink_ >= trailerArena_.arrayCount || firstItem.valueIndex >= trailerArena_.valueCount) {
    return PdfStatus::failure(PdfError::Malformed, sectionOffset_);
  }
  const PdfArrayItem& countItem = trailerArena_.arrayItems[xrefIndexLink_];
  xrefIndexLink_ = countItem.next;
  if (countItem.valueIndex >= trailerArena_.valueCount) {
    return PdfStatus::failure(PdfError::Malformed, sectionOffset_);
  }
  const PdfValue& first = trailerArena_.values[firstItem.valueIndex];
  const PdfValue& count = trailerArena_.values[countItem.valueIndex];
  xrefCurrentObject_ = static_cast<uint32_t>(first.integerValue);
  xrefRangeRemaining_ = static_cast<uint32_t>(count.integerValue);
  --xrefIndexPairsRemaining_;
  return PdfStatus::success();
}

PdfStatus PdfXrefParser::consumeXrefByte(const uint8_t byte) {
  if (xrefDecodedEntries_ >= xrefExpectedEntries_ || xrefRangeRemaining_ == 0) {
    return PdfStatus::failure(PdfError::Malformed, sectionOffset_);
  }
  while (xrefFieldIndex_ < 3 && xrefWidths_[xrefFieldIndex_] == 0) {
    xrefFieldValues_[xrefFieldIndex_] = xrefFieldIndex_ == 0 ? 1 : 0;
    ++xrefFieldIndex_;
  }
  if (xrefFieldIndex_ >= 3) {
    return PdfStatus::failure(PdfError::Malformed, sectionOffset_);
  }
  if (xrefFieldValues_[xrefFieldIndex_] > (std::numeric_limits<uint64_t>::max() >> 8)) {
    return PdfStatus::failure(PdfError::LimitExceeded, sectionOffset_);
  }
  xrefFieldValues_[xrefFieldIndex_] = (xrefFieldValues_[xrefFieldIndex_] << 8) | byte;
  ++xrefFieldByteIndex_;
  if (xrefFieldByteIndex_ < xrefWidths_[xrefFieldIndex_]) {
    return PdfStatus::success();
  }
  xrefFieldByteIndex_ = 0;
  ++xrefFieldIndex_;
  while (xrefFieldIndex_ < 3 && xrefWidths_[xrefFieldIndex_] == 0) {
    xrefFieldValues_[xrefFieldIndex_] = 0;
    ++xrefFieldIndex_;
  }
  if (xrefFieldIndex_ < 3) {
    return PdfStatus::success();
  }

  PdfXrefEntry entry;
  entry.objectNumber = xrefCurrentObject_;
  if (xrefFieldValues_[0] > 2) {
    return PdfStatus::failure(PdfError::Malformed, sectionOffset_);
  }
  entry.type = static_cast<PdfXrefEntryType>(xrefFieldValues_[0]);
  if (entry.type == PdfXrefEntryType::Free || entry.type == PdfXrefEntryType::Uncompressed) {
    if (xrefFieldValues_[2] > UINT16_MAX) {
      return PdfStatus::failure(PdfError::LimitExceeded, sectionOffset_);
    }
    entry.offset = xrefFieldValues_[1];
    entry.generation = static_cast<uint16_t>(xrefFieldValues_[2]);
    if (entry.type == PdfXrefEntryType::Uncompressed && entry.offset >= source_.size) {
      return PdfStatus::failure(PdfError::InvalidOffset, entry.offset);
    }
  } else {
    if (xrefFieldValues_[1] > PdfLimits::MaxIndirectObjects || xrefFieldValues_[2] > UINT32_MAX) {
      return PdfStatus::failure(PdfError::LimitExceeded, sectionOffset_);
    }
    entry.offset = xrefFieldValues_[1];
    entry.objectStreamIndex = static_cast<uint32_t>(xrefFieldValues_[2]);
  }
  const PdfStatus appendStatus = table_.appendNewest(entry);
  if (!appendStatus.ok()) {
    return appendStatus;
  }

  ++xrefDecodedEntries_;
  --xrefRangeRemaining_;
  ++xrefCurrentObject_;
  if (xrefRangeRemaining_ == 0 && xrefIndexPairsRemaining_ != 0) {
    const PdfStatus rangeStatus = advanceXrefRange();
    if (!rangeStatus.ok()) {
      return rangeStatus;
    }
  }
  xrefFieldIndex_ = 0;
  xrefFieldByteIndex_ = 0;
  std::memset(xrefFieldValues_, 0, sizeof(xrefFieldValues_));
  return PdfStatus::success();
}

PdfStatus PdfXrefParser::writeDecodedXref(void* context, const uint8_t* source, const size_t requested,
                                          size_t* bytesWritten) {
  if (context == nullptr || source == nullptr || bytesWritten == nullptr) {
    return PdfStatus::failure(PdfError::InvalidArgument);
  }
  auto& parser = *static_cast<PdfXrefParser*>(context);
  *bytesWritten = 0;
  for (size_t index = 0; index < requested; ++index) {
    const PdfStatus status = parser.consumeXrefByte(source[index]);
    if (!status.ok()) {
      return status;
    }
    ++*bytesWritten;
  }
  return PdfStatus::success();
}

PdfStatus PdfXrefParser::finishXrefStream() {
  if (xrefDecodedEntries_ != xrefExpectedEntries_ || xrefFieldIndex_ != 0 || xrefFieldByteIndex_ != 0 ||
      xrefRangeRemaining_ != 0 || xrefIndexPairsRemaining_ != 0) {
    return PdfStatus::failure(PdfError::UnexpectedEof, sectionOffset_);
  }
  PdfObjectReference existingRoot;
  if (hasPendingPrev_) {
    return enterSection(pendingPrev_);
  }
  if (!table_.root(&existingRoot)) {
    return PdfStatus::failure(PdfError::Malformed, sectionOffset_);
  }
  phase_ = Phase::Done;
  return PdfStatus::success();
}
