#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <cstdlib>
#include <new>

#include "PdfIo.h"
#include "PdfTestIo.h"

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

void* operator new[](const std::size_t size) {
  return ::operator new(size);
}

void operator delete(void* memory) noexcept {
  std::free(memory);
}

void operator delete[](void* memory) noexcept {
  std::free(memory);
}

void operator delete(void* memory, std::size_t) noexcept {
  std::free(memory);
}

void operator delete[](void* memory, std::size_t) noexcept {
  std::free(memory);
}

TEST(PdfAllocationTest, HotReadCoreAllocatesNothingWhenFailureInterceptorIsArmed) {
  PdfTestByteSource memory({1, 2, 3, 4});
  const PdfByteSource source = memory.source();
  std::array<uint8_t, 4> output{};
  PdfStatus status;
  size_t allocationCount = 0;

  {
    AllocationWatch watch;
    EXPECT_NO_THROW(status = pdfReadExact(source, 0, output.data(), output.size()));
    allocationCount = watch.count();
  }

  EXPECT_TRUE(status.ok());
  EXPECT_EQ(allocationCount, 0u);
  EXPECT_EQ(output, (std::array<uint8_t, 4>{1, 2, 3, 4}));
}
