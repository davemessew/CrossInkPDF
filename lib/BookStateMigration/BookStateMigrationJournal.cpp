#include "BookStateMigrationJournal.h"

#include <cstring>

namespace BookStateMigration {
namespace {

constexpr uint8_t kMagic[] = {'B', 'M', 'J', '1'};
constexpr uint16_t kVersion = 1;
constexpr uint8_t kCommittedMarker = 0xa5;

const char* slotPath(const Slot slot) { return slot == Slot::A ? kSlotAPath : kSlotBPath; }
Slot opposite(const Slot slot) { return slot == Slot::A ? Slot::B : Slot::A; }

void writeU16(uint8_t* const destination, const uint16_t value) {
  destination[0] = static_cast<uint8_t>(value);
  destination[1] = static_cast<uint8_t>(value >> 8U);
}

void writeU32(uint8_t* const destination, const uint32_t value) {
  for (uint8_t index = 0; index < 4; ++index) {
    destination[index] = static_cast<uint8_t>(value >> (index * 8U));
  }
}

void writeU64(uint8_t* const destination, const uint64_t value) {
  for (uint8_t index = 0; index < 8; ++index) {
    destination[index] = static_cast<uint8_t>(value >> (index * 8U));
  }
}

uint16_t readU16(const uint8_t* const source) {
  return static_cast<uint16_t>(source[0]) | (static_cast<uint16_t>(source[1]) << 8U);
}

uint32_t readU32(const uint8_t* const source) {
  uint32_t value = 0;
  for (uint8_t index = 0; index < 4; ++index) {
    value |= static_cast<uint32_t>(source[index]) << (index * 8U);
  }
  return value;
}

uint64_t readU64(const uint8_t* const source) {
  uint64_t value = 0;
  for (uint8_t index = 0; index < 8; ++index) {
    value |= static_cast<uint64_t>(source[index]) << (index * 8U);
  }
  return value;
}

uint32_t crc32(const uint8_t* const bytes, const size_t length) {
  uint32_t crc = UINT32_MAX;
  for (size_t index = 0; index < length; ++index) {
    crc ^= bytes[index];
    for (uint8_t bit = 0; bit < 8; ++bit) {
      const uint32_t mask = 0U - (crc & 1U);
      crc = (crc >> 1U) ^ (0xedb88320U & mask);
    }
  }
  return ~crc;
}

bool validPhase(const Phase phase) {
  return phase >= Phase::Prepared && phase <= Phase::Abandoned;
}

bool validFormat(const BookFormat format) { return format == BookFormat::Epub || format == BookFormat::Pdf; }

bool validPath(const StringView path) {
  if (path.data == nullptr || path.length == 0 || path.length > kMaxPathBytes || path.data[0] != '/') {
    return false;
  }
  return std::memchr(path.data, '\0', path.length) == nullptr;
}

Status validateRecord(const Record& record) {
  if (!validPhase(record.phase) || !validFormat(record.format) || !validPath(record.oldPath) ||
      !validPath(record.newPath)) {
    if (record.oldPath.length > kMaxPathBytes || record.newPath.length > kMaxPathBytes) {
      return Status::LimitExceeded;
    }
    return Status::InvalidArgument;
  }
  if (record.oldPath.length == record.newPath.length &&
      std::memcmp(record.oldPath.data, record.newPath.data, record.oldPath.length) == 0) {
    return Status::InvalidArgument;
  }
  return Status::Ok;
}

Status closeHandle(const Io& io, Handle* const handle, const Status prior) {
  if (handle == nullptr || !handle->valid()) {
    return prior;
  }
  const Status closeStatus = io.close(io.context, handle);
  return prior == Status::Ok ? closeStatus : prior;
}

struct SlotRead {
  bool present = false;
  bool valid = false;
  Record record{};
};

Status readSlot(const Io& io, const Scratch scratch, const Slot slot, SlotRead* const output) {
  if (output == nullptr) {
    return Status::InvalidArgument;
  }
  *output = {};
  Handle handle{};
  Status status = io.open(io.context, slotPath(slot), OpenMode::Read, &handle);
  if (status == Status::NotFound) {
    return Status::Ok;
  }
  if (status != Status::Ok) {
    return status;
  }
  output->present = true;

  size_t length = 0;
  status = io.size(io.context, handle, &length);
  if (status == Status::Ok && (length < kEncodedPrefixBytes + kEncodedCrcBytes || length > scratch.capacity)) {
    status = Status::Corrupt;
  }
  if (status == Status::Ok) {
    size_t actual = 0;
    status = io.read(io.context, handle, 0, scratch.data, length, &actual);
    if (status == Status::Ok && actual != length) {
      status = Status::Corrupt;
    }
  }
  status = closeHandle(io, &handle, status);
  if (status == Status::Corrupt) {
    return Status::Ok;
  }
  if (status != Status::Ok) {
    return status;
  }

  status = decode(scratch.data, length, &output->record);
  if (status == Status::Corrupt) {
    return Status::Ok;
  }
  if (status != Status::Ok) {
    return status;
  }
  output->valid = true;
  return Status::Ok;
}

bool recordsEqual(const Record& left, const Record& right) {
  return left.sequence == right.sequence && left.phase == right.phase && left.format == right.format &&
         left.oldHash == right.oldHash && left.newHash == right.newHash &&
         left.oldPath.length == right.oldPath.length && left.newPath.length == right.newPath.length &&
         std::memcmp(left.oldPath.data, right.oldPath.data, left.oldPath.length) == 0 &&
         std::memcmp(left.newPath.data, right.newPath.data, left.newPath.length) == 0;
}

Status removeSlot(const Io& io, const Slot slot) {
  const Status status = io.remove(io.context, slotPath(slot));
  return status == Status::NotFound ? Status::Ok : status;
}

Status writeRecord(const Io& io, const Scratch scratch, const Slot target, const Record& record,
                   Selection* const committed) {
  size_t encodedLength = 0;
  Status status = encode(record, scratch, &encodedLength);
  if (status != Status::Ok) {
    return status;
  }
  const uint32_t expectedCrc = readU32(scratch.data + encodedLength - kEncodedCrcBytes);

  Handle handle{};
  status = io.open(io.context, slotPath(target), OpenMode::WriteTruncate, &handle);
  if (status != Status::Ok) {
    return status;
  }
  size_t written = 0;
  status = io.write(io.context, handle, scratch.data, encodedLength, &written);
  if (status == Status::Ok && written != encodedLength) {
    status = Status::IoFailure;
  }
  if (status == Status::Ok) {
    status = io.flush(io.context, handle);
  }
  if (status == Status::Ok) {
    status = io.sync(io.context, handle);
  }
  status = closeHandle(io, &handle, status);
  if (status != Status::Ok) {
    if (!handle.valid()) {
      (void)removeSlot(io, target);
    }
    return status;
  }

  SlotRead verified{};
  status = readSlot(io, scratch, target, &verified);
  if (status != Status::Ok || !verified.valid || !recordsEqual(verified.record, record) ||
      readU32(scratch.data + encodedLength - kEncodedCrcBytes) != expectedCrc) {
    (void)removeSlot(io, target);
    return status == Status::Ok ? Status::Corrupt : status;
  }
  if (committed != nullptr) {
    committed->selected = true;
    committed->slot = target;
    committed->record = verified.record;
  }
  return Status::Ok;
}

bool legalAdvance(const Phase current, const Phase next) {
  if (current == Phase::Prepared && next == Phase::Abandoned) {
    return true;
  }
  if (current >= Phase::Prepared && current < Phase::OldStateRemoved) {
    return static_cast<uint8_t>(next) == static_cast<uint8_t>(current) + 1U;
  }
  return false;
}

ReadState readStateFor(const Selection& selection) {
  if (!selection.selected) {
    return {};
  }
  const bool activated = selection.record.phase == Phase::Activated ||
                         selection.record.phase == Phase::OldStateRemoved;
  return {Status::Ok,
          true,
          activated ? HashResolution::NewHash : HashResolution::OldHashFallback,
          activated ? selection.record.newHash : selection.record.oldHash,
          selection.record.phase};
}

StepResult failure(const Status status, const Selection& selection) {
  return {status,
          StepDisposition::Idle,
          selection.selected ? selection.record.phase : Phase::Prepared,
          readStateFor(selection)};
}

StepResult advanceResult(Journal& journal, const Selection& selection, const Phase next) {
  Selection committed{};
  const Status status = journal.advance(selection, next, &committed);
  if (status != Status::Ok) {
    return failure(status, selection);
  }
  return {Status::Ok, StepDisposition::Advanced, next, readStateFor(committed)};
}

Status runPair(const MigrationCallback first, const MigrationCallback second, void* const context,
               const Record& record) {
  Status status = first(context, record);
  if (status == Status::Ok) {
    status = second(context, record);
  }
  return status;
}

}  // namespace

bool Io::valid() const {
  return context != nullptr && open != nullptr && size != nullptr && read != nullptr && write != nullptr &&
         flush != nullptr && sync != nullptr && close != nullptr && remove != nullptr;
}

bool MigrationOperations::valid() const {
  return context != nullptr && locateSource != nullptr && renameSource != nullptr && copyCache != nullptr &&
         verifyCache != nullptr && copyBookmarks != nullptr && verifyBookmarks != nullptr &&
         copyClippings != nullptr && verifyClippings != nullptr && verifyState != nullptr &&
         activateRecent != nullptr && activateOpenPath != nullptr && verifyActivation != nullptr &&
         removeOldState != nullptr;
}

bool sequenceNewer(const uint32_t candidate, const uint32_t reference) {
  return candidate != reference && static_cast<int32_t>(candidate - reference) > 0;
}

Status encode(const Record& record, const Scratch scratch, size_t* const encodedLength) {
  if (encodedLength == nullptr) {
    return Status::InvalidArgument;
  }
  if (scratch.data == nullptr || scratch.capacity < kScratchCapacity) {
    return Status::ScratchTooSmall;
  }
  const Status validation = validateRecord(record);
  if (validation != Status::Ok) {
    return validation;
  }
  const size_t payloadLength = record.oldPath.length + record.newPath.length;
  const size_t totalLength = kEncodedPrefixBytes + payloadLength + kEncodedCrcBytes;
  if (payloadLength > UINT16_MAX || totalLength > scratch.capacity) {
    return Status::LimitExceeded;
  }

  uint8_t* const bytes = scratch.data;
  std::memcpy(bytes, kMagic, sizeof(kMagic));
  writeU16(bytes + 4, kVersion);
  writeU16(bytes + 6, static_cast<uint16_t>(kEncodedPrefixBytes));
  writeU32(bytes + 8, record.sequence);
  bytes[12] = static_cast<uint8_t>(record.phase);
  bytes[13] = static_cast<uint8_t>(record.format);
  bytes[14] = kCommittedMarker;
  bytes[15] = 0;
  writeU64(bytes + 16, record.oldHash);
  writeU64(bytes + 24, record.newHash);
  writeU16(bytes + 32, static_cast<uint16_t>(record.oldPath.length));
  writeU16(bytes + 34, static_cast<uint16_t>(record.newPath.length));
  writeU16(bytes + 36, static_cast<uint16_t>(payloadLength));
  writeU16(bytes + 38, 0);
  std::memmove(bytes + kEncodedPrefixBytes, record.oldPath.data, record.oldPath.length);
  std::memmove(bytes + kEncodedPrefixBytes + record.oldPath.length, record.newPath.data, record.newPath.length);
  writeU32(bytes + kEncodedPrefixBytes + payloadLength, crc32(bytes, kEncodedPrefixBytes + payloadLength));
  *encodedLength = totalLength;
  return Status::Ok;
}

Status decode(const uint8_t* const bytes, const size_t length, Record* const record) {
  if (record == nullptr) {
    return Status::InvalidArgument;
  }
  if (bytes == nullptr || length < kEncodedPrefixBytes + kEncodedCrcBytes || length > kScratchCapacity) {
    return Status::Corrupt;
  }
  if (std::memcmp(bytes, kMagic, sizeof(kMagic)) != 0 || readU16(bytes + 4) != kVersion ||
      readU16(bytes + 6) != kEncodedPrefixBytes || bytes[14] != kCommittedMarker || bytes[15] != 0 ||
      readU16(bytes + 38) != 0) {
    return Status::Corrupt;
  }
  const Phase phase = static_cast<Phase>(bytes[12]);
  const BookFormat format = static_cast<BookFormat>(bytes[13]);
  const size_t oldLength = readU16(bytes + 32);
  const size_t newLength = readU16(bytes + 34);
  const size_t payloadLength = readU16(bytes + 36);
  if (!validPhase(phase) || !validFormat(format) || oldLength == 0 || newLength == 0 ||
      oldLength > kMaxPathBytes || newLength > kMaxPathBytes || payloadLength != oldLength + newLength ||
      length != kEncodedPrefixBytes + payloadLength + kEncodedCrcBytes) {
    return Status::Corrupt;
  }
  const uint32_t expected = readU32(bytes + length - kEncodedCrcBytes);
  if (crc32(bytes, length - kEncodedCrcBytes) != expected) {
    return Status::Corrupt;
  }

  Record decoded{readU32(bytes + 8),
                 phase,
                 format,
                 readU64(bytes + 16),
                 readU64(bytes + 24),
                 {reinterpret_cast<const char*>(bytes + kEncodedPrefixBytes), oldLength},
                 {reinterpret_cast<const char*>(bytes + kEncodedPrefixBytes + oldLength), newLength}};
  if (!validPath(decoded.oldPath) || !validPath(decoded.newPath) ||
      (oldLength == newLength && std::memcmp(decoded.oldPath.data, decoded.newPath.data, oldLength) == 0)) {
    return Status::Corrupt;
  }
  *record = decoded;
  return Status::Ok;
}

Status reseal(uint8_t* const bytes, const size_t length) {
  if (bytes == nullptr || length < kEncodedPrefixBytes + kEncodedCrcBytes || length > kScratchCapacity) {
    return Status::InvalidArgument;
  }
  const size_t payloadLength = readU16(bytes + 36);
  if (readU16(bytes + 4) != kVersion || readU16(bytes + 6) != kEncodedPrefixBytes ||
      length != kEncodedPrefixBytes + payloadLength + kEncodedCrcBytes) {
    return Status::Corrupt;
  }
  writeU32(bytes + length - kEncodedCrcBytes, crc32(bytes, length - kEncodedCrcBytes));
  return Status::Ok;
}

Journal::Journal(const Io io, const Scratch scratch) : io_(io), scratch_(scratch) {}

Status Journal::load(Selection* const selection) {
  if (selection == nullptr || !io_.valid()) {
    return Status::InvalidArgument;
  }
  *selection = {};
  if (scratch_.data == nullptr || scratch_.capacity < kScratchCapacity) {
    return Status::ScratchTooSmall;
  }

  SlotRead a{};
  Status status = readSlot(io_, scratch_, Slot::A, &a);
  if (status != Status::Ok) {
    return status;
  }
  const uint32_t aSequence = a.record.sequence;

  SlotRead b{};
  status = readSlot(io_, scratch_, Slot::B, &b);
  if (status != Status::Ok) {
    return status;
  }
  if (b.valid && (!a.valid || sequenceNewer(b.record.sequence, aSequence))) {
    selection->selected = true;
    selection->slot = Slot::B;
    selection->record = b.record;
    return Status::Ok;
  }
  if (a.valid) {
    status = readSlot(io_, scratch_, Slot::A, &a);
    if (status != Status::Ok || !a.valid) {
      return status == Status::Ok ? Status::Corrupt : status;
    }
    selection->selected = true;
    selection->slot = Slot::A;
    selection->record = a.record;
    return Status::Ok;
  }
  return (a.present || b.present) ? Status::Corrupt : Status::Ok;
}

Status Journal::begin(const Record& record, Selection* const committed) {
  if (!io_.valid()) {
    return Status::InvalidArgument;
  }
  if (scratch_.data == nullptr || scratch_.capacity < kScratchCapacity) {
    return Status::ScratchTooSmall;
  }
  Selection existing{};
  Status status = load(&existing);
  if (status != Status::Ok) {
    return status;
  }
  if (existing.selected) {
    return Status::Conflict;
  }
  Record prepared = record;
  prepared.sequence = 1;
  prepared.phase = Phase::Prepared;
  return writeRecord(io_, scratch_, Slot::A, prepared, committed);
}

Status Journal::advance(const Selection& current, const Phase phase, Selection* const committed) {
  if (!io_.valid() || !current.selected || !legalAdvance(current.record.phase, phase)) {
    return Status::InvalidArgument;
  }
  Record next = current.record;
  ++next.sequence;
  next.phase = phase;
  return writeRecord(io_, scratch_, opposite(current.slot), next, committed);
}

Status Journal::cleanup(const Selection& terminal) {
  if (!io_.valid() || !terminal.selected ||
      (terminal.record.phase != Phase::OldStateRemoved && terminal.record.phase != Phase::Abandoned)) {
    return Status::InvalidArgument;
  }
  Status status = removeSlot(io_, opposite(terminal.slot));
  if (status != Status::Ok) {
    return status;
  }
  return removeSlot(io_, terminal.slot);
}

ReadState Journal::readState() {
  Selection selection{};
  const Status status = load(&selection);
  if (status != Status::Ok) {
    return {status, false, HashResolution::None, 0, Phase::Prepared};
  }
  return readStateFor(selection);
}

StepResult recoverOne(Journal& journal, const MigrationOperations& operations) {
  if (!operations.valid()) {
    return {Status::InvalidArgument, StepDisposition::Idle, Phase::Prepared, {}};
  }
  Selection selection{};
  const Status loadStatus = journal.load(&selection);
  if (loadStatus != Status::Ok) {
    return failure(loadStatus, selection);
  }
  if (!selection.selected) {
    return {Status::Ok, StepDisposition::Idle, Phase::Prepared, {}};
  }

  const Record& record = selection.record;
  Status status = Status::Ok;
  switch (record.phase) {
    case Phase::Prepared: {
      const SourceObservation source = operations.locateSource(operations.context, record);
      if (source.status != Status::Ok) {
        return failure(source.status, selection);
      }
      if (source.location == SourceLocation::Both) {
        return failure(Status::Conflict, selection);
      }
      if (source.location == SourceLocation::Missing) {
        return failure(Status::NotFound, selection);
      }
      if (source.location == SourceLocation::OldOnly) {
        status = operations.renameSource(operations.context, record);
        const SourceObservation renamed = operations.locateSource(operations.context, record);
        if (renamed.status != Status::Ok) {
          return failure(renamed.status, selection);
        }
        if (status != Status::Ok) {
          if (renamed.location == SourceLocation::NewOnly) {
            return advanceResult(journal, selection, Phase::SourceMoved);
          }
          if (renamed.location == SourceLocation::Both) {
            return failure(Status::Conflict, selection);
          }
          if (renamed.location == SourceLocation::Missing) {
            return failure(Status::NotFound, selection);
          }
          Selection abandoned{};
          const Status abandonStatus = journal.advance(selection, Phase::Abandoned, &abandoned);
          if (abandonStatus != Status::Ok) {
            return failure(abandonStatus, selection);
          }
          return {status, StepDisposition::Advanced, Phase::Abandoned, readStateFor(abandoned)};
        }
        if (renamed.location != SourceLocation::NewOnly) {
          if (renamed.location == SourceLocation::Both) {
            return failure(Status::Conflict, selection);
          }
          return failure(renamed.location == SourceLocation::Missing ? Status::NotFound : Status::OperationFailed,
                         selection);
        }
      }
      return advanceResult(journal, selection, Phase::SourceMoved);
    }
    case Phase::SourceMoved:
      status = runPair(operations.copyCache, operations.verifyCache, operations.context, record);
      return status == Status::Ok ? advanceResult(journal, selection, Phase::CacheCopied)
                                  : failure(status, selection);
    case Phase::CacheCopied:
      status = runPair(operations.copyBookmarks, operations.verifyBookmarks, operations.context, record);
      return status == Status::Ok ? advanceResult(journal, selection, Phase::BookmarksCopied)
                                  : failure(status, selection);
    case Phase::BookmarksCopied:
      status = runPair(operations.copyClippings, operations.verifyClippings, operations.context, record);
      return status == Status::Ok ? advanceResult(journal, selection, Phase::ClippingsCopied)
                                  : failure(status, selection);
    case Phase::ClippingsCopied:
      status = operations.verifyState(operations.context, record);
      return status == Status::Ok ? advanceResult(journal, selection, Phase::StateVerified)
                                  : failure(status, selection);
    case Phase::StateVerified:
      status = operations.activateRecent(operations.context, record);
      if (status == Status::Ok) {
        status = operations.activateOpenPath(operations.context, record);
      }
      if (status == Status::Ok) {
        status = operations.verifyActivation(operations.context, record);
      }
      return status == Status::Ok ? advanceResult(journal, selection, Phase::Activated)
                                  : failure(status, selection);
    case Phase::Activated:
      status = operations.removeOldState(operations.context, {record.oldPath, record.oldHash, record.format});
      return status == Status::Ok ? advanceResult(journal, selection, Phase::OldStateRemoved)
                                  : failure(status, selection);
    case Phase::OldStateRemoved:
      status = journal.cleanup(selection);
      return status == Status::Ok
                 ? StepResult{Status::Ok, StepDisposition::Complete, Phase::OldStateRemoved, {}}
                 : failure(status, selection);
    case Phase::Abandoned:
      status = journal.cleanup(selection);
      return status == Status::Ok
                 ? StepResult{Status::Ok, StepDisposition::Abandoned, Phase::Abandoned, {}}
                 : failure(status, selection);
  }
  return failure(Status::Corrupt, selection);
}

}  // namespace BookStateMigration
