#include "BookActions.h"

#include <Epub.h>
#include <Epub/EpubRenderMode.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>
#include <Xtc.h>
// PDF_BOOK_ACTIONS_PARITY_BEGIN: includes
#include <Memory.h>
#include <PdfCachedProductState.h>
#include <PdfHalCacheIo.h>
#include <PdfSourceIdentity.h>
#include "util/PdfDeleteUtils.h"
// PDF_BOOK_ACTIONS_PARITY_END: includes

#include <cstdio>

#include "BookmarkStore.h"
#include "ClippingStore.h"
#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "RecentBooksStore.h"
#include "activities/reader/BookReadingStats.h"
#include "activities/reader/EpubReaderActivity.h"
#include "activities/reader/GlobalReadingStats.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/BookCacheUtils.h"
#include "util/BookMoveUtils.h"

namespace BookActions {
namespace {

// PDF_BOOK_ACTIONS_PARITY_BEGIN: helpers
constexpr char PDF_ACTION_CACHE_DIRECTORY[] = "/.crosspoint";

struct PdfRecentMetadataWorkspace {
  PdfHalCacheIoContext io{};
  PdfCachedProductState state{};
};

static_assert(sizeof(PdfRecentMetadataWorkspace) <= 2048,
              "PDF completion metadata workspace exceeded its cold-path budget");

[[gnu::noinline]] bool resolveWritablePdfCachePath(const std::string& path, std::string& cachePath,
                                                   uint64_t* const resolvedHashOutput = nullptr) {
  const uint64_t normalHash = pdfPathHash64(path.c_str(), path.size());
  uint64_t resolvedHash = normalHash;
  bool readOnlyFallback = true;
  if (!BookMoveUtils::migrationCacheHash(path, normalHash, &resolvedHash, &readOnlyFallback) || readOnlyFallback) {
    LOG_ERR("BookActions", "Refusing PDF stats access while migration state is unresolved");
    return false;
  }

  char resolvedPath[PDF_CACHE_PATH_CAPACITY]{};
  if (!pdfFormatCacheRootForHash(PDF_ACTION_CACHE_DIRECTORY, resolvedHash, resolvedPath, sizeof(resolvedPath))) {
    LOG_ERR("BookActions", "Failed to resolve PDF cache path: %s", path.c_str());
    return false;
  }
  cachePath = resolvedPath;
  if (resolvedHashOutput != nullptr) *resolvedHashOutput = resolvedHash;
  return true;
}

[[gnu::noinline]] bool ensureVerifiedPdfCacheRoot(const std::string& cachePath) {
  if (!Storage.exists(cachePath.c_str()) && !Storage.mkdir(cachePath.c_str())) {
    LOG_ERR("BookActions", "Failed to create PDF stats root: %s", cachePath.c_str());
    return false;
  }

  FsFile root = Storage.open(cachePath.c_str());
  const bool validDirectory = root && root.isDirectory();
  root.close();
  if (!validDirectory) {
    LOG_ERR("BookActions", "Failed to verify PDF stats root: %s", cachePath.c_str());
    return false;
  }
  return true;
}

[[gnu::noinline]] void loadPdfRecentMetadata(const std::string& path, const std::string& displayName,
                                             const uint64_t resolvedHash, std::string& title, std::string& author,
                                             std::string& thumbnail) {
  title = displayName;
  author.clear();
  thumbnail.clear();

  // Cached-product I/O and fixed metadata buffers exceed the 256-byte stack
  // budget, so this cold action uses one checked, short-lived heap workspace.
  auto workspace = makeUniqueNoThrow<PdfRecentMetadataWorkspace>();
  if (!workspace) {
    LOG_ERR("BookActions", "Failed to allocate PDF completion metadata workspace (%u bytes)",
            static_cast<unsigned>(sizeof(PdfRecentMetadataWorkspace)));
    return;
  }

  const uint64_t normalHash = pdfPathHash64(path.c_str(), path.size());
  const uint64_t* const cacheHashOverride = resolvedHash == normalHash ? nullptr : &resolvedHash;
  const PdfCachedProductStateLoadResult loaded =
      pdfLoadCachedProductState(pdfHalCacheIo(workspace->io), path.c_str(), PDF_ACTION_CACHE_DIRECTORY,
                                &workspace->state, cacheHashOverride);
  if (!loaded.available()) return;

  if (workspace->state.title[0] != '\0') title = workspace->state.title;
  author = workspace->state.author;
  thumbnail = workspace->state.thumbnailPath;
}

[[gnu::noinline]] void setPdfFinishedDateIfNeeded(BookReadingStats& stats, const bool completed) {
  if (!completed || stats.finishedDateManual) return;

  ReadingStatsDateTime now;
  if (getCurrentLocalReadingStatsDateTime(now)) stats.finishedDate = now.date;
}

[[gnu::noinline]] bool verifyPdfCompletionDurable(const std::string& cachePath, const bool completed) {
  if (BookReadingStats::load(cachePath).isCompleted == completed) return true;

  LOG_ERR("BookActions", "PDF completion was not durable: %s", cachePath.c_str());
  return false;
}

[[gnu::noinline]] bool toggleAndSavePdfBookStats(const std::string& cachePath, bool& completed) {
  BookReadingStats stats = BookReadingStats::load(cachePath);
  const bool nextCompleted = !stats.isCompleted;
  stats.isCompleted = nextCompleted;
  setPdfFinishedDateIfNeeded(stats, nextCompleted);
  stats.save(cachePath);
  // save() has no result, so a bounded stats-file readback is the durable
  // success witness before global stats, recents, or the source path can move.
  if (!verifyPdfCompletionDurable(cachePath, nextCompleted)) return false;

  completed = nextCompleted;
  return true;
}

[[gnu::noinline]] void updatePdfGlobalCompletionStats(const bool completed) {
  GlobalReadingStats globalStats = GlobalReadingStats::load();
  if (completed) {
    globalStats.completedBooks++;
  } else if (globalStats.completedBooks > 0) {
    globalStats.completedBooks--;
  }

  globalStats.save();
}

[[gnu::noinline]] void updatePdfRecentAfterCompletion(const std::string& fullPath,
                                                      const std::string& displayName,
                                                      const uint64_t resolvedHash, const bool completed) {
  if (!SETTINGS.removeReadBooksFromRecents) return;
  if (completed) {
    RECENT_BOOKS.removeByPath(fullPath);
    return;
  }

  std::string title;
  std::string author;
  std::string thumbnail;
  loadPdfRecentMetadata(fullPath, displayName, resolvedHash, title, author, thumbnail);
  RECENT_BOOKS.addOrUpdateBook(fullPath, title, author, thumbnail);
}

[[gnu::noinline]] void moveCompletedPdfIfConfigured(const std::string& fullPath, const std::string& displayName,
                                                    const bool completed) {
  if (!completed || !SETTINGS.moveFinishedToReadFolder || fullPath.rfind("/Read/", 0) == 0) return;

  const std::string destination = BookMoveUtils::buildReadFolderDestination(fullPath);
  const BookMoveUtils::MoveResult result =
      BookMoveUtils::moveBook(fullPath, destination, !SETTINGS.removeReadBooksFromRecents);
  if (result == BookMoveUtils::MoveResult::Complete) return;

  LOG_ERR("BookActions", "PDF move remains incomplete (%u)", static_cast<unsigned>(result));
  snprintf(APP_STATE.pendingAlertTitle, sizeof(APP_STATE.pendingAlertTitle), "%s",
           tr(STR_MOVE_TO_READ_FAILED_TITLE));
  snprintf(APP_STATE.pendingAlertBody, sizeof(APP_STATE.pendingAlertBody), tr(STR_MOVE_TO_READ_FAILED_BODY),
           displayName.c_str());
  APP_STATE.pendingAlertGoHomeOnBack.store(false, std::memory_order_relaxed);
  APP_STATE.hasPendingAlert.store(true, std::memory_order_release);
}

[[gnu::noinline]] bool togglePdfBookCompleted(const std::string& fullPath, const std::string& displayName,
                                              bool& completed) {
  std::string cachePath;
  uint64_t resolvedHash = 0;
  if (!resolveWritablePdfCachePath(fullPath, cachePath, &resolvedHash)) return false;
  if (!ensureVerifiedPdfCacheRoot(cachePath)) return false;

  if (!toggleAndSavePdfBookStats(cachePath, completed)) return false;
  updatePdfGlobalCompletionStats(completed);
  updatePdfRecentAfterCompletion(fullPath, displayName, resolvedHash, completed);
  moveCompletedPdfIfConfigured(fullPath, displayName, completed);
  return true;
}
// PDF_BOOK_ACTIONS_PARITY_END: helpers
bool hasReadingStats(const std::string& path) {
  // PDF_BOOK_ACTIONS_PARITY_BEGIN: reading-stats predicate
  if (FsHelpers::hasPdfExtension(path)) {
    return true;
  }
  // PDF_BOOK_ACTIONS_PARITY_END: reading-stats predicate
  return FsHelpers::hasEpubExtension(path) || FsHelpers::hasXtcExtension(path);
}

std::string bookStatsCachePath(const std::string& path) {
  // PDF_BOOK_ACTIONS_PARITY_BEGIN: stats root
  if (FsHelpers::hasPdfExtension(path)) {
    std::string cachePath;
    return resolveWritablePdfCachePath(path, cachePath) ? cachePath : "";
  }
  // PDF_BOOK_ACTIONS_PARITY_END: stats root
  if (FsHelpers::hasEpubExtension(path)) {
    return Epub(path, "/.crosspoint").getCachePath();
  }
  if (FsHelpers::hasXtcExtension(path)) {
    return Xtc(path, "/.crosspoint").getCachePath();
  }
  return "";
}

}  // namespace

std::vector<FileBrowserActionActivity::MenuItem> buildBookActionItems(const std::string& fullPath,
                                                                      const bool includeRemoveFromRecents) {
  // PDF_BOOK_ACTIONS_PARITY_BEGIN: menu
  if (FsHelpers::hasPdfExtension(fullPath)) {
    std::vector<FileBrowserActionActivity::MenuItem> items;
    items.reserve(includeRemoveFromRecents ? 5 : 4);
    items.push_back({FileBrowserAction::Delete, StrId::STR_DELETE});
    items.push_back({FileBrowserAction::DeleteCache, StrId::STR_DELETE_CACHE});
    items.push_back({FileBrowserAction::DeleteStats, StrId::STR_DELETE_BOOK_STATS});
    items.push_back({FileBrowserAction::ToggleCompleted,
                     isBookCompleted(fullPath) ? StrId::STR_MARK_UNFINISHED : StrId::STR_MARK_FINISHED});
    if (includeRemoveFromRecents) {
      items.push_back({FileBrowserAction::RemoveFromRecents, StrId::STR_REMOVE_FROM_RECENTS_ACTION});
    }
    return items;
  }
  // PDF_BOOK_ACTIONS_PARITY_END: menu
  std::vector<FileBrowserActionActivity::MenuItem> items;
  items.reserve(includeRemoveFromRecents ? 7 : 6);
  items.push_back({FileBrowserAction::Delete, StrId::STR_DELETE});
  if (hasClearableBookCache(fullPath)) {
    items.push_back({FileBrowserAction::DeleteCache, StrId::STR_DELETE_CACHE});
  }
  if (FsHelpers::hasEpubExtension(fullPath)) {
    items.push_back({FileBrowserAction::EpubRenderMode, StrId::STR_EPUB_RENDER_MODE});
    items.push_back({FileBrowserAction::ResetReaderSettings, StrId::STR_RESET_BOOK_READER_SETTINGS});
  }
  if (hasReadingStats(fullPath)) {
    items.push_back({FileBrowserAction::DeleteStats, StrId::STR_DELETE_BOOK_STATS});
    items.push_back({FileBrowserAction::ToggleCompleted,
                     isBookCompleted(fullPath) ? StrId::STR_MARK_UNFINISHED : StrId::STR_MARK_FINISHED});
  }
  if (includeRemoveFromRecents) {
    items.push_back({FileBrowserAction::RemoveFromRecents, StrId::STR_REMOVE_FROM_RECENTS_ACTION});
  }
  return items;
}

bool hasClearableBookCache(const std::string& path) {
  // PDF_BOOK_ACTIONS_PARITY_BEGIN: clearable cache
  if (FsHelpers::hasPdfExtension(path)) {
    return true;
  }
  // PDF_BOOK_ACTIONS_PARITY_END: clearable cache
  return FsHelpers::hasEpubExtension(path) || FsHelpers::hasXtcExtension(path);
}

void clearFileMetadata(const std::string& fullPath) {
  if (FsHelpers::hasEpubExtension(fullPath)) {
    Epub(fullPath, "/.crosspoint").clearCache();
    BookmarkStore::deleteForFilePath(fullPath, "epub");
    ClippingStore::deleteForFilePath(fullPath, "epub");
  } else if (FsHelpers::hasXtcExtension(fullPath)) {
    BookmarkStore::deleteForFilePath(fullPath, "xtc");
  } else if (FsHelpers::hasTxtExtension(fullPath) || FsHelpers::hasMarkdownExtension(fullPath)) {
    BookmarkStore::deleteForFilePath(fullPath, "txt");
  }
  LOG_DBG("BookActions", "Cleared metadata for: %s", fullPath.c_str());
}

// PDF_BOOK_ACTIONS_PARITY_BEGIN: delete
bool deletePdfBook(const std::string& fullPath) {
  if (!FsHelpers::hasPdfExtension(fullPath)) return false;
  return PdfDeleteUtils::deletePdfBook(fullPath) == PdfDeleteUtils::Result::Complete;
}
// PDF_BOOK_ACTIONS_PARITY_END: delete
bool clearBookCache(const std::string& fullPath) {
  // PDF_BOOK_ACTIONS_PARITY_BEGIN: clear cache
  if (FsHelpers::hasPdfExtension(fullPath)) {
    return clearBookCachePreservingUserState(fullPath);
  }
  // PDF_BOOK_ACTIONS_PARITY_END: clear cache
  if (FsHelpers::hasEpubExtension(fullPath) || FsHelpers::hasXtcExtension(fullPath)) {
    return clearBookCachePreservingUserState(fullPath);
  }
  return false;
}

bool deleteBookStats(const std::string& fullPath) {
  const std::string cachePath = bookStatsCachePath(fullPath);
  if (cachePath.empty()) {
    return false;
  }
  return BookReadingStats::remove(cachePath);
}

bool resetBookReaderSettings(const std::string& fullPath) {
  if (!FsHelpers::hasEpubExtension(fullPath)) {
    return false;
  }
  return EpubReaderActivity::resetBookReaderSettings(fullPath);
}

std::vector<std::string> epubRenderModeOptions() {
  return {I18N.get(StrId::STR_RENDER_MODE_CROSSINK_DEFAULT), I18N.get(StrId::STR_RENDER_MODE_BALANCED),
          I18N.get(StrId::STR_RENDER_MODE_LIGHT)};
}

uint8_t epubRenderModeDisplayIndex(const uint8_t renderMode) {
  switch (static_cast<EpubRenderMode>(renderMode)) {
    case EpubRenderMode::Balanced:
      return 1;
    case EpubRenderMode::Light:
      return 2;
    case EpubRenderMode::CrossInkDefault:
    default:
      return 0;
  }
}

uint8_t epubRenderModeForDisplayIndex(const uint8_t displayIndex) {
  switch (displayIndex) {
    case 1:
      return static_cast<uint8_t>(EpubRenderMode::Balanced);
    case 2:
      return static_cast<uint8_t>(EpubRenderMode::Light);
    case 0:
    default:
      return static_cast<uint8_t>(EpubRenderMode::CrossInkDefault);
  }
}

std::string confirmationHeading(const StrId actionLabelId) {
  return std::string(tr(STR_CONFIRM)) + ": " + std::string(I18N.get(actionLabelId));
}

bool isBookCompleted(const std::string& fullPath) {
  const std::string cachePath = bookStatsCachePath(fullPath);
  return !cachePath.empty() && BookReadingStats::load(cachePath).isCompleted;
}

bool toggleBookCompleted(const std::string& fullPath, const std::string& displayName, bool& completed) {
  // PDF_BOOK_ACTIONS_PARITY_BEGIN: completion
  if (FsHelpers::hasPdfExtension(fullPath)) {
    return togglePdfBookCompleted(fullPath, displayName, completed);
  }
  // PDF_BOOK_ACTIONS_PARITY_END: completion
  const bool isEpub = FsHelpers::hasEpubExtension(fullPath);
  const bool isXtc = FsHelpers::hasXtcExtension(fullPath);
  if (!isEpub && !isXtc) {
    return false;
  }

  Epub epub(fullPath, "/.crosspoint");
  Xtc xtc(fullPath, "/.crosspoint");
  std::string cachePath;
  std::string title;
  std::string author;
  std::string thumbPath;
  if (isEpub) {
    epub.setupCacheDir();
    cachePath = epub.getCachePath();
    title = epub.getTitle();
    author = epub.getAuthor();
    thumbPath = epub.getThumbBmpPath();
  } else {
    if (!xtc.load()) {
      return false;
    }
    xtc.setupCacheDir();
    cachePath = xtc.getCachePath();
    title = xtc.getTitle();
    author = xtc.getAuthor();
    thumbPath = xtc.getThumbBmpPath();
  }

  BookReadingStats stats = BookReadingStats::load(cachePath);
  completed = !stats.isCompleted;
  stats.isCompleted = completed;
  if (completed && !stats.finishedDateManual) {
    ReadingStatsDateTime now;
    if (getCurrentLocalReadingStatsDateTime(now)) {
      stats.finishedDate = now.date;
    }
  }

  GlobalReadingStats globalStats = GlobalReadingStats::load();
  if (completed) {
    globalStats.completedBooks++;
  } else if (globalStats.completedBooks > 0) {
    globalStats.completedBooks--;
  }

  stats.save(cachePath);
  globalStats.save();

  if (SETTINGS.removeReadBooksFromRecents) {
    if (completed) {
      RECENT_BOOKS.removeByPath(fullPath);
    } else {
      RECENT_BOOKS.addOrUpdateBook(fullPath, title, author, thumbPath);
    }
  }

  if (isEpub && completed && SETTINGS.moveFinishedToReadFolder && fullPath.rfind("/Read/", 0) != 0) {
    const std::string oldCachePath = epub.getCachePath();
    const std::string dstPath = BookMoveUtils::buildReadFolderDestination(fullPath);
    LOG_INF("BookActions", "Moving completed epub: %s -> %s", fullPath.c_str(), dstPath.c_str());
    if (!Storage.rename(fullPath.c_str(), dstPath.c_str())) {
      LOG_ERR("BookActions", "Failed to move book to 'Read' folder");
      snprintf(APP_STATE.pendingAlertTitle, sizeof(APP_STATE.pendingAlertTitle), "%s",
               tr(STR_MOVE_TO_READ_FAILED_TITLE));
      snprintf(APP_STATE.pendingAlertBody, sizeof(APP_STATE.pendingAlertBody), tr(STR_MOVE_TO_READ_FAILED_BODY),
               displayName.c_str());
      APP_STATE.pendingAlertGoHomeOnBack.store(false, std::memory_order_relaxed);
      APP_STATE.hasPendingAlert.store(true, std::memory_order_release);
      return true;
    }

    BookMoveUtils::migrateMovedEpubState(fullPath, dstPath, oldCachePath, title, author,
                                         !SETTINGS.removeReadBooksFromRecents);
  }

  return true;
}

void drawToast(const GfxRenderer& renderer, const char* msg) {
  constexpr int toastPadX = 20;
  constexpr int toastPadY = 12;
  const int msgW = renderer.getTextWidth(UI_10_FONT_ID, msg);
  const int msgH = renderer.getLineHeight(UI_10_FONT_ID);
  const int toastW = msgW + toastPadX * 2;
  const int toastH = msgH + toastPadY * 2;
  const int toastX = (renderer.getScreenWidth() - toastW) / 2;
  const int toastY = (renderer.getScreenHeight() - toastH) / 2;
  renderer.fillRect(toastX, toastY, toastW, toastH, true);
  renderer.drawText(UI_10_FONT_ID, toastX + toastPadX, toastY + toastPadY, msg, false);
  renderer.displayBuffer();
}

}  // namespace BookActions
