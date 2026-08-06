#include "PdfDirectoryDeleteScan.h"

#include <HalStorage.h>
#include <Memory.h>

#include <algorithm>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

int failures = 0;
size_t mixedDirectoryIterations = 0;
size_t mixedExplicitHeapBytes = 0;
size_t mixedSpoolWrittenBytes = 0;
size_t mixedSpoolReadBytes = 0;

void expect(const bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

struct CallbackLog {
  std::vector<std::string> deletedPdfs;
  std::vector<std::string> clearedLegacy;
  bool failDelete = false;
  bool spoolWasClosedForEveryDelete = true;
  bool failNextReplayReadAfterFirstClear = false;
  size_t failLegacyAt = 0;
};

bool deletePdf(void* const context, const char* const path) {
  auto& log = *static_cast<CallbackLog*>(context);
  log.spoolWasClosedForEveryDelete =
      log.spoolWasClosedForEveryDelete && Storage.activeFileHandles() == 0;
  const std::string exactPath = path == nullptr ? "" : path;
  Storage.events.emplace_back("pdf-callback:" + exactPath);
  if (log.failDelete) return false;
  log.deletedPdfs.push_back(exactPath);
  return Storage.remove(exactPath.c_str());
}

bool clearLegacy(void* const context, const std::string_view path) {
  auto& log = *static_cast<CallbackLog*>(context);
  log.clearedLegacy.emplace_back(path);
  Storage.events.emplace_back(std::string("clear:") + std::string(path));
  if (log.failNextReplayReadAfterFirstClear &&
      log.clearedLegacy.size() == 1U) {
    Storage.failNextRead();
  }
  return log.clearedLegacy.size() != log.failLegacyAt;
}

PdfDirectoryDeleteScan::DeleteCallbacks callbacksFor(CallbackLog& log) {
  return {&log, &deletePdf, &clearLegacy};
}

void reset() {
  Storage.reset();
  TestMemory::reset();
  Storage.mkdir("/.crosspoint");
  Storage.mkdir("/Books");
}

void expectPreflightFailurePreservesTree(
    const PdfDirectoryDeleteScan::Status actual,
    const PdfDirectoryDeleteScan::Status expected, const CallbackLog& log,
    const std::string& context) {
  expect(actual == expected, context + " must report the exact failure");
  expect(Storage.exists("/Books") && Storage.exists("/Books/book.pdf") &&
             Storage.exists("/Books/keep.epub"),
         context + " must preserve the selected tree before mutation");
  expect(log.deletedPdfs.empty() && log.clearedLegacy.empty(),
         context + " must not invoke PDF or metadata mutation callbacks");
  expect(!Storage.exists(PdfDirectoryDeleteScan::kSpoolTempPath) &&
             !Storage.exists(PdfDirectoryDeleteScan::kSpoolSealedPath),
         context + " must clean temporary and sealed spools");
}

void putSimpleMixedTree() {
  Storage.putFile("/Books/book.pdf", {1});
  Storage.putFile("/Books/keep.epub", {2});
}

void testFirstPdfHasNoCountCapAndClosesSpoolBeforeJournalCallbacks() {
  reset();
  constexpr size_t kPdfCount = 130;
  for (size_t index = 0; index < kPdfCount; ++index) {
    std::string ordinal = std::to_string(1000U + index);
    Storage.putFile("/Books/" + ordinal + ".pdf", {1});
  }
  Storage.putFile("/Books/keep.epub", {2});
  CallbackLog log;
  const auto status = PdfDirectoryDeleteScan::deletePdfDirectoryNoThrow(
      "/Books", callbacksFor(log));

  expect(status == PdfDirectoryDeleteScan::Status::Complete,
         "130 PDFs must replay without the former 128-record cap (status=" +
             std::to_string(static_cast<unsigned>(status)) + ")");
  expect(log.deletedPdfs.size() == kPdfCount,
         "every spooled PDF must use the journal callback");
  expect(log.spoolWasClosedForEveryDelete,
         "spool reader must close before every PDF journal callback");
  expect(log.clearedLegacy ==
             std::vector<std::string>({"/Books/keep.epub"}),
         "mixed deletion must preserve legacy metadata cleanup");
  expect(!Storage.exists("/Books") &&
             !Storage.exists(PdfDirectoryDeleteScan::kSpoolTempPath) &&
             !Storage.exists(PdfDirectoryDeleteScan::kSpoolSealedPath),
         "successful mixed deletion must remove the tree and spool");
}

void testLatePdfDefersSpoolAndQuantifiesMixedCost() {
  reset();
  constexpr size_t kLegacyDirectories = 70;
  for (size_t index = 0; index < kLegacyDirectories; ++index) {
    Storage.putFile("/Books/a" + std::to_string(1000U + index) +
                        "/book.epub",
                    {1});
  }
  Storage.putFile("/Books/zz-last.pdf", {2});
  CallbackLog log;
  const auto status = PdfDirectoryDeleteScan::deletePdfDirectoryNoThrow(
      "/Books", callbacksFor(log));

  expect(status == PdfDirectoryDeleteScan::Status::Complete &&
             log.deletedPdfs ==
                 std::vector<std::string>({"/Books/zz-last.pdf"}) &&
             log.clearedLegacy.size() == kLegacyDirectories,
         "late PDF must be discovered, sealed, validated, and replayed (status=" +
             std::to_string(static_cast<unsigned>(status)) + ")");
  const auto lastLegacyOpen =
      std::find(Storage.events.begin(), Storage.events.end(),
                "open-direct:/Books/a1069");
  const auto firstLegacyOpen =
      std::find(Storage.events.begin(), Storage.events.end(),
                "open-direct:/Books/a1000");
  const auto firstSpoolOpen =
      std::find(Storage.events.begin(), Storage.events.end(),
                std::string("open-file-write:") +
                    PdfDirectoryDeleteScan::kSpoolTempPath);
  expect(firstLegacyOpen != Storage.events.end() &&
             lastLegacyOpen != Storage.events.end() &&
             firstSpoolOpen != Storage.events.end() &&
             firstLegacyOpen < firstSpoolOpen &&
             firstSpoolOpen < lastLegacyOpen,
         "typed spool must open at the first retained legacy path, not allocate per book");

  mixedDirectoryIterations = Storage.directoryIterationCalls();
  mixedExplicitHeapBytes = 0;
  for (const size_t bytes : TestMemory::allocationSizes) {
    mixedExplicitHeapBytes += bytes;
  }
  mixedSpoolWrittenBytes =
      Storage.pathWrittenBytes(PdfDirectoryDeleteScan::kSpoolTempPath);
  mixedSpoolReadBytes =
      Storage.pathReadBytes(PdfDirectoryDeleteScan::kSpoolSealedPath);
}

void testSpoolFailuresAbortBeforeTreeMutationAndCleanArtifacts() {
  reset();
  putSimpleMixedTree();
  TestMemory::failAllocationCall = 2;
  CallbackLog log;
  expectPreflightFailurePreservesTree(
      PdfDirectoryDeleteScan::deletePdfDirectoryNoThrow("/Books",
                                                     callbacksFor(log)),
      PdfDirectoryDeleteScan::Status::AllocationFailure, log,
      "spool workspace OOM");

  reset();
  putSimpleMixedTree();
  Storage.failNextOpenOf(PdfDirectoryDeleteScan::kSpoolTempPath);
  log = {};
  expectPreflightFailurePreservesTree(
      PdfDirectoryDeleteScan::deletePdfDirectoryNoThrow("/Books",
                                                     callbacksFor(log)),
      PdfDirectoryDeleteScan::Status::SpoolOpenFailure, log,
      "spool create failure");

  reset();
  putSimpleMixedTree();
  Storage.failNextShortWrite();
  log = {};
  expectPreflightFailurePreservesTree(
      PdfDirectoryDeleteScan::deletePdfDirectoryNoThrow("/Books",
                                                     callbacksFor(log)),
      PdfDirectoryDeleteScan::Status::SpoolWriteFailure, log,
      "spool short write");

  reset();
  putSimpleMixedTree();
  Storage.failNextSync();
  log = {};
  expectPreflightFailurePreservesTree(
      PdfDirectoryDeleteScan::deletePdfDirectoryNoThrow("/Books",
                                                     callbacksFor(log)),
      PdfDirectoryDeleteScan::Status::SpoolSyncFailure, log,
      "spool sync failure");

  reset();
  putSimpleMixedTree();
  Storage.failNextOpenOf(PdfDirectoryDeleteScan::kSpoolSealedPath);
  log = {};
  expectPreflightFailurePreservesTree(
      PdfDirectoryDeleteScan::deletePdfDirectoryNoThrow("/Books",
                                                     callbacksFor(log)),
      PdfDirectoryDeleteScan::Status::SpoolOpenFailure, log,
      "sealed spool validation open failure");

  reset();
  putSimpleMixedTree();
  Storage.failNextRead();
  log = {};
  expectPreflightFailurePreservesTree(
      PdfDirectoryDeleteScan::deletePdfDirectoryNoThrow("/Books",
                                                     callbacksFor(log)),
      PdfDirectoryDeleteScan::Status::SpoolReadFailure, log,
      "sealed spool validation read failure");

  reset();
  putSimpleMixedTree();
  Storage.corruptOnNextSyncOf(PdfDirectoryDeleteScan::kSpoolTempPath);
  log = {};
  expectPreflightFailurePreservesTree(
      PdfDirectoryDeleteScan::deletePdfDirectoryNoThrow("/Books",
                                                     callbacksFor(log)),
      PdfDirectoryDeleteScan::Status::SpoolCorrupt, log,
      "sealed spool CRC corruption");
}

void testStaleRebootSpoolsCleanBeforeDiscoveryAndTraversalAliasRejects() {
  reset();
  Storage.putFile(PdfDirectoryDeleteScan::kSpoolTempPath, {1, 2, 3});
  Storage.putFile(PdfDirectoryDeleteScan::kSpoolSealedPath, {4, 5, 6});
  Storage.putFile("/Books/keep.epub", {7});
  CallbackLog log;
  const auto recovered = PdfDirectoryDeleteScan::deletePdfDirectoryNoThrow(
      "/Books", callbacksFor(log));
  expect(recovered == PdfDirectoryDeleteScan::Status::Complete &&
             !Storage.exists(PdfDirectoryDeleteScan::kSpoolTempPath) &&
             !Storage.exists(PdfDirectoryDeleteScan::kSpoolSealedPath),
         "stale reboot artifacts must clean before a fresh legacy discovery");
  const auto staleRemove =
      std::find(Storage.events.begin(), Storage.events.end(),
                std::string("remove:") +
                    PdfDirectoryDeleteScan::kSpoolSealedPath);
  const auto discoveryOpen =
      std::find(Storage.events.begin(), Storage.events.end(),
                "open-direct:/Books");
  expect(staleRemove != Storage.events.end() &&
             discoveryOpen != Storage.events.end() &&
             staleRemove < discoveryOpen,
         "stale spool cleanup must finish before directory discovery");

  reset();
  Storage.putFile("/Books/../escape.pdf", {8});
  log = {};
  const auto alias = PdfDirectoryDeleteScan::deletePdfDirectoryNoThrow(
      "/Books", callbacksFor(log));
  expect(alias == PdfDirectoryDeleteScan::Status::PathLimit &&
             Storage.exists("/Books") && log.deletedPdfs.empty(),
         "dot-dot traversal alias must fail before spool or tree mutation");
}

void testPreflightAndPostCommitCleanupFailuresRemainDistinct() {
  reset();
  putSimpleMixedTree();
  Storage.putFile(PdfDirectoryDeleteScan::kSpoolSealedPath, {9});
  Storage.failNextRemoveOf(PdfDirectoryDeleteScan::kSpoolSealedPath);
  CallbackLog log;

  auto status = PdfDirectoryDeleteScan::deletePdfDirectoryNoThrow(
      "/Books", callbacksFor(log));
  expect(status == PdfDirectoryDeleteScan::Status::SpoolCleanupFailure &&
             Storage.exists("/Books/book.pdf") &&
             Storage.exists("/Books/keep.epub") && log.deletedPdfs.empty() &&
             log.clearedLegacy.empty(),
         "preflight spool cleanup failure must remain a hard failure before mutation");

  reset();
  putSimpleMixedTree();
  Storage.failNextRemoveOf(PdfDirectoryDeleteScan::kSpoolSealedPath);
  log = {};
  status = PdfDirectoryDeleteScan::deletePdfDirectoryNoThrow(
      "/Books", callbacksFor(log));
  expect(status ==
             PdfDirectoryDeleteScan::Status::CommittedWithCleanupWarning,
         "strict post-commit spool cleanup failure must be distinguishable success");
  expect(!Storage.exists("/Books") &&
             log.deletedPdfs ==
                 std::vector<std::string>{"/Books/book.pdf"} &&
             log.clearedLegacy ==
                 std::vector<std::string>{"/Books/keep.epub"},
         "strict cleanup warning must retain completed PDF and metadata deletion");
  expect(Storage.exists(PdfDirectoryDeleteScan::kSpoolSealedPath),
         "strict cleanup warning must leave the failed spool artifact observable");
}

void testStrictReplayReadFailureAfterCommitIsDistinguishable() {
  reset();
  Storage.putFile("/Books/a.epub", {1});
  Storage.putFile("/Books/b.epub", {2});
  Storage.putFile("/Books/z.pdf", {3});
  CallbackLog log;
  log.failNextReplayReadAfterFirstClear = true;

  const auto status = PdfDirectoryDeleteScan::deletePdfDirectoryNoThrow(
      "/Books", callbacksFor(log));

  expect(status ==
             PdfDirectoryDeleteScan::Status::CommittedWithCleanupWarning,
         "strict post-commit replay read failure must remain distinguishable committed success");
  expect(!Storage.exists("/Books") &&
             log.deletedPdfs ==
                 std::vector<std::string>{"/Books/z.pdf"} &&
             log.clearedLegacy ==
                 std::vector<std::string>{"/Books/a.epub"},
         "strict replay failure must retain committed PDF/tree deletion and completed metadata prefix");
}

void testStrictReplayOpenFailureAfterCommitIsDistinguishable() {
  reset();
  Storage.putFile("/Books/a.epub", {1});
  Storage.putFile("/Books/z.pdf", {3});
  Storage.failNextOpenOfAfterRemoveDir(
      PdfDirectoryDeleteScan::kSpoolSealedPath);
  CallbackLog log;

  const auto status = PdfDirectoryDeleteScan::deletePdfDirectoryNoThrow(
      "/Books", callbacksFor(log));

  expect(status ==
             PdfDirectoryDeleteScan::Status::CommittedWithCleanupWarning,
         "strict post-commit replay open allocation failure must remain distinguishable committed success");
  expect(!Storage.exists("/Books") &&
             log.deletedPdfs ==
                 std::vector<std::string>{"/Books/z.pdf"} &&
             log.clearedLegacy.empty(),
         "strict replay open failure must retain committed PDF/tree deletion without claiming metadata replay");
}

void testStrictReplayCloseFailureAfterCommitIsDistinguishable() {
  reset();
  Storage.putFile("/Books/a.epub", {1});
  Storage.putFile("/Books/z.pdf", {3});
  Storage.failNextCloseOfAfterRemoveDir(
      PdfDirectoryDeleteScan::kSpoolSealedPath);
  CallbackLog log;

  const auto status = PdfDirectoryDeleteScan::deletePdfDirectoryNoThrow(
      "/Books", callbacksFor(log));

  expect(status ==
             PdfDirectoryDeleteScan::Status::CommittedWithCleanupWarning,
         "strict post-commit replay close failure must remain distinguishable committed success");
  expect(!Storage.exists("/Books") &&
             log.deletedPdfs ==
                 std::vector<std::string>{"/Books/z.pdf"} &&
             log.clearedLegacy ==
                 std::vector<std::string>{"/Books/a.epub"},
         "strict replay close failure must retain committed PDF/tree deletion and completed metadata replay");
}

void testStrictMetadataCallbackFailuresWarnAndContinueAfterCommit() {
  const std::vector<std::string> expectedPaths{
      "/Books/a.epub", "/Books/b.epub", "/Books/c.epub"};
  for (size_t failedAt = 1; failedAt <= expectedPaths.size(); ++failedAt) {
    reset();
    Storage.putFile(expectedPaths[0], {1});
    Storage.putFile(expectedPaths[1], {2});
    Storage.putFile(expectedPaths[2], {3});
    Storage.putFile("/Books/z.pdf", {4});
    CallbackLog log;
    log.failLegacyAt = failedAt;

    const auto status = PdfDirectoryDeleteScan::deletePdfDirectoryNoThrow(
        "/Books", callbacksFor(log));

    expect(status ==
               PdfDirectoryDeleteScan::Status::CommittedWithCleanupWarning,
           "first, middle, and last strict metadata callback failure must report committed warning");
    expect(!Storage.exists("/Books") &&
               log.deletedPdfs ==
                   std::vector<std::string>{"/Books/z.pdf"} &&
               log.clearedLegacy == expectedPaths,
           "strict metadata callback failure must preserve PDF/tree commit and continue every later callback");
  }
}

}  // namespace

int main() {
  testFirstPdfHasNoCountCapAndClosesSpoolBeforeJournalCallbacks();
  testLatePdfDefersSpoolAndQuantifiesMixedCost();
  testSpoolFailuresAbortBeforeTreeMutationAndCleanArtifacts();
  testStaleRebootSpoolsCleanBeforeDiscoveryAndTraversalAliasRejects();
  testPreflightAndPostCommitCleanupFailuresRemainDistinct();
  testStrictReplayReadFailureAfterCommitIsDistinguishable();
  testStrictReplayOpenFailureAfterCommitIsDistinguishable();
  testStrictReplayCloseFailureAfterCommitIsDistinguishable();
  testStrictMetadataCallbackFailuresWarnAndContinueAfterCommit();
  if (failures != 0) return 1;
  std::cout << "PDF_DIRECTORY_DELETE_SPOOL_PASS"
            << " mixed_iterations=" << mixedDirectoryIterations
            << " mixed_explicit_heap_bytes=" << mixedExplicitHeapBytes
            << " spool_written_bytes=" << mixedSpoolWrittenBytes
            << " spool_read_bytes=" << mixedSpoolReadBytes << '\n';
  return 0;
}
