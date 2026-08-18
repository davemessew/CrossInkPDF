#include "ReaderActivity.h"

#include <FsHelpers.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Memory.h>
#include <PdfHalReflowDocument.h>
#include <PdfSourceIdentity.h>

#ifdef SIMULATOR
#include <cstdlib>
#endif

#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "Epub.h"
#include "EpubReaderActivity.h"
#include "PdfPrepareActivity.h"
#include "ReaderRoute.h"
#include "SdCardFontSystem.h"
#include "Txt.h"
#include "TxtReaderActivity.h"
#include "Xtc.h"
#include "XtcReaderActivity.h"
#include "activities/util/BmpViewerActivity.h"
#include "activities/util/FullScreenMessageActivity.h"
#include "components/UITheme.h"
#include "util/BookMoveUtils.h"

bool ReaderActivity::isXtcFile(const std::string& path) { return FsHelpers::hasXtcExtension(path); }

bool ReaderActivity::isTxtFile(const std::string& path) {
  return FsHelpers::hasTxtExtension(path) ||
         FsHelpers::hasMarkdownExtension(path);  // Treat .md as txt files (until we have a markdown reader)
}

static bool isImagePreviewFile(const std::string& path) {
  return FsHelpers::hasBmpExtension(path) || FsHelpers::hasPngExtension(path);
}

bool ReaderActivity::shouldShowLoadingPopup(const std::string& path) {
  // Only first-open EPUBs are slow enough to need the popup (they build the
  // spine/TOC cache). A cached EPUB opens in ~ms, so showing the popup would
  // just add an extra full e-ink refresh (~3s on X3) before the reader paints
  // its first page; that page's own refresh is the visible "working" feedback.
  // Other formats, and EPUBs without a metadata cache yet, keep the popup.
  if (FsHelpers::hasPdfExtension(path)) {
    // PDF preparation owns its static progress screen; a separate popup would
    // add an unnecessary full e-ink refresh.
    return false;
  }
  if (isXtcFile(path) || isTxtFile(path) || isImagePreviewFile(path)) {
    return true;
  }
  return !Epub::hasCache(path, "/.crosspoint");
}

int ReaderActivity::initialRefreshCountdown() const {
  if (!allowFastInitialRefresh) return 0;

  const int refreshFrequency = SETTINGS.getRefreshFrequency();
  return refreshFrequency > 1 ? refreshFrequency : 2;
}

ReaderActivity::EpubOpenResult ReaderActivity::loadEpub(const std::string& path) {
  EpubOpenResult result;
  if (!Storage.exists(path.c_str())) {
    LOG_ERR("READER", "File does not exist: %s", path.c_str());
    return result;
  }

  auto epub = makeUniqueNoThrow<Epub>(path, "/.crosspoint");
  if (!epub) {
    LOG_ERR("READER", "Failed to allocate EPUB object");
    result.failure = Epub::OpenFailure::OutOfMemory;
    return result;
  }
  // First open: building the spine/TOC index (book.bin) takes a couple of seconds. Show the
  // indexing popup so it isn't a silent wait on the home screen. The cachePath/hash is known at
  // construction, so this check is valid before load(); a cached open loads in a blink -> no popup.
  const bool uncached = !Storage.exists((epub->getCachePath() + "/book.bin").c_str());
  if (uncached) {
    // The popup replaces the restored Quick Resume frame, so the reader must clean it.
    allowFastInitialRefresh = false;
    GUI.drawPopup(renderer, tr(STR_INDEXING));
  }
  // Keep one settings snapshot for both EPUB preparation and the reader handoff.
  result.readerSettings = EpubReaderActivity::readBookReaderSettings(*epub);
  // Lend the framebuffer's 48 KB for every EPUB load: even a cached book may
  // rebuild stale/missing CSS and need miniz's ~43 KB streaming workspace. The
  // panel keeps showing its last image, and the next activity redraws fully.
  GfxRenderer::FrameBufferLoan loan(renderer);
  const bool loaded = epub->load(true, result.readerSettings.readerSettings.embeddedStyle == 0);
  loan.end();
  if (loaded) {
    result.epub = std::move(epub);
    result.failure = Epub::OpenFailure::None;
    return result;
  }

  LOG_ERR("READER", "Failed to load epub");
  result.failure = epub->getLastLoadFailure();
  return result;
}

void ReaderActivity::queueEpubOpenAlert(const Epub::OpenFailure failure) {
  const bool outOfMemory = failure == Epub::OpenFailure::OutOfMemory;
  const char* title = outOfMemory ? tr(STR_MEMORY_ERROR) : tr(STR_INDEX_FAILED);
  const char* body = outOfMemory ? tr(STR_EPUB_OPEN_MEMORY_BODY) : tr(STR_EPUB_OPEN_FAILED_BODY);
  snprintf(APP_STATE.pendingAlertTitle, sizeof(APP_STATE.pendingAlertTitle), "%s", title);
  snprintf(APP_STATE.pendingAlertBody, sizeof(APP_STATE.pendingAlertBody), "%s", body);
  APP_STATE.pendingAlertGoHomeOnBack.store(false, std::memory_order_relaxed);
  APP_STATE.hasPendingAlert.store(true, std::memory_order_release);
}

std::unique_ptr<Xtc> ReaderActivity::loadXtc(const std::string& path) {
  if (!Storage.exists(path.c_str())) {
    LOG_ERR("READER", "File does not exist: %s", path.c_str());
    return nullptr;
  }

  auto xtc = makeUniqueNoThrow<Xtc>(path, "/.crosspoint");
  if (!xtc) {
    LOG_ERR("READER", "Failed to allocate XTC object");
    return nullptr;
  }
  if (xtc->load()) {
    return xtc;
  }

  LOG_ERR("READER", "Failed to load XTC");
  return nullptr;
}

std::unique_ptr<Txt> ReaderActivity::loadTxt(const std::string& path) {
  if (!Storage.exists(path.c_str())) {
    LOG_ERR("READER", "File does not exist: %s", path.c_str());
    return nullptr;
  }

  auto txt = makeUniqueNoThrow<Txt>(path, "/.crosspoint");
  if (!txt) {
    LOG_ERR("READER", "Failed to allocate TXT object");
    return nullptr;
  }
  if (txt->load()) {
    return txt;
  }

  LOG_ERR("READER", "Failed to load TXT");
  return nullptr;
}

void ReaderActivity::goToLibrary(const std::string& fromBookPath) {
  // If coming from a book, start in that book's folder; otherwise start from root
  auto initialPath = fromBookPath.empty() ? "/" : FsHelpers::extractFolderPath(fromBookPath);
  activityManager.goToFileBrowser(std::move(initialPath));
}

void ReaderActivity::onGoToReflowReader(std::unique_ptr<ReflowDocument> document) {
  currentBookPath = document->getPath();
  auto reader = makeUniqueNoThrow<EpubReaderActivity>(renderer, mappedInput, std::move(document));
  if (!reader) {
    LOG_ERR("READER", "Failed to allocate reflow reader");
    onGoBack();
    return;
  }
  activityManager.replaceActivity(std::move(reader));
}

void ReaderActivity::onGoToEpubReader(std::unique_ptr<Epub> epub,
                                      EpubReaderActivity::BookReaderSettingsData readerSettings) {
  const auto epubPath = epub->getPath();
  currentBookPath = epubPath;
  auto reader = makeUniqueNoThrow<EpubReaderActivity>(renderer, mappedInput, std::move(epub), readerSettings,
                                                       initialRefreshCountdown(), cleanImageBaseOnEntry);
  if (!reader) {
    LOG_ERR("READER", "Failed to allocate EPUB reader");
    onGoBack();
    return;
  }
  activityManager.replaceActivity(std::move(reader));
}

void ReaderActivity::onGoToBmpViewer(const std::string& path) {
  activityManager.replaceActivity(std::make_unique<BmpViewerActivity>(renderer, mappedInput, path));
}

void ReaderActivity::onGoToXtcReader(std::unique_ptr<Xtc> xtc) {
  const auto xtcPath = xtc->getPath();
  currentBookPath = xtcPath;
  activityManager.replaceActivity(
      std::make_unique<XtcReaderActivity>(renderer, mappedInput, std::move(xtc), initialRefreshCountdown()));
}

void ReaderActivity::onGoToTxtReader(std::unique_ptr<Txt> txt) {
  const auto txtPath = txt->getPath();
  currentBookPath = txtPath;
  activityManager.replaceActivity(
      std::make_unique<TxtReaderActivity>(renderer, mappedInput, std::move(txt), initialRefreshCountdown()));
}

bool ReaderActivity::openLibraryRoute() {
  goToLibrary();
  return true;
}

bool ReaderActivity::openImageRoute() {
  onGoToBmpViewer(initialBookPath);
  return true;
}

bool ReaderActivity::openXtcRoute() {
  auto xtc = loadXtc(initialBookPath);
  if (!xtc) {
    onGoBack();
    return false;
  }
  onGoToXtcReader(std::move(xtc));
  return true;
}

bool ReaderActivity::openTextRoute() {
  auto txt = loadTxt(initialBookPath);
  if (!txt) {
    onGoBack();
    return false;
  }
  onGoToTxtReader(std::move(txt));
  return true;
}

bool ReaderActivity::openPdfRoute() {
#if defined(CROSSINK_ENABLE_PDF) && CROSSINK_ENABLE_PDF
  const uint64_t normalCacheHash = pdfPathHash64(initialBookPath.c_str(), initialBookPath.size());
  uint64_t resolvedCacheHash = normalCacheHash;
  bool readOnlyFallback = true;
  const bool cacheResolved =
      BookMoveUtils::migrationCacheHash(initialBookPath, normalCacheHash, &resolvedCacheHash, &readOnlyFallback);
  if (!cacheResolved || readOnlyFallback) {
    LOG_ERR("READER", "PDF open blocked by read-only migration cache state");
    onGoBack();
    return false;
  }
  if (!Storage.exists(initialBookPath.c_str())) {
    LOG_ERR("READER", "PDF file does not exist: %s", initialBookPath.c_str());
    onGoBack();
    return false;
  }
  const uint64_t* const cacheHashOverride = resolvedCacheHash == normalCacheHash ? nullptr : &resolvedCacheHash;
  PdfStatus status{};
  auto document =
      loadPdfHalReflowDocumentNoThrow(initialBookPath.c_str(), "/.crosspoint", &status, cacheHashOverride);
#ifdef SIMULATOR
  const char* const injectedCacheErrorBook = std::getenv("CROSSINK_SIMULATOR_PDF_CACHE_ERROR_BOOK");
  if (injectedCacheErrorBook != nullptr && initialBookPath == injectedCacheErrorBook) {
    document.reset();
    status = PdfStatus::failure(PdfError::IoFailure);
  }
#endif
  if (document) {
    onGoToReflowReader(std::move(document));
    return true;
  }
  if (status.error == PdfError::InsufficientMemory || status.error == PdfError::IoFailure ||
      status.error == PdfError::InvalidArgument) {
    LOG_ERR("READER", "PDF cache check failed before preparation: error=%u", static_cast<unsigned>(status.error));
    auto errorActivity =
        makeUniqueNoThrow<PdfPrepareActivity>(renderer, mappedInput, initialBookPath, status);
    if (!errorActivity) {
      LOG_ERR("READER", "Failed to allocate PDF error activity");
      onGoBack();
      return false;
    }
    activityManager.replaceActivity(std::move(errorActivity));
    return true;
  }
  auto preparation = makeUniqueNoThrow<PdfPrepareActivity>(renderer, mappedInput, initialBookPath);
  if (!preparation) {
    LOG_ERR("READER", "Failed to allocate PDF preparation activity");
    onGoBack();
    return false;
  }
  activityManager.replaceActivity(std::move(preparation));
  return true;
#else
  LOG_ERR("READER", "PDF reader is disabled in this firmware build");
  onGoBack();
  return false;
#endif
}

bool ReaderActivity::openEpubRoute() {
  auto result = loadEpub(initialBookPath);
  if (!result.epub) {
    queueEpubOpenAlert(result.failure);
    onGoBack();
    return false;
  }
  onGoToEpubReader(std::move(result.epub), std::move(result.readerSettings));
  return true;
}

bool ReaderActivity::dispatchLibrary(void* context) {
  return static_cast<ReaderActivity*>(context)->openLibraryRoute();
}
bool ReaderActivity::dispatchImage(void* context) { return static_cast<ReaderActivity*>(context)->openImageRoute(); }
bool ReaderActivity::dispatchXtc(void* context) { return static_cast<ReaderActivity*>(context)->openXtcRoute(); }
bool ReaderActivity::dispatchText(void* context) { return static_cast<ReaderActivity*>(context)->openTextRoute(); }
bool ReaderActivity::dispatchPdf(void* context) { return static_cast<ReaderActivity*>(context)->openPdfRoute(); }
bool ReaderActivity::dispatchEpub(void* context) { return static_cast<ReaderActivity*>(context)->openEpubRoute(); }

void ReaderActivity::onEnter() {
  Activity::onEnter();

  if (suppressInitialBackRelease) {
    mappedInput.suppressNextBackRelease();
  }

  const ReaderRoute route = selectReaderRoute(initialBookPath);

  if (route != ReaderRoute::Library && shouldShowLoadingPopup(initialBookPath)) {
    GUI.drawPopup(renderer, tr(STR_LOADING_POPUP));
  }

  if (route != ReaderRoute::Library && route != ReaderRoute::Image) {
    sdFontSystem.ensureLoaded(renderer);
  }

  currentBookPath = initialBookPath;
  const ReaderRouteHandlers handlers{
      this, dispatchLibrary, dispatchImage, dispatchXtc, dispatchText, dispatchPdf, dispatchEpub,
  };
  (void)dispatchReaderRoute(initialBookPath, handlers);
}

void ReaderActivity::onGoBack() { finish(); }
