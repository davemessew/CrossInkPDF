#include <PdfPixelCacheWriter.h>
#include <PixelCache.h>
#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <new>

namespace {

std::atomic<bool> gTrackAllocations{false};
std::atomic<size_t> gNewCount{0};
std::atomic<size_t> gMallocCount{0};
std::atomic<size_t> gFreeCount{0};

}  // namespace

void* operator new(const std::size_t size) {
  if (gTrackAllocations.load(std::memory_order_relaxed)) {
    gNewCount.fetch_add(1, std::memory_order_relaxed);
  }
  if (void* const memory = std::malloc(size)) {
    return memory;
  }
  throw std::bad_alloc();
}

void* operator new[](const std::size_t size) {
  if (gTrackAllocations.load(std::memory_order_relaxed)) {
    gNewCount.fetch_add(1, std::memory_order_relaxed);
  }
  if (void* const memory = std::malloc(size)) {
    return memory;
  }
  throw std::bad_alloc();
}

void operator delete(void* const memory) noexcept { std::free(memory); }
void operator delete[](void* const memory) noexcept { std::free(memory); }
void operator delete(void* const memory, std::size_t) noexcept { std::free(memory); }
void operator delete[](void* const memory, std::size_t) noexcept { std::free(memory); }

#if defined(PDF_PIXEL_CACHE_WRAP_MALLOC)
extern "C" void* __real_malloc(std::size_t size);
extern "C" void __real_free(void* memory);

extern "C" void* __wrap_malloc(const std::size_t size) {
  if (gTrackAllocations.load(std::memory_order_relaxed)) {
    gMallocCount.fetch_add(1, std::memory_order_relaxed);
  }
  return __real_malloc(size);
}

extern "C" void __wrap_free(void* const memory) {
  if (gTrackAllocations.load(std::memory_order_relaxed)) {
    gFreeCount.fetch_add(1, std::memory_order_relaxed);
  }
  __real_free(memory);
}
#endif

namespace {

using pixel_cache::Layout;
using pixel_cache::Status;

template <size_t Capacity>
struct FixedSink {
  std::array<uint8_t, Capacity> bytes{};
  size_t size = 0;
  size_t calls = 0;
  size_t maxBytesPerWrite = Capacity;
  size_t failCall = std::numeric_limits<size_t>::max();
  PdfStatus forcedFailure = PdfStatus::failure(PdfError::InsufficientStorage, 91);

  static PdfStatus write(void* context, const uint8_t* source, const size_t requested, size_t* bytesWritten) {
    auto& self = *static_cast<FixedSink*>(context);
    if (source == nullptr || bytesWritten == nullptr) {
      return PdfStatus::failure(PdfError::InvalidArgument);
    }
    const size_t call = self.calls++;
    if (call == self.failCall) {
      *bytesWritten = 0;
      return self.forcedFailure;
    }

    const size_t actual = requested < self.maxBytesPerWrite ? requested : self.maxBytesPerWrite;
    if (actual > self.bytes.size() - self.size) {
      *bytesWritten = 0;
      return PdfStatus::failure(PdfError::InsufficientStorage);
    }
    for (size_t i = 0; i < actual; ++i) {
      self.bytes[self.size + i] = source[i];
    }
    self.size += actual;
    *bytesWritten = actual;
    return PdfStatus::success();
  }

  PdfByteSink sink() { return {this, &FixedSink::write}; }
};

TEST(PixelCacheLayout, UsesLegacyCeilingBytesPerRow) {
  struct Case {
    size_t width;
    size_t expectedBytesPerRow;
  };
  constexpr std::array<Case, 4> cases{{
      {1, 1},
      {3, 1},
      {4, 1},
      {5, 2},
  }};

  for (const auto& testCase : cases) {
    Layout layout{};
    ASSERT_EQ(pixel_cache::calculateLayout(testCase.width, 7, layout), Status::Ok);
    EXPECT_EQ(layout.width, testCase.width);
    EXPECT_EQ(layout.height, 7);
    EXPECT_EQ(layout.bytesPerRow, testCase.expectedBytesPerRow);
    EXPECT_EQ(layout.payloadBytes, testCase.expectedBytesPerRow * 7);
    EXPECT_EQ(layout.fileBytes, pixel_cache::kHeaderSize + testCase.expectedBytesPerRow * 7);
  }
}

TEST(PixelCacheLayout, EncodesAndDecodesLegacyLittleEndianHeader) {
  Layout layout{};
  ASSERT_EQ(pixel_cache::calculateLayout(0x1234, 0x5678, layout), Status::Ok);

  std::array<uint8_t, pixel_cache::kHeaderSize> header{};
  pixel_cache::encodeHeader(layout, header.data());
  EXPECT_EQ(header, (std::array<uint8_t, 4>{0x34, 0x12, 0x78, 0x56}));

  Layout decoded{};
  ASSERT_EQ(pixel_cache::decodeHeader(header.data(), header.size(), decoded), Status::Ok);
  EXPECT_EQ(decoded.width, layout.width);
  EXPECT_EQ(decoded.height, layout.height);
  EXPECT_EQ(decoded.bytesPerRow, layout.bytesPerRow);
  EXPECT_EQ(decoded.payloadBytes, layout.payloadBytes);
  EXPECT_EQ(decoded.fileBytes, layout.fileBytes);
}

TEST(PixelCacheLayout, RejectsInvalidDimensionsWithoutPublishingPartialLayout) {
  constexpr std::array<std::array<size_t, 2>, 4> invalidDimensions{{
      {0, 1},
      {1, 0},
      {static_cast<size_t>(std::numeric_limits<uint16_t>::max()) + 1U, 1},
      {1, static_cast<size_t>(std::numeric_limits<uint16_t>::max()) + 1U},
  }};

  for (const auto& dimensions : invalidDimensions) {
    Layout layout{9, 9, 9, 9, 9};
    EXPECT_EQ(pixel_cache::calculateLayout(dimensions[0], dimensions[1], layout), Status::InvalidDimensions);
    EXPECT_EQ(layout.width, 0);
    EXPECT_EQ(layout.height, 0);
    EXPECT_EQ(layout.bytesPerRow, 0U);
    EXPECT_EQ(layout.payloadBytes, 0U);
    EXPECT_EQ(layout.fileBytes, 0U);
  }
}

TEST(PixelCacheLayout, DetectsSizeOverflowBeforeNarrowingDimensions) {
  Layout layout{9, 9, 9, 9, 9};
  EXPECT_EQ(
      pixel_cache::calculateLayout(std::numeric_limits<size_t>::max(), std::numeric_limits<size_t>::max(), layout),
      Status::SizeOverflow);
  EXPECT_EQ(layout.fileBytes, 0U);
}

TEST(PixelCacheLayout, RejectsMissingOrTruncatedHeaders) {
  Layout layout{9, 9, 9, 9, 9};
  const std::array<uint8_t, 3> shortHeader{1, 0, 1};

  EXPECT_EQ(pixel_cache::decodeHeader(nullptr, pixel_cache::kHeaderSize, layout), Status::InvalidArgument);
  EXPECT_EQ(pixel_cache::decodeHeader(shortHeader.data(), shortHeader.size(), layout), Status::InvalidArgument);
  EXPECT_EQ(layout.fileBytes, 0U);
}

TEST(PdfPixelCacheWriter, ProducesByteIdenticalLegacyFixturesForWidthsOneThreeFourAndFive) {
  struct Fixture {
    uint16_t width;
    std::array<uint8_t, 5> pixels;
    std::array<uint8_t, 2> packed;
    size_t packedSize;
  };
  constexpr std::array<Fixture, 4> fixtures{{
      {1, {3, 0, 0, 0, 0}, {0xc0, 0x00}, 1},
      {3, {0, 1, 2, 0, 0}, {0x18, 0x00}, 1},
      {4, {0, 1, 2, 3, 0}, {0x1b, 0x00}, 1},
      {5, {3, 2, 1, 0, 2}, {0xe4, 0x80}, 2},
  }};

  for (const auto& fixture : fixtures) {
    FixedSink<16> sink;
    PdfPixelCacheWriter writer;
    ASSERT_TRUE(writer.begin(sink.sink(), fixture.width, 1).ok());
    ASSERT_TRUE(writer.writeRow(0, fixture.pixels.data(), fixture.width).ok());
    ASSERT_TRUE(writer.finish().ok());

    ASSERT_EQ(sink.size, pixel_cache::kHeaderSize + fixture.packedSize);
    EXPECT_EQ(sink.bytes[0], static_cast<uint8_t>(fixture.width));
    EXPECT_EQ(sink.bytes[1], 0);
    EXPECT_EQ(sink.bytes[2], 1);
    EXPECT_EQ(sink.bytes[3], 0);
    for (size_t i = 0; i < fixture.packedSize; ++i) {
      EXPECT_EQ(sink.bytes[pixel_cache::kHeaderSize + i], fixture.packed[i]);
    }
  }
}

TEST(PdfPixelCacheWriter, PreservesRowsAndZeroesEveryUnusedPaddingBit) {
  constexpr std::array<uint8_t, 5> firstRow{3, 3, 3, 3, 3};
  constexpr std::array<uint8_t, 5> secondRow{1, 2, 3, 0, 1};
  constexpr std::array<uint8_t, 8> expected{
      0x05, 0x00, 0x02, 0x00, 0xff, 0xc0, 0x6c, 0x40,
  };

  FixedSink<16> sink;
  PdfPixelCacheWriter writer;
  ASSERT_TRUE(writer.begin(sink.sink(), 5, 2).ok());
  ASSERT_TRUE(writer.writeRow(0, firstRow.data(), firstRow.size()).ok());
  ASSERT_TRUE(writer.writeRow(1, secondRow.data(), secondRow.size()).ok());
  ASSERT_TRUE(writer.finish().ok());

  ASSERT_EQ(sink.size, expected.size());
  for (size_t i = 0; i < expected.size(); ++i) {
    EXPECT_EQ(sink.bytes[i], expected[i]);
  }
  EXPECT_EQ(sink.bytes[5] & 0x3fU, 0U);
  EXPECT_EQ(sink.bytes[7] & 0x3fU, 0U);
}

TEST(PdfPixelCacheWriter, RejectsOutOfOrderRowsBeforeWritingPayload) {
  constexpr std::array<uint8_t, 5> row{};
  FixedSink<16> sink;
  PdfPixelCacheWriter writer;
  ASSERT_TRUE(writer.begin(sink.sink(), row.size(), 2).ok());

  const PdfStatus status = writer.writeRow(1, row.data(), row.size());

  EXPECT_EQ(status.error, PdfError::InvalidOffset);
  EXPECT_EQ(status.offset, 1U);
  EXPECT_EQ(writer.rowsWritten(), 0U);
  EXPECT_EQ(sink.size, pixel_cache::kHeaderSize);
}

TEST(PdfPixelCacheWriter, RejectsRowsWithTheWrongPixelCountBeforeWritingPayload) {
  constexpr std::array<uint8_t, 6> oversizedRow{};
  FixedSink<16> sink;
  PdfPixelCacheWriter writer;
  ASSERT_TRUE(writer.begin(sink.sink(), 5, 1).ok());

  const PdfStatus status = writer.writeRow(0, oversizedRow.data(), oversizedRow.size());

  EXPECT_EQ(status.error, PdfError::InvalidArgument);
  EXPECT_EQ(status.offset, oversizedRow.size());
  EXPECT_EQ(writer.rowsWritten(), 0U);
  EXPECT_EQ(sink.size, pixel_cache::kHeaderSize);
}

TEST(PdfPixelCacheWriter, RejectsRowsBeyondTheDeclaredHeight) {
  constexpr std::array<uint8_t, 4> row{};
  FixedSink<16> sink;
  PdfPixelCacheWriter writer;
  ASSERT_TRUE(writer.begin(sink.sink(), row.size(), 1).ok());
  ASSERT_TRUE(writer.writeRow(0, row.data(), row.size()).ok());

  const PdfStatus status = writer.writeRow(1, row.data(), row.size());

  EXPECT_EQ(status.error, PdfError::InvalidOffset);
  EXPECT_EQ(status.offset, 1U);
  EXPECT_EQ(writer.rowsWritten(), 1U);
  EXPECT_EQ(sink.size, pixel_cache::kHeaderSize + 1U);
}

TEST(PdfPixelCacheWriter, RejectsFinishUntilEveryDeclaredRowWasWritten) {
  constexpr std::array<uint8_t, 4> row{};
  FixedSink<16> sink;
  PdfPixelCacheWriter writer;
  ASSERT_TRUE(writer.begin(sink.sink(), row.size(), 2).ok());
  ASSERT_TRUE(writer.writeRow(0, row.data(), row.size()).ok());

  const PdfStatus status = writer.finish();

  EXPECT_EQ(status.error, PdfError::UnexpectedEof);
  EXPECT_EQ(status.offset, 1U);
}

TEST(PdfPixelCacheWriter, RejectsInvalidDimensionsAndSizeOverflowBeforeCallingSink) {
  FixedSink<16> sink;
  PdfPixelCacheWriter zeroWidth;
  PdfPixelCacheWriter overflowing;

  const PdfStatus invalidStatus = zeroWidth.begin(sink.sink(), 0, 1);
  const PdfStatus overflowStatus =
      overflowing.begin(sink.sink(), std::numeric_limits<size_t>::max(), std::numeric_limits<size_t>::max());

  EXPECT_EQ(invalidStatus.error, PdfError::InvalidArgument);
  EXPECT_EQ(overflowStatus.error, PdfError::LimitExceeded);
  EXPECT_EQ(sink.calls, 0U);
}

TEST(PdfPixelCacheWriter, TreatsShortHeaderWriteAsIoFailure) {
  FixedSink<16> sink;
  sink.maxBytesPerWrite = 2;
  PdfPixelCacheWriter writer;

  const PdfStatus status = writer.begin(sink.sink(), 4, 1);

  EXPECT_EQ(status.error, PdfError::IoFailure);
  EXPECT_EQ(status.offset, 2U);
  EXPECT_EQ(writer.bytesWritten(), 2U);
  EXPECT_EQ(sink.calls, 1U);
}

TEST(PdfPixelCacheWriter, TreatsShortRowWriteAsIoFailure) {
  constexpr std::array<uint8_t, 5> row{0, 1, 2, 3, 0};
  FixedSink<16> sink;
  PdfPixelCacheWriter writer;
  ASSERT_TRUE(writer.begin(sink.sink(), row.size(), 1).ok());
  sink.maxBytesPerWrite = 1;

  const PdfStatus status = writer.writeRow(0, row.data(), row.size());

  EXPECT_EQ(status.error, PdfError::IoFailure);
  EXPECT_EQ(status.offset, pixel_cache::kHeaderSize + 1U);
  EXPECT_EQ(writer.rowsWritten(), 0U);
}

TEST(PdfPixelCacheWriter, PropagatesSinkErrorsExactlyAndKeepsThemSticky) {
  constexpr std::array<uint8_t, 4> row{};
  FixedSink<16> sink;
  PdfPixelCacheWriter writer;
  ASSERT_TRUE(writer.begin(sink.sink(), row.size(), 1).ok());
  sink.failCall = sink.calls;
  sink.forcedFailure = PdfStatus::failure(PdfError::InsufficientStorage, 1234);

  const PdfStatus first = writer.writeRow(0, row.data(), row.size());
  const size_t callsAfterFailure = sink.calls;
  const PdfStatus second = writer.writeRow(0, row.data(), row.size());
  const PdfStatus finish = writer.finish();

  EXPECT_EQ(first.error, PdfError::InsufficientStorage);
  EXPECT_EQ(first.offset, 1234U);
  EXPECT_EQ(second.error, first.error);
  EXPECT_EQ(second.offset, first.offset);
  EXPECT_EQ(finish.error, first.error);
  EXPECT_EQ(finish.offset, first.offset);
  EXPECT_EQ(sink.calls, callsAfterFailure);
  EXPECT_EQ(writer.rowsWritten(), 0U);
}

TEST(PdfPixelCacheWriter, RejectsOutOfRangePixelsBeforeWritingPayloadAndKeepsTheFailureSticky) {
  std::array<uint8_t, 513> row{};
  row[512] = 4;
  FixedSink<256> sink;
  PdfPixelCacheWriter writer;
  ASSERT_TRUE(writer.begin(sink.sink(), row.size(), 1).ok());

  const PdfStatus first = writer.writeRow(0, row.data(), row.size());
  const size_t callsAfterFailure = sink.calls;
  const PdfStatus second = writer.writeRow(0, row.data(), row.size());
  const PdfStatus finish = writer.finish();

  EXPECT_EQ(first.error, PdfError::InvalidArgument);
  EXPECT_EQ(first.offset, 512U);
  EXPECT_EQ(second.error, first.error);
  EXPECT_EQ(second.offset, first.offset);
  EXPECT_EQ(finish.error, first.error);
  EXPECT_EQ(finish.offset, first.offset);
  EXPECT_EQ(sink.calls, callsAfterFailure);
  EXPECT_EQ(sink.size, pixel_cache::kHeaderSize);
  EXPECT_EQ(writer.rowsWritten(), 0U);
}

TEST(PdfPixelCacheWriter, PreservesWriteRowFailureWhenBeginIsCalledLater) {
  constexpr std::array<uint8_t, 1> row{};
  FixedSink<16> sink;
  PdfPixelCacheWriter writer;

  const PdfStatus first = writer.writeRow(0, row.data(), row.size());
  const PdfStatus begin = writer.begin(sink.sink(), row.size(), 1);
  const PdfStatus finish = writer.finish();

  EXPECT_EQ(first.error, PdfError::InvalidArgument);
  EXPECT_EQ(first.offset, row.size());
  EXPECT_EQ(begin.error, first.error);
  EXPECT_EQ(begin.offset, first.offset);
  EXPECT_EQ(finish.error, first.error);
  EXPECT_EQ(finish.offset, first.offset);
  EXPECT_EQ(sink.calls, 0U);
}

TEST(PdfPixelCacheWriter, PreservesFinishFailureWhenBeginIsCalledLater) {
  FixedSink<16> sink;
  PdfPixelCacheWriter writer;

  const PdfStatus first = writer.finish();
  const PdfStatus begin = writer.begin(sink.sink(), 1, 1);
  const PdfStatus secondFinish = writer.finish();

  EXPECT_EQ(first.error, PdfError::InvalidArgument);
  EXPECT_EQ(first.offset, 0U);
  EXPECT_EQ(begin.error, first.error);
  EXPECT_EQ(begin.offset, first.offset);
  EXPECT_EQ(secondFinish.error, first.error);
  EXPECT_EQ(secondFinish.offset, first.offset);
  EXPECT_EQ(sink.calls, 0U);
}

TEST(PdfPixelCacheAllocationWitness, PositiveControlDetectsNewMallocAndFree) {
  gNewCount.store(0, std::memory_order_relaxed);
  gMallocCount.store(0, std::memory_order_relaxed);
  gFreeCount.store(0, std::memory_order_relaxed);
  gTrackAllocations.store(true, std::memory_order_relaxed);

  void* const newMemory = ::operator new[](8);
  ::operator delete[](newMemory);
  void* (*volatile rawMalloc)(std::size_t) = &std::malloc;
  void (*volatile rawFree)(void*) = &std::free;
  void* const mallocMemory = rawMalloc(8);
  const bool mallocSucceeded = mallocMemory != nullptr;
  if (mallocSucceeded) {
    static_cast<volatile uint8_t*>(mallocMemory)[0] = 1;
  }
  rawFree(mallocMemory);

  gTrackAllocations.store(false, std::memory_order_relaxed);

  ASSERT_TRUE(mallocSucceeded);
  EXPECT_GT(gNewCount.load(std::memory_order_relaxed), 0U);
#if defined(PDF_PIXEL_CACHE_WRAP_MALLOC)
  EXPECT_GT(gMallocCount.load(std::memory_order_relaxed), 0U);
  EXPECT_GT(gFreeCount.load(std::memory_order_relaxed), 0U);
#endif
}

TEST(PdfPixelCacheWriter, AllocatesNothingAcrossBeginChunkedRowAndFinish) {
  std::array<uint8_t, 513> row{};
  for (size_t i = 0; i < row.size(); ++i) {
    row[i] = static_cast<uint8_t>(i & 0x03U);
  }
  FixedSink<256> sink;
  PdfPixelCacheWriter writer;

  gNewCount.store(0, std::memory_order_relaxed);
  gMallocCount.store(0, std::memory_order_relaxed);
  gFreeCount.store(0, std::memory_order_relaxed);
  gTrackAllocations.store(true, std::memory_order_relaxed);
  const PdfStatus beginStatus = writer.begin(sink.sink(), row.size(), 1);
  const PdfStatus rowStatus = writer.writeRow(0, row.data(), row.size());
  const PdfStatus finishStatus = writer.finish();
  gTrackAllocations.store(false, std::memory_order_relaxed);

  EXPECT_TRUE(beginStatus.ok());
  EXPECT_TRUE(rowStatus.ok());
  EXPECT_TRUE(finishStatus.ok());
  EXPECT_EQ(gNewCount.load(std::memory_order_relaxed), 0U);
  EXPECT_EQ(gMallocCount.load(std::memory_order_relaxed), 0U);
  EXPECT_EQ(gFreeCount.load(std::memory_order_relaxed), 0U);
  EXPECT_EQ(sink.calls, 3U);  // Header + 128-byte chunk + one-byte chunk.
  EXPECT_EQ(writer.bytesWritten(), pixel_cache::kHeaderSize + 129U);
  EXPECT_LE(sizeof(PdfPixelCacheWriter), 96U);
}

}  // namespace
