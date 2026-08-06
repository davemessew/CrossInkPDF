#include <PdfImageExtractor.h>
#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <new>
#include <vector>

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

#if defined(PDF_IMAGE_EXTRACTOR_WRAP_MALLOC)
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

template <size_t Capacity>
struct FixedSink {
  std::array<uint8_t, Capacity> bytes{};
  size_t size = 0;
  size_t calls = 0;
  size_t maximumWrite = Capacity;
  size_t failCall = std::numeric_limits<size_t>::max();
  PdfStatus forcedFailure = PdfStatus::failure(PdfError::InsufficientStorage, 77);

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
    const size_t actual = requested < self.maximumWrite ? requested : self.maximumWrite;
    if (actual > self.bytes.size() - self.size) {
      *bytesWritten = 0;
      return PdfStatus::failure(PdfError::InsufficientStorage);
    }
    for (size_t index = 0; index < actual; ++index) {
      self.bytes[self.size + index] = source[index];
    }
    self.size += actual;
    *bytesWritten = actual;
    return PdfStatus::success();
  }

  PdfByteSink sink() { return {this, write}; }
};

struct MemorySource {
  const uint8_t* bytes = nullptr;
  size_t size = 0;
  size_t maximumRead = std::numeric_limits<size_t>::max();
  size_t calls = 0;
  size_t zeroReadCall = std::numeric_limits<size_t>::max();
  size_t failCall = std::numeric_limits<size_t>::max();
  PdfStatus forcedFailure = PdfStatus::failure(PdfError::InsufficientMemory, 99);

  static PdfStatus read(void* context, const uint64_t offset, uint8_t* destination, const size_t requested,
                        size_t* bytesRead) {
    auto& self = *static_cast<MemorySource*>(context);
    if (destination == nullptr || bytesRead == nullptr || offset > self.size) {
      return PdfStatus::failure(PdfError::InvalidArgument, offset);
    }
    const size_t call = self.calls++;
    if (call == self.failCall) {
      *bytesRead = 0;
      return self.forcedFailure;
    }
    if (call == self.zeroReadCall) {
      *bytesRead = 0;
      return PdfStatus::success();
    }
    size_t actual = requested < self.maximumRead ? requested : self.maximumRead;
    const size_t available = self.size - static_cast<size_t>(offset);
    if (actual > available) {
      actual = available;
    }
    for (size_t index = 0; index < actual; ++index) {
      destination[index] = self.bytes[static_cast<size_t>(offset) + index];
    }
    *bytesRead = actual;
    return PdfStatus::success();
  }

  PdfByteSource source(const uint64_t advertisedSize = 0) {
    return {this, advertisedSize == 0 ? size : advertisedSize, read};
  }
};

PdfImageParameters grayParameters(const uint32_t width, const uint32_t height, const uint8_t bitsPerComponent = 8) {
  PdfImageParameters parameters{};
  parameters.width = width;
  parameters.height = height;
  parameters.maximumOutputWidth = static_cast<uint16_t>(width > 4096U ? 4096U : width);
  parameters.maximumOutputHeight = static_cast<uint16_t>(height > 4096U ? 4096U : height);
  parameters.maximumOutputBytes = 1024U * 1024U;
  parameters.bitsPerComponent = bitsPerComponent;
  parameters.colorSpace = PdfImageColorSpace::Gray;
  return parameters;
}

uint8_t cachedPixel(const uint8_t* bytes, const size_t size, const uint16_t width, const uint16_t x, const uint16_t y) {
  const size_t bytesPerRow = width / 4U + (width % 4U == 0U ? 0U : 1U);
  const size_t offset = 4U + static_cast<size_t>(y) * bytesPerRow + x / 4U;
  EXPECT_LT(offset, size);
  return static_cast<uint8_t>((bytes[offset] >> (6U - (x & 3U) * 2U)) & 0x03U);
}

template <size_t Capacity, size_t SourceCapacity = 8192, size_t OutputCapacity = 4096>
PdfStatus extractImage(const PdfImageParameters& parameters, const uint8_t* decodedBytes, const size_t decodedSize,
                       FixedSink<Capacity>& output, PdfImageInfo* info = nullptr, const size_t maximumRead = 4096) {
  std::array<uint8_t, SourceCapacity> sourceRow{};
  std::array<uint8_t, OutputCapacity> outputRow{};
  MemorySource source{decodedBytes, decodedSize, maximumRead};
  PdfImageExtractor extractor;
  const PdfImageWorkspace workspace{
      sourceRow.data(),
      sourceRow.size(),
      outputRow.data(),
      outputRow.size(),
  };
  PdfStatus status = extractor.begin(parameters, output.sink(), workspace);
  if (status.ok()) {
    status = extractor.extractDecoded(source.source());
  }
  if (status.ok()) {
    status = extractor.finish();
  }
  if (info != nullptr) {
    *info = extractor.info();
  }
  return status;
}

uint32_t fnv1a(const uint8_t* bytes, const size_t size) {
  uint32_t hash = 2166136261U;
  for (size_t index = 0; index < size; ++index) {
    hash ^= bytes[index];
    hash *= 16777619U;
  }
  return hash;
}

TEST(PdfImageExtractorValidation, RejectsZeroDimensionsBeforeWritingAHeader) {
  std::array<uint8_t, 8> sourceRow{};
  std::array<uint8_t, 8> outputRow{};
  FixedSink<32> output;
  PdfImageExtractor extractor;
  PdfImageParameters parameters = grayParameters(1, 1);
  parameters.width = 0;
  const PdfImageWorkspace workspace{
      sourceRow.data(),
      sourceRow.size(),
      outputRow.data(),
      outputRow.size(),
  };

  const PdfStatus status = extractor.begin(parameters, output.sink(), workspace);

  EXPECT_EQ(status.error, PdfError::InvalidArgument);
  EXPECT_EQ(output.size, 0U);
}

TEST(PdfImageExtractorValidation, RejectsUnsupportedColorDepthPredictorAndDecode) {
  struct InvalidCase {
    PdfImageColorSpace colorSpace;
    uint8_t bits;
    uint8_t predictor;
    PdfImageDecode decode;
  };
  constexpr std::array<InvalidCase, 7> invalid{{
      {PdfImageColorSpace::Gray, 3, 1, PdfImageDecode::Normal},
      {PdfImageColorSpace::RGB, 4, 1, PdfImageDecode::Normal},
      {PdfImageColorSpace::ImageMask, 8, 1, PdfImageDecode::Normal},
      {PdfImageColorSpace::Gray, 8, 3, PdfImageDecode::Normal},
      {PdfImageColorSpace::Gray, 8, 16, PdfImageDecode::Normal},
      {static_cast<PdfImageColorSpace>(99), 8, 1, PdfImageDecode::Normal},
      {PdfImageColorSpace::Gray, 8, 1, static_cast<PdfImageDecode>(99)},
  }};

  for (const InvalidCase& testCase : invalid) {
    std::array<uint8_t, 8> sourceRow{};
    std::array<uint8_t, 8> outputRow{};
    FixedSink<32> output;
    PdfImageExtractor extractor;
    PdfImageParameters parameters = grayParameters(1, 1, testCase.bits);
    parameters.colorSpace = testCase.colorSpace;
    parameters.predictor = testCase.predictor;
    parameters.decode = testCase.decode;
    const PdfImageWorkspace workspace{
        sourceRow.data(),
        sourceRow.size(),
        outputRow.data(),
        outputRow.size(),
    };

    const PdfStatus status = extractor.begin(parameters, output.sink(), workspace);

    EXPECT_EQ(status.error, PdfError::UnsupportedEncoding);
    EXPECT_EQ(output.size, 0U);
  }
}

TEST(PdfImageExtractorValidation, RejectsInvalidIndexedPalettesBeforeWriting) {
  constexpr std::array<uint8_t, 6> palette{};
  struct InvalidCase {
    uint16_t entries;
    size_t bytes;
    const uint8_t* palette;
  };
  const std::array<InvalidCase, 4> invalid{{
      {0, 0, palette.data()},
      {257, palette.size(), palette.data()},
      {3, 2, palette.data()},
      {3, 3, nullptr},
  }};

  for (const InvalidCase& testCase : invalid) {
    std::array<uint8_t, 8> sourceRow{};
    std::array<uint8_t, 8> outputRow{};
    FixedSink<32> output;
    PdfImageExtractor extractor;
    PdfImageParameters parameters = grayParameters(1, 1, 2);
    parameters.colorSpace = PdfImageColorSpace::IndexedGray;
    parameters.palette = testCase.palette;
    parameters.paletteEntries = testCase.entries;
    parameters.paletteBytes = testCase.bytes;
    const PdfImageWorkspace workspace{
        sourceRow.data(),
        sourceRow.size(),
        outputRow.data(),
        outputRow.size(),
    };

    const PdfStatus status = extractor.begin(parameters, output.sink(), workspace);

    EXPECT_FALSE(status.ok());
    EXPECT_EQ(output.size, 0U);
  }
}

TEST(PdfImageExtractorValidation, EnforcesSixteenMegapixelAndEightKiBSourceRowCaps) {
  struct LimitCase {
    PdfImageParameters parameters;
    PdfError error;
  };
  std::array<LimitCase, 4> cases{{
      {grayParameters(4000, 4001), PdfError::LimitExceeded},
      {grayParameters(std::numeric_limits<uint32_t>::max(), std::numeric_limits<uint32_t>::max()),
       PdfError::LimitExceeded},
      {grayParameters(8193, 1), PdfError::LimitExceeded},
      {grayParameters(2731, 1), PdfError::LimitExceeded},
  }};
  cases[3].parameters.colorSpace = PdfImageColorSpace::RGB;
  cases[3].parameters.bitsPerComponent = 8;

  for (const LimitCase& testCase : cases) {
    std::array<uint8_t, 8192> sourceRow{};
    std::array<uint8_t, 4096> outputRow{};
    FixedSink<32> output;
    PdfImageExtractor extractor;
    const PdfImageWorkspace workspace{
        sourceRow.data(),
        sourceRow.size(),
        outputRow.data(),
        outputRow.size(),
    };

    const PdfStatus status = extractor.begin(testCase.parameters, output.sink(), workspace);

    EXPECT_EQ(status.error, testCase.error);
    EXPECT_EQ(output.size, 0U);
  }
}

TEST(PdfImageExtractorValidation, ReportsRequiredCallerWorkspaceAsRecoverableMemoryFailure) {
  FixedSink<32> output;
  PdfImageParameters parameters = grayParameters(9, 1);
  std::array<uint8_t, 8> shortSourceRow{};
  std::array<uint8_t, 9> outputRow{};
  PdfImageExtractor sourceTooSmall;
  const PdfImageWorkspace shortSource{
      shortSourceRow.data(),
      shortSourceRow.size(),
      outputRow.data(),
      outputRow.size(),
  };

  const PdfStatus sourceStatus = sourceTooSmall.begin(parameters, output.sink(), shortSource);

  EXPECT_EQ(sourceStatus.error, PdfError::InsufficientMemory);
  EXPECT_EQ(sourceStatus.offset, 9U);
  EXPECT_EQ(output.size, 0U);

  std::array<uint8_t, 9> sourceRow{};
  std::array<uint8_t, 8> shortOutputRow{};
  PdfImageExtractor outputTooSmall;
  const PdfImageWorkspace shortOutput{
      sourceRow.data(),
      sourceRow.size(),
      shortOutputRow.data(),
      shortOutputRow.size(),
  };

  const PdfStatus outputStatus = outputTooSmall.begin(parameters, output.sink(), shortOutput);

  EXPECT_EQ(outputStatus.error, PdfError::InsufficientMemory);
  EXPECT_EQ(outputStatus.offset, 9U);
  EXPECT_EQ(output.size, 0U);
}

TEST(PdfImageExtractorValidation, EnforcesOutputByteCapBeforeWritingAHeader) {
  std::array<uint8_t, 16> sourceRow{};
  std::array<uint8_t, 16> outputRow{};
  FixedSink<32> output;
  PdfImageExtractor extractor;
  PdfImageParameters parameters = grayParameters(8, 8);
  parameters.maximumOutputBytes = 19;
  const PdfImageWorkspace workspace{
      sourceRow.data(),
      sourceRow.size(),
      outputRow.data(),
      outputRow.size(),
  };

  const PdfStatus status = extractor.begin(parameters, output.sink(), workspace);

  EXPECT_EQ(status.error, PdfError::LimitExceeded);
  EXPECT_EQ(status.offset, 20U);
  EXPECT_EQ(output.size, 0U);
}

TEST(PdfImageExtractorValidation, AcceptsExactSixteenMegapixelEightKiBAndFourKiBBoundaries) {
  std::array<uint8_t, 8192> sourceRow{};
  std::array<uint8_t, 4096> outputRow{};
  FixedSink<32> rowBoundaryOutput;
  PdfImageExtractor rowBoundaryExtractor;
  PdfImageParameters rowBoundary = grayParameters(8192, 1);
  rowBoundary.maximumOutputWidth = 4096;
  rowBoundary.maximumOutputBytes = 1028;
  const PdfImageWorkspace rowBoundaryWorkspace{
      sourceRow.data(),
      sourceRow.size(),
      outputRow.data(),
      outputRow.size(),
  };

  const PdfStatus rowBoundaryStatus =
      rowBoundaryExtractor.begin(rowBoundary, rowBoundaryOutput.sink(), rowBoundaryWorkspace);

  ASSERT_TRUE(rowBoundaryStatus.ok());
  EXPECT_EQ(rowBoundaryExtractor.info().sourceRowBytes, 8192U);
  EXPECT_EQ(rowBoundaryExtractor.info().outputWidth, 4096U);
  EXPECT_EQ(rowBoundaryExtractor.info().outputBytes, 1028U);

  std::array<uint8_t, 4000> exactPixelSourceRow{};
  std::array<uint8_t, 4000> exactPixelOutputRow{};
  FixedSink<32> exactPixelOutput;
  PdfImageExtractor exactPixelExtractor;
  PdfImageParameters exactPixels = grayParameters(4000, 4000);
  exactPixels.maximumOutputBytes = 4000004;
  const PdfImageWorkspace exactPixelWorkspace{
      exactPixelSourceRow.data(),
      exactPixelSourceRow.size(),
      exactPixelOutputRow.data(),
      exactPixelOutputRow.size(),
  };

  const PdfStatus exactPixelStatus =
      exactPixelExtractor.begin(exactPixels, exactPixelOutput.sink(), exactPixelWorkspace);

  ASSERT_TRUE(exactPixelStatus.ok());
  EXPECT_EQ(static_cast<uint64_t>(exactPixelExtractor.info().sourceWidth) * exactPixelExtractor.info().sourceHeight,
            16000000U);
  EXPECT_EQ(exactPixelExtractor.info().outputBytes, 4000004U);
}

TEST(PdfImageExtractorValidation, AcceptsAnExactTwoHundredFiftySixEntryIndexedRgbPalette) {
  std::array<uint8_t, 768> palette{};
  palette[765] = 255;
  palette[766] = 255;
  palette[767] = 255;
  constexpr std::array<uint8_t, 1> index{255};
  PdfImageParameters parameters = grayParameters(1, 1, 8);
  parameters.colorSpace = PdfImageColorSpace::IndexedRGB;
  parameters.palette = palette.data();
  parameters.paletteBytes = palette.size();
  parameters.paletteEntries = 256;
  FixedSink<32> output;

  const PdfStatus status = extractImage(parameters, index.data(), index.size(), output);

  ASSERT_TRUE(status.ok());
  EXPECT_EQ(cachedPixel(output.bytes.data(), output.size, 1, 0, 0), 3);
}

TEST(PdfImageExtractorSizing, PreservesAspectRatioWithoutUpscaling) {
  std::array<uint8_t, 4800> sourceRow{};
  std::array<uint8_t, 800> outputRow{};
  FixedSink<32> output;
  PdfImageExtractor extractor;
  PdfImageParameters parameters = grayParameters(1600, 1200);
  parameters.colorSpace = PdfImageColorSpace::RGB;
  parameters.bitsPerComponent = 8;
  parameters.maximumOutputWidth = 800;
  parameters.maximumOutputHeight = 480;
  parameters.maximumOutputBytes = 200000;
  const PdfImageWorkspace workspace{
      sourceRow.data(),
      sourceRow.size(),
      outputRow.data(),
      outputRow.size(),
  };

  const PdfStatus status = extractor.begin(parameters, output.sink(), workspace);

  ASSERT_TRUE(status.ok());
  EXPECT_EQ(extractor.info().outputWidth, 640U);
  EXPECT_EQ(extractor.info().outputHeight, 480U);
  ASSERT_EQ(output.size, 4U);
  EXPECT_EQ(output.bytes[0], 0x80);
  EXPECT_EQ(output.bytes[1], 0x02);
  EXPECT_EQ(output.bytes[2], 0xe0);
  EXPECT_EQ(output.bytes[3], 0x01);
}

TEST(PdfImageExtractorSamples, ConvertsGrayOneTwoFourAndEightBitSamples) {
  struct Fixture {
    uint8_t bits;
    std::array<uint8_t, 7> source;
    size_t sourceBytes;
    uint16_t width;
    std::array<uint8_t, 7> expected;
  };
  constexpr std::array<Fixture, 4> fixtures{{
      {1, {0x58}, 1, 5, {0, 3, 0, 3, 3}},
      {2, {0x1b}, 1, 4, {0, 1, 2, 3}},
      {4, {0x05, 0xaf}, 2, 4, {0, 1, 2, 3}},
      {8, {0, 84, 85, 169, 170, 254, 255}, 7, 7, {0, 1, 1, 2, 2, 3, 3}},
  }};

  for (const Fixture& fixture : fixtures) {
    FixedSink<32> output;
    const PdfImageParameters parameters = grayParameters(fixture.width, 1, fixture.bits);

    const PdfStatus status = extractImage(parameters, fixture.source.data(), fixture.sourceBytes, output);

    ASSERT_TRUE(status.ok());
    for (uint16_t x = 0; x < fixture.width; ++x) {
      EXPECT_EQ(cachedPixel(output.bytes.data(), output.size, fixture.width, x, 0), fixture.expected[x]);
    }
  }
}

TEST(PdfImageExtractorSamples, ConvertsRgbAndIndexedGrayAndRgb) {
  constexpr std::array<uint8_t, 15> rgb{
      255, 0, 0, 0, 255, 0, 0, 0, 255, 255, 255, 255, 0, 0, 0,
  };
  constexpr std::array<uint8_t, 4> grayPalette{0, 85, 170, 255};
  constexpr std::array<uint8_t, 12> rgbPalette{
      0, 0, 0, 255, 0, 0, 0, 255, 0, 255, 255, 255,
  };
  constexpr std::array<uint8_t, 1> indices{0x1b};

  FixedSink<32> rgbOutput;
  PdfImageParameters rgbParameters = grayParameters(5, 1);
  rgbParameters.colorSpace = PdfImageColorSpace::RGB;
  const PdfStatus rgbStatus = extractImage(rgbParameters, rgb.data(), rgb.size(), rgbOutput);
  ASSERT_TRUE(rgbStatus.ok());
  constexpr std::array<uint8_t, 5> expectedRgb{1, 2, 0, 3, 0};
  for (uint16_t x = 0; x < expectedRgb.size(); ++x) {
    EXPECT_EQ(cachedPixel(rgbOutput.bytes.data(), rgbOutput.size, 5, x, 0), expectedRgb[x]);
  }

  FixedSink<32> grayOutput;
  PdfImageParameters grayIndexed = grayParameters(4, 1, 2);
  grayIndexed.colorSpace = PdfImageColorSpace::IndexedGray;
  grayIndexed.palette = grayPalette.data();
  grayIndexed.paletteBytes = grayPalette.size();
  grayIndexed.paletteEntries = grayPalette.size();
  ASSERT_TRUE(extractImage(grayIndexed, indices.data(), indices.size(), grayOutput).ok());
  for (uint16_t x = 0; x < 4; ++x) {
    EXPECT_EQ(cachedPixel(grayOutput.bytes.data(), grayOutput.size, 4, x, 0), x);
  }

  FixedSink<32> indexedRgbOutput;
  PdfImageParameters rgbIndexed = grayIndexed;
  rgbIndexed.colorSpace = PdfImageColorSpace::IndexedRGB;
  rgbIndexed.palette = rgbPalette.data();
  rgbIndexed.paletteBytes = rgbPalette.size();
  ASSERT_TRUE(extractImage(rgbIndexed, indices.data(), indices.size(), indexedRgbOutput).ok());
  constexpr std::array<uint8_t, 4> expectedIndexedRgb{0, 1, 2, 3};
  for (uint16_t x = 0; x < expectedIndexedRgb.size(); ++x) {
    EXPECT_EQ(cachedPixel(indexedRgbOutput.bytes.data(), indexedRgbOutput.size, 4, x, 0), expectedIndexedRgb[x]);
  }
}

TEST(PdfImageExtractorSamples, MapsNormalIndexedDecodeAcrossTheFullSampleDomainThenClipsToHighValue) {
  constexpr std::array<uint8_t, 3> palette{0, 85, 170};
  constexpr std::array<uint8_t, 1> indices{0x1b};
  PdfImageParameters parameters = grayParameters(4, 1, 2);
  parameters.colorSpace = PdfImageColorSpace::IndexedGray;
  parameters.palette = palette.data();
  parameters.paletteBytes = palette.size();
  parameters.paletteEntries = palette.size();
  FixedSink<32> output;

  const PdfStatus status = extractImage(parameters, indices.data(), indices.size(), output);

  ASSERT_TRUE(status.ok());
  constexpr std::array<uint8_t, 4> expected{0, 1, 2, 2};
  for (uint16_t x = 0; x < expected.size(); ++x) {
    EXPECT_EQ(cachedPixel(output.bytes.data(), output.size, 4, x, 0), expected[x]);
  }
}

TEST(PdfImageExtractorSamples, MapsInvertedIndexedDecodeAcrossTheFullSampleDomainThenClipsToHighValue) {
  constexpr std::array<uint8_t, 3> palette{0, 85, 170};
  constexpr std::array<uint8_t, 1> indices{0x1b};
  PdfImageParameters parameters = grayParameters(4, 1, 2);
  parameters.colorSpace = PdfImageColorSpace::IndexedGray;
  parameters.decode = PdfImageDecode::Inverted;
  parameters.palette = palette.data();
  parameters.paletteBytes = palette.size();
  parameters.paletteEntries = palette.size();
  FixedSink<32> output;

  const PdfStatus status = extractImage(parameters, indices.data(), indices.size(), output);

  ASSERT_TRUE(status.ok());
  constexpr std::array<uint8_t, 4> expected{2, 2, 1, 0};
  for (uint16_t x = 0; x < expected.size(); ++x) {
    EXPECT_EQ(cachedPixel(output.bytes.data(), output.size, 4, x, 0), expected[x]);
  }
}

TEST(PdfImageExtractorSamples, AcceptsOneBitSamplesWithFourIndexedPaletteEntries) {
  constexpr std::array<uint8_t, 4> palette{0, 85, 170, 255};
  constexpr std::array<uint8_t, 1> indices{0x40};
  PdfImageParameters parameters = grayParameters(2, 1, 1);
  parameters.colorSpace = PdfImageColorSpace::IndexedGray;
  parameters.palette = palette.data();
  parameters.paletteBytes = palette.size();
  parameters.paletteEntries = palette.size();
  FixedSink<32> output;

  const PdfStatus status = extractImage(parameters, indices.data(), indices.size(), output);

  ASSERT_TRUE(status.ok());
  EXPECT_EQ(cachedPixel(output.bytes.data(), output.size, 2, 0, 0), 0);
  EXPECT_EQ(cachedPixel(output.bytes.data(), output.size, 2, 1, 0), 1);
}

TEST(PdfImageExtractorSamples, AppliesFullDecodeInversionToGraySamples) {
  constexpr std::array<uint8_t, 2> gray{0, 255};
  FixedSink<32> grayOutput;
  PdfImageParameters grayInverted = grayParameters(2, 1);
  grayInverted.decode = PdfImageDecode::Inverted;
  ASSERT_TRUE(extractImage(grayInverted, gray.data(), gray.size(), grayOutput).ok());
  EXPECT_EQ(cachedPixel(grayOutput.bytes.data(), grayOutput.size, 2, 0, 0), 3);
  EXPECT_EQ(cachedPixel(grayOutput.bytes.data(), grayOutput.size, 2, 1, 0), 0);
}

TEST(PdfImageExtractorSamples, UsesPdfImageMaskStencilPolarityForNormalAndInvertedDecode) {
  constexpr std::array<uint8_t, 1> mask{0x60};
  PdfImageParameters maskParameters = grayParameters(4, 1, 1);
  maskParameters.colorSpace = PdfImageColorSpace::ImageMask;
  FixedSink<32> maskOutput;
  ASSERT_TRUE(extractImage(maskParameters, mask.data(), mask.size(), maskOutput).ok());
  constexpr std::array<uint8_t, 4> expectedMask{0, 3, 3, 0};
  for (uint16_t x = 0; x < expectedMask.size(); ++x) {
    EXPECT_EQ(cachedPixel(maskOutput.bytes.data(), maskOutput.size, 4, x, 0), expectedMask[x]);
  }

  maskParameters.decode = PdfImageDecode::Inverted;
  FixedSink<32> invertedMaskOutput;
  ASSERT_TRUE(extractImage(maskParameters, mask.data(), mask.size(), invertedMaskOutput).ok());
  for (uint16_t x = 0; x < expectedMask.size(); ++x) {
    EXPECT_EQ(cachedPixel(invertedMaskOutput.bytes.data(), invertedMaskOutput.size, 4, x, 0),
              static_cast<uint8_t>(3U - expectedMask[x]));
  }
}

TEST(PdfImageExtractorSamples, QuantizesTheCurrentImageMaskPaintLuminance) {
  constexpr std::array<uint8_t, 1> mask{0x40};
  PdfImageParameters maskParameters = grayParameters(2, 1, 1);
  maskParameters.colorSpace = PdfImageColorSpace::ImageMask;
  maskParameters.imageMaskPaintLuminance = 128;

  FixedSink<32> normalOutput;
  ASSERT_TRUE(extractImage(maskParameters, mask.data(), mask.size(), normalOutput).ok());
  EXPECT_EQ(cachedPixel(normalOutput.bytes.data(), normalOutput.size, 2, 0, 0), 2);
  EXPECT_EQ(cachedPixel(normalOutput.bytes.data(), normalOutput.size, 2, 1, 0), 3);

  maskParameters.decode = PdfImageDecode::Inverted;
  FixedSink<32> invertedOutput;
  ASSERT_TRUE(extractImage(maskParameters, mask.data(), mask.size(), invertedOutput).ok());
  EXPECT_EQ(cachedPixel(invertedOutput.bytes.data(), invertedOutput.size, 2, 0, 0), 3);
  EXPECT_EQ(cachedPixel(invertedOutput.bytes.data(), invertedOutput.size, 2, 1, 0), 2);
}

TEST(PdfImageExtractorPredictors, ReconstructsTiffForPackedGrayAndRgbSamples) {
  constexpr std::array<uint8_t, 1> gray2Difference{0x1b};
  PdfImageParameters gray2 = grayParameters(4, 1, 2);
  gray2.predictor = 2;
  FixedSink<32> gray2Output;
  ASSERT_TRUE(extractImage(gray2, gray2Difference.data(), gray2Difference.size(), gray2Output).ok());
  constexpr std::array<uint8_t, 4> expectedGray2{0, 1, 3, 2};
  for (uint16_t x = 0; x < expectedGray2.size(); ++x) {
    EXPECT_EQ(cachedPixel(gray2Output.bytes.data(), gray2Output.size, 4, x, 0), expectedGray2[x]);
  }

  constexpr std::array<uint8_t, 3> gray4Difference{0x1d, 0x6b, 0x90};
  PdfImageParameters gray4 = grayParameters(5, 1, 4);
  gray4.predictor = 2;
  FixedSink<32> gray4Output;
  ASSERT_TRUE(extractImage(gray4, gray4Difference.data(), gray4Difference.size(), gray4Output).ok());
  constexpr std::array<uint8_t, 5> expectedGray4{0, 3, 1, 3, 2};
  for (uint16_t x = 0; x < expectedGray4.size(); ++x) {
    EXPECT_EQ(cachedPixel(gray4Output.bytes.data(), gray4Output.size, 5, x, 0), expectedGray4[x]);
  }

  constexpr std::array<uint8_t, 6> rgbDifference{10, 20, 30, 30, 30, 30};
  PdfImageParameters rgb = grayParameters(2, 1);
  rgb.colorSpace = PdfImageColorSpace::RGB;
  rgb.predictor = 2;
  FixedSink<32> rgbOutput;
  ASSERT_TRUE(extractImage(rgb, rgbDifference.data(), rgbDifference.size(), rgbOutput).ok());
  EXPECT_EQ(cachedPixel(rgbOutput.bytes.data(), rgbOutput.size, 2, 0, 0), 0);
  EXPECT_EQ(cachedPixel(rgbOutput.bytes.data(), rgbOutput.size, 2, 1, 0), 0);
}

TEST(PdfImageExtractorPredictors, AcceptsPngPredictorValuesTenThroughFifteenAndAllRowFilters) {
  struct Fixture {
    uint8_t predictor;
    std::array<uint8_t, 8> encoded;
    size_t encodedSize;
    uint16_t height;
    std::array<uint8_t, 6> expected;
  };
  constexpr std::array<Fixture, 6> fixtures{{
      {10, {0, 10, 20, 30}, 4, 1, {0, 0, 0}},
      {11, {1, 10, 10, 10}, 4, 1, {0, 0, 0}},
      {12, {0, 10, 20, 30, 2, 10, 10, 10}, 8, 2, {0, 0, 0, 0, 0, 0}},
      {13, {3, 10, 15, 20}, 4, 1, {0, 0, 0}},
      {14, {0, 5, 25, 15, 4, 5, 251, 15}, 8, 2, {0, 0, 0, 0, 0, 0}},
      {15, {0, 0, 85, 170, 1, 255, 85, 85}, 8, 2, {0, 1, 2, 3, 1, 2}},
  }};

  for (const Fixture& fixture : fixtures) {
    PdfImageParameters parameters = grayParameters(3, fixture.height);
    parameters.predictor = fixture.predictor;
    FixedSink<32> output;
    ASSERT_TRUE(extractImage(parameters, fixture.encoded.data(), fixture.encodedSize, output).ok());
    for (uint16_t y = 0; y < fixture.height; ++y) {
      for (uint16_t x = 0; x < 3; ++x) {
        EXPECT_EQ(cachedPixel(output.bytes.data(), output.size, 3, x, y), fixture.expected[y * 3U + x]);
      }
    }
  }
}

TEST(PdfImageExtractorPredictors, RejectsMalformedPngFilterTagsWithTheDecodedOffset) {
  constexpr std::array<uint8_t, 4> malformed{5, 0, 0, 0};
  PdfImageParameters parameters = grayParameters(3, 1);
  parameters.predictor = 15;
  FixedSink<32> output;

  const PdfStatus status = extractImage(parameters, malformed.data(), malformed.size(), output);

  EXPECT_EQ(status.error, PdfError::Malformed);
  EXPECT_EQ(status.offset, 0U);
  EXPECT_EQ(output.size, 4U);
}

TEST(PdfImageExtractorPredictors, RejectsTruncatedPredictorRowsAtTheExactDecodedOffset) {
  constexpr std::array<uint8_t, 2> truncated{1, 10};
  PdfImageParameters parameters = grayParameters(3, 1);
  parameters.predictor = 15;
  FixedSink<32> output;

  const PdfStatus status = extractImage(parameters, truncated.data(), truncated.size(), output);

  EXPECT_EQ(status.error, PdfError::UnexpectedEof);
  EXPECT_EQ(status.offset, truncated.size());
  EXPECT_EQ(output.size, 4U);
}

TEST(PdfImageExtractorScaling, DownscalesWithDeterministicCenterSampling) {
  constexpr std::array<uint8_t, 16> source{
      0, 0, 0, 0, 0, 64, 0, 128, 0, 0, 0, 0, 0, 192, 0, 255,
  };
  PdfImageParameters parameters = grayParameters(4, 4);
  parameters.maximumOutputWidth = 2;
  parameters.maximumOutputHeight = 2;
  FixedSink<32> output;
  PdfImageInfo info{};

  const PdfStatus status = extractImage(parameters, source.data(), source.size(), output, &info);

  ASSERT_TRUE(status.ok());
  EXPECT_EQ(info.outputWidth, 2U);
  EXPECT_EQ(info.outputHeight, 2U);
  EXPECT_EQ(cachedPixel(output.bytes.data(), output.size, 2, 0, 0), 1);
  EXPECT_EQ(cachedPixel(output.bytes.data(), output.size, 2, 1, 0), 2);
  EXPECT_EQ(cachedPixel(output.bytes.data(), output.size, 2, 0, 1), 3);
  EXPECT_EQ(cachedPixel(output.bytes.data(), output.size, 2, 1, 1), 3);
}

TEST(PdfImageExtractorScaling, IncrementalHorizontalMappingMatchesTheCenterFormula) {
  constexpr std::array<uint8_t, 17> source{
      0, 64, 128, 192, 0, 64, 128, 192, 0, 64, 128, 192, 0, 64, 128, 192, 0,
  };
  PdfImageParameters parameters = grayParameters(source.size(), 1);
  parameters.maximumOutputWidth = 7;
  FixedSink<32> output;

  ASSERT_TRUE(extractImage(parameters, source.data(), source.size(), output).ok());

  for (uint16_t outputX = 0; outputX < 7; ++outputX) {
    const uint32_t sourceX = static_cast<uint32_t>((static_cast<uint64_t>(outputX) * 2U + 1U) * source.size() / 14U);
    EXPECT_EQ(cachedPixel(output.bytes.data(), output.size, 7, outputX, 0),
              static_cast<uint8_t>(source[sourceX] >> 6U));
  }
}

TEST(PdfImageExtractorSoftMask, FlattensGray8AlphaToWhiteAndSupportsInversion) {
  constexpr std::array<uint8_t, 2> base{0, 0};
  constexpr std::array<uint8_t, 2> mask{255, 0};
  for (const PdfImageDecode maskDecode : {PdfImageDecode::Normal, PdfImageDecode::Inverted}) {
    std::array<uint8_t, 2> sourceRow{};
    std::array<uint8_t, 2> outputRow{};
    FixedSink<32> output;
    PdfImageExtractor extractor;
    PdfImageParameters parameters = grayParameters(2, 1);
    parameters.hasSoftMask = true;
    parameters.softMaskDecode = maskDecode;
    const PdfImageWorkspace workspace{
        sourceRow.data(),
        sourceRow.size(),
        outputRow.data(),
        outputRow.size(),
    };
    ASSERT_TRUE(extractor.begin(parameters, output.sink(), workspace).ok());
    const PdfByteSink baseSink = extractor.decodedSink();
    size_t baseWritten = 0;
    ASSERT_TRUE(baseSink.write(baseSink.context, base.data(), base.size(), &baseWritten).ok());
    EXPECT_EQ(baseWritten, base.size());
    EXPECT_TRUE(extractor.awaitingSoftMask());
    EXPECT_EQ(output.size, 4U);

    const PdfByteSink maskSink = extractor.softMaskSink();
    size_t maskWritten = 0;
    ASSERT_TRUE(maskSink.write(maskSink.context, mask.data(), mask.size(), &maskWritten).ok());
    EXPECT_EQ(maskWritten, mask.size());
    ASSERT_TRUE(extractor.finish().ok());
    if (maskDecode == PdfImageDecode::Normal) {
      EXPECT_EQ(cachedPixel(output.bytes.data(), output.size, 2, 0, 0), 0);
      EXPECT_EQ(cachedPixel(output.bytes.data(), output.size, 2, 1, 0), 3);
    } else {
      EXPECT_EQ(cachedPixel(output.bytes.data(), output.size, 2, 0, 0), 3);
      EXPECT_EQ(cachedPixel(output.bytes.data(), output.size, 2, 1, 0), 0);
    }
  }
}

TEST(PdfImageExtractorSoftMask, RequiresOneSameSizeMaskRowAfterEachBaseRow) {
  constexpr std::array<uint8_t, 4> base{0, 0, 0, 0};
  constexpr std::array<uint8_t, 1> shortMask{255};
  std::array<uint8_t, 2> sourceRow{};
  std::array<uint8_t, 2> outputRow{};
  FixedSink<32> output;
  PdfImageExtractor extractor;
  PdfImageParameters parameters = grayParameters(2, 2);
  parameters.hasSoftMask = true;
  const PdfImageWorkspace workspace{
      sourceRow.data(),
      sourceRow.size(),
      outputRow.data(),
      outputRow.size(),
  };
  ASSERT_TRUE(extractor.begin(parameters, output.sink(), workspace).ok());
  const PdfByteSink baseSink = extractor.decodedSink();
  size_t baseWritten = 0;

  const PdfStatus baseStatus = baseSink.write(baseSink.context, base.data(), base.size(), &baseWritten);

  EXPECT_TRUE(baseStatus.ok());
  EXPECT_EQ(baseWritten, 2U);
  EXPECT_TRUE(extractor.awaitingSoftMask());
  const PdfByteSink maskSink = extractor.softMaskSink();
  size_t maskWritten = 0;
  EXPECT_TRUE(maskSink.write(maskSink.context, shortMask.data(), shortMask.size(), &maskWritten).ok());
  EXPECT_EQ(maskWritten, 1U);
  const PdfStatus finishStatus = extractor.finish();
  EXPECT_EQ(finishStatus.error, PdfError::UnexpectedEof);
  EXPECT_EQ(finishStatus.offset, 1U);
}

TEST(PdfImageExtractorSoftMask, DownscalesTheMaskWithTheSameCenterMappingAsTheBaseRow) {
  constexpr std::array<uint8_t, 4> base{0, 0, 0, 0};
  constexpr std::array<uint8_t, 4> mask{0, 255, 0, 0};
  std::array<uint8_t, 4> sourceRow{};
  std::array<uint8_t, 2> outputRow{};
  FixedSink<32> output;
  PdfImageExtractor extractor;
  PdfImageParameters parameters = grayParameters(4, 1);
  parameters.maximumOutputWidth = 2;
  parameters.hasSoftMask = true;
  const PdfImageWorkspace workspace{
      sourceRow.data(),
      sourceRow.size(),
      outputRow.data(),
      outputRow.size(),
  };
  ASSERT_TRUE(extractor.begin(parameters, output.sink(), workspace).ok());
  const PdfByteSink baseSink = extractor.decodedSink();
  size_t baseWritten = 0;
  ASSERT_TRUE(baseSink.write(baseSink.context, base.data(), base.size(), &baseWritten).ok());
  const PdfByteSink maskSink = extractor.softMaskSink();
  size_t maskWritten = 0;
  ASSERT_TRUE(maskSink.write(maskSink.context, mask.data(), mask.size(), &maskWritten).ok());
  ASSERT_TRUE(extractor.finish().ok());

  EXPECT_EQ(cachedPixel(output.bytes.data(), output.size, 2, 0, 0), 0);
  EXPECT_EQ(cachedPixel(output.bytes.data(), output.size, 2, 1, 0), 3);
}

TEST(PdfImageExtractorSoftMask, RequiresTheExplicitRowAdapterInsteadOfOpeningASecondSource) {
  constexpr std::array<uint8_t, 2> base{0, 0};
  std::array<uint8_t, 2> sourceRow{};
  std::array<uint8_t, 2> outputRow{};
  FixedSink<32> output;
  MemorySource source{base.data(), base.size()};
  PdfImageExtractor extractor;
  PdfImageParameters parameters = grayParameters(2, 1);
  parameters.hasSoftMask = true;
  const PdfImageWorkspace workspace{
      sourceRow.data(),
      sourceRow.size(),
      outputRow.data(),
      outputRow.size(),
  };
  ASSERT_TRUE(extractor.begin(parameters, output.sink(), workspace).ok());

  const PdfStatus status = extractor.extractDecoded(source.source());

  EXPECT_EQ(status.error, PdfError::InvalidArgument);
  EXPECT_EQ(source.calls, 0U);
}

TEST(PdfImageExtractorIo, CompletesAcrossOneByteReadsAndRejectsZeroAndExtraBytes) {
  constexpr std::array<uint8_t, 4> sourceBytes{0, 85, 170, 255};
  PdfImageParameters parameters = grayParameters(4, 1);
  FixedSink<32> shortReadOutput;
  ASSERT_TRUE(extractImage(parameters, sourceBytes.data(), sourceBytes.size(), shortReadOutput, nullptr, 1).ok());
  EXPECT_EQ(cachedPixel(shortReadOutput.bytes.data(), shortReadOutput.size, 4, 3, 0), 3);

  std::array<uint8_t, 8> sourceRow{};
  std::array<uint8_t, 8> outputRow{};
  FixedSink<32> zeroOutput;
  PdfImageExtractor zeroExtractor;
  const PdfImageWorkspace workspace{
      sourceRow.data(),
      sourceRow.size(),
      outputRow.data(),
      outputRow.size(),
  };
  ASSERT_TRUE(zeroExtractor.begin(parameters, zeroOutput.sink(), workspace).ok());
  MemorySource zeroSource{sourceBytes.data(), sourceBytes.size()};
  zeroSource.zeroReadCall = 0;
  const PdfStatus zeroStatus = zeroExtractor.extractDecoded(zeroSource.source());
  EXPECT_EQ(zeroStatus.error, PdfError::UnexpectedEof);
  EXPECT_EQ(zeroStatus.offset, 0U);

  constexpr std::array<uint8_t, 5> extra{0, 85, 170, 255, 1};
  FixedSink<32> extraOutput;
  const PdfStatus extraStatus = extractImage(parameters, extra.data(), extra.size(), extraOutput);
  EXPECT_EQ(extraStatus.error, PdfError::Malformed);
  EXPECT_EQ(extraStatus.offset, 4U);
}

TEST(PdfImageExtractorIo, PropagatesSourceAndOutputFailuresAndKeepsThemSticky) {
  constexpr std::array<uint8_t, 4> bytes{0, 85, 170, 255};
  std::array<uint8_t, 8> sourceRow{};
  std::array<uint8_t, 8> outputRow{};
  PdfImageParameters parameters = grayParameters(4, 1);
  const PdfImageWorkspace workspace{
      sourceRow.data(),
      sourceRow.size(),
      outputRow.data(),
      outputRow.size(),
  };

  FixedSink<32> sourceFailureOutput;
  PdfImageExtractor sourceFailureExtractor;
  ASSERT_TRUE(sourceFailureExtractor.begin(parameters, sourceFailureOutput.sink(), workspace).ok());
  MemorySource failingSource{bytes.data(), bytes.size()};
  failingSource.failCall = 0;
  const PdfStatus sourceStatus = sourceFailureExtractor.extractDecoded(failingSource.source());
  EXPECT_EQ(sourceStatus.error, PdfError::InsufficientMemory);
  EXPECT_EQ(sourceStatus.offset, 99U);
  EXPECT_EQ(sourceFailureExtractor.finish().error, sourceStatus.error);

  FixedSink<32> writeFailureOutput;
  writeFailureOutput.failCall = 1;
  writeFailureOutput.forcedFailure = PdfStatus::failure(PdfError::InsufficientStorage, 123);
  const PdfStatus writeStatus = extractImage(parameters, bytes.data(), bytes.size(), writeFailureOutput);
  EXPECT_EQ(writeStatus.error, PdfError::InsufficientStorage);
  EXPECT_EQ(writeStatus.offset, 123U);
}

TEST(PdfImageExtractorIo, TreatsShortHeaderAndPayloadWritesAsIoFailure) {
  constexpr std::array<uint8_t, 4> bytes{0, 85, 170, 255};
  PdfImageParameters parameters = grayParameters(4, 1);

  FixedSink<32> shortHeader;
  shortHeader.maximumWrite = 2;
  const PdfStatus headerStatus = extractImage(parameters, bytes.data(), bytes.size(), shortHeader);
  EXPECT_EQ(headerStatus.error, PdfError::IoFailure);
  EXPECT_EQ(headerStatus.offset, 2U);

  FixedSink<32> shortPayload;
  PdfImageExtractor extractor;
  std::array<uint8_t, 8> sourceRow{};
  std::array<uint8_t, 8> outputRow{};
  const PdfImageWorkspace workspace{
      sourceRow.data(),
      sourceRow.size(),
      outputRow.data(),
      outputRow.size(),
  };
  ASSERT_TRUE(extractor.begin(parameters, shortPayload.sink(), workspace).ok());
  shortPayload.maximumWrite = 0;
  const PdfByteSink sink = extractor.decodedSink();
  size_t written = 0;
  const PdfStatus payloadStatus = sink.write(sink.context, bytes.data(), bytes.size(), &written);
  EXPECT_EQ(payloadStatus.error, PdfError::IoFailure);
  EXPECT_EQ(payloadStatus.offset, 4U);
  EXPECT_EQ(extractor.finish().error, payloadStatus.error);
}

TEST(PdfImageExtractorDeterminism, ProducesStableDimensionsRowsAndHash) {
  constexpr std::array<uint8_t, 8> source{0, 85, 170, 255, 255, 170, 85, 0};
  constexpr std::array<uint8_t, 8> perturbedSource{0, 85, 170, 255, 255, 170, 85, 255};
  PdfImageParameters parameters = grayParameters(4, 2);
  FixedSink<32> first;
  FixedSink<32> second;
  FixedSink<32> perturbed;
  PdfImageInfo firstInfo{};
  PdfImageInfo secondInfo{};

  ASSERT_TRUE(extractImage(parameters, source.data(), source.size(), first, &firstInfo, 1).ok());
  ASSERT_TRUE(extractImage(parameters, source.data(), source.size(), second, &secondInfo, 3).ok());
  ASSERT_TRUE(extractImage(parameters, perturbedSource.data(), perturbedSource.size(), perturbed).ok());

  ASSERT_EQ(first.size, second.size);
  for (size_t index = 0; index < first.size; ++index) {
    EXPECT_EQ(first.bytes[index], second.bytes[index]);
  }
  EXPECT_EQ(firstInfo.outputWidth, 4U);
  EXPECT_EQ(firstInfo.outputHeight, 2U);
  EXPECT_EQ(firstInfo.outputBytes, 6U);
  EXPECT_EQ(fnv1a(first.bytes.data(), first.size), 0xf0ab9fa4U);
  EXPECT_NE(fnv1a(perturbed.bytes.data(), perturbed.size), 0xf0ab9fa4U);
}

TEST(PdfImageExtractorAllocationWitness, AllocatesNothingAcrossShortReadConversionAndFinish) {
  constexpr std::array<uint8_t, 8> sourceBytes{0, 85, 170, 255, 255, 170, 85, 0};
  std::array<uint8_t, 8> sourceRow{};
  std::array<uint8_t, 8> outputRow{};
  FixedSink<32> output;
  MemorySource source{sourceBytes.data(), sourceBytes.size(), 1};
  PdfImageExtractor extractor;
  const PdfImageParameters parameters = grayParameters(4, 2);
  const PdfImageWorkspace workspace{
      sourceRow.data(),
      sourceRow.size(),
      outputRow.data(),
      outputRow.size(),
  };

  gNewCount.store(0, std::memory_order_relaxed);
  gMallocCount.store(0, std::memory_order_relaxed);
  gFreeCount.store(0, std::memory_order_relaxed);
  gTrackAllocations.store(true, std::memory_order_relaxed);
  const PdfStatus beginStatus = extractor.begin(parameters, output.sink(), workspace);
  const PdfStatus extractStatus = extractor.extractDecoded(source.source());
  const PdfStatus finishStatus = extractor.finish();
  gTrackAllocations.store(false, std::memory_order_relaxed);

  EXPECT_TRUE(beginStatus.ok());
  EXPECT_TRUE(extractStatus.ok());
  EXPECT_TRUE(finishStatus.ok());
  EXPECT_EQ(gNewCount.load(std::memory_order_relaxed), 0U);
  EXPECT_EQ(gMallocCount.load(std::memory_order_relaxed), 0U);
  EXPECT_EQ(gFreeCount.load(std::memory_order_relaxed), 0U);
}

}  // namespace
