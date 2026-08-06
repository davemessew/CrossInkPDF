#include <gtest/gtest-spi.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

#include "PdfCacheFormat.h"
#include "PdfImageCache.h"
#include "PdfImagePreparation.h"
#include "PdfPreparation.h"
#include "PdfReflowDocument.h"
#include "PdfSemanticWriter.h"
#include "PdfTestCacheIo.h"
#include "PdfTestIo.h"
#include "Print.h"

namespace {

struct SemanticHarness {
  std::vector<uint8_t> output;
  std::vector<PdfSemanticBlockRecord> blocks;
  uint8_t workspace[PdfSemanticWriterLimits::MinimumOutputBufferBytes]{};

  static PdfStatus write(void* context, const uint8_t* source, const size_t requested, size_t* bytesWritten) {
    if (context == nullptr || source == nullptr || bytesWritten == nullptr) {
      return PdfStatus::failure(PdfError::InvalidArgument);
    }
    auto& self = *static_cast<SemanticHarness*>(context);
    self.output.insert(self.output.end(), source, source + requested);
    *bytesWritten = requested;
    return PdfStatus::success();
  }

  static PdfStatus emit(void* context, const PdfSemanticBlockRecord& record) {
    if (context == nullptr) {
      return PdfStatus::failure(PdfError::InvalidArgument);
    }
    static_cast<SemanticHarness*>(context)->blocks.push_back(record);
    return PdfStatus::success();
  }
};

struct PreparationHarness {
  PdfTestCacheIo storage;
  PdfResourceSnapshot resources{128U * 1024U, 96U * 1024U, 8U * 1024U};
  uint32_t nowMs = 0;
  bool chargeIoTime = false;

  uint32_t ioOperations() const { return storage.operationCalls(); }

  uint32_t clockMs() const { return nowMs + (chargeIoTime ? ioOperations() : 0); }

  static uint32_t now(void* context) { return static_cast<PreparationHarness*>(context)->clockMs(); }
  static PdfResourceSnapshot measure(void* context) { return static_cast<PreparationHarness*>(context)->resources; }

  PdfPreparationConfig config(const char* sourcePath = "/books/jpeg-caption.pdf") {
    return {
        storage.io(), sourcePath, "/.crosspoint", this, now, {this, measure, nullptr}, storage.renameCallback(),
        800,          480,
    };
  }
};

class BufferPrint final : public Print {
 public:
  size_t write(const uint8_t* const bytes, const size_t length) override {
    output.insert(output.end(), bytes, bytes + length);
    return length;
  }

  std::vector<uint8_t> output;
};

struct CountingByteSource {
  explicit CountingByteSource(const std::vector<uint8_t>& value) : bytes(value) {}

  PdfByteSource source() { return {this, bytes.size(), readAt}; }

  static PdfStatus readAt(void* context, uint64_t offset, uint8_t* destination, size_t requested, size_t* bytesRead) {
    if (context == nullptr || destination == nullptr || bytesRead == nullptr) {
      return PdfStatus::failure(PdfError::InvalidArgument, offset);
    }
    auto& self = *static_cast<CountingByteSource*>(context);
    ++self.readCalls;
    self.readOffsets.push_back(offset);
    if (offset > self.bytes.size()) {
      return PdfStatus::failure(PdfError::InvalidOffset, offset);
    }
    *bytesRead = std::min(requested, self.bytes.size() - static_cast<size_t>(offset));
    self.readSizes.push_back(*bytesRead);
    self.totalBytesRead += *bytesRead;
    if (*bytesRead != 0) {
      std::memcpy(destination, self.bytes.data() + offset, *bytesRead);
    }
    return PdfStatus::success();
  }

  const std::vector<uint8_t>& bytes;
  uint32_t readCalls = 0;
  uint64_t totalBytesRead = 0;
  std::vector<uint64_t> readOffsets;
  std::vector<size_t> readSizes;
};

std::vector<uint8_t> largeSyntheticJpegBytes() {
  std::vector<uint8_t> jpeg = {
      0xff, 0xd8, 0xff, 0xe0, 0x00, 0x10, 'J', 'F', 'I', 'F', 0x00, 0x01,
  };
  jpeg.reserve(jpeg.size() + (20U * 1024U) + 2U);
  uint32_t state = 0xA341316CU;
  for (size_t index = 0; index < 20U * 1024U; ++index) {
    state ^= state << 13U;
    state ^= state >> 17U;
    state ^= state << 5U;
    jpeg.push_back(static_cast<uint8_t>(state));
  }
  jpeg.push_back(0xff);
  jpeg.push_back(0xd9);
  return jpeg;
}

PdfStatus cacheJpegToTerminal(PdfImageCache& cache, const PdfByteSource& source, const uint16_t width,
                              const uint16_t height, PdfCachedImage* result) {
  PdfImageCacheRuntime runtime{};
  PdfStatus status = cache.beginJpeg(source, width, height, result, &runtime);
  if (!status) {
    return status;
  }
  for (uint16_t step = 0; step < 10000; ++step) {
    PdfWorkBudget budget{32, PdfLimits::SourceBufferBytes};
    const PdfStepResult outcome = cache.stepJpeg(runtime, budget);
    if (outcome.failed()) {
      return outcome.status;
    }
    if (outcome.complete()) {
      return PdfStatus::success();
    }
  }
  cache.abortJpeg(runtime);
  return PdfStatus::failure(PdfError::BudgetExhausted);
}

PdfStatus captureJpeg(PdfImageCache& cache, const std::vector<uint8_t>& bytes, const uint8_t temporaryOrdinal,
                      PdfCapturedJpeg* captured) {
  PdfImageCacheRuntime runtime{};
  PdfStatus status = cache.beginJpegCapture(temporaryOrdinal, bytes.size(), &runtime);
  for (size_t offset = 0; status && offset < bytes.size();) {
    const size_t length = std::min<size_t>(PdfLimits::SourceBufferBytes, bytes.size() - offset);
    status = cache.appendJpegCapture(bytes.data() + offset, length, runtime);
    offset += length;
  }
  if (status) {
    status = cache.finishJpegCapture(runtime, captured);
  }
  if (!status) {
    cache.abortJpeg(runtime);
  }
  return status;
}

PdfStatus publishCapturedJpeg(PdfImageCache& cache, const PdfCapturedJpeg& captured, const uint16_t width,
                              const uint16_t height, PdfCachedImage* result) {
  PdfImageCacheRuntime runtime{};
  PdfStatus status = cache.beginCapturedJpeg(captured, width, height, result, &runtime);
  if (!status) {
    return status;
  }
  for (uint16_t step = 0; step < 10000; ++step) {
    PdfWorkBudget budget{32, PdfLimits::SourceBufferBytes};
    const PdfStepResult outcome = cache.stepJpeg(runtime, budget);
    if (outcome.failed()) {
      return outcome.status;
    }
    if (outcome.complete()) {
      return PdfStatus::success();
    }
  }
  cache.abortJpeg(runtime);
  return PdfStatus::failure(PdfError::BudgetExhausted);
}

PdfStatus writeTwoByTwoMaskSpool(PdfTestCacheIo& storage, const char* path, PdfMaskSpool& spool) {
  PdfStatus status = spool.beginWrite(storage.io(), path);
  PdfByteSink baseSink{};
  if (status) {
    status = spool.beginRecord(42, 7, 2, 2, &baseSink);
  }
  const uint8_t base[] = {2, 0, 2, 0, 0x10, 0xB0};
  size_t written = 0;
  if (status) {
    status = baseSink.write(baseSink.context, base, sizeof(base), &written);
    if (status && written != sizeof(base)) {
      status = PdfStatus::failure(PdfError::IoFailure, written);
    }
  }
  uint8_t maskRows[4]{};
  uint8_t alphaRows[2]{};
  PdfMaskPlaneWriter plane;
  PdfMaskPlaneConfig config{};
  config.sourceWidth = 2;
  config.sourceHeight = 2;
  config.outputWidth = 2;
  config.outputHeight = 2;
  config.bitsPerComponent = 8;
  config.rowWorkspace = maskRows;
  config.rowWorkspaceBytes = sizeof(maskRows);
  config.outputWorkspace = alphaRows;
  config.outputWorkspaceBytes = sizeof(alphaRows);
  if (status) {
    status = spool.beginAlpha(config, &plane);
  }
  const uint8_t alpha[] = {0xFF, 0x80, 0x00, 0xFF};
  PdfByteSink alphaSink = plane.decodedSink();
  if (status) {
    status = alphaSink.write(alphaSink.context, alpha, sizeof(alpha), &written);
    if (status && written != sizeof(alpha)) {
      status = PdfStatus::failure(PdfError::IoFailure, written);
    }
  }
  if (status) {
    status = plane.finish();
  }
  if (status) {
    status = spool.finishRecord();
  }
  return status ? spool.closeWrite() : status;
}

PdfStatus openMaskSpoolToTerminal(PdfMaskSpool& spool, PdfTestCacheIo& storage, const char* path, uint8_t* workspace,
                                  const size_t workspaceBytes) {
  PdfMaskSpoolReadRuntime runtime{};
  PdfStatus status = spool.beginRead(storage.io(), path, workspace, workspaceBytes, &runtime);
  if (!status) {
    return status;
  }
  for (uint16_t step = 0; step < 10000; ++step) {
    PdfWorkBudget budget{32, PdfLimits::SourceBufferBytes};
    const PdfStepResult outcome = spool.stepReadOpen(runtime, budget);
    if (outcome.failed()) {
      return outcome.status;
    }
    if (outcome.complete()) {
      return PdfStatus::success();
    }
  }
  spool.abort();
  return PdfStatus::failure(PdfError::BudgetExhausted);
}

std::vector<uint8_t> loadFixture(const char* name) {
  const std::filesystem::path path =
      std::filesystem::path(__FILE__).parent_path().parent_path() / "pdf_reflow_core" / "fixtures" / name;
  std::ifstream input(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

void writeLittleEndian32(std::vector<uint8_t>* const bytes, const size_t offset, const uint32_t value) {
  ASSERT_NE(bytes, nullptr);
  ASSERT_LE(offset + sizeof(value), bytes->size());
  (*bytes)[offset] = static_cast<uint8_t>(value);
  (*bytes)[offset + 1U] = static_cast<uint8_t>(value >> 8U);
  (*bytes)[offset + 2U] = static_cast<uint8_t>(value >> 16U);
  (*bytes)[offset + 3U] = static_cast<uint8_t>(value >> 24U);
}

uint32_t readLittleEndian32(const std::vector<uint8_t>& bytes, const size_t offset) {
  EXPECT_LE(offset + sizeof(uint32_t), bytes.size());
  return static_cast<uint32_t>(bytes[offset]) | static_cast<uint32_t>(bytes[offset + 1U]) << 8U |
         static_cast<uint32_t>(bytes[offset + 2U]) << 16U | static_cast<uint32_t>(bytes[offset + 3U]) << 24U;
}

void resealTrailingCrc(std::vector<uint8_t>* const bytes) {
  ASSERT_NE(bytes, nullptr);
  ASSERT_GE(bytes->size(), sizeof(uint32_t));
  writeLittleEndian32(bytes, bytes->size() - sizeof(uint32_t),
                      pdfCacheCrc32(bytes->data(), bytes->size() - sizeof(uint32_t)));
}

PdfStepResult runToTerminal(PdfPreparation& preparation, PreparationHarness& harness) {
  for (uint32_t step = 0; step < 20000; ++step) {
    const PdfStepResult result = preparation.step();
    ++harness.nowMs;
    if (!result.yielded()) {
      return result;
    }
  }
  return PdfStepResult::failure(PdfStatus::failure(PdfError::BudgetExhausted));
}

struct PreparationStepObservation {
  PdfStepResult result{};
  uint32_t elapsedMs = 0;
  uint32_t operations = 0;
  uint32_t openCalls = 0;
  uint32_t readCalls = 0;
  uint32_t writeCalls = 0;
  uint32_t flushCalls = 0;
  uint32_t syncCalls = 0;
  uint32_t closeCalls = 0;
  uint32_t removeCalls = 0;
  uint32_t renameCalls = 0;
  uint32_t mkdirCalls = 0;
  uint32_t listCalls = 0;
  uint32_t capacityCalls = 0;
  uint32_t metadataCalls = 0;
  std::string events;
  uint64_t bytesRead = 0;
  uint64_t bytesWritten = 0;
};

PreparationStepObservation observePreparationStep(PdfPreparation& preparation, PreparationHarness& harness) {
  const uint32_t startedAt = harness.clockMs();
  const auto wallStartedAt = std::chrono::steady_clock::now();
  const uint32_t operationsBefore = harness.ioOperations();
  const uint32_t openBefore = harness.storage.openCalls();
  const uint32_t readBefore = harness.storage.readCalls();
  const uint32_t writeBefore = harness.storage.writeCalls();
  const uint32_t flushBefore = harness.storage.flushCalls();
  const uint32_t syncBefore = harness.storage.syncCalls();
  const uint32_t closeBefore = harness.storage.closeCalls();
  const uint32_t removeBefore = harness.storage.removeCalls();
  const uint32_t renameBefore = harness.storage.renameCalls();
  const uint32_t mkdirBefore = harness.storage.mkdirCalls();
  const uint32_t listBefore = harness.storage.listCalls();
  const uint32_t capacityBefore = harness.storage.capacityCalls();
  const uint32_t metadataBefore = harness.storage.metadataCalls();
  const size_t eventsBefore = harness.storage.events().size();
  const uint64_t bytesReadBefore = harness.storage.bytesReadTotal();
  const uint64_t bytesWrittenBefore = harness.storage.bytesWrittenTotal();
  PreparationStepObservation observation{};
  observation.result = preparation.step();
  const auto wallElapsed = std::chrono::steady_clock::now() - wallStartedAt;
  const uint64_t wallMicroseconds =
      static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(wallElapsed).count());
  const uint32_t wallMilliseconds = static_cast<uint32_t>((wallMicroseconds + 999U) / 1000U);
  observation.elapsedMs = std::max(harness.clockMs() - startedAt, wallMilliseconds);
  observation.operations = harness.ioOperations() - operationsBefore;
  observation.openCalls = harness.storage.openCalls() - openBefore;
  observation.readCalls = harness.storage.readCalls() - readBefore;
  observation.writeCalls = harness.storage.writeCalls() - writeBefore;
  observation.flushCalls = harness.storage.flushCalls() - flushBefore;
  observation.syncCalls = harness.storage.syncCalls() - syncBefore;
  observation.closeCalls = harness.storage.closeCalls() - closeBefore;
  observation.removeCalls = harness.storage.removeCalls() - removeBefore;
  observation.renameCalls = harness.storage.renameCalls() - renameBefore;
  observation.mkdirCalls = harness.storage.mkdirCalls() - mkdirBefore;
  observation.listCalls = harness.storage.listCalls() - listBefore;
  observation.capacityCalls = harness.storage.capacityCalls() - capacityBefore;
  observation.metadataCalls = harness.storage.metadataCalls() - metadataBefore;
  for (size_t index = eventsBefore; index < harness.storage.events().size(); ++index) {
    if (!observation.events.empty()) {
      observation.events += ',';
    }
    observation.events += harness.storage.events()[index];
  }
  observation.bytesRead = harness.storage.bytesReadTotal() - bytesReadBefore;
  observation.bytesWritten = harness.storage.bytesWrittenTotal() - bytesWrittenBefore;
  return observation;
}

void expectBoundedPreparationStep(const PreparationStepObservation& observation) {
  SCOPED_TRACE(::testing::Message() << "io open=" << observation.openCalls << " read=" << observation.readCalls
                                    << " write=" << observation.writeCalls << " flush=" << observation.flushCalls
                                    << " sync=" << observation.syncCalls << " close=" << observation.closeCalls
                                    << " remove=" << observation.removeCalls << " rename=" << observation.renameCalls
                                    << " mkdir=" << observation.mkdirCalls << " list=" << observation.listCalls
                                    << " capacity=" << observation.capacityCalls
                                    << " metadata=" << observation.metadataCalls << " events=" << observation.events);
  EXPECT_LE(observation.elapsedMs, 8U);
  EXPECT_LE(observation.operations, 32U);
  EXPECT_LE(observation.bytesRead, PdfLimits::SourceBufferBytes);
  EXPECT_LE(observation.bytesWritten, PdfLimits::SourceBufferBytes);
}

PdfStepResult cancelToTerminalBounded(PdfPreparation& preparation, PreparationHarness& harness) {
  preparation.requestCancel();
  for (uint32_t slice = 0; slice < 256; ++slice) {
    const PreparationStepObservation observation = observePreparationStep(preparation, harness);
    expectBoundedPreparationStep(observation);
    ++harness.nowMs;
    if (!observation.result.yielded()) {
      return observation.result;
    }
  }
  ADD_FAILURE() << "cancellation did not reach a terminal state";
  return PdfStepResult::failure(PdfStatus::failure(PdfError::BudgetExhausted));
}

void expectRetainedCancelledGeneration(PdfTestCacheIo& storage, const char* cacheRoot, const PdfSourceIdentity& source,
                                       const uint32_t generation) {
  const std::string generationRoot = std::string(cacheRoot) + "/gen_" + std::to_string(generation);
  ASSERT_NE(generation, 0U);
  EXPECT_TRUE(storage.exists(generationRoot));
  for (const std::string& path : storage.paths()) {
    if (!path.starts_with(generationRoot + "/")) {
      continue;
    }
    const size_t leafOffset = path.find_last_of('/');
    const std::string leaf = path.substr(leafOffset == std::string::npos ? 0 : leafOffset + 1U);
    EXPECT_FALSE(leaf.starts_with("build."));
    EXPECT_FALSE(leaf.starts_with("build-"));
    EXPECT_FALSE(leaf.ends_with(".tmp"));
  }

  PdfCacheStore cache;
  ASSERT_TRUE(cache.initialize(storage.io(), cacheRoot).ok());
  PdfBuildCheckpointSelection checkpoints{};
  ASSERT_TRUE(cache.loadCheckpointSlots(source, &checkpoints).ok());
  ASSERT_TRUE(checkpoints.selected);
  const PdfBuildCheckpointSlotState& selected = checkpoints.slots[static_cast<uint8_t>(checkpoints.selectedSlot)];
  EXPECT_TRUE(selected.valid);
  EXPECT_TRUE(selected.sourceMatches);
  EXPECT_EQ(checkpoints.checkpoint.phase, PdfBuildPhase::Cancelled);
  EXPECT_EQ(checkpoints.checkpoint.generation, generation);
}

void expectFreshGenerationRestart(PreparationHarness& harness, const char* sourcePath,
                                  const uint32_t cancelledGeneration) {
  PdfPreparation resumed;
  ASSERT_TRUE(resumed.begin(harness.config(sourcePath)).ok());
  const PdfStepResult result = runToTerminal(resumed, harness);
  ASSERT_TRUE(result.complete()) << static_cast<int>(result.status.error) << "@" << result.status.offset;
  EXPECT_FALSE(resumed.resumedFromCheckpoint());
  EXPECT_NE(resumed.generation(), cancelledGeneration);
  EXPECT_EQ(harness.storage.openHandleCount(), 0U);
}

uint64_t observedPathBytes(const std::vector<PdfTestReadObservation>& observations, const std::string_view path) {
  uint64_t bytes = 0;
  for (const PdfTestReadObservation& observation : observations) {
    if (observation.path == path) {
      bytes += observation.bytesRead;
    }
  }
  return bytes;
}

uint32_t writeTruncateOpensForPath(const std::vector<PdfTestOpenObservation>& observations,
                                   const std::string_view path) {
  return static_cast<uint32_t>(
      std::count_if(observations.begin(), observations.end(), [&](const PdfTestOpenObservation& observation) {
        return observation.path == path && observation.mode == PdfCacheOpenMode::WriteTruncate;
      }));
}

uint64_t observedSourceRangeBytes(const std::vector<PdfTestReadObservation>& observations,
                                  const std::string_view sourcePath, const uint64_t rangeOffset,
                                  const uint64_t rangeLength) {
  const uint64_t rangeEnd = rangeOffset + rangeLength;
  uint64_t sourceBytesRead = 0;
  for (const PdfTestReadObservation& observation : observations) {
    if (observation.path != sourcePath || observation.bytesRead == 0) {
      continue;
    }
    const uint64_t observationStart = std::max<uint64_t>(rangeOffset, observation.offset);
    const uint64_t observationEnd = std::min<uint64_t>(rangeEnd, observation.offset + observation.bytesRead);
    if (observationStart < observationEnd) {
      sourceBytesRead += observationEnd - observationStart;
    }
  }
  return sourceBytesRead;
}

void expectSourceRangeReadOnce(const std::vector<PdfTestReadObservation>& observations,
                               const std::string_view sourcePath, const uint64_t rangeOffset,
                               const uint64_t rangeLength) {
  const uint64_t sourceBytesRead = observedSourceRangeBytes(observations, sourcePath, rangeOffset, rangeLength);
  EXPECT_EQ(sourceBytesRead, rangeLength);
}

TEST(PdfImagePreparation, PublicStepBudgetWitnessRejectsAnOperationOverrun) {
  PreparationStepObservation violating{};
  violating.operations = 33;
  EXPECT_NONFATAL_FAILURE(expectBoundedPreparationStep(violating), "observation.operations");
}

TEST(PdfImagePreparation, SourceRangeReadOnceWitnessRejectsDuplicateCoverage) {
  const std::vector<PdfTestReadObservation> duplicated = {
      {"/books/inline-dct.pdf", 10, 8, 8},
      {"/books/inline-dct.pdf", 10, 8, 8},
  };
  EXPECT_NONFATAL_FAILURE(expectSourceRangeReadOnce(duplicated, "/books/inline-dct.pdf", 10, 8), "sourceBytesRead");
}

TEST(PdfImagePreparation, SingleReaderBackendWitnessRejectsASecondConcurrentReader) {
  PdfTestCacheIo storage;
  storage.addFile("/first.bin", "first");
  storage.addFile("/second.bin", "second");
  storage.setMaximumReadHandles(1);
  PdfCacheHandle first{};
  PdfCacheHandle second{};
  ASSERT_TRUE(storage.io().open(storage.io().context, "/first.bin", PdfCacheOpenMode::Read, &first).ok());
  const PdfStatus rejected = storage.io().open(storage.io().context, "/second.bin", PdfCacheOpenMode::Read, &second);
  EXPECT_EQ(rejected.error, PdfError::LimitExceeded);
  EXPECT_FALSE(second.valid());
  EXPECT_TRUE(storage.io().close(storage.io().context, &first).ok());
  EXPECT_EQ(storage.openHandleCount(), 0U);
}

PdfRequiredFileRecord requiredRecord(const std::string& relativePath, const std::vector<uint8_t>& bytes) {
  PdfRequiredFileRecord record{};
  EXPECT_LT(relativePath.size(), sizeof(record.path));
  std::memcpy(record.path, relativePath.data(), relativePath.size());
  record.path[relativePath.size()] = '\0';
  record.pathLength = static_cast<uint8_t>(relativePath.size());
  record.size = bytes.size();
  record.crc32 = pdfCacheCrc32(bytes.data(), bytes.size());
  return record;
}

TEST(PdfImagePreparation, EmitsDeterministicRetainedImageAtTheTextAnchorWithoutChangingWordCounts) {
  SemanticHarness harness;
  PdfSemanticWriter writer;
  ASSERT_TRUE(writer
                  .begin({&harness, SemanticHarness::write}, {&harness, SemanticHarness::emit},
                         {harness.workspace, sizeof(harness.workspace)})
                  .ok());
  ASSERT_TRUE(writer.beginBlock({PdfSemanticBlockKind::Paragraph, 7, 0}).ok());
  constexpr char text[] = "Figure caption";
  ASSERT_TRUE(writer.writeText(reinterpret_cast<const uint8_t*>(text), std::strlen(text)).ok());
  constexpr char resource[] = "../images/0123456789abcdef.pxc";
  ASSERT_TRUE(
      writer.writeRetainedImage(reinterpret_cast<const uint8_t*>(resource), std::strlen(resource), 320, 200).ok());
  ASSERT_TRUE(writer.endBlock().ok());
  ASSERT_TRUE(writer.finish().ok());

  const std::string xhtml(harness.output.begin(), harness.output.end());
  EXPECT_NE(xhtml.find("<p id=\"b00000007\">Figure caption"
                       "<img src=\"../images/0123456789abcdef.pxc\" width=\"320\" height=\"200\" alt=\"\"/></p>"),
            std::string::npos);
  ASSERT_EQ(harness.blocks.size(), 1U);
  EXPECT_EQ(harness.blocks[0].wordCount, 2U);
  EXPECT_EQ(writer.totalWords(), 2U);
}

TEST(PdfImagePreparation, ReadsEachJpegSourceOnceAndDeduplicatesByHashCrcAndLength) {
  const std::vector<uint8_t> jpeg = {
      0xff, 0xd8, 0xff, 0xe0, 0x00, 0x10, 'J', 'F', 'I', 'F', 0x00, 0x01, 0xff, 0xd9,
  };
  CountingByteSource source(jpeg);
  PdfTestCacheIo storage;
  storage.addDirectory("/cache");
  PdfCacheBudget budget{};
  budget.hardLimit = 1024 * 1024;
  budget.limit = budget.hardLimit;
  uint8_t ioWorkspace[PdfLimits::DecoderOutputBytes]{};
  PdfImageCacheEntry entries[PdfLimits::MaxCoverCandidateSources]{};
  PdfImageCache cache;
  ASSERT_TRUE(cache
                  .begin({storage.io(), "/cache", 42, &budget, ioWorkspace, sizeof(ioWorkspace), entries,
                          static_cast<uint8_t>(std::size(entries)), storage.renameCallback()})
                  .ok());

  PdfCachedImage first{};
  ASSERT_TRUE(cacheJpegToTerminal(cache, source.source(), 640, 480, &first).ok());
  ASSERT_FALSE(first.reused);
  ASSERT_TRUE(storage.exists(first.fullPath));
  EXPECT_EQ(storage.bytes(first.fullPath), jpeg);
  EXPECT_EQ(first.record.size, jpeg.size());
  EXPECT_EQ(first.record.crc32, pdfCacheCrc32(jpeg.data(), jpeg.size()));
  char expectedPath[PDF_CACHE_PATH_CAPACITY]{};
  const uint64_t contentHash = pdfCacheFnv64(jpeg.data(), jpeg.size());
  const uint32_t contentCrc32 = pdfCacheCrc32(jpeg.data(), jpeg.size());
  ASSERT_GT(std::snprintf(expectedPath, sizeof(expectedPath), "/cache/gen_42/images/%016llx-%08lx-%016llx.jpg",
                          static_cast<unsigned long long>(contentHash), static_cast<unsigned long>(contentCrc32),
                          static_cast<unsigned long long>(jpeg.size())),
            0);
  EXPECT_STREQ(first.fullPath, expectedPath);
  EXPECT_EQ(std::string(first.record.path, first.record.pathLength),
            std::string(expectedPath).substr(std::strlen("/cache/")));
  EXPECT_EQ(storage.openHandleCount(), 0U);
  EXPECT_EQ(source.readCalls, 1U);

  PdfCachedImage repeated{};
  ASSERT_TRUE(cacheJpegToTerminal(cache, source.source(), 640, 480, &repeated).ok());
  EXPECT_TRUE(repeated.reused);
  EXPECT_STREQ(repeated.fullPath, first.fullPath);
  EXPECT_EQ(repeated.record.crc32, first.record.crc32);
  EXPECT_EQ(storage.bytes(first.fullPath), jpeg);
  const std::vector<std::string> storedPaths = storage.paths();
  EXPECT_EQ(std::count_if(storedPaths.begin(), storedPaths.end(),
                          [](const std::string& path) { return path.ends_with(".jpg"); }),
            1);
  EXPECT_EQ(cache.entryCount(), 1U);
  EXPECT_EQ(source.readCalls, 2U);
}

TEST(PdfImagePreparation, CapturedInlineJpegsPublishAtomicallyAndDeduplicateWithoutASecondSource) {
  const std::vector<uint8_t> jpeg = {
      0xff, 0xd8, 0xff, 0xe0, 0x00, 0x10, 'J', 'F', 'I', 'F', 0x00, 0x01, 0xff, 0xd9,
  };
  PdfTestCacheIo storage;
  storage.addDirectory("/cache");
  PdfCacheBudget budget{};
  budget.hardLimit = 1024 * 1024;
  budget.limit = budget.hardLimit;
  uint8_t ioWorkspace[PdfLimits::DecoderOutputBytes]{};
  PdfImageCacheEntry entries[2]{};
  PdfImageCache cache;
  ASSERT_TRUE(cache
                  .begin({storage.io(), "/cache", 48, &budget, ioWorkspace, sizeof(ioWorkspace), entries,
                          static_cast<uint8_t>(std::size(entries)), storage.renameCallback()})
                  .ok());

  PdfCapturedJpeg firstCapture{};
  ASSERT_TRUE(captureJpeg(cache, jpeg, 0, &firstCapture).ok());
  EXPECT_EQ(firstCapture.sourceBytes, jpeg.size());
  EXPECT_EQ(firstCapture.contentHash, pdfCacheFnv64(jpeg.data(), jpeg.size()));
  EXPECT_EQ(firstCapture.sourceCrc32, pdfCacheCrc32(jpeg.data(), jpeg.size()));
  PdfCachedImage first{};
  ASSERT_TRUE(publishCapturedJpeg(cache, firstCapture, 640, 480, &first).ok());
  EXPECT_FALSE(first.reused);
  EXPECT_EQ(storage.bytes(first.fullPath), jpeg);

  PdfCapturedJpeg repeatedCapture{};
  ASSERT_TRUE(captureJpeg(cache, jpeg, 1, &repeatedCapture).ok());
  PdfCachedImage repeated{};
  ASSERT_TRUE(publishCapturedJpeg(cache, repeatedCapture, 640, 480, &repeated).ok());

  EXPECT_TRUE(repeated.reused);
  EXPECT_STREQ(repeated.fullPath, first.fullPath);
  EXPECT_EQ(cache.entryCount(), 1U);
  const std::vector<std::string> paths = storage.paths();
  EXPECT_EQ(std::count_if(paths.begin(), paths.end(), [](const std::string& path) { return path.ends_with(".jpg"); }),
            1);
  EXPECT_EQ(std::count_if(paths.begin(), paths.end(), [](const std::string& path) { return path.ends_with(".tmp"); }),
            0);
  EXPECT_EQ(storage.openHandleCount(), 0U);
}

TEST(PdfImagePreparation, LargeJpegUsesOneSequentialSourcePassWithoutRereadingChunks) {
  const std::vector<uint8_t> jpeg = largeSyntheticJpegBytes();
  CountingByteSource source(jpeg);
  PdfTestCacheIo storage;
  storage.addDirectory("/cache");
  PdfCacheBudget budget{};
  budget.hardLimit = 1024 * 1024;
  budget.limit = budget.hardLimit;
  uint8_t ioWorkspace[PdfLimits::DecoderOutputBytes]{};
  PdfImageCacheEntry entries[2]{};
  PdfImageCache cache;
  ASSERT_TRUE(cache
                  .begin({storage.io(), "/cache", 44, &budget, ioWorkspace, sizeof(ioWorkspace), entries,
                          static_cast<uint8_t>(std::size(entries)), storage.renameCallback()})
                  .ok());

  PdfCachedImage cached{};
  ASSERT_TRUE(cacheJpegToTerminal(cache, source.source(), 640, 480, &cached).ok());

  ASSERT_EQ(source.totalBytesRead, jpeg.size());
  ASSERT_EQ(source.readOffsets.size(), source.readSizes.size());
  uint64_t expectedOffset = 0;
  for (size_t index = 0; index < source.readOffsets.size(); ++index) {
    EXPECT_EQ(source.readOffsets[index], expectedOffset) << index;
    EXPECT_LE(source.readSizes[index], PdfLimits::SourceBufferBytes) << index;
    expectedOffset += source.readSizes[index];
  }
  EXPECT_EQ(expectedOffset, jpeg.size());
  EXPECT_EQ(storage.bytes(cached.fullPath), jpeg);
  EXPECT_EQ(storage.openHandleCount(), 0U);
}

TEST(PdfImagePreparation, CachesMoreThanEightUniqueJpegsWithDistinctPathsAndReusesAtCapacity) {
  PdfTestCacheIo storage;
  storage.addDirectory("/cache");
  PdfCacheBudget budget{};
  budget.hardLimit = 1024 * 1024;
  budget.limit = budget.hardLimit;
  uint8_t ioWorkspace[PdfLimits::DecoderOutputBytes]{};
  PdfImageCacheEntry entries[12]{};
  PdfImageCache cache;
  ASSERT_TRUE(cache
                  .begin({storage.io(), "/cache", 43, &budget, ioWorkspace, sizeof(ioWorkspace), entries,
                          static_cast<uint8_t>(std::size(entries)), storage.renameCallback()})
                  .ok());

  std::vector<std::string> paths;
  std::vector<uint8_t> firstJpeg;
  for (uint8_t index = 0; index < std::size(entries); ++index) {
    const std::vector<uint8_t> jpeg = {
        0xff, 0xd8, 0xff, 0xe1, 0x00, 0x03, index, 0xff, 0xd9,
    };
    if (index == 0) {
      firstJpeg = jpeg;
    }
    CountingByteSource source(jpeg);
    PdfCachedImage cached{};
    ASSERT_TRUE(cacheJpegToTerminal(cache, source.source(), 32, 24, &cached).ok()) << static_cast<unsigned>(index);
    EXPECT_FALSE(cached.reused);
    EXPECT_EQ(source.readCalls, 1U);
    EXPECT_EQ(std::find(paths.begin(), paths.end(), cached.fullPath), paths.end());
    paths.emplace_back(cached.fullPath);
  }
  EXPECT_EQ(cache.entryCount(), std::size(entries));

  CountingByteSource repeatedSource(firstJpeg);
  PdfCachedImage repeated{};
  ASSERT_TRUE(cacheJpegToTerminal(cache, repeatedSource.source(), 32, 24, &repeated).ok());
  EXPECT_TRUE(repeated.reused);
  EXPECT_EQ(repeated.fullPath, paths.front());
  EXPECT_EQ(repeatedSource.readCalls, 1U);
  EXPECT_EQ(cache.entryCount(), std::size(entries));
}

TEST(PdfImagePreparation, SameHashWithDifferentCrcAndLengthPublishesDistinctJpegPaths) {
  PdfTestCacheIo storage;
  storage.addDirectory("/cache");
  PdfCacheBudget budget{};
  budget.hardLimit = 1024 * 1024;
  budget.limit = budget.hardLimit;
  uint8_t ioWorkspace[PdfLimits::DecoderOutputBytes]{};
  PdfImageCacheEntry entries[2]{};
  PdfImageCache cache;
  ASSERT_TRUE(cache
                  .begin({storage.io(), "/cache", 45, &budget, ioWorkspace, sizeof(ioWorkspace), entries,
                          static_cast<uint8_t>(std::size(entries)), storage.renameCallback()})
                  .ok());

  const std::vector<uint8_t> firstBytes = {
      0xff, 0xd8, 0x01, 0x02, 0xff, 0xd9,
  };
  CountingByteSource firstSource(firstBytes);
  PdfCachedImage first{};
  ASSERT_TRUE(cacheJpegToTerminal(cache, firstSource.source(), 32, 24, &first).ok());

  const std::vector<uint8_t> secondBytes = {
      0xff, 0xd8, 0x09, 0x08, 0x07, 0xff, 0xd9,
  };
  const uint64_t collidedHash = pdfCacheFnv64(secondBytes.data(), secondBytes.size());
  entries[0].contentHash = collidedHash;
  char collidedFirstPath[PDF_CACHE_PATH_CAPACITY]{};
  ASSERT_GT(
      std::snprintf(collidedFirstPath, sizeof(collidedFirstPath), "/cache/gen_45/images/%016llx-%08lx-%016llx.jpg",
                    static_cast<unsigned long long>(collidedHash),
                    static_cast<unsigned long>(pdfCacheCrc32(firstBytes.data(), firstBytes.size())),
                    static_cast<unsigned long long>(firstBytes.size())),
      0);
  ASSERT_TRUE(storage.renameCallback()(storage.io().context, first.fullPath, collidedFirstPath).ok());

  CountingByteSource secondSource(secondBytes);
  PdfCachedImage second{};
  ASSERT_TRUE(cacheJpegToTerminal(cache, secondSource.source(), 32, 24, &second).ok());

  EXPECT_FALSE(second.reused);
  EXPECT_NE(second.fullPath, std::string(collidedFirstPath));
  EXPECT_EQ(storage.bytes(collidedFirstPath), firstBytes);
  EXPECT_EQ(storage.bytes(second.fullPath), secondBytes);
  EXPECT_EQ(cache.entryCount(), 2U);
}

TEST(PdfImagePreparation, UniqueJpegBeyondIdentityCapacityRemovesTemporaryWithoutPublishing) {
  PdfTestCacheIo storage;
  storage.addDirectory("/cache");
  PdfCacheBudget budget{};
  budget.hardLimit = 1024 * 1024;
  budget.limit = budget.hardLimit;
  uint8_t ioWorkspace[PdfLimits::DecoderOutputBytes]{};
  PdfImageCacheEntry entries[1]{};
  PdfImageCache cache;
  ASSERT_TRUE(cache
                  .begin({storage.io(), "/cache", 46, &budget, ioWorkspace, sizeof(ioWorkspace), entries, 1,
                          storage.renameCallback()})
                  .ok());
  const std::vector<uint8_t> firstBytes = {
      0xff, 0xd8, 0x01, 0xff, 0xd9,
  };
  CountingByteSource firstSource(firstBytes);
  PdfCachedImage first{};
  ASSERT_TRUE(cacheJpegToTerminal(cache, firstSource.source(), 32, 24, &first).ok());
  const std::vector<uint8_t> secondBytes = {
      0xff, 0xd8, 0x02, 0xff, 0xd9,
  };
  CountingByteSource secondSource(secondBytes);
  PdfCachedImage second{};

  const PdfStatus status = cacheJpegToTerminal(cache, secondSource.source(), 32, 24, &second);

  EXPECT_EQ(status.error, PdfError::LimitExceeded);
  EXPECT_EQ(cache.entryCount(), 1U);
  EXPECT_EQ(storage.bytes(first.fullPath), firstBytes);
  const std::vector<std::string> paths = storage.paths();
  EXPECT_EQ(std::count_if(paths.begin(), paths.end(),
                          [](const std::string& path) { return path.ends_with(".jpg") || path.ends_with(".tmp"); }),
            1);
  EXPECT_EQ(storage.openHandleCount(), 0U);
}

TEST(PdfImagePreparation, JpegRenameFailureRemovesTemporaryAndPublishesNoIdentity) {
  PdfTestCacheIo storage;
  storage.addDirectory("/cache");
  PdfCacheBudget budget{};
  budget.hardLimit = 1024 * 1024;
  budget.limit = budget.hardLimit;
  uint8_t ioWorkspace[PdfLimits::DecoderOutputBytes]{};
  PdfImageCacheEntry entries[1]{};
  PdfImageCache cache;
  ASSERT_TRUE(cache
                  .begin({storage.io(), "/cache", 47, &budget, ioWorkspace, sizeof(ioWorkspace), entries, 1,
                          storage.renameCallback()})
                  .ok());
  storage.fail(PdfTestFaultPoint::Rename);
  const std::vector<uint8_t> bytes = {
      0xff, 0xd8, 0x03, 0xff, 0xd9,
  };
  CountingByteSource source(bytes);
  PdfCachedImage image{};

  const PdfStatus status = cacheJpegToTerminal(cache, source.source(), 32, 24, &image);

  EXPECT_EQ(status.error, PdfError::IoFailure);
  EXPECT_EQ(cache.entryCount(), 0U);
  const std::vector<std::string> paths = storage.paths();
  EXPECT_EQ(std::count_if(paths.begin(), paths.end(),
                          [](const std::string& path) { return path.ends_with(".jpg") || path.ends_with(".tmp"); }),
            0);
  EXPECT_EQ(storage.openHandleCount(), 0U);
}

TEST(PdfImagePreparation, RemovesPartialJpegAndPublishesNoEntryAfterAShortWrite) {
  const std::vector<uint8_t> jpeg = {0xff, 0xd8, 1, 2, 3, 4, 5, 6, 0xff, 0xd9};
  PdfTestByteSource source(jpeg);
  PdfTestCacheIo storage;
  storage.addDirectory("/cache");
  storage.setWriteAllowance(4);
  PdfCacheBudget budget{};
  budget.hardLimit = 1024 * 1024;
  budget.limit = budget.hardLimit;
  uint8_t ioWorkspace[PdfLimits::DecoderOutputBytes]{};
  PdfImageCacheEntry entries[1]{};
  PdfImageCache cache;
  ASSERT_TRUE(cache
                  .begin({storage.io(), "/cache", 9, &budget, ioWorkspace, sizeof(ioWorkspace), entries, 1,
                          storage.renameCallback()})
                  .ok());

  PdfCachedImage result{};
  const PdfStatus status = cacheJpegToTerminal(cache, source.source(), 10, 10, &result);

  EXPECT_EQ(status.error, PdfError::IoFailure);
  EXPECT_EQ(cache.entryCount(), 0U);
  EXPECT_EQ(storage.openHandleCount(), 0U);
  for (const std::string& path : storage.paths()) {
    EXPECT_EQ(path.find(".jpg"), std::string::npos) << path;
  }
}

TEST(PdfImagePreparation, RemovesClosedJpegAndPublishesNoEntryWhenDurableCloseEvidenceFails) {
  const std::vector<uint8_t> jpeg = {0xff, 0xd8, 8, 7, 6, 5, 0xff, 0xd9};
  PdfTestByteSource source(jpeg);
  PdfTestCacheIo storage;
  storage.addDirectory("/cache");
  storage.fail(PdfTestFaultPoint::Close);
  PdfCacheBudget budget{};
  budget.hardLimit = 1024 * 1024;
  budget.limit = budget.hardLimit;
  uint8_t ioWorkspace[PdfLimits::DecoderOutputBytes]{};
  PdfImageCacheEntry entries[1]{};
  PdfImageCache cache;
  ASSERT_TRUE(cache
                  .begin({storage.io(), "/cache", 10, &budget, ioWorkspace, sizeof(ioWorkspace), entries, 1,
                          storage.renameCallback()})
                  .ok());

  PdfCachedImage result{};
  const PdfStatus status = cacheJpegToTerminal(cache, source.source(), 10, 10, &result);

  EXPECT_EQ(status.error, PdfError::IoFailure);
  EXPECT_EQ(cache.entryCount(), 0U);
  EXPECT_EQ(storage.openHandleCount(), 0U);
  for (const std::string& path : storage.paths()) {
    EXPECT_EQ(path.find(".jpg"), std::string::npos) << path;
  }
}

TEST(PdfImagePreparation, RetainsFiguresAndEarlyCoversButSuppressesBackgroundRulesTinyDecorationsAndRepeatedLogos) {
  const PdfImageMeaningInput figure{
      {PdfObjectReference{7, 0}, 4, 640, 480, 80 << 16, 180 << 16, 560 << 16, 540 << 16}, 612, 792, 3, 11, 1, true,
  };
  PdfImageMeaningDecision decision{};
  ASSERT_TRUE(pdfClassifyMeaningfulImage(figure, &decision).ok());
  EXPECT_TRUE(decision.retain);
  EXPECT_FALSE(decision.coverCandidate);
  EXPECT_EQ(decision.anchorOrdinal, 11U);

  PdfImageMeaningInput cover = figure;
  cover.placement.xMin = 20 << 16;
  cover.placement.yMin = 20 << 16;
  cover.placement.xMax = 592 << 16;
  cover.placement.yMax = 772 << 16;
  cover.sourcePageIndex = 0;
  cover.nearbySemanticContent = false;
  cover.firstMeaningfulEarlyImage = true;
  ASSERT_TRUE(pdfClassifyMeaningfulImage(cover, &decision).ok());
  EXPECT_TRUE(decision.retain);
  EXPECT_TRUE(decision.coverCandidate);

  PdfImageMeaningInput background = cover;
  background.sourcePageIndex = 4;
  background.firstMeaningfulEarlyImage = false;
  ASSERT_TRUE(pdfClassifyMeaningfulImage(background, &decision).ok());
  EXPECT_FALSE(decision.retain);
  EXPECT_EQ(decision.omitReason, PdfImageOmitReason::Background);

  PdfImageMeaningInput rule = figure;
  rule.placement.yMax = rule.placement.yMin + (2 << 16);
  ASSERT_TRUE(pdfClassifyMeaningfulImage(rule, &decision).ok());
  EXPECT_FALSE(decision.retain);
  EXPECT_EQ(decision.omitReason, PdfImageOmitReason::Rule);

  PdfImageMeaningInput tiny = figure;
  tiny.placement.xMax = tiny.placement.xMin + (12 << 16);
  tiny.placement.yMax = tiny.placement.yMin + (12 << 16);
  ASSERT_TRUE(pdfClassifyMeaningfulImage(tiny, &decision).ok());
  EXPECT_FALSE(decision.retain);
  EXPECT_EQ(decision.omitReason, PdfImageOmitReason::TinyDecoration);

  PdfImageMeaningInput logo = figure;
  logo.repetitionCount = 3;
  ASSERT_TRUE(pdfClassifyMeaningfulImage(logo, &decision).ok());
  EXPECT_FALSE(decision.retain);
  EXPECT_EQ(decision.omitReason, PdfImageOmitReason::RepeatedDecoration);
}

TEST(PdfImagePreparation, ReusesTheEightKiBPageTextAndFourKiBDecoderWorkspacesForTheImagePhase) {
  uint8_t pageText[PdfLimits::PageTextBytes]{};
  uint8_t decoderOutput[PdfLimits::DecoderOutputBytes]{};
  PdfImageWorkspace workspace{};

  ASSERT_TRUE(
      pdfMakePreparationImageWorkspace(pageText, sizeof(pageText), decoderOutput, sizeof(decoderOutput), &workspace)
          .ok());
  EXPECT_EQ(workspace.sourceRow, pageText);
  EXPECT_EQ(workspace.sourceRowCapacity, PdfLimits::PageTextBytes);
  EXPECT_EQ(workspace.outputRow, decoderOutput);
  EXPECT_EQ(workspace.outputRowCapacity, PdfLimits::DecoderOutputBytes);

  EXPECT_EQ(
      pdfMakePreparationImageWorkspace(pageText, sizeof(pageText) - 1, decoderOutput, sizeof(decoderOutput), &workspace)
          .error,
      PdfError::InsufficientMemory);
  EXPECT_EQ(
      pdfMakePreparationImageWorkspace(pageText, sizeof(pageText), decoderOutput, sizeof(decoderOutput) - 1, &workspace)
          .error,
      PdfError::InsufficientMemory);
}

TEST(PdfImagePreparation, MaskSpoolCloseWritesAtMostOneMetadataChunkPerStep) {
  PdfTestCacheIo storage;
  storage.addDirectory("/cache");
  constexpr char path[] = "/cache/build.mask";
  PdfMaskSpool writer;
  ASSERT_TRUE(writer.beginWrite(storage.io(), path).ok());

  for (uint8_t index = 0; index < PDF_MASK_SPOOL_MAX_RECORDS; ++index) {
    PdfByteSink baseSink{};
    ASSERT_TRUE(writer.beginRecord(42U + index, 7U + index, 1, 1, &baseSink).ok());
    const uint8_t base = index;
    size_t written = 0;
    ASSERT_TRUE(baseSink.write(baseSink.context, &base, sizeof(base), &written).ok());
    ASSERT_EQ(written, sizeof(base));

    uint8_t maskRow[1]{};
    uint8_t alphaRow[1]{};
    PdfMaskPlaneWriter plane;
    PdfMaskPlaneConfig config{};
    config.sourceWidth = 1;
    config.sourceHeight = 1;
    config.outputWidth = 1;
    config.outputHeight = 1;
    config.bitsPerComponent = 8;
    config.rowWorkspace = maskRow;
    config.rowWorkspaceBytes = sizeof(maskRow);
    config.outputWorkspace = alphaRow;
    config.outputWorkspaceBytes = sizeof(alphaRow);
    ASSERT_TRUE(writer.beginAlpha(config, &plane).ok());
    PdfByteSink alphaSink = plane.decodedSink();
    const uint8_t alpha = 0xFF;
    ASSERT_TRUE(alphaSink.write(alphaSink.context, &alpha, sizeof(alpha), &written).ok());
    ASSERT_EQ(written, sizeof(alpha));
    ASSERT_TRUE(plane.finish().ok());
    ASSERT_TRUE(writer.finishRecord().ok());
  }

  PdfMaskSpoolCloseRuntime runtime{};
  ASSERT_TRUE(writer.beginCloseWrite(&runtime).ok());
  bool complete = false;
  for (uint16_t step = 0; step < PDF_MASK_SPOOL_MAX_RECORDS + 8U && !complete; ++step) {
    const uint32_t writesBefore = storage.writeCalls();
    const uint64_t bytesBefore = storage.bytesWrittenTotal();
    PdfWorkBudget budget{1, 60};
    const PdfStepResult result = writer.stepCloseWrite(runtime, budget);
    ASSERT_FALSE(result.failed());
    EXPECT_LE(storage.writeCalls() - writesBefore, 1U);
    EXPECT_LE(storage.bytesWrittenTotal() - bytesBefore, 60U);
    complete = result.complete();
  }
  ASSERT_TRUE(complete);
  EXPECT_EQ(storage.openHandleCount(), 0U);

  uint8_t ioWorkspace[64]{};
  PdfMaskSpool reader;
  ASSERT_TRUE(openMaskSpoolToTerminal(reader, storage, path, ioWorkspace, sizeof(ioWorkspace)).ok());
  EXPECT_EQ(reader.recordCount(), PDF_MASK_SPOOL_MAX_RECORDS);
  EXPECT_TRUE(reader.closeRead().ok());
  EXPECT_EQ(storage.openHandleCount(), 0U);
}

TEST(PdfImagePreparation, MaskSpoolRejectsPayloadCrcCorruptionAndClosesItsOnlyReader) {
  PdfTestCacheIo storage;
  storage.addDirectory("/cache");
  constexpr char path[] = "/cache/build.mask";
  PdfMaskSpool writer;
  ASSERT_TRUE(writer.beginWrite(storage.io(), path).ok());
  PdfByteSink baseSink{};
  ASSERT_TRUE(writer.beginRecord(42, 7, 2, 2, &baseSink).ok());
  const uint8_t base[] = {2, 0, 2, 0, 0x10, 0xB0};
  size_t written = 0;
  ASSERT_TRUE(baseSink.write(baseSink.context, base, sizeof(base), &written).ok());
  ASSERT_EQ(written, sizeof(base));
  uint8_t maskRows[4]{};
  uint8_t alphaRows[2]{};
  PdfMaskPlaneWriter plane;
  PdfMaskPlaneConfig config{};
  config.sourceWidth = 2;
  config.sourceHeight = 2;
  config.outputWidth = 2;
  config.outputHeight = 2;
  config.bitsPerComponent = 8;
  config.rowWorkspace = maskRows;
  config.rowWorkspaceBytes = sizeof(maskRows);
  config.outputWorkspace = alphaRows;
  config.outputWorkspaceBytes = sizeof(alphaRows);
  ASSERT_TRUE(writer.beginAlpha(config, &plane).ok());
  PdfByteSink alphaSink = plane.decodedSink();
  const uint8_t alpha[] = {0xFF, 0x80, 0x00, 0xFF};
  ASSERT_TRUE(alphaSink.write(alphaSink.context, alpha, sizeof(alpha), &written).ok());
  ASSERT_EQ(written, sizeof(alpha));
  ASSERT_TRUE(plane.finish().ok());
  ASSERT_TRUE(writer.finishRecord().ok());
  ASSERT_TRUE(writer.closeWrite().ok());
  ASSERT_EQ(storage.openCallsForPath(path), 1U);
  ASSERT_EQ(storage.openHandleCount(), 0U);

  storage.corruptByte(path, 12, 0x01);
  uint8_t ioWorkspace[64]{};
  PdfMaskSpool reader;
  const PdfStatus status = openMaskSpoolToTerminal(reader, storage, path, ioWorkspace, sizeof(ioWorkspace));

  EXPECT_EQ(status.error, PdfError::Malformed);
  EXPECT_EQ(storage.openCallsForPath(path), 2U);
  EXPECT_EQ(storage.openHandleCount(), 0U);
  EXPECT_FALSE(storage.exists(path));
}

TEST(PdfImagePreparation, MaskSpoolWriteFaultMatrixClosesAndRemovesEveryPartialArtifact) {
  constexpr char path[] = "/cache/build.mask";
  for (uint32_t occurrence = 1; occurrence <= 6; ++occurrence) {
    PdfTestCacheIo storage;
    storage.addDirectory("/cache");
    storage.fail(PdfTestFaultPoint::Write, occurrence);
    PdfMaskSpool spool;

    const PdfStatus status = writeTwoByTwoMaskSpool(storage, path, spool);
    spool.abort();

    EXPECT_FALSE(status.ok()) << occurrence;
    EXPECT_EQ(storage.openHandleCount(), 0U) << occurrence;
    EXPECT_FALSE(storage.exists(path)) << occurrence;
  }
  for (const PdfTestFaultPoint fault :
       {PdfTestFaultPoint::Open, PdfTestFaultPoint::Flush, PdfTestFaultPoint::Sync, PdfTestFaultPoint::Close}) {
    PdfTestCacheIo storage;
    storage.addDirectory("/cache");
    storage.fail(fault);
    PdfMaskSpool spool;

    const PdfStatus status = writeTwoByTwoMaskSpool(storage, path, spool);
    spool.abort();

    EXPECT_FALSE(status.ok()) << static_cast<unsigned>(fault);
    EXPECT_EQ(storage.openHandleCount(), 0U) << static_cast<unsigned>(fault);
    EXPECT_FALSE(storage.exists(path)) << static_cast<unsigned>(fault);
  }
}

TEST(PdfImagePreparation, MaskSpoolReadFaultMatrixClosesItsReaderAndRejectsTheBuildSpool) {
  constexpr char path[] = "/cache/build.mask";
  for (const PdfTestFaultPoint fault : {PdfTestFaultPoint::Open, PdfTestFaultPoint::Metadata}) {
    PdfTestCacheIo storage;
    storage.addDirectory("/cache");
    PdfMaskSpool writer;
    ASSERT_TRUE(writeTwoByTwoMaskSpool(storage, path, writer).ok());
    storage.fail(fault);
    uint8_t workspace[64]{};
    PdfMaskSpool reader;

    const PdfStatus status = openMaskSpoolToTerminal(reader, storage, path, workspace, sizeof(workspace));

    EXPECT_FALSE(status.ok()) << static_cast<unsigned>(fault);
    EXPECT_EQ(storage.openHandleCount(), 0U) << static_cast<unsigned>(fault);
    EXPECT_FALSE(storage.exists(path)) << static_cast<unsigned>(fault);
  }
  for (uint32_t occurrence = 1; occurrence <= 5; ++occurrence) {
    PdfTestCacheIo storage;
    storage.addDirectory("/cache");
    PdfMaskSpool writer;
    ASSERT_TRUE(writeTwoByTwoMaskSpool(storage, path, writer).ok());
    storage.fail(PdfTestFaultPoint::Read, occurrence);
    uint8_t workspace[64]{};
    PdfMaskSpool reader;

    const PdfStatus status = openMaskSpoolToTerminal(reader, storage, path, workspace, sizeof(workspace));

    EXPECT_FALSE(status.ok()) << occurrence;
    EXPECT_EQ(storage.openHandleCount(), 0U) << occurrence;
    EXPECT_FALSE(storage.exists(path)) << occurrence;
  }
  PdfTestCacheIo storage;
  storage.addDirectory("/cache");
  PdfMaskSpool writer;
  ASSERT_TRUE(writeTwoByTwoMaskSpool(storage, path, writer).ok());
  uint8_t workspace[64]{};
  PdfMaskSpool reader;
  ASSERT_TRUE(openMaskSpoolToTerminal(reader, storage, path, workspace, sizeof(workspace)).ok());
  storage.fail(PdfTestFaultPoint::Close);
  EXPECT_EQ(reader.closeRead().error, PdfError::IoFailure);
  reader.abort();
  EXPECT_EQ(storage.openHandleCount(), 0U);
  EXPECT_FALSE(storage.exists(path));
}

TEST(PdfImagePreparation, MaskSpoolRejectsHeaderFooterRecordAndBothPlaneCorruption) {
  constexpr char path[] = "/cache/build.mask";
  for (const size_t corruption : {size_t{0}, size_t{12}, size_t{18}, size_t{22}, size_t{82}}) {
    PdfTestCacheIo storage;
    storage.addDirectory("/cache");
    PdfMaskSpool writer;
    ASSERT_TRUE(writeTwoByTwoMaskSpool(storage, path, writer).ok());
    const size_t size = storage.bytes(path).size();
    const size_t offset = corruption == 82 ? size - 1 : corruption;
    storage.corruptByte(path, offset, 0x01);
    uint8_t workspace[64]{};
    PdfMaskSpool reader;

    const PdfStatus status = openMaskSpoolToTerminal(reader, storage, path, workspace, sizeof(workspace));

    EXPECT_EQ(status.error, PdfError::Malformed) << offset;
    EXPECT_EQ(storage.openHandleCount(), 0U) << offset;
    EXPECT_FALSE(storage.exists(path)) << offset;
  }
}

TEST(PdfImagePreparation, PdfPreparationRetainsJpegBesideCaptionAndRegistersItBeforeManifestCommit) {
  PreparationHarness harness;
  harness.storage.addFile("/books/jpeg-caption.pdf", loadFixture("jpeg_caption.pdf"), 1234, true);
  PdfPreparation preparation;
  ASSERT_TRUE(preparation.begin(harness.config()).ok());

  const PdfStepResult result = runToTerminal(preparation, harness);

  ASSERT_TRUE(result.complete()) << static_cast<int>(result.status.error) << "@" << result.status.offset
                                 << " phase=" << static_cast<int>(preparation.phase());
  const std::string generationRoot =
      std::string(preparation.cacheRoot()) + "/gen_" + std::to_string(preparation.generation());
  const auto paths = harness.storage.paths();
  const auto jpegPath = std::find_if(paths.begin(), paths.end(), [&](const std::string& path) {
    return path.starts_with(generationRoot + "/images/") && path.ends_with(".jpg");
  });
  ASSERT_NE(jpegPath, paths.end());
  const std::vector<uint8_t>& cachedJpeg = harness.storage.bytes(*jpegPath);
  ASSERT_EQ(cachedJpeg.size(), 341U);
  EXPECT_EQ(pdfCacheFnv64(cachedJpeg.data(), cachedJpeg.size()), 0x73CF4D5248A29162ULL);
  EXPECT_EQ(pdfCacheCrc32(cachedJpeg.data(), cachedJpeg.size()), 0x43FBC23CU);
  const std::string sectionPath = generationRoot + "/sections/000000.xhtml";
  const std::string xhtml(harness.storage.bytes(sectionPath).begin(), harness.storage.bytes(sectionPath).end());
  EXPECT_NE(xhtml.find("Figure caption."), std::string::npos);
  EXPECT_NE(xhtml.find("<img src=\"../images/"), std::string::npos);
  EXPECT_EQ(preparation.totalWords(), 2U);

  PdfCacheStore cache;
  ASSERT_TRUE(cache.initialize(harness.storage.io(), preparation.cacheRoot()).ok());
  PdfCacheManifestSelection selection{};
  ASSERT_TRUE(cache.loadManifestSlots(preparation.sourceIdentity(), &selection).ok());
  ASSERT_TRUE(selection.selected);
  EXPECT_EQ(selection.manifest.requiredFileCount, 6U);

  PdfReflowDocument reopened;
  const PdfStatus initializeStatus =
      reopened.initialize(harness.storage.io(), "/books/jpeg-caption.pdf", "/.crosspoint");
  ASSERT_TRUE(initializeStatus.ok()) << "initialize PdfStatus{error=" << static_cast<int>(initializeStatus.error)
                                     << ", offset=" << initializeStatus.offset << '}';
  const PdfStatus reopenStatus = reopened.loadCompletedCache();
  ASSERT_TRUE(reopenStatus.ok()) << "loadCompletedCache PdfStatus{error=" << static_cast<int>(reopenStatus.error)
                                 << ", offset=" << reopenStatus.offset << '}';
  ASSERT_EQ(reopened.getSectionCount(), 1);
  EXPECT_EQ(reopened.getTotalWordCount(), 2U);
  EXPECT_EQ(reopened.getSectionInfo(0).wordCount, 2U);

  BufferPrint reopenedSection;
  ASSERT_TRUE(reopened.streamSection(0, reopenedSection, 37U));
  const std::string reopenedXhtml(reopenedSection.output.begin(), reopenedSection.output.end());
  EXPECT_NE(reopenedXhtml.find("<img src=\"../images/"), std::string::npos);
  EXPECT_NE(reopenedXhtml.find("Figure caption."), std::string::npos);
  EXPECT_EQ(harness.storage.openHandleCount(), 0U);
}

TEST(PdfImagePreparation, PdfPreparationInflatesGrayRasterIntoPixelCacheBeforeManifestCommit) {
  PreparationHarness harness;
  harness.storage.addFile("/books/flate-gray-caption.pdf", loadFixture("flate_gray_caption.pdf"), 1234, true);
  PdfPreparation preparation;
  ASSERT_TRUE(preparation.begin(harness.config("/books/flate-gray-caption.pdf")).ok());

  const PdfStepResult result = runToTerminal(preparation, harness);

  ASSERT_TRUE(result.complete()) << static_cast<int>(result.status.error) << "@" << result.status.offset
                                 << " phase=" << static_cast<int>(preparation.phase());
  const std::string generationRoot =
      std::string(preparation.cacheRoot()) + "/gen_" + std::to_string(preparation.generation());
  const auto paths = harness.storage.paths();
  const auto pixelCachePath = std::find_if(paths.begin(), paths.end(), [&](const std::string& path) {
    return path.starts_with(generationRoot + "/images/") && path.ends_with(".pxc");
  });
  ASSERT_NE(pixelCachePath, paths.end());
  const std::vector<uint8_t> expectedPixelCache = {2, 0, 2, 0, 0x10, 0xB0};
  EXPECT_EQ(harness.storage.bytes(*pixelCachePath), expectedPixelCache);
  const std::string sectionPath = generationRoot + "/sections/000000.xhtml";
  const std::string xhtml(harness.storage.bytes(sectionPath).begin(), harness.storage.bytes(sectionPath).end());
  EXPECT_NE(xhtml.find("Raster caption."), std::string::npos);
  EXPECT_NE(xhtml.find("<img src=\"../images/"), std::string::npos);

  PdfCacheStore cache;
  ASSERT_TRUE(cache.initialize(harness.storage.io(), preparation.cacheRoot()).ok());
  PdfCacheManifestSelection selection{};
  ASSERT_TRUE(cache.loadManifestSlots(preparation.sourceIdentity(), &selection).ok());
  ASSERT_TRUE(selection.selected);
  EXPECT_EQ(selection.manifest.requiredFileCount, 6U);
  const std::string spoolPath = generationRoot + "/build.nav";
  EXPECT_FALSE(harness.storage.exists(spoolPath));
  EXPECT_EQ(harness.storage.openCallsForPath(spoolPath), 2U);
  EXPECT_GT(preparation.navigationSpoolBytes(), 0U);
  EXPECT_EQ(preparation.navigationSpoolWriteCount(), 1U);
  EXPECT_EQ(preparation.navigationSpoolReadCount(), 1U);
  EXPECT_LE(preparation.resourcePeakBytes(), PdfLimits::TotalWorkspaceBytes);
  EXPECT_EQ(harness.storage.openHandleCount(), 0U);
}

TEST(PdfImagePreparation, DirectAndFullyIndirectIndexedRgbImagesReachThePixelCache) {
  struct Case {
    const char* fixture;
    const char* source;
    const char* caption;
  };
  const Case cases[] = {
      {"direct_indexed_caption.pdf", "/books/direct-indexed.pdf", "Direct indexed caption."},
      {"fully_indirect_indexed_caption.pdf", "/books/indirect-indexed.pdf", "Indirect indexed caption."},
  };
  for (const Case& testCase : cases) {
    PreparationHarness harness;
    harness.storage.addFile(testCase.source, loadFixture(testCase.fixture), 1234, true);
    PdfPreparation preparation;
    ASSERT_TRUE(preparation.begin(harness.config(testCase.source)).ok());

    const PdfStepResult result = runToTerminal(preparation, harness);

    ASSERT_TRUE(result.complete()) << testCase.fixture << " error=" << static_cast<int>(result.status.error) << "@"
                                   << result.status.offset << " phase=" << static_cast<int>(preparation.phase());
    const std::string generationRoot =
        std::string(preparation.cacheRoot()) + "/gen_" + std::to_string(preparation.generation());
    const auto paths = harness.storage.paths();
    const auto pixelCachePath = std::find_if(paths.begin(), paths.end(), [&](const std::string& path) {
      return path.starts_with(generationRoot + "/images/") && path.ends_with(".pxc");
    });
    ASSERT_NE(pixelCachePath, paths.end()) << testCase.fixture;
    const std::vector<uint8_t> expectedPixelCache = {4, 0, 1, 0, 0x1B};
    EXPECT_EQ(harness.storage.bytes(*pixelCachePath), expectedPixelCache) << testCase.fixture;
    const std::string sectionPath = generationRoot + "/sections/000000.xhtml";
    const std::string xhtml(harness.storage.bytes(sectionPath).begin(), harness.storage.bytes(sectionPath).end());
    EXPECT_NE(xhtml.find(testCase.caption), std::string::npos);
    EXPECT_NE(xhtml.find("<img src=\"../images/"), std::string::npos);
    PdfCacheStore cache;
    ASSERT_TRUE(cache.initialize(harness.storage.io(), preparation.cacheRoot()).ok());
    PdfCacheManifestSelection selection{};
    ASSERT_TRUE(cache.loadManifestSlots(preparation.sourceIdentity(), &selection).ok());
    ASSERT_TRUE(selection.selected);
    EXPECT_EQ(selection.manifest.warningFlags, 0U);
    EXPECT_EQ(selection.manifest.requiredFileCount, 6U);
    EXPECT_EQ(harness.storage.openHandleCount(), 0U);
  }
}

TEST(PdfImagePreparation, LargeRasterDecodeYieldsAcrossBoundedPreparationSlices) {
  PreparationHarness harness;
  harness.storage.addFile("/books/large-raster.pdf", loadFixture("large_raster_caption.pdf"), 1234, true);
  PdfPreparation preparation;
  ASSERT_TRUE(preparation.begin(harness.config("/books/large-raster.pdf")).ok());
  while (preparation.phase() != PdfPreparationPhase::DecodeImages) {
    const PdfStepResult step = preparation.step();
    ++harness.nowMs;
    ASSERT_TRUE(step.yielded()) << static_cast<int>(step.status.error) << "@" << step.status.offset
                                << " phase=" << static_cast<int>(preparation.phase());
  }

  uint32_t decodeSlices = 0;
  while (preparation.phase() == PdfPreparationPhase::DecodeImages) {
    const PdfStepResult step = preparation.step();
    ++harness.nowMs;
    ASSERT_TRUE(step.yielded()) << static_cast<int>(step.status.error) << "@" << step.status.offset;
    ++decodeSlices;
  }

  EXPECT_GT(decodeSlices, 32U);
  EXPECT_LE(harness.storage.maximumReadRequest(), PdfLimits::SourceBufferBytes);
  const PdfStepResult terminal = runToTerminal(preparation, harness);
  ASSERT_TRUE(terminal.complete()) << static_cast<int>(terminal.status.error) << "@" << terminal.status.offset;
  EXPECT_EQ(harness.storage.openHandleCount(), 0U);
}

TEST(PdfImagePreparation, PreparationRasterBoundsControlMaskedPixelCacheDimensions) {
  PreparationHarness harness;
  constexpr char sourcePath[] = "/books/configured-raster-bounds.pdf";
  harness.storage.addFile(sourcePath, loadFixture("large_raster_caption.pdf"), 1234, true);
  PdfPreparationConfig config = harness.config(sourcePath);
  config.maximumRasterOutputWidth = 320;
  config.maximumRasterOutputHeight = 200;
  PdfPreparation preparation;
  ASSERT_TRUE(preparation.begin(config).ok());

  const PdfStepResult result = runToTerminal(preparation, harness);

  ASSERT_TRUE(result.complete()) << static_cast<int>(result.status.error) << "@" << result.status.offset;
  const std::string generationRoot =
      std::string(preparation.cacheRoot()) + "/gen_" + std::to_string(preparation.generation());
  const std::vector<std::string> paths = harness.storage.paths();
  const auto pixelCachePath = std::find_if(paths.begin(), paths.end(), [&](const std::string& path) {
    return path.starts_with(generationRoot + "/images/") && path.ends_with(".pxc");
  });
  ASSERT_NE(pixelCachePath, paths.end());
  const std::vector<uint8_t>& pixelCache = harness.storage.bytes(*pixelCachePath);
  ASSERT_GE(pixelCache.size(), pixel_cache::kHeaderSize);
  pixel_cache::Layout layout{};
  ASSERT_EQ(pixel_cache::decodeHeader(pixelCache.data(), pixelCache.size(), layout), pixel_cache::Status::Ok);
  EXPECT_EQ(layout.width, 320U);
  EXPECT_EQ(layout.height, 192U);
  EXPECT_EQ(harness.storage.openHandleCount(), 0U);
}

TEST(PdfImagePreparation, PreparationRejectsMissingRasterOutputBoundsBeforeOpeningSource) {
  PreparationHarness harness;
  const PdfPreparationConfig config{
      harness.storage.io(),
      "/books/missing-bounds.pdf",
      "/.crosspoint",
      &harness,
      PreparationHarness::now,
      {&harness, PreparationHarness::measure, nullptr},
      harness.storage.renameCallback(),
  };
  PdfPreparation preparation;

  const PdfStatus status = preparation.begin(config);

  ASSERT_FALSE(status.ok());
  EXPECT_EQ(status.error, PdfError::InvalidArgument);
  EXPECT_EQ(harness.storage.openHandleCount(), 0U);
  EXPECT_EQ(harness.storage.openCalls(), 0U);
}

TEST(PdfImagePreparation, LargeMaskedPreparationKeepsEveryPublicStepWithinCooperativeBudget) {
  PreparationHarness harness;
  harness.storage.addFile("/books/large-bounded.pdf", loadFixture("large_raster_caption.pdf"), 1234, true);
  harness.chargeIoTime = true;
  PdfPreparation preparation;
  ASSERT_TRUE(preparation.begin(harness.config("/books/large-bounded.pdf")).ok());

  for (uint32_t slice = 0; slice < 30000; ++slice) {
    const PdfPreparationPhase phaseBefore = preparation.phase();
    const PreparationStepObservation observation = observePreparationStep(preparation, harness);
    SCOPED_TRACE(::testing::Message() << "slice=" << slice << " phaseBefore=" << static_cast<int>(phaseBefore)
                                      << " phaseAfter=" << static_cast<int>(preparation.phase()));
    expectBoundedPreparationStep(observation);
    if (!observation.result.yielded()) {
      ASSERT_TRUE(observation.result.complete())
          << static_cast<int>(observation.result.status.error) << "@" << observation.result.status.offset
          << " phase=" << static_cast<int>(preparation.phase());
      EXPECT_EQ(harness.storage.openHandleCount(), 0U) << ::testing::PrintToString(harness.storage.openHandlePaths());
      return;
    }
    ++harness.nowMs;
  }

  FAIL() << "preparation did not complete within the slice limit";
}

TEST(PdfImagePreparation, LargeMaskCompositeYieldsAtThePublicPreparationStepBoundary) {
  PreparationHarness harness;
  harness.storage.addFile("/books/large-mask.pdf", loadFixture("large_raster_caption.pdf"), 1234, true);
  PdfPreparation preparation;
  ASSERT_TRUE(preparation.begin(harness.config("/books/large-mask.pdf")).ok());
  while (preparation.phase() != PdfPreparationPhase::DecodeImages) {
    const PdfStepResult step = preparation.step();
    ++harness.nowMs;
    ASSERT_TRUE(step.yielded()) << static_cast<int>(step.status.error) << "@" << step.status.offset;
  }

  bool compositeStarted = false;
  uint32_t compositeSlices = 0;
  while (preparation.phase() == PdfPreparationPhase::DecodeImages) {
    const PreparationStepObservation observation = observePreparationStep(preparation, harness);
    if (preparation.maskSpoolReadCount() != 0) {
      compositeStarted = true;
    }
    if (compositeStarted) {
      expectBoundedPreparationStep(observation);
      ++compositeSlices;
    }
    ASSERT_TRUE(observation.result.yielded())
        << static_cast<int>(observation.result.status.error) << "@" << observation.result.status.offset;
    ++harness.nowMs;
  }

  ASSERT_TRUE(compositeStarted);
  EXPECT_GT(compositeSlices, 1U);
  const PdfStepResult terminal = runToTerminal(preparation, harness);
  ASSERT_TRUE(terminal.complete()) << static_cast<int>(terminal.status.error) << "@" << terminal.status.offset;
  EXPECT_EQ(harness.storage.openHandleCount(), 0U);
}

TEST(PdfImagePreparation, LargeRasterPrehashYieldsAtThePublicPreparationStepBoundary) {
  PreparationHarness harness;
  harness.storage.addFile("/books/large-prehash.pdf", loadFixture("large_raster_caption.pdf"), 1234, true);
  PdfPreparation preparation;
  ASSERT_TRUE(preparation.begin(harness.config("/books/large-prehash.pdf")).ok());
  while (preparation.phase() != PdfPreparationPhase::CacheImage) {
    const PdfStepResult step = preparation.step();
    ++harness.nowMs;
    ASSERT_TRUE(step.yielded()) << static_cast<int>(step.status.error) << "@" << step.status.offset;
  }

  harness.chargeIoTime = true;
  uint32_t prehashSlices = 0;
  while (preparation.phase() == PdfPreparationPhase::CacheImage) {
    const PreparationStepObservation observation = observePreparationStep(preparation, harness);
    expectBoundedPreparationStep(observation);
    ASSERT_TRUE(observation.result.yielded())
        << static_cast<int>(observation.result.status.error) << "@" << observation.result.status.offset;
    ++harness.nowMs;
    ++prehashSlices;
  }

  EXPECT_GT(prehashSlices, 1U);
  const PdfStepResult terminal = runToTerminal(preparation, harness);
  ASSERT_TRUE(terminal.complete()) << static_cast<int>(terminal.status.error) << "@" << terminal.status.offset;
  EXPECT_EQ(harness.storage.openHandleCount(), 0U);
}

TEST(PdfImagePreparation, LargeJpegCopyYieldsAtThePublicPreparationStepBoundary) {
  PreparationHarness harness;
  harness.storage.addFile("/books/large-jpeg.pdf", loadFixture("jpeg_caption.pdf"), 1234, true);
  PdfPreparation preparation;
  ASSERT_TRUE(preparation.begin(harness.config("/books/large-jpeg.pdf")).ok());
  while (preparation.phase() != PdfPreparationPhase::CacheImage) {
    const PdfStepResult step = preparation.step();
    ++harness.nowMs;
    ASSERT_TRUE(step.yielded()) << static_cast<int>(step.status.error) << "@" << step.status.offset;
  }

  harness.chargeIoTime = true;
  uint32_t jpegSlices = 0;
  while (preparation.phase() == PdfPreparationPhase::CacheImage) {
    const PreparationStepObservation observation = observePreparationStep(preparation, harness);
    expectBoundedPreparationStep(observation);
    ASSERT_TRUE(observation.result.yielded())
        << static_cast<int>(observation.result.status.error) << "@" << observation.result.status.offset;
    ++harness.nowMs;
    ++jpegSlices;
  }

  EXPECT_GT(jpegSlices, 1U);
  const PdfStepResult terminal = runToTerminal(preparation, harness);
  ASSERT_TRUE(terminal.complete()) << static_cast<int>(terminal.status.error) << "@" << terminal.status.offset;
  EXPECT_EQ(harness.storage.openHandleCount(), 0U);
}

TEST(PdfImagePreparation, ResumeAfterEmitSectionsReusesTextAndContinuesDeferredImages) {
  constexpr char sourcePath[] = "/books/resume-sections.pdf";
  const std::vector<uint8_t> fixture = loadFixture("raster_cover_caption.pdf");

  PreparationHarness freshHarness;
  freshHarness.storage.setMaximumReadHandles(1);
  freshHarness.storage.addFile(sourcePath, fixture, 1234, true);
  PdfPreparation fresh;
  ASSERT_TRUE(fresh.begin(freshHarness.config(sourcePath)).ok());
  ASSERT_TRUE(runToTerminal(fresh, freshHarness).complete());
  const std::string freshRoot = std::string(fresh.cacheRoot()) + "/gen_" + std::to_string(fresh.generation());
  const std::vector<uint8_t> freshSection = freshHarness.storage.bytes(freshRoot + "/sections/000000.xhtml");
  const std::vector<uint8_t> freshMetadata = freshHarness.storage.bytes(freshRoot + "/metadata.bin");
  const std::vector<uint8_t> freshOutline = freshHarness.storage.bytes(freshRoot + "/outline.bin");
  const std::vector<uint8_t> freshCover = freshHarness.storage.bytes(freshRoot + "/cover.bmp");
  const PdfPreparationWorkCounters freshWork = fresh.workCounters();

  PreparationHarness harness;
  harness.storage.setMaximumReadHandles(1);
  harness.storage.addFile(sourcePath, fixture, 1234, true);
  PdfPreparation preparation;
  ASSERT_TRUE(preparation.begin(harness.config(sourcePath)).ok());
  for (uint32_t slice = 0; slice < 20000 && preparation.phase() != PdfPreparationPhase::SpoolNavigation; ++slice) {
    const PdfStepResult step = preparation.step();
    ++harness.nowMs;
    ASSERT_TRUE(step.yielded()) << static_cast<int>(step.status.error) << "@" << step.status.offset;
  }
  ASSERT_EQ(preparation.phase(), PdfPreparationPhase::SpoolNavigation);
  const uint32_t generation = preparation.generation();
  const std::string generationRoot = std::string(preparation.cacheRoot()) + "/gen_" + std::to_string(generation);
  const std::string sectionPath = generationRoot + "/sections/000000.xhtml";
  const std::string deferredPath = generationRoot + "/build.images";
  ASSERT_TRUE(harness.storage.exists(sectionPath));
  ASSERT_TRUE(harness.storage.exists(deferredPath));
  const std::vector<uint8_t> sectionBytes = harness.storage.bytes(sectionPath);
  const std::vector<uint8_t> deferredBytes = harness.storage.bytes(deferredPath);

  const PdfStepResult cancelled = cancelToTerminalBounded(preparation, harness);
  ASSERT_TRUE(cancelled.failed());
  ASSERT_EQ(cancelled.status.error, PdfError::Cancelled);
  EXPECT_EQ(preparation.durableResumePhase(), PdfBuildResumePhase::AfterEmitSections);
  PdfCacheStore cache;
  ASSERT_TRUE(cache.initialize(harness.storage.io(), preparation.cacheRoot()).ok());
  PdfBuildCheckpointSelection checkpoints{};
  ASSERT_TRUE(cache.loadCheckpointSlots(preparation.sourceIdentity(), &checkpoints).ok());
  ASSERT_TRUE(checkpoints.selected);
  EXPECT_EQ(checkpoints.checkpoint.resumePhase, PdfBuildResumePhase::AfterEmitSections);
  EXPECT_EQ(checkpoints.checkpoint.generation, generation);
  EXPECT_EQ(checkpoints.checkpoint.lastVerifiedPage, 1U);
  EXPECT_EQ(checkpoints.checkpoint.emittedSections, 1U);
  ASSERT_TRUE(harness.storage.exists(deferredPath));
  EXPECT_EQ(harness.storage.bytes(deferredPath), deferredBytes);
  const PreparationHarness cancelledBaseline = harness;

  harness.storage.clearOpenObservations();
  harness.storage.clearRemoveObservations();
  harness.storage.clearReadObservations();
  PdfPreparation resumed;
  ASSERT_TRUE(resumed.begin(harness.config(sourcePath)).ok());
  const PdfStepResult result = runToTerminal(resumed, harness);
  ASSERT_TRUE(result.complete()) << static_cast<int>(result.status.error) << "@" << result.status.offset
                                 << " phase=" << static_cast<int>(resumed.phase());
  EXPECT_TRUE(resumed.resumedFromCheckpoint());
  EXPECT_EQ(resumed.resumedPhase(), PdfBuildResumePhase::AfterEmitSections);
  EXPECT_EQ(resumed.generation(), generation);
  EXPECT_EQ(harness.storage.bytes(sectionPath), sectionBytes);
  EXPECT_EQ(harness.storage.bytes(sectionPath), freshSection);
  EXPECT_EQ(harness.storage.bytes(generationRoot + "/metadata.bin"), freshMetadata);
  EXPECT_EQ(harness.storage.bytes(generationRoot + "/outline.bin"), freshOutline);
  EXPECT_EQ(harness.storage.bytes(generationRoot + "/cover.bmp"), freshCover);
  for (const char* leaf : {"build.images", "build.image-files", "build.image-files.resume", "build.nav", "build.mask",
                           "resume.sections", "resume.a", "resume.b"}) {
    EXPECT_FALSE(harness.storage.exists(generationRoot + "/" + leaf)) << leaf;
  }
  EXPECT_EQ(writeTruncateOpensForPath(harness.storage.openObservations(), sectionPath), 0U);
  EXPECT_EQ(
      std::count(harness.storage.removeObservations().begin(), harness.storage.removeObservations().end(), sectionPath),
      0);
  EXPECT_LT(observedPathBytes(harness.storage.readObservations(), sourcePath),
            observedPathBytes(freshHarness.storage.readObservations(), sourcePath));
  EXPECT_LT(resumed.workCounters().xrefSteps, freshWork.xrefSteps);
  EXPECT_LT(resumed.workCounters().pagesWalked, freshWork.pagesWalked);
  EXPECT_LT(resumed.workCounters().contentTokens, freshWork.contentTokens);
  EXPECT_LT(resumed.workCounters().sectionsEmitted, freshWork.sectionsEmitted);
  EXPECT_EQ(harness.storage.openHandleCount(), 0U);

  enum class Mutation : uint8_t {
    ControlCrc,
    MissingImageSpool,
    TruncatedImageSpool,
  };
  for (const Mutation mutation : {Mutation::ControlCrc, Mutation::MissingImageSpool, Mutation::TruncatedImageSpool}) {
    SCOPED_TRACE(static_cast<int>(mutation));
    PreparationHarness rejected = cancelledBaseline;
    const std::string controlPath = generationRoot + "/resume.sections";
    if (mutation == Mutation::ControlCrc) {
      std::vector<uint8_t> control = rejected.storage.bytes(controlPath);
      ASSERT_EQ(control.size(), 256U);
      control.back() ^= 0x01U;
      rejected.storage.addFile(controlPath, control);
    } else if (mutation == Mutation::MissingImageSpool) {
      PdfCacheIo io = rejected.storage.io();
      ASSERT_TRUE(io.remove(io.context, deferredPath.c_str(), false).ok());
    } else {
      ASSERT_GT(rejected.storage.bytes(deferredPath).size(), 1U);
      rejected.storage.truncateFile(deferredPath, rejected.storage.bytes(deferredPath).size() - 1U);
    }
    rejected.storage.clearOpenObservations();
    rejected.storage.clearRemoveObservations();
    PdfPreparation freshRestart;
    ASSERT_TRUE(freshRestart.begin(rejected.config(sourcePath)).ok());
    const PdfStepResult freshResult = runToTerminal(freshRestart, rejected);
    ASSERT_TRUE(freshResult.complete()) << static_cast<int>(freshResult.status.error) << "@"
                                        << freshResult.status.offset;
    EXPECT_FALSE(freshRestart.resumedFromCheckpoint());
    EXPECT_NE(freshRestart.generation(), generation);
    EXPECT_EQ(writeTruncateOpensForPath(rejected.storage.openObservations(), sectionPath), 0U);
    EXPECT_EQ(std::count(rejected.storage.removeObservations().begin(), rejected.storage.removeObservations().end(),
                         sectionPath),
              0);
    EXPECT_EQ(rejected.storage.openHandleCount(), 0U);
  }
}

TEST(PdfImagePreparation, ResumeAfterOneDurableImageSkipsItAndContinuesLaterDeferredImages) {
  constexpr char sourcePath[] = "/books/resume-after-image.pdf";
  const std::vector<uint8_t> fixture = loadFixture("three_figures_one_page.pdf");

  PreparationHarness freshHarness;
  freshHarness.storage.setMaximumReadHandles(1);
  freshHarness.storage.addFile(sourcePath, fixture, 1234, true);
  PdfPreparation fresh;
  ASSERT_TRUE(fresh.begin(freshHarness.config(sourcePath)).ok());
  ASSERT_TRUE(runToTerminal(fresh, freshHarness).complete());
  const PdfPreparationWorkCounters freshWork = fresh.workCounters();

  PreparationHarness harness;
  harness.storage.setMaximumReadHandles(1);
  harness.storage.addFile(sourcePath, fixture, 1234, true);
  PdfPreparation interrupted;
  ASSERT_TRUE(interrupted.begin(harness.config(sourcePath)).ok());
  for (uint32_t slice = 0; slice < 30000 && (interrupted.phase() != PdfPreparationPhase::DecodeImages ||
                                             interrupted.workCounters().imagesEmitted != 1U);
       ++slice) {
    const PdfStepResult step = interrupted.step();
    ++harness.nowMs;
    ASSERT_TRUE(step.yielded()) << static_cast<int>(step.status.error) << "@" << step.status.offset
                                << " phase=" << static_cast<int>(interrupted.phase());
  }
  ASSERT_EQ(interrupted.phase(), PdfPreparationPhase::DecodeImages);
  ASSERT_EQ(interrupted.workCounters().imagesEmitted, 1U);
  const uint32_t generation = interrupted.generation();
  const std::string generationRoot = std::string(interrupted.cacheRoot()) + "/gen_" + std::to_string(generation);
  const std::vector<std::string> boundaryPaths = harness.storage.paths();
  const auto firstImage = std::find_if(boundaryPaths.begin(), boundaryPaths.end(), [&](const std::string& path) {
    return path.starts_with(generationRoot + "/images/") && path.ends_with(".pxc");
  });
  ASSERT_NE(firstImage, boundaryPaths.end());
  const std::string firstImagePath = *firstImage;
  const std::vector<uint8_t> firstImageBytes = harness.storage.bytes(firstImagePath);
  ASSERT_FALSE(firstImageBytes.empty());

  const PdfStepResult cancelled = cancelToTerminalBounded(interrupted, harness);
  ASSERT_TRUE(cancelled.failed());
  ASSERT_EQ(cancelled.status.error, PdfError::Cancelled);
  EXPECT_EQ(interrupted.durableResumePhase(), PdfBuildResumePhase::AfterImage);
  PdfCacheStore cache;
  ASSERT_TRUE(cache.initialize(harness.storage.io(), interrupted.cacheRoot()).ok());
  PdfBuildCheckpointSelection checkpoints{};
  ASSERT_TRUE(cache.loadCheckpointSlots(interrupted.sourceIdentity(), &checkpoints).ok());
  ASSERT_TRUE(checkpoints.selected);
  EXPECT_EQ(checkpoints.checkpoint.phase, PdfBuildPhase::Cancelled);
  EXPECT_EQ(checkpoints.checkpoint.resumePhase, PdfBuildResumePhase::AfterImage);
  EXPECT_EQ(checkpoints.checkpoint.emittedImages, 1U);
  EXPECT_GT(checkpoints.checkpoint.journalBytes, 0U);
  const PreparationHarness cancelledBaseline = harness;

  harness.storage.clearOpenObservations();
  harness.storage.clearRemoveObservations();
  PdfPreparation resumed;
  ASSERT_TRUE(resumed.begin(harness.config(sourcePath)).ok());
  const PdfStepResult result = runToTerminal(resumed, harness);
  ASSERT_TRUE(result.complete()) << static_cast<int>(result.status.error) << "@" << result.status.offset
                                 << " phase=" << static_cast<int>(resumed.phase());
  EXPECT_TRUE(resumed.resumedFromCheckpoint());
  EXPECT_EQ(resumed.resumedPhase(), PdfBuildResumePhase::AfterImage);
  EXPECT_EQ(resumed.generation(), generation);
  EXPECT_EQ(harness.storage.bytes(firstImagePath), firstImageBytes);
  EXPECT_EQ(writeTruncateOpensForPath(harness.storage.openObservations(), firstImagePath), 0U);
  EXPECT_EQ(std::count(harness.storage.removeObservations().begin(), harness.storage.removeObservations().end(),
                       firstImagePath),
            0);
  EXPECT_LT(resumed.workCounters().imagesEmitted, freshWork.imagesEmitted);
  EXPECT_LT(resumed.workCounters().contentTokens, freshWork.contentTokens);
  EXPECT_LT(resumed.workCounters().sourceBytesRead, freshWork.sourceBytesRead);
  EXPECT_EQ(harness.storage.openHandleCount(), 0U);

  enum class Mutation : uint8_t {
    CursorRewind,
    MissingControl,
  };
  for (const Mutation mutation : {Mutation::CursorRewind, Mutation::MissingControl}) {
    SCOPED_TRACE(static_cast<int>(mutation));
    PreparationHarness rejected = cancelledBaseline;
    const std::string controlPath = generationRoot + "/resume.sections";
    if (mutation == Mutation::CursorRewind) {
      std::vector<uint8_t> control = rejected.storage.bytes(controlPath);
      ASSERT_EQ(control.size(), 256U);
      ASSERT_EQ(control[212], 1U);
      control[212] = 0;
      resealTrailingCrc(&control);
      rejected.storage.addFile(controlPath, control);
    } else {
      PdfCacheIo io = rejected.storage.io();
      ASSERT_TRUE(io.remove(io.context, controlPath.c_str(), false).ok());
    }
    rejected.storage.clearOpenObservations();
    rejected.storage.clearRemoveObservations();
    PdfPreparation freshRestart;
    ASSERT_TRUE(freshRestart.begin(rejected.config(sourcePath)).ok());
    const PdfStepResult freshResult = runToTerminal(freshRestart, rejected);
    ASSERT_TRUE(freshResult.complete()) << static_cast<int>(freshResult.status.error) << "@"
                                        << freshResult.status.offset;
    EXPECT_FALSE(freshRestart.resumedFromCheckpoint());
    EXPECT_NE(freshRestart.generation(), generation);
    EXPECT_EQ(writeTruncateOpensForPath(rejected.storage.openObservations(), firstImagePath), 0U);
    EXPECT_EQ(std::count(rejected.storage.removeObservations().begin(), rejected.storage.removeObservations().end(),
                         firstImagePath),
              0);
    EXPECT_EQ(rejected.storage.openHandleCount(), 0U);
  }
}

TEST(PdfImagePreparation, ResumeAfterVerifiedSectionAndImageReusesDurableArtifactsAndDoesLessSourceIo) {
  constexpr char sourcePath[] = "/books/resume-finalize.pdf";
  const std::vector<uint8_t> fixture = loadFixture("jpeg_caption.pdf");
  PreparationHarness harness;
  harness.storage.setMaximumReadHandles(1);
  harness.storage.addFile(sourcePath, fixture, 1234, true);
  PdfPreparation preparation;
  ASSERT_TRUE(preparation.begin(harness.config(sourcePath)).ok());
  for (uint32_t slice = 0; slice < 20000 && (preparation.phase() != PdfPreparationPhase::CloseSource ||
                                             preparation.workCounters().imagesEmitted == 0);
       ++slice) {
    const PdfStepResult step = preparation.step();
    ++harness.nowMs;
    ASSERT_TRUE(step.yielded()) << static_cast<int>(step.status.error) << "@" << step.status.offset;
  }
  ASSERT_EQ(preparation.phase(), PdfPreparationPhase::CloseSource);
  ASSERT_GT(preparation.workCounters().imagesEmitted, 0U);

  const uint32_t generation = preparation.generation();
  const std::string generationRoot = std::string(preparation.cacheRoot()) + "/gen_" + std::to_string(generation);
  const std::string sectionPath = generationRoot + "/sections/000000.xhtml";
  const std::vector<std::string> durablePaths = harness.storage.paths();
  const auto image = std::find_if(durablePaths.begin(), durablePaths.end(), [&](const std::string& path) {
    return path.starts_with(generationRoot + "/images/") && path.ends_with(".jpg");
  });
  ASSERT_TRUE(harness.storage.exists(sectionPath));
  ASSERT_NE(image, durablePaths.end());
  const std::string imagePath = *image;
  const std::vector<uint8_t> sectionBytes = harness.storage.bytes(sectionPath);
  const std::vector<uint8_t> imageBytes = harness.storage.bytes(imagePath);
  ASSERT_FALSE(sectionBytes.empty());
  ASSERT_FALSE(imageBytes.empty());

  const PdfStepResult cancelled = cancelToTerminalBounded(preparation, harness);
  ASSERT_TRUE(cancelled.failed());
  ASSERT_EQ(cancelled.status.error, PdfError::Cancelled);
  PdfCacheStore cache;
  ASSERT_TRUE(cache.initialize(harness.storage.io(), preparation.cacheRoot()).ok());
  PdfBuildCheckpointSelection checkpoints{};
  ASSERT_TRUE(cache.loadCheckpointSlots(preparation.sourceIdentity(), &checkpoints).ok());
  ASSERT_TRUE(checkpoints.selected);
  EXPECT_EQ(checkpoints.checkpoint.phase, PdfBuildPhase::Cancelled);
  EXPECT_EQ(checkpoints.checkpoint.resumePhase, PdfBuildResumePhase::AfterImageRepair);
  EXPECT_GT(checkpoints.checkpoint.journalBytes, 0U);
  EXPECT_EQ(checkpoints.checkpoint.generation, generation);
  EXPECT_GE(checkpoints.checkpoint.lastVerifiedPage, 1U);
  EXPECT_GE(checkpoints.checkpoint.emittedSections, 1U);
  EXPECT_GE(checkpoints.checkpoint.emittedImages, 1U);
  const PreparationHarness cancelledBaseline = harness;

  harness.storage.clearOpenObservations();
  harness.storage.clearRemoveObservations();
  harness.storage.clearReadObservations();
  PdfPreparation resumed;
  ASSERT_TRUE(resumed.begin(harness.config(sourcePath)).ok());
  const PdfStepResult resumedResult = runToTerminal(resumed, harness);
  ASSERT_TRUE(resumedResult.complete()) << static_cast<int>(resumedResult.status.error) << "@"
                                        << resumedResult.status.offset
                                        << " phase=" << static_cast<int>(resumed.phase());
  EXPECT_TRUE(resumed.resumedFromCheckpoint());
  EXPECT_EQ(resumed.resumedPhase(), PdfBuildResumePhase::AfterImageRepair);
  EXPECT_EQ(resumed.generation(), generation);
  EXPECT_EQ(harness.storage.bytes(sectionPath), sectionBytes);
  EXPECT_EQ(harness.storage.bytes(imagePath), imageBytes);
  EXPECT_EQ(writeTruncateOpensForPath(harness.storage.openObservations(), sectionPath), 0U);
  EXPECT_EQ(writeTruncateOpensForPath(harness.storage.openObservations(), imagePath), 0U);
  EXPECT_EQ(
      std::count(harness.storage.removeObservations().begin(), harness.storage.removeObservations().end(), sectionPath),
      0);
  EXPECT_EQ(
      std::count(harness.storage.removeObservations().begin(), harness.storage.removeObservations().end(), imagePath),
      0);
  const uint64_t resumedSourceBytes = observedPathBytes(harness.storage.readObservations(), sourcePath);
  const PdfPreparationWorkCounters resumedWork = resumed.workCounters();

  PreparationHarness freshHarness;
  freshHarness.storage.setMaximumReadHandles(1);
  freshHarness.storage.addFile(sourcePath, fixture, 1234, true);
  PdfPreparation fresh;
  ASSERT_TRUE(fresh.begin(freshHarness.config(sourcePath)).ok());
  ASSERT_TRUE(runToTerminal(fresh, freshHarness).complete());
  const uint64_t freshSourceBytes = observedPathBytes(freshHarness.storage.readObservations(), sourcePath);
  const PdfPreparationWorkCounters freshWork = fresh.workCounters();
  EXPECT_LT(resumedSourceBytes, freshSourceBytes);
  EXPECT_LT(resumedWork.xrefSteps, freshWork.xrefSteps);
  EXPECT_LT(resumedWork.pagesWalked, freshWork.pagesWalked);
  EXPECT_LT(resumedWork.contentTokens, freshWork.contentTokens);
  EXPECT_LT(resumedWork.sectionsEmitted, freshWork.sectionsEmitted);
  EXPECT_LT(resumedWork.imagesEmitted, freshWork.imagesEmitted);
  EXPECT_LT(resumedWork.sourceBytesRead, freshWork.sourceBytesRead);
  EXPECT_EQ(harness.storage.openHandleCount(), 0U);

  enum class Mutation : uint8_t {
    InvalidCursor,
    ControlCrc,
    MissingControl,
  };
  for (const Mutation mutation : {Mutation::InvalidCursor, Mutation::ControlCrc, Mutation::MissingControl}) {
    SCOPED_TRACE(static_cast<int>(mutation));
    PreparationHarness rejected = cancelledBaseline;
    const std::string controlPath = generationRoot + "/resume.sections";
    if (mutation == Mutation::MissingControl) {
      PdfCacheIo io = rejected.storage.io();
      ASSERT_TRUE(io.remove(io.context, controlPath.c_str(), false).ok());
    } else {
      std::vector<uint8_t> control = rejected.storage.bytes(controlPath);
      ASSERT_EQ(control.size(), 256U);
      ASSERT_EQ(control[212], 0U);
      if (mutation == Mutation::InvalidCursor) {
        control[212] = 1U;
        resealTrailingCrc(&control);
      } else {
        control.back() ^= 0x01U;
      }
      rejected.storage.addFile(controlPath, control);
    }
    rejected.storage.clearOpenObservations();
    rejected.storage.clearRemoveObservations();
    PdfPreparation freshRestart;
    ASSERT_TRUE(freshRestart.begin(rejected.config(sourcePath)).ok());
    const PdfStepResult freshResult = runToTerminal(freshRestart, rejected);
    ASSERT_TRUE(freshResult.complete()) << static_cast<int>(freshResult.status.error) << "@"
                                        << freshResult.status.offset;
    EXPECT_FALSE(freshRestart.resumedFromCheckpoint());
    EXPECT_NE(freshRestart.generation(), generation);
    EXPECT_EQ(writeTruncateOpensForPath(rejected.storage.openObservations(), sectionPath), 0U);
    EXPECT_EQ(writeTruncateOpensForPath(rejected.storage.openObservations(), imagePath), 0U);
    EXPECT_EQ(std::count(rejected.storage.removeObservations().begin(), rejected.storage.removeObservations().end(),
                         sectionPath),
              0);
    EXPECT_EQ(std::count(rejected.storage.removeObservations().begin(), rejected.storage.removeObservations().end(),
                         imagePath),
              0);
    EXPECT_EQ(rejected.storage.openHandleCount(), 0U);
  }
}

TEST(PdfImagePreparation, ResumeAfterRasterRepairCanonicalizesUnusedDeferredImageMappings) {
  constexpr char sourcePath[] = "/books/resume-raster-finalize.pdf";
  PreparationHarness harness;
  harness.storage.setMaximumReadHandles(1);
  harness.storage.addFile(sourcePath, loadFixture("raster_cover_caption.pdf"), 1234, true);

  PdfPreparation preparation;
  ASSERT_TRUE(preparation.begin(harness.config(sourcePath)).ok());
  for (uint32_t slice = 0; slice < 20000 && (preparation.phase() != PdfPreparationPhase::CloseSource ||
                                             preparation.workCounters().imagesEmitted == 0);
       ++slice) {
    const PdfStepResult step = preparation.step();
    ++harness.nowMs;
    ASSERT_TRUE(step.yielded()) << static_cast<int>(step.status.error) << "@" << step.status.offset;
  }
  ASSERT_EQ(preparation.phase(), PdfPreparationPhase::CloseSource);
  ASSERT_GT(preparation.workCounters().imagesEmitted, 0U);

  const uint32_t generation = preparation.generation();
  const PdfStepResult cancelled = cancelToTerminalBounded(preparation, harness);
  ASSERT_TRUE(cancelled.failed());
  ASSERT_EQ(cancelled.status.error, PdfError::Cancelled);
  ASSERT_EQ(preparation.durableResumePhase(), PdfBuildResumePhase::AfterImageRepair);

  const std::string controlPath =
      std::string(preparation.cacheRoot()) + "/gen_" + std::to_string(generation) + "/resume.sections";
  const std::vector<uint8_t> control = harness.storage.bytes(controlPath);
  ASSERT_EQ(control.size(), 256U);
  ASSERT_EQ(control[18], 0U);
  for (size_t index = 0; index < 64U; ++index) {
    EXPECT_EQ(control[148U + index], UINT8_MAX) << "unused canonical mapping index " << index;
  }

  PdfPreparation resumed;
  ASSERT_TRUE(resumed.begin(harness.config(sourcePath)).ok());
  const PdfStepResult resumedResult = runToTerminal(resumed, harness);
  ASSERT_TRUE(resumedResult.complete()) << static_cast<int>(resumedResult.status.error) << "@"
                                        << resumedResult.status.offset;
  EXPECT_TRUE(resumed.resumedFromCheckpoint());
  EXPECT_EQ(resumed.resumedPhase(), PdfBuildResumePhase::AfterImageRepair);
  EXPECT_EQ(resumed.generation(), generation);
}

TEST(PdfImagePreparation, ResumeStateMutationsFailClosedIntoAFreshGeneration) {
  constexpr char sourcePath[] = "/books/resume-mutations.pdf";
  PreparationHarness baseline;
  baseline.storage.setMaximumReadHandles(1);
  baseline.storage.addFile(sourcePath, loadFixture("jpeg_caption.pdf"), 1234, true);
  PdfPreparation preparation;
  ASSERT_TRUE(preparation.begin(baseline.config(sourcePath)).ok());
  while (preparation.phase() != PdfPreparationPhase::CommitManifest) {
    const PdfStepResult step = preparation.step();
    ++baseline.nowMs;
    ASSERT_TRUE(step.yielded()) << static_cast<int>(step.status.error) << "@" << step.status.offset;
  }
  const PdfStepResult cancelled = cancelToTerminalBounded(preparation, baseline);
  ASSERT_TRUE(cancelled.failed());
  ASSERT_EQ(cancelled.status.error, PdfError::Cancelled);
  EXPECT_EQ(preparation.durableResumePhase(), PdfBuildResumePhase::CommitManifest);

  PdfCacheStore cache;
  ASSERT_TRUE(cache.initialize(baseline.storage.io(), preparation.cacheRoot()).ok());
  PdfBuildCheckpointSelection checkpoints{};
  ASSERT_TRUE(cache.loadCheckpointSlots(preparation.sourceIdentity(), &checkpoints).ok());
  ASSERT_TRUE(checkpoints.selected);
  ASSERT_EQ(checkpoints.checkpoint.resumePhase, PdfBuildResumePhase::CommitManifest);
  const uint32_t cancelledGeneration = checkpoints.checkpoint.generation;
  const std::string cacheRoot = preparation.cacheRoot();
  const std::string generationRoot = cacheRoot + "/gen_" + std::to_string(cancelledGeneration);
  const std::string checkpointPath =
      cacheRoot + (checkpoints.selectedSlot == PdfCacheSlot::A ? "/build.a" : "/build.b");
  const char resumeSlot = (checkpoints.checkpoint.sequence & 1U) != 0 ? 'a' : 'b';
  const std::string resumePath = generationRoot + "/resume." + resumeSlot;
  const std::string sectionPath = generationRoot + "/sections/000000.xhtml";
  const std::vector<std::string> paths = baseline.storage.paths();
  const auto image = std::find_if(paths.begin(), paths.end(), [&](const std::string& path) {
    return path.starts_with(generationRoot + "/images/") && path.ends_with(".jpg");
  });
  ASSERT_NE(image, paths.end());
  const std::string imagePath = *image;
  ASSERT_TRUE(baseline.storage.exists(checkpointPath));
  ASSERT_TRUE(baseline.storage.exists(resumePath));

  PreparationHarness accepted = baseline;
  accepted.storage.clearOpenObservations();
  accepted.storage.clearRemoveObservations();
  PdfPreparation resumedCommit;
  ASSERT_TRUE(resumedCommit.begin(accepted.config(sourcePath)).ok());
  const PdfStepResult resumedCommitResult = runToTerminal(resumedCommit, accepted);
  ASSERT_TRUE(resumedCommitResult.complete())
      << static_cast<int>(resumedCommitResult.status.error) << "@" << resumedCommitResult.status.offset;
  EXPECT_TRUE(resumedCommit.resumedFromCheckpoint());
  EXPECT_EQ(resumedCommit.resumedPhase(), PdfBuildResumePhase::CommitManifest);
  EXPECT_EQ(resumedCommit.generation(), cancelledGeneration);
  EXPECT_EQ(writeTruncateOpensForPath(accepted.storage.openObservations(), sectionPath), 0U);
  EXPECT_EQ(writeTruncateOpensForPath(accepted.storage.openObservations(), imagePath), 0U);
  EXPECT_EQ(std::count(accepted.storage.removeObservations().begin(), accepted.storage.removeObservations().end(),
                       sectionPath),
            0);
  EXPECT_EQ(
      std::count(accepted.storage.removeObservations().begin(), accepted.storage.removeObservations().end(), imagePath),
      0);
  EXPECT_LT(resumedCommit.workCounters().xrefSteps, preparation.workCounters().xrefSteps);
  EXPECT_LT(resumedCommit.workCounters().pagesWalked, preparation.workCounters().pagesWalked);
  EXPECT_LT(resumedCommit.workCounters().contentTokens, preparation.workCounters().contentTokens);
  EXPECT_LT(resumedCommit.workCounters().sectionsEmitted, preparation.workCounters().sectionsEmitted);
  EXPECT_LT(resumedCommit.workCounters().imagesEmitted, preparation.workCounters().imagesEmitted);
  EXPECT_EQ(accepted.storage.openHandleCount(), 0U);

  enum class Mutation : uint8_t {
    ZeroPageCursor,
    CheckpointCount,
    UnderlyingPhase,
    DeleteSection,
    TruncateImage,
    LedgerRecordOffset,
    LedgerCrc,
  };
  constexpr Mutation mutations[] = {
      Mutation::ZeroPageCursor, Mutation::CheckpointCount,    Mutation::UnderlyingPhase, Mutation::DeleteSection,
      Mutation::TruncateImage,  Mutation::LedgerRecordOffset, Mutation::LedgerCrc,
  };
  for (const Mutation mutation : mutations) {
    SCOPED_TRACE(static_cast<int>(mutation));
    PreparationHarness harness = baseline;
    harness.storage.clearOpenObservations();
    harness.storage.clearRemoveObservations();
    if (mutation == Mutation::ZeroPageCursor || mutation == Mutation::CheckpointCount ||
        mutation == Mutation::UnderlyingPhase) {
      std::vector<uint8_t> checkpoint = harness.storage.bytes(checkpointPath);
      ASSERT_EQ(checkpoint.size(), 96U);
      if (mutation == Mutation::ZeroPageCursor) {
        writeLittleEndian32(&checkpoint, 56, 0);
      } else if (mutation == Mutation::CheckpointCount) {
        writeLittleEndian32(&checkpoint, 64, readLittleEndian32(checkpoint, 64) + 1U);
      } else {
        checkpoint[53] = static_cast<uint8_t>(PdfBuildResumePhase::None);
      }
      resealTrailingCrc(&checkpoint);
      harness.storage.addFile(checkpointPath, checkpoint);
    } else if (mutation == Mutation::DeleteSection) {
      PdfCacheIo io = harness.storage.io();
      ASSERT_TRUE(io.remove(io.context, sectionPath.c_str(), false).ok());
      harness.storage.clearRemoveObservations();
    } else if (mutation == Mutation::TruncateImage) {
      ASSERT_GT(harness.storage.bytes(imagePath).size(), 1U);
      harness.storage.truncateFile(imagePath, harness.storage.bytes(imagePath).size() - 1U);
    } else {
      std::vector<uint8_t> ledger = harness.storage.bytes(resumePath);
      ASSERT_GT(ledger.size(), 100U);
      if (mutation == Mutation::LedgerRecordOffset) {
        writeLittleEndian32(&ledger, 84U + 4U, readLittleEndian32(ledger, 84U + 4U) + 1U);
        resealTrailingCrc(&ledger);
      } else {
        ledger.back() ^= 0x01U;
      }
      harness.storage.addFile(resumePath, ledger);
    }

    PdfPreparation restarted;
    ASSERT_TRUE(restarted.begin(harness.config(sourcePath)).ok());
    const PdfStepResult result = runToTerminal(restarted, harness);
    ASSERT_TRUE(result.complete()) << static_cast<int>(result.status.error) << "@" << result.status.offset
                                   << " phase=" << static_cast<int>(restarted.phase());
    EXPECT_FALSE(restarted.resumedFromCheckpoint());
    EXPECT_NE(restarted.generation(), cancelledGeneration);
    EXPECT_EQ(writeTruncateOpensForPath(harness.storage.openObservations(), sectionPath), 0U);
    EXPECT_EQ(writeTruncateOpensForPath(harness.storage.openObservations(), imagePath), 0U);
    EXPECT_EQ(harness.storage.openHandleCount(), 0U);
  }
}

TEST(PdfImagePreparation, CancellationDuringLargeRasterPrehashRetainsAResumableGeneration) {
  PreparationHarness harness;
  harness.storage.setMaximumReadHandles(1);
  harness.storage.addFile("/books/cancel-prehash.pdf", loadFixture("large_raster_caption.pdf"), 1234, true);
  PdfPreparation first;
  ASSERT_TRUE(first.begin(harness.config("/books/cancel-prehash.pdf")).ok());
  ASSERT_TRUE(runToTerminal(first, harness).complete());

  PdfCacheStore cache;
  ASSERT_TRUE(cache.initialize(harness.storage.io(), first.cacheRoot()).ok());
  PdfCacheManifestSelection before{};
  ASSERT_TRUE(cache.loadManifestSlots(first.sourceIdentity(), &before).ok());
  ASSERT_TRUE(before.selected);
  const uint32_t activeGeneration = before.manifest.generation;
  const std::string activeRoot = std::string(first.cacheRoot()) + "/gen_" + std::to_string(activeGeneration);
  const auto activePaths = harness.storage.paths();
  const auto activeImage = std::find_if(activePaths.begin(), activePaths.end(), [&](const std::string& path) {
    return path.starts_with(activeRoot + "/images/") && path.ends_with(".pxc");
  });
  ASSERT_NE(activeImage, activePaths.end());
  const std::string activeImagePath = *activeImage;
  const std::vector<uint8_t> activeImageBytes = harness.storage.bytes(activeImagePath);

  PdfPreparation rebuild;
  ASSERT_TRUE(rebuild.begin(harness.config("/books/cancel-prehash.pdf")).ok());
  while (rebuild.phase() != PdfPreparationPhase::CacheImage) {
    const PdfStepResult step = rebuild.step();
    ++harness.nowMs;
    ASSERT_TRUE(step.yielded());
  }
  ASSERT_NE(rebuild.generation(), activeGeneration);
  harness.chargeIoTime = true;
  const PreparationStepObservation prehash = observePreparationStep(rebuild, harness);
  expectBoundedPreparationStep(prehash);
  ASSERT_TRUE(prehash.result.yielded());
  ASSERT_EQ(rebuild.phase(), PdfPreparationPhase::CacheImage);

  const uint32_t cancelledGeneration = rebuild.generation();
  const PdfStepResult cancelled = cancelToTerminalBounded(rebuild, harness);

  ASSERT_TRUE(cancelled.failed());
  EXPECT_EQ(cancelled.status.error, PdfError::Cancelled);
  PdfCacheManifestSelection after{};
  ASSERT_TRUE(cache.loadManifestSlots(first.sourceIdentity(), &after).ok());
  ASSERT_TRUE(after.selected);
  EXPECT_EQ(after.manifest.generation, activeGeneration);
  ASSERT_TRUE(harness.storage.exists(activeImagePath));
  EXPECT_EQ(harness.storage.bytes(activeImagePath), activeImageBytes);
  expectRetainedCancelledGeneration(harness.storage, rebuild.cacheRoot(), rebuild.sourceIdentity(),
                                    cancelledGeneration);
  EXPECT_EQ(harness.storage.openHandleCount(), 0U);
  expectFreshGenerationRestart(harness, "/books/cancel-prehash.pdf", cancelledGeneration);
}

TEST(PdfImagePreparation, CancellationDuringLargeJpegCopyRemovesThePartialAndRetainsAResumableGeneration) {
  PreparationHarness harness;
  harness.storage.setMaximumReadHandles(1);
  harness.storage.addFile("/books/cancel-jpeg.pdf", loadFixture("jpeg_caption.pdf"), 1234, true);
  PdfPreparation first;
  ASSERT_TRUE(first.begin(harness.config("/books/cancel-jpeg.pdf")).ok());
  ASSERT_TRUE(runToTerminal(first, harness).complete());

  PdfCacheStore cache;
  ASSERT_TRUE(cache.initialize(harness.storage.io(), first.cacheRoot()).ok());
  PdfCacheManifestSelection before{};
  ASSERT_TRUE(cache.loadManifestSlots(first.sourceIdentity(), &before).ok());
  ASSERT_TRUE(before.selected);
  const uint32_t activeGeneration = before.manifest.generation;
  const std::string activeRoot = std::string(first.cacheRoot()) + "/gen_" + std::to_string(activeGeneration);
  const auto activePaths = harness.storage.paths();
  const auto activeImage = std::find_if(activePaths.begin(), activePaths.end(), [&](const std::string& path) {
    return path.starts_with(activeRoot + "/images/") && path.ends_with(".jpg");
  });
  ASSERT_NE(activeImage, activePaths.end());
  const std::string activeImagePath = *activeImage;
  const std::vector<uint8_t> activeImageBytes = harness.storage.bytes(activeImagePath);

  PdfPreparation rebuild;
  ASSERT_TRUE(rebuild.begin(harness.config("/books/cancel-jpeg.pdf")).ok());
  while (rebuild.phase() != PdfPreparationPhase::CacheImage) {
    const PdfStepResult step = rebuild.step();
    ++harness.nowMs;
    ASSERT_TRUE(step.yielded());
  }
  ASSERT_NE(rebuild.generation(), activeGeneration);
  const uint32_t cancelledGeneration = rebuild.generation();
  const std::string cancelledGenerationRoot =
      std::string(rebuild.cacheRoot()) + "/gen_" + std::to_string(cancelledGeneration);
  const std::string cancelledImageRoot = cancelledGenerationRoot + "/images/";
  harness.chargeIoTime = true;
  bool partialWritten = false;
  for (uint8_t slice = 0; slice < 8 && !partialWritten; ++slice) {
    const PreparationStepObservation observation = observePreparationStep(rebuild, harness);
    expectBoundedPreparationStep(observation);
    ASSERT_TRUE(observation.result.yielded());
    ASSERT_EQ(rebuild.phase(), PdfPreparationPhase::CacheImage);
    ++harness.nowMs;
    const auto paths = harness.storage.paths();
    const auto partial = std::find_if(paths.begin(), paths.end(), [&](const std::string& path) {
      return path == cancelledImageRoot + "build-jpeg.tmp" && !harness.storage.bytes(path).empty();
    });
    partialWritten = partial != paths.end();
  }
  ASSERT_TRUE(partialWritten);

  const PdfStepResult cancelled = cancelToTerminalBounded(rebuild, harness);

  ASSERT_TRUE(cancelled.failed());
  EXPECT_EQ(cancelled.status.error, PdfError::Cancelled);
  PdfCacheManifestSelection after{};
  ASSERT_TRUE(cache.loadManifestSlots(first.sourceIdentity(), &after).ok());
  ASSERT_TRUE(after.selected);
  EXPECT_EQ(after.manifest.generation, activeGeneration);
  ASSERT_TRUE(harness.storage.exists(activeImagePath));
  EXPECT_EQ(harness.storage.bytes(activeImagePath), activeImageBytes);
  expectRetainedCancelledGeneration(harness.storage, rebuild.cacheRoot(), rebuild.sourceIdentity(),
                                    cancelledGeneration);
  EXPECT_EQ(harness.storage.openHandleCount(), 0U);
  expectFreshGenerationRestart(harness, "/books/cancel-jpeg.pdf", cancelledGeneration);
}

TEST(PdfImagePreparation, CancellationDuringLargeMaskCompositeRemovesPartialStateAndRetainsAResumableGeneration) {
  PreparationHarness harness;
  harness.storage.setMaximumReadHandles(1);
  harness.storage.addFile("/books/cancel-mask.pdf", loadFixture("large_raster_caption.pdf"), 1234, true);
  PdfPreparation first;
  ASSERT_TRUE(first.begin(harness.config("/books/cancel-mask.pdf")).ok());
  ASSERT_TRUE(runToTerminal(first, harness).complete());

  PdfCacheStore cache;
  ASSERT_TRUE(cache.initialize(harness.storage.io(), first.cacheRoot()).ok());
  PdfCacheManifestSelection before{};
  ASSERT_TRUE(cache.loadManifestSlots(first.sourceIdentity(), &before).ok());
  ASSERT_TRUE(before.selected);
  const uint32_t activeGeneration = before.manifest.generation;
  const std::string activeRoot = std::string(first.cacheRoot()) + "/gen_" + std::to_string(activeGeneration);
  const auto activePaths = harness.storage.paths();
  const auto activeImage = std::find_if(activePaths.begin(), activePaths.end(), [&](const std::string& path) {
    return path.starts_with(activeRoot + "/images/") && path.ends_with(".pxc");
  });
  ASSERT_NE(activeImage, activePaths.end());
  const std::string activeImagePath = *activeImage;
  const std::vector<uint8_t> activeImageBytes = harness.storage.bytes(activeImagePath);

  PdfPreparation rebuild;
  ASSERT_TRUE(rebuild.begin(harness.config("/books/cancel-mask.pdf")).ok());
  while (rebuild.phase() != PdfPreparationPhase::DecodeImages) {
    const PdfStepResult step = rebuild.step();
    ++harness.nowMs;
    ASSERT_TRUE(step.yielded());
  }
  ASSERT_NE(rebuild.generation(), activeGeneration);
  const uint32_t cancelledGeneration = rebuild.generation();
  const std::string cancelledGenerationRoot =
      std::string(rebuild.cacheRoot()) + "/gen_" + std::to_string(cancelledGeneration);
  bool partialWritten = false;
  for (uint32_t slice = 0; slice < 2000 && !partialWritten; ++slice) {
    if (rebuild.maskSpoolReadCount() != 0) {
      harness.chargeIoTime = true;
    }
    const PreparationStepObservation observation = observePreparationStep(rebuild, harness);
    if (rebuild.maskSpoolReadCount() != 0) {
      harness.chargeIoTime = true;
      expectBoundedPreparationStep(observation);
    }
    ASSERT_TRUE(observation.result.yielded())
        << static_cast<int>(observation.result.status.error) << "@" << observation.result.status.offset;
    ASSERT_EQ(rebuild.phase(), PdfPreparationPhase::DecodeImages);
    ++harness.nowMs;
    const auto paths = harness.storage.paths();
    const auto partial = std::find_if(paths.begin(), paths.end(), [&](const std::string& path) {
      return path.starts_with(cancelledGenerationRoot + "/images/") && path.ends_with(".pxc") &&
             harness.storage.bytes(path).size() > pixel_cache::kHeaderSize;
    });
    partialWritten = partial != paths.end();
  }
  ASSERT_TRUE(partialWritten);

  const PdfStepResult cancelled = cancelToTerminalBounded(rebuild, harness);

  ASSERT_TRUE(cancelled.failed());
  EXPECT_EQ(cancelled.status.error, PdfError::Cancelled);
  PdfCacheManifestSelection after{};
  ASSERT_TRUE(cache.loadManifestSlots(first.sourceIdentity(), &after).ok());
  ASSERT_TRUE(after.selected);
  EXPECT_EQ(after.manifest.generation, activeGeneration);
  ASSERT_TRUE(harness.storage.exists(activeImagePath));
  EXPECT_EQ(harness.storage.bytes(activeImagePath), activeImageBytes);
  EXPECT_FALSE(harness.storage.exists(cancelledGenerationRoot + "/build.mask"));
  expectRetainedCancelledGeneration(harness.storage, rebuild.cacheRoot(), rebuild.sourceIdentity(),
                                    cancelledGeneration);
  EXPECT_EQ(harness.storage.openHandleCount(), 0U);
  expectFreshGenerationRestart(harness, "/books/cancel-mask.pdf", cancelledGeneration);
}

TEST(PdfImagePreparation, CancellationDuringLargeRasterDecodeClosesPartialOutputAndRetainsAResumableGeneration) {
  PreparationHarness harness;
  harness.storage.setMaximumReadHandles(1);
  harness.storage.addFile("/books/cancel-raster.pdf", loadFixture("large_raster_caption.pdf"), 1234, true);
  PdfPreparation preparation;
  ASSERT_TRUE(preparation.begin(harness.config("/books/cancel-raster.pdf")).ok());
  while (preparation.phase() != PdfPreparationPhase::DecodeImages) {
    const PdfStepResult step = preparation.step();
    ++harness.nowMs;
    ASSERT_TRUE(step.yielded());
  }
  for (uint8_t slice = 0; slice < 3; ++slice) {
    const PdfStepResult step = preparation.step();
    ++harness.nowMs;
    ASSERT_TRUE(step.yielded());
    ASSERT_EQ(preparation.phase(), PdfPreparationPhase::DecodeImages);
  }

  harness.chargeIoTime = true;
  const uint32_t cancelledGeneration = preparation.generation();
  const PdfStepResult cancelled = cancelToTerminalBounded(preparation, harness);

  ASSERT_TRUE(cancelled.failed());
  EXPECT_EQ(cancelled.status.error, PdfError::Cancelled);
  PdfCacheStore cache;
  ASSERT_TRUE(cache.initialize(harness.storage.io(), preparation.cacheRoot()).ok());
  PdfCacheManifestSelection selection{};
  ASSERT_TRUE(cache.loadManifestSlots(preparation.sourceIdentity(), &selection).ok());
  EXPECT_FALSE(selection.selected);
  expectRetainedCancelledGeneration(harness.storage, preparation.cacheRoot(), preparation.sourceIdentity(),
                                    cancelledGeneration);
  EXPECT_EQ(harness.storage.openHandleCount(), 0U);
  expectFreshGenerationRestart(harness, "/books/cancel-raster.pdf", cancelledGeneration);
}

TEST(PdfImagePreparation, NavigationSpoolWriteFailurePublishesNoManifestAndLeavesNoOpenHandle) {
  PreparationHarness harness;
  harness.storage.addFile("/books/flate-gray-caption.pdf", loadFixture("flate_gray_caption.pdf"), 1234, true);
  PdfPreparation preparation;
  ASSERT_TRUE(preparation.begin(harness.config("/books/flate-gray-caption.pdf")).ok());
  while (preparation.phase() != PdfPreparationPhase::SpoolNavigation) {
    const PdfStepResult result = preparation.step();
    ++harness.nowMs;
    ASSERT_TRUE(result.yielded());
  }
  harness.storage.fail(PdfTestFaultPoint::Write);

  const PdfStepResult result = runToTerminal(preparation, harness);

  ASSERT_TRUE(result.failed());
  EXPECT_EQ(result.status.error, PdfError::IoFailure);
  PdfCacheStore cache;
  ASSERT_TRUE(cache.initialize(harness.storage.io(), preparation.cacheRoot()).ok());
  PdfCacheManifestSelection selection{};
  ASSERT_TRUE(cache.loadManifestSlots(preparation.sourceIdentity(), &selection).ok());
  EXPECT_FALSE(selection.selected);
  const std::string spoolPath =
      std::string(preparation.cacheRoot()) + "/gen_" + std::to_string(preparation.generation()) + "/build.nav";
  EXPECT_FALSE(harness.storage.exists(spoolPath));
  EXPECT_EQ(harness.storage.openHandleCount(), 0U);
}

TEST(PdfImagePreparation, FailedRebuildKeepsThePreviouslyActiveGenerationAndEveryRequiredFile) {
  PreparationHarness harness;
  harness.storage.addFile("/books/atomic-rebuild.pdf", loadFixture("flate_gray_caption.pdf"), 1234, true);
  PdfPreparation first;
  ASSERT_TRUE(first.begin(harness.config("/books/atomic-rebuild.pdf")).ok());
  ASSERT_TRUE(runToTerminal(first, harness).complete());
  PdfCacheStore cache;
  ASSERT_TRUE(cache.initialize(harness.storage.io(), first.cacheRoot()).ok());
  PdfCacheManifestSelection before{};
  ASSERT_TRUE(cache.loadManifestSlots(first.sourceIdentity(), &before).ok());
  ASSERT_TRUE(before.selected);
  const uint32_t activeGeneration = before.manifest.generation;
  const std::string activeRoot = std::string(first.cacheRoot()) + "/gen_" + std::to_string(activeGeneration);
  const auto beforePaths = harness.storage.paths();
  const auto activeImage = std::find_if(beforePaths.begin(), beforePaths.end(), [&](const std::string& path) {
    return path.starts_with(activeRoot + "/images/") && path.ends_with(".pxc");
  });
  ASSERT_NE(activeImage, beforePaths.end());
  const std::string activeImagePath = *activeImage;
  const std::vector<uint8_t> activeImageBytes = harness.storage.bytes(activeImagePath);

  PdfPreparation rebuild;
  ASSERT_TRUE(rebuild.begin(harness.config("/books/atomic-rebuild.pdf")).ok());
  while (rebuild.phase() != PdfPreparationPhase::WriteMetadata) {
    const PdfStepResult step = rebuild.step();
    ++harness.nowMs;
    ASSERT_TRUE(step.yielded()) << static_cast<int>(step.status.error) << "@" << step.status.offset;
  }
  EXPECT_NE(rebuild.generation(), activeGeneration);
  harness.storage.fail(PdfTestFaultPoint::Write);
  const PdfStepResult failed = runToTerminal(rebuild, harness);
  ASSERT_TRUE(failed.failed());
  harness.storage.clearFault();

  PdfCacheManifestSelection after{};
  ASSERT_TRUE(cache.loadManifestSlots(first.sourceIdentity(), &after).ok());
  ASSERT_TRUE(after.selected);
  EXPECT_EQ(after.manifest.generation, activeGeneration);
  ASSERT_TRUE(harness.storage.exists(activeImagePath));
  EXPECT_EQ(harness.storage.bytes(activeImagePath), activeImageBytes);
  EXPECT_EQ(harness.storage.openHandleCount(), 0U);
}

TEST(PdfImagePreparation, UnsupportedOptionalImageIsOmittedWithWarningWhileTextSurvives) {
  PreparationHarness harness;
  harness.storage.addFile("/books/unsupported-jpx.pdf", loadFixture("unsupported_jpx_caption.pdf"), 1234, true);
  PdfPreparation preparation;
  ASSERT_TRUE(preparation.begin(harness.config("/books/unsupported-jpx.pdf")).ok());

  const PdfStepResult result = runToTerminal(preparation, harness);

  ASSERT_TRUE(result.complete()) << static_cast<int>(result.status.error) << "@" << result.status.offset;
  const std::string generationRoot =
      std::string(preparation.cacheRoot()) + "/gen_" + std::to_string(preparation.generation());
  const std::string sectionPath = generationRoot + "/sections/000000.xhtml";
  const std::string xhtml(harness.storage.bytes(sectionPath).begin(), harness.storage.bytes(sectionPath).end());
  EXPECT_NE(xhtml.find("Text survives."), std::string::npos);
  EXPECT_EQ(xhtml.find("<img "), std::string::npos);
  PdfCacheStore cache;
  ASSERT_TRUE(cache.initialize(harness.storage.io(), preparation.cacheRoot()).ok());
  PdfCacheManifestSelection selection{};
  ASSERT_TRUE(cache.loadManifestSlots(preparation.sourceIdentity(), &selection).ok());
  ASSERT_TRUE(selection.selected);
  EXPECT_NE(selection.manifest.warningFlags, 0U);
  EXPECT_EQ(selection.manifest.requiredFileCount, 5U);
  EXPECT_EQ(harness.storage.openHandleCount(), 0U);
}

TEST(PdfImagePreparation, MalformedDeferredRasterRemovesItsTagAndRecomputesTheSectionLedger) {
  PreparationHarness harness;
  harness.storage.setMaximumReadHandles(1);
  constexpr char sourcePath[] = "/books/malformed-flate.pdf";
  harness.storage.addFile(sourcePath, loadFixture("malformed_flate_caption.pdf"), 1234, true);
  PdfPreparation preparation;
  ASSERT_TRUE(preparation.begin(harness.config(sourcePath)).ok());

  const PdfStepResult result = runToTerminal(preparation, harness);

  std::string eventTrace;
  for (const std::string& event : harness.storage.events()) {
    eventTrace += event;
    eventTrace += '\n';
  }
  ASSERT_TRUE(result.complete()) << static_cast<int>(result.status.error) << "@" << result.status.offset
                                 << " phase=" << static_cast<int>(preparation.phase()) << '\n'
                                 << eventTrace;
  const std::string generationRelative = "gen_" + std::to_string(preparation.generation());
  const std::string generationRoot = std::string(preparation.cacheRoot()) + "/" + generationRelative;
  const std::string sectionRelative = generationRelative + "/sections/000000.xhtml";
  const std::string sectionPath = std::string(preparation.cacheRoot()) + "/" + sectionRelative;
  const std::vector<uint8_t>& sectionBytes = harness.storage.bytes(sectionPath);
  const std::string xhtml(sectionBytes.begin(), sectionBytes.end());
  EXPECT_NE(xhtml.find("Malformed raster caption survives."), std::string::npos);
  EXPECT_EQ(xhtml.find("<img "), std::string::npos);
  const auto generatedPaths = harness.storage.paths();
  EXPECT_EQ(std::find_if(generatedPaths.begin(), generatedPaths.end(),
                         [&](const std::string& path) { return path.starts_with(generationRoot + "/images/"); }),
            generatedPaths.end());

  PdfCacheStore cache;
  ASSERT_TRUE(cache.initialize(harness.storage.io(), preparation.cacheRoot()).ok());
  PdfCacheManifestSelection selection{};
  ASSERT_TRUE(cache.loadManifestSlots(preparation.sourceIdentity(), &selection).ok());
  ASSERT_TRUE(selection.selected);
  EXPECT_NE(selection.manifest.warningFlags, 0U);
  EXPECT_EQ(selection.manifest.requiredFileCount, 5U);

  const std::string relativePaths[] = {
      sectionRelative,
      generationRelative + "/cover.bmp",
      generationRelative + "/thumb.bmp",
      generationRelative + "/metadata.bin",
      generationRelative + "/outline.bin",
  };
  uint64_t ledger = PDF_CACHE_FNV64_OFFSET;
  uint64_t requiredBytes = 0;
  for (const std::string& relativePath : relativePaths) {
    const std::string fullPath = std::string(preparation.cacheRoot()) + "/" + relativePath;
    ASSERT_TRUE(harness.storage.exists(fullPath)) << fullPath;
    const auto record = requiredRecord(relativePath, harness.storage.bytes(fullPath));
    ledger = pdfUpdateRequiredFileLedger(ledger, record);
    requiredBytes += record.size;
  }
  EXPECT_EQ(selection.manifest.requiredFileLedger, ledger);
  EXPECT_EQ(selection.manifest.requiredFileBytes, requiredBytes);
  EXPECT_EQ(harness.storage.openHandleCount(), 0U);
}

TEST(PdfImagePreparation, DeferredRasterWriteFailureOmitsOnlyTheImageAndStillPublishesText) {
  PreparationHarness harness;
  constexpr char sourcePath[] = "/books/raster-write-failure.pdf";
  harness.storage.addFile(sourcePath, loadFixture("flate_gray_caption.pdf"), 1234, true);
  PdfPreparation preparation;
  ASSERT_TRUE(preparation.begin(harness.config(sourcePath)).ok());
  while (preparation.phase() != PdfPreparationPhase::DecodeImages) {
    const PdfStepResult step = preparation.step();
    ++harness.nowMs;
    ASSERT_TRUE(step.yielded()) << static_cast<int>(step.status.error) << "@" << step.status.offset;
  }
  harness.storage.fail(PdfTestFaultPoint::Write);

  const PdfStepResult result = runToTerminal(preparation, harness);

  ASSERT_TRUE(result.complete()) << static_cast<int>(result.status.error) << "@" << result.status.offset;
  const std::string generationRoot =
      std::string(preparation.cacheRoot()) + "/gen_" + std::to_string(preparation.generation());
  const std::string sectionPath = generationRoot + "/sections/000000.xhtml";
  const std::vector<uint8_t>& sectionBytes = harness.storage.bytes(sectionPath);
  const std::string xhtml(sectionBytes.begin(), sectionBytes.end());
  EXPECT_NE(xhtml.find("Raster caption."), std::string::npos);
  EXPECT_EQ(xhtml.find("<img "), std::string::npos);
  const auto generatedPaths = harness.storage.paths();
  EXPECT_EQ(std::find_if(generatedPaths.begin(), generatedPaths.end(),
                         [&](const std::string& path) { return path.starts_with(generationRoot + "/images/"); }),
            generatedPaths.end());
  PdfCacheStore cache;
  ASSERT_TRUE(cache.initialize(harness.storage.io(), preparation.cacheRoot()).ok());
  PdfCacheManifestSelection selection{};
  ASSERT_TRUE(cache.loadManifestSlots(preparation.sourceIdentity(), &selection).ok());
  ASSERT_TRUE(selection.selected);
  EXPECT_NE(selection.manifest.warningFlags, 0U);
  EXPECT_EQ(selection.manifest.requiredFileCount, 5U);
  EXPECT_EQ(harness.storage.openHandleCount(), 0U);
}

TEST(PdfImagePreparation, FormXObjectIsSkippedWhileImageAndCaptionSurvive) {
  PreparationHarness harness;
  harness.storage.addFile("/books/mixed-form-image.pdf", loadFixture("mixed_form_image_caption.pdf"), 1234, true);
  PdfPreparation preparation;
  ASSERT_TRUE(preparation.begin(harness.config("/books/mixed-form-image.pdf")).ok());

  const PdfStepResult result = runToTerminal(preparation, harness);

  ASSERT_TRUE(result.complete()) << static_cast<int>(result.status.error) << "@" << result.status.offset;
  const std::string generationRoot =
      std::string(preparation.cacheRoot()) + "/gen_" + std::to_string(preparation.generation());
  const std::string sectionPath = generationRoot + "/sections/000000.xhtml";
  const std::string xhtml(harness.storage.bytes(sectionPath).begin(), harness.storage.bytes(sectionPath).end());
  EXPECT_NE(xhtml.find("Mixed resource caption."), std::string::npos);
  const size_t image = xhtml.find("<img ");
  ASSERT_NE(image, std::string::npos);
  EXPECT_EQ(xhtml.find("<img ", image + 1U), std::string::npos);
  EXPECT_EQ(harness.storage.openHandleCount(), 0U);
}

TEST(PdfImagePreparation, ThreeFiguresOnOnePageStayAtTheirThreeCaptionAnchors) {
  PreparationHarness harness;
  harness.storage.addFile("/books/three-figures.pdf", loadFixture("three_figures_one_page.pdf"), 1234, true);
  PdfPreparation preparation;
  ASSERT_TRUE(preparation.begin(harness.config("/books/three-figures.pdf")).ok());

  const PdfStepResult result = runToTerminal(preparation, harness);

  ASSERT_TRUE(result.complete()) << static_cast<int>(result.status.error) << "@" << result.status.offset;
  const std::string generationRoot =
      std::string(preparation.cacheRoot()) + "/gen_" + std::to_string(preparation.generation());
  const std::string sectionPath = generationRoot + "/sections/000000.xhtml";
  const std::string xhtml(harness.storage.bytes(sectionPath).begin(), harness.storage.bytes(sectionPath).end());
  size_t cursor = 0;
  for (const char* caption : {"First figure caption.", "Second figure caption.", "Third figure caption."}) {
    const size_t captionPosition = xhtml.find(caption, cursor);
    ASSERT_NE(captionPosition, std::string::npos) << caption;
    const size_t imagePosition = xhtml.find("<img ", captionPosition);
    ASSERT_NE(imagePosition, std::string::npos) << caption;
    const size_t blockEnd = xhtml.find("</p>", captionPosition);
    ASSERT_NE(blockEnd, std::string::npos) << caption;
    EXPECT_LT(imagePosition, blockEnd) << caption;
    cursor = blockEnd + 4U;
  }
  EXPECT_EQ(xhtml.find("<img ", cursor), std::string::npos);
  EXPECT_EQ(harness.storage.openHandleCount(), 0U);
}

TEST(PdfImagePreparation, DuplicateRasterContentPublishesOnePathAndOneManifestRecord) {
  PreparationHarness harness;
  constexpr char sourcePath[] = "/books/duplicate-raster-figures.pdf";
  harness.storage.addFile(sourcePath, loadFixture("duplicate_raster_figures.pdf"), 1234, true);
  PdfPreparation preparation;
  ASSERT_TRUE(preparation.begin(harness.config(sourcePath)).ok());

  const PdfStepResult result = runToTerminal(preparation, harness);

  ASSERT_TRUE(result.complete()) << static_cast<int>(result.status.error) << "@" << result.status.offset;
  const std::string generationRoot =
      std::string(preparation.cacheRoot()) + "/gen_" + std::to_string(preparation.generation());
  const std::string sectionPath = generationRoot + "/sections/000000.xhtml";
  const std::string xhtml(harness.storage.bytes(sectionPath).begin(), harness.storage.bytes(sectionPath).end());
  const size_t firstImage = xhtml.find("<img ");
  ASSERT_NE(firstImage, std::string::npos);
  const size_t secondImage = xhtml.find("<img ", firstImage + 1U);
  ASSERT_NE(secondImage, std::string::npos);
  const auto imageSource = [&](const size_t image) {
    constexpr std::string_view prefix = "src=\"";
    const size_t start = xhtml.find(prefix, image);
    EXPECT_NE(start, std::string::npos);
    const size_t valueStart = start + prefix.size();
    const size_t end = xhtml.find('"', valueStart);
    EXPECT_NE(end, std::string::npos);
    return xhtml.substr(valueStart, end - valueStart);
  };
  EXPECT_EQ(imageSource(firstImage), imageSource(secondImage));

  const std::vector<std::string> paths = harness.storage.paths();
  const auto pixelCachePaths = std::count_if(paths.begin(), paths.end(), [&](const std::string& path) {
    return path.starts_with(generationRoot + "/images/") && path.ends_with(".pxc");
  });
  EXPECT_EQ(pixelCachePaths, 1);
  const auto pixelCachePath = std::find_if(paths.begin(), paths.end(), [&](const std::string& path) {
    return path.starts_with(generationRoot + "/images/") && path.ends_with(".pxc");
  });
  ASSERT_NE(pixelCachePath, paths.end());
  EXPECT_EQ(harness.storage.openCallsForPath(*pixelCachePath), 1U);

  PdfCacheStore cache;
  ASSERT_TRUE(cache.initialize(harness.storage.io(), preparation.cacheRoot()).ok());
  PdfCacheManifestSelection selection{};
  ASSERT_TRUE(cache.loadManifestSlots(preparation.sourceIdentity(), &selection).ok());
  ASSERT_TRUE(selection.selected);
  EXPECT_EQ(selection.manifest.requiredFileCount, 6U);
  EXPECT_EQ(harness.storage.openHandleCount(), 0U);
}

TEST(PdfImagePreparation, SameRasterBytesWithDifferentDecodeContractsStayDistinct) {
  PreparationHarness harness;
  constexpr char sourcePath[] = "/books/different-raster-contracts.pdf";
  harness.storage.addFile(sourcePath, loadFixture("same_bytes_different_raster_contract.pdf"), 1234, true);
  PdfPreparation preparation;
  ASSERT_TRUE(preparation.begin(harness.config(sourcePath)).ok());

  const PdfStepResult result = runToTerminal(preparation, harness);

  ASSERT_TRUE(result.complete()) << static_cast<int>(result.status.error) << "@" << result.status.offset;
  const std::string generationRoot =
      std::string(preparation.cacheRoot()) + "/gen_" + std::to_string(preparation.generation());
  const std::string sectionPath = generationRoot + "/sections/000000.xhtml";
  const std::string xhtml(harness.storage.bytes(sectionPath).begin(), harness.storage.bytes(sectionPath).end());
  const size_t firstImage = xhtml.find("<img ");
  ASSERT_NE(firstImage, std::string::npos);
  const size_t secondImage = xhtml.find("<img ", firstImage + 1U);
  ASSERT_NE(secondImage, std::string::npos);
  const auto imageSource = [&](const size_t image) {
    constexpr std::string_view prefix = "src=\"";
    const size_t start = xhtml.find(prefix, image);
    EXPECT_NE(start, std::string::npos);
    const size_t valueStart = start + prefix.size();
    const size_t end = xhtml.find('"', valueStart);
    EXPECT_NE(end, std::string::npos);
    return xhtml.substr(valueStart, end - valueStart);
  };
  EXPECT_NE(imageSource(firstImage), imageSource(secondImage));

  std::vector<std::string> pixelCachePaths;
  for (const std::string& path : harness.storage.paths()) {
    if (path.starts_with(generationRoot + "/images/") && path.ends_with(".pxc")) {
      pixelCachePaths.push_back(path);
    }
  }
  ASSERT_EQ(pixelCachePaths.size(), 2U);
  for (const std::string& path : pixelCachePaths) {
    const std::string leaf = std::filesystem::path(path).filename().string();
    EXPECT_EQ(leaf.size(), 29U);
    EXPECT_EQ(leaf[16], '-');
    EXPECT_EQ(harness.storage.openCallsForPath(path), 1U);
  }
  const std::vector<uint8_t> normal = {2, 0, 2, 0, 0x10, 0xB0};
  const std::vector<uint8_t> inverted = {2, 0, 2, 0, 0xE0, 0x40};
  const std::vector<uint8_t>& first = harness.storage.bytes(pixelCachePaths[0]);
  const std::vector<uint8_t>& second = harness.storage.bytes(pixelCachePaths[1]);
  EXPECT_TRUE((first == normal && second == inverted) || (first == inverted && second == normal));

  PdfCacheStore cache;
  ASSERT_TRUE(cache.initialize(harness.storage.io(), preparation.cacheRoot()).ok());
  PdfCacheManifestSelection selection{};
  ASSERT_TRUE(cache.loadManifestSlots(preparation.sourceIdentity(), &selection).ok());
  ASSERT_TRUE(selection.selected);
  EXPECT_EQ(selection.manifest.requiredFileCount, 7U);
  EXPECT_EQ(harness.storage.openHandleCount(), 0U);
}

TEST(PdfImagePreparation, ImageMaskUsesThePaintLuminanceAndRestoresItAcrossGraphicsState) {
  PreparationHarness harness;
  constexpr char sourcePath[] = "/books/image-mask-paint.pdf";
  harness.storage.addFile(sourcePath, loadFixture("image_mask_paint_contract.pdf"), 1234, true);
  PdfPreparation preparation;
  ASSERT_TRUE(preparation.begin(harness.config(sourcePath)).ok());

  const PdfStepResult result = runToTerminal(preparation, harness);

  ASSERT_TRUE(result.complete()) << static_cast<int>(result.status.error) << "@" << result.status.offset;
  const std::string generationRoot =
      std::string(preparation.cacheRoot()) + "/gen_" + std::to_string(preparation.generation());
  const std::string sectionPath = generationRoot + "/sections/000000.xhtml";
  const std::string xhtml(harness.storage.bytes(sectionPath).begin(), harness.storage.bytes(sectionPath).end());
  const size_t firstImage = xhtml.find("<img ");
  ASSERT_NE(firstImage, std::string::npos);
  const size_t secondImage = xhtml.find("<img ", firstImage + 1U);
  ASSERT_NE(secondImage, std::string::npos);
  const auto imageSource = [&](const size_t image) {
    constexpr std::string_view prefix = "src=\"";
    const size_t start = xhtml.find(prefix, image);
    EXPECT_NE(start, std::string::npos);
    const size_t valueStart = start + prefix.size();
    const size_t end = xhtml.find('"', valueStart);
    EXPECT_NE(end, std::string::npos);
    return xhtml.substr(valueStart, end - valueStart);
  };
  EXPECT_NE(imageSource(firstImage), imageSource(secondImage));

  std::vector<std::string> pixelCachePaths;
  for (const std::string& path : harness.storage.paths()) {
    if (path.starts_with(generationRoot + "/images/") && path.ends_with(".pxc")) {
      pixelCachePaths.push_back(path);
    }
  }
  ASSERT_EQ(pixelCachePaths.size(), 2U);
  const std::vector<uint8_t> grayPaint = {4, 0, 1, 0, 0xEE};
  const std::vector<uint8_t> blackPaint = {4, 0, 1, 0, 0xCC};
  const std::vector<uint8_t>& first = harness.storage.bytes(pixelCachePaths[0]);
  const std::vector<uint8_t>& second = harness.storage.bytes(pixelCachePaths[1]);
  EXPECT_TRUE((first == grayPaint && second == blackPaint) || (first == blackPaint && second == grayPaint));

  PdfCacheStore cache;
  ASSERT_TRUE(cache.initialize(harness.storage.io(), preparation.cacheRoot()).ok());
  PdfCacheManifestSelection selection{};
  ASSERT_TRUE(cache.loadManifestSlots(preparation.sourceIdentity(), &selection).ok());
  ASSERT_TRUE(selection.selected);
  EXPECT_EQ(selection.manifest.requiredFileCount, 7U);
  EXPECT_EQ(harness.storage.openHandleCount(), 0U);
}

TEST(PdfImagePreparation, TenPagesKeepUniqueFiguresAndSuppressTheRepeatedHeaderPastEightImages) {
  PreparationHarness harness;
  harness.storage.addFile("/books/ten-page-figures.pdf", loadFixture("ten_page_figures_repeated_header.pdf"), 1234,
                          true);
  PdfPreparation preparation;
  ASSERT_TRUE(preparation.begin(harness.config("/books/ten-page-figures.pdf")).ok());

  const PdfStepResult result = runToTerminal(preparation, harness);

  ASSERT_TRUE(result.complete()) << static_cast<int>(result.status.error) << "@" << result.status.offset
                                 << " phase=" << static_cast<int>(preparation.phase());
  const std::string generationRoot =
      std::string(preparation.cacheRoot()) + "/gen_" + std::to_string(preparation.generation());
  const std::string sectionPath = generationRoot + "/sections/000000.xhtml";
  ASSERT_TRUE(harness.storage.exists(sectionPath));
  EXPECT_FALSE(harness.storage.exists(generationRoot + "/sections/000001.xhtml"));
  const std::string xhtml(harness.storage.bytes(sectionPath).begin(), harness.storage.bytes(sectionPath).end());
  size_t previousCaption = 0;
  for (uint8_t page = 0; page < 10; ++page) {
    const std::string caption = "Unique figure " + std::to_string(page + 1U) + ".";
    const size_t captionPosition = xhtml.find(caption, previousCaption);
    ASSERT_NE(captionPosition, std::string::npos) << sectionPath;
    EXPECT_GE(captionPosition, previousCaption);
    previousCaption = captionPosition;
  }
  size_t sectionImageCount = 0;
  for (size_t image = xhtml.find("<img "); image != std::string::npos; image = xhtml.find("<img ", image + 5U)) {
    ++sectionImageCount;
  }
  EXPECT_EQ(sectionImageCount, 10U);
  const auto paths = harness.storage.paths();
  const auto imageCount = std::count_if(paths.begin(), paths.end(), [&](const std::string& path) {
    return path.starts_with(generationRoot + "/images/") && path.ends_with(".pxc");
  });
  EXPECT_EQ(imageCount, 10);
  PdfCacheStore cache;
  ASSERT_TRUE(cache.initialize(harness.storage.io(), preparation.cacheRoot()).ok());
  PdfCacheManifestSelection selection{};
  ASSERT_TRUE(cache.loadManifestSlots(preparation.sourceIdentity(), &selection).ok());
  ASSERT_TRUE(selection.selected);
  EXPECT_EQ(selection.manifest.requiredFileCount, 15U);
  EXPECT_EQ(harness.storage.openHandleCount(), 0U);
}

TEST(PdfImagePreparation, SixtyFourFiguresPersistThroughTheManifestSpoolIncludingSeventeenthAndLast) {
  PreparationHarness harness;
  harness.storage.addFile("/books/sixty-four-figures.pdf", loadFixture("sixty_four_unique_figures.pdf"), 1234, true);
  PdfPreparation preparation;
  ASSERT_TRUE(preparation.begin(harness.config("/books/sixty-four-figures.pdf")).ok());

  const PdfStepResult result = runToTerminal(preparation, harness);

  ASSERT_TRUE(result.complete()) << static_cast<int>(result.status.error) << "@" << result.status.offset
                                 << " phase=" << static_cast<int>(preparation.phase());
  const std::string generationRoot =
      std::string(preparation.cacheRoot()) + "/gen_" + std::to_string(preparation.generation());
  const auto paths = harness.storage.paths();
  const auto imageCount = std::count_if(paths.begin(), paths.end(), [&](const std::string& path) {
    return path.starts_with(generationRoot + "/images/") && path.ends_with(".pxc");
  });
  EXPECT_EQ(imageCount, 64);
  std::string allSections;
  for (const std::string& path : paths) {
    if (path.starts_with(generationRoot + "/sections/") && path.ends_with(".xhtml")) {
      const auto& bytes = harness.storage.bytes(path);
      allSections.append(bytes.begin(), bytes.end());
    }
  }
  for (const char* caption : {"Bounded figure 17.", "Bounded figure 64."}) {
    const size_t captionPosition = allSections.find(caption);
    ASSERT_NE(captionPosition, std::string::npos) << caption;
    const size_t imagePosition = allSections.find("<img ", captionPosition);
    const size_t blockEnd = allSections.find("</p>", captionPosition);
    ASSERT_NE(imagePosition, std::string::npos) << caption;
    ASSERT_NE(blockEnd, std::string::npos) << caption;
    EXPECT_LT(imagePosition, blockEnd) << caption;
  }
  PdfCacheStore cache;
  ASSERT_TRUE(cache.initialize(harness.storage.io(), preparation.cacheRoot()).ok());
  PdfCacheManifestSelection selection{};
  ASSERT_TRUE(cache.loadManifestSlots(preparation.sourceIdentity(), &selection).ok());
  ASSERT_TRUE(selection.selected);
  EXPECT_EQ(selection.manifest.warningFlags, 0U);
  EXPECT_EQ(selection.manifest.requiredFileCount, 69U);
  EXPECT_EQ(harness.storage.openHandleCount(), 0U);
}

TEST(PdfImagePreparation, SixtyFourFiguresKeepEveryPublicStepWithinCooperativeBudget) {
  PreparationHarness harness;
  harness.storage.setMaximumReadHandles(1);
  harness.storage.addFile("/books/sixty-four-bounded.pdf", loadFixture("sixty_four_unique_figures.pdf"), 1234, true);
  harness.chargeIoTime = true;
  PdfPreparation preparation;
  ASSERT_TRUE(preparation.begin(harness.config("/books/sixty-four-bounded.pdf")).ok());

  for (uint32_t slice = 0; slice < 30000; ++slice) {
    const PdfPreparationPhase phaseBefore = preparation.phase();
    const PreparationStepObservation observation = observePreparationStep(preparation, harness);
    SCOPED_TRACE(::testing::Message() << "slice=" << slice << " phaseBefore=" << static_cast<int>(phaseBefore)
                                      << " phaseAfter=" << static_cast<int>(preparation.phase()));
    expectBoundedPreparationStep(observation);
    if (!observation.result.yielded()) {
      ASSERT_TRUE(observation.result.complete())
          << static_cast<int>(observation.result.status.error) << "@" << observation.result.status.offset;
      EXPECT_EQ(harness.storage.openHandleCount(), 0U) << ::testing::PrintToString(harness.storage.openHandlePaths());
      return;
    }
    ++harness.nowMs;
  }
  FAIL() << "preparation did not complete within the slice limit";
}

TEST(PdfImagePreparation, ThirtyTwoOutlineEntriesKeepEveryPublicStepWithinCooperativeBudget) {
  PreparationHarness harness;
  harness.storage.addFile("/books/thirty-two-outline-entries.pdf", loadFixture("navigation_outline_32.pdf"), 1234,
                          true);
  harness.chargeIoTime = true;
  PdfPreparation preparation;
  ASSERT_TRUE(preparation.begin(harness.config("/books/thirty-two-outline-entries.pdf")).ok());

  for (uint32_t slice = 0; slice < 30000; ++slice) {
    const PdfPreparationPhase phaseBefore = preparation.phase();
    const PreparationStepObservation observation = observePreparationStep(preparation, harness);
    SCOPED_TRACE(::testing::Message() << "slice=" << slice << " phaseBefore=" << static_cast<int>(phaseBefore)
                                      << " phaseAfter=" << static_cast<int>(preparation.phase()));
    expectBoundedPreparationStep(observation);
    if (!observation.result.yielded()) {
      ASSERT_TRUE(observation.result.complete())
          << static_cast<int>(observation.result.status.error) << "@" << observation.result.status.offset;
      const std::string outlinePath =
          std::string(preparation.cacheRoot()) + "/gen_" + std::to_string(preparation.generation()) + "/outline.bin";
      ASSERT_TRUE(harness.storage.exists(outlinePath));
      EXPECT_EQ(harness.storage.bytes(outlinePath).size(), 16U + 32U * PdfOutlineLimits::EncodedRecordBytes + 4U);
      EXPECT_EQ(harness.storage.openHandleCount(), 0U) << ::testing::PrintToString(harness.storage.openHandlePaths());
      return;
    }
    ++harness.nowMs;
  }
  FAIL() << "preparation did not complete within the slice limit";
}

TEST(PdfImagePreparation, CleanupOfManyOrphanGenerationsKeepsEveryPublicStepBounded) {
  PreparationHarness harness;
  constexpr char sourcePath[] = "/books/orphan-cleanup.pdf";
  harness.storage.addFile(sourcePath, loadFixture("classic_text.pdf"), 1234, true);
  PdfPreparation preparation;
  ASSERT_TRUE(preparation.begin(harness.config(sourcePath)).ok());
  const std::string cacheRoot = preparation.cacheRoot();
  harness.storage.addDirectory(cacheRoot);
  for (uint32_t generation = 1; generation <= 10; ++generation) {
    const std::string root = cacheRoot + "/gen_" + std::to_string(generation);
    harness.storage.addDirectory(root);
    harness.storage.addDirectory(root + "/nested");
    harness.storage.addFile(root + "/nested/stale.bin", "stale");
  }
  harness.chargeIoTime = true;

  for (uint32_t slice = 0; slice < 30000; ++slice) {
    const PdfPreparationPhase phaseBefore = preparation.phase();
    const PreparationStepObservation observation = observePreparationStep(preparation, harness);
    SCOPED_TRACE(::testing::Message() << "slice=" << slice << " phaseBefore=" << static_cast<int>(phaseBefore)
                                      << " phaseAfter=" << static_cast<int>(preparation.phase()));
    expectBoundedPreparationStep(observation);
    if (!observation.result.yielded()) {
      ASSERT_TRUE(observation.result.complete())
          << static_cast<int>(observation.result.status.error) << "@" << observation.result.status.offset;
      for (uint32_t generation = 1; generation <= 10; ++generation) {
        EXPECT_FALSE(harness.storage.exists(cacheRoot + "/gen_" + std::to_string(generation)));
      }
      const std::string activeRoot = cacheRoot + "/gen_" + std::to_string(preparation.generation());
      EXPECT_TRUE(harness.storage.exists(activeRoot));
      EXPECT_EQ(harness.storage.openHandleCount(), 0U);
      return;
    }
    ++harness.nowMs;
  }
  FAIL() << "preparation did not complete within the slice limit";
}

TEST(PdfImagePreparation, SixtyFifthFigureIsGracefullyOmittedWithoutBreakingTextOrManifest) {
  PreparationHarness harness;
  harness.storage.addFile("/books/sixty-five-figures.pdf", loadFixture("sixty_five_unique_figures.pdf"), 1234, true);
  PdfPreparation preparation;
  ASSERT_TRUE(preparation.begin(harness.config("/books/sixty-five-figures.pdf")).ok());

  const PdfStepResult result = runToTerminal(preparation, harness);

  ASSERT_TRUE(result.complete()) << static_cast<int>(result.status.error) << "@" << result.status.offset
                                 << " phase=" << static_cast<int>(preparation.phase());
  const std::string generationRoot =
      std::string(preparation.cacheRoot()) + "/gen_" + std::to_string(preparation.generation());
  const auto paths = harness.storage.paths();
  const auto imageCount = std::count_if(paths.begin(), paths.end(), [&](const std::string& path) {
    return path.starts_with(generationRoot + "/images/") && path.ends_with(".pxc");
  });
  EXPECT_EQ(imageCount, 64);
  std::string allSections;
  for (const std::string& path : paths) {
    if (path.starts_with(generationRoot + "/sections/") && path.ends_with(".xhtml")) {
      const auto& bytes = harness.storage.bytes(path);
      allSections.append(bytes.begin(), bytes.end());
    }
  }
  const size_t captionPosition = allSections.find("Bounded figure 65.");
  ASSERT_NE(captionPosition, std::string::npos);
  const size_t blockEnd = allSections.find("</p>", captionPosition);
  ASSERT_NE(blockEnd, std::string::npos);
  EXPECT_EQ(allSections.find("<img ", captionPosition), std::string::npos);
  PdfCacheStore cache;
  ASSERT_TRUE(cache.initialize(harness.storage.io(), preparation.cacheRoot()).ok());
  PdfCacheManifestSelection selection{};
  ASSERT_TRUE(cache.loadManifestSlots(preparation.sourceIdentity(), &selection).ok());
  ASSERT_TRUE(selection.selected);
  EXPECT_NE(selection.manifest.warningFlags, 0U);
  EXPECT_EQ(selection.manifest.requiredFileCount, 69U);
  EXPECT_EQ(harness.storage.openHandleCount(), 0U);
}

TEST(PdfImagePreparation, SameSizeGray8SoftMaskIsCompositedFromOneBuildSpool) {
  PreparationHarness harness;
  harness.storage.addFile("/books/soft-mask.pdf", loadFixture("soft_mask_caption.pdf"), 1234, true);
  PdfPreparation preparation;
  ASSERT_TRUE(preparation.begin(harness.config("/books/soft-mask.pdf")).ok());

  const PdfStepResult result = runToTerminal(preparation, harness);

  ASSERT_TRUE(result.complete()) << static_cast<int>(result.status.error) << "@" << result.status.offset;
  const std::string generationRoot =
      std::string(preparation.cacheRoot()) + "/gen_" + std::to_string(preparation.generation());
  const auto paths = harness.storage.paths();
  const auto pixelCachePath = std::find_if(paths.begin(), paths.end(), [&](const std::string& path) {
    return path.starts_with(generationRoot + "/images/") && path.ends_with(".pxc");
  });
  ASSERT_NE(pixelCachePath, paths.end());
  const std::vector<uint8_t> expectedPixelCache = {2, 0, 2, 0, 0x20, 0xF0};
  EXPECT_EQ(harness.storage.bytes(*pixelCachePath), expectedPixelCache);
  const std::string maskSpoolPath = generationRoot + "/build.mask";
  EXPECT_EQ(preparation.maskSpoolWriteCount(), 1U);
  EXPECT_EQ(preparation.maskSpoolReadCount(), 1U);
  EXPECT_EQ(harness.storage.openCallsForPath(maskSpoolPath), 2U);
  const auto& events = harness.storage.events();
  const auto sourceClose = std::find(events.begin(), events.end(), "close:/books/soft-mask.pdf");
  const auto firstMaskOpen = std::find(events.begin(), events.end(), "open:" + maskSpoolPath);
  const auto secondMaskOpen = firstMaskOpen == events.end()
                                  ? events.end()
                                  : std::find(firstMaskOpen + 1, events.end(), "open:" + maskSpoolPath);
  ASSERT_NE(sourceClose, events.end());
  ASSERT_NE(secondMaskOpen, events.end());
  EXPECT_LT(sourceClose, secondMaskOpen);
  EXPECT_FALSE(harness.storage.exists(maskSpoolPath));
  EXPECT_EQ(harness.storage.openHandleCount(), 0U);
}

TEST(PdfImagePreparation, SameSizeOneBitExplicitMaskIsCompositedAsBinaryTransparency) {
  PreparationHarness harness;
  harness.storage.addFile("/books/explicit-mask.pdf", loadFixture("explicit_mask_caption.pdf"), 1234, true);
  PdfPreparation preparation;
  ASSERT_TRUE(preparation.begin(harness.config("/books/explicit-mask.pdf")).ok());

  const PdfStepResult result = runToTerminal(preparation, harness);

  ASSERT_TRUE(result.complete()) << static_cast<int>(result.status.error) << "@" << result.status.offset;
  const std::string generationRoot =
      std::string(preparation.cacheRoot()) + "/gen_" + std::to_string(preparation.generation());
  const auto paths = harness.storage.paths();
  const auto pixelCachePath = std::find_if(paths.begin(), paths.end(), [&](const std::string& path) {
    return path.starts_with(generationRoot + "/images/") && path.ends_with(".pxc");
  });
  ASSERT_NE(pixelCachePath, paths.end());
  const std::vector<uint8_t> expectedPixelCache = {2, 0, 2, 0, 0x30, 0xF0};
  EXPECT_EQ(harness.storage.bytes(*pixelCachePath), expectedPixelCache);
  EXPECT_EQ(preparation.maskSpoolWriteCount(), 1U);
  EXPECT_EQ(preparation.maskSpoolReadCount(), 1U);
  EXPECT_FALSE(harness.storage.exists(generationRoot + "/build.mask"));
  EXPECT_EQ(harness.storage.openHandleCount(), 0U);
}

TEST(PdfImagePreparation, MismatchedAndCyclicSoftMasksOmitOnlyTheImage) {
  struct Case {
    const char* sourcePath;
    const char* fixture;
    const char* text;
  };
  constexpr Case cases[] = {
      {"/books/mismatched-mask.pdf", "mismatched_soft_mask_caption.pdf", "Mismatched mask text survives."},
      {"/books/cyclic-mask.pdf", "cyclic_soft_mask_caption.pdf", "Cyclic mask text survives."},
  };
  for (const Case& testCase : cases) {
    SCOPED_TRACE(testCase.fixture);
    PreparationHarness harness;
    harness.storage.addFile(testCase.sourcePath, loadFixture(testCase.fixture), 1234, true);
    PdfPreparation preparation;
    ASSERT_TRUE(preparation.begin(harness.config(testCase.sourcePath)).ok());

    const PdfStepResult result = runToTerminal(preparation, harness);

    ASSERT_TRUE(result.complete()) << static_cast<int>(result.status.error) << "@" << result.status.offset;
    const std::string generationRoot =
        std::string(preparation.cacheRoot()) + "/gen_" + std::to_string(preparation.generation());
    const std::string sectionPath = generationRoot + "/sections/000000.xhtml";
    const std::string xhtml(harness.storage.bytes(sectionPath).begin(), harness.storage.bytes(sectionPath).end());
    EXPECT_NE(xhtml.find(testCase.text), std::string::npos);
    EXPECT_EQ(xhtml.find("<img "), std::string::npos);
    PdfCacheStore cache;
    ASSERT_TRUE(cache.initialize(harness.storage.io(), preparation.cacheRoot()).ok());
    PdfCacheManifestSelection selection{};
    ASSERT_TRUE(cache.loadManifestSlots(preparation.sourceIdentity(), &selection).ok());
    ASSERT_TRUE(selection.selected);
    EXPECT_NE(selection.manifest.warningFlags, 0U);
    EXPECT_EQ(selection.manifest.requiredFileCount, 5U);
    EXPECT_EQ(preparation.maskSpoolWriteCount(), 0U);
    EXPECT_EQ(preparation.maskSpoolReadCount(), 0U);
    EXPECT_EQ(harness.storage.openHandleCount(), 0U);
  }
}

TEST(PdfImagePreparation, InlineImageUsesDeclaredRawByteCountInsteadOfStoppingAtEmbeddedEiBytes) {
  PreparationHarness harness;
  harness.storage.addFile("/books/inline-image.pdf", loadFixture("inline_image_caption.pdf"), 1234, true);
  PdfPreparation preparation;
  ASSERT_TRUE(preparation.begin(harness.config("/books/inline-image.pdf")).ok());

  const PdfStepResult result = runToTerminal(preparation, harness);

  ASSERT_TRUE(result.complete()) << static_cast<int>(result.status.error) << "@" << result.status.offset;
  const std::string generationRoot =
      std::string(preparation.cacheRoot()) + "/gen_" + std::to_string(preparation.generation());
  const auto paths = harness.storage.paths();
  const auto pixelCachePath = std::find_if(paths.begin(), paths.end(), [&](const std::string& path) {
    return path.starts_with(generationRoot + "/images/") && path.ends_with(".pxc");
  });
  ASSERT_NE(pixelCachePath, paths.end());
  const std::vector<uint8_t> expectedPixelCache = {5, 0, 1, 0, 0x05, 0x00};
  EXPECT_EQ(harness.storage.bytes(*pixelCachePath), expectedPixelCache);
  const std::string sectionPath = generationRoot + "/sections/000000.xhtml";
  const std::string xhtml(harness.storage.bytes(sectionPath).begin(), harness.storage.bytes(sectionPath).end());
  EXPECT_NE(xhtml.find("Inline caption."), std::string::npos);
  EXPECT_NE(xhtml.find("<img "), std::string::npos);
  EXPECT_EQ(harness.storage.openHandleCount(), 0U);
}

TEST(PdfImagePreparation, InlineIndexedDecodeAndDecodeParmsReachThePixelCache) {
  PreparationHarness harness;
  constexpr char sourcePath[] = "/books/inline-indexed-decode-parms.pdf";
  harness.storage.addFile(sourcePath, loadFixture("inline_indexed_decode_parms_caption.pdf"), 1234, true);
  PdfPreparation preparation;
  ASSERT_TRUE(preparation.begin(harness.config(sourcePath)).ok());

  const PdfStepResult result = runToTerminal(preparation, harness);

  ASSERT_TRUE(result.complete()) << static_cast<int>(result.status.error) << "@" << result.status.offset;
  const std::string generationRoot =
      std::string(preparation.cacheRoot()) + "/gen_" + std::to_string(preparation.generation());
  const std::vector<std::string> paths = harness.storage.paths();
  const auto pixelCachePath = std::find_if(paths.begin(), paths.end(), [&](const std::string& path) {
    return path.starts_with(generationRoot + "/images/") && path.ends_with(".pxc");
  });
  ASSERT_NE(pixelCachePath, paths.end());
  const std::vector<uint8_t> expectedPixelCache = {4, 0, 1, 0, 0xE4};
  EXPECT_EQ(harness.storage.bytes(*pixelCachePath), expectedPixelCache);
  const std::string sectionPath = generationRoot + "/sections/000000.xhtml";
  const std::string xhtml(harness.storage.bytes(sectionPath).begin(), harness.storage.bytes(sectionPath).end());
  EXPECT_NE(xhtml.find("Inline indexed predictor caption."), std::string::npos);
  EXPECT_NE(xhtml.find("<img "), std::string::npos);
  EXPECT_EQ(harness.storage.openHandleCount(), 0U);
}

TEST(PdfImagePreparation, FilteredInlineImagesUseCodecBoundariesInsteadOfEmbeddedEiBytes) {
  struct Case {
    const char* fixture;
    const char* sourcePath;
    const char* caption;
  };
  const Case cases[] = {
      {"inline_asciihex_boundary.pdf", "/books/inline-asciihex.pdf", "Inline asciihex boundary survives."},
      {"inline_ascii85_boundary.pdf", "/books/inline-ascii85.pdf", "Inline ascii85 boundary survives."},
      {"inline_flate_boundary.pdf", "/books/inline-flate.pdf", "Inline flate boundary survives."},
      {"inline_dct_boundary.pdf", "/books/inline-dct.pdf", "Inline dct boundary survives."},
  };
  for (const Case& testCase : cases) {
    PreparationHarness harness;
    harness.storage.addFile(testCase.sourcePath, loadFixture(testCase.fixture), 1234, true);
    PdfPreparation preparation;
    ASSERT_TRUE(preparation.begin(harness.config(testCase.sourcePath)).ok());

    PdfPreparationPhase terminalFrom = preparation.phase();
    PdfStepResult result = PdfStepResult::failure(PdfStatus::failure(PdfError::BudgetExhausted));
    for (uint32_t stepIndex = 0; stepIndex < 20000; ++stepIndex) {
      terminalFrom = preparation.phase();
      result = preparation.step();
      ++harness.nowMs;
      if (!result.yielded()) {
        break;
      }
    }

    ASSERT_TRUE(result.complete()) << testCase.fixture << " error=" << static_cast<int>(result.status.error) << "@"
                                   << result.status.offset << " from=" << static_cast<int>(terminalFrom)
                                   << " phase=" << static_cast<int>(preparation.phase());
    const std::string generationRoot =
        std::string(preparation.cacheRoot()) + "/gen_" + std::to_string(preparation.generation());
    const std::string sectionPath = generationRoot + "/sections/000000.xhtml";
    const std::vector<uint8_t>& sectionBytes = harness.storage.bytes(sectionPath);
    const std::string xhtml(sectionBytes.begin(), sectionBytes.end());
    EXPECT_NE(xhtml.find(testCase.caption), std::string::npos) << testCase.fixture;
    EXPECT_NE(xhtml.find("<img "), std::string::npos) << testCase.fixture;
    EXPECT_EQ(harness.storage.openHandleCount(), 0U) << testCase.fixture;
  }
}

TEST(PdfImagePreparation, InlineDctSourceBytesAreReadOnceWhileScanningAndCaching) {
  PreparationHarness harness;
  harness.storage.setMaximumReadHandles(1);
  constexpr char sourcePath[] = "/books/inline-dct-one-pass.pdf";
  const std::vector<uint8_t> fixture = loadFixture("inline_dct_one_pass.pdf");
  const uint8_t jpegStartMarker[] = {0xFF, 0xD8};
  const uint8_t jpegEndMarker[] = {0xFF, 0xD9};
  const auto jpegBegin =
      std::search(fixture.begin(), fixture.end(), std::begin(jpegStartMarker), std::end(jpegStartMarker));
  ASSERT_NE(jpegBegin, fixture.end());
  const auto jpegEnd = std::search(jpegBegin + 2, fixture.end(), std::begin(jpegEndMarker), std::end(jpegEndMarker));
  ASSERT_NE(jpegEnd, fixture.end());
  const uint64_t jpegOffset = static_cast<uint64_t>(std::distance(fixture.begin(), jpegBegin));
  const uint64_t jpegLength = static_cast<uint64_t>(std::distance(jpegBegin, jpegEnd)) + 2U;
  ASSERT_GT(jpegLength, PdfLimits::SourceBufferBytes);
  harness.storage.addFile(sourcePath, fixture, 1234, true);
  harness.chargeIoTime = true;
  PdfPreparation preparation;
  ASSERT_TRUE(preparation.begin(harness.config(sourcePath)).ok());

  for (uint32_t slice = 0; slice < 20000 && preparation.phase() != PdfPreparationPhase::ExtractText; ++slice) {
    const PreparationStepObservation observation = observePreparationStep(preparation, harness);
    expectBoundedPreparationStep(observation);
    ASSERT_TRUE(observation.result.yielded())
        << static_cast<int>(observation.result.status.error) << "@" << observation.result.status.offset;
  }
  ASSERT_EQ(preparation.phase(), PdfPreparationPhase::ExtractText);
  harness.storage.clearReadObservations();

  PdfStepResult terminal = PdfStepResult::failure(PdfStatus::failure(PdfError::BudgetExhausted));
  PdfPreparationPhase terminalFrom = preparation.phase();
  for (uint32_t slice = 0; slice < 30000; ++slice) {
    terminalFrom = preparation.phase();
    const PreparationStepObservation observation = observePreparationStep(preparation, harness);
    expectBoundedPreparationStep(observation);
    terminal = observation.result;
    if (!terminal.yielded()) {
      break;
    }
  }

  ASSERT_TRUE(terminal.complete()) << static_cast<int>(terminal.status.error) << "@" << terminal.status.offset
                                   << " from=" << static_cast<int>(terminalFrom)
                                   << " phase=" << static_cast<int>(preparation.phase());
  expectSourceRangeReadOnce(harness.storage.readObservations(), sourcePath, jpegOffset, jpegLength);
  const std::string generationRoot =
      std::string(preparation.cacheRoot()) + "/gen_" + std::to_string(preparation.generation());
  const std::vector<std::string> paths = harness.storage.paths();
  const auto jpegPath = std::find_if(paths.begin(), paths.end(), [&](const std::string& path) {
    return path.starts_with(generationRoot + "/images/") && path.ends_with(".jpg");
  });
  ASSERT_NE(jpegPath, paths.end());
  EXPECT_EQ(harness.storage.bytes(*jpegPath), std::vector<uint8_t>(jpegBegin, jpegEnd + 2));
  EXPECT_EQ(harness.storage.openHandleCount(), 0U);
}

TEST(PdfImagePreparation, CancellationDuringInlineDctCaptureRemovesThePartialAndRetainsAResumableGeneration) {
  PreparationHarness harness;
  harness.storage.setMaximumReadHandles(1);
  constexpr char sourcePath[] = "/books/cancel-inline-dct.pdf";
  harness.storage.addFile(sourcePath, loadFixture("inline_dct_one_pass.pdf"), 1234, true);
  harness.chargeIoTime = true;
  PdfPreparation preparation;
  ASSERT_TRUE(preparation.begin(harness.config(sourcePath)).ok());

  bool partialCaptureWritten = false;
  std::string generationRoot;
  for (uint32_t slice = 0; slice < 20000 && !partialCaptureWritten; ++slice) {
    const PreparationStepObservation observation = observePreparationStep(preparation, harness);
    expectBoundedPreparationStep(observation);
    ASSERT_TRUE(observation.result.yielded())
        << static_cast<int>(observation.result.status.error) << "@" << observation.result.status.offset;
    if (preparation.generation() == 0) {
      continue;
    }
    generationRoot = std::string(preparation.cacheRoot()) + "/gen_" + std::to_string(preparation.generation());
    const std::vector<std::string> paths = harness.storage.paths();
    const auto partial = std::find_if(paths.begin(), paths.end(), [&](const std::string& path) {
      return path.starts_with(generationRoot + "/images/build-inline-") && path.ends_with(".tmp") &&
             !harness.storage.bytes(path).empty();
    });
    partialCaptureWritten = partial != paths.end();
  }
  ASSERT_TRUE(partialCaptureWritten);
  ASSERT_EQ(preparation.phase(), PdfPreparationPhase::ExtractText);

  const uint32_t cancelledGeneration = preparation.generation();
  const PdfStepResult cancelled = cancelToTerminalBounded(preparation, harness);

  ASSERT_TRUE(cancelled.failed());
  EXPECT_EQ(cancelled.status.error, PdfError::Cancelled);
  EXPECT_EQ(cancelledGeneration, preparation.generation());
  expectRetainedCancelledGeneration(harness.storage, preparation.cacheRoot(), preparation.sourceIdentity(),
                                    cancelledGeneration);
  EXPECT_EQ(harness.storage.openHandleCount(), 0U);
  expectFreshGenerationRestart(harness, sourcePath, cancelledGeneration);
}

TEST(PdfImagePreparation, LivePlacementTrackingSuppressesAThreeUseLogoBeforeRasterDecode) {
  PreparationHarness harness;
  harness.storage.addFile("/books/repeated-logo.pdf", loadFixture("repeated_logo_caption.pdf"), 1234, true);
  PdfPreparation preparation;
  ASSERT_TRUE(preparation.begin(harness.config("/books/repeated-logo.pdf")).ok());

  const PdfStepResult result = runToTerminal(preparation, harness);

  ASSERT_TRUE(result.complete()) << static_cast<int>(result.status.error) << "@" << result.status.offset;
  const std::string generationRoot =
      std::string(preparation.cacheRoot()) + "/gen_" + std::to_string(preparation.generation());
  const auto paths = harness.storage.paths();
  EXPECT_EQ(std::find_if(paths.begin(), paths.end(),
                         [&](const std::string& path) { return path.starts_with(generationRoot + "/images/"); }),
            paths.end());
  const std::string sectionPath = generationRoot + "/sections/000000.xhtml";
  const std::string xhtml(harness.storage.bytes(sectionPath).begin(), harness.storage.bytes(sectionPath).end());
  EXPECT_NE(xhtml.find("Repeated logo caption."), std::string::npos);
  EXPECT_EQ(xhtml.find("<img "), std::string::npos);
  PdfCacheStore cache;
  ASSERT_TRUE(cache.initialize(harness.storage.io(), preparation.cacheRoot()).ok());
  PdfCacheManifestSelection selection{};
  ASSERT_TRUE(cache.loadManifestSlots(preparation.sourceIdentity(), &selection).ok());
  ASSERT_TRUE(selection.selected);
  EXPECT_NE(selection.manifest.warningFlags, 0U);
  EXPECT_EQ(selection.manifest.requiredFileCount, 5U);
  EXPECT_EQ(preparation.navigationSpoolWriteCount(), 0U);
  EXPECT_EQ(harness.storage.openHandleCount(), 0U);
}

TEST(PdfImagePreparation, TypographyFallbackCoverAndThumbnailAreRequiredAndReopenAfterReboot) {
  PreparationHarness harness;
  harness.storage.addFile("/books/unsupported-jpx.pdf", loadFixture("unsupported_jpx_caption.pdf"), 1234, true);
  PdfPreparation preparation;
  ASSERT_TRUE(preparation.begin(harness.config("/books/unsupported-jpx.pdf")).ok());

  const PdfStepResult result = runToTerminal(preparation, harness);

  ASSERT_TRUE(result.complete()) << static_cast<int>(result.status.error) << "@" << result.status.offset;
  const std::string generationRoot =
      std::string(preparation.cacheRoot()) + "/gen_" + std::to_string(preparation.generation());
  const std::string coverPath = generationRoot + "/cover.bmp";
  const std::string thumbPath = generationRoot + "/thumb.bmp";
  ASSERT_TRUE(harness.storage.exists(coverPath));
  ASSERT_TRUE(harness.storage.exists(thumbPath));
  ASSERT_GE(harness.storage.bytes(coverPath).size(), 62U);
  ASSERT_GE(harness.storage.bytes(thumbPath).size(), 62U);
  EXPECT_EQ(harness.storage.bytes(coverPath)[0], 'B');
  EXPECT_EQ(harness.storage.bytes(coverPath)[1], 'M');
  EXPECT_EQ(harness.storage.bytes(thumbPath)[0], 'B');
  EXPECT_EQ(harness.storage.bytes(thumbPath)[1], 'M');

  PdfCacheStore rebooted;
  ASSERT_TRUE(rebooted.initialize(harness.storage.io(), preparation.cacheRoot()).ok());
  PdfCacheManifestSelection selection{};
  ASSERT_TRUE(rebooted.loadManifestSlots(preparation.sourceIdentity(), &selection).ok());
  ASSERT_TRUE(selection.selected);
  EXPECT_EQ(selection.manifest.requiredFileCount, 5U);
  EXPECT_EQ(harness.storage.openHandleCount(), 0U);
}

TEST(PdfImagePreparation, MeaningfulEarlyRasterProducesImageDerivedCoverAndThumbnailWithinBoundedSteps) {
  PreparationHarness harness;
  harness.storage.setMaximumReadHandles(1);
  constexpr char sourcePath[] = "/books/raster-cover.pdf";
  harness.storage.addFile(sourcePath, loadFixture("raster_cover_caption.pdf"), 1234, true);
  harness.chargeIoTime = true;
  PdfPreparation preparation;
  ASSERT_TRUE(preparation.begin(harness.config(sourcePath)).ok());

  for (uint32_t slice = 0; slice < 30000; ++slice) {
    const PreparationStepObservation observation = observePreparationStep(preparation, harness);
    expectBoundedPreparationStep(observation);
    if (!observation.result.yielded()) {
      ASSERT_TRUE(observation.result.complete())
          << static_cast<int>(observation.result.status.error) << "@" << observation.result.status.offset
          << " phase=" << static_cast<int>(preparation.phase());
      break;
    }
    ++harness.nowMs;
    ASSERT_LT(slice, 29999U);
  }

  const std::string generationRoot =
      std::string(preparation.cacheRoot()) + "/gen_" + std::to_string(preparation.generation());
  const auto paths = harness.storage.paths();
  const auto pixelCachePath = std::find_if(paths.begin(), paths.end(), [&](const std::string& path) {
    return path.starts_with(generationRoot + "/images/") && path.ends_with(".pxc");
  });
  ASSERT_NE(pixelCachePath, paths.end());
  EXPECT_EQ(harness.storage.bytes(*pixelCachePath), (std::vector<uint8_t>{4, 0, 4, 0, 0x0F, 0x0F, 0x0F, 0x0F}));

  const auto bmpPixel = [](const std::vector<uint8_t>& bmp, const uint16_t width, const uint16_t x, const uint16_t y) {
    const uint32_t pixelOffset = static_cast<uint32_t>(bmp[10]) | static_cast<uint32_t>(bmp[11]) << 8U |
                                 static_cast<uint32_t>(bmp[12]) << 16U | static_cast<uint32_t>(bmp[13]) << 24U;
    const uint32_t rowBytes = ((static_cast<uint32_t>(width) + 31U) / 32U) * 4U;
    const uint8_t packed = bmp[pixelOffset + static_cast<uint32_t>(y) * rowBytes + x / 8U];
    return static_cast<uint8_t>((packed >> (7U - x % 8U)) & 0x01U);
  };
  const std::vector<uint8_t>& cover = harness.storage.bytes(generationRoot + "/cover.bmp");
  const std::vector<uint8_t>& thumb = harness.storage.bytes(generationRoot + "/thumb.bmp");
  ASSERT_EQ(cover.size(), 62U + 32U * 400U);
  ASSERT_EQ(thumb.size(), 62U + 12U * 160U);
  EXPECT_EQ(bmpPixel(cover, 240, 20, 79), 1U);
  EXPECT_EQ(bmpPixel(cover, 240, 20, 80), 0U);
  EXPECT_EQ(bmpPixel(cover, 240, 119, 200), 0U);
  EXPECT_EQ(bmpPixel(cover, 240, 120, 200), 1U);
  EXPECT_EQ(bmpPixel(cover, 240, 20, 320), 1U);
  EXPECT_EQ(bmpPixel(thumb, 96, 10, 31), 1U);
  EXPECT_EQ(bmpPixel(thumb, 96, 47, 80), 0U);
  EXPECT_EQ(bmpPixel(thumb, 96, 48, 80), 1U);
  EXPECT_EQ(harness.storage.openHandleCount(), 0U);
}

TEST(PdfImagePreparation, FirstRetainedMeaningfulEarlyRasterBecomesCoverAfterDiscardedTinyCandidate) {
  PreparationHarness harness;
  harness.storage.setMaximumReadHandles(1);
  constexpr char sourcePath[] = "/books/discarded-then-raster-cover.pdf";
  harness.storage.addFile(sourcePath, loadFixture("discarded_then_raster_cover.pdf"), 1234, true);
  PdfPreparation preparation;
  ASSERT_TRUE(preparation.begin(harness.config(sourcePath)).ok());

  const PdfStepResult result = runToTerminal(preparation, harness);

  ASSERT_TRUE(result.complete()) << static_cast<int>(result.status.error) << "@" << result.status.offset;
  const std::string generationRoot =
      std::string(preparation.cacheRoot()) + "/gen_" + std::to_string(preparation.generation());
  const std::vector<uint8_t>& cover = harness.storage.bytes(generationRoot + "/cover.bmp");
  const std::vector<uint8_t>& thumb = harness.storage.bytes(generationRoot + "/thumb.bmp");
  ASSERT_EQ(cover.size(), 62U + 32U * 400U);
  ASSERT_EQ(thumb.size(), 62U + 12U * 160U);
  const auto bmpPixel = [](const std::vector<uint8_t>& bmp, const uint16_t width, const uint16_t x, const uint16_t y) {
    const uint32_t pixelOffset = static_cast<uint32_t>(bmp[10]) | static_cast<uint32_t>(bmp[11]) << 8U |
                                 static_cast<uint32_t>(bmp[12]) << 16U | static_cast<uint32_t>(bmp[13]) << 24U;
    const uint32_t rowBytes = ((static_cast<uint32_t>(width) + 31U) / 32U) * 4U;
    return static_cast<uint8_t>((bmp[pixelOffset + static_cast<uint32_t>(y) * rowBytes + x / 8U] >> (7U - x % 8U)) &
                                0x01U);
  };
  EXPECT_EQ(bmpPixel(cover, 240, 119, 200), 0U);
  EXPECT_EQ(bmpPixel(cover, 240, 120, 200), 1U);
  EXPECT_EQ(bmpPixel(thumb, 96, 47, 80), 0U);
  EXPECT_EQ(bmpPixel(thumb, 96, 48, 80), 1U);
  EXPECT_EQ(harness.storage.openHandleCount(), 0U);
}

TEST(PdfImagePreparation, MeaningfulEarlyJpegProducesImageDerivedCoverAndThumbnail) {
  PreparationHarness harness;
  harness.storage.setMaximumReadHandles(1);
  constexpr char sourcePath[] = "/books/jpeg-cover.pdf";
  harness.storage.addFile(sourcePath, loadFixture("jpeg_cover_caption.pdf"), 1234, true);
  PdfPreparation preparation;
  ASSERT_TRUE(preparation.begin(harness.config(sourcePath)).ok());

  const PdfStepResult result = runToTerminal(preparation, harness);

  ASSERT_TRUE(result.complete()) << static_cast<int>(result.status.error) << "@" << result.status.offset;
  const std::string generationRoot =
      std::string(preparation.cacheRoot()) + "/gen_" + std::to_string(preparation.generation());
  const std::vector<uint8_t>& cover = harness.storage.bytes(generationRoot + "/cover.bmp");
  const std::vector<uint8_t>& thumb = harness.storage.bytes(generationRoot + "/thumb.bmp");
  ASSERT_EQ(cover.size(), 62U + 32U * 400U);
  ASSERT_EQ(thumb.size(), 62U + 12U * 160U);
  const auto bmpPixel = [](const std::vector<uint8_t>& bmp, const uint16_t width, const uint16_t x, const uint16_t y) {
    const uint32_t pixelOffset = static_cast<uint32_t>(bmp[10]) | static_cast<uint32_t>(bmp[11]) << 8U |
                                 static_cast<uint32_t>(bmp[12]) << 16U | static_cast<uint32_t>(bmp[13]) << 24U;
    const uint32_t rowBytes = ((static_cast<uint32_t>(width) + 31U) / 32U) * 4U;
    return static_cast<uint8_t>((bmp[pixelOffset + static_cast<uint32_t>(y) * rowBytes + x / 8U] >> (7U - x % 8U)) &
                                0x01U);
  };
  EXPECT_EQ(bmpPixel(cover, 240, 119, 200), 0U);
  EXPECT_EQ(bmpPixel(cover, 240, 120, 200), 1U);
  EXPECT_EQ(bmpPixel(thumb, 96, 47, 80), 0U);
  EXPECT_EQ(bmpPixel(thumb, 96, 48, 80), 1U);
  EXPECT_EQ(harness.storage.openHandleCount(), 0U);
}

TEST(PdfImagePreparation, CancellationDuringJpegPreviewHeaderOrRowLeavesNoResidueAndFreshRetryCompletes) {
  enum class CancellationPoint : uint8_t { Header, Row };
  for (const CancellationPoint point : {CancellationPoint::Header, CancellationPoint::Row}) {
    SCOPED_TRACE(point == CancellationPoint::Header ? "header" : "row");
    PreparationHarness harness;
    harness.storage.setMaximumReadHandles(1);
    constexpr char sourcePath[] = "/books/cancel-jpeg-preview.pdf";
    harness.storage.addFile(sourcePath, loadFixture("jpeg_cover_caption.pdf"), 1234, true);
    PdfPreparation preparation;
    ASSERT_TRUE(preparation.begin(harness.config(sourcePath)).ok());

    std::string generationRoot;
    std::string jpegPath;
    bool reachedCancellationPoint = false;
    for (uint32_t slice = 0; slice < 30000 && !reachedCancellationPoint; ++slice) {
      const PreparationStepObservation observation = observePreparationStep(preparation, harness);
      expectBoundedPreparationStep(observation);
      ASSERT_TRUE(observation.result.yielded())
          << static_cast<int>(observation.result.status.error) << "@" << observation.result.status.offset
          << " phase=" << static_cast<int>(preparation.phase());
      ++harness.nowMs;
      if (preparation.generation() == 0) {
        continue;
      }
      generationRoot = std::string(preparation.cacheRoot()) + "/gen_" + std::to_string(preparation.generation());
      if (jpegPath.empty()) {
        const std::vector<std::string> paths = harness.storage.paths();
        const auto jpeg = std::find_if(paths.begin(), paths.end(), [&](const std::string& path) {
          return path.starts_with(generationRoot + "/images/") && path.ends_with(".jpg");
        });
        if (jpeg != paths.end()) {
          jpegPath = *jpeg;
        }
      }
      const std::string coverPath = generationRoot + "/cover.bmp";
      if (point == CancellationPoint::Header) {
        reachedCancellationPoint = !jpegPath.empty() &&
                                   observedPathBytes(harness.storage.readObservations(), jpegPath) != 0 &&
                                   !harness.storage.exists(coverPath);
      } else {
        reachedCancellationPoint = harness.storage.exists(coverPath) && harness.storage.bytes(coverPath).size() > 62U;
      }
    }
    ASSERT_TRUE(reachedCancellationPoint);
    const uint32_t cancelledGeneration = preparation.generation();

    const PdfStepResult cancelled = cancelToTerminalBounded(preparation, harness);

    ASSERT_TRUE(cancelled.failed());
    EXPECT_EQ(cancelled.status.error, PdfError::Cancelled);
    EXPECT_EQ(harness.storage.openHandleCount(), 0U);
    EXPECT_FALSE(harness.storage.exists(generationRoot + "/cover.bmp"));
    EXPECT_FALSE(harness.storage.exists(generationRoot + "/thumb.bmp"));
    for (const std::string& path : harness.storage.paths()) {
      if (path.starts_with(generationRoot)) {
        EXPECT_FALSE(path.ends_with(".tmp")) << path;
        EXPECT_EQ(path.find("/build-"), std::string::npos) << path;
      }
    }
    expectRetainedCancelledGeneration(harness.storage, preparation.cacheRoot(), preparation.sourceIdentity(),
                                      cancelledGeneration);
    expectFreshGenerationRestart(harness, sourcePath, cancelledGeneration);
  }
}

TEST(PdfImagePreparation, PreviewUnsupportedJpegCoversFallBackToTypographyWithoutDroppingTextOrImage) {
  struct TestCase {
    const char* fixture;
    const char* sourcePath;
    const char* caption;
    uint8_t frameMarker;
  };
  constexpr TestCase testCases[] = {
      {"progressive_jpeg_cover_caption.pdf", "/books/progressive-jpeg-cover.pdf", "Progressive JPEG cover caption.",
       0xc2},
      {"sof1_jpeg_cover_caption.pdf", "/books/sof1-jpeg-cover.pdf", "SOF1 JPEG cover caption.", 0xc1},
  };

  for (const TestCase& testCase : testCases) {
    SCOPED_TRACE(testCase.fixture);
    PreparationHarness harness;
    harness.storage.setMaximumReadHandles(1);
    const std::vector<uint8_t> fixture = loadFixture(testCase.fixture);
    const uint8_t jpegStartMarker[] = {0xff, 0xd8};
    const uint8_t jpegEndMarker[] = {0xff, 0xd9};
    const uint8_t frameMarker[] = {0xff, testCase.frameMarker};
    const auto jpegBegin =
        std::search(fixture.begin(), fixture.end(), std::begin(jpegStartMarker), std::end(jpegStartMarker));
    ASSERT_NE(jpegBegin, fixture.end());
    const auto jpegEnd = std::search(jpegBegin + 2, fixture.end(), std::begin(jpegEndMarker), std::end(jpegEndMarker));
    ASSERT_NE(jpegEnd, fixture.end());
    ASSERT_NE(std::search(jpegBegin, jpegEnd, std::begin(frameMarker), std::end(frameMarker)), jpegEnd);
    const uint64_t jpegOffset = static_cast<uint64_t>(std::distance(fixture.begin(), jpegBegin));
    const uint64_t jpegLength = static_cast<uint64_t>(std::distance(jpegBegin, jpegEnd)) + 2U;
    const std::vector<uint8_t> jpeg(jpegBegin, jpegEnd + 2);
    harness.storage.addFile(testCase.sourcePath, fixture, 1234, true);
    PdfPreparation preparation;
    ASSERT_TRUE(preparation.begin(harness.config(testCase.sourcePath)).ok());

    for (uint32_t slice = 0; slice < 20000 && preparation.phase() != PdfPreparationPhase::CacheImage; ++slice) {
      const PreparationStepObservation observation = observePreparationStep(preparation, harness);
      expectBoundedPreparationStep(observation);
      ASSERT_TRUE(observation.result.yielded())
          << static_cast<int>(observation.result.status.error) << "@" << observation.result.status.offset
          << " phase=" << static_cast<int>(preparation.phase());
      ++harness.nowMs;
    }
    ASSERT_EQ(preparation.phase(), PdfPreparationPhase::CacheImage);
    harness.storage.clearReadObservations();

    PdfStepResult terminal = PdfStepResult::failure(PdfStatus::failure(PdfError::BudgetExhausted));
    bool navigationRecordsPrepared = false;
    for (uint32_t slice = 0; slice < 30000; ++slice) {
      const PdfPreparationPhase phaseBefore = preparation.phase();
      harness.chargeIoTime = preparation.phase() == PdfPreparationPhase::CacheImage ||
                             (preparation.phase() == PdfPreparationPhase::CloseSource && navigationRecordsPrepared);
      const PreparationStepObservation observation = observePreparationStep(preparation, harness);
      if (phaseBefore == PdfPreparationPhase::CloseSource && !navigationRecordsPrepared) {
        EXPECT_LE(observation.operations, 32U);
        EXPECT_LE(observation.bytesRead, PdfLimits::SourceBufferBytes);
        EXPECT_LE(observation.bytesWritten, PdfLimits::SourceBufferBytes);
      } else {
        expectBoundedPreparationStep(observation);
      }
      if (phaseBefore == PdfPreparationPhase::CloseSource) {
        navigationRecordsPrepared = true;
      }
      terminal = observation.result;
      if (!terminal.yielded()) {
        break;
      }
      ++harness.nowMs;
    }

    ASSERT_TRUE(terminal.complete()) << static_cast<int>(terminal.status.error) << "@" << terminal.status.offset
                                     << " phase=" << static_cast<int>(preparation.phase());
    expectSourceRangeReadOnce(harness.storage.readObservations(), testCase.sourcePath, jpegOffset, jpegLength);
    const std::string generationRoot =
        std::string(preparation.cacheRoot()) + "/gen_" + std::to_string(preparation.generation());
    const std::vector<std::string> paths = harness.storage.paths();
    const auto jpegPath = std::find_if(paths.begin(), paths.end(), [&](const std::string& path) {
      return path.starts_with(generationRoot + "/images/") && path.ends_with(".jpg");
    });
    ASSERT_NE(jpegPath, paths.end());
    EXPECT_EQ(harness.storage.bytes(*jpegPath), jpeg);

    const std::string sectionPath = generationRoot + "/sections/000000.xhtml";
    const std::string xhtml(harness.storage.bytes(sectionPath).begin(), harness.storage.bytes(sectionPath).end());
    EXPECT_NE(xhtml.find(testCase.caption), std::string::npos);
    EXPECT_NE(xhtml.find("<img "), std::string::npos);

    const std::string coverPath = generationRoot + "/cover.bmp";
    const std::string thumbPath = generationRoot + "/thumb.bmp";
    const std::vector<uint8_t>& cover = harness.storage.bytes(coverPath);
    const std::vector<uint8_t>& thumb = harness.storage.bytes(thumbPath);
    ASSERT_EQ(cover.size(), 62U + 32U * 400U);
    ASSERT_EQ(thumb.size(), 62U + 12U * 160U);
    const auto bmpPixel = [](const std::vector<uint8_t>& bmp, const uint16_t width, const uint16_t x,
                             const uint16_t y) {
      const uint32_t pixelOffset = static_cast<uint32_t>(bmp[10]) | static_cast<uint32_t>(bmp[11]) << 8U |
                                   static_cast<uint32_t>(bmp[12]) << 16U | static_cast<uint32_t>(bmp[13]) << 24U;
      const uint32_t rowBytes = ((static_cast<uint32_t>(width) + 31U) / 32U) * 4U;
      return static_cast<uint8_t>((bmp[pixelOffset + static_cast<uint32_t>(y) * rowBytes + x / 8U] >> (7U - x % 8U)) &
                                  0x01U);
    };
    const auto hasBlackPixel = [&](const std::vector<uint8_t>& bmp, const uint16_t width, const uint16_t firstRow,
                                   const uint16_t rowCount) {
      for (uint16_t row = firstRow; row < static_cast<uint16_t>(firstRow + rowCount); ++row) {
        for (uint16_t x = 0; x < width; ++x) {
          if (bmpPixel(bmp, width, x, row) == 0U) {
            return true;
          }
        }
      }
      return false;
    };
    EXPECT_TRUE(hasBlackPixel(cover, 240, 80, 21));
    EXPECT_TRUE(hasBlackPixel(thumb, 96, 32, 7));
    EXPECT_EQ(bmpPixel(cover, 240, 20, 200), 1U);
    EXPECT_EQ(bmpPixel(cover, 240, 119, 200), 1U);
    EXPECT_EQ(bmpPixel(thumb, 96, 10, 80), 1U);
    for (const std::string& path : paths) {
      if (path.starts_with(generationRoot)) {
        EXPECT_FALSE(path.ends_with(".tmp")) << path;
        EXPECT_EQ(path.find("/build-"), std::string::npos) << path;
      }
    }

    PdfCacheStore rebooted;
    ASSERT_TRUE(rebooted.initialize(harness.storage.io(), preparation.cacheRoot()).ok());
    PdfCacheManifestSelection selection{};
    ASSERT_TRUE(rebooted.loadManifestSlots(preparation.sourceIdentity(), &selection).ok());
    ASSERT_TRUE(selection.selected);
    EXPECT_EQ(selection.manifest.requiredFileCount, 6U);
    EXPECT_EQ(preparation.navigationSpoolWriteCount(), 0U);
    EXPECT_EQ(harness.storage.openHandleCount(), 0U);
  }
}

TEST(PdfImagePreparation, MalformedJpegCoverRemainsFailClosedWithoutPartialCoverAssets) {
  PreparationHarness harness;
  harness.storage.setMaximumReadHandles(1);
  constexpr char sourcePath[] = "/books/malformed-jpeg-cover.pdf";
  std::vector<uint8_t> fixture = loadFixture("progressive_jpeg_cover_caption.pdf");
  const uint8_t jpegStartMarker[] = {0xff, 0xd8};
  const auto jpegBegin =
      std::search(fixture.begin(), fixture.end(), std::begin(jpegStartMarker), std::end(jpegStartMarker));
  ASSERT_NE(jpegBegin, fixture.end());
  *jpegBegin = 0;
  harness.storage.addFile(sourcePath, fixture, 1234, true);
  PdfPreparation preparation;
  ASSERT_TRUE(preparation.begin(harness.config(sourcePath)).ok());

  PdfStepResult terminal = PdfStepResult::failure(PdfStatus::failure(PdfError::BudgetExhausted));
  for (uint32_t slice = 0; slice < 30000; ++slice) {
    const PreparationStepObservation observation = observePreparationStep(preparation, harness);
    expectBoundedPreparationStep(observation);
    terminal = observation.result;
    if (!terminal.yielded()) {
      break;
    }
    ++harness.nowMs;
  }

  ASSERT_TRUE(terminal.failed());
  EXPECT_EQ(terminal.status.error, PdfError::Malformed);
  const std::string generationRoot =
      std::string(preparation.cacheRoot()) + "/gen_" + std::to_string(preparation.generation());
  EXPECT_FALSE(harness.storage.exists(generationRoot + "/cover.bmp"));
  EXPECT_FALSE(harness.storage.exists(generationRoot + "/thumb.bmp"));
  PdfCacheStore cache;
  ASSERT_TRUE(cache.initialize(harness.storage.io(), preparation.cacheRoot()).ok());
  PdfCacheManifestSelection selection{};
  ASSERT_TRUE(cache.loadManifestSlots(preparation.sourceIdentity(), &selection).ok());
  EXPECT_FALSE(selection.selected);
  EXPECT_EQ(harness.storage.openHandleCount(), 0U);
}

TEST(PdfImagePreparation, CropOriginAndQuarterTurnAreAppliedBeforeCoverClassification) {
  PreparationHarness harness;
  harness.storage.setMaximumReadHandles(1);
  constexpr char sourcePath[] = "/books/rotated-crop-cover.pdf";
  harness.storage.addFile(sourcePath, loadFixture("rotated_crop_raster_caption.pdf"), 1234, true);
  PdfPreparation preparation;
  ASSERT_TRUE(preparation.begin(harness.config(sourcePath)).ok());

  const PdfStepResult result = runToTerminal(preparation, harness);

  ASSERT_TRUE(result.complete()) << static_cast<int>(result.status.error) << "@" << result.status.offset;
  const std::string generationRoot =
      std::string(preparation.cacheRoot()) + "/gen_" + std::to_string(preparation.generation());
  const std::vector<uint8_t>& cover = harness.storage.bytes(generationRoot + "/cover.bmp");
  ASSERT_GE(cover.size(), 62U + 32U * 201U);
  const uint32_t pixelOffset = static_cast<uint32_t>(cover[10]) | static_cast<uint32_t>(cover[11]) << 8U |
                               static_cast<uint32_t>(cover[12]) << 16U | static_cast<uint32_t>(cover[13]) << 24U;
  const uint8_t left = cover[pixelOffset + 200U * 32U + 119U / 8U];
  const uint8_t right = cover[pixelOffset + 200U * 32U + 120U / 8U];
  EXPECT_EQ((left >> (7U - 119U % 8U)) & 0x01U, 0U);
  EXPECT_EQ((right >> (7U - 120U % 8U)) & 0x01U, 1U);
  EXPECT_EQ(harness.storage.openHandleCount(), 0U);
}

TEST(PdfImagePreparation, TypographyCoverWriteFailurePublishesNoManifestOrPartialAsset) {
  PreparationHarness harness;
  harness.storage.addFile("/books/unsupported-jpx.pdf", loadFixture("unsupported_jpx_caption.pdf"), 1234, true);
  PdfPreparation preparation;
  ASSERT_TRUE(preparation.begin(harness.config("/books/unsupported-jpx.pdf")).ok());
  while (preparation.phase() != PdfPreparationPhase::CloseSource) {
    const PdfStepResult result = preparation.step();
    ++harness.nowMs;
    ASSERT_TRUE(result.yielded());
  }
  harness.storage.fail(PdfTestFaultPoint::Write);

  const PdfStepResult result = runToTerminal(preparation, harness);

  ASSERT_TRUE(result.failed());
  EXPECT_EQ(result.status.error, PdfError::IoFailure);
  const std::string generationRoot =
      std::string(preparation.cacheRoot()) + "/gen_" + std::to_string(preparation.generation());
  EXPECT_FALSE(harness.storage.exists(generationRoot + "/cover.bmp"));
  EXPECT_FALSE(harness.storage.exists(generationRoot + "/thumb.bmp"));
  PdfCacheStore cache;
  ASSERT_TRUE(cache.initialize(harness.storage.io(), preparation.cacheRoot()).ok());
  PdfCacheManifestSelection selection{};
  ASSERT_TRUE(cache.loadManifestSlots(preparation.sourceIdentity(), &selection).ok());
  EXPECT_FALSE(selection.selected);
  EXPECT_EQ(harness.storage.openHandleCount(), 0U);
}

}  // namespace
