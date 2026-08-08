#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <memory>
#include <new>
#include <string>
#include <vector>

#include "PdfCacheStore.h"
#include "PdfContentInterpreter.h"
#include "PdfLimits.h"
#include "PdfObjectParser.h"
#include "PdfPreparation.h"
#include "PdfStreamDecoder.h"
#include "PdfTestCacheIo.h"
#include "PdfTestIo.h"
#include "PdfXref.h"

namespace {

thread_local bool gAllocationWatchActive = false;
thread_local size_t gWatchedAllocationCount = 0;
thread_local size_t gWatchedSourceSizedCount = 0;
thread_local size_t gWatchedSourceSize = 0;
thread_local size_t gNothrowArrayCallCount = 0;
thread_local size_t gFailNothrowArrayCall = 0;
thread_local PdfCacheIo::WriteFn gOriginalCacheWrite = nullptr;
thread_local uint32_t gEnospcWriteOccurrence = 0;
thread_local uint32_t gCacheWriteCount = 0;
thread_local uint32_t gEnospcFailureCount = 0;

void recordAllocation(const size_t size) {
  if (!gAllocationWatchActive) {
    return;
  }
  ++gWatchedAllocationCount;
  if (size == gWatchedSourceSize) {
    ++gWatchedSourceSizedCount;
  }
}

struct AllocationObservation {
  size_t allocationCount = 0;
  size_t sourceSizedCount = 0;
  size_t nothrowArrayCallCount = 0;
};

class AllocationWatch {
 public:
  explicit AllocationWatch(const size_t sourceSize, const size_t failNothrowArrayCall = 0) {
    gWatchedAllocationCount = 0;
    gWatchedSourceSizedCount = 0;
    gWatchedSourceSize = sourceSize;
    gNothrowArrayCallCount = 0;
    gFailNothrowArrayCall = failNothrowArrayCall;
    gAllocationWatchActive = true;
  }

  ~AllocationWatch() {
    gAllocationWatchActive = false;
    gFailNothrowArrayCall = 0;
  }

  AllocationObservation observation() const {
    return {gWatchedAllocationCount, gWatchedSourceSizedCount, gNothrowArrayCallCount};
  }
};

class EnospcWriteFault {
 public:
  EnospcWriteFault(PdfCacheIo io, const uint32_t occurrence) : io_(io) {
    gOriginalCacheWrite = io.write;
    gEnospcWriteOccurrence = occurrence;
    gCacheWriteCount = 0;
    gEnospcFailureCount = 0;
    io_.write = write;
  }

  ~EnospcWriteFault() {
    gOriginalCacheWrite = nullptr;
    gEnospcWriteOccurrence = 0;
    gCacheWriteCount = 0;
    gEnospcFailureCount = 0;
  }

  PdfCacheIo io() const { return io_; }
  uint32_t writeCount() const { return gCacheWriteCount; }
  uint32_t failureCount() const { return gEnospcFailureCount; }

 private:
  static PdfStatus write(void* const context, const PdfCacheHandle handle, const uint8_t* const source,
                         const size_t requested, size_t* const bytesWritten) {
    ++gCacheWriteCount;
    if (gEnospcWriteOccurrence != 0 && gCacheWriteCount == gEnospcWriteOccurrence) {
      ++gEnospcFailureCount;
      if (bytesWritten != nullptr) {
        *bytesWritten = 0;
      }
      return PdfStatus::failure(PdfError::InsufficientStorage);
    }
    return gOriginalCacheWrite == nullptr ? PdfStatus::failure(PdfError::InvalidArgument)
                                          : gOriginalCacheWrite(context, handle, source, requested, bytesWritten);
  }

  PdfCacheIo io_{};
};

std::vector<uint8_t> loadClassicFixture() {
  const std::filesystem::path path =
      std::filesystem::path(__FILE__).parent_path().parent_path() / "pdf_reflow_core" / "fixtures" / "classic_text.pdf";
  std::ifstream input(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

struct XrefHarness {
  std::array<uint8_t, PdfLimits::SourceBufferBytes> sourceBuffer{};
  std::array<PdfValue, 64> values{};
  std::array<PdfDictionaryEntry, 64> dictionaries{};
  std::array<PdfArrayItem, 64> arrays{};
  std::array<uint8_t, 1024> text{};
  PdfObjectArena arena{
      values.data(),       static_cast<uint16_t>(values.size()),
      dictionaries.data(), static_cast<uint16_t>(dictionaries.size()),
      arrays.data(),       static_cast<uint16_t>(arrays.size()),
      text.data(),         static_cast<uint16_t>(text.size()),
  };
  PdfTestRecordStore records{sizeof(PdfXrefEntry), 256};
  PdfXrefTable table{records.store()};
};

struct ParseObservation {
  PdfStepResult result = PdfStepResult::failure(PdfStatus::failure(PdfError::BudgetExhausted));
  AllocationObservation allocations{};
};

ParseObservation parseXrefWithoutHeapAllocations(const std::vector<uint8_t>& bytes) {
  PdfTestByteSource memory(bytes);
  const PdfByteSource source = memory.source();
  XrefHarness harness;

  ParseObservation observation;
  {
    AllocationWatch watch(bytes.size());
    PdfXrefParser parser(source, harness.sourceBuffer.data(), harness.sourceBuffer.size(), harness.arena,
                         harness.table);
    parser.begin();
    for (uint32_t step = 0; step < 16384; ++step) {
      PdfWorkBudget budget{32, 4096};
      observation.result = parser.step(budget);
      if (!observation.result.yielded()) {
        break;
      }
    }
    observation.allocations = watch.observation();
  }
  return observation;
}

bool isInputDerivedXrefResult(const PdfError error) {
  switch (error) {
    case PdfError::None:
    case PdfError::InvalidOffset:
    case PdfError::UnexpectedEof:
    case PdfError::LimitExceeded:
    case PdfError::Unsupported:
    case PdfError::UnsupportedFilter:
    case PdfError::ExpansionLimit:
    case PdfError::Malformed:
    case PdfError::Encrypted:
      return true;
    case PdfError::InvalidArgument:
    case PdfError::IoFailure:
    case PdfError::BudgetExhausted:
    case PdfError::UnsupportedEncoding:
    case PdfError::NoReadableText:
    case PdfError::InsufficientMemory:
    case PdfError::InsufficientStorage:
    case PdfError::Cancelled:
      return false;
  }
  return false;
}

uint32_t nextRandom(uint32_t* const state) {
  uint32_t value = *state;
  value ^= value << 13U;
  value ^= value >> 17U;
  value ^= value << 5U;
  *state = value;
  return value;
}

PdfStepResult parseNestedArrays(const uint8_t depth) {
  std::string input(depth, '[');
  input += '0';
  input.append(depth, ']');
  PdfTestByteSource memory({input.begin(), input.end()});
  const PdfByteSource source = memory.source();
  std::array<uint8_t, 256> sourceBuffer{};
  std::array<PdfValue, 80> values{};
  std::array<PdfDictionaryEntry, 1> dictionaries{};
  std::array<PdfArrayItem, 80> arrays{};
  std::array<uint8_t, 1> text{};
  PdfObjectArena arena{
      values.data(),       static_cast<uint16_t>(values.size()),
      dictionaries.data(), static_cast<uint16_t>(dictionaries.size()),
      arrays.data(),       static_cast<uint16_t>(arrays.size()),
      text.data(),         static_cast<uint16_t>(text.size()),
  };
  PdfLexer lexer(source, sourceBuffer.data(), sourceBuffer.size());
  PdfObjectParser parser(lexer, arena);
  parser.begin();
  for (uint16_t step = 0; step < 512; ++step) {
    PdfWorkBudget budget{1, 1};
    const PdfStepResult result = parser.step(budget);
    if (!result.yielded()) {
      return result;
    }
  }
  return PdfStepResult::failure(PdfStatus::failure(PdfError::BudgetExhausted));
}

struct InterpreterHarness {
  std::array<uint8_t, 128> sourceBuffer{};
  std::array<PdfContentOperand, 4> operands{};
  std::array<PdfContentArrayItem, 4> arrayItems{};
  std::array<uint8_t, 64> scratchText{};
  std::array<uint8_t, 64> markedText{};
  std::array<uint8_t, 64> pageText{};
  std::array<PdfTextRun, 1> runs{};
  std::array<PdfImagePlacement, 1> images{};
  uint32_t documentOperatorCount = 0;
  PdfPageModel model;
  PdfContentInterpreter interpreter;

  InterpreterHarness()
      : model({pageText.data(), pageText.size(), runs.data(), static_cast<uint16_t>(runs.size()), images.data(),
               static_cast<uint16_t>(images.size())}),
        interpreter({sourceBuffer.data(), sourceBuffer.size(), operands.data(), static_cast<uint8_t>(operands.size()),
                     arrayItems.data(), static_cast<uint8_t>(arrayItems.size()), scratchText.data(),
                     static_cast<uint16_t>(scratchText.size()), markedText.data(),
                     static_cast<uint16_t>(markedText.size()), &documentOperatorCount}) {}
};

PdfStepResult runInterpreter(PdfContentInterpreter& interpreter) {
  for (uint16_t step = 0; step < 1024; ++step) {
    PdfWorkBudget budget{4, 64};
    const PdfStepResult result = interpreter.step(budget);
    if (!result.yielded()) {
      return result;
    }
  }
  return PdfStepResult::failure(PdfStatus::failure(PdfError::BudgetExhausted));
}

struct OperatorObservation {
  PdfStepResult result = PdfStepResult::failure(PdfStatus::failure(PdfError::BudgetExhausted));
  uint32_t documentOperatorCount = 0;
  PdfPageWarning warnings = PdfPageWarning::None;
};

OperatorObservation interpretOneDocumentOperator(const uint32_t initialDocumentOperatorCount) {
  PdfTestByteSource input(std::vector<uint8_t>{'n'});
  const PdfByteSource source = input.source();
  const PdfContentResources resources{};
  InterpreterHarness harness;
  harness.documentOperatorCount = initialDocumentOperatorCount;
  OperatorObservation observation;
  const PdfStatus status = harness.interpreter.begin(&source, 1, resources, harness.model);
  observation.result = status.ok() ? runInterpreter(harness.interpreter) : PdfStepResult::failure(status);
  observation.documentOperatorCount = harness.documentOperatorCount;
  observation.warnings = harness.model.warnings();
  return observation;
}

struct FormChain;

struct FormResolverContext {
  FormChain* chain = nullptr;
  uint8_t index = 0;
};

struct FormChain {
  std::vector<std::unique_ptr<PdfTestByteSource>> sources;
  std::vector<FormResolverContext> contexts;
  std::vector<PdfContentResources> resources;

  explicit FormChain(const uint8_t count) {
    sources.reserve(count);
    contexts.resize(count);
    resources.resize(count);
    for (uint8_t index = 0; index < count; ++index) {
      const bool final = index + 1U == count;
      sources.push_back(std::make_unique<PdfTestByteSource>(final ? std::vector<uint8_t>{'n'}
                                                                  : std::vector<uint8_t>{'/', 'F', ' ', 'D', 'o'}));
      contexts[index] = {this, index};
      resources[index] = {&contexts[index], nullptr, resolve};
    }
  }

  static PdfStatus resolve(void* const context, const uint8_t* const name, const size_t length,
                           PdfContentXObject* const object) {
    if (context == nullptr || name == nullptr || object == nullptr || length != 1U || name[0] != 'F') {
      return PdfStatus::failure(PdfError::Malformed);
    }
    auto& resolver = *static_cast<FormResolverContext*>(context);
    if (resolver.chain == nullptr || resolver.index >= resolver.chain->sources.size()) {
      return PdfStatus::failure(PdfError::Malformed);
    }
    const size_t index = resolver.index;
    *object = {};
    object->kind = PdfContentXObjectKind::Form;
    object->reference = {static_cast<uint32_t>(index + 1U), 0};
    object->content = resolver.chain->sources[index]->source();
    object->resources = index + 1U < resolver.chain->resources.size() ? &resolver.chain->resources[index + 1U]
                                                                      : &resolver.chain->resources[index];
    object->bbox = {0, 0, PdfFixed16::fromInteger(100).raw, PdfFixed16::fromInteger(100).raw};
    object->hasBBox = true;
    return PdfStatus::success();
  }
};

struct FormObservation {
  PdfStepResult result = PdfStepResult::failure(PdfStatus::failure(PdfError::BudgetExhausted));
  uint8_t maximumDepth = 0;
  PdfPageWarning warnings = PdfPageWarning::None;
};

FormObservation interpretFormChain(const uint8_t formCount) {
  PdfTestByteSource page(std::vector<uint8_t>{'/', 'F', ' ', 'D', 'o'});
  const PdfByteSource source = page.source();
  FormChain forms(formCount);
  InterpreterHarness harness;
  FormObservation observation;
  const PdfStatus status = harness.interpreter.begin(&source, 1, forms.resources[0], harness.model);
  observation.result = status.ok() ? runInterpreter(harness.interpreter) : PdfStepResult::failure(status);
  observation.maximumDepth = harness.interpreter.maximumFormDepth();
  observation.warnings = harness.model.warnings();
  return observation;
}

struct DecodeObservation {
  PdfStepResult result = PdfStepResult::failure(PdfStatus::failure(PdfError::BudgetExhausted));
  size_t outputBytes = 0;
};

DecodeObservation decodeThreeHundredBytes(const uint64_t maximumExpandedBytes) {
  static constexpr std::array<uint8_t, 13> encoded{
      0x78, 0xda, 0x73, 0x74, 0x1c, 0x05, 0xc4, 0x02, 0x00, 0xcb, 0x9e, 0x4c, 0x2d,
  };
  PdfTestByteSource input({encoded.begin(), encoded.end()});
  PdfTestByteSink output;
  std::array<uint8_t, PdfLimits::SourceBufferBytes> sourceBuffer{};
  std::array<uint8_t, PdfLimits::DecoderOutputBytes> outputBuffer{};
  auto dictionary = std::make_unique<uint8_t[]>(PdfLimits::UzlibDictionaryBytes);
  PdfStreamDecoder decoder({sourceBuffer.data(), sourceBuffer.size(), outputBuffer.data(), outputBuffer.size(),
                            dictionary.get(), PdfLimits::UzlibDictionaryBytes});
  const std::array<PdfStreamFilter, 1> filters{PdfStreamFilter::Flate};
  const PdfStreamDecodeLimits limits{maximumExpandedBytes, std::numeric_limits<uint16_t>::max()};
  DecodeObservation observation;
  const PdfStatus beginStatus = decoder.begin(input.source(), output.sink(), filters.data(), filters.size(), limits);
  if (!beginStatus.ok()) {
    observation.result = PdfStepResult::failure(beginStatus);
    return observation;
  }
  for (uint16_t step = 0; step < 512; ++step) {
    PdfWorkBudget budget{32, 4096};
    observation.result = decoder.step(budget);
    if (!observation.result.yielded()) {
      break;
    }
  }
  observation.outputBytes = output.bytes().size();
  return observation;
}

struct PreparationHarness {
  PdfTestCacheIo storage;
  PdfResourceSnapshot resources{128U * 1024U, 96U * 1024U, 8U * 1024U};
  uint32_t nowMs = 0;

  static uint32_t now(void* const context) { return static_cast<PreparationHarness*>(context)->nowMs; }

  static PdfResourceSnapshot measure(void* const context) {
    return static_cast<PreparationHarness*>(context)->resources;
  }

  PdfPreparationConfig config() { return config(storage.io()); }

  PdfPreparationConfig config(const PdfCacheIo io) {
    return {
        io,  "/books/adversarial.pdf", "/.crosspoint",           this,
        now, {this, measure, nullptr}, storage.renameCallback(), 800,
        480,
    };
  }
};

PdfStepResult runPreparation(PdfPreparation& preparation, PreparationHarness& harness) {
  for (uint32_t step = 0; step < 20000; ++step) {
    const PdfStepResult result = preparation.step();
    ++harness.nowMs;
    if (!result.yielded()) {
      return result;
    }
  }
  return PdfStepResult::failure(PdfStatus::failure(PdfError::BudgetExhausted));
}

}  // namespace

void* operator new(const std::size_t size) {
  recordAllocation(size);
  if (void* const memory = std::malloc(size == 0 ? 1 : size)) {
    return memory;
  }
  throw std::bad_alloc();
}

void* operator new[](const std::size_t size) {
  recordAllocation(size);
  if (void* const memory = std::malloc(size == 0 ? 1 : size)) {
    return memory;
  }
  throw std::bad_alloc();
}

void* operator new(const std::size_t size, const std::nothrow_t&) noexcept {
  recordAllocation(size);
  return std::malloc(size == 0 ? 1 : size);
}

void* operator new[](const std::size_t size, const std::nothrow_t&) noexcept {
  recordAllocation(size);
  if (gAllocationWatchActive) {
    ++gNothrowArrayCallCount;
    if (gFailNothrowArrayCall != 0 && gNothrowArrayCallCount == gFailNothrowArrayCall) {
      return nullptr;
    }
  }
  return std::malloc(size == 0 ? 1 : size);
}

void operator delete(void* const memory) noexcept { std::free(memory); }
void operator delete[](void* const memory) noexcept { std::free(memory); }
void operator delete(void* const memory, std::size_t) noexcept { std::free(memory); }
void operator delete[](void* const memory, std::size_t) noexcept { std::free(memory); }
void operator delete(void* const memory, const std::nothrow_t&) noexcept { std::free(memory); }
void operator delete[](void* const memory, const std::nothrow_t&) noexcept { std::free(memory); }

TEST(PdfAdversarialTest, FixedSeedByteMutationsHaveStableClassesAndAllocateNoSourceSizedBuffer) {
  const std::vector<uint8_t> fixture = loadClassicFixture();
  ASSERT_FALSE(fixture.empty());

  AllocationObservation positiveControl;
  {
    AllocationWatch watch(fixture.size());
    void* const allocation = ::operator new(fixture.size());
    ::operator delete(allocation);
    positiveControl = watch.observation();
  }
  ASSERT_EQ(positiveControl.allocationCount, 1U);
  ASSERT_EQ(positiveControl.sourceSizedCount, 1U);

  const ParseObservation baseline = parseXrefWithoutHeapAllocations(fixture);
  ASSERT_TRUE(baseline.result.complete()) << static_cast<int>(baseline.result.status.error);
  ASSERT_EQ(baseline.allocations.allocationCount, 0U);
  ASSERT_EQ(baseline.allocations.sourceSizedCount, 0U);

  constexpr uint32_t kSeed = 0x4c61c2a7U;
  constexpr uint16_t kMutationCount = 128;
  uint32_t random = kSeed;
  uint16_t completed = 0;
  uint16_t failed = 0;
  for (uint16_t mutation = 0; mutation < kMutationCount; ++mutation) {
    std::vector<uint8_t> bytes = fixture;
    const size_t offset = nextRandom(&random) % bytes.size();
    const uint8_t mask = static_cast<uint8_t>(1U << (nextRandom(&random) & 7U));
    bytes[offset] ^= mask;

    const ParseObservation first = parseXrefWithoutHeapAllocations(bytes);
    const ParseObservation replay = parseXrefWithoutHeapAllocations(bytes);
    SCOPED_TRACE(::testing::Message() << "seed=" << kSeed << " mutation=" << mutation << " offset=" << offset
                                      << " mask=" << static_cast<uint16_t>(mask));
    EXPECT_EQ(first.result.state, replay.result.state);
    EXPECT_EQ(first.result.status.error, replay.result.status.error);
    EXPECT_TRUE(isInputDerivedXrefResult(first.result.status.error));
    EXPECT_EQ(first.allocations.allocationCount, 0U);
    EXPECT_EQ(first.allocations.sourceSizedCount, 0U);
    EXPECT_EQ(replay.allocations.allocationCount, 0U);
    EXPECT_EQ(replay.allocations.sourceSizedCount, 0U);
    completed += first.result.complete() ? 1U : 0U;
    failed += first.result.failed() ? 1U : 0U;
  }
  EXPECT_GT(completed, 0U);
  EXPECT_GT(failed, 0U);
}

TEST(PdfAdversarialTest, ConfiguredObjectRecordCapacityHasExactAndShortByOneWitness) {
  const std::array<PdfXrefEntry, 2> entries{
      PdfXrefEntry{1, 0, PdfXrefEntryType::Uncompressed, 0, 20, 0},
      PdfXrefEntry{2, 0, PdfXrefEntryType::Uncompressed, 0, 40, 0},
  };

  PdfTestRecordStore exactRecords(sizeof(PdfXrefEntry), entries.size());
  PdfXrefTable exact(exactRecords.store());
  EXPECT_TRUE(exact.appendNewest(entries[0]).ok());
  EXPECT_TRUE(exact.appendNewest(entries[1]).ok());
  EXPECT_EQ(exact.entryCount(), entries.size());

  PdfTestRecordStore shortRecords(sizeof(PdfXrefEntry), entries.size() - 1U);
  PdfXrefTable shortByOne(shortRecords.store());
  EXPECT_TRUE(shortByOne.appendNewest(entries[0]).ok());
  EXPECT_EQ(shortByOne.appendNewest(entries[1]).error, PdfError::LimitExceeded);
}

TEST(PdfAdversarialTest, ObjectContainerDepthAcceptsNAndRejectsNPlusOne) {
  const PdfStepResult exact = parseNestedArrays(PdfLimits::MaxContainerNesting);
  ASSERT_TRUE(exact.complete()) << static_cast<int>(exact.status.error);

  const PdfStepResult over = parseNestedArrays(PdfLimits::MaxContainerNesting + 1U);
  ASSERT_TRUE(over.failed());
  EXPECT_EQ(over.status.error, PdfError::LimitExceeded);
}

TEST(PdfAdversarialTest, DocumentOperatorLimitEndsOnlyTheCurrentPage) {
  const OperatorObservation exact = interpretOneDocumentOperator(PdfLimits::MaxOperatorsPerDocument - 1U);
  ASSERT_TRUE(exact.result.complete()) << static_cast<int>(exact.result.status.error);
  EXPECT_EQ(exact.documentOperatorCount, PdfLimits::MaxOperatorsPerDocument);

  const OperatorObservation shortByOne = interpretOneDocumentOperator(PdfLimits::MaxOperatorsPerDocument);
  ASSERT_TRUE(shortByOne.result.complete()) << static_cast<int>(shortByOne.result.status.error);
  EXPECT_EQ(shortByOne.documentOperatorCount, 0U);
  EXPECT_NE(static_cast<uint16_t>(shortByOne.warnings) & static_cast<uint16_t>(PdfPageWarning::VectorArtOmitted), 0U);
}

TEST(PdfAdversarialTest, FormDepthAcceptsProductionMaximumAndOmitsOneMore) {
  const FormObservation exact = interpretFormChain(PdfLimits::MaxFormDepth);
  ASSERT_TRUE(exact.result.complete()) << static_cast<int>(exact.result.status.error);
  EXPECT_EQ(exact.maximumDepth, PdfLimits::MaxFormDepth);

  const FormObservation over = interpretFormChain(PdfLimits::MaxFormDepth + 1U);
  ASSERT_TRUE(over.result.complete()) << static_cast<int>(over.result.status.error);
  EXPECT_EQ(over.maximumDepth, PdfLimits::MaxFormDepth);
  EXPECT_NE(static_cast<uint16_t>(over.warnings) & static_cast<uint16_t>(PdfPageWarning::VectorArtOmitted), 0U);
}

TEST(PdfAdversarialTest, ExpandedByteLimitAcceptsNAndRejectsNAtNMinusOneCapacity) {
  constexpr uint64_t kExpandedBytes = 300;
  const DecodeObservation exact = decodeThreeHundredBytes(kExpandedBytes);
  ASSERT_TRUE(exact.result.complete()) << static_cast<int>(exact.result.status.error);
  EXPECT_EQ(exact.outputBytes, kExpandedBytes);

  const DecodeObservation shortByOne = decodeThreeHundredBytes(kExpandedBytes - 1U);
  ASSERT_TRUE(shortByOne.result.failed());
  EXPECT_EQ(shortByOne.result.status.error, PdfError::ExpansionLimit);
}

TEST(PdfAdversarialTest, CacheByteLimitAcceptsNAndRejectsNAtNMinusOneCapacity) {
  constexpr uint64_t kRequiredBytes = 17;
  PdfCacheBudget exact{};
  exact.hardLimit = kRequiredBytes;
  exact.limit = kRequiredBytes;
  EXPECT_TRUE(pdfReserveCacheBytes(&exact, kRequiredBytes, PdfCacheFileKind::Required).ok());
  EXPECT_EQ(exact.requiredBytes, kRequiredBytes);

  PdfCacheBudget shortByOne{};
  shortByOne.hardLimit = kRequiredBytes - 1U;
  shortByOne.limit = kRequiredBytes - 1U;
  EXPECT_EQ(pdfReserveCacheBytes(&shortByOne, kRequiredBytes, PdfCacheFileKind::Required).error,
            PdfError::InsufficientStorage);
  EXPECT_EQ(shortByOne.requiredBytes, 0U);
}

TEST(PdfAdversarialTest, EveryFixedWorkspaceAllocationFailureUnwindsBeforeOpeningSource) {
  const std::vector<uint8_t> fixture = loadClassicFixture();
  ASSERT_FALSE(fixture.empty());

  for (size_t failureOrdinal = 1; failureOrdinal <= PDF_RESOURCE_SLOT_COUNT; ++failureOrdinal) {
    SCOPED_TRACE(failureOrdinal);
    PreparationHarness harness;
    harness.storage.addFile("/books/adversarial.pdf", fixture, 1234, true);
    PdfPreparation preparation;
    ASSERT_TRUE(preparation.begin(harness.config()).ok());
    PdfStepResult result;
    AllocationObservation allocations;
    {
      AllocationWatch watch(fixture.size(), failureOrdinal);
      result = runPreparation(preparation, harness);
      allocations = watch.observation();
    }

    ASSERT_TRUE(result.failed());
    EXPECT_EQ(result.status.error, PdfError::InsufficientMemory);
    EXPECT_EQ(preparation.phase(), PdfPreparationPhase::Failed);
    EXPECT_EQ(allocations.nothrowArrayCallCount, failureOrdinal);
    EXPECT_EQ(allocations.sourceSizedCount, 0U);
    EXPECT_EQ(preparation.resourceCurrentBytes(), 0U);
    EXPECT_EQ(harness.storage.openCalls(), 0U);
    EXPECT_EQ(harness.storage.openHandleCount(), 0U);
  }
}

TEST(PdfAdversarialTest, LowCacheCapacityFailsClosedWithoutACommittedManifest) {
  const std::vector<uint8_t> fixture = loadClassicFixture();
  ASSERT_FALSE(fixture.empty());
  PreparationHarness harness;
  harness.storage.addFile("/books/adversarial.pdf", fixture, 1234, true);
  harness.storage.setCapacity(64ULL * 1024ULL * 1024ULL, PDF_CACHE_MIN_FREE_RESERVE_BYTES, true);
  PdfPreparation preparation;
  ASSERT_TRUE(preparation.begin(harness.config()).ok());

  const PdfStepResult result = runPreparation(preparation, harness);

  ASSERT_TRUE(result.failed());
  EXPECT_EQ(result.status.error, PdfError::InsufficientStorage);
  EXPECT_EQ(preparation.phase(), PdfPreparationPhase::Failed);
  EXPECT_EQ(preparation.resourceCurrentBytes(), 0U);
  EXPECT_EQ(harness.storage.openHandleCount(), 0U);
  for (const std::string& path : harness.storage.paths()) {
    EXPECT_FALSE(path.ends_with("/manifest.a"));
    EXPECT_FALSE(path.ends_with("/manifest.b"));
  }
}

TEST(PdfAdversarialTest, WriteEnospcPropagatesAndLeavesNoCommittedManifest) {
  const std::vector<uint8_t> fixture = loadClassicFixture();
  ASSERT_FALSE(fixture.empty());
  PreparationHarness harness;
  harness.storage.addFile("/books/adversarial.pdf", fixture, 1234, true);
  EnospcWriteFault fault(harness.storage.io(), 1);
  PdfPreparation preparation;
  ASSERT_TRUE(preparation.begin(harness.config(fault.io())).ok());

  const PdfStepResult result = runPreparation(preparation, harness);

  ASSERT_TRUE(result.failed());
  EXPECT_EQ(result.status.error, PdfError::InsufficientStorage);
  EXPECT_EQ(preparation.phase(), PdfPreparationPhase::Failed);
  EXPECT_GE(fault.writeCount(), 1U);
  EXPECT_EQ(fault.failureCount(), 1U);
  EXPECT_EQ(preparation.resourceCurrentBytes(), 0U);
  EXPECT_EQ(harness.storage.openHandleCount(), 0U);
  for (const std::string& path : harness.storage.paths()) {
    EXPECT_FALSE(path.ends_with("/manifest.a"));
    EXPECT_FALSE(path.ends_with("/manifest.b"));
  }
}
