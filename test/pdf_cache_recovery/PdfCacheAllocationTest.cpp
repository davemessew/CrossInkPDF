#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <new>

#include "PdfBuildCheckpoint.h"
#include "PdfCacheManifest.h"
#include "PdfCacheStore.h"

namespace {

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

struct FixedBytes {
  std::array<uint8_t, 256> bytes{};
  size_t length = 0;

  static PdfStatus write(void* context, const uint8_t* source, const size_t requested, size_t* bytesWritten) {
    if (context == nullptr || source == nullptr || bytesWritten == nullptr) {
      return PdfStatus::failure(PdfError::InvalidArgument);
    }
    auto& self = *static_cast<FixedBytes*>(context);
    if (requested > self.bytes.size() - self.length) {
      return PdfStatus::failure(PdfError::InsufficientStorage, self.length);
    }
    std::memcpy(self.bytes.data() + self.length, source, requested);
    self.length += requested;
    *bytesWritten = requested;
    return PdfStatus::success();
  }

  static PdfStatus read(void* context, const uint64_t offset, uint8_t* destination, const size_t requested,
                        size_t* bytesRead) {
    if (context == nullptr || destination == nullptr || bytesRead == nullptr) {
      return PdfStatus::failure(PdfError::InvalidArgument, offset);
    }
    auto& self = *static_cast<FixedBytes*>(context);
    if (offset > self.length) {
      return PdfStatus::failure(PdfError::InvalidOffset, offset);
    }
    const size_t count = std::min(requested, self.length - static_cast<size_t>(offset));
    std::memcpy(destination, self.bytes.data() + static_cast<size_t>(offset), count);
    *bytesRead = count;
    return PdfStatus::success();
  }

  PdfByteSink sink() { return {this, write}; }
  PdfByteSource source() { return {this, length, read}; }
};

struct SingleRecordTable {
  PdfRequiredFileRecord record{};

  static PdfStatus read(void* context, const uint32_t index, PdfRequiredFileRecord* output) {
    if (context == nullptr || output == nullptr || index != 0) {
      return PdfStatus::failure(PdfError::InvalidArgument, index);
    }
    *output = static_cast<SingleRecordTable*>(context)->record;
    return PdfStatus::success();
  }

  PdfRequiredFileTableSource source() { return {this, 1, read}; }
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

TEST(PdfCacheAllocation, CodecsAndPolicyAllocateNothing) {
  SingleRecordTable table;
  constexpr char path[] = "metadata.bin";
  table.record.pathLength = sizeof(path) - 1;
  std::memcpy(table.record.path, path, sizeof(path));
  table.record.size = 4;
  table.record.crc32 = 0x12345678;

  PdfCacheManifest expected{};
  expected.sequence = 7;
  expected.completed = true;
  expected.source = {123, {false, 0}, 11, 22};
  expected.generation = 9;
  expected.totalWords = 2;
  expected.requiredFileCount = 1;
  expected.requiredFileBytes = table.record.size;
  expected.requiredFileLedger = pdfUpdateRequiredFileLedger(PDF_CACHE_FNV64_OFFSET, table.record);
  PdfBuildCheckpoint checkpoint{};
  checkpoint.sequence = 7;
  checkpoint.source = expected.source;
  checkpoint.generation = 9;
  checkpoint.phase = PdfBuildPhase::EmitSections;
  FixedBytes manifestBytes;
  FixedBytes checkpointBytes;
  PdfCacheManifest decodedManifest{};
  PdfBuildCheckpoint decodedCheckpoint{};
  PdfCacheBudget budget{};
  PdfStatus manifestEncodeStatus;
  PdfStatus manifestDecodeStatus;
  PdfStatus checkpointEncodeStatus;
  PdfStatus checkpointDecodeStatus;
  PdfStatus budgetStatus;
  bool allocationFailed = false;
  size_t allocationCount = 0;

  {
    AllocationWatch watch;
    try {
      manifestEncodeStatus = pdfEncodeCacheManifest(expected, table.source(), manifestBytes.sink());
      manifestDecodeStatus = pdfDecodeCacheManifest(manifestBytes.source(), &decodedManifest, {});
      checkpointEncodeStatus = pdfEncodeBuildCheckpoint(checkpoint, checkpointBytes.sink());
      checkpointDecodeStatus = pdfDecodeBuildCheckpoint(checkpointBytes.source(), &decodedCheckpoint);
      budgetStatus = pdfInitializeCacheBudget(
          expected.source.size, {{true, 64ULL * 1024ULL * 1024ULL}, {true, 32ULL * 1024ULL * 1024ULL}}, 1024, &budget);
    } catch (const std::bad_alloc&) {
      allocationFailed = true;
    }
    allocationCount = watch.count();
  }

  EXPECT_FALSE(allocationFailed);
  EXPECT_EQ(allocationCount, 0u);
  EXPECT_TRUE(manifestEncodeStatus.ok());
  EXPECT_TRUE(manifestDecodeStatus.ok());
  EXPECT_TRUE(checkpointEncodeStatus.ok());
  EXPECT_TRUE(checkpointDecodeStatus.ok());
  EXPECT_TRUE(budgetStatus.ok());
  EXPECT_EQ(decodedManifest.sequence, expected.sequence);
  EXPECT_EQ(decodedCheckpoint.sequence, checkpoint.sequence);
}
