#include "PdfProgressStore.h"

#include <cstdio>
#include <cstring>

#include "PdfCacheFormat.h"
#include "PdfIo.h"
#include "PdfLayoutWordIndex.h"

namespace {

constexpr uint8_t kMagic[] = {'P', 'R', 'P', 'G'};
constexpr uint16_t kVersion = 1;
constexpr size_t kRecordBytes = 96;
constexpr size_t kCrcOffset = kRecordBytes - sizeof(uint32_t);
constexpr uint32_t kHasPageCount = 1U;
constexpr uint32_t kHasSemanticPosition = 2U;
constexpr uint32_t kHasWordCursor = 4U;
constexpr uint32_t kKnownFlags = kHasPageCount | kHasSemanticPosition | kHasWordCursor;
constexpr uint32_t kModificationTimeKnown = 1U << 31U;
constexpr char kSlotA[] = "progress.a";
constexpr char kSlotB[] = "progress.b";

void putU16(uint8_t* const destination, const uint16_t value) {
  destination[0] = static_cast<uint8_t>(value);
  destination[1] = static_cast<uint8_t>(value >> 8U);
}

void putU32(uint8_t* const destination, const uint32_t value) {
  destination[0] = static_cast<uint8_t>(value);
  destination[1] = static_cast<uint8_t>(value >> 8U);
  destination[2] = static_cast<uint8_t>(value >> 16U);
  destination[3] = static_cast<uint8_t>(value >> 24U);
}

void putU64(uint8_t* const destination, const uint64_t value) {
  for (uint8_t index = 0; index < 8; ++index) {
    destination[index] = static_cast<uint8_t>(value >> (index * 8U));
  }
}

uint16_t getU16(const uint8_t* const source) {
  return static_cast<uint16_t>(source[0]) | static_cast<uint16_t>(source[1]) << 8U;
}

uint32_t getU32(const uint8_t* const source) {
  return static_cast<uint32_t>(source[0]) | static_cast<uint32_t>(source[1]) << 8U |
         static_cast<uint32_t>(source[2]) << 16U | static_cast<uint32_t>(source[3]) << 24U;
}

uint64_t getU64(const uint8_t* const source) {
  uint64_t value = 0;
  for (uint8_t index = 0; index < 8; ++index) {
    value |= static_cast<uint64_t>(source[index]) << (index * 8U);
  }
  return value;
}

size_t boundedLength(const char* const value, const size_t capacity) {
  size_t length = 0;
  while (length < capacity && value[length] != '\0') {
    ++length;
  }
  return length;
}

PdfStatus closePreservingStatus(const PdfCacheIo& io, PdfCacheHandle* const handle, const PdfStatus prior) {
  const PdfStatus closeStatus = io.close(io.context, handle);
  return prior ? closeStatus : prior;
}

bool slotCanBeOverwritten(const PdfStatus status) {
  switch (status.error) {
    case PdfError::None:
    case PdfError::InvalidOffset:
    case PdfError::UnexpectedEof:
    case PdfError::Malformed:
      return true;
    default:
      return false;
  }
}

}  // namespace

struct PdfProgressStore::SlotRecord {
  uint32_t sequence = 0;
  uint32_t flags = 0;
  PdfSourceIdentity source{};
  uint32_t totalWords = 0;
  int32_t sectionIndex = 0;
  int32_t pageNumber = 0;
  int32_t pageCount = 0;
  uint32_t globalWordOrdinal = 0;
  uint32_t blockWordOffset = 0;
  uint32_t wordCursor = 0;
  char blockAnchor[PDF_LAYOUT_WORD_ANCHOR_BYTES] = {};
};

PdfStatus PdfProgressStore::initialize(const PdfCacheIo& io, const char* const cacheRoot,
                                       const PdfSourceIdentity& source, const uint32_t totalWords) {
  if (!io.valid() || cacheRoot == nullptr || cacheRoot[0] == '\0') {
    return PdfStatus::failure(PdfError::InvalidArgument);
  }
  io_ = io;
  cacheRoot_ = cacheRoot;
  source_ = source;
  totalWords_ = totalWords;
  pendingSlot_ = PendingSlot::None;
  pendingSequence_ = 0;
  initialized_ = true;
  return PdfStatus::success();
}

bool PdfProgressStore::formatPath(const char* const name, char destination[PDF_CACHE_PATH_CAPACITY]) const {
  const int length = std::snprintf(destination, PDF_CACHE_PATH_CAPACITY, "%s/%s", cacheRoot_, name);
  return length > 0 && static_cast<size_t>(length) < PDF_CACHE_PATH_CAPACITY;
}

bool PdfProgressStore::matchesIdentity(const SlotRecord& record) const {
  return initialized_ && record.totalWords == totalWords_ && pdfSourceIdentityEqual(record.source, source_);
}

PdfStatus PdfProgressStore::readSlot(const char* const name, SlotRecord* const record) const {
  if (!initialized_ || record == nullptr) {
    return PdfStatus::failure(PdfError::InvalidArgument);
  }
  char path[PDF_CACHE_PATH_CAPACITY];
  if (!formatPath(name, path)) {
    return PdfStatus::failure(PdfError::LimitExceeded);
  }
  PdfCacheHandle handle{};
  PdfStatus status = io_.open(io_.context, path, PdfCacheOpenMode::Read, &handle);
  if (!status) {
    return status;
  }
  PdfCacheFileMetadata metadata{};
  status = io_.metadata(io_.context, handle, &metadata);
  if (status && (metadata.directory || metadata.symlinkLike || metadata.size != kRecordBytes)) {
    status = PdfStatus::failure(PdfError::Malformed);
  }
  uint8_t encoded[kRecordBytes];
  size_t bytesRead = 0;
  if (status) {
    status = io_.read(io_.context, handle, 0, encoded, sizeof(encoded), &bytesRead);
    if (status && bytesRead != sizeof(encoded)) {
      status = PdfStatus::failure(PdfError::UnexpectedEof, bytesRead);
    }
  }
  status = closePreservingStatus(io_, &handle, status);
  if (!status) {
    return status;
  }
  if (std::memcmp(encoded, kMagic, sizeof(kMagic)) != 0 || getU16(encoded + 4) != kVersion ||
      getU16(encoded + 6) != kRecordBytes || (getU32(encoded + 12) & ~(kKnownFlags | kModificationTimeKnown)) != 0 ||
      encoded[82] >= PDF_LAYOUT_WORD_ANCHOR_BYTES ||
      getU32(encoded + kCrcOffset) != pdfCacheCrc32(encoded, kCrcOffset)) {
    return PdfStatus::failure(PdfError::Malformed);
  }
  if (encoded[83] != 0) {
    return PdfStatus::failure(PdfError::Malformed);
  }
  for (size_t index = 88; index < kCrcOffset; ++index) {
    if (encoded[index] != 0) {
      return PdfStatus::failure(PdfError::Malformed);
    }
  }
  *record = {};
  record->sequence = getU32(encoded + 8);
  record->flags = getU32(encoded + 12);
  record->source.size = getU64(encoded + 16);
  record->source.modificationTime.value = getU64(encoded + 24);
  record->source.headFingerprint = getU64(encoded + 32);
  record->source.tailFingerprint = getU64(encoded + 40);
  record->totalWords = getU32(encoded + 48);
  record->sectionIndex = static_cast<int32_t>(getU32(encoded + 52));
  record->pageNumber = static_cast<int32_t>(getU32(encoded + 56));
  record->pageCount = static_cast<int32_t>(getU32(encoded + 60));
  record->globalWordOrdinal = getU32(encoded + 64);
  record->blockWordOffset = getU32(encoded + 68);
  record->wordCursor = getU32(encoded + 84);
  std::memcpy(record->blockAnchor, encoded + 72, encoded[82]);
  record->source.modificationTime.known = (record->flags & kModificationTimeKnown) != 0;
  record->flags &= ~kModificationTimeKnown;
  if (record->source.modificationTime.known != source_.modificationTime.known || (record->flags & ~kKnownFlags) != 0 ||
      encoded[72 + encoded[82]] != '\0') {
    return PdfStatus::failure(PdfError::Malformed);
  }
  return PdfStatus::success();
}

PdfStatus PdfProgressStore::writeSlot(const char* const name, const SlotRecord& record) const {
  char path[PDF_CACHE_PATH_CAPACITY];
  if (!formatPath(name, path)) {
    return PdfStatus::failure(PdfError::LimitExceeded);
  }
  uint8_t encoded[kRecordBytes]{};
  std::memcpy(encoded, kMagic, sizeof(kMagic));
  putU16(encoded + 4, kVersion);
  putU16(encoded + 6, kRecordBytes);
  putU32(encoded + 8, record.sequence);
  uint32_t encodedFlags = record.flags;
  if (record.source.modificationTime.known) {
    encodedFlags |= kModificationTimeKnown;
  }
  putU32(encoded + 12, encodedFlags);
  putU64(encoded + 16, record.source.size);
  putU64(encoded + 24, record.source.modificationTime.value);
  putU64(encoded + 32, record.source.headFingerprint);
  putU64(encoded + 40, record.source.tailFingerprint);
  putU32(encoded + 48, record.totalWords);
  putU32(encoded + 52, static_cast<uint32_t>(record.sectionIndex));
  putU32(encoded + 56, static_cast<uint32_t>(record.pageNumber));
  putU32(encoded + 60, static_cast<uint32_t>(record.pageCount));
  putU32(encoded + 64, record.globalWordOrdinal);
  putU32(encoded + 68, record.blockWordOffset);
  const size_t anchorLength = boundedLength(record.blockAnchor, sizeof(record.blockAnchor));
  if (anchorLength == sizeof(record.blockAnchor)) {
    return PdfStatus::failure(PdfError::LimitExceeded);
  }
  std::memcpy(encoded + 72, record.blockAnchor, anchorLength);
  encoded[82] = static_cast<uint8_t>(anchorLength);
  putU32(encoded + 84, record.wordCursor);
  putU32(encoded + kCrcOffset, pdfCacheCrc32(encoded, kCrcOffset));

  PdfCacheHandle handle{};
  PdfStatus status = io_.open(io_.context, path, PdfCacheOpenMode::WriteTruncate, &handle);
  if (!status) {
    return status;
  }
  size_t bytesWritten = 0;
  status = io_.write(io_.context, handle, encoded, sizeof(encoded), &bytesWritten);
  if (status && bytesWritten != sizeof(encoded)) {
    status = PdfStatus::failure(PdfError::InsufficientStorage, bytesWritten);
  }
  if (status) {
    status = io_.flush(io_.context, handle);
  }
  if (status) {
    status = io_.sync(io_.context, handle);
  }
  status = closePreservingStatus(io_, &handle, status);
  // Failure means the commit was not confirmed. The complete record may still
  // be durable, so deleting it would add FAT work without proving rollback.
  return status;
}

PdfStatus PdfProgressStore::load(ReflowReadingPosition* const position) const {
  if (position == nullptr || !initialized_) {
    return PdfStatus::failure(PdfError::InvalidArgument);
  }
  SlotRecord a;
  SlotRecord b;
  const PdfStatus aStatus = pendingSlot_ == PendingSlot::A ? PdfStatus::failure(PdfError::InvalidOffset)
                                                           : readSlot(kSlotA, &a);
  const PdfStatus bStatus = pendingSlot_ == PendingSlot::B ? PdfStatus::failure(PdfError::InvalidOffset)
                                                           : readSlot(kSlotB, &b);
  const bool aValid = aStatus && matchesIdentity(a);
  const bool bValid = bStatus && matchesIdentity(b);
  if (!aValid && !bValid) {
    if (!slotCanBeOverwritten(aStatus)) {
      return aStatus;
    }
    if (!slotCanBeOverwritten(bStatus)) {
      return bStatus;
    }
    return PdfStatus::failure(PdfError::InvalidOffset);
  }
  const SlotRecord& selected = !aValid ? b : (!bValid || !pdfCacheSequenceNewer(b.sequence, a.sequence) ? a : b);
  if (selected.sectionIndex < 0 || selected.pageNumber < 0 ||
      ((selected.flags & kHasPageCount) != 0 && selected.pageCount < 0) ||
      ((selected.flags & kHasSemanticPosition) != 0 && selected.globalWordOrdinal >= totalWords_) ||
      ((selected.flags & kHasWordCursor) != 0 && selected.wordCursor > totalWords_) ||
      ((selected.flags & kHasWordCursor) == 0 && selected.wordCursor != 0)) {
    return PdfStatus::failure(PdfError::Malformed);
  }
  *position = {};
  position->sectionIndex = selected.sectionIndex;
  position->pageNumber = selected.pageNumber;
  position->pageCount = selected.pageCount;
  position->hasPageCount = (selected.flags & kHasPageCount) != 0;
  position->hasSemanticPosition = (selected.flags & kHasSemanticPosition) != 0;
  position->hasWordCursor = (selected.flags & kHasWordCursor) != 0;
  position->globalWordOrdinal = selected.globalWordOrdinal;
  position->blockWordOffset = selected.blockWordOffset;
  position->wordCursor = selected.wordCursor;
  std::memcpy(position->blockAnchor, selected.blockAnchor, sizeof(position->blockAnchor));
  return PdfStatus::success();
}

PdfStatus PdfProgressStore::save(const ReflowReadingPosition& position) const {
  if (!initialized_ || position.sectionIndex < 0 || position.pageNumber < 0 ||
      (position.hasPageCount && position.pageCount < 0) ||
      (position.hasSemanticPosition && position.globalWordOrdinal >= totalWords_) ||
      (position.hasWordCursor && position.wordCursor > totalWords_)) {
    return PdfStatus::failure(PdfError::InvalidOffset);
  }
  const size_t anchorLength = boundedLength(position.blockAnchor, sizeof(position.blockAnchor));
  if (anchorLength == sizeof(position.blockAnchor)) {
    return PdfStatus::failure(PdfError::LimitExceeded);
  }
  PendingSlot targetSlot = pendingSlot_;
  uint32_t nextSequence = pendingSequence_;
  if (targetSlot == PendingSlot::None) {
    SlotRecord a;
    SlotRecord b;
    const PdfStatus aStatus = readSlot(kSlotA, &a);
    if (!slotCanBeOverwritten(aStatus)) {
      return aStatus;
    }
    const PdfStatus bStatus = readSlot(kSlotB, &b);
    if (!slotCanBeOverwritten(bStatus)) {
      return bStatus;
    }
    const bool aValid = aStatus && matchesIdentity(a);
    const bool bValid = bStatus && matchesIdentity(b);
    const uint32_t aSequence = aValid ? a.sequence : 0;
    const uint32_t bSequence = bValid ? b.sequence : 0;
    targetSlot = PendingSlot::A;
    nextSequence = 1U;
    if (aValid && (!bValid || !pdfCacheSequenceNewer(bSequence, aSequence))) {
      targetSlot = PendingSlot::B;
      nextSequence = aSequence + 1U;
    } else if (bValid) {
      targetSlot = PendingSlot::A;
      nextSequence = bSequence + 1U;
    }
  }

  SlotRecord record;
  record.sequence = nextSequence;
  record.flags = (position.hasPageCount ? kHasPageCount : 0U) |
                 (position.hasSemanticPosition ? kHasSemanticPosition : 0U) |
                 (position.hasWordCursor ? kHasWordCursor : 0U);
  record.source = source_;
  record.totalWords = totalWords_;
  record.sectionIndex = position.sectionIndex;
  record.pageNumber = position.pageNumber;
  record.pageCount = position.pageCount;
  record.globalWordOrdinal = position.globalWordOrdinal;
  record.blockWordOffset = position.blockWordOffset;
  record.wordCursor = position.wordCursor;
  std::memcpy(record.blockAnchor, position.blockAnchor, anchorLength);
  const PdfStatus status = writeSlot(targetSlot == PendingSlot::A ? kSlotA : kSlotB, record);
  if (!status) {
    pendingSlot_ = targetSlot;
    pendingSequence_ = nextSequence;
    return status;
  }
  pendingSlot_ = PendingSlot::None;
  pendingSequence_ = 0;
  return PdfStatus::success();
}
