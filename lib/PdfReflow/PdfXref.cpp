#include "PdfXref.h"

#include <algorithm>
#include <cstring>
#include <limits>

#include "PdfCheckedMath.h"
#include "PdfLimits.h"

namespace {

constexpr size_t kNewestObjectSlotBytes = sizeof(uint32_t);
constexpr uint8_t kNewestObjectProbeLimit = 8;

bool tokenEquals(const PdfToken& token, const char* expected) {
  const size_t length = std::strlen(expected);
  return token.length == length && std::memcmp(token.bytes, expected, length) == 0;
}

bool isPdfWhitespace(const uint8_t byte) {
  return byte == 0 || byte == '\t' || byte == '\n' || byte == '\f' || byte == '\r' || byte == ' ';
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
  if (finalized_ || first == nullptr || firstBytes < kNewestObjectSlotBytes ||
      firstBytes % kNewestObjectSlotBytes != 0 || secondBytes % kNewestObjectSlotBytes != 0 ||
      (secondBytes != 0 && second == nullptr)) {
    return PdfStatus::failure(PdfError::InvalidArgument, firstBytes);
  }
  seenObjectsFirst_ = first;
  seenObjectsFirstBytes_ = firstBytes;
  seenObjectsSecond_ = secondBytes == 0 ? nullptr : second;
  seenObjectsSecondBytes_ = secondBytes;
  std::memset(seenObjectsFirst_, 0, seenObjectsFirstBytes_);
  if (seenObjectsSecond_ != nullptr) {
    std::memset(seenObjectsSecond_, 0, seenObjectsSecondBytes_);
  }
  // Existing compacted records are deliberately not reloaded into scarce RAM.
  // Older revisions may append, then the next boundary compacts exactly again.
  sectionCompactionRequired_ = entryCount_ != 0;
  newestObjectDense_ = false;
  return PdfStatus::success();
}

bool PdfXrefTable::newestObjectAlreadySeen(const uint32_t objectNumber) {
  const size_t totalBytes = seenObjectsFirstBytes_ + seenObjectsSecondBytes_;
  if (newestObjectDense_) {
    if (objectNumber >= totalBytes * 8U) {
      sectionCompactionRequired_ = true;
      return false;
    }
    const size_t byteOffset = objectNumber >> 3U;
    uint8_t* const destination = byteOffset < seenObjectsFirstBytes_
                                     ? seenObjectsFirst_ + byteOffset
                                     : seenObjectsSecond_ + (byteOffset - seenObjectsFirstBytes_);
    const uint8_t mask = static_cast<uint8_t>(1U << (objectNumber & 7U));
    const bool seen = (*destination & mask) != 0;
    *destination = static_cast<uint8_t>(*destination | mask);
    return seen;
  }
  const size_t slotCount = totalBytes / kNewestObjectSlotBytes;
  if (seenObjectsFirst_ == nullptr || slotCount == 0) {
    return false;
  }
  const uint32_t encoded = objectNumber + 1U;
  size_t slot = (objectNumber * 2654435761U) % slotCount;
  const size_t probes = std::min<size_t>(slotCount, kNewestObjectProbeLimit);
  for (size_t probe = 0; probe < probes; ++probe) {
    const size_t byteOffset = slot * kNewestObjectSlotBytes;
    uint8_t* const destination = byteOffset < seenObjectsFirstBytes_
                                     ? seenObjectsFirst_ + byteOffset
                                     : seenObjectsSecond_ + (byteOffset - seenObjectsFirstBytes_);
    uint32_t observed = 0;
    std::memcpy(&observed, destination, sizeof(observed));
    if (observed == encoded) {
      return true;
    }
    if (observed == 0) {
      std::memcpy(destination, &encoded, sizeof(encoded));
      return false;
    }
    slot = slot + 1U == slotCount ? 0 : slot + 1U;
  }
  // Keep accepting this section, but do not follow /Prev until an exact
  // external merge has removed any duplicates this bounded accelerator missed.
  sectionCompactionRequired_ = true;
  return false;
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
  security_ = {};
  hasRoot_ = false;
  hasInfo_ = false;
  hasSecurity_ = false;
  finalized_ = false;
  appendOrderStrict_ = true;
  lastAppendedObject_ = 0;
  secondSortedRunStart_ = 0;
  lookupWindowCount_ = 0;
  lookupWindowFirstOrdinal_ = 0;
  lastLookupOrdinal_ = 0;
  lookupWindowToken_ = 0;
  localityStreak_ = 0;
  sampleStride_ = 0;
  sampleCount_ = 0;
  sampleBuildCount_ = 0;
  lookupMissCount_ = 0;
  victimCount_ = 0;
  appendBatchCount_ = 0;
  sortedRunCount_ = 0;
  hasLastLookupOrdinal_ = false;
  sampleIndexReady_ = false;
  sampleIndexDisabled_ = false;
  sectionCompactionRequired_ = false;
  newestObjectDense_ = false;
  if (seenObjectsFirst_ != nullptr) {
    std::memset(seenObjectsFirst_, 0, seenObjectsFirstBytes_);
  }
  if (seenObjectsSecond_ != nullptr) {
    std::memset(seenObjectsSecond_, 0, seenObjectsSecondBytes_);
  }
}

PdfStatus PdfXrefTable::preflightAppend(const uint32_t count) const {
  if (count == 0 || !records_.valid() || records_.recordSize != sizeof(PdfXrefEntry)) {
    return PdfStatus::failure(PdfError::InvalidArgument, count);
  }
  const uint64_t requiredRecords = static_cast<uint64_t>(entryCount_) + count;
  if (requiredRecords > records_.capacity) {
    return PdfStatus::failure(PdfError::InsufficientStorage, requiredRecords * sizeof(PdfXrefEntry));
  }
  return PdfStatus::success();
}

void PdfXrefTable::prepareNewestObjectRange(const uint32_t firstObject, const uint32_t count) {
  const uint64_t end = static_cast<uint64_t>(firstObject) + count;
  const uint64_t bitCapacity = static_cast<uint64_t>(seenObjectsFirstBytes_ + seenObjectsSecondBytes_) * 8U;
  if (seenObjectsFirst_ != nullptr && end <= bitCapacity && (entryCount_ == 0 || newestObjectDense_)) {
    newestObjectDense_ = true;
  }
}

PdfStatus PdfXrefTable::preflightAppendRange(const uint32_t firstObject, const uint32_t count) const {
  if (!newestObjectDense_) {
    return preflightAppend(count);
  }
  const uint64_t end = static_cast<uint64_t>(firstObject) + count;
  const size_t totalBytes = seenObjectsFirstBytes_ + seenObjectsSecondBytes_;
  if (count == 0 || end > static_cast<uint64_t>(totalBytes) * 8U) {
    return preflightAppend(count);
  }
  uint32_t unseen = 0;
  for (uint32_t object = firstObject; object < end; ++object) {
    const size_t byteOffset = object >> 3U;
    const uint8_t* const source = byteOffset < seenObjectsFirstBytes_
                                      ? seenObjectsFirst_ + byteOffset
                                      : seenObjectsSecond_ + (byteOffset - seenObjectsFirstBytes_);
    unseen += (*source & static_cast<uint8_t>(1U << (object & 7U))) == 0 ? 1U : 0U;
  }
  return unseen == 0 ? PdfStatus::success() : preflightAppend(unseen);
}

void PdfXrefTable::initializeSampleIndex() {
  sampleStride_ = 0;
  sampleCount_ = 0;
  sampleBuildCount_ = 0;
  lookupMissCount_ = 0;
  sampleIndexReady_ = false;
  sampleIndexDisabled_ = entryCount_ < kSampleIndexMinimumRecords;
  if (sampleIndexDisabled_) {
    return;
  }
  sampleStride_ = (entryCount_ + kSampleIndexEntries - 1U) / kSampleIndexEntries;
  sampleCount_ = static_cast<uint8_t>((entryCount_ + sampleStride_ - 1U) / sampleStride_);
}

void PdfXrefTable::configureBinaryLookup(PdfXrefLookupState* const state) const {
  if (state == nullptr) {
    return;
  }
  state->first = 0;
  state->last = entryCount_;
  state->phase = PdfXrefLookupPhase::Binary;
  if (!sampleIndexReady_ || sampleCount_ == 0 || sampleStride_ == 0) {
    return;
  }
  uint8_t first = 0;
  uint8_t last = sampleCount_;
  while (first < last) {
    const uint8_t middle = static_cast<uint8_t>(first + (last - first) / 2U);
    if (sampleObjectNumbers_[middle] <= state->objectNumber) {
      first = static_cast<uint8_t>(middle + 1U);
    } else {
      last = middle;
    }
  }
  const uint8_t sampleIndex = first == 0 ? 0 : static_cast<uint8_t>(first - 1U);
  state->first = static_cast<uint32_t>(sampleIndex) * sampleStride_;
  state->last = std::min<uint32_t>(entryCount_, state->first + sampleStride_);
}

void PdfXrefTable::rememberVictim(const PdfXrefEntry& entry, const uint32_t ordinal) const {
  uint8_t existing = victimCount_;
  for (uint8_t index = 0; index < victimCount_; ++index) {
    if (victimEntries()[index].objectNumber == entry.objectNumber) {
      existing = index;
      break;
    }
  }
  const uint8_t destinationCount =
      existing < victimCount_ ? victimCount_ : std::min<uint8_t>(kVictimEntries, victimCount_ + 1U);
  const uint8_t shiftFrom = existing < victimCount_ ? existing : static_cast<uint8_t>(destinationCount - 1U);
  for (uint8_t index = shiftFrom; index != 0; --index) {
    victimEntries()[index] = victimEntries()[index - 1U];
    victimOrdinals_[index] = victimOrdinals_[index - 1U];
  }
  victimEntries()[0] = entry;
  victimOrdinals_[0] = ordinal;
  victimCount_ = destinationCount;
}

PdfStatus PdfXrefTable::appendNewest(const PdfXrefEntry& entry) {
  if (finalized_ || !records_.valid() || records_.recordSize != sizeof(PdfXrefEntry) ||
      entry.objectNumber > PdfLimits::MaxIndirectObjectNumber) {
    return PdfStatus::failure(PdfError::InvalidArgument, entry.objectNumber);
  }
  if (newestObjectAlreadySeen(entry.objectNumber)) {
    return PdfStatus::success();
  }
  if (entryCount_ >= records_.capacity) {
    return PdfStatus::failure(PdfError::InsufficientStorage,
                              (static_cast<uint64_t>(entryCount_) + 1U) * sizeof(PdfXrefEntry));
  }
  if (entryCount_ == 0) {
    sortedRunCount_ = 1;
  } else if (entry.objectNumber <= lastAppendedObject_) {
    appendOrderStrict_ = false;
    if (sortedRunCount_ < UINT8_MAX) {
      ++sortedRunCount_;
    }
    if (sortedRunCount_ == 2) {
      secondSortedRunStart_ = entryCount_;
    }
  }
  lastAppendedObject_ = entry.objectNumber;
  if (records_.writeMany == nullptr) {
    const PdfStatus status = pdfWriteRecord(records_, entryCount_, &entry);
    if (status) {
      ++entryCount_;
    }
    return status;
  }
  entryStorage_[appendBatchCount_++] = entry;
  ++entryCount_;
  return appendBatchCount_ == kAppendBatchEntries ? flushPendingWrites() : PdfStatus::success();
}

PdfStatus PdfXrefTable::flushPendingWrites() {
  if (appendBatchCount_ == 0) {
    return PdfStatus::success();
  }
  if (!records_.valid() || records_.recordSize != sizeof(PdfXrefEntry) ||
      appendBatchCount_ > entryCount_) {
    return PdfStatus::failure(PdfError::InvalidArgument, appendBatchCount_);
  }
  const uint32_t first = entryCount_ - appendBatchCount_;
  const PdfStatus status = pdfWriteRecords(records_, first, entryStorage_, appendBatchCount_);
  if (status) {
    appendBatchCount_ = 0;
  }
  return status;
}

PdfStatus PdfXrefTable::adoptCompactedRecords(const PdfFixedRecordStore& records, const uint32_t count,
                                              const uint32_t lastObjectNumber) {
  if (finalized_ || count == 0 || !records.valid() || records.recordSize != sizeof(PdfXrefEntry) ||
      count > records.capacity || lastObjectNumber > PdfLimits::MaxIndirectObjectNumber) {
    return PdfStatus::failure(PdfError::InvalidArgument, count);
  }
  records_ = records;
  entryCount_ = count;
  appendOrderStrict_ = true;
  lastAppendedObject_ = lastObjectNumber;
  secondSortedRunStart_ = 0;
  lookupWindowCount_ = 0;
  victimCount_ = 0;
  appendBatchCount_ = 0;
  sortedRunCount_ = 1;
  sampleIndexReady_ = false;
  sampleIndexDisabled_ = false;
  sectionCompactionRequired_ = false;
  newestObjectDense_ = false;
  return PdfStatus::success();
}

PdfStatus PdfXrefTable::adoptSortedRecords(const uint32_t count) {
  if (finalized_ || entryCount_ != 0 || appendBatchCount_ != 0 || count == 0 || !records_.valid() ||
      records_.recordSize != sizeof(PdfXrefEntry) || count > records_.capacity) {
    return PdfStatus::failure(PdfError::InvalidArgument, count);
  }
  entryCount_ = count;
  finalized_ = true;
  appendOrderStrict_ = true;
  secondSortedRunStart_ = 0;
  sortedRunCount_ = 1;
  initializeSampleIndex();
  return PdfStatus::success();
}

bool PdfXrefTable::recordsAreTwoSortedRuns(uint32_t* const secondRunStart) const {
  if (secondRunStart == nullptr || appendOrderStrict_ || sortedRunCount_ != 2 ||
      secondSortedRunStart_ == 0 || secondSortedRunStart_ >= entryCount_) {
    return false;
  }
  *secondRunStart = secondSortedRunStart_;
  return true;
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
  const PdfStatus flushStatus = flushPendingWrites();
  if (!flushStatus) {
    return flushStatus;
  }
  if (entryCount_ == 0) {
    finalized_ = true;
    initializeSampleIndex();
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
  initializeSampleIndex();
  return PdfStatus::success();
}

PdfStatus PdfXrefTable::beginFind(const uint32_t objectNumber, PdfXrefLookupState* const state) const {
  if (state == nullptr || !records_.valid() || records_.recordSize != sizeof(PdfXrefEntry) ||
      objectNumber > PdfLimits::MaxIndirectObjectNumber) {
    return PdfStatus::failure(PdfError::InvalidArgument, objectNumber);
  }
  *state = {};
  state->objectNumber = objectNumber;
  for (uint8_t index = 0; index < lookupWindowCount_; ++index) {
    if (lookupWindowEntries()[index].objectNumber == objectNumber) {
      state->entry = lookupWindowEntries()[index];
      state->phase = PdfXrefLookupPhase::Done;
      lastLookupOrdinal_ = lookupWindowFirstOrdinal_ + index;
      hasLastLookupOrdinal_ = true;
      localityStreak_ = kLocalityStreakRequired;
      rememberVictim(state->entry, lastLookupOrdinal_);
      return PdfStatus::success();
    }
  }
  for (uint8_t index = 0; index < victimCount_; ++index) {
    if (victimEntries()[index].objectNumber == objectNumber) {
      state->entry = victimEntries()[index];
      state->phase = PdfXrefLookupPhase::Done;
      lastLookupOrdinal_ = victimOrdinals_[index];
      hasLastLookupOrdinal_ = true;
      localityStreak_ = kLocalityStreakRequired;
      rememberVictim(state->entry, lastLookupOrdinal_);
      return PdfStatus::success();
    }
  }
  if (!finalized_) {
    state->last = entryCount_;
    state->phase = PdfXrefLookupPhase::Linear;
  } else if (!sampleIndexReady_ && !sampleIndexDisabled_ &&
             lookupMissCount_ >= kSampleIndexLookupThreshold) {
    state->phase = PdfXrefLookupPhase::BuildSampleIndex;
  } else {
    configureBinaryLookup(state);
  }
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
    if (state.phase == PdfXrefLookupPhase::BuildSampleIndex) {
      if (sampleIndexReady_ || sampleIndexDisabled_) {
        configureBinaryLookup(&state);
        continue;
      }
      if (sampleBuildCount_ >= sampleCount_) {
        sampleIndexReady_ = true;
        configureBinaryLookup(&state);
        continue;
      }
      if (budget.cancelRequested()) {
        state.status = PdfStatus::failure(PdfError::Cancelled, state.objectNumber);
        state.phase = PdfXrefLookupPhase::Failed;
        return PdfStepResult::failure(state.status);
      }
      if (budget.stopRequested() || budget.operationsRemaining == 0 ||
          budget.bytesRemaining < sizeof(PdfXrefEntry)) {
        return PdfStepResult::paused();
      }
      --budget.operationsRemaining;
      budget.bytesRemaining -= sizeof(PdfXrefEntry);
      const uint32_t ordinal = static_cast<uint32_t>(sampleBuildCount_) * sampleStride_;
      const PdfStatus sampleStatus = pdfReadRecord(records_, ordinal, &state.entry);
      if (!sampleStatus ||
          (sampleBuildCount_ != 0 && state.entry.objectNumber <= sampleObjectNumbers_[sampleBuildCount_ - 1U])) {
        sampleIndexDisabled_ = true;
        configureBinaryLookup(&state);
        continue;
      }
      sampleObjectNumbers_[sampleBuildCount_] = state.entry.objectNumber;
      ++sampleBuildCount_;
      continue;
    }

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
      lookupMissCount_ = std::min<uint8_t>(kSampleIndexLookupThreshold,
                                           static_cast<uint8_t>(lookupMissCount_ + 1U));
      rememberVictim(state.entry, state.first);
      const uint32_t ordinalDistance = hasLastLookupOrdinal_
                                           ? (state.first > lastLookupOrdinal_ ? state.first - lastLookupOrdinal_
                                                                              : lastLookupOrdinal_ - state.first)
                                           : UINT32_MAX;
      const bool nearbyForward = hasLastLookupOrdinal_ && state.first > lastLookupOrdinal_ &&
                                 ordinalDistance <= kLookupWindowEntries;
      if (nearbyForward) {
        localityStreak_ =
            std::min<uint8_t>(kLocalityStreakRequired, static_cast<uint8_t>(localityStreak_ + 1U));
      } else {
        localityStreak_ = 0;
      }
      const bool readAhead = finalized_ && nearbyForward && localityStreak_ >= kLocalityStreakRequired;
      if (readAhead) {
        localityStreak_ = 0;
      }
      lastLookupOrdinal_ = state.first;
      hasLastLookupOrdinal_ = true;
      lookupWindowCount_ = 0;
      lookupWindowFirstOrdinal_ = state.first;
      lookupWindowEntries()[0] = state.entry;
      ++lookupWindowToken_;
      if (lookupWindowToken_ == 0) {
        ++lookupWindowToken_;
      }
      state.readAheadToken = lookupWindowToken_;
      state.cursor = 1;
      state.last = readAhead ? std::min<uint32_t>(kLookupWindowEntries, entryCount_ - state.first) : 1U;
      state.phase = PdfXrefLookupPhase::ReadAhead;
      continue;
    }

    if (state.phase == PdfXrefLookupPhase::ReadAhead) {
      if (state.readAheadToken != lookupWindowToken_) {
        state.phase = PdfXrefLookupPhase::Done;
        *entry = state.entry;
        return PdfStepResult::completed();
      }
      if (state.cursor >= state.last) {
        lookupWindowCount_ = static_cast<uint8_t>(state.last);
        state.phase = PdfXrefLookupPhase::Done;
        *entry = state.entry;
        return PdfStepResult::completed();
      }
      if (budget.cancelRequested()) {
        lookupWindowCount_ = 0;
        state.phase = PdfXrefLookupPhase::Done;
        *entry = state.entry;
        return PdfStepResult::completed();
      }
      if (budget.stopRequested() || budget.operationsRemaining == 0 ||
          budget.bytesRemaining < sizeof(PdfXrefEntry)) {
        return PdfStepResult::paused();
      }
      --budget.operationsRemaining;
      budget.bytesRemaining -= sizeof(PdfXrefEntry);
      if (!pdfReadRecord(records_, lookupWindowFirstOrdinal_ + state.cursor,
                         &lookupWindowEntries()[state.cursor]).ok()) {
        lookupWindowCount_ = 0;
        state.phase = PdfXrefLookupPhase::Done;
        *entry = state.entry;
        return PdfStepResult::completed();
      }
      ++state.cursor;
      continue;
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

PdfStatus PdfXrefTable::setSecurity(const PdfSecurityTrailer& security) {
  if (!security.encrypted || security.encryptionReference.objectNumber == 0 ||
      security.fileIdentifierLength == 0 ||
      security.fileIdentifierLength > sizeof(security.fileIdentifier)) {
    return PdfStatus::failure(PdfError::Encrypted, security.encryptionReference.objectNumber);
  }
  if (hasSecurity_) {
    return security_.encryptionReference == security.encryptionReference &&
                   security_.fileIdentifierLength == security.fileIdentifierLength &&
                   std::memcmp(security_.fileIdentifier, security.fileIdentifier,
                               security.fileIdentifierLength) == 0
               ? PdfStatus::success()
               : PdfStatus::failure(PdfError::Encrypted, security.encryptionReference.objectNumber);
  }
  security_ = security;
  hasSecurity_ = true;
  return PdfStatus::success();
}

bool PdfXrefTable::security(PdfSecurityTrailer* security) const {
  if (!hasSecurity_ || security == nullptr) {
    return false;
  }
  *security = security_;
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
      xrefIndexSink_{"Index", 5, 1, this, consumeNamedIndex},
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
  prevCycleAnchor_ = 0;
  prevCyclePower_ = 0;
  prevCycleLength_ = 0;
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
  xrefIndexStartOffset_ = 0;
  xrefIndexEndOffset_ = 0;
  xrefIndexReadOffset_ = 0;
  xrefIndexNumber_ = 0;
  xrefExpectedEntries_ = 0;
  xrefDecodedEntries_ = 0;
  xrefCurrentObject_ = 0;
  xrefRangeRemaining_ = 0;
  xrefIndexObservedFirst_ = 0;
  xrefIndexPairsRemaining_ = 0;
  entryGeneration_ = 0;
  eolByte_ = 0;
  xrefFieldIndex_ = 0;
  xrefFieldByteIndex_ = 0;
  xrefIndexBufferPosition_ = 0;
  xrefIndexBufferLength_ = 0;
  streamFilterCount_ = 0;
  streamDecodeParameters_ = {};
  hasPendingPrev_ = false;
  hasPrevCycleAnchor_ = false;
  hasPendingEntry_ = false;
  pendingEntryFromStream_ = false;
  xrefUsesDefaultIndex_ = false;
  xrefIndexSeen_ = false;
  xrefIndexHaveObservedFirst_ = false;
  xrefIndexNeedRange_ = false;
  xrefIndexScanHaveFirst_ = false;
  xrefIndexNumberStarted_ = false;
  xrefIndexNumberHasDigits_ = false;
  xrefIndexInComment_ = false;
  std::memset(xrefFieldValues_, 0, sizeof(xrefFieldValues_));
  std::memset(xrefWidths_, 0, sizeof(xrefWidths_));
  const size_t tailLength = source_.size > sourceBufferSize_ ? sourceBufferSize_ : static_cast<size_t>(source_.size);
  tailRead_ = {source_.size - tailLength, sourceBuffer_, tailLength, 0};
}

PdfStepResult PdfXrefParser::step(PdfWorkBudget& budget) {
  if (phase_ == Phase::Done) {
    return PdfStepResult::completed();
  }
  if (phase_ == Phase::AwaitCompaction) {
    return PdfStepResult::paused();
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
          xrefIndexNeedRange_ = true;
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

    if (xrefIndexNeedRange_) {
      const PdfStepResult rangeResult = stepAdvanceXrefRange(budget);
      if (!rangeResult.complete()) {
        return rangeResult.failed() ? fail(rangeResult.status) : rangeResult;
      }
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
        if (phase_ == Phase::AwaitCompaction) {
          return PdfStepResult::paused();
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
      if (phase_ == Phase::AwaitCompaction) {
        return PdfStepResult::paused();
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
        if (!parseUnsigned(token, &streamObjectNumber_) ||
            streamObjectNumber_ > PdfLimits::MaxIndirectObjectNumber) {
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
        xrefIndexStartOffset_ = 0;
        xrefIndexEndOffset_ = 0;
        xrefIndexObservedFirst_ = 0;
        xrefExpectedEntries_ = 0;
        xrefIndexPairsRemaining_ = 0;
        xrefIndexSeen_ = false;
        xrefIndexHaveObservedFirst_ = false;
        trailerParser_.setNamedIntegerArraySink(&xrefIndexSink_);
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
          trailerParser_.setNamedIntegerArraySink(nullptr);
          trailerParser_.begin();
          phase_ = Phase::ParseTrailer;
          break;
        }
        if (!parseUnsigned(token, &value) || value > PdfLimits::MaxIndirectObjectNumber) {
          return fail(PdfStatus::failure(PdfError::Malformed, lexer_.tokenOffset()));
        }
        subsectionStart_ = static_cast<uint32_t>(value);
        phase_ = Phase::SectionCount;
        break;

      case Phase::SectionCount: {
        if (!parseUnsigned(token, &value) || value == 0 || value > PdfLimits::MaxXrefRecords) {
          return fail(PdfStatus::failure(PdfError::LimitExceeded, lexer_.tokenOffset()));
        }
        uint64_t end = 0;
        if (!pdfCheckedAdd(subsectionStart_, value, &end) ||
            end > PdfLimits::MaxIndirectObjectNumber + 1ULL) {
          return fail(PdfStatus::failure(PdfError::LimitExceeded, lexer_.tokenOffset()));
        }
        table_.prepareNewestObjectRange(subsectionStart_, static_cast<uint32_t>(value));
        const PdfStatus storageStatus =
            table_.preflightAppendRange(subsectionStart_, static_cast<uint32_t>(value));
        if (!storageStatus.ok()) {
          return fail(storageStatus);
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
  if (!hasPrevCycleAnchor_) {
    prevCycleAnchor_ = offset;
    prevCyclePower_ = 1;
    prevCycleLength_ = 0;
    hasPrevCycleAnchor_ = true;
  } else {
    if (prevCycleLength_ == UINT32_MAX) {
      return PdfStatus::failure(PdfError::LimitExceeded, offset);
    }
    ++prevCycleLength_;
    if (offset == prevCycleAnchor_) {
      return PdfStatus::failure(PdfError::Malformed, offset);
    }
    if (prevCycleLength_ == prevCyclePower_) {
      prevCycleAnchor_ = offset;
      prevCyclePower_ = prevCyclePower_ > UINT32_MAX / 2U ? UINT32_MAX : prevCyclePower_ * 2U;
      prevCycleLength_ = 0;
    }
  }
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
  return finishSection();
}

PdfStatus PdfXrefParser::finishSection() {
  if (hasPendingPrev_) {
    if (table_.sectionCompactionRequired()) {
      phase_ = Phase::AwaitCompaction;
      return PdfStatus::success();
    }
    return enterSection(pendingPrev_);
  }
  PdfObjectReference existingRoot;
  if (!table_.root(&existingRoot)) {
    return PdfStatus::failure(PdfError::Malformed, lexer_.tokenOffset());
  }
  phase_ = Phase::Done;
  return PdfStatus::success();
}

PdfStatus PdfXrefParser::resumeAfterCompaction() {
  if (phase_ != Phase::AwaitCompaction || !hasPendingPrev_) {
    return PdfStatus::failure(PdfError::InvalidArgument, pendingPrev_);
  }
  return enterSection(pendingPrev_);
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
    if (valueIndex >= trailerArena_.valueCount ||
        trailerArena_.values[valueIndex].kind != PdfValueKind::Reference) {
      return PdfStatus::failure(PdfError::Encrypted, lexer_.tokenOffset());
    }
    uint16_t identifiersIndex = PDF_INVALID_INDEX;
    uint16_t firstIdentifierIndex = PDF_INVALID_INDEX;
    if (!pdfDictionaryFind(trailerArena_, rootIndex, "ID", &identifiersIndex) ||
        identifiersIndex >= trailerArena_.valueCount ||
        trailerArena_.values[identifiersIndex].kind != PdfValueKind::Array ||
        trailerArena_.values[identifiersIndex].count < 1 ||
        !pdfArrayAt(trailerArena_, identifiersIndex, 0, &firstIdentifierIndex) ||
        firstIdentifierIndex >= trailerArena_.valueCount ||
        trailerArena_.values[firstIdentifierIndex].kind != PdfValueKind::String ||
        trailerArena_.values[firstIdentifierIndex].textLength == 0 ||
        trailerArena_.values[firstIdentifierIndex].textLength > PdfSecurityTrailer::FileIdentifierCapacity ||
        static_cast<uint32_t>(trailerArena_.values[firstIdentifierIndex].textOffset) +
                trailerArena_.values[firstIdentifierIndex].textLength >
            trailerArena_.textLength) {
      return PdfStatus::failure(PdfError::Encrypted, lexer_.tokenOffset());
    }
    PdfSecurityTrailer security{};
    security.encrypted = true;
    security.encryptionReference = {trailerArena_.values[valueIndex].objectNumber,
                                    trailerArena_.values[valueIndex].generation};
    security.fileIdentifierLength =
        static_cast<uint8_t>(trailerArena_.values[firstIdentifierIndex].textLength);
    std::memcpy(security.fileIdentifier,
                trailerArena_.text + trailerArena_.values[firstIdentifierIndex].textOffset,
                security.fileIdentifierLength);
    const PdfStatus securityStatus = table_.setSecurity(security);
    if (!securityStatus) {
      return securityStatus;
    }
  }
  if (pdfDictionaryFind(trailerArena_, rootIndex, "Size", &valueIndex)) {
    const PdfValue& size = trailerArena_.values[valueIndex];
    if (size.kind != PdfValueKind::Integer || size.integerValue < 1 ||
        static_cast<uint64_t>(size.integerValue) > PdfLimits::MaxIndirectObjectNumber + 1ULL) {
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

PdfStatus PdfXrefParser::consumeNamedIndex(void* const context, const PdfNamedIntegerArrayEvent event,
                                           const int64_t value, const uint64_t sourceOffset) {
  if (context == nullptr) {
    return PdfStatus::failure(PdfError::InvalidArgument, sourceOffset);
  }
  auto& parser = *static_cast<PdfXrefParser*>(context);
  if (event == PdfNamedIntegerArrayEvent::Begin) {
    if (parser.xrefIndexSeen_) {
      return PdfStatus::failure(PdfError::Malformed, sourceOffset);
    }
    parser.xrefIndexSeen_ = true;
    parser.xrefIndexStartOffset_ = sourceOffset;
    return PdfStatus::success();
  }
  if (!parser.xrefIndexSeen_) {
    return PdfStatus::failure(PdfError::Malformed, sourceOffset);
  }
  if (event == PdfNamedIntegerArrayEvent::End) {
    if (parser.xrefIndexHaveObservedFirst_ || parser.xrefIndexPairsRemaining_ == 0 ||
        sourceOffset < parser.xrefIndexStartOffset_) {
      return PdfStatus::failure(PdfError::Malformed, sourceOffset);
    }
    parser.xrefIndexEndOffset_ = sourceOffset;
    return PdfStatus::success();
  }
  if (value < 0) {
    return PdfStatus::failure(PdfError::Malformed, sourceOffset);
  }
  if (!parser.xrefIndexHaveObservedFirst_) {
    if (static_cast<uint64_t>(value) > PdfLimits::MaxIndirectObjectNumber) {
      return PdfStatus::failure(PdfError::LimitExceeded, sourceOffset);
    }
    parser.xrefIndexObservedFirst_ = static_cast<uint32_t>(value);
    parser.xrefIndexHaveObservedFirst_ = true;
    return PdfStatus::success();
  }
  if (value == 0) {
    return PdfStatus::failure(PdfError::Malformed, sourceOffset);
  }
  uint64_t rangeEnd = 0;
  uint64_t expectedEntries = 0;
  if (!pdfCheckedAdd(parser.xrefIndexObservedFirst_, static_cast<uint64_t>(value), &rangeEnd) ||
      rangeEnd > PdfLimits::MaxIndirectObjectNumber + 1ULL ||
      !pdfCheckedAdd(parser.xrefExpectedEntries_, static_cast<uint64_t>(value), &expectedEntries) ||
      expectedEntries > PdfLimits::MaxXrefRecords || parser.xrefIndexPairsRemaining_ == UINT32_MAX) {
    return PdfStatus::failure(PdfError::LimitExceeded, sourceOffset);
  }
  parser.xrefExpectedEntries_ = static_cast<uint32_t>(expectedEntries);
  ++parser.xrefIndexPairsRemaining_;
  parser.xrefIndexHaveObservedFirst_ = false;
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
  uint64_t totalWidth = 0;
  for (uint16_t ordinal = 0; ordinal < 3; ++ordinal) {
    uint16_t itemIndex = PDF_INVALID_INDEX;
    if (!pdfArrayAt(trailerArena_, valueIndex, ordinal, &itemIndex) || itemIndex >= trailerArena_.valueCount) {
      return PdfStatus::failure(PdfError::Malformed, sectionOffset_);
    }
    const PdfValue& width = trailerArena_.values[itemIndex];
    if (width.kind != PdfValueKind::Integer || width.integerValue < 0) {
      return PdfStatus::failure(PdfError::Malformed, sectionOffset_);
    }
    xrefWidths_[ordinal] = static_cast<uint64_t>(width.integerValue);
    if (!pdfCheckedAdd(totalWidth, xrefWidths_[ordinal], &totalWidth)) {
      return PdfStatus::failure(PdfError::LimitExceeded, sectionOffset_);
    }
  }
  if (totalWidth == 0) {
    return PdfStatus::failure(PdfError::Malformed, sectionOffset_);
  }

  uint16_t sizeIndex = PDF_INVALID_INDEX;
  if (!pdfDictionaryFind(trailerArena_, rootIndex, "Size", &sizeIndex) || sizeIndex >= trailerArena_.valueCount) {
    return PdfStatus::failure(PdfError::Malformed, sectionOffset_);
  }
  const PdfValue& size = trailerArena_.values[sizeIndex];
  if (size.kind != PdfValueKind::Integer || size.integerValue < 1 ||
      static_cast<uint64_t>(size.integerValue) > PdfLimits::MaxIndirectObjectNumber + 1ULL) {
    return PdfStatus::failure(PdfError::LimitExceeded, sectionOffset_);
  }

  xrefDecodedEntries_ = 0;
  xrefFieldIndex_ = 0;
  xrefFieldByteIndex_ = 0;
  std::memset(xrefFieldValues_, 0, sizeof(xrefFieldValues_));
  xrefUsesDefaultIndex_ = true;
  if (pdfDictionaryFind(trailerArena_, rootIndex, "Index", &valueIndex)) {
    if (!xrefIndexSeen_ || valueIndex >= trailerArena_.valueCount ||
        trailerArena_.values[valueIndex].kind != PdfValueKind::Array || xrefIndexPairsRemaining_ == 0 ||
        xrefIndexEndOffset_ <= xrefIndexStartOffset_) {
      return PdfStatus::failure(PdfError::Malformed, sectionOffset_);
    }
    xrefUsesDefaultIndex_ = false;
    xrefIndexReadOffset_ = xrefIndexStartOffset_;
    xrefIndexBufferPosition_ = 0;
    xrefIndexBufferLength_ = 0;
    xrefIndexNumber_ = 0;
    xrefIndexScanHaveFirst_ = false;
    xrefIndexNumberStarted_ = false;
    xrefIndexNumberHasDigits_ = false;
    xrefIndexInComment_ = false;
    xrefIndexNeedRange_ = true;
  } else {
    if (xrefIndexSeen_) {
      return PdfStatus::failure(PdfError::Malformed, sectionOffset_);
    }
    if (static_cast<uint64_t>(size.integerValue) > PdfLimits::MaxXrefRecords) {
      return PdfStatus::failure(PdfError::LimitExceeded, sectionOffset_);
    }
    xrefExpectedEntries_ = static_cast<uint32_t>(size.integerValue);
    xrefCurrentObject_ = 0;
    xrefRangeRemaining_ = xrefExpectedEntries_;
  }

  if (!xrefIndexSeen_) {
    table_.prepareNewestObjectRange(0, xrefExpectedEntries_);
  }
  const PdfStatus storageStatus = !xrefIndexSeen_ ? table_.preflightAppendRange(0, xrefExpectedEntries_)
                                                  : table_.preflightAppend(xrefExpectedEntries_);
  if (!storageStatus.ok()) {
    return storageStatus;
  }

  const PdfStatus filterStatus =
      pdfStreamFiltersFromDictionary(trailerArena_, rootIndex, streamFilters_, PdfLimits::MaxFiltersPerStream,
                                     &streamFilterCount_, &streamDecodeParameters_);
  if (!filterStatus.ok()) {
    return filterStatus;
  }
  if (streamDecodeParameters_.predictor != 1U && streamDecodeParameters_.columns != totalWidth) {
    return PdfStatus::failure(PdfError::Malformed, sectionOffset_);
  }
  return PdfStatus::success();
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
  const PdfStatus status = streamDecoder_->begin(streamSource, streamSink, streamFilters_, streamFilterCount_,
                                                 streamLimits, true, &streamDecodeParameters_);
  if (status.ok()) {
    phase_ = Phase::DecodeXrefStream;
  }
  return status;
}

PdfStepResult PdfXrefParser::stepAdvanceXrefRange(PdfWorkBudget& budget) {
  while (true) {
    if (budget.cancelRequested()) {
      return PdfStepResult::failure(PdfStatus::failure(PdfError::Cancelled, xrefIndexReadOffset_));
    }
    if (budget.stopRequested()) {
      return PdfStepResult::paused();
    }
    bool numberComplete = false;
    if (xrefIndexBufferPosition_ == xrefIndexBufferLength_) {
      if (xrefIndexReadOffset_ >= xrefIndexEndOffset_) {
        if (!xrefIndexNumberHasDigits_) {
          return PdfStepResult::failure(PdfStatus::failure(PdfError::UnexpectedEof, xrefIndexReadOffset_));
        }
        numberComplete = true;
      } else {
        if (!budget.consumeOperation()) {
          return PdfStepResult::paused();
        }
        const uint64_t remaining64 = xrefIndexEndOffset_ - xrefIndexReadOffset_;
        const size_t remaining = static_cast<size_t>(std::min<uint64_t>(remaining64, sizeof(xrefIndexBuffer_)));
        const size_t requested = budget.takeBytes(remaining);
        if (requested == 0) {
          return PdfStepResult::paused();
        }
        size_t bytesRead = 0;
        const PdfStatus status = source_.readAt(source_.context, xrefIndexReadOffset_, xrefIndexBuffer_, requested,
                                                &bytesRead);
        if (!status.ok()) {
          return PdfStepResult::failure(status);
        }
        if (bytesRead == 0 || bytesRead > requested) {
          return PdfStepResult::failure(PdfStatus::failure(
              bytesRead == 0 ? PdfError::UnexpectedEof : PdfError::IoFailure, xrefIndexReadOffset_));
        }
        xrefIndexReadOffset_ += bytesRead;
        xrefIndexBufferPosition_ = 0;
        xrefIndexBufferLength_ = static_cast<uint8_t>(bytesRead);
      }
    }

    if (!numberComplete) {
      const uint8_t byte = xrefIndexBuffer_[xrefIndexBufferPosition_++];
      if (xrefIndexInComment_) {
        if (byte == '\r' || byte == '\n') {
          xrefIndexInComment_ = false;
        }
        continue;
      }
      if (!xrefIndexNumberStarted_) {
        if (isPdfWhitespace(byte)) {
          continue;
        }
        if (byte == '%') {
          xrefIndexInComment_ = true;
          continue;
        }
        if (byte == '+') {
          xrefIndexNumberStarted_ = true;
          continue;
        }
        if (byte < '0' || byte > '9') {
          return PdfStepResult::failure(PdfStatus::failure(PdfError::Malformed, xrefIndexReadOffset_));
        }
        xrefIndexNumberStarted_ = true;
      } else if (byte < '0' || byte > '9') {
        if (!xrefIndexNumberHasDigits_ || (!isPdfWhitespace(byte) && byte != '%')) {
          return PdfStepResult::failure(PdfStatus::failure(PdfError::Malformed, xrefIndexReadOffset_));
        }
        xrefIndexInComment_ = byte == '%';
        numberComplete = true;
      }

      if (!numberComplete && byte >= '0' && byte <= '9') {
        const uint8_t digit = static_cast<uint8_t>(byte - '0');
        if (xrefIndexNumber_ > (std::numeric_limits<uint64_t>::max() - digit) / 10U) {
          return PdfStepResult::failure(PdfStatus::failure(PdfError::LimitExceeded, xrefIndexReadOffset_));
        }
        xrefIndexNumber_ = xrefIndexNumber_ * 10U + digit;
        xrefIndexNumberHasDigits_ = true;
      }
    }

    if (!numberComplete) {
      continue;
    }
    const uint64_t number = xrefIndexNumber_;
    xrefIndexNumber_ = 0;
    xrefIndexNumberStarted_ = false;
    xrefIndexNumberHasDigits_ = false;
    if (!xrefIndexScanHaveFirst_) {
      if (number > PdfLimits::MaxIndirectObjectNumber) {
        return PdfStepResult::failure(PdfStatus::failure(PdfError::LimitExceeded, xrefIndexReadOffset_));
      }
      xrefCurrentObject_ = static_cast<uint32_t>(number);
      xrefIndexScanHaveFirst_ = true;
      continue;
    }
    uint64_t rangeEnd = 0;
    if (number == 0 || number > UINT32_MAX ||
        !pdfCheckedAdd(xrefCurrentObject_, number, &rangeEnd) ||
        rangeEnd > PdfLimits::MaxIndirectObjectNumber + 1ULL || xrefIndexPairsRemaining_ == 0) {
      return PdfStepResult::failure(PdfStatus::failure(PdfError::LimitExceeded, xrefIndexReadOffset_));
    }
    xrefRangeRemaining_ = static_cast<uint32_t>(number);
    --xrefIndexPairsRemaining_;
    xrefIndexScanHaveFirst_ = false;
    xrefIndexNeedRange_ = false;
    return PdfStepResult::completed();
  }
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
  const uint64_t retainedOffset =
      xrefWidths_[xrefFieldIndex_] > sizeof(uint64_t) ? xrefWidths_[xrefFieldIndex_] - sizeof(uint64_t) : 0;
  if (xrefFieldByteIndex_ < retainedOffset) {
    if (byte != 0) {
      return PdfStatus::failure(PdfError::LimitExceeded, sectionOffset_);
    }
  } else {
    xrefFieldValues_[xrefFieldIndex_] = (xrefFieldValues_[xrefFieldIndex_] << 8) | byte;
  }
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
    if (xrefFieldValues_[1] > PdfLimits::MaxIndirectObjectNumber || xrefFieldValues_[2] > UINT32_MAX) {
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
  return finishSection();
}
