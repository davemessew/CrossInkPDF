#include <BookStateMigrationJournal.h>
#include <BookmarkStore.h>
#include <ClippingStore.h>
#include <CrossPointState.h>
#include <Epub.h>
#include <HalStorage.h>
#include <Memory.h>
#include <PdfDeleteUtils.h>
#include <PdfSourceIdentity.h>
#include <RecentBooksStore.h>

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

#include "BookMoveUtils.h"

namespace {

int failures = 0;

void expect(const bool condition, const char* message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

std::string pdfCachePath(const std::string& bookPath) {
  return "/.crosspoint/pdf_" + std::to_string(pdfPathHash64(bookPath.c_str(), bookPath.size()));
}

std::string epubCachePath(const std::string& bookPath) { return Epub::cachePathForFilePath(bookPath, "/.crosspoint"); }

std::vector<uint8_t> bytes(const size_t count, const uint8_t seed) {
  std::vector<uint8_t> result(count);
  for (size_t index = 0; index < result.size(); ++index) {
    result[index] = static_cast<uint8_t>(seed + index * 17U);
  }
  return result;
}

void resetState() {
  Storage.reset();
  BookmarkStore::stores.clear();
  BookmarkStore::events.clear();
  BookmarkStore::failCopy = false;
  ClippingStore::stores.clear();
  ClippingStore::events.clear();
  RecentBooksStore::paths.clear();
  RecentBooksStore::persistedPaths.clear();
  RecentBooksStore::events.clear();
  RecentBooksStore::dropNextPersistence = false;
  CrossPointState::events.clear();
  CrossPointState::persistedPath.clear();
  CrossPointState::dropNextPersistence = false;
  APP_STATE.openBookPath().clear();
  PdfDeleteUtils::testFence = BookMutationFence::Clear;
  PdfDeleteUtils::fenceQueries = 0;
}

void seedPdf(const std::string& oldPath) {
  Storage.mkdir("/Books");
  Storage.mkdir("/Read");
  Storage.mkdir("/.crosspoint");
  Storage.putFile(oldPath, bytes(37, 3));
  const std::string cache = pdfCachePath(oldPath);
  Storage.putFile(cache + "/manifest.bin", bytes(5003, 7));
  Storage.putFile(cache + "/sections/0001.bin", bytes(73, 11));
  Storage.putFile(cache + "/images/empty.bin", {});
  BookmarkStore::stores[oldPath] = "bookmark-state";
  ClippingStore::stores[oldPath] = "clipping-state";
  RecentBooksStore::paths.insert(oldPath);
  RecentBooksStore::persistedPaths.insert(oldPath);
  APP_STATE.openBookPath() = oldPath;
  CrossPointState::persistedPath = oldPath;
}

void seedLegacyEpub(const std::string& oldPath) {
  Storage.mkdir("/Books");
  Storage.mkdir("/Read");
  Storage.mkdir("/.crosspoint");
  Storage.putFile(oldPath, bytes(37, 13));
  Storage.putFile(epubCachePath(oldPath) + "/manifest.bin", bytes(51, 17));
  BookmarkStore::stores[oldPath] = "legacy-bookmark-state";
  ClippingStore::stores[oldPath] = "legacy-clipping-state";
  RecentBooksStore::paths.insert(oldPath);
  RecentBooksStore::persistedPaths.insert(oldPath);
  APP_STATE.openBookPath() = oldPath;
  CrossPointState::persistedPath = oldPath;
}

void testEmptyBootRecoverySkipsFullMoveWorkspace() {
  resetState();
  TestMemory::reset();

  expect(BookMoveUtils::recoverPendingBookMove() ==
             BookMoveUtils::MoveResult::NoPendingMove,
         "empty boot storage must report no pending book move");
  expect(TestMemory::allocations == 1U,
         "empty boot recovery must allocate only one bounded journal reader");
  expect(TestMemory::allocationSizes.size() == 1U &&
             TestMemory::allocationSizes.front() <= 3U * 1024U,
         "empty boot recovery must not allocate the full book-move workspace");
  expect(Storage.fileOpenCalls() == 0U && Storage.maximumFileHandles() == 0U,
         "absent book-move slots must not acquire a journal file handle");
  if (!TestMemory::allocationSizes.empty()) {
    std::cout << "BOOK_MOVE_BOOT_PREFLIGHT allocation_bytes="
              << TestMemory::allocationSizes.front() << " opens="
              << Storage.fileOpenCalls() << '\n';
  }
}

void testCompleteMoveCopiesAndVerifiesBeforeTerminalCleanup() {
  resetState();
  const std::string oldPath = "/Books/book.pdf";
  const std::string newPath = "/Read/book.pdf";
  seedPdf(oldPath);
  const std::string oldCache = pdfCachePath(oldPath);
  const std::string newCache = pdfCachePath(newPath);
  const auto manifest = Storage.bytes(oldCache + "/manifest.bin");
  const auto section = Storage.bytes(oldCache + "/sections/0001.bin");

  const BookMoveUtils::MoveResult result = BookMoveUtils::moveBook(oldPath, newPath);

  expect(result == BookMoveUtils::MoveResult::Complete, "journaled PDF move must complete");
  expect(!Storage.exists(oldPath.c_str()) && Storage.exists(newPath.c_str()),
         "source rename must leave exactly the new source");
  expect(!Storage.exists(oldCache.c_str()) && Storage.exists(newCache.c_str()),
         "old cache must be removed after new cache activation");
  expect(Storage.bytes(newCache + "/manifest.bin") == manifest, "large cache file must copy byte-exactly");
  expect(Storage.bytes(newCache + "/sections/0001.bin") == section, "nested cache file must copy byte-exactly");
  expect(Storage.bytes(newCache + "/images/empty.bin").empty(), "zero-byte cache files must survive");
  expect(!BookmarkStore::stores.contains(oldPath) && BookmarkStore::stores.at(newPath) == "bookmark-state",
         "bookmarks must be copied then old-key state removed");
  expect(!ClippingStore::stores.contains(oldPath) && ClippingStore::stores.at(newPath) == "clipping-state",
         "clippings must be copied then old-key state removed");
  expect(!RecentBooksStore::paths.contains(oldPath) && RecentBooksStore::paths.contains(newPath),
         "recent entry must activate the new path");
  expect(APP_STATE.openBookPath() == newPath, "open-book state must activate the new path");
  expect(!Storage.exists(BookStateMigration::kSlotAPath) && !Storage.exists(BookStateMigration::kSlotBPath),
         "terminal move must clean both journal slots");
  expect(Storage.maximumFileHandles() == 1, "streamed copies must hold at most one file handle");

  const auto sync = std::find(Storage.events.begin(), Storage.events.end(), "sync:/.crosspoint/book_move.a");
  const auto rename = std::find(Storage.events.begin(), Storage.events.end(), "rename:/Books/book.pdf->/Read/book.pdf");
  expect(sync != Storage.events.end() && rename != Storage.events.end() && sync < rename,
         "Prepared journal must sync before source rename");
}

void testFailureAfterSourceRenameRetainsOldStateAndRebootsIdempotently() {
  resetState();
  const std::string oldPath = "/Books/retry.pdf";
  const std::string newPath = "/Read/retry.pdf";
  seedPdf(oldPath);
  const std::string oldCache = pdfCachePath(oldPath);
  BookmarkStore::failCopy = true;

  expect(BookMoveUtils::moveBook(oldPath, newPath) == BookMoveUtils::MoveResult::Pending,
         "copy failure after rename must leave a recoverable journal");
  expect(!Storage.exists(oldPath.c_str()) && Storage.exists(newPath.c_str()),
         "new source must never be deleted after rename");
  expect(Storage.exists(oldCache.c_str()), "old cache must remain before activation");
  expect(BookmarkStore::stores.contains(oldPath), "old bookmarks must remain before activation");
  expect(ClippingStore::stores.contains(oldPath), "old clippings must remain before activation");

  const uint64_t oldHash = pdfPathHash64(oldPath.c_str(), oldPath.size());
  const uint64_t newHash = pdfPathHash64(newPath.c_str(), newPath.size());
  uint64_t resolved = 0;
  bool readOnlyFallback = false;
  TestMemory::reset();
  expect(BookMoveUtils::migrationCacheHash(newPath, newHash, &resolved, &readOnlyFallback) && resolved == oldHash,
         "pre-activation cache lookup must retain the old-hash fallback");
  expect(readOnlyFallback, "pre-activation cache fallback must be read-only");
  expect(TestMemory::allocations == 1, "pending cache lookup must allocate one bounded journal-read session");
  expect(Storage.maximumFileHandles() == 1, "journal fallback lookup must retain one-reader ordering");

  expect(BookMoveUtils::recoverPendingBookMove() == BookMoveUtils::MoveResult::Complete,
         "reboot recovery must finish the interrupted move");
  expect(Storage.exists(newPath.c_str()), "recovery must preserve the new source");
  expect(!Storage.exists(oldCache.c_str()), "recovery may clean old cache only after activation");
  resolved = 0;
  readOnlyFallback = true;
  expect(BookMoveUtils::migrationCacheHash(newPath, newHash, &resolved, &readOnlyFallback) && resolved == newHash,
         "after terminal activation the normal new cache hash must apply");
  expect(!readOnlyFallback, "cleared migration must permit the normal writable cache");
  expect(BookMoveUtils::recoverPendingBookMove() == BookMoveUtils::MoveResult::NoPendingMove,
         "repeated recovery must be idempotent");
}

void testPendingMoveFencesOnlyItsAffectedPdfCacheIdentity() {
  resetState();
  const std::string oldPathA = "/Books/pending-a.pdf";
  const std::string newPathA = "/Read/pending-a.pdf";
  const std::string pathB = "/Books/unrelated-b.pdf";
  seedPdf(oldPathA);
  Storage.putFile(pathB, bytes(41, 19));
  BookmarkStore::failCopy = true;

  expect(BookMoveUtils::moveBook(oldPathA, newPathA) == BookMoveUtils::MoveResult::Pending,
         "fixture must leave PDF A pending after its source rename");

  const uint64_t oldHashA = pdfPathHash64(oldPathA.c_str(), oldPathA.size());
  const uint64_t newHashA = pdfPathHash64(newPathA.c_str(), newPathA.size());
  uint64_t resolvedA = 0;
  bool readOnlyA = false;
  expect(BookMoveUtils::migrationCacheHash(newPathA, newHashA, &resolvedA, &readOnlyA) &&
             resolvedA == oldHashA && readOnlyA,
         "pending PDF A must resolve only to its retained read-only cache");

  const uint64_t normalHashB = pdfPathHash64(pathB.c_str(), pathB.size());
  uint64_t resolvedB = 0;
  bool readOnlyB = true;
  expect(BookMoveUtils::migrationCacheHash(pathB, normalHashB, &resolvedB, &readOnlyB) &&
             resolvedB == normalHashB && !readOnlyB,
         "unrelated existing PDF B must keep its normal writable cache while A is pending");
  expect(Storage.exists(pathB.c_str()), "unrelated PDF B must remain available for normal reader resume");
}

void testActivatedJournalUsesNewWritableHashUntilCleanupRetries() {
  resetState();
  const std::string oldPath = "/Books/activated.pdf";
  const std::string newPath = "/Read/activated.pdf";
  seedPdf(oldPath);
  Storage.failNextRemoveDir();

  expect(BookMoveUtils::moveBook(oldPath, newPath) == BookMoveUtils::MoveResult::Pending,
         "old-state cleanup failure must leave the activated journal retryable");
  const uint64_t newHash = pdfPathHash64(newPath.c_str(), newPath.size());
  uint64_t resolved = 0;
  bool readOnlyFallback = true;
  expect(BookMoveUtils::migrationCacheHash(newPath, newHash, &resolved, &readOnlyFallback) && resolved == newHash,
         "activated migration must select the new cache hash");
  expect(!readOnlyFallback, "activated migration cache must no longer use the read-only fallback");
  expect(BookMoveUtils::recoverPendingBookMove() == BookMoveUtils::MoveResult::Complete,
         "activated cleanup must remain retryable");
}

void testResolverFailureFencesFallbackAndPreservesNormalHashOutput() {
  resetState();
  const std::string oldPath = "/Books/read-fault.pdf";
  const std::string newPath = "/Read/read-fault.pdf";
  seedPdf(oldPath);
  BookmarkStore::failCopy = true;
  expect(BookMoveUtils::moveBook(oldPath, newPath) == BookMoveUtils::MoveResult::Pending,
         "fixture must leave a pre-activation journal");

  Storage.failAllReads();
  const uint64_t newHash = pdfPathHash64(newPath.c_str(), newPath.size());
  uint64_t resolved = 0;
  bool readOnlyFallback = false;
  expect(!BookMoveUtils::migrationCacheHash(newPath, newHash, &resolved, &readOnlyFallback),
         "journal read failure must fail closed");
  expect(resolved == newHash, "failed lookup must leave the caller's normal hash intact");
  expect(readOnlyFallback, "failed lookup must request a conservative read-only fence");
}

void testKnownAbsentJournalNeedsNoLookupAllocation() {
  resetState();
  expect(BookMoveUtils::recoverPendingBookMove() == BookMoveUtils::MoveResult::NoPendingMove,
         "empty storage must establish the no-journal state");
  TestMemory::reset();
  const std::string path = "/Books/no-journal.pdf";
  const uint64_t normalHash = pdfPathHash64(path.c_str(), path.size());
  uint64_t resolved = 0;
  bool readOnlyFallback = true;

  expect(BookMoveUtils::migrationCacheHash(path, normalHash, &resolved, &readOnlyFallback),
         "known-absent journal lookup must succeed");
  expect(resolved == normalHash && !readOnlyFallback, "known-absent lookup must retain normal writable hash");
  expect(TestMemory::allocations == 0, "known-absent lookup must allocate nothing");
}

void testFirstEmptyResolverLookupPublishesNoJournalFastPath() {
  resetState();
  const std::string oldPath = "/Books/empty-resolver-old.pdf";
  const std::string newPath = "/Read/empty-resolver-new.pdf";
  seedPdf(oldPath);
  BookmarkStore::failCopy = true;
  expect(BookMoveUtils::moveBook(oldPath, newPath) == BookMoveUtils::MoveResult::Pending,
         "fixture must establish lookup-required migration state");
  Storage.reset();
  TestMemory::reset();

  const std::string path = "/Books/no-journal-after-lookup.pdf";
  const uint64_t normalHash = pdfPathHash64(path.c_str(), path.size());
  uint64_t resolved = 0;
  bool readOnlyFallback = true;
  expect(BookMoveUtils::migrationCacheHash(path, normalHash, &resolved, &readOnlyFallback),
         "first empty resolver lookup must retain the normal cache");
  expect(TestMemory::allocations == 1, "first empty resolver lookup must use one bounded journal session");
  expect(BookMoveUtils::migrationCacheHash(path, normalHash, &resolved, &readOnlyFallback),
         "published empty resolver lookup must remain reusable");
  expect(TestMemory::allocations == 1, "published no-journal lookup must avoid every later allocation");
}

void testRenameFailureLeavesOldSourceAndStateUntouched() {
  resetState();
  const std::string oldPath = "/Books/fail.pdf";
  const std::string newPath = "/Read/fail.pdf";
  seedPdf(oldPath);
  Storage.failNextRename();

  expect(BookMoveUtils::moveBook(oldPath, newPath) == BookMoveUtils::MoveResult::Pending,
         "definitive source rename failure must report a pending terminal cleanup");
  expect(Storage.exists(oldPath.c_str()) && !Storage.exists(newPath.c_str()),
         "rename failure must preserve the old source");
  expect(BookmarkStore::stores.contains(oldPath) && !BookmarkStore::stores.contains(newPath),
         "rename failure must not copy or delete bookmark state");
  expect(ClippingStore::stores.contains(oldPath) && !ClippingStore::stores.contains(newPath),
         "rename failure must not copy or delete clipping state");
  expect(BookMoveUtils::recoverPendingBookMove() == BookMoveUtils::MoveResult::Abandoned,
         "reboot must idempotently clean an abandoned move");
  expect(Storage.exists(oldPath.c_str()), "abandoned cleanup must not delete the old source");
}

void testRealStateHashCollisionFailsBeforeJournalMutation() {
  resetState();
  const std::string oldPath = "/books/URYGSiQsIopJ.pdf";
  const std::string newPath = "/books/t5SWIj04yLzY.pdf";
  Storage.mkdir("/books");
  Storage.mkdir("/.crosspoint");
  Storage.putFile(oldPath, bytes(10, 1));

  expect(BookMoveUtils::moveBook(oldPath, newPath) == BookMoveUtils::MoveResult::Conflict,
         "real CRC32 path-key collision must fail closed");
  expect(Storage.exists(oldPath.c_str()) && !Storage.exists(newPath.c_str()),
         "collision rejection must precede source rename");
  expect(!Storage.exists(BookStateMigration::kSlotAPath) && !Storage.exists(BookStateMigration::kSlotBPath),
         "collision rejection must not create a journal");
}

void testReadFolderDestinationKeepsExistingSuffixBehavior() {
  resetState();
  Storage.mkdir("/Read");
  Storage.putFile("/Read/book.epub", bytes(1, 1));
  Storage.putFile("/Read/book (2).epub", bytes(1, 2));

  expect(BookMoveUtils::buildReadFolderDestination("/Books/book.epub") == "/Read/book (3).epub",
         "read-folder destination must retain the first available numeric suffix");
  expect(BookMoveUtils::buildReadFolderDestination("/Books/unique.pdf") == "/Read/unique.pdf",
         "read-folder destination must retain an unused filename");
}

void testNonPdfMovesNeverEnterTheJournaledPath() {
  for (const std::string extension : {".epub", ".txt", ".xtc"}) {
    resetState();
    const std::string oldPath = "/Books/legacy" + extension;
    const std::string newPath = "/Read/legacy" + extension;
    Storage.mkdir("/Books");
    Storage.mkdir("/Read");
    Storage.mkdir("/.crosspoint");
    Storage.putFile(oldPath, bytes(9, 4));

    expect(BookMoveUtils::moveBook(oldPath, newPath) == BookMoveUtils::MoveResult::Unsupported,
           "non-PDF move must reject the PDF journal path");
    expect(Storage.exists(oldPath.c_str()) && !Storage.exists(newPath.c_str()),
           "non-PDF journal rejection must not rename the source");
    expect(!Storage.exists(BookStateMigration::kSlotAPath) && !Storage.exists(BookStateMigration::kSlotBPath),
           "non-PDF journal rejection must not touch journal slots");
  }
}

void testLegacyEpubMigrationRetainsPrePdfBehavior() {
  resetState();
  const std::string oldPath = "/Books/legacy.epub";
  const std::string newPath = "/Read/legacy.epub";
  seedLegacyEpub(oldPath);
  const std::string oldCache = epubCachePath(oldPath);
  const std::string newCache = epubCachePath(newPath);

  expect(Storage.rename(oldPath.c_str(), newPath.c_str()), "legacy caller must rename EPUB before state migration");
  expect(BookMoveUtils::migrateMovedEpubState(oldPath, newPath, oldCache, "Legacy title", "Legacy author", true),
         "legacy EPUB state migration must retain its result contract");
  expect(!Storage.exists(oldCache.c_str()) && Storage.exists(newCache.c_str()),
         "legacy EPUB cache must retain rename semantics");
  expect(!BookmarkStore::stores.contains(oldPath) && BookmarkStore::stores.contains(newPath),
         "legacy EPUB bookmarks must retain migrate-after-rename semantics");
  expect(!ClippingStore::stores.contains(oldPath) && ClippingStore::stores.contains(newPath),
         "legacy EPUB clippings must retain migrate-after-rename semantics");
  expect(!RecentBooksStore::paths.contains(oldPath) && RecentBooksStore::paths.contains(newPath),
         "legacy EPUB recents must retain update-path semantics");
  expect(APP_STATE.openBookPath() == newPath, "legacy EPUB open path must retain save-after-update semantics");
  expect(!Storage.exists(BookStateMigration::kSlotAPath) && !Storage.exists(BookStateMigration::kSlotBPath),
         "legacy EPUB state migration must never create a PDF move journal");
}

void testLegacyEpubRemoveFromRecentsSemanticsRemainUnchanged() {
  resetState();
  const std::string oldPath = "/Books/legacy-remove.epub";
  const std::string newPath = "/Read/legacy-remove.epub";
  seedLegacyEpub(oldPath);
  RecentBooksStore::paths.insert(newPath);
  RecentBooksStore::persistedPaths = RecentBooksStore::paths;

  expect(Storage.rename(oldPath.c_str(), newPath.c_str()), "legacy remove-policy caller must rename EPUB first");
  expect(BookMoveUtils::migrateMovedEpubState(oldPath, newPath, epubCachePath(oldPath), "Legacy title", "Legacy author",
                                              false),
         "legacy remove-policy state migration must retain its result contract");
  expect(!RecentBooksStore::paths.contains(oldPath) && !RecentBooksStore::paths.contains(newPath),
         "legacy remove policy must erase both old and new recent entries");
  expect(!Storage.exists(BookStateMigration::kSlotAPath) && !Storage.exists(BookStateMigration::kSlotBPath),
         "legacy remove policy must remain outside the PDF journal");
}

void testDroppedRecentPersistenceCannotDeleteOldState() {
  resetState();
  const std::string oldPath = "/Books/recent-power-loss.pdf";
  const std::string newPath = "/Read/recent-power-loss.pdf";
  seedPdf(oldPath);
  const std::string oldCache = pdfCachePath(oldPath);
  RecentBooksStore::dropNextPersistence = true;

  expect(BookMoveUtils::moveBook(oldPath, newPath) == BookMoveUtils::MoveResult::Pending,
         "RAM-only recent activation must not advance the durable journal");
  expect(Storage.exists(oldCache.c_str()) && BookmarkStore::stores.contains(oldPath) &&
             ClippingStore::stores.contains(oldPath),
         "old state must remain until recent activation reloads from storage");

  RecentBooksStore::paths = RecentBooksStore::persistedPaths;
  APP_STATE.openBookPath() = CrossPointState::persistedPath;
  expect(BookMoveUtils::recoverPendingBookMove() == BookMoveUtils::MoveResult::Complete,
         "reboot retry must durably activate recents before cleanup");
}

void testDroppedOpenPathPersistenceCannotDeleteOldState() {
  resetState();
  const std::string oldPath = "/Books/open-power-loss.pdf";
  const std::string newPath = "/Read/open-power-loss.pdf";
  seedPdf(oldPath);
  const std::string oldCache = pdfCachePath(oldPath);
  CrossPointState::dropNextPersistence = true;

  expect(BookMoveUtils::moveBook(oldPath, newPath) == BookMoveUtils::MoveResult::Pending,
         "RAM-only open-path activation must not advance the durable journal");
  expect(Storage.exists(oldCache.c_str()) && BookmarkStore::stores.contains(oldPath) &&
             ClippingStore::stores.contains(oldPath),
         "old state must remain until open path reloads from storage");

  RecentBooksStore::paths = RecentBooksStore::persistedPaths;
  APP_STATE.openBookPath() = CrossPointState::persistedPath;
  expect(BookMoveUtils::recoverPendingBookMove() == BookMoveUtils::MoveResult::Complete,
         "reboot retry must durably activate the open path before cleanup");
}

void testRemoveFromRecentsPolicySurvivesReboot() {
  resetState();
  const std::string oldPath = "/Books/remove-retry.pdf";
  const std::string newPath = "/Read/remove-retry.pdf";
  seedPdf(oldPath);
  BookmarkStore::failCopy = true;

  expect(BookMoveUtils::moveBook(oldPath, newPath, false) == BookMoveUtils::MoveResult::Pending,
         "interrupted remove-policy move must remain recoverable");
  RecentBooksStore::paths = RecentBooksStore::persistedPaths;
  APP_STATE.openBookPath() = CrossPointState::persistedPath;
  expect(BookMoveUtils::recoverPendingBookMove() == BookMoveUtils::MoveResult::Complete,
         "reboot must resume the remove-policy move");
  expect(!RecentBooksStore::paths.contains(oldPath) && !RecentBooksStore::paths.contains(newPath) &&
             !RecentBooksStore::persistedPaths.contains(oldPath) && !RecentBooksStore::persistedPaths.contains(newPath),
         "remove policy must durably erase both old and new recent paths");
}

void testPendingDeleteSerializesPdfMoveBeforeJournalMutation() {
  for (const BookMutationFence fence :
       {BookMutationFence::MatchingPending, BookMutationFence::UnrelatedPending, BookMutationFence::Indeterminate}) {
    resetState();
    const std::string oldPath = "/Books/delete-fenced.pdf";
    const std::string newPath = "/Read/delete-fenced.pdf";
    seedPdf(oldPath);
    PdfDeleteUtils::testFence = fence;

    const BookMoveUtils::MoveResult result = BookMoveUtils::moveBook(oldPath, newPath);
    const BookMoveUtils::MoveResult expected =
        fence == BookMutationFence::Indeterminate ? BookMoveUtils::MoveResult::Pending
                                                  : BookMoveUtils::MoveResult::Conflict;
    expect(result == expected, "pending delete must serialize a PDF move");
    expect(Storage.exists(oldPath.c_str()) && !Storage.exists(newPath.c_str()),
           "delete-fenced move must preserve source visibility");
    expect(!Storage.exists(BookStateMigration::kSlotAPath) && !Storage.exists(BookStateMigration::kSlotBPath),
           "delete-fenced move must not prepare its journal");
    expect(PdfDeleteUtils::fenceQueries == 1, "move must query the delete fence exactly once");
  }
}

}  // namespace

int main() {
  testEmptyBootRecoverySkipsFullMoveWorkspace();
  testCompleteMoveCopiesAndVerifiesBeforeTerminalCleanup();
  testFailureAfterSourceRenameRetainsOldStateAndRebootsIdempotently();
  testPendingMoveFencesOnlyItsAffectedPdfCacheIdentity();
  testActivatedJournalUsesNewWritableHashUntilCleanupRetries();
  testResolverFailureFencesFallbackAndPreservesNormalHashOutput();
  testFirstEmptyResolverLookupPublishesNoJournalFastPath();
  testKnownAbsentJournalNeedsNoLookupAllocation();
  testRenameFailureLeavesOldSourceAndStateUntouched();
  testRealStateHashCollisionFailsBeforeJournalMutation();
  testReadFolderDestinationKeepsExistingSuffixBehavior();
  testNonPdfMovesNeverEnterTheJournaledPath();
  testLegacyEpubMigrationRetainsPrePdfBehavior();
  testLegacyEpubRemoveFromRecentsSemanticsRemainUnchanged();
  testDroppedRecentPersistenceCannotDeleteOldState();
  testDroppedOpenPathPersistenceCannotDeleteOldState();
  testRemoveFromRecentsPolicySurvivesReboot();
  testPendingDeleteSerializesPdfMoveBeforeJournalMutation();
  if (failures != 0) return 1;
  std::cout << "BOOK_MOVE_PRODUCT_PASS\n";
  return 0;
}
