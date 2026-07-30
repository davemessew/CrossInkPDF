#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <new>
#include <vector>

#include "PdfCacheFormat.h"
#include "PdfSavedItemWordMap.h"
#include "PdfSavedItemsStore.h"
#include "PdfTestCacheIo.h"

namespace {

constexpr uint32_t kLayoutFingerprint = 0x4c415931U;
constexpr uint16_t kMapperBatchRecords = 4;

std::atomic<bool> allocationWatchArmed{false};
std::atomic<size_t> watchedAllocationCount{0};

class AllocationWatch {
 public:
  AllocationWatch() {
    watchedAllocationCount.store(0);
    allocationWatchArmed.store(true);
  }
  ~AllocationWatch() { allocationWatchArmed.store(false); }

  size_t count() const { return watchedAllocationCount.load(); }
};

PdfSourceIdentity identity() {
  return {
      4096,
      {true, 123456},
      0x0123456789abcdefULL,
      0xfedcba9876543210ULL,
  };
}

void copyAnchor(char destination[PDF_SAVED_ITEM_ANCHOR_BYTES], const char* source) {
  std::memset(destination, 0, PDF_SAVED_ITEM_ANCHOR_BYTES);
  std::memcpy(destination, source, std::min(std::strlen(source), static_cast<size_t>(PDF_SAVED_ITEM_ANCHOR_BYTES - 1)));
}

PdfSavedItem bookmark(const uint16_t id, const uint32_t ordinal, const uint16_t page, const uint16_t pageCount) {
  PdfSavedItem item{};
  item.timestamp = 1000U + id;
  item.startGlobalWordOrdinal = ordinal;
  item.startBlockWordOffset = ordinal;
  item.itemId = id;
  item.sectionIndex = 2;
  item.fallbackStartPage = page;
  item.fallbackEndPage = page;
  item.fallbackPageCount = pageCount;
  item.kind = PdfSavedItemKind::Bookmark;
  item.flags = PDF_SAVED_ITEM_HAS_START_SEMANTIC | PDF_SAVED_ITEM_HAS_FALLBACK_PAGES;
  copyAnchor(item.startBlockAnchor, "b00000000");
  item.fallbackLayoutFingerprint = kLayoutFingerprint;
  return item;
}

PdfSavedItem clipping(const uint16_t id, const uint32_t first, const uint32_t last, const uint16_t firstPage,
                      const uint16_t lastPage, const uint16_t pageCount) {
  PdfSavedItem item{};
  item.timestamp = 2000U + id;
  item.startGlobalWordOrdinal = first;
  item.endGlobalWordOrdinal = last;
  item.startBlockWordOffset = first;
  item.endBlockWordOffset = last >= 4 ? last - 4 : last;
  item.itemId = id;
  item.sectionIndex = 2;
  item.fallbackStartPage = firstPage;
  item.fallbackEndPage = lastPage;
  item.fallbackPageCount = pageCount;
  item.kind = PdfSavedItemKind::Clipping;
  item.flags = PDF_SAVED_ITEM_HAS_START_SEMANTIC | PDF_SAVED_ITEM_HAS_END_SEMANTIC | PDF_SAVED_ITEM_HAS_FALLBACK_PAGES;
  copyAnchor(item.startBlockAnchor, "b00000000");
  copyAnchor(item.endBlockAnchor, "b00000001");
  item.fallbackLayoutFingerprint = kLayoutFingerprint;
  return item;
}

void putU32(std::vector<uint8_t>* bytes, const size_t offset, const uint32_t value) {
  ASSERT_NE(bytes, nullptr);
  ASSERT_LE(offset + sizeof(value), bytes->size());
  (*bytes)[offset] = static_cast<uint8_t>(value);
  (*bytes)[offset + 1] = static_cast<uint8_t>(value >> 8U);
  (*bytes)[offset + 2] = static_cast<uint8_t>(value >> 16U);
  (*bytes)[offset + 3] = static_cast<uint8_t>(value >> 24U);
}

uint32_t getU32(const std::vector<uint8_t>& bytes, const size_t offset) {
  return static_cast<uint32_t>(bytes[offset]) | static_cast<uint32_t>(bytes[offset + 1]) << 8U |
         static_cast<uint32_t>(bytes[offset + 2]) << 16U | static_cast<uint32_t>(bytes[offset + 3]) << 24U;
}

struct LayoutFixture {
  PdfLayoutWordIndexInfo info{};
  std::array<PdfLayoutWordRange, 8> ranges{};
  uint16_t count = 0;
  uint32_t inspectCalls = 0;
  uint32_t readCalls = 0;
  uint16_t maximumReadCount = 0;
  uint32_t layoutFingerprint = kLayoutFingerprint;

  static PdfStatus inspect(void* context, const uint16_t sectionIndex, PdfLayoutWordIndexInfo* output) {
    if (context == nullptr || output == nullptr) {
      return PdfStatus::failure(PdfError::InvalidArgument);
    }
    auto& self = *static_cast<LayoutFixture*>(context);
    ++self.inspectCalls;
    if (sectionIndex != self.info.sectionIndex) {
      return PdfStatus::failure(PdfError::InvalidOffset, sectionIndex);
    }
    *output = self.info;
    return PdfStatus::success();
  }

  static PdfStatus read(void* context, const uint16_t sectionIndex, const uint16_t firstPage, const uint16_t requested,
                        PdfLayoutWordRange* output) {
    if (context == nullptr || output == nullptr || requested == 0) {
      return PdfStatus::failure(PdfError::InvalidArgument);
    }
    auto& self = *static_cast<LayoutFixture*>(context);
    ++self.readCalls;
    self.maximumReadCount = std::max(self.maximumReadCount, requested);
    if (sectionIndex != self.info.sectionIndex || firstPage >= self.count ||
        requested > static_cast<uint16_t>(self.count - firstPage)) {
      return PdfStatus::failure(PdfError::InvalidOffset, firstPage);
    }
    for (uint16_t index = 0; index < requested; ++index) {
      output[index] = self.ranges[firstPage + index];
    }
    return PdfStatus::success();
  }

  PdfSavedItemWordIndexSource source() { return {this, inspect, read, layoutFingerprint}; }
};

PdfLayoutWordRange range(const uint32_t first, const uint32_t last, const uint32_t blockOffset, const char* anchor) {
  PdfLayoutWordRange result{};
  result.firstGlobalWordOrdinal = first;
  result.lastGlobalWordOrdinal = last;
  result.firstBlockWordOffset = blockOffset;
  result.wordCursor = last + 1U;
  copyAnchor(result.blockAnchor, anchor);
  result.valid = true;
  return result;
}

struct MaximumPageLayout {
  PdfLayoutWordIndexInfo info{2, UINT16_MAX, 0, UINT16_MAX};
  uint32_t expectedFirstPage = 0;
  uint32_t readCalls = 0;
  uint16_t maximumReadCount = 0;
  uint16_t lastFirstPage = 0;

  static PdfStatus inspect(void* context, const uint16_t sectionIndex, PdfLayoutWordIndexInfo* output) {
    if (context == nullptr || output == nullptr) {
      return PdfStatus::failure(PdfError::InvalidArgument);
    }
    auto& self = *static_cast<MaximumPageLayout*>(context);
    if (sectionIndex != self.info.sectionIndex) {
      return PdfStatus::failure(PdfError::InvalidOffset, sectionIndex);
    }
    *output = self.info;
    return PdfStatus::success();
  }

  static PdfStatus read(void* context, const uint16_t sectionIndex, const uint16_t firstPage, const uint16_t requested,
                        PdfLayoutWordRange* output) {
    if (context == nullptr || output == nullptr || requested == 0) {
      return PdfStatus::failure(PdfError::InvalidArgument);
    }
    auto& self = *static_cast<MaximumPageLayout*>(context);
    const uint32_t first = firstPage;
    if (sectionIndex != self.info.sectionIndex || first != self.expectedFirstPage || requested > kMapperBatchRecords ||
        first + requested > self.info.pageCount) {
      return PdfStatus::failure(PdfError::InvalidOffset, first);
    }
    ++self.readCalls;
    self.maximumReadCount = std::max(self.maximumReadCount, requested);
    self.lastFirstPage = firstPage;
    for (uint16_t index = 0; index < requested; ++index) {
      const uint32_t page = first + index;
      output[index] = range(page, page, page, "b00000000");
    }
    self.expectedFirstPage += requested;
    return PdfStatus::success();
  }

  PdfSavedItemWordIndexSource source() { return {this, inspect, read, kLayoutFingerprint}; }
};

LayoutFixture compactLayout() {
  LayoutFixture layout;
  layout.info = {2, 2, 0, 8};
  layout.count = 2;
  layout.ranges[0] = range(0, 3, 0, "b00000000");
  layout.ranges[1] = range(4, 7, 0, "b00000001");
  return layout;
}

LayoutFixture largeFontLayout() {
  LayoutFixture layout;
  layout.info = {2, 3, 0, 8};
  layout.count = 3;
  layout.ranges[0] = range(0, 1, 0, "b00000000");
  layout.ranges[1] = range(2, 5, 2, "b00000000");
  layout.ranges[2] = range(6, 7, 2, "b00000001");
  return layout;
}

class FixedCacheIo {
 public:
  PdfCacheIo io() {
    return {this,       openThunk,   readThunk,  writeThunk, flushThunk,    syncThunk,
            closeThunk, removeThunk, mkdirThunk, listThunk,  capacityThunk, metadataThunk};
  }

  bool concurrentOpenAttempted() const { return concurrentOpenAttempted_; }

 private:
  struct Slot {
    std::array<uint8_t, PDF_SAVED_ITEMS_MAX_SLOT_BYTES> bytes{};
    size_t size = 0;
    bool exists = false;
  };

  static int slotForPath(const char* path) {
    if (path == nullptr) {
      return -1;
    }
    const size_t length = std::strlen(path);
    constexpr char slotA[] = "saved_items.a";
    constexpr char slotB[] = "saved_items.b";
    if (length >= sizeof(slotA) - 1 && std::strcmp(path + length - (sizeof(slotA) - 1), slotA) == 0) {
      return 0;
    }
    if (length >= sizeof(slotB) - 1 && std::strcmp(path + length - (sizeof(slotB) - 1), slotB) == 0) {
      return 1;
    }
    return -1;
  }

  static PdfStatus openThunk(void* context, const char* path, PdfCacheOpenMode mode, PdfCacheHandle* handle) {
    return static_cast<FixedCacheIo*>(context)->open(path, mode, handle);
  }
  static PdfStatus readThunk(void* context, PdfCacheHandle handle, uint64_t offset, uint8_t* destination,
                             size_t requested, size_t* bytesRead) {
    return static_cast<FixedCacheIo*>(context)->read(handle, offset, destination, requested, bytesRead);
  }
  static PdfStatus writeThunk(void* context, PdfCacheHandle handle, const uint8_t* source, size_t requested,
                              size_t* bytesWritten) {
    return static_cast<FixedCacheIo*>(context)->write(handle, source, requested, bytesWritten);
  }
  static PdfStatus flushThunk(void*, PdfCacheHandle) { return PdfStatus::success(); }
  static PdfStatus syncThunk(void*, PdfCacheHandle) { return PdfStatus::success(); }
  static PdfStatus closeThunk(void* context, PdfCacheHandle* handle) {
    return static_cast<FixedCacheIo*>(context)->close(handle);
  }
  static PdfStatus removeThunk(void* context, const char* path, bool) {
    return static_cast<FixedCacheIo*>(context)->remove(path);
  }
  static PdfStatus mkdirThunk(void*, const char*) { return PdfStatus::success(); }
  static PdfStatus listThunk(void*, const char*, PdfCacheListVisitor, void*) { return PdfStatus::success(); }
  static PdfStatus capacityThunk(void*, PdfCacheCapacity* capacity) {
    if (capacity == nullptr) {
      return PdfStatus::failure(PdfError::InvalidArgument);
    }
    *capacity = {};
    return PdfStatus::success();
  }
  static PdfStatus metadataThunk(void* context, PdfCacheHandle handle, PdfCacheFileMetadata* metadata) {
    return static_cast<FixedCacheIo*>(context)->metadata(handle, metadata);
  }

  PdfStatus open(const char* path, const PdfCacheOpenMode mode, PdfCacheHandle* handle) {
    if (handle == nullptr) {
      return PdfStatus::failure(PdfError::InvalidArgument);
    }
    if (openSlot_ >= 0) {
      concurrentOpenAttempted_ = true;
      return PdfStatus::failure(PdfError::LimitExceeded);
    }
    const int slotIndex = slotForPath(path);
    if (slotIndex < 0) {
      return PdfStatus::failure(PdfError::InvalidOffset);
    }
    Slot& slot = slots_[slotIndex];
    if (mode == PdfCacheOpenMode::Read && !slot.exists) {
      return PdfStatus::failure(PdfError::InvalidOffset);
    }
    if (mode == PdfCacheOpenMode::WriteTruncate) {
      slot.exists = true;
      slot.size = 0;
    }
    openSlot_ = slotIndex;
    writable_ = mode == PdfCacheOpenMode::WriteTruncate;
    position_ = 0;
    handle->value = 0;
    return PdfStatus::success();
  }

  PdfStatus read(const PdfCacheHandle handle, const uint64_t offset, uint8_t* destination, const size_t requested,
                 size_t* bytesRead) {
    if (!handle.valid() || openSlot_ < 0 || destination == nullptr || bytesRead == nullptr) {
      return PdfStatus::failure(PdfError::InvalidArgument);
    }
    const Slot& slot = slots_[openSlot_];
    if (offset > slot.size) {
      return PdfStatus::failure(PdfError::InvalidOffset, offset);
    }
    *bytesRead = std::min(requested, slot.size - static_cast<size_t>(offset));
    std::memcpy(destination, slot.bytes.data() + static_cast<size_t>(offset), *bytesRead);
    return PdfStatus::success();
  }

  PdfStatus write(const PdfCacheHandle handle, const uint8_t* source, const size_t requested, size_t* bytesWritten) {
    if (!handle.valid() || openSlot_ < 0 || !writable_ || source == nullptr || bytesWritten == nullptr ||
        requested > slots_[openSlot_].bytes.size() - position_) {
      return PdfStatus::failure(PdfError::InvalidArgument);
    }
    Slot& slot = slots_[openSlot_];
    std::memcpy(slot.bytes.data() + position_, source, requested);
    position_ += requested;
    slot.size = position_;
    *bytesWritten = requested;
    return PdfStatus::success();
  }

  PdfStatus close(PdfCacheHandle* handle) {
    if (handle == nullptr || !handle->valid() || openSlot_ < 0) {
      return PdfStatus::failure(PdfError::InvalidArgument);
    }
    openSlot_ = -1;
    writable_ = false;
    position_ = 0;
    handle->value = 0xff;
    return PdfStatus::success();
  }

  PdfStatus remove(const char* path) {
    const int slotIndex = slotForPath(path);
    if (slotIndex < 0) {
      return PdfStatus::failure(PdfError::InvalidOffset);
    }
    slots_[slotIndex] = {};
    return PdfStatus::success();
  }

  PdfStatus metadata(const PdfCacheHandle handle, PdfCacheFileMetadata* metadata) {
    if (!handle.valid() || openSlot_ < 0 || metadata == nullptr) {
      return PdfStatus::failure(PdfError::InvalidArgument);
    }
    metadata->size = slots_[openSlot_].size;
    return PdfStatus::success();
  }

  Slot slots_[2]{};
  int openSlot_ = -1;
  size_t position_ = 0;
  bool writable_ = false;
  bool concurrentOpenAttempted_ = false;
};

class AmbiguousFaultIo {
 public:
  explicit AmbiguousFaultIo(PdfTestCacheIo* storage) : base_(storage->io()) {}

  PdfCacheIo io() {
    return {this,       openThunk,   readThunk,  writeThunk, flushThunk,    syncThunk,
            closeThunk, removeThunk, mkdirThunk, listThunk,  capacityThunk, metadataThunk};
  }

  void failSyncAndRemove() {
    failSync_ = true;
    failRemove_ = true;
  }
  void failWritableCloseAndRemove() {
    failWritableClose_ = true;
    failRemove_ = true;
  }
  void clearFaults() {
    failSync_ = false;
    failWritableClose_ = false;
    failRemove_ = false;
  }
  uint32_t removeCalls() const { return removeCalls_; }

 private:
  static PdfStatus openThunk(void* context, const char* path, const PdfCacheOpenMode mode, PdfCacheHandle* handle) {
    auto& self = *static_cast<AmbiguousFaultIo*>(context);
    const PdfStatus status = self.base_.open(self.base_.context, path, mode, handle);
    if (status) {
      self.openWritable_ = mode == PdfCacheOpenMode::WriteTruncate;
    }
    return status;
  }
  static PdfStatus readThunk(void* context, const PdfCacheHandle handle, const uint64_t offset, uint8_t* destination,
                             const size_t requested, size_t* bytesRead) {
    auto& self = *static_cast<AmbiguousFaultIo*>(context);
    return self.base_.read(self.base_.context, handle, offset, destination, requested, bytesRead);
  }
  static PdfStatus writeThunk(void* context, const PdfCacheHandle handle, const uint8_t* source, const size_t requested,
                              size_t* bytesWritten) {
    auto& self = *static_cast<AmbiguousFaultIo*>(context);
    return self.base_.write(self.base_.context, handle, source, requested, bytesWritten);
  }
  static PdfStatus flushThunk(void* context, const PdfCacheHandle handle) {
    auto& self = *static_cast<AmbiguousFaultIo*>(context);
    return self.base_.flush(self.base_.context, handle);
  }
  static PdfStatus syncThunk(void* context, const PdfCacheHandle handle) {
    auto& self = *static_cast<AmbiguousFaultIo*>(context);
    const PdfStatus status = self.base_.sync(self.base_.context, handle);
    return status && self.failSync_ ? PdfStatus::failure(PdfError::IoFailure) : status;
  }
  static PdfStatus closeThunk(void* context, PdfCacheHandle* handle) {
    auto& self = *static_cast<AmbiguousFaultIo*>(context);
    const bool fail = self.openWritable_ && self.failWritableClose_;
    const PdfStatus status = self.base_.close(self.base_.context, handle);
    self.openWritable_ = false;
    return status && fail ? PdfStatus::failure(PdfError::IoFailure) : status;
  }
  static PdfStatus removeThunk(void* context, const char* path, const bool recursive) {
    auto& self = *static_cast<AmbiguousFaultIo*>(context);
    ++self.removeCalls_;
    if (self.failRemove_) {
      return PdfStatus::failure(PdfError::IoFailure);
    }
    return self.base_.remove(self.base_.context, path, recursive);
  }
  static PdfStatus mkdirThunk(void* context, const char* path) {
    auto& self = *static_cast<AmbiguousFaultIo*>(context);
    return self.base_.mkdir(self.base_.context, path);
  }
  static PdfStatus listThunk(void* context, const char* path, const PdfCacheListVisitor visitor, void* visitorContext) {
    auto& self = *static_cast<AmbiguousFaultIo*>(context);
    return self.base_.list(self.base_.context, path, visitor, visitorContext);
  }
  static PdfStatus capacityThunk(void* context, PdfCacheCapacity* capacity) {
    auto& self = *static_cast<AmbiguousFaultIo*>(context);
    return self.base_.capacity(self.base_.context, capacity);
  }
  static PdfStatus metadataThunk(void* context, const PdfCacheHandle handle, PdfCacheFileMetadata* metadata) {
    auto& self = *static_cast<AmbiguousFaultIo*>(context);
    return self.base_.metadata(self.base_.context, handle, metadata);
  }

  PdfCacheIo base_{};
  uint32_t removeCalls_ = 0;
  bool openWritable_ = false;
  bool failSync_ = false;
  bool failWritableClose_ = false;
  bool failRemove_ = false;
};

}  // namespace

void* operator new(const std::size_t size) {
  if (allocationWatchArmed.load()) {
    watchedAllocationCount.fetch_add(1);
    throw std::bad_alloc();
  }
  if (void* memory = std::malloc(size)) {
    return memory;
  }
  throw std::bad_alloc();
}

void* operator new[](const std::size_t size) { return ::operator new(size); }
void operator delete(void* memory) noexcept { std::free(memory); }
void operator delete[](void* memory) noexcept { std::free(memory); }
void operator delete(void* memory, std::size_t) noexcept { std::free(memory); }
void operator delete[](void* memory, std::size_t) noexcept { std::free(memory); }

TEST(PdfSavedItemsStoreTest, AlternatesSlotsAndRoundTripsBookmarkAndClipping) {
  PdfTestCacheIo files;
  files.addDirectory("/cache");
  PdfSavedItemsStore store;
  ASSERT_TRUE(store.initialize(files.io(), "/cache", identity(), 8));

  std::array<PdfSavedItem, 2> items{bookmark(1, 2, 0, 2), clipping(2, 2, 5, 0, 1, 2)};
  ASSERT_TRUE(store.save(items.data(), 1));
  EXPECT_TRUE(files.exists("/cache/saved_items.a"));
  EXPECT_FALSE(files.exists("/cache/saved_items.b"));
  ASSERT_TRUE(store.save(items.data(), items.size()));
  EXPECT_TRUE(files.exists("/cache/saved_items.b"));
  EXPECT_EQ(files.bytes("/cache/saved_items.b").size(),
            PDF_SAVED_ITEMS_HEADER_BYTES + items.size() * PDF_SAVED_ITEMS_RECORD_BYTES);

  std::array<PdfSavedItem, PDF_SAVED_ITEMS_MAX_RECORDS> loaded{};
  PdfSavedItemsBuffer output{loaded.data(), static_cast<uint16_t>(loaded.size()), 99};
  ASSERT_TRUE(store.load(&output));
  ASSERT_EQ(output.count, 2);
  EXPECT_EQ(loaded[0].kind, PdfSavedItemKind::Bookmark);
  EXPECT_EQ(loaded[0].itemId, 1);
  EXPECT_EQ(loaded[0].startGlobalWordOrdinal, 2U);
  EXPECT_EQ(loaded[1].kind, PdfSavedItemKind::Clipping);
  EXPECT_EQ(loaded[1].endGlobalWordOrdinal, 5U);
  EXPECT_STREQ(loaded[1].endBlockAnchor, "b00000001");
  EXPECT_EQ(files.openHandleCount(), 0U);
}

TEST(PdfSavedItemsStoreTest, TornPrefixesNeverReplaceThePreviousSlot) {
  constexpr std::array<uint64_t, 8> allowances{0, 1, 55, 79, 80, 81, 120, 135};
  for (const uint64_t allowance : allowances) {
    SCOPED_TRACE(allowance);
    PdfTestCacheIo files;
    files.addDirectory("/cache");
    PdfSavedItemsStore store;
    ASSERT_TRUE(store.initialize(files.io(), "/cache", identity(), 8));
    const PdfSavedItem stable = bookmark(1, 1, 0, 2);
    ASSERT_TRUE(store.save(&stable, 1));

    const PdfSavedItem attempted = bookmark(2, 6, 1, 2);
    files.setWriteAllowance(allowance);
    EXPECT_FALSE(store.save(&attempted, 1));
    files.clearWriteAllowance();

    std::array<PdfSavedItem, 2> loaded{};
    PdfSavedItemsBuffer output{loaded.data(), static_cast<uint16_t>(loaded.size()), 0};
    ASSERT_TRUE(store.load(&output));
    ASSERT_EQ(output.count, 1);
    EXPECT_EQ(loaded[0].itemId, stable.itemId);
    EXPECT_EQ(loaded[0].startGlobalWordOrdinal, stable.startGlobalWordOrdinal);
    EXPECT_EQ(files.openHandleCount(), 0U);
  }
}

TEST(PdfSavedItemsStoreTest, RejectsCorruptHeaderAndRecordCrcs) {
  PdfTestCacheIo files;
  files.addDirectory("/cache");
  PdfSavedItemsStore store;
  ASSERT_TRUE(store.initialize(files.io(), "/cache", identity(), 8));
  const PdfSavedItem item = bookmark(1, 2, 0, 2);
  ASSERT_TRUE(store.save(&item, 1));

  std::array<PdfSavedItem, 2> loaded{};
  PdfSavedItemsBuffer output{loaded.data(), static_cast<uint16_t>(loaded.size()), 0};
  files.corruptByte("/cache/saved_items.a", 24, 0x40);
  EXPECT_FALSE(store.load(&output));
  EXPECT_EQ(output.count, 0);

  ASSERT_TRUE(store.save(&item, 1));
  files.corruptByte("/cache/saved_items.a", PDF_SAVED_ITEMS_HEADER_BYTES + 8, 0x20);
  files.corruptByte("/cache/saved_items.b", PDF_SAVED_ITEMS_HEADER_BYTES + 8, 0x20);
  EXPECT_FALSE(store.load(&output));
  EXPECT_EQ(output.count, 0);
}

TEST(PdfSavedItemsStoreTest, AcceptsStructurallyValidSequenceAndTimestampChangesWhenCrcsAreRebuilt) {
  PdfTestCacheIo files;
  files.addDirectory("/cache");
  PdfSavedItemsStore store;
  ASSERT_TRUE(store.initialize(files.io(), "/cache", identity(), 8));
  const PdfSavedItem item = bookmark(1, 2, 0, 2);
  ASSERT_TRUE(store.save(&item, 1));

  std::vector<uint8_t> repaired = files.bytes("/cache/saved_items.a");
  putU32(&repaired, PDF_SAVED_ITEMS_SEQUENCE_OFFSET, 17U);
  files.addFile("/cache/saved_items.a", repaired);

  std::array<PdfSavedItem, 1> loaded{};
  PdfSavedItemsBuffer output{loaded.data(), static_cast<uint16_t>(loaded.size()), 0};
  EXPECT_FALSE(store.load(&output));
  EXPECT_EQ(output.count, 0);

  putU32(&repaired, PDF_SAVED_ITEMS_HEADER_CRC_OFFSET,
         pdfCacheCrc32(repaired.data(), PDF_SAVED_ITEMS_HEADER_CRC_OFFSET));
  files.addFile("/cache/saved_items.a", repaired);

  ASSERT_TRUE(store.load(&output));
  ASSERT_EQ(output.count, 1);
  EXPECT_EQ(loaded[0].itemId, item.itemId);

  constexpr uint32_t changedTimestamp = 0x12345678U;
  putU32(&repaired, PDF_SAVED_ITEMS_HEADER_BYTES + 4, changedTimestamp);
  files.addFile("/cache/saved_items.a", repaired);
  EXPECT_FALSE(store.load(&output));
  EXPECT_EQ(output.count, 0);

  putU32(&repaired, PDF_SAVED_ITEMS_RECORDS_CRC_OFFSET,
         pdfCacheCrc32(repaired.data() + PDF_SAVED_ITEMS_HEADER_BYTES, PDF_SAVED_ITEMS_RECORD_BYTES));
  putU32(&repaired, PDF_SAVED_ITEMS_HEADER_CRC_OFFSET,
         pdfCacheCrc32(repaired.data(), PDF_SAVED_ITEMS_HEADER_CRC_OFFSET));
  files.addFile("/cache/saved_items.a", repaired);

  ASSERT_TRUE(store.load(&output));
  ASSERT_EQ(output.count, 1);
  EXPECT_EQ(loaded[0].timestamp, changedTimestamp);
}

TEST(PdfSavedItemsStoreTest, EmitsTheExactPsitV1GoldenBytes) {
  PdfTestCacheIo files;
  files.addDirectory("/cache");
  PdfSavedItemsStore store;
  ASSERT_TRUE(store.initialize(files.io(), "/cache", identity(), 8));
  const PdfSavedItem item = bookmark(1, 2, 0, 2);
  ASSERT_TRUE(store.save(&item, 1));

  constexpr std::array<uint8_t, PDF_SAVED_ITEMS_HEADER_BYTES + PDF_SAVED_ITEMS_RECORD_BYTES> expected{
      0x50, 0x53, 0x49, 0x54, 0x01, 0x00, 0x50, 0x00, 0x38, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01,
      0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x40, 0xe2,
      0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0xef, 0xcd, 0xab, 0x89, 0x67, 0x45, 0x23, 0x01, 0x10, 0x32, 0x54,
      0x76, 0x98, 0xba, 0xdc, 0xfe, 0x08, 0x00, 0x00, 0x00, 0x7f, 0x04, 0x06, 0x15, 0xb2, 0xd8, 0xc2, 0xd6,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x01, 0x05, 0xe9,
      0x03, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x62, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30,
      0x30, 0x30, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x31, 0x59, 0x41, 0x4c,
  };
  EXPECT_EQ(files.bytes("/cache/saved_items.a"), std::vector<uint8_t>(expected.begin(), expected.end()));
}

TEST(PdfSavedItemsStoreTest, FallsBackToOlderSlotWhenTheNewerRecordCrcIsCorrupt) {
  PdfTestCacheIo files;
  files.addDirectory("/cache");
  PdfSavedItemsStore store;
  ASSERT_TRUE(store.initialize(files.io(), "/cache", identity(), 8));
  const PdfSavedItem stable = bookmark(1, 2, 0, 2);
  const PdfSavedItem newer = bookmark(2, 6, 1, 2);
  ASSERT_TRUE(store.save(&stable, 1));
  ASSERT_TRUE(store.save(&newer, 1));
  files.corruptByte("/cache/saved_items.b", PDF_SAVED_ITEMS_HEADER_BYTES + 8, 0x20);

  std::array<PdfSavedItem, 2> loaded{};
  PdfSavedItemsBuffer output{loaded.data(), static_cast<uint16_t>(loaded.size()), 0};
  ASSERT_TRUE(store.load(&output));
  ASSERT_EQ(output.count, 1);
  EXPECT_EQ(loaded[0].itemId, stable.itemId);
  EXPECT_EQ(loaded[0].startGlobalWordOrdinal, stable.startGlobalWordOrdinal);
}

TEST(PdfSavedItemsStoreTest, FailedWritesAndDurabilityCallsKeepThePreviousSlot) {
  for (const PdfTestFaultPoint fault :
       {PdfTestFaultPoint::Write, PdfTestFaultPoint::Flush, PdfTestFaultPoint::Sync, PdfTestFaultPoint::Close}) {
    SCOPED_TRACE(static_cast<int>(fault));
    PdfTestCacheIo files;
    files.addDirectory("/cache");
    PdfSavedItemsStore store;
    ASSERT_TRUE(store.initialize(files.io(), "/cache", identity(), 8));
    const PdfSavedItem stable = bookmark(1, 2, 0, 2);
    const PdfSavedItem attempted = bookmark(2, 6, 1, 2);
    ASSERT_TRUE(store.save(&stable, 1));

    files.fail(fault, fault == PdfTestFaultPoint::Close ? 2 : 1);
    EXPECT_FALSE(store.save(&attempted, 1));
    files.clearFault();
    EXPECT_EQ(files.openHandleCount(), 0U);

    std::array<PdfSavedItem, 2> loaded{};
    PdfSavedItemsBuffer output{loaded.data(), static_cast<uint16_t>(loaded.size()), 0};
    ASSERT_TRUE(store.load(&output));
    ASSERT_EQ(output.count, 1);
    EXPECT_EQ(loaded[0].itemId, stable.itemId);
  }
}

TEST(PdfSavedItemsStoreTest, AmbiguousSyncOrCloseFailureMasksPendingButRebootMayAcceptCompleteBytes) {
  for (const bool failClose : {false, true}) {
    SCOPED_TRACE(failClose ? "close" : "sync");
    PdfTestCacheIo files;
    files.addDirectory("/cache");
    AmbiguousFaultIo faults(&files);
    PdfSavedItemsStore store;
    ASSERT_TRUE(store.initialize(faults.io(), "/cache", identity(), 8));
    const PdfSavedItem stable = bookmark(1, 2, 0, 2);
    const PdfSavedItem attempted = bookmark(2, 6, 1, 2);
    ASSERT_TRUE(store.save(&stable, 1));

    failClose ? faults.failWritableCloseAndRemove() : faults.failSyncAndRemove();
    EXPECT_FALSE(store.save(&attempted, 1));
    EXPECT_EQ(faults.removeCalls(), 0U);

    std::array<PdfSavedItem, 2> loaded{};
    PdfSavedItemsBuffer output{loaded.data(), static_cast<uint16_t>(loaded.size()), 0};
    ASSERT_TRUE(store.load(&output));
    ASSERT_EQ(output.count, 1);
    EXPECT_EQ(loaded[0].itemId, stable.itemId);

    faults.clearFaults();
    PdfSavedItemsStore rebooted;
    ASSERT_TRUE(rebooted.initialize(faults.io(), "/cache", identity(), 8));
    ASSERT_TRUE(rebooted.load(&output));
    ASSERT_EQ(output.count, 1);
    EXPECT_EQ(loaded[0].itemId, attempted.itemId);
  }
}

TEST(PdfSavedItemsStoreTest, RetryAfterAmbiguousFailureNeverTruncatesTheLastConfirmedSlot) {
  PdfTestCacheIo files;
  files.addDirectory("/cache");
  AmbiguousFaultIo faults(&files);
  PdfSavedItemsStore store;
  ASSERT_TRUE(store.initialize(faults.io(), "/cache", identity(), 8));
  const PdfSavedItem stable = bookmark(1, 2, 0, 2);
  const PdfSavedItem ambiguous = bookmark(2, 5, 1, 2);
  const PdfSavedItem retry = bookmark(3, 6, 1, 2);
  ASSERT_TRUE(store.save(&stable, 1));
  const std::vector<uint8_t> confirmedBytes = files.bytes("/cache/saved_items.a");

  faults.failSyncAndRemove();
  EXPECT_FALSE(store.save(&ambiguous, 1));
  faults.clearFaults();

  files.setWriteAllowance(0);
  EXPECT_FALSE(store.save(&retry, 1));
  files.clearWriteAllowance();
  ASSERT_TRUE(files.exists("/cache/saved_items.a"));
  EXPECT_EQ(files.bytes("/cache/saved_items.a"), confirmedBytes);

  std::array<PdfSavedItem, 2> loaded{};
  PdfSavedItemsBuffer output{loaded.data(), static_cast<uint16_t>(loaded.size()), 0};
  ASSERT_TRUE(store.load(&output));
  ASSERT_EQ(output.count, 1);
  EXPECT_EQ(loaded[0].itemId, stable.itemId);

  ASSERT_TRUE(store.save(&retry, 1));
  ASSERT_TRUE(store.load(&output));
  ASSERT_EQ(output.count, 1);
  EXPECT_EQ(loaded[0].itemId, retry.itemId);
}

TEST(PdfSavedItemsStoreTest, LoadPropagatesTransientInspectionFailuresInsteadOfReportingNoItems) {
  PdfTestCacheIo files;
  files.addDirectory("/cache");
  PdfSavedItemsStore store;
  ASSERT_TRUE(store.initialize(files.io(), "/cache", identity(), 8));
  const PdfSavedItem first = bookmark(1, 2, 0, 2);
  const PdfSavedItem second = bookmark(2, 6, 1, 2);
  ASSERT_TRUE(store.save(&first, 1));
  ASSERT_TRUE(store.save(&second, 1));

  std::array<PdfSavedItem, 2> loaded{};
  PdfSavedItemsBuffer output{loaded.data(), static_cast<uint16_t>(loaded.size()), 0};
  files.fail(PdfTestFaultPoint::Read, 2);
  EXPECT_EQ(store.load(&output).error, PdfError::IoFailure);
  EXPECT_EQ(output.count, 0);
  files.clearFault();
}

TEST(PdfSavedItemsStoreTest, TransientNewestSlotReadOrCloseFailureNeverReturnsOrRewritesOlderState) {
  struct FaultCase {
    PdfTestFaultPoint point;
    uint32_t occurrence;
  };
  constexpr std::array<FaultCase, 3> faults{{
      {PdfTestFaultPoint::Read, 3},
      {PdfTestFaultPoint::Read, 4},
      {PdfTestFaultPoint::Close, 3},
  }};

  for (const FaultCase fault : faults) {
    SCOPED_TRACE(static_cast<int>(fault.point));
    SCOPED_TRACE(fault.occurrence);
    PdfTestCacheIo files;
    files.addDirectory("/cache");
    PdfSavedItemsStore store;
    ASSERT_TRUE(store.initialize(files.io(), "/cache", identity(), 8));
    const PdfSavedItem older = bookmark(1, 2, 0, 2);
    const PdfSavedItem newer = bookmark(2, 6, 1, 2);
    ASSERT_TRUE(store.save(&older, 1));
    ASSERT_TRUE(store.save(&newer, 1));
    const std::vector<uint8_t> confirmedA = files.bytes("/cache/saved_items.a");
    const std::vector<uint8_t> confirmedB = files.bytes("/cache/saved_items.b");

    std::array<PdfSavedItem, 2> loaded{};
    PdfSavedItemsBuffer output{loaded.data(), static_cast<uint16_t>(loaded.size()), 0};
    files.fail(fault.point, fault.occurrence);
    const PdfStatus status = store.load(&output);
    if (status) {
      ASSERT_TRUE(store.save(output.items, output.count));
    }
    EXPECT_EQ(status.error, PdfError::IoFailure);
    EXPECT_EQ(output.count, 0);
    files.clearFault();

    EXPECT_EQ(files.bytes("/cache/saved_items.a"), confirmedA);
    EXPECT_EQ(files.bytes("/cache/saved_items.b"), confirmedB);
    ASSERT_TRUE(store.load(&output));
    ASSERT_EQ(output.count, 1);
    EXPECT_EQ(loaded[0].itemId, newer.itemId);
  }
}

TEST(PdfSavedItemsStoreTest, TransientInspectionFailureNeverOverwritesTheOnlyValidSlot) {
  for (const PdfTestFaultPoint fault :
       {PdfTestFaultPoint::Read, PdfTestFaultPoint::Metadata, PdfTestFaultPoint::Close}) {
    SCOPED_TRACE(static_cast<int>(fault));
    PdfTestCacheIo files;
    files.addDirectory("/cache");
    PdfSavedItemsStore store;
    ASSERT_TRUE(store.initialize(files.io(), "/cache", identity(), 8));
    const PdfSavedItem stable = bookmark(1, 2, 0, 2);
    const PdfSavedItem attempted = bookmark(2, 6, 1, 2);
    ASSERT_TRUE(store.save(&stable, 1));

    files.fail(fault);
    EXPECT_FALSE(store.save(&attempted, 1));
    files.clearFault();

    std::array<PdfSavedItem, 2> loaded{};
    PdfSavedItemsBuffer output{loaded.data(), static_cast<uint16_t>(loaded.size()), 0};
    ASSERT_TRUE(store.load(&output));
    ASSERT_EQ(output.count, 1);
    EXPECT_EQ(loaded[0].itemId, stable.itemId);
  }
}

TEST(PdfSavedItemsStoreTest, EmptyCollectionDurablyClearsAllItems) {
  PdfTestCacheIo files;
  files.addDirectory("/cache");
  PdfSavedItemsStore store;
  ASSERT_TRUE(store.initialize(files.io(), "/cache", identity(), 8));
  const PdfSavedItem item = bookmark(1, 2, 0, 2);
  ASSERT_TRUE(store.save(&item, 1));
  ASSERT_TRUE(store.save(nullptr, 0));

  std::array<PdfSavedItem, 2> loaded{};
  PdfSavedItemsBuffer output{loaded.data(), static_cast<uint16_t>(loaded.size()), 99};
  ASSERT_TRUE(store.load(&output));
  EXPECT_EQ(output.count, 0);
  EXPECT_EQ(files.bytes("/cache/saved_items.b").size(), PDF_SAVED_ITEMS_HEADER_BYTES);
}

TEST(PdfSavedItemsStoreTest, SemanticOnlyItemKeepsItsSectionWithoutAStalePageFallback) {
  PdfTestCacheIo files;
  files.addDirectory("/cache");
  PdfSavedItemsStore store;
  ASSERT_TRUE(store.initialize(files.io(), "/cache", identity(), 8));
  PdfSavedItem item = bookmark(1, 2, 0, 2);
  item.flags &= static_cast<uint8_t>(~PDF_SAVED_ITEM_HAS_FALLBACK_PAGES);
  item.fallbackStartPage = 0;
  item.fallbackEndPage = 0;
  item.fallbackPageCount = 0;
  item.fallbackLayoutFingerprint = 0;
  ASSERT_TRUE(store.save(&item, 1));

  std::array<PdfSavedItem, 1> loaded{};
  PdfSavedItemsBuffer output{loaded.data(), static_cast<uint16_t>(loaded.size()), 0};
  ASSERT_TRUE(store.load(&output));
  ASSERT_EQ(output.count, 1);
  EXPECT_EQ(loaded[0].sectionIndex, 2);
  EXPECT_EQ(loaded[0].flags & PDF_SAVED_ITEM_HAS_FALLBACK_PAGES, 0);
}

TEST(PdfSavedItemsStoreTest, IgnoresChangedSourceIdentityAndTotalWordCount) {
  PdfTestCacheIo files;
  files.addDirectory("/cache");
  PdfSavedItemsStore writer;
  ASSERT_TRUE(writer.initialize(files.io(), "/cache", identity(), 8));
  const PdfSavedItem item = bookmark(1, 2, 0, 2);
  ASSERT_TRUE(writer.save(&item, 1));

  std::array<PdfSavedItem, 2> loaded{};
  PdfSavedItemsBuffer output{loaded.data(), static_cast<uint16_t>(loaded.size()), 0};
  PdfSourceIdentity changed = identity();
  changed.tailFingerprint ^= 1U;
  PdfSavedItemsStore changedSource;
  ASSERT_TRUE(changedSource.initialize(files.io(), "/cache", changed, 8));
  EXPECT_FALSE(changedSource.load(&output));

  PdfSavedItemsStore changedWords;
  ASSERT_TRUE(changedWords.initialize(files.io(), "/cache", identity(), 9));
  EXPECT_FALSE(changedWords.load(&output));
}

TEST(PdfSavedItemsStoreTest, EnforcesPerKindLimitsUniqueIdsAndNonEmptySelections) {
  PdfTestCacheIo files;
  files.addDirectory("/cache");
  PdfSavedItemsStore store;
  ASSERT_TRUE(store.initialize(files.io(), "/cache", identity(), 256));

  std::array<PdfSavedItem, PDF_SAVED_ITEMS_MAX_RECORDS> maximum{};
  for (uint16_t index = 0; index < PDF_SAVED_ITEMS_MAX_BOOKMARKS; ++index) {
    maximum[index] = bookmark(static_cast<uint16_t>(index + 1), index, 0, 1);
  }
  for (uint16_t index = 0; index < PDF_SAVED_ITEMS_MAX_CLIPPINGS; ++index) {
    maximum[PDF_SAVED_ITEMS_MAX_BOOKMARKS + index] =
        clipping(static_cast<uint16_t>(PDF_SAVED_ITEMS_MAX_BOOKMARKS + index + 1), 128 + index, 128 + index, 0, 0, 1);
  }
  EXPECT_TRUE(store.save(maximum.data(), maximum.size()));

  std::array<PdfSavedItem, PDF_SAVED_ITEMS_MAX_BOOKMARKS + 1> tooManyBookmarks{};
  for (uint16_t index = 0; index < tooManyBookmarks.size(); ++index) {
    tooManyBookmarks[index] = bookmark(static_cast<uint16_t>(index + 1), index, 0, 1);
  }
  EXPECT_EQ(store.save(tooManyBookmarks.data(), tooManyBookmarks.size()).error, PdfError::LimitExceeded);

  std::array<PdfSavedItem, 2> duplicate{bookmark(7, 1, 0, 1), bookmark(7, 2, 0, 1)};
  EXPECT_EQ(store.save(duplicate.data(), duplicate.size()).error, PdfError::Malformed);

  PdfSavedItem empty = clipping(9, 5, 4, 0, 0, 1);
  EXPECT_EQ(store.save(&empty, 1).error, PdfError::InvalidOffset);
}

TEST(PdfSavedItemsValidationTest, CanonicalValidatorRejectsReservedIdsAndMalformedSemanticTemplates) {
  PdfSavedItem valid = bookmark(1, 2, 0, 2);
  EXPECT_TRUE(pdfValidateSavedItem(valid, 8));

  valid.itemId = 0;
  EXPECT_EQ(pdfValidateSavedItem(valid, 8).error, PdfError::Malformed);
  valid.itemId = UINT16_MAX;
  EXPECT_EQ(pdfValidateSavedItem(valid, 8).error, PdfError::Malformed);

  valid = bookmark(1, 2, 0, 2);
  valid.flags = 0;
  EXPECT_EQ(pdfValidateSavedItem(valid, 8).error, PdfError::InvalidOffset);
  valid = bookmark(1, 8, 0, 2);
  EXPECT_EQ(pdfValidateSavedItem(valid, 8).error, PdfError::InvalidOffset);
}

TEST(PdfSavedItemsStoreTest, SelectsSequenceZeroAfterUint32Wrap) {
  PdfTestCacheIo files;
  files.addDirectory("/cache");
  PdfSavedItemsStore store;
  ASSERT_TRUE(store.initialize(files.io(), "/cache", identity(), 8));
  const PdfSavedItem first = bookmark(1, 1, 0, 2);
  ASSERT_TRUE(store.save(&first, 1));

  std::vector<uint8_t> wrapped = files.bytes("/cache/saved_items.a");
  putU32(&wrapped, PDF_SAVED_ITEMS_SEQUENCE_OFFSET, UINT32_MAX);
  putU32(&wrapped, PDF_SAVED_ITEMS_HEADER_CRC_OFFSET, pdfCacheCrc32(wrapped.data(), PDF_SAVED_ITEMS_HEADER_CRC_OFFSET));
  files.addFile("/cache/saved_items.a", wrapped);

  const PdfSavedItem second = bookmark(2, 6, 1, 2);
  ASSERT_TRUE(store.save(&second, 1));
  EXPECT_EQ(getU32(files.bytes("/cache/saved_items.b"), PDF_SAVED_ITEMS_SEQUENCE_OFFSET), 0U);

  std::array<PdfSavedItem, 2> loaded{};
  PdfSavedItemsBuffer output{loaded.data(), static_cast<uint16_t>(loaded.size()), 0};
  ASSERT_TRUE(store.load(&output));
  ASSERT_EQ(output.count, 1);
  EXPECT_EQ(loaded[0].itemId, second.itemId);
}

TEST(PdfSavedItemWordMapTest, PreservesSemanticRangeAcrossChangedPaginationInFourRecordBatches) {
  PdfSavedItem item = clipping(2, 2, 5, 0, 1, 2);
  LayoutFixture compact = compactLayout();
  PdfSavedItemPageRange compactPages;
  ASSERT_TRUE(pdfMapSavedItemWordRange(compact.source(), item, &compactPages));
  EXPECT_EQ(compactPages.sectionIndex, 2);
  EXPECT_EQ(compactPages.startPage, 0);
  EXPECT_EQ(compactPages.endPage, 1);
  EXPECT_EQ(compactPages.pageCount, 2);
  EXPECT_TRUE(compactPages.exact);

  LayoutFixture large = largeFontLayout();
  PdfSavedItemPageRange largePages;
  ASSERT_TRUE(pdfMapSavedItemWordRange(large.source(), item, &largePages));
  EXPECT_EQ(largePages.startPage, 1);
  EXPECT_EQ(largePages.endPage, 1);
  EXPECT_EQ(largePages.pageCount, 3);
  EXPECT_TRUE(largePages.exact);
  EXPECT_LE(large.maximumReadCount, 4);
  EXPECT_EQ(large.readCalls, 1U);
}

TEST(PdfSavedItemWordMapTest, MissingAnchorsFallBackToGlobalOrdinals) {
  PdfSavedItem item = clipping(2, 2, 5, 0, 1, 2);
  copyAnchor(item.startBlockAnchor, "missing");
  copyAnchor(item.endBlockAnchor, "missing");
  LayoutFixture layout = largeFontLayout();
  PdfSavedItemPageRange pages;
  ASSERT_TRUE(pdfMapSavedItemWordRange(layout.source(), item, &pages));
  EXPECT_EQ(pages.startPage, 1);
  EXPECT_EQ(pages.endPage, 1);
  EXPECT_TRUE(pages.exact);
}

TEST(PdfSavedItemWordMapTest, UsesPageFallbackOnlyWhenPaginationAndLayoutFingerprintAreUnchanged) {
  PdfSavedItem item = bookmark(1, 7, 1, 2);
  LayoutFixture unchanged = compactLayout();
  unchanged.ranges[0] = {};
  unchanged.ranges[1] = {};
  PdfSavedItemPageRange pages;
  ASSERT_TRUE(pdfMapSavedItemWordRange(unchanged.source(), item, &pages));
  EXPECT_EQ(pages.startPage, 1);
  EXPECT_EQ(pages.endPage, 1);
  EXPECT_FALSE(pages.exact);

  LayoutFixture samePageCountDifferentLayout = compactLayout();
  samePageCountDifferentLayout.ranges[0] = {};
  samePageCountDifferentLayout.ranges[1] = {};
  samePageCountDifferentLayout.layoutFingerprint ^= 1U;
  EXPECT_EQ(pdfMapSavedItemWordRange(samePageCountDifferentLayout.source(), item, &pages).error,
            PdfError::InvalidOffset);

  LayoutFixture changed = largeFontLayout();
  changed.ranges[0] = {};
  changed.ranges[1] = {};
  changed.ranges[2] = {};
  EXPECT_EQ(pdfMapSavedItemWordRange(changed.source(), item, &pages).error, PdfError::InvalidOffset);
}

TEST(PdfSavedItemWordMapTest, RejectsUnknownFlagsAndOrdinalsOutsideTheStoredSection) {
  LayoutFixture layout = compactLayout();
  PdfSavedItemPageRange pages;

  PdfSavedItem unknownFlags = bookmark(1, 2, 0, 2);
  unknownFlags.flags |= 0x80U;
  EXPECT_EQ(pdfMapSavedItemWordRange(layout.source(), unknownFlags, &pages).error, PdfError::Malformed);

  PdfSavedItem wrongSectionOrdinal = bookmark(2, 9, 1, 2);
  EXPECT_EQ(pdfMapSavedItemWordRange(layout.source(), wrongSectionOrdinal, &pages).error, PdfError::InvalidOffset);
}

TEST(PdfSavedItemWordMapTest, ScansLongIndexesInFourRecordWindows) {
  LayoutFixture layout;
  layout.info = {2, 8, 0, 8};
  layout.count = 8;
  for (uint16_t index = 0; index < layout.count; ++index) {
    layout.ranges[index] = range(index, index, index, "b00000000");
  }
  PdfSavedItem item = bookmark(1, 7, 7, 8);
  PdfSavedItemPageRange pages;
  ASSERT_TRUE(pdfMapSavedItemWordRange(layout.source(), item, &pages));
  EXPECT_EQ(pages.startPage, 7);
  EXPECT_EQ(layout.readCalls, 2U);
  EXPECT_EQ(layout.maximumReadCount, 4);
}

TEST(PdfSavedItemWordMapTest, ScansUint16MaximumPageCountWithoutWrappingTheCursor) {
  MaximumPageLayout layout;
  const PdfSavedItem item = bookmark(1, UINT16_MAX - 1U, UINT16_MAX - 1U, UINT16_MAX);
  PdfSavedItemPageRange pages;

  ASSERT_TRUE(pdfMapSavedItemWordRange(layout.source(), item, &pages));
  EXPECT_EQ(pages.startPage, UINT16_MAX - 1U);
  EXPECT_EQ(pages.endPage, UINT16_MAX - 1U);
  EXPECT_TRUE(pages.exact);
  EXPECT_EQ(layout.expectedFirstPage, static_cast<uint32_t>(UINT16_MAX));
  EXPECT_EQ(layout.readCalls, 16384U);
  EXPECT_EQ(layout.maximumReadCount, 4);
  EXPECT_EQ(layout.lastFirstPage, UINT16_MAX - 3U);
}

TEST(PdfSavedItemPageWordMapperTest, RepeatedVisibleTextUsesSemanticOrdinalsForPageLocalOffsets) {
  PdfSavedItem item = clipping(9, 12, 12, 0, 0, 1);
  PdfSavedItemPageWordMapper mapper;
  ASSERT_TRUE(mapper.begin(item, 2, 10, 12));

  // Words 0 and 1 may render the same token or split glyph fragments. The
  // semantic-attach flag keeps both on ordinal 10; the later repeated token at
  // ordinal 12 is the only saved clipping word.
  mapper.addWord(0, false);
  mapper.addWord(1, true);
  mapper.addWord(2, false);
  mapper.addWord(3, false);

  PdfSavedItemPageWordRange range;
  ASSERT_TRUE(mapper.finish(&range));
  EXPECT_EQ(range.startWord, 3);
  EXPECT_EQ(range.endWord, 3);
}

TEST(PdfSavedItemsAllocationTest, StoreAndMapperAllocateNothingAndUseOneOpenHandle) {
  FixedCacheIo files;
  PdfSavedItemsStore store;
  ASSERT_TRUE(store.initialize(files.io(), "/cache", identity(), 8));
  std::array<PdfSavedItem, 2> items{bookmark(1, 2, 0, 2), clipping(2, 2, 5, 0, 1, 2)};
  std::array<PdfSavedItem, 2> loaded{};
  PdfSavedItemsBuffer output{loaded.data(), static_cast<uint16_t>(loaded.size()), 0};
  LayoutFixture layout = compactLayout();
  PdfSavedItemPageRange pages;
  PdfStatus saveStatus;
  PdfStatus loadStatus;
  PdfStatus mapStatus;
  bool allocationFailed = false;
  size_t allocationCount = 0;

  {
    AllocationWatch watch;
    try {
      saveStatus = store.save(items.data(), items.size());
      loadStatus = store.load(&output);
      mapStatus = pdfMapSavedItemWordRange(layout.source(), loaded[1], &pages);
    } catch (const std::bad_alloc&) {
      allocationFailed = true;
    }
    allocationCount = watch.count();
  }

  EXPECT_FALSE(allocationFailed);
  EXPECT_EQ(allocationCount, 0U);
  EXPECT_TRUE(saveStatus);
  EXPECT_TRUE(loadStatus);
  EXPECT_TRUE(mapStatus);
  EXPECT_FALSE(files.concurrentOpenAttempted());
  EXPECT_EQ(output.count, 2);
  EXPECT_EQ(pages.startPage, 0);
  EXPECT_EQ(pages.endPage, 1);
}
