#include <gtest/gtest.h>

#include <array>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "PdfCheckedMath.h"
#include "PdfReadingOrder.h"
#include "PdfRunStore.h"
#include "PdfTestIo.h"

namespace {

constexpr int32_t fx(const int16_t value) { return PdfFixed16::fromInteger(value).raw; }
constexpr PdfRectangle PAGE{fx(0), fx(0), fx(612), fx(792)};

PdfTextRun makeRun(const int x, const int y, const int width, const int height, const uint32_t sourceOrder,
                   const int dx = -1, const int dy = 0) {
  PdfTextRun run{};
  run.sourceOrder = sourceOrder;
  run.xMin = fx(static_cast<int16_t>(x));
  run.yMin = fx(static_cast<int16_t>(y));
  run.xMax = fx(static_cast<int16_t>(x + width));
  run.yMax = fx(static_cast<int16_t>(y + height));
  run.baselineX = run.xMin;
  run.baseline = run.yMin;
  run.baselineDx = fx(static_cast<int16_t>(dx < 0 ? width : dx));
  run.baselineDy = fx(static_cast<int16_t>(dy));
  return run;
}

struct SourceLifecycle {
  bool sourceOpen = true;
  bool spillReading = false;
  uint32_t closes = 0;
  uint32_t reopens = 0;
  uint64_t continuation = 0;

  static PdfStatus closeSource(void* context, const uint64_t continuation) {
    auto& state = *static_cast<SourceLifecycle*>(context);
    if (!state.sourceOpen || state.spillReading) {
      return PdfStatus::failure(PdfError::IoFailure);
    }
    state.sourceOpen = false;
    state.spillReading = true;
    state.continuation = continuation;
    ++state.closes;
    return PdfStatus::success();
  }

  static PdfStatus reopenSource(void* context, const uint64_t continuation) {
    auto& state = *static_cast<SourceLifecycle*>(context);
    if (state.sourceOpen || !state.spillReading || continuation != state.continuation) {
      return PdfStatus::failure(PdfError::IoFailure);
    }
    state.spillReading = false;
    state.sourceOpen = true;
    ++state.reopens;
    return PdfStatus::success();
  }

  PdfRunStoreLifecycle lifecycle() { return {this, closeSource, reopenSource}; }
};

struct RunHarness {
  std::vector<PdfTextRun> memoryRuns;
  std::vector<uint8_t> memoryText;
  PdfTestRecordStore spillRuns;
  PdfTestByteStore spillText;
  SourceLifecycle lifecycle;
  PdfRunStore store;

  RunHarness(const size_t runCapacity, const size_t textCapacity, const uint32_t spillRunCapacity = 1024,
             const uint64_t spillTextCapacity = 256 * 1024)
      : memoryRuns(runCapacity),
        memoryText(textCapacity),
        spillRuns(sizeof(PdfTextRun), spillRunCapacity),
        spillText(spillTextCapacity),
        store({memoryRuns.data(), static_cast<uint16_t>(memoryRuns.size()), memoryText.data(), memoryText.size(),
               spillRuns.store(), spillText.store()},
              lifecycle.lifecycle()) {
    EXPECT_TRUE(store.reset().ok());
  }

  void add(const std::string& text, const int x, const int y, const int width, const int height,
           const uint32_t sourceOrder, const int dx = -1, const int dy = 0) {
    ASSERT_TRUE(store
                    .append(makeRun(x, y, width, height, sourceOrder, dx, dy),
                            reinterpret_cast<const uint8_t*>(text.data()), text.size())
                    .ok());
  }
};

struct TranscriptSink {
  PdfRunStore* store = nullptr;
  std::string transcript;
  std::vector<PdfReadingOrderItem> items;

  static PdfStatus emit(void* context, const PdfReadingOrderItem& item) {
    auto& sink = *static_cast<TranscriptSink*>(context);
    PdfTextRun run{};
    PdfStatus status = sink.store->readRun(item.runOrdinal, &run);
    if (!status.ok()) {
      return status;
    }
    std::string text(run.textLength, '\0');
    status = sink.store->readTextExact(item.runOrdinal, 0, reinterpret_cast<uint8_t*>(text.data()), text.size());
    if (!status.ok()) {
      return status;
    }
    if (!sink.transcript.empty()) {
      sink.transcript.push_back(' ');
    }
    sink.transcript += text;
    sink.items.push_back(item);
    return PdfStatus::success();
  }

  PdfReadingOrderSink sink() { return {this, emit}; }
};

std::string reduce(RunHarness& runs, PdfRepeatedBandTracker* bands = nullptr, const uint32_t pageOrdinal = 0,
                   const uint64_t continuation = 321) {
  std::array<PdfReadingOrderItem, PdfLimits::PageRunCount> order{};
  PdfReadingOrderReducer reducer({order.data(), static_cast<uint16_t>(order.size())});
  TranscriptSink transcript{&runs.store, {}, {}};
  uint32_t emitted = 0;
  EXPECT_TRUE(reducer.reduce(runs.store, PAGE, pageOrdinal, bands, continuation, transcript.sink(), &emitted).ok());
  EXPECT_EQ(emitted, transcript.items.size());
  return transcript.transcript;
}

void observeBand(PdfRepeatedBandTracker& tracker, const uint32_t page, const PdfTextRun& run, const std::string& text) {
  ASSERT_TRUE(tracker.observe(page, PAGE, run, reinterpret_cast<const uint8_t*>(text.data()), text.size()).ok());
}

class DiskRecordStore {
 public:
  DiskRecordStore(const std::filesystem::path& path, const size_t recordSize, const uint32_t capacity)
      : path_(path), recordSize_(recordSize), capacity_(capacity) {
    stream_.open(path_, std::ios::binary | std::ios::in | std::ios::out | std::ios::trunc);
  }
  ~DiskRecordStore() {
    stream_.close();
    std::error_code ignored;
    std::filesystem::remove(path_, ignored);
  }
  PdfFixedRecordStore store() { return {this, capacity_, recordSize_, read, write}; }

 private:
  static PdfStatus read(void* context, const uint32_t ordinal, void* output, const size_t size) {
    auto& self = *static_cast<DiskRecordStore*>(context);
    if (output == nullptr || size != self.recordSize_ || ordinal >= self.capacity_) {
      return PdfStatus::failure(PdfError::InvalidOffset, ordinal);
    }
    self.stream_.clear();
    self.stream_.seekg(static_cast<std::streamoff>(ordinal) * static_cast<std::streamoff>(size));
    self.stream_.read(static_cast<char*>(output), static_cast<std::streamsize>(size));
    return self.stream_.gcount() == static_cast<std::streamsize>(size)
               ? PdfStatus::success()
               : PdfStatus::failure(PdfError::UnexpectedEof, ordinal);
  }
  static PdfStatus write(void* context, const uint32_t ordinal, const void* input, const size_t size) {
    auto& self = *static_cast<DiskRecordStore*>(context);
    if (input == nullptr || size != self.recordSize_ || ordinal >= self.capacity_) {
      return PdfStatus::failure(PdfError::InvalidOffset, ordinal);
    }
    self.stream_.clear();
    self.stream_.seekp(static_cast<std::streamoff>(ordinal) * static_cast<std::streamoff>(size));
    self.stream_.write(static_cast<const char*>(input), static_cast<std::streamsize>(size));
    self.stream_.flush();
    return self.stream_ ? PdfStatus::success() : PdfStatus::failure(PdfError::IoFailure, ordinal);
  }

  std::filesystem::path path_;
  size_t recordSize_;
  uint32_t capacity_;
  std::fstream stream_;
};

class DiskByteStore {
 public:
  DiskByteStore(const std::filesystem::path& path, const uint64_t capacity) : path_(path), capacity_(capacity) {
    stream_.open(path_, std::ios::binary | std::ios::in | std::ios::out | std::ios::trunc);
  }
  ~DiskByteStore() {
    stream_.close();
    std::error_code ignored;
    std::filesystem::remove(path_, ignored);
  }
  PdfByteStore store() { return {this, capacity_, reset, size, readAt, write}; }

 private:
  static PdfStatus reset(void* context) {
    auto& self = *static_cast<DiskByteStore*>(context);
    self.stream_.close();
    self.stream_.open(self.path_, std::ios::binary | std::ios::in | std::ios::out | std::ios::trunc);
    self.size_ = 0;
    return self.stream_ ? PdfStatus::success() : PdfStatus::failure(PdfError::IoFailure);
  }
  static uint64_t size(void* context) { return static_cast<DiskByteStore*>(context)->size_; }
  static PdfStatus readAt(void* context, const uint64_t offset, uint8_t* output, const size_t requested,
                          size_t* bytesRead) {
    auto& self = *static_cast<DiskByteStore*>(context);
    if (output == nullptr || bytesRead == nullptr || offset > self.size_) {
      return PdfStatus::failure(PdfError::InvalidOffset, offset);
    }
    const size_t count = static_cast<size_t>(std::min<uint64_t>(requested, self.size_ - offset));
    self.stream_.clear();
    self.stream_.seekg(static_cast<std::streamoff>(offset));
    self.stream_.read(reinterpret_cast<char*>(output), static_cast<std::streamsize>(count));
    *bytesRead = static_cast<size_t>(self.stream_.gcount());
    return *bytesRead == count ? PdfStatus::success() : PdfStatus::failure(PdfError::UnexpectedEof, offset);
  }
  static PdfStatus write(void* context, const uint8_t* input, const size_t requested, size_t* bytesWritten) {
    auto& self = *static_cast<DiskByteStore*>(context);
    if (input == nullptr || bytesWritten == nullptr || requested > self.capacity_ - self.size_) {
      return PdfStatus::failure(PdfError::InsufficientStorage, self.size_);
    }
    self.stream_.clear();
    self.stream_.seekp(static_cast<std::streamoff>(self.size_));
    self.stream_.write(reinterpret_cast<const char*>(input), static_cast<std::streamsize>(requested));
    self.stream_.flush();
    if (!self.stream_) {
      return PdfStatus::failure(PdfError::IoFailure, self.size_);
    }
    self.size_ += requested;
    *bytesWritten = requested;
    return PdfStatus::success();
  }

  std::filesystem::path path_;
  uint64_t capacity_;
  uint64_t size_ = 0;
  std::fstream stream_;
};

}  // namespace

TEST(PdfReadingOrderTest, OrdersSingleColumnLinesTopToBottomWithSourceTieBreak) {
  RunHarness runs(16, 1024);
  runs.add("third", 72, 600, 40, 12, 2);
  runs.add("first", 72, 720, 40, 12, 0);
  runs.add("second", 72, 660, 50, 12, 1);
  EXPECT_EQ(reduce(runs), "first second third");
}

TEST(PdfReadingOrderTest, OrdersTwoAndThreeColumnsWithSpanningHeadings) {
  RunHarness two(16, 2048);
  two.add("Right one.", 360, 700, 90, 12, 1);
  two.add("Heading", 72, 760, 430, 16, 0);
  two.add("Left two.", 72, 670, 80, 12, 3);
  two.add("Right two.", 360, 670, 90, 12, 4);
  two.add("Left one.", 72, 700, 80, 12, 2);
  EXPECT_EQ(reduce(two), "Heading Left one. Left two. Right one. Right two.");

  RunHarness three(16, 2048);
  three.add("C2a", 250, 700, 40, 12, 1);
  three.add("C3b", 430, 670, 40, 12, 5);
  three.add("C1b", 70, 670, 40, 12, 3);
  three.add("C3a", 430, 700, 40, 12, 2);
  three.add("C1a", 70, 700, 40, 12, 0);
  three.add("C2b", 250, 670, 40, 12, 4);
  EXPECT_EQ(reduce(three), "C1a C1b C2a C2b C3a C3b");
}

TEST(PdfReadingOrderTest, KeepsShortAlignedTableCellsInRowMajorOrderAfterColumns) {
  RunHarness runs(32, 4096);
  runs.add("Left one.", 72, 720, 75, 12, 0);
  runs.add("Right one.", 360, 720, 80, 12, 1);
  runs.add("Left two.", 72, 696, 75, 12, 2);
  runs.add("Right two.", 360, 696, 80, 12, 3);
  runs.add("Name", 72, 636, 35, 12, 4);
  runs.add("Value", 212, 636, 40, 12, 5);
  runs.add("Alpha", 72, 616, 40, 12, 6);
  runs.add("10", 212, 616, 18, 12, 7);
  EXPECT_EQ(reduce(runs), "Left one. Left two. Right one. Right two. Name Value Alpha 10");
}

TEST(PdfReadingOrderTest, DropsRotatedMarginNoiseAndFallsBackDeterministicallyForAmbiguity) {
  RunHarness runs(16, 1024);
  runs.add("body one", 100, 700, 80, 12, 3);
  runs.add("rotated", 20, 300, 12, 80, 0, 0, 80);
  runs.add("body two", 105, 670, 80, 12, 2);
  runs.add("wide bridge", 80, 640, 430, 12, 1);
  EXPECT_EQ(reduce(runs), "body one body two wide bridge");
}

TEST(PdfReadingOrderTest, SuppressesOnlyBandsRepeatedAcrossThreeDistinctPages) {
  PdfRepeatedBandTracker bands;
  const PdfTextRun header = makeRun(72, 760, 120, 10, 0);
  const PdfTextRun footer = makeRun(72, 50, 120, 10, 2);
  const PdfTextRun heading = makeRun(72, 730, 180, 16, 1);
  for (uint32_t page = 0; page < 3; ++page) {
    observeBand(bands, page, header, "Repeated Header");
    if (page == 0) {
      observeBand(bands, page, heading, "One-off heading");
    }
    observeBand(bands, page, footer, "Repeated Footer");
  }
  EXPECT_TRUE(bands.hasRepeatedBands());

  RunHarness runs(8, 1024);
  runs.add("Repeated Header", 72, 760, 120, 10, 0);
  runs.add("One-off heading", 72, 730, 180, 16, 1);
  runs.add("First body.", 72, 650, 100, 12, 2);
  runs.add("Repeated Footer", 72, 50, 120, 10, 3);
  EXPECT_EQ(reduce(runs, &bands), "One-off heading First body.");

  PdfRepeatedBandTracker outOfOrder;
  observeBand(outOfOrder, 2, header, "Header");
  EXPECT_EQ(
      outOfOrder.observe(1, PAGE, header, reinterpret_cast<const uint8_t*>("Header"), std::strlen("Header")).error,
      PdfError::InvalidArgument);
}

TEST(PdfReadingOrderTest, GroupsContinuationLinesAndStartsParagraphAfterTerminalPunctuation) {
  RunHarness runs(8, 1024);
  runs.add("A sentence continues", 72, 720, 140, 12, 0);
  runs.add("with more words.", 72, 700, 120, 12, 1);
  runs.add("Next paragraph", 72, 680, 110, 12, 2);

  std::array<PdfReadingOrderItem, PdfLimits::PageRunCount> order{};
  PdfReadingOrderReducer reducer({order.data(), static_cast<uint16_t>(order.size())});
  TranscriptSink transcript{&runs.store, {}, {}};
  uint32_t emitted = 0;
  ASSERT_TRUE(reducer.reduce(runs.store, PAGE, 0, nullptr, 0, transcript.sink(), &emitted).ok());
  ASSERT_EQ(transcript.items.size(), 3u);
  EXPECT_NE(transcript.items[0].flags & PdfOrderNewBlock, 0);
  EXPECT_EQ(transcript.items[1].flags & PdfOrderNewBlock, 0);
  EXPECT_NE(transcript.items[2].flags & PdfOrderNewBlock, 0);
}

TEST(PdfReadingOrderTest, RealDiskSpillMatchesRoomyPathWithoutTruncatingDenseText) {
  constexpr uint32_t RUNS = 300;
  std::vector<std::string> fragments;
  fragments.reserve(RUNS);
  for (uint32_t index = 0; index < RUNS; ++index) {
    char text[48];
    std::snprintf(text, sizeof(text), "Run%03u bounded-spill-payload-%03u.", index, index);
    fragments.emplace_back(text);
  }

  RunHarness roomy(512, 32 * 1024);
  for (uint32_t index = 0; index < RUNS; ++index) {
    roomy.add(fragments[index], 40 + static_cast<int>(index % 3) * 180, 760 - static_cast<int>(index / 3) * 7, 150, 8,
              index);
  }
  const std::string expected = reduce(roomy);

  const std::filesystem::path base = std::filesystem::temp_directory_path() / "crossink-pdf-run-store";
  DiskRecordStore diskRuns(base.string() + ".records", sizeof(PdfTextRun), RUNS);
  DiskByteStore diskText(base.string() + ".text", 64 * 1024);
  std::array<PdfTextRun, PdfLimits::PageRunCount> memoryRuns{};
  std::array<uint8_t, PdfLimits::PageTextBytes> memoryText{};
  SourceLifecycle lifecycle;
  PdfRunStore spilled({memoryRuns.data(), static_cast<uint16_t>(memoryRuns.size()), memoryText.data(),
                       memoryText.size(), diskRuns.store(), diskText.store()},
                      lifecycle.lifecycle());
  ASSERT_TRUE(spilled.reset().ok());
  for (uint32_t index = 0; index < RUNS; ++index) {
    ASSERT_TRUE(spilled
                    .append(makeRun(40 + static_cast<int>(index % 3) * 180, 760 - static_cast<int>(index / 3) * 7, 150,
                                    8, index),
                            reinterpret_cast<const uint8_t*>(fragments[index].data()), fragments[index].size())
                    .ok());
  }
  ASSERT_TRUE(spilled.spilled());
  PdfTextRun witness{};
  EXPECT_EQ(spilled.readRun(PdfLimits::PageRunCount, &witness).error, PdfError::IoFailure);

  std::array<PdfReadingOrderItem, PdfLimits::PageRunCount> order{};
  PdfReadingOrderReducer reducer({order.data(), static_cast<uint16_t>(order.size())});
  TranscriptSink transcript{&spilled, {}, {}};
  uint32_t emitted = 0;
  ASSERT_TRUE(reducer.reduce(spilled, PAGE, 0, nullptr, 987654, transcript.sink(), &emitted).ok());
  EXPECT_EQ(transcript.transcript, expected);
  EXPECT_GT(transcript.transcript.size(), PdfLimits::PageTextBytes);
  EXPECT_EQ(emitted, RUNS);
  EXPECT_EQ(lifecycle.closes, 1u);
  EXPECT_EQ(lifecycle.reopens, 1u);
  EXPECT_TRUE(lifecycle.sourceOpen);
  EXPECT_FALSE(lifecycle.spillReading);
  EXPECT_EQ(lifecycle.continuation, 987654u);
}

TEST(PdfReadingOrderTest, SpillReadsOnlyAfterSourceCloses) {
  RunHarness runs(1, 4);
  runs.add("first", 72, 700, 40, 12, 0);
  runs.add("second", 72, 670, 45, 12, 1);
  runs.spillRuns.forbidReadsWhile(&runs.lifecycle.sourceOpen);
  runs.spillText.forbidReadsWhile(&runs.lifecycle.sourceOpen);

  ASSERT_TRUE(runs.store.beginReduction(42).ok());
  PdfTextRun second{};
  EXPECT_TRUE(runs.store.readRun(1, &second).ok());
  ASSERT_TRUE(runs.store.endReduction().ok());
  EXPECT_EQ(runs.lifecycle.closes, 1u);
  EXPECT_EQ(runs.lifecycle.reopens, 1u);
}

TEST(PdfReadingOrderTest, SpillReadFaultStillReopensSourceExactlyOnce) {
  RunHarness runs(1, 4);
  runs.add("one", 72, 700, 30, 12, 0);
  runs.add("second", 72, 670, 45, 12, 1);
  runs.spillRuns.setReadFailureOrdinal(0);

  std::array<PdfReadingOrderItem, PdfLimits::PageRunCount> order{};
  PdfReadingOrderReducer reducer({order.data(), static_cast<uint16_t>(order.size())});
  TranscriptSink transcript{&runs.store, {}, {}};
  uint32_t emitted = 0;
  const PdfStatus status = reducer.reduce(runs.store, PAGE, 0, nullptr, 44, transcript.sink(), &emitted);
  EXPECT_EQ(status.error, PdfError::IoFailure);
  EXPECT_EQ(runs.lifecycle.closes, 1u);
  EXPECT_EQ(runs.lifecycle.reopens, 1u);
  EXPECT_TRUE(runs.lifecycle.sourceOpen);
}

TEST(PdfReadingOrderTest, CapacityFailureDoesNotCommitRunOrEnterSpillMode) {
  std::array<PdfTextRun, 1> memoryRuns{};
  std::array<uint8_t, 4> memoryText{};
  PdfRunStore runs(
      {memoryRuns.data(), static_cast<uint16_t>(memoryRuns.size()), memoryText.data(), memoryText.size(), {}, {}});
  ASSERT_TRUE(runs.reset().ok());
  const PdfTextRun run = makeRun(72, 700, 40, 12, 0);

  EXPECT_EQ(runs.append(run, reinterpret_cast<const uint8_t*>("12345"), 5).error, PdfError::InsufficientStorage);
  EXPECT_EQ(runs.count(), 0u);
  EXPECT_EQ(runs.textLength(), 0u);
  EXPECT_FALSE(runs.spilled());
  EXPECT_TRUE(runs.append(run, reinterpret_cast<const uint8_t*>("ok"), 2).ok());
  EXPECT_EQ(runs.count(), 1u);
}
