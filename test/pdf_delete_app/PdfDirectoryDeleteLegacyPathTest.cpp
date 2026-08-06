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
size_t pureDirectoryIterations = 0;
size_t pureExplicitHeapBytes = 0;
size_t pureSpoolWrittenBytes = 0;
size_t pureSpoolReadBytes = 0;
constexpr char kLegacySpoolFirstPath[] =
    "/.crosspoint/pdf-directory-delete.legacy-a";
constexpr char kLegacySpoolRetryPath[] =
    "/.crosspoint/pdf-directory-delete.legacy-b";

size_t countEventsWithPrefix(const std::string& prefix) {
  return static_cast<size_t>(std::count_if(
      Storage.events.begin(), Storage.events.end(),
      [&prefix](const std::string& event) { return event.rfind(prefix, 0) == 0; }));
}

void expect(const bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

struct CallbackLog {
  std::vector<std::string> deletedPdfs;
  std::vector<std::string> clearedLegacy;
  size_t pdfPreparations = 0;
  bool failNextLegacyReplayReadAfterFirstClear = false;
  size_t failLegacyAt = 0;
};

bool deletePdf(void* const context, const char* const path) {
  auto& log = *static_cast<CallbackLog*>(context);
  log.deletedPdfs.emplace_back(path == nullptr ? "" : path);
  Storage.events.emplace_back(std::string("pdf:") + (path == nullptr ? "" : path));
  return true;
}

bool clearLegacy(void* const context, const std::string_view path) {
  auto& log = *static_cast<CallbackLog*>(context);
  log.clearedLegacy.emplace_back(path);
  Storage.events.emplace_back(std::string("clear:") + std::string(path));
  if (log.failNextLegacyReplayReadAfterFirstClear &&
      log.clearedLegacy.size() == 1U) {
    Storage.failNextRead();
  }
  return log.clearedLegacy.size() != log.failLegacyAt;
}

struct AllocationFreeClearLog {
  size_t callbacks = 0;
  bool scannerAllocationsStable = true;
  char firstPath[32]{};
};

bool clearLegacyWithoutPathAllocation(void* const context,
                                      const std::string_view path) {
  auto& log = *static_cast<AllocationFreeClearLog*>(context);
  log.scannerAllocationsStable =
      log.scannerAllocationsStable && TestMemory::allocations == 2U;
  if (log.callbacks == 0U && path.size() < sizeof(log.firstPath)) {
    std::copy(path.begin(), path.end(), log.firstPath);
    log.firstPath[path.size()] = '\0';
  }
  ++log.callbacks;
  return true;
}

bool preparePdf(void* const context) {
  ++static_cast<CallbackLog*>(context)->pdfPreparations;
  return true;
}

void reset() {
  Storage.reset();
  TestMemory::reset();
  Storage.mkdir("/.crosspoint");
  Storage.mkdir("/Books");
}

void testWideAndDeepPureEpubUsesExactLegacyDeletePathWithoutPdfWorkspace() {
  reset();
  constexpr size_t kWideDirectories = 70;
  constexpr size_t kDeepDirectories = 80;
  for (size_t index = 0; index < kWideDirectories; ++index) {
    Storage.putFile("/Books/w" + std::to_string(index) + "/book.epub", {1});
  }
  std::string deepPath = "/Books";
  for (size_t depth = 0; depth < kDeepDirectories; ++depth) {
    deepPath += "/d";
    Storage.putFile(deepPath + "/book.epub", {2});
  }

  // The former unconditional PDF scan allocated exactly 34,976 bytes. A pure
  // legacy tree must never request that allocation or depend on its success.
  TestMemory::failAtOrAboveBytes = 34976U;
  CallbackLog log;
  const PdfDirectoryDeleteScan::DeleteCallbacks callbacks{
      &log, &deletePdf, &clearLegacy};
  const PdfDirectoryDeleteScan::Status status =
      PdfDirectoryDeleteScan::deleteDirectoryNoThrow("/Books", callbacks);

  expect(status == PdfDirectoryDeleteScan::Status::Complete,
         "wide/deep pure EPUB tree must take the successful legacy delete path");
  expect(!Storage.exists("/Books"),
         "legacy recursive directory removal result must remain successful");
  expect(log.deletedPdfs.empty(),
         "pure EPUB deletion must never enter the PDF journal callback");
  expect(log.clearedLegacy.size() == kWideDirectories + kDeepDirectories,
         "legacy metadata cleanup must retain every EPUB path");
  const auto remove = std::find(Storage.events.begin(), Storage.events.end(),
                                "remove-dir:/Books");
  const auto firstClear = std::find_if(
      Storage.events.begin(), Storage.events.end(),
      [](const std::string& event) { return event.rfind("clear:", 0) == 0; });
  expect(remove != Storage.events.end() && firstClear != Storage.events.end() &&
             remove < firstClear,
         "legacy metadata must still clear only after recursive removal");
  expect(Storage.pathOpenCount(PdfDirectoryDeleteScan::kSpoolTempPath) == 0 &&
             Storage.pathOpenCount(PdfDirectoryDeleteScan::kSpoolSealedPath) == 0 &&
             Storage.pathWrittenBytes(PdfDirectoryDeleteScan::kSpoolTempPath) == 0 &&
             Storage.pathWrittenBytes(PdfDirectoryDeleteScan::kSpoolSealedPath) == 0,
         "pure EPUB discovery must never create, open, or write a PDF spool");
  expect(Storage.maximumDirectDirectoryReaders() <= 1,
         "discovery must hold at most one direct directory reader");
  constexpr size_t kDirectories =
      1U + kWideDirectories + kDeepDirectories;
  constexpr size_t kEntries =
      2U * (kWideDirectories + kDeepDirectories);
  expect(Storage.directoryIterationCalls() == kDirectories + kEntries,
         "pure EPUB routing must examine every entry and EOF exactly once");
  expect(std::none_of(TestMemory::allocationSizes.begin(),
                      TestMemory::allocationSizes.end(),
                      [](const size_t bytes) { return bytes >= 34976U; }),
         "pure EPUB deletion must not request the former PDF workspace");

  pureDirectoryIterations = Storage.directoryIterationCalls();
  pureExplicitHeapBytes =
      TestMemory::allocationSizes.empty() ? 0U : TestMemory::allocationSizes.front();
  pureSpoolWrittenBytes =
      Storage.pathWrittenBytes(kLegacySpoolFirstPath) +
      Storage.pathWrittenBytes(kLegacySpoolRetryPath);
  pureSpoolReadBytes = Storage.pathReadBytes(kLegacySpoolFirstPath) +
                       Storage.pathReadBytes(kLegacySpoolRetryPath);
}

void testPureEpubIgnoresStalePdfSpoolCleanupFailure() {
  reset();
  Storage.putFile("/Books/book.epub", {1});
  Storage.putFile(PdfDirectoryDeleteScan::kSpoolTempPath, {9});
  Storage.failNextRemoveOf(PdfDirectoryDeleteScan::kSpoolTempPath);
  CallbackLog log;

  const auto status = PdfDirectoryDeleteScan::deleteDirectoryNoThrow(
      "/Books", {&log, &deletePdf, &clearLegacy});

  expect(status == PdfDirectoryDeleteScan::Status::Complete,
         "pure EPUB deletion must not depend on stale PDF spool cleanup");
  expect(!Storage.exists("/Books"),
         "pure EPUB tree must still use recursive legacy removal");
  expect(Storage.exists(PdfDirectoryDeleteScan::kSpoolTempPath),
         "pure EPUB deletion must not touch unrelated PDF spool state");
  expect(log.deletedPdfs.empty() &&
             log.clearedLegacy == std::vector<std::string>{"/Books/book.epub"},
         "pure EPUB metadata behavior must remain the legacy behavior");
  expect(TestMemory::allocations == 2 &&
             TestMemory::allocationSizes.front() <= 1024U,
         "a shallow legacy scan must use one walker and one reusable replay allocation");
}

void testPureLegacyPathDoesNotImposePdfPathOrNameCaps() {
  reset();
  const std::string longRoot = "/" + std::string(1100, 'r');
  const std::string longRootBook = longRoot + "/book.epub";
  Storage.mkdir(longRoot.c_str());
  Storage.putFile(longRootBook, {1});
  CallbackLog longRootLog;
  auto status = PdfDirectoryDeleteScan::deleteDirectoryNoThrow(
      longRoot, {&longRootLog, &deletePdf, &clearLegacy});
  expect(status == PdfDirectoryDeleteScan::Status::Complete &&
             !Storage.exists(longRoot.c_str()) &&
             longRootLog.clearedLegacy ==
                 std::vector<std::string>{longRootBook},
         "pure EPUB root longer than the PDF path workspace must still delete");

  reset();
  const std::string longName = std::string(300, 'n') + ".epub";
  const std::string longPath = "/Books/" + longName;
  Storage.putFile(longPath, {2});
  CallbackLog longNameLog;
  status = PdfDirectoryDeleteScan::deleteDirectoryNoThrow(
      "/Books", {&longNameLog, &deletePdf, &clearLegacy});
  expect(status == PdfDirectoryDeleteScan::Status::Complete &&
             !Storage.exists("/Books"),
         "pure EPUB entry longer than the PDF name workspace must still delete");
  expect(longNameLog.deletedPdfs.empty() &&
             longNameLog.clearedLegacy == std::vector<std::string>{longPath},
         "truncated simulator names must retry and clear the exact long EPUB metadata path");
}

void testTruncatedLongPdfNameStillRoutesToRecoveredJournalDelete() {
  reset();
  const std::string longName = std::string(180, 'p') + ".pdf";
  const std::string longPath = "/Books/" + longName;
  Storage.putFile(longPath, {3});
  CallbackLog log;

  const auto status = PdfDirectoryDeleteScan::deleteDirectoryNoThrow(
      "/Books", {&log, &deletePdf, &clearLegacy, &preparePdf});

  expect(status == PdfDirectoryDeleteScan::Status::Complete &&
             log.pdfPreparations == 1 &&
             log.deletedPdfs == std::vector<std::string>{longPath} &&
             log.clearedLegacy.empty(),
         "truncated simulator names must still detect and journal the exact long PDF path");
}

void testPdfSuffixedDirectoryRemainsOnLegacyRoute() {
  reset();
  Storage.putFile("/Books/archive.pdf/book.epub", {1});
  CallbackLog log;

  const auto status = PdfDirectoryDeleteScan::deleteDirectoryNoThrow(
      "/Books", {&log, &deletePdf, &clearLegacy, &preparePdf});

  expect(status == PdfDirectoryDeleteScan::Status::Complete &&
             !Storage.exists("/Books") && log.pdfPreparations == 0U &&
             log.deletedPdfs.empty() &&
             log.clearedLegacy ==
                 std::vector<std::string>{"/Books/archive.pdf/book.epub"},
         "a directory whose name ends in .pdf must remain a legacy directory");
  expect(Storage.pathOpenCount(PdfDirectoryDeleteScan::kSpoolTempPath) == 0U &&
             Storage.pathOpenCount(PdfDirectoryDeleteScan::kSpoolSealedPath) == 0U,
         ".pdf-suffixed directories must not enter strict PDF spool work");
}

void testLongLegacyNamesReuseOneFallibleNameBuffer() {
  reset();
  const std::string firstPath =
      "/Books/a" + std::string(180, 'a') + ".epub";
  const std::string secondPath =
      "/Books/b" + std::string(180, 'b') + ".epub";
  Storage.putFile(firstPath, {1});
  Storage.putFile(secondPath, {2});
  CallbackLog log;

  const auto status = PdfDirectoryDeleteScan::deleteDirectoryNoThrow(
      "/Books", {&log, &deletePdf, &clearLegacy, &preparePdf});

  expect(status == PdfDirectoryDeleteScan::Status::Complete &&
             !Storage.exists("/Books") &&
             log.clearedLegacy ==
                 std::vector<std::string>({firstPath, secondPath}),
         "multiple long legacy names must retain baseline deletion and metadata cleanup");
  expect(TestMemory::allocations == 3,
         "long-name traversal must allocate one walker, one grown name buffer, and one replay buffer");
}

void testFlatLegacyMetadataUsesNoPerBookScannerAllocations() {
  reset();
  constexpr size_t kBookCount = 300U;
  for (size_t index = 0; index < kBookCount; ++index) {
    Storage.putFile("/Books/book" + std::to_string(index) + ".epub", {1});
  }
  CallbackLog log;

  const auto status = PdfDirectoryDeleteScan::deleteDirectoryNoThrow(
      "/Books", {&log, &deletePdf, &clearLegacy, &preparePdf});

  expect(status == PdfDirectoryDeleteScan::Status::Complete &&
             !Storage.exists("/Books") &&
             log.clearedLegacy.size() == kBookCount,
         "typed scratch spool must retain every flat legacy metadata path");
  expect(TestMemory::allocations == 2,
         "legacy metadata count must not add scanner heap allocations per book");
  const auto committed = std::find(Storage.events.begin(), Storage.events.end(),
                                   "remove-dir:/Books");
  expect(committed != Storage.events.end() &&
             std::count(committed, Storage.events.end(),
                        std::string("open-file-read:") +
                            kLegacySpoolFirstPath) == 1,
         "300-record post-commit replay must use one sequential spool open");
}

void testLegacyReplayPassesAViewWithoutPostDeleteScannerAllocation() {
  reset();
  Storage.putFile("/Books/a.epub", {1});
  Storage.putFile("/Books/b.xtc", {2});
  Storage.putFile("/Books/c.txt", {3});
  AllocationFreeClearLog log;

  const auto status = PdfDirectoryDeleteScan::deleteDirectoryNoThrow(
      "/Books", {&log, &deletePdf, &clearLegacyWithoutPathAllocation,
                 &preparePdf});

  expect(status == PdfDirectoryDeleteScan::Status::Complete &&
             !Storage.exists("/Books") && log.callbacks == 3U &&
             std::string_view(log.firstPath) == "/Books/a.epub",
         "legacy replay must expose each validated path as a non-owning view");
  expect(log.scannerAllocationsStable && TestMemory::allocations == 2U,
         "legacy replay callbacks must not add a post-delete scanner path allocation");
}

void testLegacyReplayOomFailsBeforeDirectoryMutation() {
  reset();
  Storage.putFile("/Books/book.epub", {1});
  TestMemory::failAllocationCall = 2U;
  CallbackLog log;

  const auto status = PdfDirectoryDeleteScan::deleteDirectoryNoThrow(
      "/Books", {&log, &deletePdf, &clearLegacy, &preparePdf});

  expect(status == PdfDirectoryDeleteScan::Status::AllocationFailure,
         "fallible replay-buffer OOM must be result-bearing");
  expect(Storage.exists("/Books/book.epub") && log.clearedLegacy.empty() &&
             countEventsWithPrefix("remove-dir:") == 0,
         "replay-buffer OOM must fail before directory or metadata mutation");
}

void testPersistentLongNameAllocationFailureFailsClosed() {
  reset();
  Storage.putFile("/Books/a.epub", {1});
  const std::string longName = std::string(180, 'a') + ".epub";
  const std::string longPath = "/Books/" + longName;
  Storage.putFile(longPath, {4});
  TestMemory::failAllocationCall = 2U;
  CallbackLog log;

  const auto status = PdfDirectoryDeleteScan::deleteDirectoryNoThrow(
      "/Books", {&log, &deletePdf, &clearLegacy, &preparePdf});

  expect(status == PdfDirectoryDeleteScan::Status::AllocationFailure,
         "persistent name allocation failure must remain result-bearing");
  expect(Storage.exists("/Books/a.epub") && Storage.exists(longPath.c_str()) &&
             log.deletedPdfs.empty() && log.clearedLegacy.empty() &&
             log.pdfPreparations == 0 &&
             countEventsWithPrefix("remove-dir:") == 0,
         "incomplete allocation discovery must preserve the tree and metadata");
  expect(Storage.pathOpenCount(PdfDirectoryDeleteScan::kSpoolTempPath) == 0 &&
             Storage.pathOpenCount(PdfDirectoryDeleteScan::kSpoolSealedPath) == 0,
         "long-name allocation failure must not enter PDF spool work");
  expect(TestMemory::allocations == 2,
         "failed discovery must retain only the walker and failed name growth allocations");
}

void testPublicRouteRetriesOneShotTraversalFailuresReadOnly() {
  for (const int failureKind : {0, 1, 2}) {
    reset();
    Storage.putFile("/Books/book.epub", {1});
    if (failureKind == 0) {
      Storage.failNextOpenOf("/Books");
    } else if (failureKind == 1) {
      Storage.failNextDirectoryIterationOf("/Books");
    } else {
      Storage.failCloseOnCall(1);
    }
    CallbackLog log;

    const auto status = PdfDirectoryDeleteScan::deleteDirectoryNoThrow(
        "/Books", {&log, &deletePdf, &clearLegacy, &preparePdf});

    expect(status == PdfDirectoryDeleteScan::Status::Complete,
           "one-shot open/iteration/close failure must retry and complete");
    expect(!Storage.exists("/Books") && log.deletedPdfs.empty() &&
               log.pdfPreparations == 0 &&
               log.clearedLegacy == std::vector<std::string>{"/Books/book.epub"},
           "transient retry must retain the pure legacy commit behavior");
    expect(Storage.pathOpenCount(PdfDirectoryDeleteScan::kSpoolTempPath) == 0 &&
               Storage.pathOpenCount(PdfDirectoryDeleteScan::kSpoolSealedPath) == 0,
           "transient legacy discovery failures must never enter PDF spool work");
  }
}

void testIncompleteLegacyDiscoveryPreservesTreeAndMetadata() {
  reset();
  Storage.putFile("/Books/a.epub", {1});
  Storage.putFile("/Books/z/book.txt", {2});
  // Attempt one reaches the child directory but cannot close its entry. The
  // retry reaches the same point and cannot open that child directory.
  Storage.failCloseOnCall(6);
  Storage.failNextOpenOf("/Books/z");
  CallbackLog log;

  const auto status = PdfDirectoryDeleteScan::deleteDirectoryNoThrow(
      "/Books", {&log, &deletePdf, &clearLegacy, &preparePdf});

  expect(status == PdfDirectoryDeleteScan::Status::OpenFailure,
         "incomplete legacy discovery must report its persistent open failure");
  expect(Storage.exists("/Books/a.epub") &&
             Storage.exists("/Books/z/book.txt") && log.deletedPdfs.empty() &&
             log.clearedLegacy.empty() && log.pdfPreparations == 0 &&
             countEventsWithPrefix("remove-dir:") == 0,
         "incomplete legacy discovery must preserve the full tree and all metadata");
  expect(Storage.pathOpenCount(PdfDirectoryDeleteScan::kSpoolTempPath) == 0 &&
             Storage.pathOpenCount(PdfDirectoryDeleteScan::kSpoolSealedPath) == 0,
         "incomplete legacy discovery must not create PDF spools");
}

void testPersistentIterationFailureFailsClosed() {
  reset();
  Storage.putFile("/Books/a.epub", {1});
  Storage.putFile("/Books/z/book.txt", {2});
  Storage.failCloseOnCall(6);
  Storage.failNextDirectoryIterationOf("/Books/z");
  CallbackLog log;

  const auto status = PdfDirectoryDeleteScan::deleteDirectoryNoThrow(
      "/Books", {&log, &deletePdf, &clearLegacy, &preparePdf});

  expect(status == PdfDirectoryDeleteScan::Status::IterationFailure,
         "persistent iteration failure must remain result-bearing");
  expect(Storage.exists("/Books/a.epub") &&
             Storage.exists("/Books/z/book.txt") && log.deletedPdfs.empty() &&
             log.clearedLegacy.empty() && log.pdfPreparations == 0 &&
             countEventsWithPrefix("remove-dir:") == 0,
         "incomplete iteration must preserve the tree and metadata");
}

void testPersistentCloseFailureFailsClosed() {
  reset();
  Storage.putFile("/Books/book.epub", {1});
  Storage.failNextOpenOf("/Books");
  Storage.failCloseOnCall(5);
  CallbackLog log;

  const auto status = PdfDirectoryDeleteScan::deleteDirectoryNoThrow(
      "/Books", {&log, &deletePdf, &clearLegacy, &preparePdf});

  expect(status == PdfDirectoryDeleteScan::Status::CloseFailure,
         "persistent close failure must remain result-bearing");
  expect(Storage.exists("/Books/book.epub") && log.deletedPdfs.empty() &&
             log.clearedLegacy.empty() && log.pdfPreparations == 0 &&
             countEventsWithPrefix("remove-dir:") == 0,
         "incomplete close must preserve the tree and metadata");
}

void testPersistentPathFailureFailsClosed() {
  reset();
  Storage.putFile("/Books/a.epub", {1});
  Storage.mkdir("/Books/z\\bad");
  CallbackLog log;

  const auto status = PdfDirectoryDeleteScan::deleteDirectoryNoThrow(
      "/Books", {&log, &deletePdf, &clearLegacy, &preparePdf});

  expect(status == PdfDirectoryDeleteScan::Status::PathLimit,
         "persistent path failure must remain result-bearing");
  expect(Storage.exists("/Books/a.epub") && Storage.exists("/Books/z\\bad") &&
             log.deletedPdfs.empty() && log.clearedLegacy.empty() &&
             log.pdfPreparations == 0 &&
             countEventsWithPrefix("remove-dir:") == 0,
         "incomplete path discovery must preserve the tree and metadata");
}

void testIncompleteDiscoveryBeforeLaterPdfFailsClosed() {
  reset();
  Storage.putFile("/Books/a.epub", {1});
  Storage.putFile("/Books/z/book.pdf", {2});
  // Attempt one reaches the child directory but cannot close its entry.
  // Retry reaches the same point and cannot open the child, so the PDF remains
  // beyond the proven discovery prefix.
  Storage.failCloseOnCall(6);
  Storage.failNextOpenOf("/Books/z");
  CallbackLog log;

  const auto status = PdfDirectoryDeleteScan::deleteDirectoryNoThrow(
      "/Books", {&log, &deletePdf, &clearLegacy, &preparePdf});

  expect(status == PdfDirectoryDeleteScan::Status::OpenFailure,
         "incomplete discovery before a later PDF must report the scan failure");
  expect(Storage.exists("/Books/a.epub") &&
             Storage.exists("/Books/z/book.pdf") && log.deletedPdfs.empty() &&
             log.clearedLegacy.empty() && log.pdfPreparations == 0 &&
             countEventsWithPrefix("remove-dir:") == 0,
         "incomplete discovery must not bypass PDF deletion bookkeeping");
}

void testPositivePdfClassificationNeverFallsThroughAfterCloseFailure() {
  reset();
  Storage.putFile("/Books/a.epub", {1});
  Storage.putFile("/Books/book.pdf", {2});
  // Attempt one fails before discovery. Attempt two positively classifies the
  // PDF, then reports the injected close failure.
  Storage.failNextOpenOf("/Books");
  Storage.failCloseOnCall(6);
  CallbackLog log;

  const auto status = PdfDirectoryDeleteScan::deleteDirectoryNoThrow(
      "/Books", {&log, &deletePdf, &clearLegacy, &preparePdf});

  expect(status == PdfDirectoryDeleteScan::Status::CloseFailure,
         "a failure after positive PDF classification must remain result-bearing");
  expect(Storage.exists("/Books/book.pdf") && log.deletedPdfs.empty() &&
             log.clearedLegacy.empty() && log.pdfPreparations == 0 &&
             countEventsWithPrefix("remove-dir:") == 0,
         "positive PDF classification must never fall through to legacy removal");
}

void testRoutingRunsPdfRecoveryOnlyForPdfTrees() {
  reset();
  Storage.putFile("/Books/book.epub", {1});
  CallbackLog log;
  auto status = PdfDirectoryDeleteScan::deleteDirectoryNoThrow(
      "/Books", {&log, &deletePdf, &clearLegacy, &preparePdf});
  expect(status == PdfDirectoryDeleteScan::Status::Complete &&
             log.pdfPreparations == 0,
         "pure EPUB routing must not run PDF recovery");

  reset();
  Storage.putFile("/Books/book.pdf", {2});
  status = PdfDirectoryDeleteScan::deleteDirectoryNoThrow(
      "/Books", {&log, &deletePdf, &clearLegacy, &preparePdf});
  expect(status == PdfDirectoryDeleteScan::Status::Complete &&
             log.pdfPreparations == 1 &&
             log.deletedPdfs == std::vector<std::string>{"/Books/book.pdf"},
         "PDF routing must run recovery once before journaled deletion");
}

void testPositivePdfRouteFailsClosedWhenLegacySpoolCleanupFails() {
  reset();
  Storage.putFile("/Books/a.epub", {1});
  Storage.putFile("/Books/z.pdf", {2});
  Storage.failNextRemoveOf(kLegacySpoolFirstPath);
  CallbackLog log;

  const auto status = PdfDirectoryDeleteScan::deleteDirectoryNoThrow(
      "/Books", {&log, &deletePdf, &clearLegacy, &preparePdf});

  expect(status == PdfDirectoryDeleteScan::Status::SpoolCleanupFailure,
         "positive PDF routing must report legacy-spool cleanup failure");
  expect(Storage.exists("/Books/a.epub") && Storage.exists("/Books/z.pdf") &&
             log.pdfPreparations == 0U && log.deletedPdfs.empty() &&
             log.clearedLegacy.empty() &&
             countEventsWithPrefix("remove-dir:") == 0U,
         "legacy-spool cleanup failure must fail closed before recovery or deletion");
  expect(Storage.exists(kLegacySpoolFirstPath),
         "injected cleanup failure must retain its diagnostic spool artifact");
}

void testLegacyCleanupFailureAfterCommitIsDistinguishable() {
  reset();
  Storage.putFile("/Books/book.epub", {1});
  Storage.failNextRemoveOf(kLegacySpoolFirstPath);
  CallbackLog log;

  const auto status = PdfDirectoryDeleteScan::deleteDirectoryNoThrow(
      "/Books", {&log, &deletePdf, &clearLegacy, &preparePdf});

  expect(status ==
             PdfDirectoryDeleteScan::Status::CommittedWithCleanupWarning,
         "post-commit legacy spool cleanup failure must be distinguishable success");
  expect(!Storage.exists("/Books") &&
             log.clearedLegacy ==
                 std::vector<std::string>{"/Books/book.epub"} &&
             countEventsWithPrefix("remove-dir:") == 1U,
         "cleanup warning must retain the committed delete and metadata replay");
  expect(Storage.exists(kLegacySpoolFirstPath),
         "post-commit cleanup warning must leave the failed spool artifact observable");
}

void testLegacyReplayReadFailureAfterCommitIsDistinguishable() {
  reset();
  Storage.putFile("/Books/a.epub", {1});
  Storage.putFile("/Books/b.epub", {2});
  CallbackLog log;
  log.failNextLegacyReplayReadAfterFirstClear = true;

  const auto status = PdfDirectoryDeleteScan::deleteDirectoryNoThrow(
      "/Books", {&log, &deletePdf, &clearLegacy, &preparePdf});

  expect(status ==
             PdfDirectoryDeleteScan::Status::CommittedWithCleanupWarning,
         "post-commit legacy replay read failure must remain distinguishable committed success");
  expect(!Storage.exists("/Books") &&
             log.clearedLegacy ==
                 std::vector<std::string>{"/Books/a.epub"} &&
             countEventsWithPrefix("remove-dir:") == 1U,
         "replay read failure must retain the committed tree deletion and completed metadata prefix");
}

void testLegacyReplayOpenFailureAfterCommitIsDistinguishable() {
  reset();
  Storage.putFile("/Books/a.epub", {1});
  Storage.failNextOpenOfAfterRemoveDir(kLegacySpoolFirstPath);
  CallbackLog log;

  const auto status = PdfDirectoryDeleteScan::deleteDirectoryNoThrow(
      "/Books", {&log, &deletePdf, &clearLegacy, &preparePdf});

  expect(status ==
             PdfDirectoryDeleteScan::Status::CommittedWithCleanupWarning,
         "post-commit legacy replay open allocation failure must remain distinguishable committed success");
  expect(!Storage.exists("/Books") && log.clearedLegacy.empty() &&
             countEventsWithPrefix("remove-dir:") == 1U,
         "replay open failure must retain committed tree deletion without claiming metadata replay");
}

void testLegacyReplayCloseFailureAfterCommitIsDistinguishable() {
  reset();
  Storage.putFile("/Books/a.epub", {1});
  Storage.failNextCloseOfAfterRemoveDir(kLegacySpoolFirstPath);
  CallbackLog log;

  const auto status = PdfDirectoryDeleteScan::deleteDirectoryNoThrow(
      "/Books", {&log, &deletePdf, &clearLegacy, &preparePdf});

  expect(status ==
             PdfDirectoryDeleteScan::Status::CommittedWithCleanupWarning,
         "post-commit legacy replay close failure must remain distinguishable committed success");
  expect(!Storage.exists("/Books") &&
             log.clearedLegacy ==
                 std::vector<std::string>{"/Books/a.epub"} &&
             countEventsWithPrefix("remove-dir:") == 1U,
         "replay close failure must retain committed tree deletion and completed metadata replay");
}

void testLegacyMetadataCallbackFailuresWarnAndContinueAfterCommit() {
  const std::vector<std::string> expectedPaths{
      "/Books/a.epub", "/Books/b.epub", "/Books/c.epub"};
  for (size_t failedAt = 1; failedAt <= expectedPaths.size(); ++failedAt) {
    reset();
    Storage.putFile(expectedPaths[0], {1});
    Storage.putFile(expectedPaths[1], {2});
    Storage.putFile(expectedPaths[2], {3});
    CallbackLog log;
    log.failLegacyAt = failedAt;

    const auto status = PdfDirectoryDeleteScan::deleteDirectoryNoThrow(
        "/Books", {&log, &deletePdf, &clearLegacy, &preparePdf});

    expect(status ==
               PdfDirectoryDeleteScan::Status::CommittedWithCleanupWarning,
           "first, middle, and last legacy metadata callback failure must report committed warning");
    expect(!Storage.exists("/Books") &&
               log.clearedLegacy == expectedPaths &&
               countEventsWithPrefix("remove-dir:") == 1U,
           "legacy metadata callback failure must preserve commit and continue every later callback");
  }
}

void testPdfPresenceClassifierIsReadOnlyAndDoesNotCreateSpools() {
  reset();
  Storage.putFile("/Books/nested/book.epub", {1});
  bool containsPdf = true;

  auto status = PdfDirectoryDeleteScan::containsPdfNoThrow("/Books", &containsPdf);

  expect(status == PdfDirectoryDeleteScan::Status::Complete && !containsPdf,
         "pure EPUB classification must report no PDF");
  expect(Storage.exists("/Books/nested/book.epub") &&
             Storage.pathOpenCount(PdfDirectoryDeleteScan::kSpoolTempPath) == 0 &&
             Storage.pathOpenCount(PdfDirectoryDeleteScan::kSpoolSealedPath) == 0 &&
             countEventsWithPrefix("remove-dir:") == 0,
         "pure EPUB classification must be read-only and spool-free");

  reset();
  Storage.putFile("/Books/nested/book.pdf", {2});
  containsPdf = false;
  status = PdfDirectoryDeleteScan::containsPdfNoThrow("/Books", &containsPdf);

  expect(status == PdfDirectoryDeleteScan::Status::Complete && containsPdf,
         "nested PDF classification must select the strict route");
  expect(Storage.exists("/Books/nested/book.pdf") &&
             Storage.pathOpenCount(PdfDirectoryDeleteScan::kSpoolTempPath) == 0 &&
             Storage.pathOpenCount(PdfDirectoryDeleteScan::kSpoolSealedPath) == 0 &&
             countEventsWithPrefix("remove-dir:") == 0,
         "nested PDF classification must be read-only and spool-free");
}

}  // namespace

int main() {
  testWideAndDeepPureEpubUsesExactLegacyDeletePathWithoutPdfWorkspace();
  testPureEpubIgnoresStalePdfSpoolCleanupFailure();
  testPureLegacyPathDoesNotImposePdfPathOrNameCaps();
  testTruncatedLongPdfNameStillRoutesToRecoveredJournalDelete();
  testPdfSuffixedDirectoryRemainsOnLegacyRoute();
  testLongLegacyNamesReuseOneFallibleNameBuffer();
  testFlatLegacyMetadataUsesNoPerBookScannerAllocations();
  testLegacyReplayPassesAViewWithoutPostDeleteScannerAllocation();
  testLegacyReplayOomFailsBeforeDirectoryMutation();
  testPersistentLongNameAllocationFailureFailsClosed();
  testPublicRouteRetriesOneShotTraversalFailuresReadOnly();
  testIncompleteLegacyDiscoveryPreservesTreeAndMetadata();
  testPersistentIterationFailureFailsClosed();
  testPersistentCloseFailureFailsClosed();
  testPersistentPathFailureFailsClosed();
  testIncompleteDiscoveryBeforeLaterPdfFailsClosed();
  testPositivePdfClassificationNeverFallsThroughAfterCloseFailure();
  testRoutingRunsPdfRecoveryOnlyForPdfTrees();
  testPositivePdfRouteFailsClosedWhenLegacySpoolCleanupFails();
  testLegacyCleanupFailureAfterCommitIsDistinguishable();
  testLegacyReplayReadFailureAfterCommitIsDistinguishable();
  testLegacyReplayOpenFailureAfterCommitIsDistinguishable();
  testLegacyReplayCloseFailureAfterCommitIsDistinguishable();
  testLegacyMetadataCallbackFailuresWarnAndContinueAfterCommit();
  testPdfPresenceClassifierIsReadOnlyAndDoesNotCreateSpools();
  if (failures != 0) return 1;
  std::cout << "PDF_DIRECTORY_DELETE_LEGACY_PATH_PASS"
            << " iterations=" << pureDirectoryIterations
            << " explicit_heap_bytes=" << pureExplicitHeapBytes
            << " spool_written_bytes=" << pureSpoolWrittenBytes
            << " spool_read_bytes=" << pureSpoolReadBytes << '\n';
  return 0;
}
