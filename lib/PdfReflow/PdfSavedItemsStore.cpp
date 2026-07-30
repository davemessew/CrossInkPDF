#include "PdfSavedItemsStore.h"

#include <cstdio>
#include <cstring>

#include "PdfCacheFormat.h"

namespace {

constexpr uint8_t kMagic[] = {'P', 'S', 'I', 'T'};
constexpr uint16_t kVersion = 1;
constexpr uint32_t kModificationTimeKnown = 1U;
constexpr uint32_t kKnownHeaderFlags = kModificationTimeKnown;
constexpr uint8_t kKnownItemFlags =
    PDF_SAVED_ITEM_HAS_START_SEMANTIC | PDF_SAVED_ITEM_HAS_END_SEMANTIC | PDF_SAVED_ITEM_HAS_FALLBACK_PAGES;
constexpr size_t kRecordsCrcOffset = PDF_SAVED_ITEMS_RECORDS_CRC_OFFSET;
constexpr size_t kHeaderReservedOffset = 68;
constexpr char kSlotA[] = "saved_items.a";
constexpr char kSlotB[] = "saved_items.b";

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

bool allZero(const char* const bytes, const size_t length) {
  for (size_t index = 0; index < length; ++index) {
    if (bytes[index] != '\0') {
      return false;
    }
  }
  return true;
}

bool allZero(const uint8_t* const bytes, const size_t length) {
  for (size_t index = 0; index < length; ++index) {
    if (bytes[index] != 0) {
      return false;
    }
  }
  return true;
}

PdfStatus closePreservingStatus(const PdfCacheIo& io, PdfCacheHandle* const handle, const PdfStatus prior) {
  const PdfStatus closeStatus = io.close(io.context, handle);
  return prior ? closeStatus : prior;
}

PdfStatus validateSavedItem(const PdfSavedItem& item, const uint32_t totalWords) {
  if (item.itemId == 0 || item.itemId == UINT16_MAX || (item.flags & ~kKnownItemFlags) != 0) {
    return PdfStatus::failure(PdfError::Malformed);
  }
  if (boundedLength(item.startBlockAnchor, sizeof(item.startBlockAnchor)) == sizeof(item.startBlockAnchor) ||
      boundedLength(item.endBlockAnchor, sizeof(item.endBlockAnchor)) == sizeof(item.endBlockAnchor)) {
    return PdfStatus::failure(PdfError::Malformed);
  }

  const bool hasStart = (item.flags & PDF_SAVED_ITEM_HAS_START_SEMANTIC) != 0;
  const bool hasEnd = (item.flags & PDF_SAVED_ITEM_HAS_END_SEMANTIC) != 0;
  const bool hasFallback = (item.flags & PDF_SAVED_ITEM_HAS_FALLBACK_PAGES) != 0;
  if (!hasStart || item.startGlobalWordOrdinal >= totalWords) {
    return PdfStatus::failure(PdfError::InvalidOffset, item.startGlobalWordOrdinal);
  }

  if (item.kind == PdfSavedItemKind::Bookmark) {
    if (hasEnd || item.endGlobalWordOrdinal != 0 || item.endBlockWordOffset != 0 ||
        !allZero(item.endBlockAnchor, sizeof(item.endBlockAnchor))) {
      return PdfStatus::failure(PdfError::Malformed);
    }
  } else if (item.kind == PdfSavedItemKind::Clipping) {
    if (!hasEnd || item.endGlobalWordOrdinal >= totalWords || item.endGlobalWordOrdinal < item.startGlobalWordOrdinal) {
      return PdfStatus::failure(PdfError::InvalidOffset, item.endGlobalWordOrdinal);
    }
  } else {
    return PdfStatus::failure(PdfError::Malformed);
  }

  if (hasFallback) {
    if (item.fallbackLayoutFingerprint == 0 || item.fallbackPageCount == 0 ||
        item.fallbackStartPage > item.fallbackEndPage || item.fallbackEndPage >= item.fallbackPageCount ||
        (item.kind == PdfSavedItemKind::Bookmark && item.fallbackStartPage != item.fallbackEndPage)) {
      return PdfStatus::failure(PdfError::InvalidOffset, item.fallbackStartPage);
    }
  } else if (item.fallbackStartPage != 0 || item.fallbackEndPage != 0 || item.fallbackPageCount != 0) {
    return PdfStatus::failure(PdfError::Malformed);
  } else if (item.fallbackLayoutFingerprint != 0) {
    return PdfStatus::failure(PdfError::Malformed);
  }
  return PdfStatus::success();
}

void encodeItem(const PdfSavedItem& item, uint8_t output[PDF_SAVED_ITEMS_RECORD_BYTES]) {
  std::memset(output, 0, PDF_SAVED_ITEMS_RECORD_BYTES);
  putU16(output, item.itemId);
  output[2] = static_cast<uint8_t>(item.kind);
  output[3] = item.flags;
  putU32(output + 4, item.timestamp);
  putU32(output + 8, item.startGlobalWordOrdinal);
  putU32(output + 12, item.endGlobalWordOrdinal);
  putU32(output + 16, item.startBlockWordOffset);
  putU32(output + 20, item.endBlockWordOffset);
  putU16(output + 24, item.sectionIndex);
  putU16(output + 26, item.fallbackStartPage);
  putU16(output + 28, item.fallbackEndPage);
  putU16(output + 30, item.fallbackPageCount);
  std::memcpy(output + 32, item.startBlockAnchor, sizeof(item.startBlockAnchor));
  std::memcpy(output + 42, item.endBlockAnchor, sizeof(item.endBlockAnchor));
  putU32(output + 52, item.fallbackLayoutFingerprint);
}

PdfStatus decodeItem(const uint8_t input[PDF_SAVED_ITEMS_RECORD_BYTES], const uint32_t totalWords,
                     PdfSavedItem* const item) {
  if (item == nullptr) {
    return PdfStatus::failure(PdfError::InvalidArgument);
  }
  *item = {};
  item->itemId = getU16(input);
  item->kind = static_cast<PdfSavedItemKind>(input[2]);
  item->flags = input[3];
  item->timestamp = getU32(input + 4);
  item->startGlobalWordOrdinal = getU32(input + 8);
  item->endGlobalWordOrdinal = getU32(input + 12);
  item->startBlockWordOffset = getU32(input + 16);
  item->endBlockWordOffset = getU32(input + 20);
  item->sectionIndex = getU16(input + 24);
  item->fallbackStartPage = getU16(input + 26);
  item->fallbackEndPage = getU16(input + 28);
  item->fallbackPageCount = getU16(input + 30);
  std::memcpy(item->startBlockAnchor, input + 32, sizeof(item->startBlockAnchor));
  std::memcpy(item->endBlockAnchor, input + 42, sizeof(item->endBlockAnchor));
  item->fallbackLayoutFingerprint = getU32(input + 52);
  return pdfValidateSavedItem(*item, totalWords);
}

PdfStatus inspectItems(const PdfSavedItem* const items, const uint16_t count, const uint32_t totalWords,
                       uint16_t* const bookmarkCount, uint16_t* const clippingCount) {
  if ((count != 0 && items == nullptr) || bookmarkCount == nullptr || clippingCount == nullptr) {
    return PdfStatus::failure(PdfError::InvalidArgument);
  }
  if (count > PDF_SAVED_ITEMS_MAX_RECORDS) {
    return PdfStatus::failure(PdfError::LimitExceeded);
  }
  *bookmarkCount = 0;
  *clippingCount = 0;
  for (uint16_t index = 0; index < count; ++index) {
    const PdfStatus status = pdfValidateSavedItem(items[index], totalWords);
    if (!status) {
      return status;
    }
    for (uint16_t prior = 0; prior < index; ++prior) {
      if (items[prior].itemId == items[index].itemId) {
        return PdfStatus::failure(PdfError::Malformed, items[index].itemId);
      }
    }
    if (items[index].kind == PdfSavedItemKind::Bookmark) {
      if (++*bookmarkCount > PDF_SAVED_ITEMS_MAX_BOOKMARKS) {
        return PdfStatus::failure(PdfError::LimitExceeded);
      }
    } else if (++*clippingCount > PDF_SAVED_ITEMS_MAX_CLIPPINGS) {
      return PdfStatus::failure(PdfError::LimitExceeded);
    }
  }
  return PdfStatus::success();
}

uint32_t calculateRecordsCrc(const PdfSavedItem* const items, const uint16_t count) {
  uint32_t crc = 0;
  uint8_t encoded[PDF_SAVED_ITEMS_RECORD_BYTES];
  for (uint16_t index = 0; index < count; ++index) {
    encodeItem(items[index], encoded);
    crc = pdfCacheCrc32(encoded, sizeof(encoded), crc);
  }
  return crc;
}

bool absentOrCorruptSlot(const PdfStatus status) {
  return status.error == PdfError::InvalidOffset || status.error == PdfError::UnexpectedEof ||
         status.error == PdfError::Malformed;
}

}  // namespace

PdfStatus pdfValidateSavedItem(const PdfSavedItem& item, const uint32_t totalWords) {
  return validateSavedItem(item, totalWords);
}

struct PdfSavedItemsStore::SlotInfo {
  uint32_t sequence = 0;
  uint32_t recordsCrc = 0;
  uint16_t recordCount = 0;
  uint16_t bookmarkCount = 0;
  uint16_t clippingCount = 0;
  bool matchesIdentity = false;
};

PdfStatus PdfSavedItemsStore::initialize(const PdfCacheIo& io, const char* const cacheRoot,
                                         const PdfSourceIdentity& source, const uint32_t totalWords) {
  if (!io.valid() || cacheRoot == nullptr || cacheRoot[0] == '\0') {
    return PdfStatus::failure(PdfError::InvalidArgument);
  }
  io_ = io;
  cacheRoot_ = cacheRoot;
  source_ = source;
  totalWords_ = totalWords;
  pendingSlot_ = nullptr;
  pendingSequence_ = 0;
  initialized_ = true;
  return PdfStatus::success();
}

bool PdfSavedItemsStore::formatPath(const char* const name, char destination[PDF_CACHE_PATH_CAPACITY]) const {
  const int length = std::snprintf(destination, PDF_CACHE_PATH_CAPACITY, "%s/%s", cacheRoot_, name);
  return length > 0 && static_cast<size_t>(length) < PDF_CACHE_PATH_CAPACITY;
}

PdfStatus PdfSavedItemsStore::inspectSlot(const char* const name, SlotInfo* const info) const {
  if (!initialized_ || info == nullptr) {
    return PdfStatus::failure(PdfError::InvalidArgument);
  }
  *info = {};
  IoWorkspace& workspace = ioWorkspace_;
  if (!formatPath(name, workspace.path)) {
    return PdfStatus::failure(PdfError::LimitExceeded);
  }

  PdfCacheHandle handle{};
  PdfStatus status = io_.open(io_.context, workspace.path, PdfCacheOpenMode::Read, &handle);
  if (!status) {
    return status;
  }
  PdfCacheFileMetadata metadata{};
  status = io_.metadata(io_.context, handle, &metadata);
  size_t bytesRead = 0;
  if (status) {
    status = io_.read(io_.context, handle, 0, workspace.encoded, sizeof(workspace.encoded), &bytesRead);
    if (status && bytesRead != sizeof(workspace.encoded)) {
      status = PdfStatus::failure(PdfError::UnexpectedEof, bytesRead);
    }
  }
  status = closePreservingStatus(io_, &handle, status);
  if (!status) {
    return status;
  }

  const uint8_t* const encoded = workspace.encoded;
  const uint16_t recordCount = getU16(encoded + 10);
  const uint16_t bookmarkCount = getU16(encoded + 12);
  const uint16_t clippingCount = getU16(encoded + 14);
  const uint32_t flags = getU32(encoded + 20);
  const uint64_t expectedSize =
      PDF_SAVED_ITEMS_HEADER_BYTES + static_cast<uint64_t>(recordCount) * PDF_SAVED_ITEMS_RECORD_BYTES;
  if (metadata.directory || metadata.symlinkLike || metadata.size != expectedSize ||
      std::memcmp(encoded, kMagic, sizeof(kMagic)) != 0 || getU16(encoded + 4) != kVersion ||
      getU16(encoded + 6) != PDF_SAVED_ITEMS_HEADER_BYTES || getU16(encoded + 8) != PDF_SAVED_ITEMS_RECORD_BYTES ||
      recordCount > PDF_SAVED_ITEMS_MAX_RECORDS || bookmarkCount > PDF_SAVED_ITEMS_MAX_BOOKMARKS ||
      clippingCount > PDF_SAVED_ITEMS_MAX_CLIPPINGS ||
      static_cast<uint16_t>(bookmarkCount + clippingCount) != recordCount || (flags & ~kKnownHeaderFlags) != 0 ||
      getU32(encoded + PDF_SAVED_ITEMS_HEADER_CRC_OFFSET) !=
          pdfCacheCrc32(encoded, PDF_SAVED_ITEMS_HEADER_CRC_OFFSET) ||
      !allZero(encoded + kHeaderReservedOffset, PDF_SAVED_ITEMS_HEADER_BYTES - kHeaderReservedOffset)) {
    return PdfStatus::failure(PdfError::Malformed);
  }

  const bool modificationTimeKnown = (flags & kModificationTimeKnown) != 0;
  info->sequence = getU32(encoded + PDF_SAVED_ITEMS_SEQUENCE_OFFSET);
  info->recordsCrc = getU32(encoded + kRecordsCrcOffset);
  info->recordCount = recordCount;
  info->bookmarkCount = bookmarkCount;
  info->clippingCount = clippingCount;
  info->matchesIdentity = getU32(encoded + 56) == totalWords_ && getU64(encoded + 24) == source_.size &&
                          modificationTimeKnown == source_.modificationTime.known &&
                          (!modificationTimeKnown || getU64(encoded + 32) == source_.modificationTime.value) &&
                          getU64(encoded + 40) == source_.headFingerprint &&
                          getU64(encoded + 48) == source_.tailFingerprint;
  return PdfStatus::success();
}

PdfStatus PdfSavedItemsStore::readSlot(const char* const name, const SlotInfo& expected,
                                       PdfSavedItemsBuffer* const output) const {
  if (output == nullptr || expected.recordCount > output->capacity ||
      (expected.recordCount != 0 && output->items == nullptr)) {
    return PdfStatus::failure(PdfError::LimitExceeded);
  }
  IoWorkspace& workspace = ioWorkspace_;
  if (!formatPath(name, workspace.path)) {
    return PdfStatus::failure(PdfError::LimitExceeded);
  }
  PdfCacheHandle handle{};
  PdfStatus status = io_.open(io_.context, workspace.path, PdfCacheOpenMode::Read, &handle);
  if (!status) {
    return status;
  }

  size_t bytesRead = 0;
  status = io_.read(io_.context, handle, 0, workspace.encoded, PDF_SAVED_ITEMS_HEADER_BYTES, &bytesRead);
  if (status && bytesRead != PDF_SAVED_ITEMS_HEADER_BYTES) {
    status = PdfStatus::failure(PdfError::UnexpectedEof, bytesRead);
  }
  const uint8_t* const header = workspace.encoded;
  if (status &&
      (getU32(header + PDF_SAVED_ITEMS_SEQUENCE_OFFSET) != expected.sequence ||
       getU16(header + 10) != expected.recordCount || getU16(header + 12) != expected.bookmarkCount ||
       getU16(header + 14) != expected.clippingCount || getU32(header + kRecordsCrcOffset) != expected.recordsCrc ||
       getU32(header + PDF_SAVED_ITEMS_HEADER_CRC_OFFSET) !=
           pdfCacheCrc32(header, PDF_SAVED_ITEMS_HEADER_CRC_OFFSET))) {
    status = PdfStatus::failure(PdfError::Malformed);
  }

  uint32_t recordsCrc = 0;
  uint16_t bookmarkCount = 0;
  uint16_t clippingCount = 0;
  for (uint16_t index = 0; status && index < expected.recordCount; ++index) {
    const uint64_t offset = PDF_SAVED_ITEMS_HEADER_BYTES + static_cast<uint64_t>(index) * PDF_SAVED_ITEMS_RECORD_BYTES;
    bytesRead = 0;
    status = io_.read(io_.context, handle, offset, workspace.encoded, PDF_SAVED_ITEMS_RECORD_BYTES, &bytesRead);
    if (status && bytesRead != PDF_SAVED_ITEMS_RECORD_BYTES) {
      status = PdfStatus::failure(PdfError::UnexpectedEof, offset + bytesRead);
    }
    if (!status) {
      break;
    }
    recordsCrc = pdfCacheCrc32(workspace.encoded, PDF_SAVED_ITEMS_RECORD_BYTES, recordsCrc);
    status = decodeItem(workspace.encoded, totalWords_, &output->items[index]);
    if (!status) {
      break;
    }
    for (uint16_t prior = 0; prior < index; ++prior) {
      if (output->items[prior].itemId == output->items[index].itemId) {
        status = PdfStatus::failure(PdfError::Malformed, output->items[index].itemId);
        break;
      }
    }
    if (output->items[index].kind == PdfSavedItemKind::Bookmark) {
      ++bookmarkCount;
    } else {
      ++clippingCount;
    }
  }
  status = closePreservingStatus(io_, &handle, status);
  if (status && (recordsCrc != expected.recordsCrc || bookmarkCount != expected.bookmarkCount ||
                 clippingCount != expected.clippingCount)) {
    status = PdfStatus::failure(PdfError::Malformed);
  }
  if (status) {
    output->count = expected.recordCount;
  }
  return status;
}

PdfStatus PdfSavedItemsStore::writeSlot(const char* const name, const uint32_t sequence,
                                        const PdfSavedItem* const items, const uint16_t count,
                                        const uint16_t bookmarkCount, const uint16_t clippingCount,
                                        const uint32_t recordsCrc) const {
  IoWorkspace& workspace = ioWorkspace_;
  if (!formatPath(name, workspace.path)) {
    return PdfStatus::failure(PdfError::LimitExceeded);
  }
  PdfCacheHandle handle{};
  PdfStatus status = io_.open(io_.context, workspace.path, PdfCacheOpenMode::WriteTruncate, &handle);
  if (!status) {
    return status;
  }
  pendingSlot_ = name;
  pendingSequence_ = sequence;

  uint8_t* const encoded = workspace.encoded;
  std::memset(encoded, 0, PDF_SAVED_ITEMS_HEADER_BYTES);
  std::memcpy(encoded, kMagic, sizeof(kMagic));
  putU16(encoded + 4, kVersion);
  putU16(encoded + 6, PDF_SAVED_ITEMS_HEADER_BYTES);
  putU16(encoded + 8, PDF_SAVED_ITEMS_RECORD_BYTES);
  putU16(encoded + 10, count);
  putU16(encoded + 12, bookmarkCount);
  putU16(encoded + 14, clippingCount);
  putU32(encoded + PDF_SAVED_ITEMS_SEQUENCE_OFFSET, sequence);
  putU32(encoded + 20, source_.modificationTime.known ? kModificationTimeKnown : 0U);
  putU64(encoded + 24, source_.size);
  putU64(encoded + 32, source_.modificationTime.value);
  putU64(encoded + 40, source_.headFingerprint);
  putU64(encoded + 48, source_.tailFingerprint);
  putU32(encoded + 56, totalWords_);
  putU32(encoded + kRecordsCrcOffset, recordsCrc);
  putU32(encoded + PDF_SAVED_ITEMS_HEADER_CRC_OFFSET, pdfCacheCrc32(encoded, PDF_SAVED_ITEMS_HEADER_CRC_OFFSET));

  size_t bytesWritten = 0;
  status = io_.write(io_.context, handle, encoded, PDF_SAVED_ITEMS_HEADER_BYTES, &bytesWritten);
  if (status && bytesWritten != PDF_SAVED_ITEMS_HEADER_BYTES) {
    status = PdfStatus::failure(PdfError::InsufficientStorage, bytesWritten);
  }
  for (uint16_t index = 0; status && index < count; ++index) {
    encodeItem(items[index], encoded);
    bytesWritten = 0;
    status = io_.write(io_.context, handle, encoded, PDF_SAVED_ITEMS_RECORD_BYTES, &bytesWritten);
    if (status && bytesWritten != PDF_SAVED_ITEMS_RECORD_BYTES) {
      status = PdfStatus::failure(PdfError::InsufficientStorage, bytesWritten);
    }
  }
  if (status) {
    status = io_.flush(io_.context, handle);
  }
  if (status) {
    status = io_.sync(io_.context, handle);
  }
  status = closePreservingStatus(io_, &handle, status);
  if (status) {
    pendingSlot_ = nullptr;
    pendingSequence_ = 0;
  }
  return status;
}

PdfStatus PdfSavedItemsStore::load(PdfSavedItemsBuffer* const output) const {
  if (!initialized_ || output == nullptr || (output->capacity != 0 && output->items == nullptr)) {
    return PdfStatus::failure(PdfError::InvalidArgument);
  }
  output->count = 0;
  SlotInfo a;
  SlotInfo b;
  const PdfStatus aStatus =
      pendingSlot_ == kSlotA ? PdfStatus::failure(PdfError::InvalidOffset) : inspectSlot(kSlotA, &a);
  const PdfStatus bStatus =
      pendingSlot_ == kSlotB ? PdfStatus::failure(PdfError::InvalidOffset) : inspectSlot(kSlotB, &b);
  if ((!aStatus && !absentOrCorruptSlot(aStatus)) || (!bStatus && !absentOrCorruptSlot(bStatus))) {
    return !aStatus && !absentOrCorruptSlot(aStatus) ? aStatus : bStatus;
  }
  const bool aValid = aStatus && a.matchesIdentity;
  const bool bValid = bStatus && b.matchesIdentity;
  if (!aValid && !bValid) {
    return PdfStatus::failure(PdfError::InvalidOffset);
  }

  const bool useBFirst = bValid && (!aValid || pdfCacheSequenceNewer(b.sequence, a.sequence));
  const char* const firstName = useBFirst ? kSlotB : kSlotA;
  const SlotInfo& first = useBFirst ? b : a;
  PdfStatus status = readSlot(firstName, first, output);
  if (status) {
    return status;
  }

  const bool hasFallback = useBFirst ? aValid : bValid;
  if (!hasFallback || !absentOrCorruptSlot(status)) {
    output->count = 0;
    return status;
  }
  const char* const fallbackName = useBFirst ? kSlotA : kSlotB;
  const SlotInfo& fallback = useBFirst ? a : b;
  output->count = 0;
  return readSlot(fallbackName, fallback, output);
}

PdfStatus PdfSavedItemsStore::save(const PdfSavedItem* const items, const uint16_t count) const {
  if (!initialized_) {
    return PdfStatus::failure(PdfError::InvalidArgument);
  }
  uint16_t bookmarkCount = 0;
  uint16_t clippingCount = 0;
  const PdfStatus validation = inspectItems(items, count, totalWords_, &bookmarkCount, &clippingCount);
  if (!validation) {
    return validation;
  }
  const uint32_t recordsCrc = calculateRecordsCrc(items, count);
  if (pendingSlot_ != nullptr) {
    return writeSlot(pendingSlot_, pendingSequence_, items, count, bookmarkCount, clippingCount, recordsCrc);
  }

  SlotInfo a;
  SlotInfo b;
  const PdfStatus aStatus = inspectSlot(kSlotA, &a);
  if (!aStatus && !absentOrCorruptSlot(aStatus)) {
    return aStatus;
  }
  const PdfStatus bStatus = inspectSlot(kSlotB, &b);
  if (!bStatus && !absentOrCorruptSlot(bStatus)) {
    return bStatus;
  }
  const bool aValid = aStatus && a.matchesIdentity;
  const bool bValid = bStatus && b.matchesIdentity;
  const char* target = kSlotA;
  uint32_t sequence = 1U;
  if (aValid && (!bValid || !pdfCacheSequenceNewer(b.sequence, a.sequence))) {
    target = kSlotB;
    sequence = a.sequence + 1U;
  } else if (bValid) {
    target = kSlotA;
    sequence = b.sequence + 1U;
  }
  return writeSlot(target, sequence, items, count, bookmarkCount, clippingCount, recordsCrc);
}

PdfStatus PdfSavedItemsStore::validate(const PdfSavedItem& item) const {
  if (!initialized_) {
    return PdfStatus::failure(PdfError::InvalidArgument);
  }
  return pdfValidateSavedItem(item, totalWords_);
}
