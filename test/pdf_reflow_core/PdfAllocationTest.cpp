#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <new>
#include <string>
#include <vector>

#include "PdfCMap.h"
#include "PdfContentInterpreter.h"
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

struct AllocationResources {
  PdfFontMap* font = nullptr;

  static PdfStatus resolveFont(void* context, const uint8_t* name, const size_t length, PdfFontMap** font) {
    if (context == nullptr || name == nullptr || font == nullptr || length != 2 || name[0] != 'F' || name[1] != '1') {
      return PdfStatus::failure(PdfError::UnsupportedEncoding);
    }
    *font = static_cast<AllocationResources*>(context)->font;
    return *font == nullptr ? PdfStatus::failure(PdfError::UnsupportedEncoding) : PdfStatus::success();
  }
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

TEST(PdfAllocationTest, CMapAndContentInterpreterAllocateNothingWhenInterceptorIsArmed) {
  const std::vector<uint8_t> cmapBytes{
      '1', ' ', 'b', 'e', 'g', 'i', 'n', 'c', 'o', 'd', 'e', 's', 'p', 'a', 'c', 'e', 'r', 'a', 'n', 'g', 'e', ' ',
      '<', '0', '0', '>', ' ', '<', 'F', 'F', '>', ' ', 'e', 'n', 'd', 'c', 'o', 'd', 'e', 's', 'p', 'a', 'c', 'e',
      'r', 'a', 'n', 'g', 'e', ' ', '1', ' ', 'b', 'e', 'g', 'i', 'n', 'b', 'f', 'c', 'h', 'a', 'r', ' ', '<', '4',
      '1', '>', ' ', '<', '0', '0', '4', '1', '>', ' ', 'e', 'n', 'd', 'b', 'f', 'c', 'h', 'a', 'r'};
  const std::vector<uint8_t> contentBytes{'B', 'T', ' ', '/', 'F', '1', ' ', '1', '2', ' ', 'T',
                                          'f', ' ', '(', 'A', ')', ' ', 'T', 'j', ' ', 'E', 'T'};
  PdfTestByteSource cmapInput(cmapBytes);
  PdfTestByteSource contentInput(contentBytes);
  std::array<uint8_t, 128> cmapSourceBuffer{};
  std::array<PdfCMapRecord, 2> cmapRecords{};
  PdfCMap cmap(cmapSourceBuffer.data(), cmapSourceBuffer.size(),
               {cmapRecords.data(), static_cast<uint16_t>(cmapRecords.size())});
  std::array<PdfEncodingDifference, 1> differences{};
  PdfSimpleEncoding encoding({differences.data(), static_cast<uint16_t>(differences.size())});
  std::array<PdfFontWidthRecord, 1> widths{};
  PdfFontMap font({widths.data(), static_cast<uint16_t>(widths.size())});
  ASSERT_TRUE(encoding.begin(PdfBaseEncoding::Standard).ok());
  ASSERT_TRUE(font.begin(1, false, &cmap, &encoding).ok());
  ASSERT_TRUE(font.addWidth(0, 255, 500).ok());
  AllocationResources resourceContext{&font};
  PdfContentResources resources{&resourceContext, AllocationResources::resolveFont, nullptr};
  std::array<uint8_t, 128> contentSourceBuffer{};
  std::array<PdfContentOperand, 8> operands{};
  std::array<PdfContentArrayItem, 8> arrayItems{};
  std::array<uint8_t, 128> scratchText{};
  std::array<uint8_t, 64> markedText{};
  std::array<uint8_t, 64> pageText{};
  std::array<PdfTextRun, 4> runs{};
  std::array<PdfImagePlacement, 1> images{};
  PdfPageModel model({pageText.data(), pageText.size(), runs.data(), runs.size(), images.data(), images.size()});
  PdfContentInterpreter interpreter({contentSourceBuffer.data(), contentSourceBuffer.size(), operands.data(),
                                     operands.size(), arrayItems.data(), arrayItems.size(), scratchText.data(),
                                     scratchText.size(), markedText.data(), markedText.size(), nullptr});
  const PdfByteSource contentSource = contentInput.source();
  PdfStepResult cmapResult;
  PdfStepResult contentResult;
  PdfStatus cmapBegin;
  PdfStatus contentBegin;
  PdfStatus lookupStatus;
  bool allocationFailed = false;
  size_t allocationCount = 0;

  {
    AllocationWatch watch;
    try {
      cmapBegin = cmap.begin(cmapInput.source());
      if (cmapBegin.ok()) {
        do {
          PdfWorkBudget budget{4, 32};
          cmapResult = cmap.step(budget);
        } while (cmapResult.yielded());
      }
      const uint8_t encoded = 0x41;
      PdfCMapLookup lookup;
      if (cmapResult.complete()) {
        lookupStatus = cmap.lookup(&encoded, 1, &lookup);
      }
      contentBegin = interpreter.begin(&contentSource, 1, resources, model);
      if (contentBegin.ok()) {
        do {
          PdfWorkBudget budget{4, 32};
          contentResult = interpreter.step(budget);
        } while (contentResult.yielded());
      }
    } catch (const std::bad_alloc&) {
      allocationFailed = true;
    }
    allocationCount = watch.count();
  }

  EXPECT_FALSE(allocationFailed);
  EXPECT_TRUE(cmapBegin.ok());
  EXPECT_TRUE(cmapResult.complete());
  EXPECT_TRUE(lookupStatus.ok());
  EXPECT_TRUE(contentBegin.ok());
  EXPECT_TRUE(contentResult.complete());
  EXPECT_EQ(allocationCount, 0u);
}
