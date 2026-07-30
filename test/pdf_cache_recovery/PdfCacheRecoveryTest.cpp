#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "PdfBuildCheckpoint.h"
#include "PdfCacheManifest.h"
#include "PdfCacheStore.h"
#include "PdfSourceIdentity.h"
#include "PdfTestCacheIo.h"

namespace {

constexpr char kRoot[] = "/.crosspoint/pdf_42";

PdfSourceIdentity identity(const uint64_t size = 1234, const uint64_t head = 0x1122334455667788ULL,
                           const uint64_t tail = 0x8877665544332211ULL) {
  PdfSourceIdentity result{};
  result.size = size;
  result.modificationTime = {true, 0x4a210001};
  result.headFingerprint = head;
  result.tailFingerprint = tail;
  return result;
}

PdfCacheManifest manifest(const uint32_t sequence, const uint32_t generation) {
  PdfCacheManifest result{};
  result.formatVersion = PDF_CACHE_FORMAT_VERSION;
  result.capabilityVersion = PDF_CACHE_CAPABILITY_VERSION;
  result.sequence = sequence;
  result.completed = true;
  result.warningFlags = 3;
  result.source = identity();
  result.generation = generation;
  result.totalWords = 77;
  return result;
}

struct RecordTable {
  std::vector<PdfRequiredFileRecord> records;

  static PdfStatus read(void* context, const uint32_t index, PdfRequiredFileRecord* record) {
    auto& self = *static_cast<RecordTable*>(context);
    if (record == nullptr || index >= self.records.size()) {
      return PdfStatus::failure(PdfError::InvalidArgument);
    }
    *record = self.records[index];
    return PdfStatus::success();
  }

  PdfRequiredFileTableSource source() { return {this, static_cast<uint32_t>(records.size()), read}; }
};

PdfRequiredFileRecord record(const char* path, const uint64_t size, const uint32_t crc) {
  PdfRequiredFileRecord result{};
  const size_t length = std::strlen(path);
  EXPECT_LT(length, sizeof(result.path));
  result.pathLength = static_cast<uint8_t>(length);
  std::memcpy(result.path, path, length);
  result.path[length] = '\0';
  result.size = size;
  result.crc32 = crc;
  return result;
}

void sealManifest(PdfCacheManifest* value, const RecordTable& table) {
  ASSERT_NE(value, nullptr);
  value->requiredFileCount = static_cast<uint32_t>(table.records.size());
  value->requiredFileBytes = 0;
  value->requiredFileLedger = PDF_CACHE_FNV64_OFFSET;
  for (const auto& item : table.records) {
    value->requiredFileBytes += item.size;
    value->requiredFileLedger = pdfUpdateRequiredFileLedger(value->requiredFileLedger, item);
  }
}

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
    const size_t count = std::min<size_t>(requested, self.bytes->size() - static_cast<size_t>(offset));
    if (count != 0) {
      std::memcpy(destination, self.bytes->data() + static_cast<size_t>(offset), count);
    }
    *bytesRead = count;
    return PdfStatus::success();
  }

  PdfByteSource source() { return {this, static_cast<uint64_t>(bytes == nullptr ? 0 : bytes->size()), read}; }
};

struct RecordCollector {
  std::vector<PdfRequiredFileRecord> records;

  static PdfStatus accept(void* context, const PdfRequiredFileRecord& value) {
    static_cast<RecordCollector*>(context)->records.push_back(value);
    return PdfStatus::success();
  }

  PdfRequiredFileTableVisitor visitor() { return {this, accept}; }
};

VectorSink encodeManifest(const PdfCacheManifest& value, RecordTable& table) {
  VectorSink output;
  EXPECT_TRUE(pdfEncodeCacheManifest(value, table.source(), output.sink()));
  return output;
}

PdfCacheCommitEvidence evidence(const PdfCacheManifest& value) {
  return {true, value.requiredFileCount, value.requiredFileBytes, value.requiredFileLedger};
}

void writeLe32(std::vector<uint8_t>* bytes, const size_t offset, const uint32_t value) {
  ASSERT_NE(bytes, nullptr);
  ASSERT_LE(offset + 4, bytes->size());
  (*bytes)[offset] = static_cast<uint8_t>(value);
  (*bytes)[offset + 1] = static_cast<uint8_t>(value >> 8U);
  (*bytes)[offset + 2] = static_cast<uint8_t>(value >> 16U);
  (*bytes)[offset + 3] = static_cast<uint8_t>(value >> 24U);
}

void rewriteTrailingCrc(std::vector<uint8_t>* bytes) {
  ASSERT_NE(bytes, nullptr);
  ASSERT_GE(bytes->size(), 4u);
  writeLe32(bytes, bytes->size() - 4, pdfCacheCrc32(bytes->data(), bytes->size() - 4));
}

void commit(PdfTestCacheIo& storage, const PdfCacheManifest& value, RecordTable& table) {
  PdfCacheStore store;
  ASSERT_TRUE(store.initialize(storage.io(), kRoot));
  PdfCacheManifestSelection prior{};
  ASSERT_TRUE(store.loadManifestSlots(value.source, &prior));
  PdfCacheManifestSelection committed{};
  ASSERT_TRUE(store.commitManifest(value, table.source(), evidence(value), prior, &committed));
  ASSERT_TRUE(committed.selected);
  ASSERT_EQ(committed.manifest.sequence, value.sequence);
}

}  // namespace

TEST(PdfCacheCodec, ManifestRoundTripsExplicitFieldsAndRecords) {
  RecordTable table{{record("metadata.bin", 12, 0x01020304), record("sections/000000.xhtml", 345, 0xaabbccdd)}};
  PdfCacheManifest expected = manifest(9, 17);
  sealManifest(&expected, table);

  VectorSink encoded = encodeManifest(expected, table);
  ASSERT_GT(encoded.bytes.size(), sizeof(expected));

  VectorSource input{&encoded.bytes};
  PdfCacheManifest actual{};
  RecordCollector collector;
  ASSERT_TRUE(pdfDecodeCacheManifest(input.source(), &actual, collector.visitor()));

  EXPECT_TRUE(pdfSourceIdentityEqual(expected.source, actual.source));
  EXPECT_EQ(actual.formatVersion, expected.formatVersion);
  EXPECT_EQ(actual.capabilityVersion, expected.capabilityVersion);
  EXPECT_EQ(actual.sequence, 9u);
  EXPECT_TRUE(actual.completed);
  EXPECT_EQ(actual.warningFlags, 3u);
  EXPECT_EQ(actual.generation, 17u);
  EXPECT_EQ(actual.totalWords, 77u);
  EXPECT_EQ(actual.requiredFileCount, 2u);
  EXPECT_EQ(actual.requiredFileBytes, 357u);
  EXPECT_EQ(actual.requiredFileLedger, expected.requiredFileLedger);
  ASSERT_EQ(collector.records.size(), 2u);
  EXPECT_STREQ(collector.records[1].path, "sections/000000.xhtml");
  EXPECT_EQ(collector.records[1].size, 345u);
  EXPECT_EQ(collector.records[1].crc32, 0xaabbccddu);
}

TEST(PdfCacheCodec, RejectsMagicVersionLengthCrcAndEveryTruncatedPrefix) {
  RecordTable table{{record("metadata.bin", 12, 0x01020304)}};
  PdfCacheManifest value = manifest(9, 17);
  sealManifest(&value, table);
  const VectorSink valid = encodeManifest(value, table);

  for (size_t length = 0; length < valid.bytes.size(); ++length) {
    std::vector<uint8_t> torn(valid.bytes.begin(), valid.bytes.begin() + length);
    VectorSource input{&torn};
    PdfCacheManifest decoded{};
    EXPECT_FALSE(pdfDecodeCacheManifest(input.source(), &decoded, {}).ok()) << "prefix=" << length;
  }

  for (const size_t offset : {size_t{0}, size_t{4}, valid.bytes.size() - 8, valid.bytes.size() - 1}) {
    std::vector<uint8_t> corrupted = valid.bytes;
    corrupted[offset] ^= 0x5a;
    VectorSource input{&corrupted};
    PdfCacheManifest decoded{};
    EXPECT_FALSE(pdfDecodeCacheManifest(input.source(), &decoded, {}).ok()) << "offset=" << offset;
  }

  std::vector<uint8_t> extended = valid.bytes;
  extended.push_back(0);
  VectorSource extra{&extended};
  PdfCacheManifest decoded{};
  EXPECT_FALSE(pdfDecodeCacheManifest(extra.source(), &decoded, {}).ok());

  for (const size_t offset : {size_t{0}, size_t{4}, size_t{6}, size_t{8}}) {
    std::vector<uint8_t> invalidField = valid.bytes;
    invalidField[offset] ^= 1;
    rewriteTrailingCrc(&invalidField);
    VectorSource invalidInput{&invalidField};
    EXPECT_FALSE(pdfDecodeCacheManifest(invalidInput.source(), &decoded, {}).ok()) << "field=" << offset;
  }
  std::vector<uint8_t> invalidLength = valid.bytes;
  writeLe32(&invalidLength, invalidLength.size() - 8, static_cast<uint32_t>(invalidLength.size() - 1));
  rewriteTrailingCrc(&invalidLength);
  VectorSource invalidLengthInput{&invalidLength};
  EXPECT_FALSE(pdfDecodeCacheManifest(invalidLengthInput.source(), &decoded, {}).ok());
  EXPECT_EQ(valid.bytes[12], 9u);
  EXPECT_EQ(valid.bytes[13], 0u);
  EXPECT_EQ(valid.bytes[14], 0u);
  EXPECT_EQ(valid.bytes[15], 0u);
}

TEST(PdfCacheCodec, CheckpointRoundTripsAndRejectsEveryTruncatedPrefix) {
  PdfBuildCheckpoint expected{};
  expected.sequence = UINT32_MAX;
  expected.source = identity();
  expected.generation = 21;
  expected.phase = PdfBuildPhase::EmitSections;
  expected.lastVerifiedPage = 98;
  expected.lastVerifiedObject = 301;
  expected.emittedSections = 7;
  expected.emittedImages = 2;
  expected.cumulativeWords = 12345;
  expected.outputBytes = 987654;
  expected.warningFlags = 5;

  VectorSink encoded;
  ASSERT_TRUE(pdfEncodeBuildCheckpoint(expected, encoded.sink()));
  ASSERT_EQ(encoded.bytes.size(), 96u);

  VectorSource input{&encoded.bytes};
  PdfBuildCheckpoint actual{};
  ASSERT_TRUE(pdfDecodeBuildCheckpoint(input.source(), &actual));
  EXPECT_EQ(actual.sequence, UINT32_MAX);
  EXPECT_TRUE(pdfSourceIdentityEqual(actual.source, expected.source));
  EXPECT_EQ(actual.generation, 21u);
  EXPECT_EQ(actual.phase, PdfBuildPhase::EmitSections);
  EXPECT_EQ(actual.lastVerifiedPage, 98u);
  EXPECT_EQ(actual.lastVerifiedObject, 301u);
  EXPECT_EQ(actual.emittedSections, 7u);
  EXPECT_EQ(actual.emittedImages, 2u);
  EXPECT_EQ(actual.cumulativeWords, 12345u);
  EXPECT_EQ(actual.outputBytes, 987654u);
  EXPECT_EQ(actual.warningFlags, 5u);
  EXPECT_EQ(encoded.bytes[8], 0xff);
  EXPECT_EQ(encoded.bytes[9], 0xff);
  EXPECT_EQ(encoded.bytes[10], 0xff);
  EXPECT_EQ(encoded.bytes[11], 0xff);

  for (const size_t offset : {size_t{0}, size_t{4}}) {
    std::vector<uint8_t> invalidField = encoded.bytes;
    invalidField[offset] ^= 1;
    rewriteTrailingCrc(&invalidField);
    VectorSource invalidInput{&invalidField};
    PdfBuildCheckpoint ignored{};
    EXPECT_FALSE(pdfDecodeBuildCheckpoint(invalidInput.source(), &ignored).ok()) << "field=" << offset;
  }
  std::vector<uint8_t> invalidLength = encoded.bytes;
  writeLe32(&invalidLength, invalidLength.size() - 8, 95);
  rewriteTrailingCrc(&invalidLength);
  VectorSource invalidLengthInput{&invalidLength};
  PdfBuildCheckpoint ignored{};
  EXPECT_FALSE(pdfDecodeBuildCheckpoint(invalidLengthInput.source(), &ignored).ok());

  for (size_t length = 0; length < encoded.bytes.size(); ++length) {
    std::vector<uint8_t> torn(encoded.bytes.begin(), encoded.bytes.begin() + length);
    VectorSource tornInput{&torn};
    PdfBuildCheckpoint ignored{};
    EXPECT_FALSE(pdfDecodeBuildCheckpoint(tornInput.source(), &ignored).ok()) << "prefix=" << length;
  }
}

TEST(PdfSourceIdentity, StablePathHashMatchesEpubFnv64) {
  EXPECT_EQ(pdfPathHash64("/Books/Test.pdf", 15), 8989207754416008753ULL);
  char root[PDF_CACHE_PATH_CAPACITY]{};
  ASSERT_TRUE(pdfFormatCacheRoot("/.crosspoint", "/Books/Test.pdf", root, sizeof(root)));
  EXPECT_STREQ(root, "/.crosspoint/pdf_8989207754416008753");
}

TEST(PdfSourceIdentity, UsesOneHandleAndAtMostTwoReadsAtBoundaries) {
  struct FingerprintCase {
    size_t size;
    uint64_t head;
    uint64_t tail;
  };
  constexpr FingerprintCase cases[] = {
      {0, 15133559704570191681ULL, 13279783359471968049ULL},    {1, 7769460238139891198ULL, 1936602289605935822ULL},
      {4096, 10945948658690093889ULL, 167458934019418929ULL},   {4097, 6357150361060389088ULL, 1143987284501321137ULL},
      {8191, 6792228438408778583ULL, 7828222814041122749ULL},   {8192, 2056956650374677745ULL, 4627910732330320337ULL},
      {20000, 14243664372940492479ULL, 7599169692907089233ULL},
  };
  for (const auto& testCase : cases) {
    const size_t size = testCase.size;
    PdfTestCacheIo storage;
    std::vector<uint8_t> bytes(size);
    for (size_t index = 0; index < bytes.size(); ++index) {
      bytes[index] = static_cast<uint8_t>((index * 37U + 11U) & 0xffU);
    }
    storage.addFile("/book.pdf", bytes, 123, true);
    std::array<uint8_t, PDF_SOURCE_FINGERPRINT_BYTES> workspace{};
    PdfSourceIdentity result{};
    ASSERT_TRUE(pdfComputeSourceIdentity(storage.io(), "/book.pdf", workspace.data(), workspace.size(), &result))
        << "size=" << size;
    EXPECT_EQ(result.size, size);
    EXPECT_TRUE(result.modificationTime.known);
    EXPECT_EQ(result.modificationTime.value, 123u);
    EXPECT_EQ(result.headFingerprint, testCase.head);
    EXPECT_EQ(result.tailFingerprint, testCase.tail);
    EXPECT_EQ(storage.openCalls(), 1u);
    EXPECT_EQ(storage.closeCalls(), 1u);
    EXPECT_EQ(storage.openHandleCount(), 0u);
    EXPECT_EQ(storage.readCalls(), size == 0 ? 0u : (size <= 4096 ? 1u : 2u));
  }
}

TEST(PdfSourceIdentity, DomainsHeadTailOffsetsLengthsAndSize) {
  PdfTestCacheIo storage;
  std::vector<uint8_t> original(12000, 0x41);
  storage.addFile("/book.pdf", original);
  std::array<uint8_t, PDF_SOURCE_FINGERPRINT_BYTES> workspace{};
  PdfSourceIdentity baseline{};
  ASSERT_TRUE(pdfComputeSourceIdentity(storage.io(), "/book.pdf", workspace.data(), workspace.size(), &baseline));

  std::vector<uint8_t> middle = original;
  middle[6000] ^= 1;
  storage.addFile("/middle.pdf", middle);
  PdfSourceIdentity middleIdentity{};
  ASSERT_TRUE(
      pdfComputeSourceIdentity(storage.io(), "/middle.pdf", workspace.data(), workspace.size(), &middleIdentity));
  EXPECT_EQ(middleIdentity.headFingerprint, baseline.headFingerprint);
  EXPECT_EQ(middleIdentity.tailFingerprint, baseline.tailFingerprint);

  std::vector<uint8_t> head = original;
  head[0] ^= 1;
  storage.addFile("/head.pdf", head);
  PdfSourceIdentity headIdentity{};
  ASSERT_TRUE(pdfComputeSourceIdentity(storage.io(), "/head.pdf", workspace.data(), workspace.size(), &headIdentity));
  EXPECT_NE(headIdentity.headFingerprint, baseline.headFingerprint);
  EXPECT_EQ(headIdentity.tailFingerprint, baseline.tailFingerprint);

  std::vector<uint8_t> tail = original;
  tail.back() ^= 1;
  storage.addFile("/tail.pdf", tail);
  PdfSourceIdentity tailIdentity{};
  ASSERT_TRUE(pdfComputeSourceIdentity(storage.io(), "/tail.pdf", workspace.data(), workspace.size(), &tailIdentity));
  EXPECT_EQ(tailIdentity.headFingerprint, baseline.headFingerprint);
  EXPECT_NE(tailIdentity.tailFingerprint, baseline.tailFingerprint);
}

TEST(PdfCacheRecovery, SelectsHighestValidMatchingManifestIncludingSequenceWrap) {
  PdfTestCacheIo storage;
  RecordTable table{{record("metadata.bin", 12, 1)}};
  PdfCacheManifest old = manifest(UINT32_MAX, 41);
  sealManifest(&old, table);
  commit(storage, old, table);

  PdfCacheStore store;
  ASSERT_TRUE(store.initialize(storage.io(), kRoot));
  PdfCacheManifestSelection prior{};
  ASSERT_TRUE(store.loadManifestSlots(old.source, &prior));
  PdfCacheManifest wrapped = manifest(0, 42);
  sealManifest(&wrapped, table);
  PdfCacheManifestSelection latest{};
  ASSERT_TRUE(store.commitManifest(wrapped, table.source(), evidence(wrapped), prior, &latest));
  ASSERT_TRUE(latest.selected);
  EXPECT_EQ(latest.manifest.sequence, 0u);
  EXPECT_EQ(latest.manifest.generation, 42u);
  EXPECT_TRUE(pdfCacheSequenceNewer(0, UINT32_MAX));
  EXPECT_FALSE(pdfCacheSequenceNewer(UINT32_MAX, 0));
}

TEST(PdfCacheRecovery, IgnoresCorruptTruncatedAndIdentityMismatchedSlots) {
  PdfTestCacheIo storage;
  RecordTable table{{record("metadata.bin", 12, 1)}};
  PdfCacheManifest first = manifest(1, 11);
  sealManifest(&first, table);
  commit(storage, first, table);
  PdfCacheManifest second = manifest(2, 12);
  sealManifest(&second, table);
  commit(storage, second, table);

  const std::string newestPath = std::string(kRoot) + "/manifest.b";
  storage.truncateFile(newestPath, storage.bytes(newestPath).size() / 2);

  PdfCacheStore store;
  ASSERT_TRUE(store.initialize(storage.io(), kRoot));
  PdfCacheManifestSelection selected{};
  ASSERT_TRUE(store.loadManifestSlots(first.source, &selected));
  ASSERT_TRUE(selected.selected);
  EXPECT_EQ(selected.manifest.sequence, 1u);

  PdfCacheManifestSelection mismatch{};
  ASSERT_TRUE(store.loadManifestSlots(identity(9999), &mismatch));
  EXPECT_FALSE(mismatch.selected);
}

TEST(PdfCacheRecovery, SelectsHighestValidMatchingCheckpoint) {
  PdfTestCacheIo storage;
  PdfCacheStore store;
  ASSERT_TRUE(store.initialize(storage.io(), kRoot));
  PdfBuildCheckpoint first{};
  first.sequence = UINT32_MAX;
  first.source = identity();
  first.generation = 9;
  first.phase = PdfBuildPhase::ParsePages;
  ASSERT_TRUE(store.commitCheckpoint(first));
  PdfBuildCheckpoint wrapped = first;
  wrapped.sequence = 0;
  wrapped.lastVerifiedPage = 8;
  ASSERT_TRUE(store.commitCheckpoint(wrapped));

  PdfBuildCheckpointSelection selected{};
  ASSERT_TRUE(store.loadCheckpointSlots(identity(), &selected));
  ASSERT_TRUE(selected.selected);
  EXPECT_EQ(selected.checkpoint.sequence, 0u);
  EXPECT_EQ(selected.checkpoint.lastVerifiedPage, 8u);
}

TEST(PdfCacheRecovery, CheckpointCommitWritesOnlyTheAlternatingInactiveSlot) {
  PdfTestCacheIo storage;
  PdfCacheStore store;
  ASSERT_TRUE(store.initialize(storage.io(), kRoot));
  PdfBuildCheckpoint checkpoint{};
  checkpoint.sequence = 1;
  checkpoint.source = identity();
  checkpoint.generation = 9;
  checkpoint.phase = PdfBuildPhase::ParsePages;

  const uint32_t firstOpenCount = storage.openCalls();
  ASSERT_TRUE(store.commitCheckpoint(checkpoint));
  EXPECT_EQ(storage.openCalls() - firstOpenCount, 2u);
  checkpoint.sequence = 2;
  checkpoint.lastVerifiedPage = 8;
  const uint32_t secondOpenCount = storage.openCalls();
  ASSERT_TRUE(store.commitCheckpoint(checkpoint));
  EXPECT_EQ(storage.openCalls() - secondOpenCount, 2u);

  PdfBuildCheckpointSelection selected{};
  ASSERT_TRUE(store.loadCheckpointSlots(identity(), &selected));
  ASSERT_TRUE(selected.selected);
  EXPECT_EQ(selected.checkpoint.sequence, 2u);
  EXPECT_EQ(selected.checkpoint.lastVerifiedPage, 8u);
}

TEST(PdfCachePolicy, DebouncesRoutineCheckpointsButForcesTerminalState) {
  PdfCheckpointGate gate{};
  pdfCheckpointCommitted(&gate, 10, 1000, 10000);
  EXPECT_FALSE(pdfCheckpointDue(gate, 17, 1000 + 512 * 1024 - 1, 16000, false));
  EXPECT_FALSE(pdfCheckpointDue(gate, 18, 1000, 14999, false));
  EXPECT_TRUE(pdfCheckpointDue(gate, 18, 1000, 15000, false));
  EXPECT_TRUE(pdfCheckpointDue(gate, 10, 1000 + 512 * 1024, 15000, false));
  EXPECT_TRUE(pdfCheckpointDue(gate, 10, 1000, 10001, true));
}

TEST(PdfCachePolicy, EnforcesHardCapReserveAndOptionalFirstOmission) {
  PdfCacheCapacity known{{true, 100ULL * 1024ULL * 1024ULL}, {true, 40ULL * 1024ULL * 1024ULL}};
  PdfCacheBudget budget{};
  ASSERT_TRUE(pdfInitializeCacheBudget(10ULL * 1024ULL * 1024ULL, known, 20ULL * 1024ULL * 1024ULL, &budget));
  EXPECT_EQ(budget.hardLimit, 21ULL * 1024ULL * 1024ULL);
  EXPECT_EQ(budget.limit, 21ULL * 1024ULL * 1024ULL);
  EXPECT_EQ(budget.requiredReserve, 20ULL * 1024ULL * 1024ULL);
  EXPECT_EQ(pdfReserveCacheBytes(&budget, 2ULL * 1024ULL * 1024ULL, PdfCacheFileKind::Optional).error,
            PdfError::Unsupported);
  EXPECT_TRUE(budget.optionalOmitted);
  EXPECT_TRUE(pdfReserveCacheBytes(&budget, 20ULL * 1024ULL * 1024ULL, PdfCacheFileKind::Required));
  EXPECT_EQ(pdfReserveCacheBytes(&budget, 2ULL * 1024ULL * 1024ULL, PdfCacheFileKind::Required).error,
            PdfError::InsufficientStorage);

  PdfCacheCapacity lowFree{{true, 64ULL * 1024ULL * 1024ULL}, {true, 16ULL * 1024ULL * 1024ULL}};
  PdfCacheBudget unavailable{};
  ASSERT_TRUE(pdfInitializeCacheBudget(1, lowFree, 0, &unavailable));
  EXPECT_EQ(unavailable.limit, 0u);
}

TEST(PdfCacheCleanup, RemovesOnlyUnreferencedSafeGenerationDirectories) {
  PdfTestCacheIo storage;
  RecordTable table{{record("metadata.bin", 12, 1)}};
  PdfCacheManifest first = manifest(1, 7);
  sealManifest(&first, table);
  commit(storage, first, table);
  PdfCacheManifest second = manifest(2, 8);
  sealManifest(&second, table);
  commit(storage, second, table);
  storage.addDirectory(std::string(kRoot) + "/gen_7");
  storage.addDirectory(std::string(kRoot) + "/gen_8");
  storage.addDirectory(std::string(kRoot) + "/gen_9", true);
  storage.addDirectory(std::string(kRoot) + "/gen_10");
  storage.addFile(std::string(kRoot) + "/gen_10/partial.bin", "partial");
  storage.addDirectory(std::string(kRoot) + "/gen_bad");
  storage.addDirectory(std::string(kRoot) + "/..");
  storage.addFile(std::string(kRoot) + "/foreign", "keep");

  PdfCacheStore store;
  ASSERT_TRUE(store.initialize(storage.io(), kRoot));
  ASSERT_TRUE(store.cleanupUnreferencedGenerations());
  EXPECT_TRUE(storage.exists(std::string(kRoot) + "/gen_7"));
  EXPECT_TRUE(storage.exists(std::string(kRoot) + "/gen_8"));
  EXPECT_TRUE(storage.exists(std::string(kRoot) + "/gen_9"));
  EXPECT_FALSE(storage.exists(std::string(kRoot) + "/gen_10"));
  EXPECT_TRUE(storage.exists(std::string(kRoot) + "/gen_bad"));
  EXPECT_TRUE(storage.exists(std::string(kRoot) + "/.."));
  EXPECT_TRUE(storage.exists(std::string(kRoot) + "/foreign"));
}

TEST(PdfCacheCleanup, MissingRootIsANoOpAndUnsafeRootsOrRelativePathsAreRejected) {
  PdfTestCacheIo storage;
  PdfCacheStore missing;
  ASSERT_TRUE(missing.initialize(storage.io(), kRoot));
  EXPECT_TRUE(missing.cleanupUnreferencedGenerations());

  PdfCacheStore unsafe;
  EXPECT_FALSE(unsafe.initialize(storage.io(), "/").ok());
  EXPECT_FALSE(unsafe.initialize(storage.io(), "/.crosspoint/../foreign").ok());
  EXPECT_FALSE(unsafe.initialize(storage.io(), "relative").ok());
  EXPECT_FALSE(pdfValidateCacheRelativePath("../metadata.bin", 15));
  EXPECT_FALSE(pdfValidateCacheRelativePath("/metadata.bin", 13));
  EXPECT_FALSE(pdfValidateCacheRelativePath("sections\\page.xhtml", 19));
  EXPECT_FALSE(pdfValidateCacheRelativePath("C:metadata.bin", 14));
  EXPECT_TRUE(pdfValidateCacheRelativePath("sections/page.xhtml", 19));
}

TEST(PdfCacheWriter, AccumulatesSizeAndCrcWithoutRereadingOutput) {
  PdfTestCacheIo storage;
  storage.addDirectory(kRoot);
  storage.addDirectory(std::string(kRoot) + "/gen_1");
  PdfCacheTrackedWriter writer{};
  ASSERT_TRUE(pdfOpenTrackedCacheWriter(storage.io(), (std::string(kRoot) + "/gen_1/metadata.bin").c_str(),
                                        "metadata.bin", PdfCacheFileKind::Required, 1024, &writer));
  const std::array<uint8_t, 5> first{{1, 2, 3, 4, 5}};
  const std::array<uint8_t, 3> second{{6, 7, 8}};
  ASSERT_TRUE(pdfWriteTrackedCacheFile(&writer, first.data(), first.size()));
  ASSERT_TRUE(pdfWriteTrackedCacheFile(&writer, second.data(), second.size()));
  const uint32_t readCallsBeforeClose = storage.readCalls();
  PdfRequiredFileRecord result{};
  ASSERT_TRUE(pdfCloseTrackedCacheFile(&writer, &result));
  EXPECT_EQ(result.size, 8u);
  EXPECT_EQ(result.crc32, pdfCacheCrc32(second.data(), second.size(), pdfCacheCrc32(first.data(), first.size(), 0)));
  EXPECT_EQ(storage.readCalls(), readCallsBeforeClose);
  EXPECT_EQ(storage.openHandleCount(), 0u);
}
