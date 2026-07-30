#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <cstdlib>
#include <new>

#include "PdfIo.h"
#include "PdfLexer.h"
#include "PdfObjectParser.h"
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

void* operator new[](const std::size_t size) { return ::operator new(size); }

void operator delete(void* memory) noexcept { std::free(memory); }

void operator delete[](void* memory) noexcept { std::free(memory); }

void operator delete(void* memory, std::size_t) noexcept { std::free(memory); }

void operator delete[](void* memory, std::size_t) noexcept { std::free(memory); }

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

TEST(PdfAllocationTest, LexerAndObjectParserAllocateNothingWhenFailureInterceptorIsArmed) {
  PdfTestByteSource memory({'<', '<', ' ', '/', 'T', 'y', 'p', 'e', ' ', '/', 'P', 'a', 'g', 'e', ' ', '/',
                            'P', 'a', 'r', 'e', 'n', 't', ' ', '2', ' ', '0', ' ', 'R', ' ', '>', '>'});
  const PdfByteSource source = memory.source();
  std::array<uint8_t, 4096> sourceBuffer{};
  std::array<PdfValue, 16> values{};
  std::array<PdfDictionaryEntry, 8> dictionaries{};
  std::array<PdfArrayItem, 4> arrays{};
  std::array<uint8_t, 64> text{};
  PdfObjectArena arena{
      values.data(),       static_cast<uint16_t>(values.size()),
      dictionaries.data(), static_cast<uint16_t>(dictionaries.size()),
      arrays.data(),       static_cast<uint16_t>(arrays.size()),
      text.data(),         static_cast<uint16_t>(text.size()),
  };
  PdfLexer lexer(source, sourceBuffer.data(), sourceBuffer.size());
  PdfObjectParser parser(lexer, arena);
  parser.begin();
  PdfStepResult result;
  bool allocationFailed = false;
  size_t allocationCount = 0;

  {
    AllocationWatch watch;
    try {
      do {
        PdfWorkBudget budget{1, 1};
        result = parser.step(budget);
      } while (result.yielded());
    } catch (const std::bad_alloc&) {
      allocationFailed = true;
    }
    allocationCount = watch.count();
  }

  EXPECT_FALSE(allocationFailed);
  ASSERT_TRUE(result.complete());
  EXPECT_EQ(allocationCount, 0u);
  uint16_t typeIndex = PDF_INVALID_INDEX;
  ASSERT_TRUE(pdfDictionaryFind(arena, parser.rootIndex(), "Type", &typeIndex));
  EXPECT_TRUE(pdfTextEquals(arena, arena.values[typeIndex], "Page"));
}
