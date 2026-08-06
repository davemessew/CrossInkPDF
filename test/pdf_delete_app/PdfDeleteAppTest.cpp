#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <HalStorage.h>
#include <Memory.h>
#include <PdfSourceIdentity.h>

#include "BookmarkStore.h"
#include "BookMoveUtils.h"
#include "ClippingStore.h"
#include "PdfDeleteJournal.h"
#include "../../src/util/PdfDeleteUtils.h"
#include "RecentBooksStore.h"

namespace {

constexpr char kSource[] = "/Books/delete-me.pdf";
constexpr char kUnrelated[] = "/Books/keep-me.pdf";

int failures = 0;

void expect(const bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

std::string cachePath(const std::string& source) {
  char path[128]{};
  const PdfStatus formatted =
      pdfFormatCacheRootForHash("/.crosspoint", pdfPathHash64(source.data(), source.size()), path, sizeof(path));
  return formatted ? path : "";
}

void resetState() {
  Storage.reset();
  Storage.mkdir("/Books");
  Storage.mkdir("/.crosspoint");
  Storage.putFile(kSource, {1, 2, 3, 4});
  Storage.putFile(kUnrelated, {9, 8, 7});
  Storage.putFile(cachePath(kSource) + "/progress.a", {1});
  Storage.putFile(cachePath(kSource) + "/reader_settings.bin", {2});
  Storage.putFile(cachePath(kSource) + "/stats_v5.bin", {3});
  Storage.putFile(cachePath(kSource) + "/saved_items.a", {4});
  Storage.putFile(cachePath(kUnrelated) + "/progress.a", {5});
  BookmarkStore::books = {kSource, kUnrelated};
  BookmarkStore::events.clear();
  BookmarkStore::failNextDelete = false;
  ClippingStore::books = {kSource, kUnrelated};
  ClippingStore::events.clear();
  ClippingStore::failNextDelete = false;
  RecentBooksStore::paths = {kSource, kUnrelated};
  RecentBooksStore::persistedPaths = RecentBooksStore::paths;
  RecentBooksStore::events.clear();
  RecentBooksStore::failNextDurableRemoval = false;
  RecentBooksStore::canonicalExists = true;
  RecentBooksStore::failNextCanonicalRead = false;
  RecentBooksStore::failNextCanonicalParse = false;
  RecentBooksStore::canonicalBytes =
      R"({"books":[{"path":"/Books/delete-me.pdf"},{"path":"/Books/keep-me.pdf"}]})";
  RecentBooksStore::exerciseCheckedScratchAllocation = false;
  BookMoveUtils::testFence = BookMutationFence::Clear;
  BookMoveUtils::fenceQueries = 0;
  PdfDeleteUtils::resetPresenceForTest();
}

void expectCompletelyDeleted() {
  expect(!Storage.exists(kSource), "source must be removed");
  expect(!Storage.exists("/Books/.delete-me.pdf.crossink-delete"), "hidden source must be removed");
  expect(!Storage.exists(cachePath(kSource).c_str()), "entire PDF cache root must be removed");
  expect(!BookmarkStore::books.contains(kSource), "PDF bookmarks must be removed");
  expect(!ClippingStore::books.contains(kSource), "PDF clippings must be removed");
  expect(!RecentBooksStore::paths.contains(kSource), "PDF recents entry must be removed in memory");
  expect(!RecentBooksStore::persistedPaths.contains(kSource), "PDF recents entry must be removed durably");
  expect(!Storage.exists(PdfDelete::kSlotAPath) && !Storage.exists(PdfDelete::kSlotBPath),
         "completed deletion must clear both journal slots");

  expect(Storage.exists(kUnrelated), "unrelated source must remain");
  expect(Storage.exists(cachePath(kUnrelated).c_str()), "unrelated cache must remain");
  expect(BookmarkStore::books.contains(kUnrelated), "unrelated bookmarks must remain");
  expect(ClippingStore::books.contains(kUnrelated), "unrelated clippings must remain");
  expect(RecentBooksStore::paths.contains(kUnrelated), "unrelated recent entry must remain");
}

void testEmptyBootRecoverySkipsFullDeleteWorkspace() {
  resetState();
  TestMemory::reset();

  expect(PdfDeleteUtils::recoverPendingPdfDelete() ==
             PdfDeleteUtils::Result::NoPendingDelete,
         "empty boot storage must report no pending PDF delete");
  expect(TestMemory::allocations == 1U,
         "empty boot recovery must allocate only one bounded journal reader");
  expect(TestMemory::allocationSizes.size() == 1U &&
             TestMemory::allocationSizes.front() <= 7U * 1024U,
         "empty boot recovery must not allocate the full PDF delete workspace");
  expect(Storage.fileOpenCalls() == 0U && Storage.maximumFileHandles() == 0U,
         "absent PDF delete slots must not acquire a journal file handle");
  if (!TestMemory::allocationSizes.empty()) {
    std::cout << "PDF_DELETE_BOOT_PREFLIGHT allocation_bytes="
              << TestMemory::allocationSizes.front() << " opens="
              << Storage.fileOpenCalls() << '\n';
  }
}

void testCompleteDeletePurgesAllPdfState() {
  resetState();
  expect(PdfDeleteUtils::deletePdfBook(kSource) == PdfDeleteUtils::Result::Complete,
         "valid PDF deletion must complete");
  expectCompletelyDeleted();
  expect(Storage.maximumFileHandles() <= 1, "adapter and journal must retain at most one file handle");
  expect(BookmarkStore::events == std::vector<std::string>({"bookmarks:/Books/delete-me.pdf:pdf"}),
         "bookmark deletion must receive the exact source identity once");
  expect(ClippingStore::events == std::vector<std::string>({"clippings:/Books/delete-me.pdf:pdf"}),
         "clipping deletion must receive the exact source identity once");
  expect(RecentBooksStore::events == std::vector<std::string>({"recents:/Books/delete-me.pdf"}),
         "recents deletion must receive the exact source identity once");
}

void testDurableRecentFailureRecoversAfterReboot() {
  resetState();
  RecentBooksStore::failNextDurableRemoval = true;
  expect(PdfDeleteUtils::deletePdfBook(kSource) == PdfDeleteUtils::Result::Pending,
         "durable recents failure must retain a pending deletion");
  expect(RecentBooksStore::paths.contains(kSource) && RecentBooksStore::persistedPaths.contains(kSource),
         "failed recents persistence must roll the in-memory view back");
  expect(Storage.exists(PdfDelete::kSlotAPath) || Storage.exists(PdfDelete::kSlotBPath),
         "failed recents phase must retain its durable journal");

  PdfDeleteUtils::resetPresenceForTest();
  expect(PdfDeleteUtils::recoverPendingPdfDelete() == PdfDeleteUtils::Result::Complete,
         "boot recovery must retry and finish durable recent removal");
  expectCompletelyDeleted();
}

void testCanonicalRecentLoadFailuresPreserveExactBytesAndUnrelatedEntries() {
  for (const bool parseFailure : {false, true}) {
    resetState();
    const std::string canonicalBefore = RecentBooksStore::canonicalBytes;
    const auto persistedBefore = RecentBooksStore::persistedPaths;
    RecentBooksStore::paths.clear();
    RecentBooksStore::failNextCanonicalRead = !parseFailure;
    RecentBooksStore::failNextCanonicalParse = parseFailure;

    expect(PdfDeleteUtils::deletePdfBook(kSource) == PdfDeleteUtils::Result::Pending,
           parseFailure ? "corrupt canonical recents must fail deletion closed"
                        : "transient canonical recents read failure must fail deletion closed");
    expect(RecentBooksStore::canonicalBytes == canonicalBefore,
           "failed authoritative recents load must preserve canonical bytes exactly");
    expect(RecentBooksStore::persistedPaths == persistedBefore &&
               RecentBooksStore::persistedPaths.contains(kUnrelated),
           "failed authoritative recents load must preserve every persisted unrelated entry");

    PdfDeleteUtils::resetPresenceForTest();
    expect(PdfDeleteUtils::recoverPendingPdfDelete() == PdfDeleteUtils::Result::Complete,
           "reboot retry must reload canonical recents before replaying deletion");
    expectCompletelyDeleted();
  }
}

void testEveryAdapterPhaseIsReplayable() {
  for (int fault = 0; fault < 6; ++fault) {
    resetState();
    switch (fault) {
      case 0:
        Storage.failNextRename();
        break;
      case 1:
        Storage.failNextRemoveDir();
        break;
      case 2:
        BookmarkStore::failNextDelete = true;
        break;
      case 3:
        ClippingStore::failNextDelete = true;
        break;
      case 4:
        RecentBooksStore::failNextDurableRemoval = true;
        break;
      case 5:
        Storage.failNextRemoveOf("/Books/.delete-me.pdf.crossink-delete");
        break;
    }
    expect(PdfDeleteUtils::deletePdfBook(kSource) == PdfDeleteUtils::Result::Pending,
           "injected phase failure must remain pending");
    PdfDeleteUtils::resetPresenceForTest();
    expect(PdfDeleteUtils::recoverPendingPdfDelete() == PdfDeleteUtils::Result::Complete,
           "reboot recovery must finish every replayable phase");
    expectCompletelyDeleted();
  }
}

void testMoveJournalArbitrationFailsClosed() {
  for (const BookMutationFence fence :
       {BookMutationFence::MatchingPending, BookMutationFence::UnrelatedPending, BookMutationFence::Indeterminate}) {
    resetState();
    BookMoveUtils::testFence = fence;
    const PdfDeleteUtils::Result expected =
        fence == BookMutationFence::Indeterminate ? PdfDeleteUtils::Result::Pending
                                                  : PdfDeleteUtils::Result::Conflict;
    expect(PdfDeleteUtils::deletePdfBook(kSource) == expected,
           "active move journal must serialize deletion before journal mutation");
    expect(Storage.exists(kSource), "move conflict must preserve the PDF source");
    expect(!Storage.exists(PdfDelete::kSlotAPath) && !Storage.exists(PdfDelete::kSlotBPath),
           "move conflict must not prepare a delete journal");
  }
}

void testDeleteFenceIsPathAwareAndFailClosed() {
  resetState();
  RecentBooksStore::failNextDurableRemoval = true;
  expect(PdfDeleteUtils::deletePdfBook(kSource) == PdfDeleteUtils::Result::Pending,
         "fixture must leave a delete journal");
  expect(PdfDeleteUtils::mutationFenceForPath(kSource) == BookMutationFence::MatchingPending,
         "matching path must report the active delete");
  expect(PdfDeleteUtils::mutationFenceForPath(kUnrelated) == BookMutationFence::UnrelatedPending,
         "unrelated path must be distinguished even though shared recents requires serialization");

  Storage.failAllReads();
  PdfDeleteUtils::resetPresenceForTest();
  expect(PdfDeleteUtils::mutationFenceForPath(kUnrelated) == BookMutationFence::Indeterminate,
         "unreadable delete journal must fail closed");
}

void testInvalidAndNonPdfPathsDoNothing() {
  resetState();
  expect(PdfDeleteUtils::deletePdfBook("/Books/not-pdf.epub") == PdfDeleteUtils::Result::Unsupported,
         "non-PDF must never enter the deletion journal");
  expect(PdfDeleteUtils::deletePdfBook("/Books/../delete-me.pdf") == PdfDeleteUtils::Result::Invalid,
         "non-canonical source path must be rejected");
  expect(PdfDeleteUtils::deletePdfBook("") == PdfDeleteUtils::Result::Invalid,
         "empty source path must be rejected");
  expect(Storage.exists(kSource), "rejected paths must preserve the source");
  expect(!Storage.exists(PdfDelete::kSlotAPath) && !Storage.exists(PdfDelete::kSlotBPath),
         "rejected paths must not create delete intent");
}

void testPreexistingTombstoneIsNeverAdopted() {
  resetState();
  Storage.putFile("/Books/.delete-me.pdf.crossink-delete", {7});
  expect(PdfDeleteUtils::deletePdfBook(kSource) == PdfDeleteUtils::Result::Invalid,
         "preexisting tombstone must fail before durable intent");
  expect(Storage.exists(kSource) && Storage.exists("/Books/.delete-me.pdf.crossink-delete"),
         "tombstone collision must preserve both files");
  expect(!Storage.exists(PdfDelete::kSlotAPath) && !Storage.exists(PdfDelete::kSlotBPath),
         "tombstone collision must not create delete intent");
}

void testDirectorySessionAllocationFailsBeforeMutation() {
  resetState();
  TestMemory::reset();
  TestMemory::failNextAllocation = true;

  auto session = PdfDeleteUtils::makeDirectoryDeleteSessionNoThrow();

  expect(!session, "directory delete session allocation must be fallible");
  expect(Storage.exists(kSource),
         "directory session OOM must happen before source mutation");
  expect(!Storage.exists(PdfDelete::kSlotAPath) &&
             !Storage.exists(PdfDelete::kSlotBPath),
         "directory session OOM must not create durable delete intent");
}

void testPreGateOomDoesNotReleaseAnotherDeleteOwner() {
  resetState();
  PdfDeleteUtils::markDeleteStartingForTest();
  TestMemory::reset();
  TestMemory::failNextAllocation = true;

  expect(PdfDeleteUtils::deletePdfBook(kUnrelated) ==
             PdfDeleteUtils::Result::Pending,
         "pre-gate workspace OOM must report pending while another delete owns the gate");

  TestMemory::reset();
  expect(PdfDeleteUtils::mutationFenceForPath(kUnrelated) ==
             BookMutationFence::Indeterminate,
         "pre-gate OOM must not release another task's active delete gate");
  expect(TestMemory::allocations == 0U,
         "an active delete gate probe must not fall through to journal lookup");
}

void testDirectorySessionUsesExactViewAndReusesOneWorkspaceFor300Pdfs() {
  Storage.reset();
  Storage.mkdir("/Books");
  Storage.mkdir("/.crosspoint");
  BookmarkStore::books.clear();
  BookmarkStore::events.clear();
  ClippingStore::books.clear();
  ClippingStore::events.clear();
  RecentBooksStore::paths.clear();
  RecentBooksStore::persistedPaths.clear();
  RecentBooksStore::events.clear();
  RecentBooksStore::canonicalExists = true;
  RecentBooksStore::failNextDurableRemoval = false;
  RecentBooksStore::exerciseCheckedScratchAllocation = false;
  BookMoveUtils::testFence = BookMutationFence::Clear;
  BookMoveUtils::fenceQueries = 0;
  PdfDeleteUtils::resetPresenceForTest();
  TestMemory::reset();

  auto session = PdfDeleteUtils::makeDirectoryDeleteSessionNoThrow();
  expect(session != nullptr && TestMemory::allocations == 1U,
         "directory replay must prepare one checked reusable adapter workspace");
  if (!session) return;

  constexpr size_t kPdfCount = 300U;
  for (size_t index = 0; index < kPdfCount; ++index) {
    const std::string source =
        "/Books/directory-" + std::to_string(1000U + index) + ".pdf";
    Storage.putFile(source, {1});
    BookmarkStore::books.insert(source);
    ClippingStore::books.insert(source);
    RecentBooksStore::paths.insert(source);
    RecentBooksStore::persistedPaths.insert(source);
    RecentBooksStore::canonicalBytes = "canonical-before-delete";

    const std::string padded = source + ".trailing";
    const auto result = PdfDeleteUtils::deletePdfBookNoPathAlloc(
        *session, std::string_view(padded.data(), source.size()));
    expect(result == PdfDeleteUtils::Result::Complete,
           "every directory PDF must complete through the reusable view adapter");
    expect(!Storage.exists(source.c_str()),
           "the exact sliced PDF source must be removed");
    expect(Storage.exists(padded.c_str()) == false,
           "the trailing sentinel must never be treated as part of the source");
  }

  expect(TestMemory::allocations == 1U,
         "300 directory PDFs must not allocate another DeleteWorkspace");
  expect(Storage.maximumFileHandles() <= 1U,
         "300 directory PDFs must retain at most one SD handle");
  expect(Storage.fileOpenCalls() > 0U &&
             Storage.fileOpenCalls() <= kPdfCount * 40U,
         "journal open work must remain linearly bounded per PDF");
  expect(Storage.totalReadBytes() <= kPdfCount * 8192U &&
             Storage.totalWrittenBytes() <= kPdfCount * 8192U,
         "journal I/O must remain bounded per PDF");
  std::cout << "PDF_DIRECTORY_ADAPTER_300 allocations="
            << TestMemory::allocations << " opens=" << Storage.fileOpenCalls()
            << " read_bytes=" << Storage.totalReadBytes()
            << " written_bytes=" << Storage.totalWrittenBytes() << '\n';
}

void testDirectorySessionDownstreamOomPreservesCommittedPrefixAndPendingPdf() {
  resetState();
  TestMemory::reset();
  auto session = PdfDeleteUtils::makeDirectoryDeleteSessionNoThrow();
  expect(session != nullptr, "directory workspace fixture must allocate");
  if (!session) return;

  expect(PdfDeleteUtils::deletePdfBookNoPathAlloc(*session, kSource) ==
             PdfDeleteUtils::Result::Complete,
         "first directory PDF must commit before the later OOM");
  expect(!Storage.exists(kSource),
         "the committed directory prefix must remain deleted");

  constexpr char kSecond[] = "/Books/second.pdf";
  Storage.putFile(kSecond, {5});
  BookmarkStore::books.insert(kSecond);
  ClippingStore::books.insert(kSecond);
  RecentBooksStore::paths.insert(kSecond);
  RecentBooksStore::persistedPaths.insert(kSecond);
  RecentBooksStore::exerciseCheckedScratchAllocation = true;
  TestMemory::failNextAllocation = true;

  expect(PdfDeleteUtils::deletePdfBookNoPathAlloc(*session, kSecond) ==
             PdfDeleteUtils::Result::Pending,
         "checked downstream OOM must leave the current PDF journaled pending");
  expect(!Storage.exists(kSource) && !Storage.exists(kSecond) &&
             Storage.exists("/Books/.second.pdf.crossink-delete"),
         "later OOM must preserve the committed prefix and hide only the journaled current PDF");
  expect(Storage.exists(PdfDelete::kSlotAPath) ||
             Storage.exists(PdfDelete::kSlotBPath),
         "later OOM must retain durable intent for exact recovery");

  PdfDeleteUtils::resetPresenceForTest();
  expect(PdfDeleteUtils::recoverPendingPdfDelete() ==
             PdfDeleteUtils::Result::Complete,
         "recovery must finish the precise PDF left pending by OOM");
  expect(!Storage.exists("/Books/.second.pdf.crossink-delete") &&
             !RecentBooksStore::persistedPaths.contains(kSecond),
         "OOM recovery must remove only the pending PDF state");
}

}  // namespace

int main() {
  testEmptyBootRecoverySkipsFullDeleteWorkspace();
  testCompleteDeletePurgesAllPdfState();
  testDurableRecentFailureRecoversAfterReboot();
  testCanonicalRecentLoadFailuresPreserveExactBytesAndUnrelatedEntries();
  testEveryAdapterPhaseIsReplayable();
  testMoveJournalArbitrationFailsClosed();
  testDeleteFenceIsPathAwareAndFailClosed();
  testInvalidAndNonPdfPathsDoNothing();
  testPreexistingTombstoneIsNeverAdopted();
  testDirectorySessionAllocationFailsBeforeMutation();
  testPreGateOomDoesNotReleaseAnotherDeleteOwner();
  testDirectorySessionUsesExactViewAndReusesOneWorkspaceFor300Pdfs();
  testDirectorySessionDownstreamOomPreservesCommittedPrefixAndPendingPdf();
  if (failures != 0) {
    std::cerr << failures << " PDF deletion adapter test(s) failed\n";
    return 1;
  }
  std::cout << "PDF deletion adapter tests passed\n";
  return 0;
}
