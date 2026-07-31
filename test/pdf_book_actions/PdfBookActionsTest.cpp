#include <algorithm>
#include <iostream>
#include <string>
#include <vector>

#include "BookActions.h"
#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "PdfCachedProductState.h"
#include "PdfSourceIdentity.h"
#include "TestState.h"
#include "util/BookMoveUtils.h"
#include "util/PdfDeleteUtils.h"

namespace {

constexpr char kPdfPath[] = "/Books/action.pdf";
constexpr char kPdfDisplayName[] = "action.pdf";

int failures = 0;

void expect(const bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

std::vector<FileBrowserAction> actionsFor(const std::string& path, const bool includeRemove = false) {
  const auto items = BookActions::buildBookActionItems(path, includeRemove);
  std::vector<FileBrowserAction> actions;
  actions.reserve(items.size());
  for (const auto& item : items) actions.push_back(item.action);
  return actions;
}

std::string cachePathForHash(const uint64_t hash) {
  char path[PDF_CACHE_PATH_CAPACITY]{};
  const int length =
      std::snprintf(path, sizeof(path), "/.crosspoint/pdf_%016llx", static_cast<unsigned long long>(hash));
  return length > 0 ? std::string(path, static_cast<size_t>(length)) : std::string{};
}

uint64_t normalPdfHash() { return pdfPathHash64(kPdfPath, sizeof(kPdfPath) - 1U); }

std::string writablePdfCachePath() {
  const uint64_t hash = TEST_STATE.resolvedHash == 0 ? normalPdfHash() : TEST_STATE.resolvedHash;
  return cachePathForHash(hash);
}

void testPdfMenuHasParityWithoutEpubOnlyActions() {
  resetBookActionTestState();
  expect(actionsFor(kPdfPath) ==
             std::vector<FileBrowserAction>({
                 FileBrowserAction::Delete,
                 FileBrowserAction::DeleteCache,
                 FileBrowserAction::DeleteStats,
                 FileBrowserAction::ToggleCompleted,
             }),
         "PDF menu must expose Delete, Clear Cache, Delete Stats, Toggle Completed in order");
  expect(actionsFor(kPdfPath, true) ==
             std::vector<FileBrowserAction>({
                 FileBrowserAction::Delete,
                 FileBrowserAction::DeleteCache,
                 FileBrowserAction::DeleteStats,
                 FileBrowserAction::ToggleCompleted,
                 FileBrowserAction::RemoveFromRecents,
             }),
         "PDF recents menu must append existing Remove From Recents action");
  expect(BookActions::hasClearableBookCache(kPdfPath), "PDF cache must be clearable");

  const auto pdfActions = actionsFor(kPdfPath, true);
  expect(std::find(pdfActions.begin(), pdfActions.end(), FileBrowserAction::EpubRenderMode) == pdfActions.end(),
         "EPUB render mode must remain absent from PDF");
  expect(std::find(pdfActions.begin(), pdfActions.end(), FileBrowserAction::ResetReaderSettings) == pdfActions.end(),
         "EPUB reader settings reset must remain absent from PDF");
}

void testPdfDeleteUsesJournaledAdapterOnlyForPdf() {
  resetBookActionTestState();
  expect(BookActions::deletePdfBook(kPdfPath), "PDF delete must report a complete adapter result");
  expect(TEST_STATE.pdfDeleteCalls == 1 && TEST_STATE.pdfDeletePath == kPdfPath,
         "PDF delete must delegate exactly once with the unchanged source path");

  resetBookActionTestState();
  TEST_STATE.pdfDeleteResult = static_cast<uint8_t>(PdfDeleteUtils::Result::Pending);
  expect(!BookActions::deletePdfBook(kPdfPath), "pending journaled PDF delete must remain incomplete");
  expect(TEST_STATE.pdfDeleteCalls == 1, "pending PDF delete must make exactly one adapter call");

  resetBookActionTestState();
  expect(!BookActions::deletePdfBook("/Books/legacy.epub"), "PDF-only delete seam must reject legacy formats");
  expect(TEST_STATE.pdfDeleteCalls == 0, "legacy formats must never enter the PDF delete adapter");
}

void testLegacyMenusRemainExactlyOrdered() {
  resetBookActionTestState();
  expect(actionsFor("/Books/legacy.epub", true) ==
             std::vector<FileBrowserAction>({
                 FileBrowserAction::Delete,
                 FileBrowserAction::DeleteCache,
                 FileBrowserAction::EpubRenderMode,
                 FileBrowserAction::ResetReaderSettings,
                 FileBrowserAction::DeleteStats,
                 FileBrowserAction::ToggleCompleted,
                 FileBrowserAction::RemoveFromRecents,
             }),
         "EPUB menu order must remain unchanged");
  expect(actionsFor("/Books/legacy.xtc", true) ==
             std::vector<FileBrowserAction>({
                 FileBrowserAction::Delete,
                 FileBrowserAction::DeleteCache,
                 FileBrowserAction::DeleteStats,
                 FileBrowserAction::ToggleCompleted,
                 FileBrowserAction::RemoveFromRecents,
             }),
         "XTC menu order must remain unchanged");
  expect(actionsFor("/Books/legacy.txt", true) ==
             std::vector<FileBrowserAction>({
                 FileBrowserAction::Delete,
                 FileBrowserAction::RemoveFromRecents,
             }),
         "TXT menu order must remain unchanged");
}

void testPdfCacheAndStatsUseWritableMoveAwareIdentity() {
  resetBookActionTestState();
  TEST_STATE.resolvedHash = 0x1122334455667788ULL;
  const std::string expectedCache = writablePdfCachePath();
  TEST_STATE.completedByCache[expectedCache] = true;

  expect(BookActions::clearBookCache(kPdfPath), "PDF Clear Cache must delegate to the guarded cache clearer");
  expect(TEST_STATE.cacheClears == std::vector<std::string>({kPdfPath}),
         "PDF Clear Cache must pass the exact source path");
  expect(BookActions::isBookCompleted(kPdfPath), "PDF completed state must load from resolved stats root");
  expect(TEST_STATE.statsLoads == std::vector<std::string>({expectedCache}),
         "PDF completed state must use resolved cache root");
  expect(BookActions::deleteBookStats(kPdfPath), "PDF Delete Stats must remove common stats");
  expect(TEST_STATE.statsRemoves == std::vector<std::string>({expectedCache}),
         "PDF Delete Stats must use the same resolved cache root");
  expect(TEST_STATE.resolverCalls == 2, "stats read and removal must each resolve move state once");

  TEST_STATE.cacheClearResult = false;
  expect(!BookActions::clearBookCache(kPdfPath), "PDF Clear Cache must propagate guarded clearer failure");
}

void testUnsafePdfStatsIdentityFailsBeforeMutation() {
  for (const bool resolverFailure : {false, true}) {
    resetBookActionTestState();
    TEST_STATE.resolverSucceeds = !resolverFailure;
    TEST_STATE.readOnlyFallback = !resolverFailure;
    const std::string context = resolverFailure ? "resolver error" : "read-only fallback";

    expect(!BookActions::deleteBookStats(kPdfPath), context + " must refuse PDF stats deletion");
    expect(!BookActions::isBookCompleted(kPdfPath), context + " must not expose writable PDF completion state");
    bool completed = true;
    expect(!BookActions::toggleBookCompleted(kPdfPath, kPdfDisplayName, completed),
           context + " must refuse completion mutation");
    expect(completed, context + " must leave caller completion output unchanged");
    expect(TEST_STATE.statsLoads.empty() && TEST_STATE.statsSaves.empty() && TEST_STATE.statsRemoves.empty(),
           context + " must touch no per-book stats");
    expect(TEST_STATE.globalLoads == 0 && TEST_STATE.globalSaves == 0,
           context + " must touch no global stats");
    expect(TEST_STATE.recentAdds.empty() && TEST_STATE.recentRemovals.empty(),
           context + " must touch no recents");
    expect(TEST_STATE.pdfMoveCalls == 0, context + " must not begin a move");
  }
}

void testPdfCompletionUsesCommonStatsAndRemovesRecentWithoutProductLoad() {
  resetBookActionTestState();
  TEST_SETTINGS.removeReadBooksFromRecents = 1;
  TEST_STATE.globalCompleted = 4;
  TEST_STATE.dateAvailable = true;
  const std::string cachePath = writablePdfCachePath();

  bool completed = false;
  expect(BookActions::toggleBookCompleted(kPdfPath, kPdfDisplayName, completed),
         "marking cached PDF completed must succeed");
  expect(completed, "completion output must become true");
  expect(TEST_STATE.statsLoads == std::vector<std::string>({cachePath, cachePath}) &&
             TEST_STATE.statsSaves == std::vector<std::string>({cachePath}) &&
             TEST_STATE.completedByCache[cachePath],
         "PDF completion must save and read back common per-book stats at resolved root");
  expect(TEST_STATE.globalLoads == 1 && TEST_STATE.globalSaves == 1 && TEST_STATE.globalCompleted == 5,
         "PDF completion must increment and save common global stats");
  expect(TEST_STATE.recentRemovals == std::vector<std::string>({kPdfPath}),
         "completed PDF must follow remove-from-recents setting");
  expect(TEST_STATE.productLoads == 0 && TEST_STATE.sourceIdentityPasses == 0,
         "removing a recent must not load cosmetic cached product state");
  expect(TEST_STATE.epubConstructs == 0 && TEST_STATE.xtcConstructs == 0 && TEST_STATE.storageRenames == 0,
         "PDF completion must not enter legacy source handlers");
}

void testPdfUncompletionReaddsOnlyCachedProductMetadata() {
  resetBookActionTestState();
  TEST_SETTINGS.removeReadBooksFromRecents = 1;
  TEST_STATE.globalCompleted = 2;
  TEST_STATE.productKind = static_cast<uint8_t>(PdfCachedProductStateKind::Available);
  TEST_STATE.productTitle = "Cached PDF title";
  TEST_STATE.productAuthor = "Cached PDF author";
  TEST_STATE.productThumbnail = "/.crosspoint/pdf_hash/gen_9/thumb.bmp";
  const std::string cachePath = writablePdfCachePath();
  TEST_STATE.completedByCache[cachePath] = true;

  bool completed = true;
  expect(BookActions::toggleBookCompleted(kPdfPath, kPdfDisplayName, completed),
         "marking PDF unfinished must succeed");
  expect(!completed, "completion output must become false");
  expect(TEST_STATE.globalCompleted == 1, "uncompletion must decrement common global completed count");
  expect(TEST_STATE.productLoads == 1 && TEST_STATE.sourceIdentityPasses == 1,
         "PDF re-add must perform exactly one cached-product identity pass");
  expect(TEST_STATE.recentAdds.size() == 1, "unfinished PDF must be restored to recents");
  if (TEST_STATE.recentAdds.size() == 1) {
    const AddedRecent& recent = TEST_STATE.recentAdds.front();
    expect(recent.path == kPdfPath && recent.title == TEST_STATE.productTitle &&
               recent.author == TEST_STATE.productAuthor && recent.thumbnail == TEST_STATE.productThumbnail,
           "PDF recent metadata must come only from validated cached product state");
  }
  expect(TEST_STATE.epubConstructs == 0 && TEST_STATE.xtcConstructs == 0,
         "cached PDF metadata must not use EPUB/XTC extraction");
}

void testMissingStaleAndCorruptProductUseCosmeticFallback() {
  for (const PdfCachedProductStateKind kind :
       {PdfCachedProductStateKind::Missing, PdfCachedProductStateKind::Stale, PdfCachedProductStateKind::Corrupt}) {
    resetBookActionTestState();
    TEST_SETTINGS.removeReadBooksFromRecents = 1;
    TEST_STATE.globalCompleted = 1;
    TEST_STATE.productKind = static_cast<uint8_t>(kind);
    const std::string cachePath = writablePdfCachePath();
    TEST_STATE.completedByCache[cachePath] = true;

    bool completed = true;
    expect(BookActions::toggleBookCompleted(kPdfPath, kPdfDisplayName, completed),
           "cosmetic cached-product failure must not fail durable uncompletion");
    expect(!completed && !TEST_STATE.completedByCache[cachePath],
           "cosmetic fallback must still persist unfinished stats");
    expect(TEST_STATE.productLoads == 1 && TEST_STATE.sourceIdentityPasses == 1,
           "each cosmetic fallback must make one bounded identity pass");
    expect(TEST_STATE.recentAdds.size() == 1, "cosmetic fallback must still restore recent");
    if (TEST_STATE.recentAdds.size() == 1) {
      const AddedRecent& recent = TEST_STATE.recentAdds.front();
      expect(recent.title == kPdfDisplayName && recent.author.empty() && recent.thumbnail.empty(),
             "missing/stale/corrupt product must use display name and empty cached fields");
    }
  }
}

void testPdfRecentMetadataAllocationFailureUsesCosmeticFallback() {
  resetBookActionTestState();
  TEST_SETTINGS.removeReadBooksFromRecents = 1;
  TEST_STATE.globalCompleted = 1;
  TEST_STATE.failProductStateAllocation = true;
  const std::string cachePath = writablePdfCachePath();
  TEST_STATE.completedByCache[cachePath] = true;

  bool completed = true;
  expect(BookActions::toggleBookCompleted(kPdfPath, kPdfDisplayName, completed),
         "cached-product workspace allocation failure must not fail durable uncompletion");
  expect(!completed && !TEST_STATE.completedByCache[cachePath],
         "allocation fallback must still persist unfinished stats");
  expect(TEST_STATE.productStateAllocations == 1 && TEST_STATE.productLoads == 0 &&
             TEST_STATE.sourceIdentityPasses == 0,
         "allocation fallback must fail before a source identity pass");
  expect(TEST_STATE.recentAdds.size() == 1, "allocation fallback must still restore recent");
  if (TEST_STATE.recentAdds.size() == 1) {
    const AddedRecent& recent = TEST_STATE.recentAdds.front();
    expect(recent.title == kPdfDisplayName && recent.author.empty() && recent.thumbnail.empty(),
           "allocation fallback must use display name and empty cached fields");
  }
}

void testMovedPdfRecentMetadataUsesResolvedHashOverride() {
  resetBookActionTestState();
  TEST_SETTINGS.removeReadBooksFromRecents = 1;
  TEST_STATE.globalCompleted = 1;
  TEST_STATE.resolvedHash = 0xa1a2a3a4a5a6a7a8ULL;
  TEST_STATE.productKind = static_cast<uint8_t>(PdfCachedProductStateKind::Available);
  const std::string cachePath = writablePdfCachePath();
  TEST_STATE.completedByCache[cachePath] = true;

  bool completed = true;
  expect(BookActions::toggleBookCompleted(kPdfPath, kPdfDisplayName, completed),
         "moved PDF uncompletion must succeed");
  expect(TEST_STATE.productHashOverrideSupplied &&
             TEST_STATE.productHashOverride == TEST_STATE.resolvedHash,
         "cached product load must use the move-aware resolved hash");
}

void testPdfCompletionMoveUsesJournaledMoveAndRecentsPolicy() {
  for (const bool removeFromRecents : {false, true}) {
    resetBookActionTestState();
    TEST_SETTINGS.moveFinishedToReadFolder = 1;
    TEST_SETTINGS.removeReadBooksFromRecents = removeFromRecents ? 1 : 0;
    TEST_STATE.pdfMoveResult = static_cast<uint8_t>(BookMoveUtils::MoveResult::Complete);

    bool completed = false;
    expect(BookActions::toggleBookCompleted(kPdfPath, kPdfDisplayName, completed),
           "PDF completion with move enabled must succeed");
    expect(completed, "moved PDF must remain completed");
    expect(TEST_STATE.pdfMoveCalls == 1 && TEST_STATE.pdfMoveOldPath == kPdfPath &&
               TEST_STATE.pdfMoveNewPath == "/Read/action.pdf",
           "PDF completion must use journaled BookMoveUtils move");
    expect(TEST_STATE.pdfMoveKeepInRecents == !removeFromRecents,
           "PDF move must preserve configured keep/remove recents policy");
    expect(TEST_STATE.storageRenames == 0 && TEST_STATE.epubStateMigrations == 0,
           "PDF move must not use legacy rename/migration path");
  }
}

void testIncompletePdfMoveKeepsCompletionAndRaisesAlert() {
  resetBookActionTestState();
  TEST_SETTINGS.moveFinishedToReadFolder = 1;
  TEST_STATE.pdfMoveResult = static_cast<uint8_t>(BookMoveUtils::MoveResult::Pending);

  bool completed = false;
  expect(BookActions::toggleBookCompleted(kPdfPath, kPdfDisplayName, completed),
         "incomplete journaled move must not roll back persisted completion");
  expect(completed && TEST_STATE.pdfMoveCalls == 1,
         "incomplete move must preserve completed state after one move attempt");
  expect(TEST_APP_STATE.hasPendingAlert.load() && !TEST_APP_STATE.pendingAlertGoHomeOnBack.load(),
         "incomplete PDF move must surface the existing move-failed alert");

  resetBookActionTestState();
  TEST_SETTINGS.moveFinishedToReadFolder = 1;
  completed = false;
  expect(BookActions::toggleBookCompleted("/Read/action.pdf", kPdfDisplayName, completed),
         "already-filed PDF completion must succeed");
  expect(TEST_STATE.pdfMoveCalls == 0, "PDF already under /Read must not be moved again");
}

void testLegacyCompletionControlFlowStillUsesLegacyRoutes() {
  resetBookActionTestState();
  TEST_SETTINGS.moveFinishedToReadFolder = 1;
  bool completed = false;
  expect(BookActions::toggleBookCompleted("/Books/legacy.epub", "Legacy", completed),
         "legacy EPUB completion must still succeed");
  expect(TEST_STATE.epubConstructs == 1 && TEST_STATE.xtcConstructs == 1 && TEST_STATE.epubSetups == 1,
         "legacy EPUB constructor/setup flow must remain unchanged");
  expect(TEST_STATE.storageRenames == 1 && TEST_STATE.epubStateMigrations == 1 && TEST_STATE.pdfMoveCalls == 0,
         "legacy EPUB move must retain direct rename plus EPUB migration");

  resetBookActionTestState();
  completed = false;
  expect(BookActions::toggleBookCompleted("/Books/legacy.xtc", "Legacy", completed),
         "legacy XTC completion must still succeed");
  expect(TEST_STATE.epubConstructs == 1 && TEST_STATE.xtcConstructs == 1 && TEST_STATE.xtcLoads == 1 &&
             TEST_STATE.xtcSetups == 1,
         "legacy XTC construction/load/setup flow must remain unchanged");
  expect(TEST_STATE.pdfMoveCalls == 0, "legacy XTC must not use PDF move");
}

}  // namespace

int main() {
  testPdfMenuHasParityWithoutEpubOnlyActions();
  testPdfDeleteUsesJournaledAdapterOnlyForPdf();
  testLegacyMenusRemainExactlyOrdered();
  testPdfCacheAndStatsUseWritableMoveAwareIdentity();
  testUnsafePdfStatsIdentityFailsBeforeMutation();
  testPdfCompletionUsesCommonStatsAndRemovesRecentWithoutProductLoad();
  testPdfUncompletionReaddsOnlyCachedProductMetadata();
  testMissingStaleAndCorruptProductUseCosmeticFallback();
  testPdfRecentMetadataAllocationFailureUsesCosmeticFallback();
  testMovedPdfRecentMetadataUsesResolvedHashOverride();
  testPdfCompletionMoveUsesJournaledMoveAndRecentsPolicy();
  testIncompletePdfMoveKeepsCompletionAndRaisesAlert();
  testLegacyCompletionControlFlowStillUsesLegacyRoutes();
  if (failures != 0) return 1;
  std::cout << "PDF_BOOK_ACTIONS_PASS\n";
  return 0;
}
