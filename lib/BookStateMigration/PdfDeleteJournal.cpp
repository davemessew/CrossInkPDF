#include "PdfDeleteJournal.h"

#include <cstring>

namespace PdfDelete {
namespace {

constexpr uint8_t kMagic[] = {'P', 'D', 'J', '1'};
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

bool validPhase(const Phase phase) { return phase >= Phase::Prepared && phase <= Phase::SourceRemoved; }

bool validPath(const StringView path) {
  return path.data != nullptr && path.length > 0 && path.length <= kMaxPathBytes && path.data[0] == '/' &&
         std::memchr(path.data, '\0', path.length) == nullptr;
}

bool hasPdfExtension(const StringView path) {
  if (path.length < 4) return false;
  const char* const suffix = path.data + path.length - 4;
  const auto lower = [](const char value) {
    return value >= 'A' && value <= 'Z' ? static_cast<char>(value + ('a' - 'A')) : value;
  };
  return suffix[0] == '.' && lower(suffix[1]) == 'p' && lower(suffix[2]) == 'd' && lower(suffix[3]) == 'f';
}

bool matchesTombstone(const StringView source, const StringView tombstone) {
  size_t slash = source.length;
  while (slash > 0 && source.data[slash - 1] != '/') --slash;
  if (slash == 0 || slash == source.length) return false;

  const size_t suffixLength = sizeof(kTombstoneSuffix) - 1U;
  if (tombstone.length != source.length + 1U + suffixLength || std::memcmp(tombstone.data, source.data, slash) != 0 ||
      tombstone.data[slash] != '.' ||
      std::memcmp(tombstone.data + slash + 1U, source.data + slash, source.length - slash) != 0 ||
      std::memcmp(tombstone.data + source.length + 1U, kTombstoneSuffix, suffixLength) != 0) {
    return false;
  }
  return true;
}

bool sameString(const StringView first, const StringView second) {
  return first.length == second.length &&
         (first.length == 0 || std::memcmp(first.data, second.data, first.length) == 0);
}

bool samePreparedRecord(const Record& first, const Record& second) {
  return first.sequence == second.sequence && first.phase == Phase::Prepared && second.phase == Phase::Prepared &&
         first.format == BookFormat::Pdf && second.format == BookFormat::Pdf &&
         sameString(first.targets.source, second.targets.source) &&
         sameString(first.targets.tombstone, second.targets.tombstone) &&
         sameString(first.targets.cache, second.targets.cache) &&
         sameString(first.targets.bookmarks, second.targets.bookmarks) &&
         sameString(first.targets.clippings, second.targets.clippings) &&
         sameString(first.targets.recent, second.targets.recent);
}

Status validateRecord(const Record& record) {
  if (record.format != BookFormat::Pdf || !validPhase(record.phase)) {
    return Status::InvalidArgument;
  }
  return validateDeleteTargets(record.targets);
}

Status closeHandle(const Io& io, Handle* const handle, const Status prior) {
  if (handle == nullptr || !handle->valid()) return prior;
  const Status closeStatus = io.close(io.context, handle);
  return prior == Status::Ok ? closeStatus : prior;
}

Status removeSlot(const Io& io, const Slot slot) {
  const Status status = io.remove(io.context, slotPath(slot));
  return status == Status::NotFound ? Status::Ok : status;
}

struct SlotRead {
  bool present = false;
  bool valid = false;
  size_t encodedLength = 0;
  Record record{};
};

Status readSlot(const Io& io, const Scratch scratch, const Slot slot, SlotRead* const output) {
  if (output == nullptr) return Status::InvalidArgument;
  *output = {};

  Handle handle{};
  Status status = io.open(io.context, slotPath(slot), OpenMode::Read, &handle);
  if (status == Status::NotFound) return Status::Ok;
  if (status != Status::Ok) return status;
  output->present = true;

  size_t length = 0;
  status = io.size(io.context, handle, &length);
  if (status == Status::Ok && (length < kEncodedPrefixBytes + kEncodedCrcBytes || length > scratch.capacity)) {
    status = Status::Corrupt;
  }
  if (status == Status::Ok) {
    size_t actual = 0;
    status = io.read(io.context, handle, 0, scratch.data, length, &actual);
    if (status == Status::Ok && actual != length) status = Status::Corrupt;
  }
  status = closeHandle(io, &handle, status);
  if (status == Status::Corrupt) return Status::Ok;
  if (status != Status::Ok) return status;

  status = decode(scratch.data, length, &output->record);
  if (status == Status::Corrupt || status == Status::InvalidArgument) return Status::Ok;
  if (status != Status::Ok) return status;
  output->valid = true;
  output->encodedLength = length;
  return Status::Ok;
}

Status writeRecord(const Io& io, const Scratch scratch, const Slot target, const Record& record,
                   Selection* const committed, bool* const targetMayHaveChanged = nullptr) {
  if (targetMayHaveChanged != nullptr) *targetMayHaveChanged = false;
  size_t encodedLength = 0;
  Status status = encode(record, scratch, &encodedLength);
  if (status != Status::Ok) return status;
  const uint32_t expectedCrc = readU32(scratch.data + encodedLength - kEncodedCrcBytes);

  Handle handle{};
  if (targetMayHaveChanged != nullptr) *targetMayHaveChanged = true;
  status = io.open(io.context, slotPath(target), OpenMode::WriteTruncate, &handle);
  if (status != Status::Ok) return status;
  size_t written = 0;
  status = io.write(io.context, handle, scratch.data, encodedLength, &written);
  if (status == Status::Ok && written != encodedLength) status = Status::IoFailure;
  if (status == Status::Ok) status = io.flush(io.context, handle);
  if (status == Status::Ok) status = io.sync(io.context, handle);
  status = closeHandle(io, &handle, status);
  if (status != Status::Ok) {
    (void)removeSlot(io, target);
    return status;
  }

  SlotRead verified{};
  status = readSlot(io, scratch, target, &verified);
  if (status != Status::Ok || !verified.valid || verified.encodedLength != encodedLength ||
      verified.record.sequence != record.sequence || verified.record.phase != record.phase ||
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
  return current >= Phase::Prepared && current < Phase::SourceRemoved &&
         static_cast<uint8_t>(next) == static_cast<uint8_t>(current) + 1U;
}

}  // namespace

bool Io::valid() const {
  return context != nullptr && open != nullptr && size != nullptr && read != nullptr && write != nullptr &&
         flush != nullptr && sync != nullptr && close != nullptr && remove != nullptr;
}

bool sequenceNewer(const uint32_t candidate, const uint32_t reference) {
  return candidate != reference && static_cast<int32_t>(candidate - reference) > 0;
}

Status formatTombstonePath(const StringView source, char* const destination, const size_t capacity,
                           size_t* const length) {
  if (!validPath(source) || !hasPdfExtension(source) || destination == nullptr || length == nullptr) {
    return Status::InvalidArgument;
  }
  size_t slash = source.length;
  while (slash > 0 && source.data[slash - 1] != '/') --slash;
  if (slash == 0 || slash == source.length) return Status::InvalidArgument;

  const size_t suffixLength = sizeof(kTombstoneSuffix) - 1U;
  const size_t required = source.length + 1U + suffixLength;
  if (required > kMaxPathBytes) return Status::LimitExceeded;
  if (capacity <= required) return Status::ScratchTooSmall;

  std::memcpy(destination, source.data, slash);
  destination[slash] = '.';
  std::memcpy(destination + slash + 1U, source.data + slash, source.length - slash);
  std::memcpy(destination + source.length + 1U, kTombstoneSuffix, suffixLength);
  destination[required] = '\0';
  *length = required;
  return Status::Ok;
}

Status validateDeleteTargets(const Targets& targets) {
  const StringView values[kTargetCount] = {
      targets.source, targets.tombstone, targets.cache, targets.bookmarks, targets.clippings, targets.recent,
  };
  for (const StringView value : values) {
    if (!validPath(value)) {
      return value.length > kMaxPathBytes ? Status::LimitExceeded : Status::InvalidArgument;
    }
  }
  if (!hasPdfExtension(targets.source)) return Status::InvalidArgument;
  if (!matchesTombstone(targets.source, targets.tombstone)) return Status::InvalidArgument;
  return Status::Ok;
}

Status encode(const Record& record, const Scratch scratch, size_t* const encodedLength) {
  if (encodedLength == nullptr) return Status::InvalidArgument;
  if (scratch.data == nullptr || scratch.capacity < kScratchCapacity) return Status::ScratchTooSmall;
  const Status validation = validateRecord(record);
  if (validation != Status::Ok) return validation;

  const StringView values[kTargetCount] = {
      record.targets.source,    record.targets.tombstone, record.targets.cache,
      record.targets.bookmarks, record.targets.clippings, record.targets.recent,
  };
  size_t payloadLength = 0;
  for (const StringView value : values) payloadLength += value.length;
  const size_t totalLength = kEncodedPrefixBytes + payloadLength + kEncodedCrcBytes;
  if (totalLength > scratch.capacity || payloadLength > UINT32_MAX) return Status::LimitExceeded;

  uint8_t* const bytes = scratch.data;
  std::memcpy(bytes, kMagic, sizeof(kMagic));
  writeU16(bytes + 4, kVersion);
  writeU16(bytes + 6, static_cast<uint16_t>(kEncodedPrefixBytes));
  writeU32(bytes + 8, record.sequence);
  bytes[12] = static_cast<uint8_t>(record.phase);
  bytes[13] = static_cast<uint8_t>(record.format);
  bytes[14] = kCommittedMarker;
  bytes[15] = 0;
  for (size_t index = 0; index < kTargetCount; ++index) {
    writeU16(bytes + 16 + index * 2U, static_cast<uint16_t>(values[index].length));
  }
  writeU32(bytes + 28, static_cast<uint32_t>(payloadLength));
  writeU32(bytes + 32, static_cast<uint32_t>(totalLength));
  writeU32(bytes + 36, 0);

  size_t offset = kEncodedPrefixBytes;
  for (const StringView value : values) {
    std::memmove(bytes + offset, value.data, value.length);
    offset += value.length;
  }
  writeU32(bytes + offset, crc32(bytes, offset));
  *encodedLength = totalLength;
  return Status::Ok;
}

Status decode(const uint8_t* const bytes, const size_t length, Record* const record) {
  if (bytes == nullptr || record == nullptr) return Status::InvalidArgument;
  if (length < kEncodedPrefixBytes + kEncodedCrcBytes || std::memcmp(bytes, kMagic, sizeof(kMagic)) != 0 ||
      readU16(bytes + 4) != kVersion || readU16(bytes + 6) != kEncodedPrefixBytes || bytes[14] != kCommittedMarker ||
      bytes[15] != 0 || readU32(bytes + 36) != 0 || readU32(bytes + 32) != length ||
      readU32(bytes + length - kEncodedCrcBytes) != crc32(bytes, length - 4U)) {
    return Status::Corrupt;
  }

  size_t payloadLength = 0;
  uint16_t lengths[kTargetCount]{};
  for (size_t index = 0; index < kTargetCount; ++index) {
    lengths[index] = readU16(bytes + 16 + index * 2U);
    payloadLength += lengths[index];
  }
  if (readU32(bytes + 28) != payloadLength || kEncodedPrefixBytes + payloadLength + kEncodedCrcBytes != length) {
    return Status::Corrupt;
  }

  StringView values[kTargetCount]{};
  size_t offset = kEncodedPrefixBytes;
  for (size_t index = 0; index < kTargetCount; ++index) {
    values[index] = {reinterpret_cast<const char*>(bytes + offset), lengths[index]};
    offset += lengths[index];
  }
  const Record decoded{
      readU32(bytes + 8),
      static_cast<Phase>(bytes[12]),
      static_cast<BookFormat>(bytes[13]),
      {values[0], values[1], values[2], values[3], values[4], values[5]},
  };
  const Status validation = validateRecord(decoded);
  if (validation != Status::Ok) return Status::Corrupt;
  *record = decoded;
  return Status::Ok;
}

Journal::Journal(const Io io, const Scratch scratch) : io_(io), scratch_(scratch) {}

Status Journal::load(Selection* const selection) {
  if (selection == nullptr || !io_.valid()) return Status::InvalidArgument;
  if (scratch_.data == nullptr || scratch_.capacity < kScratchCapacity) return Status::ScratchTooSmall;
  *selection = {};

  SlotRead a{};
  Status status = readSlot(io_, scratch_, Slot::A, &a);
  if (status != Status::Ok) return status;
  const bool aPresent = a.present;
  const bool aValid = a.valid;
  const uint32_t aSequence = a.record.sequence;

  SlotRead b{};
  status = readSlot(io_, scratch_, Slot::B, &b);
  if (status != Status::Ok) return status;
  const bool bPresent = b.present;
  const bool bValid = b.valid;
  const uint32_t bSequence = b.record.sequence;

  if (!aValid && !bValid) return (aPresent || bPresent) ? Status::Corrupt : Status::Ok;
  if (aValid && bValid && aSequence == bSequence) return Status::Corrupt;
  const Slot selected = !bValid || (aValid && sequenceNewer(aSequence, bSequence)) ? Slot::A : Slot::B;

  SlotRead decoded{};
  status = readSlot(io_, scratch_, selected, &decoded);
  if (status != Status::Ok) return status;
  if (!decoded.valid) return Status::Corrupt;
  selection->selected = true;
  selection->slot = selected;
  selection->record = decoded.record;
  return Status::Ok;
}

BeginResult Journal::begin(const Record& record, Selection* const committed) {
  if (!io_.valid()) return {Status::InvalidArgument, BeginDisposition::Indeterminate, false};
  Selection existing{};
  Status status = load(&existing);
  if (status != Status::Ok) return {status, BeginDisposition::Indeterminate, false};
  if (existing.selected) return {Status::Conflict, BeginDisposition::Indeterminate, false};

  Record prepared = record;
  prepared.sequence = 1;
  prepared.phase = Phase::Prepared;
  bool targetMayHaveChanged = false;
  status = writeRecord(io_, scratch_, Slot::A, prepared, committed, &targetMayHaveChanged);
  if (status == Status::Ok) return {Status::Ok, BeginDisposition::Armed, true};

  Selection reconciled{};
  const Status reconciliation = load(&reconciled);
  if (reconciliation == Status::Ok && !reconciled.selected) {
    return {status, BeginDisposition::SafeFailure, false};
  }
  if (reconciliation == Status::Ok && reconciled.selected && samePreparedRecord(reconciled.record, prepared)) {
    if (committed != nullptr) *committed = reconciled;
    return {status, BeginDisposition::Armed, true};
  }
  return {status, BeginDisposition::Indeterminate, targetMayHaveChanged};
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
  if (!io_.valid() || !terminal.selected || terminal.record.phase != Phase::SourceRemoved) {
    return Status::InvalidArgument;
  }
  const Status stale = removeSlot(io_, opposite(terminal.slot));
  if (stale != Status::Ok) return stale;
  return removeSlot(io_, terminal.slot);
}

Status Journal::discardBeginAttempt() {
  if (!io_.valid()) return Status::InvalidArgument;
  const Status stale = removeSlot(io_, Slot::B);
  if (stale != Status::Ok) return stale;
  return removeSlot(io_, Slot::A);
}

Status Journal::discardPrepared(const Selection& prepared) {
  if (!io_.valid() || !prepared.selected || prepared.record.phase != Phase::Prepared) {
    return Status::InvalidArgument;
  }
  const Status stale = removeSlot(io_, opposite(prepared.slot));
  if (stale != Status::Ok) return stale;
  return removeSlot(io_, prepared.slot);
}

}  // namespace PdfDelete
