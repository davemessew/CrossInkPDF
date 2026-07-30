#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <new>
#include <string>
#include <vector>

#include "PdfIo.h"
#include "PdfLexer.h"
#include "PdfObjectParser.h"
#include "PdfStreamDecoder.h"
#include "PdfTestIo.h"
#include "PdfXref.h"

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

struct FixedSink {
  std::array<uint8_t, 32> bytes{};
  size_t length = 0;

  static PdfStatus write(void* context, const uint8_t* source, const size_t requested, size_t* bytesWritten) {
    if (context == nullptr || source == nullptr || bytesWritten == nullptr) {
      return PdfStatus::failure(PdfError::InvalidArgument);
    }
    auto& sink = *static_cast<FixedSink*>(context);
    if (requested > sink.bytes.size() - sink.length) {
      return PdfStatus::failure(PdfError::InsufficientStorage, sink.length);
    }
    std::memcpy(sink.bytes.data() + sink.length, source, requested);
    sink.length += requested;
    *bytesWritten = requested;
    return PdfStatus::success();
  }

  PdfByteSink sink() { return {this, write}; }
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

TEST(PdfAllocationTest, ExternalDictionaryFlateCoreAllocatesNothingWhenInterceptorIsArmed) {
  const std::vector<uint8_t> encoded{0x78, 0xda, 0xf3, 0x48, 0xcd, 0xc9, 0xc9, 0x57, 0x08,
                                     0x70, 0x71, 0x03, 0x00, 0x0f, 0x9e, 0x02, 0xef};
  PdfTestByteSource input(encoded);
  std::array<uint8_t, PdfLimits::SourceBufferBytes> sourceBuffer{};
  std::array<uint8_t, PdfLimits::DecoderOutputBytes> outputBuffer{};
  std::array<uint8_t, PdfLimits::UzlibDictionaryBytes> dictionary{};
  FixedSink output;
  PdfStreamDecoder decoder({sourceBuffer.data(), sourceBuffer.size(), outputBuffer.data(), outputBuffer.size(),
                            dictionary.data(), dictionary.size()});
  PdfStepResult result;
  PdfStatus beginStatus;
  bool allocationFailed = false;
  size_t allocationCount = 0;

  {
    AllocationWatch watch;
    try {
      const std::array<PdfStreamFilter, 1> filters{PdfStreamFilter::Flate};
      beginStatus = decoder.begin(input.source(), output.sink(), filters.data(), filters.size());
      if (beginStatus.ok()) {
        do {
          PdfWorkBudget budget{32, 4096};
          result = decoder.step(budget);
        } while (result.yielded());
      }
    } catch (const std::bad_alloc&) {
      allocationFailed = true;
    }
    allocationCount = watch.count();
  }

  EXPECT_FALSE(allocationFailed);
  ASSERT_TRUE(beginStatus.ok());
  ASSERT_TRUE(result.complete());
  EXPECT_TRUE(decoder.usesExternalDictionary());
  EXPECT_EQ(allocationCount, 0u);
  EXPECT_EQ(std::string(reinterpret_cast<const char*>(output.bytes.data()), output.length), "Hello PDF");
}

TEST(PdfAllocationTest, XrefExternalMergeAllocatesNothingWhenFailureInterceptorIsArmed) {
  constexpr uint32_t ENTRY_COUNT = PdfLimits::XrefMergeEntries + 3;
  PdfTestRecordStore records(sizeof(PdfXrefEntry), ENTRY_COUNT);
  PdfTestRecordStore scratch(sizeof(PdfXrefEntry), ENTRY_COUNT);
  PdfXrefTable table(records.store());
  std::array<PdfXrefEntry, PdfLimits::XrefMergeEntries> mergeBuffer{};
  PdfStatus status = PdfStatus::success();
  bool allocationFailed = false;
  size_t allocationCount = 0;

  {
    AllocationWatch watch;
    try {
      for (uint32_t index = 0; index < ENTRY_COUNT && status.ok(); ++index) {
        PdfXrefEntry entry{};
        entry.objectNumber = ENTRY_COUNT - index;
        entry.type = PdfXrefEntryType::Uncompressed;
        entry.offset = index;
        status = table.appendNewest(entry);
      }
      if (status.ok()) {
        status = table.finalize(scratch.store(), mergeBuffer.data(), mergeBuffer.size());
      }
    } catch (const std::bad_alloc&) {
      allocationFailed = true;
    }
    allocationCount = watch.count();
  }

  EXPECT_FALSE(allocationFailed);
  EXPECT_TRUE(status.ok());
  EXPECT_TRUE(table.finalized());
  EXPECT_EQ(allocationCount, 0u);
}
