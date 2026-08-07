#include "PdfXref.h"

#include <algorithm>
#include <cstring>
#include <limits>

#include "PdfCheckedMath.h"
#include "PdfLimits.h"

namespace {

constexpr size_t kNewestObjectFilterBytes = (PdfLimits::MaxIndirectObjects + 1U + 7U) / 8U;

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
  constexpr char EOF_MARKER[] = "%%EOF";
  if (bytes == nullptr || offset == nullptr) {
    return false;
  }
  size_t eof = length;
  while (eof != 0 && (bytes[eof - 1U] == 0 || bytes[eof - 1U] == ' ' || bytes[eof - 1U] == '\t' ||
                      bytes[eof - 1U] == '\n' || bytes[eof - 1U] == '\f' || bytes[eof - 1U] == '\r')) {
    --eof;
  }
  if (eof < sizeof(EOF_MARKER) - 1U ||
      std::memcmp(bytes + eof - (sizeof(EOF_MARKER) - 1U), EOF_MARKER, sizeof(EOF_MARKER) - 1U) != 0) {
    return false;
  }
  for (size_t candidate = eof - (sizeof(EOF_MARKER) - 1U); candidate-- > 0;) {
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

PdfStatus PdfXrefTable::configureNewestObjectFilter(uint8_t* const first, const size_t firstBytes,
                                                    uint8_t* const second, const size_t secondBytes) {
  if (entryCount_ != 0 || finalized_ || first == nullptr) {
    return PdfStatus::failure(PdfError::InvalidArgument, firstBytes);
  }
  const size_t usedFirstBytes = std::min(firstBytes, kNewestObjectFilterBytes);
  const size_t usedSecondBytes = kNewestObjectFilterBytes - usedFirstBytes;
  if (usedSecondBytes != 0 && (second == nullptr || secondBytes < usedSecondBytes)) {
    return PdfStatus::failure(PdfError::InvalidArgument, firstBytes + secondBytes);
  }
  seenObjectsFirst_ = first;
  seenObjectsFirstBytes_ = usedFirstBytes;
  seenObjectsSecond_ = usedSecondBytes == 0 ? nullptr : second;
  seenObjectsSecondBytes_ = usedSecondBytes;
  std::memset(seenObjectsFirst_, 0, seenObjectsFirstBytes_);
  if (seenObjectsSecond_ != nullptr) {
    std::memset(seenObjectsSecond_, 0, seenObjectsSecondBytes_);
  }
  return PdfStatus::success();
}

void PdfXrefTable::detachNewestObjectFilter() {
  seenObjectsFirst_ = nullptr;
  seenObjectsSecond_ = nullptr;
  seenObjectsFirstBytes_ = 0;
  seenObjectsSecondBytes_ = 0;
}

void PdfXrefTable::reset() {
  entryCount_ = 0;
  root_ = {};
  info_ = {};
  hasRoot_ = false;
  hasInfo_ = false;
  finalized_ = false;
  appendOrderStrict_ = true;
  lastAppendedObject_ = 0;
  if (seenObjectsFirst_ != nullptr) {
    std::memset(seenObjectsFirst_, 0, seenObjectsFirstBytes_);
  }
  if (seenObjectsSecond_ != nullptr) {
    std::memset(seenObjectsSecond_, 0, seenObjectsSecondBytes_);
  }
}

PdfStatus PdfXrefTable::appendNewest(const PdfXrefEntry& entry) {
  if (finalized_ || !records_.valid() || records_.recordSize != sizeof(PdfXrefEntry) ||
      entry.objectNumber > PdfLimits::MaxIndirectObjects) {
    return PdfStatus::failure(PdfError::InvalidArgument, entry.objectNumber);
  }
  const size_t seenByte = entry.objectNumber / 8U;
  const uint8_t seenMask = static_cast<uint8_t>(1U << (entry.objectNumber % 8U));
  uint8_t* seen = nullptr;
  if (seenObjectsFirst_ != nullptr) {
    seen = seenByte < seenObjectsFirstBytes_ ? seenObjectsFirst_ + seenByte
                                             : seenObjectsSecond_ + (seenByte - seenObjectsFirstBytes_);
  }
  if (seen != nullptr && (*seen & seenMask) != 0) {
    return PdfStatus::success();
  }
  if (entryCount_ >= records_.capacity) {
    return PdfStatus::failure(PdfError::LimitExceeded, entry.objectNumber);
  }
  const PdfStatus status = pdfWriteRecord(records_, entryCount_, &entry);
  if (status.ok()) {
    if (entryCount_ != 0 && entry.objectNumber <= lastAppendedObject_) {
      appendOrderStrict_ = false;
    }
    lastAppendedObject_ = entry.objectNumber;
    if (seen != nullptr) {
      *seen |= seenMask;
    }
    ++entryCount_;
  }
  return status;
}

PdfStatus PdfXrefTable::adoptSortedRecords(const uint32_t count) {
  if (finalized_ || entryCount_ != 0 || count == 0 || !records_.valid() ||
      records_.recordSize != sizeof(PdfXrefEntry) || count > records_.capacity) {
    return PdfStatus::failure(PdfError::InvalidArgument, count);
  }
  entryCount_ = count;
  finalized_ = true;
  appendOrderStrict_ = true;
  return PdfStatus::success();
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

PdfStatus PdfXrefTable::beginFind(const uint32_t objectNumber, PdfXrefLookupState* const state) const {
  if (state == nullptr || !records_.valid() || records_.recordSize != sizeof(PdfXrefEntry) ||
      objectNumber > PdfLimits::MaxIndirectObjects) {
    return PdfStatus::failure(PdfError::InvalidArgument, objectNumber);
  }
  *state = {};
  state->objectNumber = objectNumber;
  state->last = entryCount_;
  state->phase = finalized_ ? PdfXrefLookupPhase::Binary : PdfXrefLookupPhase::Linear;
  return PdfStatus::success();
}

PdfStepResult PdfXrefTable::stepFind(PdfXrefLookupState& state, PdfXrefEntry* const entry,
                                     PdfWorkBudget& budget) const {
  if (entry == nullptr || state.phase == PdfXrefLookupPhase::Idle || !records_.valid() ||
      records_.recordSize != sizeof(PdfXrefEntry)) {
    return PdfStepResult::failure(PdfStatus::failure(PdfError::InvalidArgument, state.objectNumber));
  }
  if (state.phase == PdfXrefLookupPhase::Done) {
    *entry = state.entry;
    return PdfStepResult::completed();
  }
  if (state.phase == PdfXrefLookupPhase::Failed) {
    return PdfStepResult::failure(state.status);
  }

  auto readRecord = [&](const uint32_t ordinal) -> PdfStepResult {
    if (budget.cancelRequested()) {
      return PdfStepResult::failure(PdfStatus::failure(PdfError::Cancelled, state.objectNumber));
    }
    if (budget.stopRequested() || budget.operationsRemaining == 0 || budget.bytesRemaining < sizeof(PdfXrefEntry)) {
      return PdfStepResult::paused();
    }
    --budget.operationsRemaining;
    budget.bytesRemaining -= sizeof(PdfXrefEntry);
    const PdfStatus status = pdfReadRecord(records_, ordinal, &state.entry);
    if (!status) {
      state.status = status;
      state.phase = PdfXrefLookupPhase::Failed;
      return PdfStepResult::failure(status);
    }
    return PdfStepResult::completed();
  };

  while (true) {
    if (state.phase == PdfXrefLookupPhase::Linear) {
      if (state.cursor >= entryCount_) {
        state.status = PdfStatus::failure(PdfError::InvalidOffset, state.objectNumber);
        state.phase = PdfXrefLookupPhase::Failed;
        return PdfStepResult::failure(state.status);
      }
      const PdfStepResult read = readRecord(state.cursor);
      if (!read.complete()) {
        return read;
      }
      ++state.cursor;
      if (state.entry.objectNumber == state.objectNumber) {
        state.phase = PdfXrefLookupPhase::Done;
        *entry = state.entry;
        return PdfStepResult::completed();
      }
      continue;
    }

    if (state.phase == PdfXrefLookupPhase::Binary) {
      if (state.first >= state.last) {
        state.phase = PdfXrefLookupPhase::Verify;
        continue;
      }
      const uint32_t middle = state.first + (state.last - state.first) / 2U;
      const PdfStepResult read = readRecord(middle);
      if (!read.complete()) {
        return read;
      }
      if (state.entry.objectNumber < state.objectNumber) {
        state.first = middle + 1U;
      } else {
        state.last = middle;
      }
      continue;
    }

    if (state.phase == PdfXrefLookupPhase::Verify) {
      if (state.first >= entryCount_) {
        state.status = PdfStatus::failure(PdfError::InvalidOffset, state.objectNumber);
        state.phase = PdfXrefLookupPhase::Failed;
        return PdfStepResult::failure(state.status);
      }
      const PdfStepResult read = readRecord(state.first);
      if (!read.complete()) {
        return read;
      }
      if (state.entry.objectNumber != state.objectNumber) {
        state.status = PdfStatus::failure(PdfError::InvalidOffset, state.objectNumber);
        state.phase = PdfXrefLookupPhase::Failed;
        return PdfStepResult::failure(state.status);
      }
      state.phase = PdfXrefLookupPhase::Done;
      *entry = state.entry;
      return PdfStepResult::completed();
    }

    state.status = PdfStatus::failure(PdfError::InvalidArgument, state.objectNumber);
    state.phase = PdfXrefLookupPhase::Failed;
    return PdfStepResult::failure(state.status);
  }
}

PdfStatus PdfXrefTable::find(const uint32_t objectNumber, PdfXrefEntry* const entry) const {
  PdfXrefLookupState state{};
  PdfStatus status = beginFind(objectNumber, &state);
  if (!status) {
    return status;
  }
  while (true) {
    PdfWorkBudget budget{UINT32_MAX, SIZE_MAX};
    const PdfStepResult result = stepFind(state, entry, budget);
    if (result.complete() || result.failed()) {
      return result.status;
    }
  }
}

bool PdfXrefTable::root(PdfObjectReference* root) const {
  if (!hasRoot_ || root == nullptr) {
    return false;
  }
  *root = root_;
  return true;
}

bool PdfXrefTable::info(PdfObjectReference* info) const {
  if (!hasInfo_ || info == nullptr) {
    return false;
  }
  *info = info_;
  return true;
}

PdfXrefParser::PdfXrefParser(const PdfByteSource& source, uint8_t* sourceBuffer, const size_t sourceBufferSize,
                             PdfObjectArena& trailerArena, PdfXrefTable& table, PdfStreamDecoder* const streamDecoder,
                             const PdfStreamDecodeLimits decodeLimits)
    : source_(source),
      sourceBuffer_(sourceBuffer),
      sourceBufferSize_(sourceBufferSize),
      lexer_(source, sourceBuffer, sourceBufferSize),
      trailerArena_(trailerArena),
      trailerParser_(lexer_, trailerArena),
      table_(table),
      streamDecoder_(streamDecoder),
      decodeLimits_(decodeLimits) {
  decodeLimits_.maxExpandedBytes =
      std::min<uint64_t>(decodeLimits_.maxExpandedBytes, PdfLimits::MaxExpandedRequiredStreamBytes);
  decodeLimits_.maxExpansionRatio = std::min<uint16_t>(decodeLimits_.maxExpansionRatio, PdfLimits::MaxExpansionRatio);
}

uint64_t PdfXrefParser::currentDecodedBytes() const {
  if (phase_ == Phase::DecodeXrefStream && streamDecoder_ != nullptr) {
    return decodedBytes_ + streamDecoder_->outputBytes();
  }
  return decodedBytes_;
}

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
  decodedBytes_ = 0;
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
  hasPendingEntry_ = false;
  pendingEntryFromStream_ = false;
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
    if (hasPendingEntry_) {
      PdfXrefEntry entry{};
      if (pendingEntryFromStream_) {
        entry.objectNumber = xrefCurrentObject_;
        entry.type = static_cast<PdfXrefEntryType>(xrefFieldValues_[0]);
        entry.offset = xrefFieldValues_[1];
        if (entry.type == PdfXrefEntryType::Compressed) {
          entry.objectStreamIndex = static_cast<uint32_t>(xrefFieldValues_[2]);
        } else {
          entry.generation = static_cast<uint16_t>(xrefFieldValues_[2]);
        }
      } else {
        entry.objectNumber = subsectionStart_ + subsectionIndex_;
        entry.generation = entryGeneration_;
        entry.type = static_cast<PdfXrefEntryType>(xrefFieldValues_[0]);
        entry.offset = entryOffset_;
      }
      if (budget.cancelRequested()) {
        return fail(PdfStatus::failure(PdfError::Cancelled, entry.objectNumber));
      }
      if (budget.stopRequested() || budget.operationsRemaining == 0 || budget.bytesRemaining < sizeof(PdfXrefEntry)) {
        return PdfStepResult::paused();
      }
      --budget.operationsRemaining;
      budget.bytesRemaining -= sizeof(PdfXrefEntry);
      const PdfStatus appendStatus = table_.appendNewest(entry);
      if (!appendStatus) {
        return fail(appendStatus);
      }
      hasPendingEntry_ = false;
      if (pendingEntryFromStream_) {
        ++xrefDecodedEntries_;
        --xrefRangeRemaining_;
        ++xrefCurrentObject_;
        if (xrefRangeRemaining_ == 0 && xrefIndexPairsRemaining_ != 0) {
          const PdfStatus rangeStatus = advanceXrefRange();
          if (!rangeStatus) {
            return fail(rangeStatus);
          }
        }
        xrefFieldIndex_ = 0;
        xrefFieldByteIndex_ = 0;
        std::memset(xrefFieldValues_, 0, sizeof(xrefFieldValues_));
      } else {
        ++subsectionIndex_;
        phase_ = subsectionIndex_ == subsectionCount_ ? Phase::SectionStartOrTrailer : Phase::EntryOffset;
      }
      pendingEntryFromStream_ = false;
      continue;
    }

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
      if (!pdfCheckedRange(streamOffset, streamLength_, source_.size)) {
        return fail(PdfStatus::failure(PdfError::InvalidOffset, streamOffset));
      }
      const uint64_t streamEnd = streamOffset + streamLength_;
      if (source_.size - streamEnd < PdfMinimumStreamBoundaryBytes) {
        return fail(PdfStatus::failure(PdfError::Malformed, streamEnd));
      }
      const PdfStatus rangeStatus = pdfInitializeByteRange(source_, streamOffset, streamLength_, &xrefStreamRange_);
      if (!rangeStatus.ok()) {
        return fail(rangeStatus);
      }
      // These classic-xref counters are phase-disjoint while validating an
      // xref-stream boundary. Reuse them so the parser remains RV32-size
      // neutral and callers with even a one-byte lexer buffer stay safe.
      subsectionStart_ = 0;  // bounded lookahead bytes consumed
      subsectionCount_ = 0;  // boundary matcher state
      subsectionIndex_ = 0;  // keyword byte index
      eolOffset_ = streamEnd;
      eolRead_ = {eolOffset_, &eolByte_, 1, 0};
      phase_ = Phase::ValidateStreamBoundary;
      continue;
    }

    if (phase_ == Phase::ValidateStreamBoundary) {
      const PdfStepResult readResult = pdfStepReadExact(source_, eolRead_, budget);
      if (!readResult.complete()) {
        return readResult.failed() ? fail(readResult.status) : readResult;
      }
      ++subsectionStart_;
      ++eolOffset_;
      bool boundaryComplete = false;
      switch (subsectionCount_) {
        case 0:
          if (eolByte_ == '\n') {
            subsectionCount_ = 2;
          } else if (eolByte_ == '\r') {
            subsectionCount_ = 1;
          } else {
            return fail(PdfStatus::failure(PdfError::Malformed, eolOffset_ - 1));
          }
          break;
        case 1:
          if (eolByte_ != '\n') {
            return fail(PdfStatus::failure(PdfError::Malformed, eolOffset_ - 1));
          }
          subsectionCount_ = 2;
          break;
        case 2: {
          static constexpr char keyword[] = "endstream";
          if (subsectionIndex_ >= sizeof(keyword) - 1 || eolByte_ != keyword[subsectionIndex_]) {
            return fail(PdfStatus::failure(PdfError::Malformed, eolOffset_ - 1));
          }
          if (++subsectionIndex_ == sizeof(keyword) - 1) {
            subsectionCount_ = 3;
            subsectionIndex_ = 0;
          }
          break;
        }
        case 3:
          if (!pdfIsStreamBoundaryWhitespace(eolByte_)) {
            return fail(PdfStatus::failure(PdfError::Malformed, eolOffset_ - 1));
          }
          subsectionCount_ = 4;
          break;
        case 4:
          if (!pdfIsStreamBoundaryWhitespace(eolByte_)) {
            if (eolByte_ != 'e') {
              return fail(PdfStatus::failure(PdfError::Malformed, eolOffset_ - 1));
            }
            subsectionCount_ = 5;
            subsectionIndex_ = 1;
          }
          break;
        case 5: {
          static constexpr char keyword[] = "endobj";
          if (subsectionIndex_ >= sizeof(keyword) - 1 || eolByte_ != keyword[subsectionIndex_]) {
            return fail(PdfStatus::failure(PdfError::Malformed, eolOffset_ - 1));
          }
          if (++subsectionIndex_ == sizeof(keyword) - 1) {
            if (eolOffset_ == source_.size) {
              boundaryComplete = true;
            } else {
              subsectionCount_ = 6;
            }
          }
          break;
        }
        case 6:
          if (!pdfIsStreamBoundaryWhitespace(eolByte_)) {
            return fail(PdfStatus::failure(PdfError::Malformed, eolOffset_ - 1));
          }
          boundaryComplete = true;
          break;
        default:
          return fail(PdfStatus::failure(PdfError::Malformed, eolOffset_ - 1));
      }
      if (boundaryComplete) {
        const PdfStatus decoderStatus = beginXrefStreamDecode();
        if (!decoderStatus.ok()) {
          return fail(decoderStatus);
        }
        continue;
      }
      if (subsectionStart_ >= PdfStreamBoundaryLookaheadBytes || eolOffset_ >= source_.size) {
        return fail(PdfStatus::failure(PdfError::Malformed, eolOffset_));
      }
      eolRead_ = {eolOffset_, &eolByte_, 1, 0};
      continue;
    }

    if (phase_ == Phase::DecodeXrefStream) {
      // Each decoder sink operation may complete at most one xref record. Cap
      // this call to one decoder operation so a completed pending record is
      // charged by the parser before more decoded bytes are accepted.
      const uint32_t callerOperations = budget.operationsRemaining;
      const uint32_t decoderOperations = std::min<uint32_t>(callerOperations, 1U);
      budget.operationsRemaining = decoderOperations;
      const PdfStepResult decodeResult = streamDecoder_->step(budget);
      const uint32_t usedOperations = decoderOperations - budget.operationsRemaining;
      budget.operationsRemaining = callerOperations - usedOperations;
      if (!decodeResult.complete()) {
        if (decodeResult.failed()) {
          phase_ = Phase::Failed;
          return decodeResult;
        }
        // A short sink accept completed one xref record. Charge and append it
        // before letting the decoder retry the retained output suffix.
        if (hasPendingEntry_) {
          continue;
        }
        return decodeResult;
      }
      if (hasPendingEntry_) {
        continue;
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
        xrefFieldValues_[0] = static_cast<uint64_t>(entry.type);
        hasPendingEntry_ = true;
        pendingEntryFromStream_ = false;
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
  PdfObjectReference existingInfo;
  if (!table_.info(&existingInfo) && pdfDictionaryFind(trailerArena_, rootIndex, "Info", &valueIndex)) {
    const PdfValue& info = trailerArena_.values[valueIndex];
    if (info.kind != PdfValueKind::Reference) {
      return PdfStatus::failure(PdfError::Malformed, lexer_.tokenOffset());
    }
    table_.setInfo({info.objectNumber, info.generation});
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

PdfStatus PdfXrefParser::beginXrefStreamDecode() {
  const PdfByteSource streamSource = pdfByteRangeSource(xrefStreamRange_);
  const PdfByteSink streamSink{this, writeDecodedXref};
  if (streamDecoder_ == nullptr) {
    return PdfStatus::failure(PdfError::UnsupportedFilter, sectionOffset_);
  }
  if (decodeLimits_.maxExpandedBytes <= decodedBytes_) {
    return PdfStatus::failure(PdfError::ExpansionLimit, decodedBytes_);
  }
  PdfStreamDecodeLimits streamLimits = decodeLimits_;
  streamLimits.maxExpandedBytes -= decodedBytes_;
  const PdfStatus status =
      streamDecoder_->begin(streamSource, streamSink, streamFilters_, streamFilterCount_, streamLimits);
  if (status.ok()) {
    phase_ = Phase::DecodeXrefStream;
  }
  return status;
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
  if (hasPendingEntry_ || xrefDecodedEntries_ >= xrefExpectedEntries_ || xrefRangeRemaining_ == 0) {
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
  hasPendingEntry_ = true;
  pendingEntryFromStream_ = true;
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
    if (parser.hasPendingEntry_) {
      break;
    }
  }
  return PdfStatus::success();
}

PdfStatus PdfXrefParser::finishXrefStream() {
  if (hasPendingEntry_ || xrefDecodedEntries_ != xrefExpectedEntries_ || xrefFieldIndex_ != 0 ||
      xrefFieldByteIndex_ != 0 || xrefRangeRemaining_ != 0 || xrefIndexPairsRemaining_ != 0) {
    return PdfStatus::failure(PdfError::UnexpectedEof, sectionOffset_);
  }
  uint64_t completedBytes = 0;
  if (streamDecoder_ == nullptr || !pdfCheckedAdd(decodedBytes_, streamDecoder_->outputBytes(), &completedBytes) ||
      completedBytes > decodeLimits_.maxExpandedBytes) {
    return PdfStatus::failure(PdfError::ExpansionLimit, decodedBytes_);
  }
  decodedBytes_ = completedBytes;
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
