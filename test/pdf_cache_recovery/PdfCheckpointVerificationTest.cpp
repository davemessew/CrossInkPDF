#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "PdfBuildCheckpoint.h"
#include "PdfCacheStore.h"
#include "PdfTestCacheIo.h"

namespace {

constexpr char kRoot[] = "/.crosspoint/pdf_checkpoint_verification";
constexpr char kCheckpointPath[] = "/.crosspoint/pdf_checkpoint_verification/build.a";

struct VectorSink {
  std::vector<uint8_t> bytes;

  static PdfStatus write(void* context, const uint8_t* source, const size_t requested, size_t* bytesWritten) {
    auto& self = *static_cast<VectorSink*>(context);
    if (source == nullptr || bytesWritten == nullptr) {
      return PdfStatus::failure(PdfError::InvalidArgument);
    }
    self.bytes.insert(self.bytes.end(), source, source + requested);
    *bytesWritten = requested;
    return PdfStatus::success();
  }

  PdfByteSink sink() { return {this, write}; }
};

struct VectorSource {
  const std::vector<uint8_t>* bytes = nullptr;

  static PdfStatus read(void* context, const uint64_t offset, uint8_t* destination, const size_t requested,
                        size_t* bytesRead) {
    auto& self = *static_cast<VectorSource*>(context);
    if (self.bytes == nullptr || destination == nullptr || bytesRead == nullptr || offset > self.bytes->size()) {
      return PdfStatus::failure(PdfError::InvalidOffset, offset);
    }
    const size_t available = self.bytes->size() - static_cast<size_t>(offset);
    *bytesRead = std::min(requested, available);
    if (*bytesRead != 0) {
      std::memcpy(destination, self.bytes->data() + static_cast<size_t>(offset), *bytesRead);
    }
    return PdfStatus::success();
  }

  PdfByteSource source() { return {this, static_cast<uint64_t>(bytes == nullptr ? 0 : bytes->size()), read}; }
};

PdfBuildCheckpoint checkpoint() {
  PdfBuildCheckpoint value{};
  value.sequence = 1;
  value.source.size = 0x0102030405060708ULL;
  value.source.modificationTime = {true, 0x1112131415161718ULL};
  value.source.headFingerprint = 0x2122232425262728ULL;
  value.source.tailFingerprint = 0x3132333435363738ULL;
  value.generation = 9;
  value.phase = PdfBuildPhase::Cancelled;
  value.resumePhase = PdfBuildResumePhase::AfterPage;
  value.lastVerifiedPage = 11;
  value.lastVerifiedObject = 12;
  value.emittedSections = 13;
  value.emittedImages = 14;
  value.cumulativeWords = 15;
  value.outputBytes = 16;
  value.warningFlags = 17;
  value.journalBytes = 1536;
  return value;
}

struct CheckpointFieldMutation {
  const char* name;
  size_t encodedOffset;
  uint8_t mask;
};

// Codec 3 stores every logical PdfBuildCheckpoint field below. Each case
// changes exactly one decoded field while leaving the record CRC-valid.
constexpr std::array<CheckpointFieldMutation, 17> kCheckpointFieldMutations{{
    {"Sequence", 8, 0x01},
    {"SourceSize", 12, 0x01},
    {"SourceModificationTimeKnown", 20, 0x01},
    {"SourceModificationTimeValue", 24, 0x01},
    {"SourceHeadFingerprint", 32, 0x01},
    {"SourceTailFingerprint", 40, 0x01},
    {"Generation", 48, 0x01},
    {"Phase", 52, 0x0a},
    {"ResumePhase", 53, 0x07},
    {"LastVerifiedPage", 56, 0x01},
    {"LastVerifiedObject", 60, 0x01},
    {"EmittedSections", 64, 0x01},
    {"EmittedImages", 68, 0x01},
    {"CumulativeWords", 72, 0x01},
    {"OutputBytes", 76, 0x01},
    {"WarningFlags", 84, 0x01},
    {"JournalBytes", 21, 0x01},
}};

class CheckpointPostWriteVerificationTest : public ::testing::TestWithParam<CheckpointFieldMutation> {};

TEST_P(CheckpointPostWriteVerificationTest, RejectsCrcValidMutationOfEveryCodec3Field) {
  const CheckpointFieldMutation mutation = GetParam();
  SCOPED_TRACE(mutation.name);

  const PdfBuildCheckpoint expected = checkpoint();
  VectorSink encoded;
  ASSERT_TRUE(pdfEncodeBuildCheckpoint(expected, encoded.sink()));
  ASSERT_EQ(encoded.bytes.size(), 96u);
  ASSERT_LT(mutation.encodedOffset, encoded.bytes.size() - sizeof(uint32_t));

  PdfTestCacheIo storage;
  PdfCacheStore store;
  ASSERT_TRUE(store.initialize(storage.io(), kRoot));
  storage.mutateByteBeforeNextRead(kCheckpointPath, mutation.encodedOffset, mutation.mask);

  const PdfStatus status = store.commitCheckpoint(expected);

  EXPECT_FALSE(status.ok());
  EXPECT_EQ(status.error, PdfError::Malformed);
  ASSERT_EQ(storage.readMutationsApplied(), 1u);
  const std::vector<uint8_t>& mutated = storage.bytes(kCheckpointPath);
  ASSERT_EQ(mutated.size(), encoded.bytes.size());
  EXPECT_EQ(mutated[mutation.encodedOffset],
            static_cast<uint8_t>(encoded.bytes[mutation.encodedOffset] ^ mutation.mask));

  VectorSource source{&mutated};
  PdfBuildCheckpoint decoded{};
  EXPECT_TRUE(pdfDecodeBuildCheckpoint(source.source(), &decoded).ok());
}

INSTANTIATE_TEST_SUITE_P(
    Codec3Fields, CheckpointPostWriteVerificationTest, ::testing::ValuesIn(kCheckpointFieldMutations),
    [](const ::testing::TestParamInfo<CheckpointFieldMutation>& info) { return info.param.name; });

TEST(PdfCheckpointDurability, UsesOneStatusBearingSyncWithoutRedundantFlushBeforeReadback) {
  PdfTestCacheIo storage;
  PdfCacheStore store;
  ASSERT_TRUE(store.initialize(storage.io(), kRoot));

  ASSERT_TRUE(store.commitCheckpoint(checkpoint()));

  EXPECT_EQ(storage.flushCalls(), 0u);
  EXPECT_EQ(storage.syncCalls(), 1u);
  EXPECT_EQ(storage.closeCalls(), 2u);
  ASSERT_EQ(storage.syncObservations().size(), 1u);
  EXPECT_EQ(storage.syncObservations().front(), kCheckpointPath);
  EXPECT_EQ(storage.openHandleCount(), 0u);
}

}  // namespace
