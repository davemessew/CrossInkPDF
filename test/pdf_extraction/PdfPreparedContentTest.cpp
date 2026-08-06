#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "PdfHiddenText.h"
#include "PdfPreparedContent.h"
#include "PdfTestIo.h"

namespace {

PdfStepResult runPrepared(PdfPreparedContentStreams& prepared) {
  for (uint16_t step = 0; step < 4096; ++step) {
    PdfWorkBudget budget{1, 17};
    const PdfStepResult result = prepared.step(budget);
    if (!result.yielded()) {
      return result;
    }
  }
  return PdfStepResult::failure(PdfStatus::failure(PdfError::BudgetExhausted));
}

PdfStepResult runCMap(PdfCMap& cmap) {
  while (true) {
    PdfWorkBudget budget{2, 32};
    const PdfStepResult result = cmap.step(budget);
    if (!result.yielded()) {
      return result;
    }
  }
}

PdfStepResult runInterpreter(PdfContentInterpreter& interpreter) {
  while (true) {
    PdfWorkBudget budget{2, 32};
    const PdfStepResult result = interpreter.step(budget);
    if (!result.yielded()) {
      return result;
    }
  }
}

struct InterpreterStorage {
  std::array<uint8_t, 512> sourceBuffer{};
  std::array<PdfContentOperand, 16> operands{};
  std::array<PdfContentArrayItem, 32> arrayItems{};
  std::array<uint8_t, 768> scratchText{};
  std::array<uint8_t, 512> markedText{};
  std::array<uint8_t, 8192> text{};
  std::array<PdfTextRun, PdfLimits::PageRunCount> runs{};
  std::array<PdfImagePlacement, 8> images{};
  uint32_t documentOperators = 0;
  PdfPageModel model;
  PdfContentInterpreter interpreter;

  InterpreterStorage()
      : model({text.data(), text.size(), runs.data(), static_cast<uint16_t>(runs.size()), images.data(),
               static_cast<uint16_t>(images.size())}),
        interpreter({sourceBuffer.data(), sourceBuffer.size(), operands.data(), static_cast<uint8_t>(operands.size()),
                     arrayItems.data(), static_cast<uint8_t>(arrayItems.size()), scratchText.data(),
                     static_cast<uint16_t>(scratchText.size()), markedText.data(),
                     static_cast<uint16_t>(markedText.size()), &documentOperators}) {}
};

struct DefaultFont {
  std::array<uint8_t, 64> cmapBuffer{};
  std::array<PdfCMapRecord, 2> cmapRecords{};
  std::array<PdfEncodingDifference, 2> differences{};
  std::array<PdfFontWidthRecord, 4> widths{};
  PdfCMap cmap;
  PdfSimpleEncoding encoding;
  PdfFontMap font;

  DefaultFont()
      : cmap(cmapBuffer.data(), cmapBuffer.size(),
             {cmapRecords.data(), static_cast<uint16_t>(cmapRecords.size())}),
        encoding({differences.data(), static_cast<uint16_t>(differences.size())}),
        font({widths.data(), static_cast<uint16_t>(widths.size())}) {}

  void initialize(const uint16_t mappedA = 0x03A9) {
    char mapping[5]{};
    ASSERT_EQ(std::snprintf(mapping, sizeof(mapping), "%04X", mappedA), 4);
    const std::string cmapText = "1 begincodespacerange <00> <FF> endcodespacerange 1 beginbfchar <41> <" +
                                 std::string(mapping) + "> endbfchar";
    PdfTestByteSource cmapInput(std::vector<uint8_t>(cmapText.begin(), cmapText.end()));
    ASSERT_TRUE(cmap.begin(cmapInput.source()).ok());
    ASSERT_TRUE(runCMap(cmap).complete());
    ASSERT_TRUE(encoding.begin(PdfBaseEncoding::Standard).ok());
    ASSERT_TRUE(font.begin(7, false, &cmap, &encoding, 500).ok());
    ASSERT_TRUE(font.addWidth(0, 255, 500).ok());
  }
};

std::string transcript(const PdfPageModel& model) {
  return {reinterpret_cast<const char*>(model.text()), model.textLength()};
}

bool persistentCancel(void* const context) { return *static_cast<const bool*>(context); }

struct CancelAfterChecks {
  uint32_t calls = 0;
  uint32_t trigger = 0;
};

bool cancelAfterChecks(void* const context) {
  auto& state = *static_cast<CancelAfterChecks*>(context);
  ++state.calls;
  return state.calls >= state.trigger;
}

struct GeneratedByteSource {
  uint64_t length = 0;

  PdfByteSource source() { return {this, length, readAt}; }

  static PdfStatus readAt(void* const context, const uint64_t offset, uint8_t* const destination,
                          const size_t requested, size_t* const bytesRead) {
    if (context == nullptr || destination == nullptr || bytesRead == nullptr) {
      return PdfStatus::failure(PdfError::InvalidArgument, offset);
    }
    const auto& generated = *static_cast<const GeneratedByteSource*>(context);
    if (offset > generated.length) {
      return PdfStatus::failure(PdfError::InvalidOffset, offset);
    }
    const size_t available = static_cast<size_t>(
        std::min<uint64_t>(generated.length - offset, std::numeric_limits<size_t>::max()));
    *bytesRead = std::min(requested, available);
    std::memset(destination, 'A', *bytesRead);
    return PdfStatus::success();
  }
};

struct CountingByteStore {
  uint64_t capacity = 0;
  uint64_t length = 0;

  PdfByteStore store() { return {this, capacity, reset, size, readAt, write}; }

  static PdfStatus reset(void* const context) {
    if (context == nullptr) {
      return PdfStatus::failure(PdfError::InvalidArgument);
    }
    static_cast<CountingByteStore*>(context)->length = 0;
    return PdfStatus::success();
  }

  static uint64_t size(void* const context) {
    return context == nullptr ? 0 : static_cast<const CountingByteStore*>(context)->length;
  }

  static PdfStatus readAt(void* const context, const uint64_t offset, uint8_t* const destination,
                          const size_t requested, size_t* const bytesRead) {
    if (context == nullptr || destination == nullptr || bytesRead == nullptr) {
      return PdfStatus::failure(PdfError::InvalidArgument, offset);
    }
    const auto& counting = *static_cast<const CountingByteStore*>(context);
    if (offset > counting.length) {
      return PdfStatus::failure(PdfError::InvalidOffset, offset);
    }
    const size_t available = static_cast<size_t>(
        std::min<uint64_t>(counting.length - offset, std::numeric_limits<size_t>::max()));
    *bytesRead = std::min(requested, available);
    std::memset(destination, 0, *bytesRead);
    return PdfStatus::success();
  }

  static PdfStatus write(void* const context, const uint8_t* const source, const size_t requested,
                         size_t* const bytesWritten) {
    if (context == nullptr || source == nullptr || bytesWritten == nullptr) {
      return PdfStatus::failure(PdfError::InvalidArgument);
    }
    auto& counting = *static_cast<CountingByteStore*>(context);
    if (counting.length > counting.capacity || requested > counting.capacity - counting.length) {
      return PdfStatus::failure(PdfError::InsufficientStorage, counting.length);
    }
    counting.length += requested;
    *bytesWritten = requested;
    return PdfStatus::success();
  }
};

PdfStepResult runPreparedWithLargeBudget(PdfPreparedContentStreams& prepared) {
  for (uint32_t step = 0; step < 65536U; ++step) {
    PdfWorkBudget budget{32, 4096};
    const PdfStepResult result = prepared.step(budget);
    if (!result.yielded()) {
      return result;
    }
  }
  return PdfStepResult::failure(PdfStatus::failure(PdfError::BudgetExhausted));
}

struct RecordReaderOwner {
  uint8_t* active = nullptr;
  uint8_t owner = 0;
};

struct InlineImageReplayHarness {
  uint32_t calls = 0;
  const void* sourceContext = nullptr;
  uint64_t idEndOffset = 0;
  uint64_t resumeOffset = 0;
  PdfObjectReference reference{};

  static PdfStepResult finish(void* const context, const PdfByteSource& source, const uint64_t idEndOffset,
                              PdfWorkBudget& budget, uint64_t* const resumeOffset,
                              PdfContentXObject* const image) {
    if (context == nullptr || resumeOffset == nullptr || image == nullptr || !budget.consumeOperation()) {
      return PdfStepResult::failure(PdfStatus::failure(PdfError::InvalidArgument, idEndOffset));
    }
    auto& harness = *static_cast<InlineImageReplayHarness*>(context);
    ++harness.calls;
    harness.sourceContext = source.context;
    harness.idEndOffset = idEndOffset;
    *resumeOffset = harness.resumeOffset;
    image->kind = PdfContentXObjectKind::Image;
    image->reference = harness.reference;
    return PdfStepResult::completed();
  }
};

struct YieldingInlineImageReplayHarness {
  uint32_t calls = 0;
  uint64_t resumeOffset = 0;
  PdfObjectReference reference{};

  static PdfStepResult finish(void* const context, const PdfByteSource& source, const uint64_t idEndOffset,
                              PdfWorkBudget& budget, uint64_t* const resumeOffset,
                              PdfContentXObject* const image) {
    (void)source;
    if (context == nullptr || resumeOffset == nullptr || image == nullptr) {
      return PdfStepResult::failure(PdfStatus::failure(PdfError::InvalidArgument, idEndOffset));
    }
    if (!budget.consumeOperation()) {
      return PdfStepResult::paused();
    }
    auto& harness = *static_cast<YieldingInlineImageReplayHarness*>(context);
    ++harness.calls;
    if (harness.calls == 1U) {
      return PdfStepResult::paused();
    }
    *resumeOffset = harness.resumeOffset;
    image->kind = PdfContentXObjectKind::Image;
    image->reference = harness.reference;
    return PdfStepResult::completed();
  }
};

struct PermissiveInlineImageReplayHarness {
  uint32_t calls = 0;

  static PdfStepResult finish(void* const context, const PdfByteSource& source, const uint64_t idEndOffset,
                              PdfWorkBudget& budget, uint64_t* const resumeOffset,
                              PdfContentXObject* const image) {
    (void)source;
    (void)idEndOffset;
    (void)budget;
    (void)resumeOffset;
    (void)image;
    if (context == nullptr) {
      return PdfStepResult::failure(PdfStatus::failure(PdfError::InvalidArgument));
    }
    ++static_cast<PermissiveInlineImageReplayHarness*>(context)->calls;
    return PdfStepResult::completed();
  }
};

PdfStatus selectRecordReader(void* const context, const bool sourceRequired) {
  if (context == nullptr) {
    return PdfStatus::failure(PdfError::InvalidArgument);
  }
  auto& selection = *static_cast<RecordReaderOwner*>(context);
  if (selection.active == nullptr) {
    return PdfStatus::failure(PdfError::InvalidArgument);
  }
  *selection.active = sourceRequired ? 0 : selection.owner;
  return PdfStatus::success();
}

TEST(PdfPreparedContentStreams, DecodesRawAndFlateStreamsIntoStableDeclaredRanges) {
  static constexpr char first[] = "BT /F1 12 Tf (First) Tj ET";
  static constexpr std::array<uint8_t, 35> secondCompressed{
      0x78, 0xda, 0x73, 0x0a, 0x51, 0xd0, 0x77, 0x33, 0x54, 0x30, 0x34, 0x52,
      0x08, 0x49, 0x53, 0xd0, 0x08, 0x4e, 0x4d, 0xce, 0xcf, 0x4b, 0xd1, 0x54,
      0x08, 0xc9, 0x52, 0x70, 0x0d, 0x01, 0x00, 0x5d, 0x00, 0x07, 0x1e,
  };
  static constexpr char second[] = "BT /F1 12 Tf (Second) Tj ET";

  PdfTestByteSource firstInput(
      std::vector<uint8_t>(reinterpret_cast<const uint8_t*>(first),
                           reinterpret_cast<const uint8_t*>(first) + sizeof(first) - 1U));
  PdfTestByteSource secondInput(std::vector<uint8_t>(secondCompressed.begin(), secondCompressed.end()));
  std::array<PdfEncodedContentStream, 2> streams{};
  streams[0].source = firstInput.source();
  streams[1].source = secondInput.source();
  streams[1].filters[0] = PdfStreamFilter::Flate;
  streams[1].filterCount = 1;

  std::array<uint8_t, PdfLimits::SourceBufferBytes> sourceBuffer{};
  std::array<uint8_t, PdfLimits::DecoderOutputBytes> outputBuffer{};
  std::array<uint8_t, PdfLimits::UzlibDictionaryBytes> dictionary{};
  PdfTestByteStore decodedStore(4096);
  PdfByteStore store = decodedStore.store();
  PdfPreparedContentStreams prepared(
      {sourceBuffer.data(), sourceBuffer.size(), outputBuffer.data(), outputBuffer.size(), dictionary.data(),
       dictionary.size()});

  ASSERT_TRUE(prepared.begin(streams.data(), streams.size(), store).ok());
  const PdfStepResult result = runPrepared(prepared);

  ASSERT_TRUE(result.complete()) << static_cast<int>(result.status.error) << '@' << result.status.offset;
  ASSERT_EQ(prepared.count(), 2U);
  EXPECT_EQ(prepared.decodedBytes(), (sizeof(first) - 1U) + (sizeof(second) - 1U));
  std::array<uint8_t, 64> decoded{};
  ASSERT_TRUE(pdfReadExact(prepared.sources()[0], 0, decoded.data(), sizeof(first) - 1U).ok());
  EXPECT_EQ(std::memcmp(decoded.data(), first, sizeof(first) - 1U), 0);
  ASSERT_TRUE(pdfReadExact(prepared.sources()[1], 0, decoded.data(), sizeof(second) - 1U).ok());
  EXPECT_EQ(std::memcmp(decoded.data(), second, sizeof(second) - 1U), 0);
  EXPECT_EQ(decodedStore.resetCount(), 1U);
}

TEST(PdfPreparedContentStreams, AppendsWithoutResetAndReturnsOnlyTheNewRange) {
  PdfTestByteSource source(std::vector<uint8_t>{'n', 'e', 'w'});
  const PdfEncodedContentStream stream{source.source(), {}, 0};
  CountingByteStore store{32, 5};
  std::array<uint8_t, PdfLimits::SourceBufferBytes> sourceBuffer{};
  std::array<uint8_t, PdfLimits::DecoderOutputBytes> outputBuffer{};
  std::array<uint8_t, PdfLimits::UzlibDictionaryBytes> dictionary{};
  PdfPreparedContentStreams prepared(
      {sourceBuffer.data(), sourceBuffer.size(), outputBuffer.data(), outputBuffer.size(), dictionary.data(),
       dictionary.size()});

  ASSERT_TRUE(prepared.beginAppend(&stream, 1, store.store(), 5).ok());
  const PdfStepResult result = runPrepared(prepared);
  ASSERT_TRUE(result.complete()) << "error=" << static_cast<int>(result.status.error)
                                 << " offset=" << result.status.offset;
  ASSERT_EQ(prepared.count(), 1U);
  EXPECT_EQ(prepared.decodedBytes(), 8U);
  EXPECT_EQ(prepared.sources()[0].size, 3U);
  EXPECT_EQ(store.length, 8U);
}

TEST(PdfPreparedContentStreams, ClearsPartialSpoolBeforeReportingRequiredStreamFailure) {
  static constexpr char first[] = "first decoded bytes";
  static constexpr char unsupported[] = "unsupported bytes";
  PdfTestByteSource firstInput(
      std::vector<uint8_t>(reinterpret_cast<const uint8_t*>(first),
                           reinterpret_cast<const uint8_t*>(first) + sizeof(first) - 1U));
  PdfTestByteSource unsupportedInput(
      std::vector<uint8_t>(reinterpret_cast<const uint8_t*>(unsupported),
                           reinterpret_cast<const uint8_t*>(unsupported) + sizeof(unsupported) - 1U));
  std::array<PdfEncodedContentStream, 2> streams{};
  streams[0].source = firstInput.source();
  streams[1].source = unsupportedInput.source();
  streams[1].filters[0] = PdfStreamFilter::Unsupported;
  streams[1].filterCount = 1;
  std::array<uint8_t, PdfLimits::SourceBufferBytes> sourceBuffer{};
  std::array<uint8_t, PdfLimits::DecoderOutputBytes> outputBuffer{};
  std::array<uint8_t, PdfLimits::UzlibDictionaryBytes> dictionary{};
  PdfTestByteStore decodedStore(4096);
  PdfByteStore store = decodedStore.store();
  PdfPreparedContentStreams prepared(
      {sourceBuffer.data(), sourceBuffer.size(), outputBuffer.data(), outputBuffer.size(), dictionary.data(),
       dictionary.size()});
  ASSERT_TRUE(prepared.begin(streams.data(), streams.size(), store).ok());

  const PdfStepResult result = runPrepared(prepared);

  ASSERT_TRUE(result.failed());
  EXPECT_EQ(result.status.error, PdfError::UnsupportedFilter);
  EXPECT_TRUE(decodedStore.bytes().empty());
  EXPECT_EQ(decodedStore.resetCount(), 2U);
  EXPECT_EQ(prepared.count(), 0U);
  EXPECT_EQ(prepared.decodedBytes(), 0U);
}

TEST(PdfPreparedContentStreams, RejectsMoreThanSixteenSourcesBeforeTouchingSpool) {
  std::array<PdfEncodedContentStream, PdfPreparedContentStreams::MaxSources + 1U> streams{};
  PdfTestByteStore decodedStore(4096);
  PdfByteStore store = decodedStore.store();
  std::array<uint8_t, PdfLimits::SourceBufferBytes> sourceBuffer{};
  std::array<uint8_t, PdfLimits::DecoderOutputBytes> outputBuffer{};
  std::array<uint8_t, PdfLimits::UzlibDictionaryBytes> dictionary{};
  PdfPreparedContentStreams prepared(
      {sourceBuffer.data(), sourceBuffer.size(), outputBuffer.data(), outputBuffer.size(), dictionary.data(),
       dictionary.size()});

  EXPECT_EQ(prepared.begin(streams.data(), streams.size(), store).error, PdfError::LimitExceeded);
  EXPECT_EQ(decodedStore.resetCount(), 0U);
}

TEST(PdfPreparedContentStreams, PersistentCancellationClearsPartialSpoolAndTerminates) {
  PdfTestByteSource input(std::vector<uint8_t>(5000, static_cast<uint8_t>('A')));
  PdfEncodedContentStream stream{};
  stream.source = input.source();
  std::array<uint8_t, PdfLimits::SourceBufferBytes> sourceBuffer{};
  std::array<uint8_t, PdfLimits::DecoderOutputBytes> outputBuffer{};
  std::array<uint8_t, PdfLimits::UzlibDictionaryBytes> dictionary{};
  PdfTestByteStore decodedStore(8192);
  PdfPreparedContentStreams prepared(
      {sourceBuffer.data(), sourceBuffer.size(), outputBuffer.data(), outputBuffer.size(), dictionary.data(),
       dictionary.size()});
  ASSERT_TRUE(prepared.begin(&stream, 1, decodedStore.store()).ok());

  for (uint16_t slice = 0; slice < 128 && decodedStore.bytes().empty(); ++slice) {
    PdfWorkBudget budget{32, 4096};
    ASSERT_TRUE(prepared.step(budget).yielded());
  }
  ASSERT_FALSE(decodedStore.bytes().empty());
  ASSERT_EQ(decodedStore.resetCount(), 1U);

  bool cancelled = true;
  PdfWorkBudget cancelBudget{1, 17, &cancelled, persistentCancel};
  ASSERT_TRUE(prepared.step(cancelBudget).yielded());
  EXPECT_FALSE(decodedStore.bytes().empty());

  PdfWorkBudget cleanupBudget{1, 17, &cancelled, persistentCancel};
  const PdfStepResult result = prepared.step(cleanupBudget);

  ASSERT_TRUE(result.failed());
  EXPECT_EQ(result.status.error, PdfError::Cancelled);
  EXPECT_TRUE(decodedStore.bytes().empty());
  EXPECT_EQ(decodedStore.resetCount(), 2U);
  EXPECT_EQ(cleanupBudget.operationsRemaining, 0U);
}

TEST(PdfPreparedContentStreams, DecoderOriginatedCancellationUsesTheSameBoundedCleanup) {
  static constexpr char content[] = "decoded content";
  PdfTestByteSource input(
      std::vector<uint8_t>(reinterpret_cast<const uint8_t*>(content),
                           reinterpret_cast<const uint8_t*>(content) + sizeof(content) - 1U));
  PdfEncodedContentStream stream{};
  stream.source = input.source();
  std::array<uint8_t, PdfLimits::SourceBufferBytes> sourceBuffer{};
  std::array<uint8_t, PdfLimits::DecoderOutputBytes> outputBuffer{};
  std::array<uint8_t, PdfLimits::UzlibDictionaryBytes> dictionary{};
  PdfTestByteStore decodedStore(4096);
  PdfPreparedContentStreams prepared(
      {sourceBuffer.data(), sourceBuffer.size(), outputBuffer.data(), outputBuffer.size(), dictionary.data(),
       dictionary.size()});
  ASSERT_TRUE(prepared.begin(&stream, 1, decodedStore.store()).ok());
  PdfWorkBudget setupBudget{1, 17};
  ASSERT_TRUE(prepared.step(setupBudget).yielded());
  setupBudget = {1, 17};
  ASSERT_TRUE(prepared.step(setupBudget).yielded());

  CancelAfterChecks cancellation{0, 3};
  PdfWorkBudget decodeBudget{8, 64, &cancellation, cancelAfterChecks};
  ASSERT_TRUE(prepared.step(decodeBudget).yielded());
  ASSERT_EQ(cancellation.calls, 3U);

  PdfWorkBudget cleanupBudget{1, 17, &cancellation, cancelAfterChecks};
  const PdfStepResult result = prepared.step(cleanupBudget);

  ASSERT_TRUE(result.failed());
  EXPECT_EQ(result.status.error, PdfError::Cancelled);
  EXPECT_EQ(cancellation.calls, 3U);
  EXPECT_TRUE(decodedStore.bytes().empty());
  EXPECT_EQ(decodedStore.resetCount(), 2U);
  EXPECT_EQ(cleanupBudget.operationsRemaining, 0U);
}

TEST(PdfPreparedContentStreams, InvalidRebeginDoesNotExposePreviouslyCompletedSources) {
  static constexpr char content[] = "completed content";
  PdfTestByteSource input(
      std::vector<uint8_t>(reinterpret_cast<const uint8_t*>(content),
                           reinterpret_cast<const uint8_t*>(content) + sizeof(content) - 1U));
  PdfEncodedContentStream stream{};
  stream.source = input.source();
  std::array<uint8_t, PdfLimits::SourceBufferBytes> sourceBuffer{};
  std::array<uint8_t, PdfLimits::DecoderOutputBytes> outputBuffer{};
  std::array<uint8_t, PdfLimits::UzlibDictionaryBytes> dictionary{};
  PdfTestByteStore decodedStore(4096);
  const PdfByteStore store = decodedStore.store();
  PdfPreparedContentStreams prepared(
      {sourceBuffer.data(), sourceBuffer.size(), outputBuffer.data(), outputBuffer.size(), dictionary.data(),
       dictionary.size()});
  ASSERT_TRUE(prepared.begin(&stream, 1, store).ok());
  ASSERT_TRUE(runPrepared(prepared).complete());
  ASSERT_EQ(prepared.count(), 1U);
  ASSERT_TRUE(prepared.sources()[0].valid());

  EXPECT_EQ(prepared.begin(nullptr, 0, store).error, PdfError::InvalidArgument);

  EXPECT_EQ(prepared.count(), 0U);
  EXPECT_EQ(prepared.decodedBytes(), 0U);
  EXPECT_FALSE(prepared.sources()[0].valid());
}

TEST(PdfPreparedContentStreams, NeverAllowsCallerToRelaxGlobalExpansionCeilings) {
  static constexpr std::array<uint8_t, 90> highRatioFlate{
      0x78, 0xda, 0xed, 0xc1, 0xb1, 0x11, 0x00, 0x10, 0x00, 0x04, 0xb0, 0x55, 0x7e, 0x03, 0xf4,
      0xce, 0x18, 0x7a, 0x85, 0x4e, 0x6b, 0x7f, 0x7b, 0xb8, 0x24, 0x2d, 0x35, 0xbd, 0xa7, 0xcc,
      0x75, 0xee, 0x4e, 0xcb, 0x18, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0xe0, 0x2b, 0x0f, 0x6b, 0xad, 0x6f, 0xbb,
  };
  const PdfStreamDecodeLimits relaxed{std::numeric_limits<uint64_t>::max(),
                                      std::numeric_limits<uint16_t>::max()};

  for (const bool ratioCase : {false, true}) {
    SCOPED_TRACE(ratioCase ? "ratio" : "absolute");
    GeneratedByteSource generated{PdfLimits::MaxExpandedRequiredStreamBytes + 1U};
    PdfTestByteSource compressed(std::vector<uint8_t>(highRatioFlate.begin(), highRatioFlate.end()));
    PdfEncodedContentStream stream{};
    stream.source = ratioCase ? compressed.source() : generated.source();
    if (ratioCase) {
      stream.filters[0] = PdfStreamFilter::Flate;
      stream.filterCount = 1;
    }

    std::array<uint8_t, PdfLimits::SourceBufferBytes> sourceBuffer{};
    std::array<uint8_t, PdfLimits::DecoderOutputBytes> outputBuffer{};
    std::array<uint8_t, PdfLimits::UzlibDictionaryBytes> dictionary{};
    CountingByteStore decodedStore{PdfLimits::MaxExpandedRequiredStreamBytes + 1U};
    PdfPreparedContentStreams prepared(
        {sourceBuffer.data(), sourceBuffer.size(), outputBuffer.data(), outputBuffer.size(), dictionary.data(),
         dictionary.size()});
    ASSERT_TRUE(prepared.begin(&stream, 1, decodedStore.store(), relaxed).ok());

    const PdfStepResult result = runPreparedWithLargeBudget(prepared);

    EXPECT_TRUE(result.failed());
    EXPECT_EQ(result.status.error, PdfError::ExpansionLimit);
    EXPECT_EQ(decodedStore.length, 0U);
    EXPECT_EQ(prepared.count(), 0U);
    EXPECT_EQ(prepared.decodedBytes(), 0U);
  }
}

TEST(PdfPreparedContentResources, InterpreterUsesStableActualTextAndToUnicodeResources) {
  DefaultFont defaultFont;
  defaultFont.initialize();
  std::array<PdfPreparedFontResource, 2> fonts{};
  std::array<PdfPreparedXObjectResource, 2> xobjects{};
  PdfPreparedContentResources resources({fonts.data(), fonts.size(), xobjects.data(), xobjects.size()});
  ASSERT_TRUE(resources.reset().ok());
  ASSERT_TRUE(resources.addFont(reinterpret_cast<const uint8_t*>("F1"), 2, &defaultFont.font).ok());

  const std::string content =
      "BT /F1 12 Tf 1 0 0 1 72 700 Tm "
      "/Span << /ActualText <FEFF00410063007400750061006C> >> BDC (Z) Tj EMC "
      "0 -24 Td <41> Tj ET";
  PdfTestByteSource input(std::vector<uint8_t>(content.begin(), content.end()));
  const PdfByteSource source = input.source();
  InterpreterStorage storage;
  const PdfContentResources descriptor = resources.descriptor();
  ASSERT_TRUE(storage.interpreter.begin(&source, 1, descriptor, storage.model).ok());

  const PdfStepResult result = runInterpreter(storage.interpreter);

  ASSERT_TRUE(result.complete()) << static_cast<int>(result.status.error) << '@' << result.status.offset;
  EXPECT_EQ(transcript(storage.model), "Actual\xCE\xA9");
  ASSERT_EQ(storage.model.runCount(), 2U);
  EXPECT_NE(storage.model.runs()[0].flags & PdfTextActualText, 0U);
}

TEST(PdfPreparedContentResources, DirectDescriptorRemainsStableAcrossCooperativeInterpreterSteps) {
  static_assert(std::is_same_v<decltype(std::declval<PdfPreparedContentResources&>().descriptor()),
                               const PdfContentResources&>);
  static_assert(!std::is_copy_constructible_v<PdfPreparedContentResources>);
  static_assert(!std::is_copy_assignable_v<PdfPreparedContentResources>);
  static_assert(!std::is_move_constructible_v<PdfPreparedContentResources>);
  static_assert(!std::is_move_assignable_v<PdfPreparedContentResources>);
  DefaultFont defaultFont;
  defaultFont.initialize();
  std::array<PdfPreparedFontResource, 1> fonts{};
  std::array<PdfPreparedXObjectResource, 1> xobjects{};
  PdfPreparedContentResources resources({fonts.data(), fonts.size(), xobjects.data(), xobjects.size()});
  ASSERT_TRUE(resources.reset().ok());
  ASSERT_TRUE(resources.addFont(reinterpret_cast<const uint8_t*>("F1"), 2, &defaultFont.font).ok());
  PdfContentXObject image{};
  image.kind = PdfContentXObjectKind::Image;
  image.reference = {21, 0};
  image.pixelWidth = 20;
  image.pixelHeight = 10;
  ASSERT_TRUE(resources.addXObject(reinterpret_cast<const uint8_t*>("Im1"), 3, image).ok());

  const std::string content =
      "q 20 0 0 10 50 600 cm /Im1 Do Q "
      "BT /F1 12 Tf 1 0 0 1 72 700 Tm (Stable descriptor) Tj ET";
  PdfTestByteSource input(std::vector<uint8_t>(content.begin(), content.end()));
  const PdfByteSource source = input.source();
  InterpreterStorage storage;
  ASSERT_TRUE(storage.interpreter.begin(&source, 1, resources.descriptor(), storage.model).ok());

  const PdfStepResult result = runInterpreter(storage.interpreter);

  ASSERT_TRUE(result.complete()) << static_cast<int>(result.status.error) << '@' << result.status.offset;
  EXPECT_EQ(transcript(storage.model), "Stable descriptor");
  ASSERT_EQ(storage.model.imageCount(), 1U);
  EXPECT_EQ(storage.model.images()[0].reference.objectNumber, 21U);
}

TEST(PdfPreparedContentResources, ForwardsSharedInlineImageReplayHookWithoutDictionaryTokenCallback) {
  InlineImageReplayHarness replay;
  replay.resumeOffset = 31;
  replay.reference = {44, 0};
  const PdfPreparedContentInlineImageHooks inlineImageHooks{&replay, nullptr, InlineImageReplayHarness::finish};
  std::array<PdfPreparedFontResource, 1> fonts{};
  std::array<PdfPreparedXObjectResource, 1> xobjects{};
  PdfPreparedContentResources resources(
      {fonts.data(), fonts.size(), xobjects.data(), xobjects.size(), &inlineImageHooks});
  ASSERT_TRUE(resources.reset().ok());

  const PdfContentResources& descriptor = resources.descriptor();
  EXPECT_EQ(descriptor.consumeInlineImageToken, nullptr);
  ASSERT_NE(descriptor.finishInlineImage, nullptr);
  const uint8_t sourceByte = 0;
  PdfTestByteSource input({sourceByte});
  const PdfByteSource source = input.source();
  PdfWorkBudget budget{1, 1};
  uint64_t resumeOffset = 0;
  PdfContentXObject image;

  const PdfStepResult result = descriptor.finishInlineImage(descriptor.context, source, 17, budget, &resumeOffset,
                                                            &image);

  ASSERT_TRUE(result.complete());
  EXPECT_EQ(replay.calls, 1U);
  EXPECT_EQ(replay.sourceContext, source.context);
  EXPECT_EQ(replay.idEndOffset, 17U);
  EXPECT_EQ(resumeOffset, 31U);
  EXPECT_EQ(image.reference.objectNumber, 44U);
}

TEST(PdfPreparedContentResources, CopiesInlineImageHooksAcrossYieldedReplayLifetime) {
  YieldingInlineImageReplayHarness replay;
  replay.resumeOffset = 37;
  replay.reference = {46, 0};
  PdfPreparedContentInlineImageHooks inlineImageHooks{&replay, nullptr, YieldingInlineImageReplayHarness::finish};
  std::array<PdfPreparedFontResource, 1> fonts{};
  std::array<PdfPreparedXObjectResource, 1> xobjects{};
  PdfPreparedContentResources resources(
      {fonts.data(), fonts.size(), xobjects.data(), xobjects.size(), &inlineImageHooks});
  ASSERT_TRUE(resources.reset().ok());
  const PdfContentResources& descriptor = resources.descriptor();
  ASSERT_NE(descriptor.finishInlineImage, nullptr);
  PdfTestByteSource input({0});
  const PdfByteSource source = input.source();
  uint64_t resumeOffset = 0;
  PdfContentXObject image;
  PdfWorkBudget firstBudget{1, 1};

  ASSERT_TRUE(
      descriptor.finishInlineImage(descriptor.context, source, 19, firstBudget, &resumeOffset, &image).yielded());
  ASSERT_EQ(replay.calls, 1U);
  inlineImageHooks = {};

  PdfWorkBudget secondBudget{1, 1};
  const PdfStepResult result =
      descriptor.finishInlineImage(descriptor.context, source, 19, secondBudget, &resumeOffset, &image);

  ASSERT_TRUE(result.complete()) << static_cast<int>(result.status.error) << '@' << result.status.offset;
  EXPECT_EQ(replay.calls, 2U);
  EXPECT_EQ(resumeOffset, 37U);
  EXPECT_EQ(image.reference.objectNumber, 46U);
}

TEST(PdfPreparedContentResources, RejectsNullInlineImageOutputsBeforeCallingHook) {
  PermissiveInlineImageReplayHarness replay;
  const PdfPreparedContentInlineImageHooks inlineImageHooks{&replay, nullptr,
                                                            PermissiveInlineImageReplayHarness::finish};
  std::array<PdfPreparedFontResource, 1> fonts{};
  std::array<PdfPreparedXObjectResource, 1> xobjects{};
  PdfPreparedContentResources resources(
      {fonts.data(), fonts.size(), xobjects.data(), xobjects.size(), &inlineImageHooks});
  ASSERT_TRUE(resources.reset().ok());
  const PdfContentResources& descriptor = resources.descriptor();
  PdfTestByteSource input({0});
  const PdfByteSource source = input.source();
  PdfContentXObject image;
  PdfWorkBudget missingResumeBudget{1, 1};

  const PdfStepResult missingResume =
      descriptor.finishInlineImage(descriptor.context, source, 23, missingResumeBudget, nullptr, &image);

  ASSERT_TRUE(missingResume.failed());
  EXPECT_EQ(missingResume.status.error, PdfError::InvalidArgument);
  EXPECT_EQ(replay.calls, 0U);

  uint64_t resumeOffset = 0;
  PdfWorkBudget missingImageBudget{1, 1};
  const PdfStepResult missingImage =
      descriptor.finishInlineImage(descriptor.context, source, 23, missingImageBudget, &resumeOffset, nullptr);

  ASSERT_TRUE(missingImage.failed());
  EXPECT_EQ(missingImage.status.error, PdfError::InvalidArgument);
  EXPECT_EQ(replay.calls, 0U);
}

TEST(PdfPreparedContentResources, FormWithoutOwnResourcesInheritsCallingDescriptor) {
  DefaultFont defaultFont;
  defaultFont.initialize();
  PdfTestByteSource formInput(
      std::vector<uint8_t>{'B', 'T', ' ', '/', 'F', '1', ' ', '1', '2', ' ', 'T', 'f', ' ', '(', 'A', ')', ' ',
                           'T', 'j', ' ', 'E', 'T'});
  std::array<PdfPreparedFontResource, 1> fonts{};
  std::array<PdfPreparedXObjectResource, 1> xobjects{};
  PdfPreparedContentResources resources({fonts.data(), fonts.size(), xobjects.data(), xobjects.size()});
  ASSERT_TRUE(resources.reset().ok());
  ASSERT_TRUE(resources.addFont(reinterpret_cast<const uint8_t*>("F1"), 2, &defaultFont.font).ok());
  PdfContentXObject form;
  form.kind = PdfContentXObjectKind::Form;
  form.reference = {45, 0};
  form.content = formInput.source();
  form.resources = nullptr;
  form.bbox = {0, 0, PdfFixed16::fromInteger(612).raw, PdfFixed16::fromInteger(792).raw};
  form.hasBBox = true;
  ASSERT_TRUE(resources.addXObject(reinterpret_cast<const uint8_t*>("Fm1"), 3, form).ok());

  const std::string pageText = "/Fm1 Do";
  PdfTestByteSource pageInput(std::vector<uint8_t>(pageText.begin(), pageText.end()));
  const PdfByteSource pageSource = pageInput.source();
  InterpreterStorage storage;
  ASSERT_TRUE(storage.interpreter.begin(&pageSource, 1, resources.descriptor(), storage.model).ok());

  const PdfStepResult result = runInterpreter(storage.interpreter);

  ASSERT_TRUE(result.complete()) << static_cast<int>(result.status.error) << '@' << result.status.offset;
  EXPECT_EQ(transcript(storage.model), "Ω");
  EXPECT_EQ(storage.interpreter.maximumFormDepth(), 1U);
}

TEST(PdfPreparedContentResources, NestedFormsShadowInheritAndRestoreCallingResources) {
  DefaultFont pageFont;
  DefaultFont childFont;
  pageFont.initialize(0x03A9);
  childFont.initialize(0x03B2);

  const std::string grandchildText = "BT /F1 12 Tf (A ) Tj ET";
  PdfTestByteSource grandchildInput(std::vector<uint8_t>(grandchildText.begin(), grandchildText.end()));
  PdfContentXObject grandchild;
  grandchild.kind = PdfContentXObjectKind::Form;
  grandchild.reference = {47, 0};
  grandchild.content = grandchildInput.source();
  grandchild.resources = nullptr;
  grandchild.bbox = {0, 0, PdfFixed16::fromInteger(612).raw, PdfFixed16::fromInteger(792).raw};
  grandchild.hasBBox = true;

  std::array<PdfPreparedFontResource, 1> childFonts{};
  std::array<PdfPreparedXObjectResource, 1> childXObjects{};
  PdfPreparedContentResources childResources(
      {childFonts.data(), childFonts.size(), childXObjects.data(), childXObjects.size()});
  ASSERT_TRUE(childResources.reset().ok());
  ASSERT_TRUE(childResources.addFont(reinterpret_cast<const uint8_t*>("F1"), 2, &childFont.font).ok());
  ASSERT_TRUE(childResources.addXObject(reinterpret_cast<const uint8_t*>("FmNested"), 8, grandchild).ok());

  const std::string childText = "BT /F1 12 Tf (A ) Tj ET /FmNested Do BT /F1 12 Tf (A ) Tj ET";
  PdfTestByteSource childInput(std::vector<uint8_t>(childText.begin(), childText.end()));
  PdfContentXObject child;
  child.kind = PdfContentXObjectKind::Form;
  child.reference = {48, 0};
  child.content = childInput.source();
  child.resources = &childResources.descriptor();
  child.bbox = {0, 0, PdfFixed16::fromInteger(612).raw, PdfFixed16::fromInteger(792).raw};
  child.hasBBox = true;

  std::array<PdfPreparedFontResource, 1> pageFonts{};
  std::array<PdfPreparedXObjectResource, 1> pageXObjects{};
  PdfPreparedContentResources pageResources(
      {pageFonts.data(), pageFonts.size(), pageXObjects.data(), pageXObjects.size()});
  ASSERT_TRUE(pageResources.reset().ok());
  ASSERT_TRUE(pageResources.addFont(reinterpret_cast<const uint8_t*>("F1"), 2, &pageFont.font).ok());
  ASSERT_TRUE(pageResources.addXObject(reinterpret_cast<const uint8_t*>("FmChild"), 7, child).ok());

  const std::string pageText = "BT /F1 12 Tf (A ) Tj ET /FmChild Do BT /F1 12 Tf (A) Tj ET";
  PdfTestByteSource pageInput(std::vector<uint8_t>(pageText.begin(), pageText.end()));
  const PdfByteSource pageSource = pageInput.source();
  InterpreterStorage storage;
  ASSERT_TRUE(storage.interpreter.begin(&pageSource, 1, pageResources.descriptor(), storage.model).ok());

  const PdfStepResult result = runInterpreter(storage.interpreter);

  ASSERT_TRUE(result.complete()) << static_cast<int>(result.status.error) << '@' << result.status.offset;
  EXPECT_EQ(transcript(storage.model), "\xCE\xA9 \xCE\xB2 \xCE\xB2 \xCE\xB2 \xCE\xA9");
  EXPECT_EQ(storage.interpreter.maximumFormDepth(), 2U);
}

TEST(PdfPreparedContentResources, ImageResourceMakesHiddenOcrQualificationAndDeduplicationObservable) {
  DefaultFont defaultFont;
  defaultFont.initialize();
  std::array<PdfPreparedFontResource, 2> fonts{};
  std::array<PdfPreparedXObjectResource, 2> xobjects{};
  PdfPreparedContentResources resources({fonts.data(), fonts.size(), xobjects.data(), xobjects.size()});
  ASSERT_TRUE(resources.reset().ok());
  ASSERT_TRUE(resources.addFont(reinterpret_cast<const uint8_t*>("F1"), 2, &defaultFont.font).ok());
  PdfContentXObject image{};
  image.kind = PdfContentXObjectKind::Image;
  image.reference = {20, 0};
  image.pixelWidth = 200;
  image.pixelHeight = 200;
  ASSERT_TRUE(resources.addXObject(reinterpret_cast<const uint8_t*>("Im1"), 3, image).ok());

  const std::string content =
      "q 180 0 0 160 60 560 cm /Im1 Do Q "
      "BT /F1 12 Tf 3 Tr 1 0 0 1 72 620 Tm (Duplicate visible text.) Tj ET "
      "BT /F1 12 Tf 0 Tr 1 0 0 1 72 720 Tm (Duplicate visible text.) Tj ET";
  PdfTestByteSource input(std::vector<uint8_t>(content.begin(), content.end()));
  const PdfByteSource source = input.source();
  InterpreterStorage storage;
  const PdfContentResources descriptor = resources.descriptor();
  ASSERT_TRUE(storage.interpreter.begin(&source, 1, descriptor, storage.model).ok());
  ASSERT_TRUE(runInterpreter(storage.interpreter).complete());
  ASSERT_EQ(storage.model.runCount(), 2U);
  ASSERT_EQ(storage.model.imageCount(), 1U);
  const uint16_t hiddenIndex = (storage.model.runs()[0].flags & PdfTextHidden) != 0 ? 0 : 1;
  const PdfHiddenTextContext context{
      {PdfFixed16::fromInteger(0).raw, PdfFixed16::fromInteger(0).raw, PdfFixed16::fromInteger(612).raw,
       PdfFixed16::fromInteger(792).raw},
      storage.model.runs(), storage.model.runCount(), storage.model.text(), storage.model.textLength(),
      storage.model.images(), storage.model.imageCount(),
  };

  EXPECT_EQ(pdfClassifyHiddenText(context, hiddenIndex), PdfHiddenTextDecision::DuplicateVisible);
}

TEST(PdfPreparedContentResources, RejectsUnboundedOrUnstableRegistration) {
  std::array<PdfPreparedFontResource, PdfPreparedContentResources::MaxFonts + 1U> tooManyFonts{};
  std::array<PdfPreparedXObjectResource, 1> xobjects{};
  PdfPreparedContentResources oversized(
      {tooManyFonts.data(), tooManyFonts.size(), xobjects.data(), xobjects.size()});
  EXPECT_EQ(oversized.reset().error, PdfError::LimitExceeded);

  std::array<PdfPreparedFontResource, 1> fonts{};
  PdfPreparedContentResources bounded({fonts.data(), fonts.size(), xobjects.data(), xobjects.size()});
  ASSERT_TRUE(bounded.reset().ok());
  EXPECT_EQ(bounded.addFont(reinterpret_cast<const uint8_t*>("F1"), 2, nullptr).error,
            PdfError::InvalidArgument);
  EXPECT_EQ(bounded.addFont(reinterpret_cast<const uint8_t*>("font-name-that-is-deliberately-too-long"), 39,
                            reinterpret_cast<PdfFontMap*>(1))
                .error,
            PdfError::LimitExceeded);
}

TEST(PdfPreparedContentResources, RejectsFontThatWouldReadSpillDuringInterpretation) {
  std::array<PdfFontWidthRecord, 1> widths{};
  PdfTestRecordStore spill(sizeof(PdfFontWidthRecord), 1);
  PdfFontMap spilledFont({widths.data(), widths.size(), spill.store()});
  ASSERT_TRUE(spilledFont.begin(3, false, nullptr, nullptr).ok());
  ASSERT_TRUE(spilledFont.addWidth(0, 31, 500).ok());
  ASSERT_TRUE(spilledFont.addWidth(32, 255, 600).ok());

  std::array<PdfPreparedFontResource, 1> fonts{};
  std::array<PdfPreparedXObjectResource, 1> xobjects{};
  PdfPreparedContentResources resources({fonts.data(), fonts.size(), xobjects.data(), xobjects.size()});
  ASSERT_TRUE(resources.reset().ok());

  EXPECT_EQ(resources.addFont(reinterpret_cast<const uint8_t*>("F1"), 2, &spilledFont).error,
            PdfError::LimitExceeded);
  EXPECT_EQ(spill.readCount(), 0U);
}

TEST(PdfPreparedContentResources, MaterializesOnlyPageUsedGlyphsFromLargeSpilledSourceMap) {
  uint8_t activeReader = 0;
  RecordReaderOwner cmapReader{&activeReader, 1};
  RecordReaderOwner widthReader{&activeReader, 2};
  std::array<uint8_t, 512> cmapBuffer{};
  std::array<PdfCMapRecord, 1> residentCmapRecords{};
  PdfTestRecordStore cmapSpill(sizeof(PdfCMapRecord), 63);
  cmapSpill.requireReaderOwner(&activeReader, cmapReader.owner);
  PdfCMap sourceCmap(cmapBuffer.data(), cmapBuffer.size(),
                     {residentCmapRecords.data(), static_cast<uint16_t>(residentCmapRecords.size()),
                      cmapSpill.store(), &cmapReader, selectRecordReader});

  std::string cmapText =
      "1 begincodespacerange <00> <FF> endcodespacerange 64 beginbfchar ";
  for (uint16_t code = 0x20; code < 0x60; ++code) {
    char mapping[18]{};
    const int length = std::snprintf(mapping, sizeof(mapping), "<%02X> <%04X> ", code, code);
    ASSERT_GT(length, 0);
    ASSERT_LT(static_cast<size_t>(length), sizeof(mapping));
    cmapText.append(mapping, static_cast<size_t>(length));
  }
  cmapText += "endbfchar";
  PdfTestByteSource cmapInput(std::vector<uint8_t>(cmapText.begin(), cmapText.end()));
  ASSERT_TRUE(sourceCmap.begin(cmapInput.source()).ok());
  ASSERT_TRUE(runCMap(sourceCmap).complete());

  std::array<PdfFontWidthRecord, 1> residentWidths{};
  PdfTestRecordStore widthSpill(sizeof(PdfFontWidthRecord), 2);
  widthSpill.requireReaderOwner(&activeReader, widthReader.owner);
  PdfFontMap sourceFont({residentWidths.data(), static_cast<uint16_t>(residentWidths.size()), widthSpill.store(),
                         &widthReader, selectRecordReader});
  ASSERT_TRUE(sourceFont.begin(19, false, &sourceCmap, nullptr, 500).ok());
  ASSERT_TRUE(sourceFont.addWidth(0, 31, 400).ok());
  ASSERT_TRUE(sourceFont.addWidth(32, 63, 500).ok());
  ASSERT_TRUE(sourceFont.addWidth(64, 255, 600).ok());

  std::array<PdfDecodedGlyph, 2> pageGlyphs{};
  PdfFontMapWorkspace pageFontWorkspace{};
  pageFontWorkspace.materializedGlyphs = pageGlyphs.data();
  pageFontWorkspace.materializedGlyphCapacity = static_cast<uint16_t>(pageGlyphs.size());
  PdfFontMap pageFont(pageFontWorkspace);
  ASSERT_TRUE(pageFont.beginMaterialized(sourceFont.fontId(), sourceFont.cid()).ok());
  static constexpr uint8_t encodedPageText[]{0x5e, 0x5f, 0x5e, 0x5e};
  ASSERT_TRUE(pageFont.materializeString(sourceFont, encodedPageText, sizeof(encodedPageText)).ok());
  EXPECT_EQ(pageFont.materializedGlyphCount(), 2U);
  ASSERT_GT(cmapSpill.readCount(), 0U);
  ASSERT_GT(widthSpill.readCount(), 0U);

  std::array<PdfPreparedFontResource, 1> fonts{};
  std::array<PdfPreparedXObjectResource, 1> xobjects{};
  PdfPreparedContentResources resources({fonts.data(), fonts.size(), xobjects.data(), xobjects.size()});
  ASSERT_TRUE(resources.reset().ok());
  ASSERT_TRUE(resources.addFont(reinterpret_cast<const uint8_t*>("F1"), 2, &pageFont).ok());

  bool interpreting = true;
  cmapSpill.forbidReadsWhile(&interpreting);
  widthSpill.forbidReadsWhile(&interpreting);
  const uint32_t cmapReadsBeforeInterpretation = cmapSpill.readCount();
  const uint32_t widthReadsBeforeInterpretation = widthSpill.readCount();
  const std::string content = "BT /F1 12 Tf 1 0 0 1 72 700 Tm <5E5F5E5E> Tj ET";
  PdfTestByteSource contentInput(std::vector<uint8_t>(content.begin(), content.end()));
  const PdfByteSource contentSource = contentInput.source();
  InterpreterStorage storage;
  ASSERT_TRUE(storage.interpreter.begin(&contentSource, 1, resources.descriptor(), storage.model).ok());

  const PdfStepResult result = runInterpreter(storage.interpreter);

  ASSERT_TRUE(result.complete()) << static_cast<int>(result.status.error) << '@' << result.status.offset;
  EXPECT_EQ(transcript(storage.model), "^_^^");
  EXPECT_EQ(cmapSpill.readCount(), cmapReadsBeforeInterpretation);
  EXPECT_EQ(widthSpill.readCount(), widthReadsBeforeInterpretation);
}

TEST(PdfPreparedContentResources, RejectsMaterializationFromDifferentFontIdentity) {
  std::array<PdfFontWidthRecord, 1> sourceWidths{};
  PdfFontMap sourceFont({sourceWidths.data(), static_cast<uint16_t>(sourceWidths.size())});
  ASSERT_TRUE(sourceFont.begin(21, false, nullptr, nullptr, 500).ok());
  ASSERT_TRUE(sourceFont.addWidth(0, 255, 500).ok());

  std::array<PdfDecodedGlyph, 1> pageGlyphs{};
  PdfFontMapWorkspace pageFontWorkspace{};
  pageFontWorkspace.materializedGlyphs = pageGlyphs.data();
  pageFontWorkspace.materializedGlyphCapacity = static_cast<uint16_t>(pageGlyphs.size());
  PdfFontMap pageFont(pageFontWorkspace);
  ASSERT_TRUE(pageFont.beginMaterialized(22, false).ok());
  static constexpr uint8_t encoded[]{'A'};

  EXPECT_EQ(pageFont.materializeString(sourceFont, encoded, sizeof(encoded)).error, PdfError::InvalidArgument);
  EXPECT_EQ(pageFont.materializedGlyphCount(), 0U);
}

TEST(PdfPreparedContentResources, RejectsMoreThanGlobalPageGlyphBudgetAcrossFontSlices) {
  std::array<uint8_t, 95> encoded{};
  for (uint8_t index = 0; index < encoded.size(); ++index) {
    encoded[index] = static_cast<uint8_t>(0x20U + index);
  }

  std::array<PdfFontWidthRecord, 1> sourceWidths1{};
  std::array<PdfFontWidthRecord, 1> sourceWidths2{};
  std::array<PdfFontWidthRecord, 1> sourceWidths3{};
  PdfFontMap sourceFont1({sourceWidths1.data(), static_cast<uint16_t>(sourceWidths1.size())});
  PdfFontMap sourceFont2({sourceWidths2.data(), static_cast<uint16_t>(sourceWidths2.size())});
  PdfFontMap sourceFont3({sourceWidths3.data(), static_cast<uint16_t>(sourceWidths3.size())});
  ASSERT_TRUE(sourceFont1.begin(31, false, nullptr, nullptr, 500).ok());
  ASSERT_TRUE(sourceFont2.begin(32, false, nullptr, nullptr, 500).ok());
  ASSERT_TRUE(sourceFont3.begin(33, false, nullptr, nullptr, 500).ok());
  ASSERT_TRUE(sourceFont1.addWidth(0, 255, 500).ok());
  ASSERT_TRUE(sourceFont2.addWidth(0, 255, 500).ok());
  ASSERT_TRUE(sourceFont3.addWidth(0, 255, 500).ok());

  std::array<PdfDecodedGlyph, 95> glyphs1{};
  std::array<PdfDecodedGlyph, 95> glyphs2{};
  std::array<PdfDecodedGlyph, 95> glyphs3{};
  PdfFontMapWorkspace pageWorkspace1{};
  PdfFontMapWorkspace pageWorkspace2{};
  PdfFontMapWorkspace pageWorkspace3{};
  pageWorkspace1.materializedGlyphs = glyphs1.data();
  pageWorkspace1.materializedGlyphCapacity = static_cast<uint16_t>(glyphs1.size());
  pageWorkspace2.materializedGlyphs = glyphs2.data();
  pageWorkspace2.materializedGlyphCapacity = static_cast<uint16_t>(glyphs2.size());
  pageWorkspace3.materializedGlyphs = glyphs3.data();
  pageWorkspace3.materializedGlyphCapacity = static_cast<uint16_t>(glyphs3.size());
  PdfFontMap pageFont1(pageWorkspace1);
  PdfFontMap pageFont2(pageWorkspace2);
  PdfFontMap pageFont3(pageWorkspace3);
  ASSERT_TRUE(pageFont1.beginMaterialized(sourceFont1.fontId(), sourceFont1.cid()).ok());
  ASSERT_TRUE(pageFont2.beginMaterialized(sourceFont2.fontId(), sourceFont2.cid()).ok());
  ASSERT_TRUE(pageFont3.beginMaterialized(sourceFont3.fontId(), sourceFont3.cid()).ok());
  ASSERT_TRUE(pageFont1.materializeString(sourceFont1, encoded.data(), encoded.size()).ok());
  ASSERT_TRUE(pageFont2.materializeString(sourceFont2, encoded.data(), encoded.size()).ok());
  ASSERT_TRUE(pageFont3.materializeString(sourceFont3, encoded.data(), encoded.size()).ok());

  std::array<PdfPreparedFontResource, 3> fonts{};
  std::array<PdfPreparedXObjectResource, 1> xobjects{};
  PdfPreparedContentResources resources({fonts.data(), fonts.size(), xobjects.data(), xobjects.size()});
  ASSERT_TRUE(resources.reset().ok());
  ASSERT_TRUE(resources.addFont(reinterpret_cast<const uint8_t*>("F1"), 2, &pageFont1).ok());
  ASSERT_TRUE(resources.addFont(reinterpret_cast<const uint8_t*>("F2"), 2, &pageFont2).ok());

  EXPECT_EQ(resources.addFont(reinterpret_cast<const uint8_t*>("F3"), 2, &pageFont3).error,
            PdfError::LimitExceeded);
  EXPECT_EQ(resources.fontCount(), 2U);
}

}  // namespace
