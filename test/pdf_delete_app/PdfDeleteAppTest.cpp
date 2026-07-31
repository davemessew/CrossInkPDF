#include <iostream>
#include <string>
#include <vector>

#include <HalStorage.h>
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

}  // namespace

int main() {
  testCompleteDeletePurgesAllPdfState();
  testDurableRecentFailureRecoversAfterReboot();
  testCanonicalRecentLoadFailuresPreserveExactBytesAndUnrelatedEntries();
  testEveryAdapterPhaseIsReplayable();
  testMoveJournalArbitrationFailsClosed();
  testDeleteFenceIsPathAwareAndFailClosed();
  testInvalidAndNonPdfPathsDoNothing();
  testPreexistingTombstoneIsNeverAdopted();
  if (failures != 0) {
    std::cerr << failures << " PDF deletion adapter test(s) failed\n";
    return 1;
  }
  std::cout << "PDF deletion adapter tests passed\n";
  return 0;
}
