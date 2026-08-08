#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "PdfCacheManifest.h"
#include "PdfCacheStore.h"
#include "PdfTestCacheIo.h"

namespace {

constexpr char kRoot[] = "/.crosspoint/pdf_99";

PdfSourceIdentity identity() { return {2222, {false, 0}, 0x1111222233334444ULL, 0xaaaabbbbccccddddULL}; }

PdfRequiredFileRecord record() {
  PdfRequiredFileRecord result{};
  constexpr char path[] = "sections/000000.xhtml";
  result.pathLength = sizeof(path) - 1;
  std::memcpy(result.path, path, sizeof(path));
  result.size = 321;
  result.crc32 = 0x12345678;
  return result;
}

struct OneRecord {
  PdfRequiredFileRecord value = record();

  static PdfStatus read(void* context, const uint32_t index, PdfRequiredFileRecord* output) {
    if (context == nullptr || output == nullptr || index != 0) {
      return PdfStatus::failure(PdfError::InvalidArgument);
    }
    *output = static_cast<OneRecord*>(context)->value;
    return PdfStatus::success();
  }

  PdfRequiredFileTableSource source() { return {this, 1, read}; }
};

PdfCacheManifest makeManifest(const uint32_t sequence, const uint32_t generation, const OneRecord& table) {
  PdfCacheManifest value{};
  value.formatVersion = PDF_CACHE_FORMAT_VERSION;
  value.capabilityVersion = PDF_CACHE_CAPABILITY_VERSION;
  value.sequence = sequence;
  value.completed = true;
  value.source = identity();
  value.generation = generation;
  value.totalWords = 12;
  value.requiredFileCount = 1;
  value.requiredFileBytes = table.value.size;
  value.requiredFileLedger = pdfUpdateRequiredFileLedger(PDF_CACHE_FNV64_OFFSET, table.value);
  return value;
}

PdfCacheCommitEvidence evidence(const PdfCacheManifest& value, const bool writersClosed = true) {
  return {writersClosed, value.requiredFileCount, value.requiredFileBytes, value.requiredFileLedger};
}

void initializeWithOld(PdfTestCacheIo* storage, PdfCacheStore* store, OneRecord* table,
                       PdfCacheManifestSelection* oldSelection) {
  ASSERT_NE(storage, nullptr);
  ASSERT_NE(store, nullptr);
  ASSERT_NE(table, nullptr);
  ASSERT_NE(oldSelection, nullptr);
  ASSERT_TRUE(store->initialize(storage->io(), kRoot));
  PdfCacheManifestSelection empty{};
  ASSERT_TRUE(store->loadManifestSlots(identity(), &empty));
  const PdfCacheManifest old = makeManifest(40, 40, *table);
  ASSERT_TRUE(store->commitManifest(old, table->source(), evidence(old), empty, oldSelection));
  ASSERT_TRUE(oldSelection->selected);
}

void expectOldStillSelected(PdfTestCacheIo& storage) {
  storage.clearFault();
  storage.clearWriteAllowance();
  PdfCacheStore recovery;
  ASSERT_TRUE(recovery.initialize(storage.io(), kRoot));
  PdfCacheManifestSelection selected{};
  ASSERT_TRUE(recovery.loadManifestSlots(identity(), &selected));
  ASSERT_TRUE(selected.selected);
  EXPECT_EQ(selected.manifest.sequence, 40u);
  EXPECT_EQ(selected.manifest.generation, 40u);
}

void expectOldSlotStillValid(PdfTestCacheIo& storage) {
  storage.clearFault();
  storage.clearWriteAllowance();
  PdfCacheStore recovery;
  ASSERT_TRUE(recovery.initialize(storage.io(), kRoot));
  PdfCacheManifestSelection selected{};
  ASSERT_TRUE(recovery.loadManifestSlots(identity(), &selected));
  const bool oldValid = (selected.slots[0].valid && selected.slots[0].manifest.sequence == 40) ||
                        (selected.slots[1].valid && selected.slots[1].manifest.sequence == 40);
  EXPECT_TRUE(oldValid);
}

}  // namespace

TEST(PdfCacheFaultInjection, EveryInactiveManifestTornWritePrefixKeepsOlderSlot) {
  OneRecord table;

  PdfTestCacheIo sizingStorage;
  PdfCacheStore sizingStore;
  PdfCacheManifestSelection sizingOld{};
  initializeWithOld(&sizingStorage, &sizingStore, &table, &sizingOld);
  const PdfCacheManifest candidate = makeManifest(41, 41, table);
  ASSERT_TRUE(sizingStore.commitManifest(candidate, table.source(), evidence(candidate), sizingOld, nullptr));
  const std::string candidatePath = sizingOld.selectedSlot == PdfCacheSlot::A ? std::string(kRoot) + "/manifest.b"
                                                                              : std::string(kRoot) + "/manifest.a";
  const size_t encodedLength = sizingStorage.bytes(candidatePath).size();
  ASSERT_GT(encodedLength, 0u);

  for (size_t prefix = 0; prefix < encodedLength; ++prefix) {
    PdfTestCacheIo storage;
    PdfCacheStore store;
    PdfCacheManifestSelection old{};
    initializeWithOld(&storage, &store, &table, &old);
    storage.setWriteAllowance(prefix);
    EXPECT_FALSE(store.commitManifest(candidate, table.source(), evidence(candidate), old, nullptr).ok())
        << "prefix=" << prefix;
    expectOldStillSelected(storage);
  }
}

TEST(PdfCacheFaultInjection, CommitFaultsNeverInvalidateOlderSlot) {
  for (const PdfTestFaultPoint point : {PdfTestFaultPoint::Open, PdfTestFaultPoint::Write, PdfTestFaultPoint::Flush,
                                        PdfTestFaultPoint::Sync, PdfTestFaultPoint::Close, PdfTestFaultPoint::Read}) {
    PdfTestCacheIo storage;
    PdfCacheStore store;
    OneRecord table;
    PdfCacheManifestSelection old{};
    initializeWithOld(&storage, &store, &table, &old);
    storage.fail(point);
    const PdfCacheManifest candidate = makeManifest(41, 41, table);
    EXPECT_FALSE(store.commitManifest(candidate, table.source(), evidence(candidate), old, nullptr).ok())
        << static_cast<int>(point);
    expectOldSlotStillValid(storage);
  }
}

TEST(PdfCacheFaultInjection, EveryInactiveCheckpointTornWritePrefixKeepsOlderSlot) {
  PdfBuildCheckpoint old{};
  old.sequence = 1;
  old.source = identity();
  old.generation = 40;
  old.phase = PdfBuildPhase::ParsePages;
  PdfBuildCheckpoint candidate = old;
  candidate.sequence = 2;
  candidate.lastVerifiedPage = 8;

  PdfTestCacheIo sizingStorage;
  PdfCacheStore sizingStore;
  ASSERT_TRUE(sizingStore.initialize(sizingStorage.io(), kRoot));
  ASSERT_TRUE(sizingStore.commitCheckpoint(old));
  ASSERT_TRUE(sizingStore.commitCheckpoint(candidate));
  const size_t encodedLength = sizingStorage.bytes(std::string(kRoot) + "/build.b").size();
  ASSERT_GT(encodedLength, 0u);

  for (size_t prefix = 0; prefix < encodedLength; ++prefix) {
    PdfTestCacheIo storage;
    PdfCacheStore store;
    ASSERT_TRUE(store.initialize(storage.io(), kRoot));
    ASSERT_TRUE(store.commitCheckpoint(old));
    storage.setWriteAllowance(prefix);
    EXPECT_FALSE(store.commitCheckpoint(candidate).ok()) << "prefix=" << prefix;
    storage.clearWriteAllowance();
    PdfBuildCheckpointSelection selected{};
    ASSERT_TRUE(store.loadCheckpointSlots(identity(), &selected));
    ASSERT_TRUE(selected.selected);
    EXPECT_EQ(selected.checkpoint.sequence, 1u) << "prefix=" << prefix;
  }
}

TEST(PdfCacheFaultInjection, ValidationAndOpenWriterStateFailBeforeCommitMarkerWrite) {
  PdfTestCacheIo storage;
  PdfCacheStore store;
  OneRecord table;
  PdfCacheManifestSelection old{};
  initializeWithOld(&storage, &store, &table, &old);
  const PdfCacheManifest candidate = makeManifest(41, 41, table);
  const uint32_t writesBefore = storage.writeCalls();

  PdfCacheCommitEvidence wrong = evidence(candidate);
  ++wrong.requiredFileCount;
  EXPECT_EQ(store.commitManifest(candidate, table.source(), wrong, old, nullptr).error, PdfError::Malformed);
  EXPECT_EQ(storage.writeCalls(), writesBefore);

  EXPECT_EQ(store.commitManifest(candidate, table.source(), evidence(candidate, false), old, nullptr).error,
            PdfError::InvalidArgument);
  EXPECT_EQ(storage.writeCalls(), writesBefore);
  expectOldStillSelected(storage);
}

TEST(PdfCacheFaultInjection, TrackedWriterFailsClosedAtEveryDurabilityBoundary) {
  for (const PdfTestFaultPoint point : {PdfTestFaultPoint::Open, PdfTestFaultPoint::Write, PdfTestFaultPoint::Sync,
                                        PdfTestFaultPoint::Close}) {
    PdfTestCacheIo storage;
    storage.addDirectory(kRoot);
    PdfCacheTrackedWriter writer{};
    storage.fail(point);
    PdfStatus status = pdfOpenTrackedCacheWriter(storage.io(), (std::string(kRoot) + "/metadata.bin").c_str(),
                                                 "metadata.bin", PdfCacheFileKind::Required, 16, &writer);
    if (point == PdfTestFaultPoint::Open) {
      EXPECT_FALSE(status.ok());
      continue;
    }
    ASSERT_TRUE(status);
    const std::array<uint8_t, 4> bytes{{1, 2, 3, 4}};
    status = pdfWriteTrackedCacheFile(&writer, bytes.data(), bytes.size());
    if (point == PdfTestFaultPoint::Write) {
      EXPECT_FALSE(status.ok());
      pdfAbortTrackedCacheFile(&writer);
      continue;
    }
    ASSERT_TRUE(status);
    PdfRequiredFileRecord output{};
    EXPECT_FALSE(pdfCloseTrackedCacheFile(&writer, &output).ok()) << static_cast<int>(point);
    EXPECT_EQ(storage.openHandleCount(), 0u);
  }

  PdfTestCacheIo shortStorage;
  PdfCacheTrackedWriter shortWriter{};
  ASSERT_TRUE(pdfOpenTrackedCacheWriter(shortStorage.io(), "/short.bin", "short.bin", PdfCacheFileKind::Required, 16,
                                        &shortWriter));
  shortStorage.setWriteAllowance(2);
  const std::array<uint8_t, 4> bytes{{1, 2, 3, 4}};
  EXPECT_FALSE(pdfWriteTrackedCacheFile(&shortWriter, bytes.data(), bytes.size()).ok());
  pdfAbortTrackedCacheFile(&shortWriter);
}

TEST(PdfCacheFaultInjection, TrackedWriterUsesOneStatusBearingSyncWithoutRedundantFlush) {
  PdfTestCacheIo storage;
  storage.addDirectory(kRoot);
  const std::string path = std::string(kRoot) + "/metadata.bin";
  PdfCacheTrackedWriter writer{};
  ASSERT_TRUE(pdfOpenTrackedCacheWriter(storage.io(), path.c_str(), "metadata.bin", PdfCacheFileKind::Required, 16,
                                        &writer));
  const std::array<uint8_t, 4> bytes{{1, 2, 3, 4}};
  ASSERT_TRUE(pdfWriteTrackedCacheFile(&writer, bytes.data(), bytes.size()));

  PdfRequiredFileRecord output{};
  ASSERT_TRUE(pdfCloseTrackedCacheFile(&writer, &output));

  EXPECT_EQ(storage.flushCalls(), 0u);
  EXPECT_EQ(storage.syncCalls(), 1u);
  EXPECT_EQ(storage.closeCalls(), 1u);
  ASSERT_EQ(storage.syncObservations().size(), 1u);
  EXPECT_EQ(storage.syncObservations().front(), path);
  EXPECT_EQ(storage.openHandleCount(), 0u);
}

TEST(PdfCacheFaultInjection, CleanupFailureLeavesCommittedSelectionRecoverable) {
  PdfTestCacheIo storage;
  PdfCacheStore store;
  OneRecord table;
  PdfCacheManifestSelection old{};
  initializeWithOld(&storage, &store, &table, &old);
  const PdfCacheManifest candidate = makeManifest(41, 41, table);
  PdfCacheManifestSelection latest{};
  ASSERT_TRUE(store.commitManifest(candidate, table.source(), evidence(candidate), old, &latest));
  storage.addDirectory(std::string(kRoot) + "/gen_39");
  storage.fail(PdfTestFaultPoint::Remove);
  EXPECT_FALSE(store.cleanupUnreferencedGenerations().ok());
  storage.clearFault();

  PdfCacheManifestSelection selected{};
  ASSERT_TRUE(store.loadManifestSlots(identity(), &selected));
  ASSERT_TRUE(selected.selected);
  EXPECT_EQ(selected.manifest.sequence, 41u);
}
