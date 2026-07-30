#include "ReaderActivity.h"

#include <FsHelpers.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Memory.h>
#include <PdfHalReflowDocument.h>

#include "CrossPointSettings.h"
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

std::unique_ptr<Epub> ReaderActivity::loadEpub(const std::string& path) {
  if (!Storage.exists(path.c_str())) {
    LOG_ERR("READER", "File does not exist: %s", path.c_str());
    return nullptr;
  }

  auto epub = makeUniqueNoThrow<Epub>(path, "/.crosspoint");
  if (!epub) {
    LOG_ERR("READER", "Failed to allocate EPUB object");
    return nullptr;
  }
  // First open: building the spine/TOC index (book.bin) takes a couple of seconds. Show the
  // indexing popup so it isn't a silent wait on the home screen. The cachePath/hash is known at
  // construction, so this check is valid before load(); a cached open loads in a blink -> no popup.
  if (!Storage.exists((epub->getCachePath() + "/book.bin").c_str())) {
    GUI.drawPopup(renderer, tr(STR_INDEXING));
  }
  if (epub->load(true, SETTINGS.embeddedStyle == 0)) {
    return epub;
  }

  LOG_ERR("READER", "Failed to load epub");
  return nullptr;
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

void ReaderActivity::onGoToEpubReader(std::unique_ptr<Epub> epub) { onGoToReflowReader(std::move(epub)); }

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

void ReaderActivity::onGoToBmpViewer(const std::string& path) {
  activityManager.replaceActivity(std::make_unique<BmpViewerActivity>(renderer, mappedInput, path));
}

void ReaderActivity::onGoToXtcReader(std::unique_ptr<Xtc> xtc) {
  const auto xtcPath = xtc->getPath();
  currentBookPath = xtcPath;
  activityManager.replaceActivity(std::make_unique<XtcReaderActivity>(renderer, mappedInput, std::move(xtc)));
}

void ReaderActivity::onGoToTxtReader(std::unique_ptr<Txt> txt) {
  const auto txtPath = txt->getPath();
  currentBookPath = txtPath;
  activityManager.replaceActivity(std::make_unique<TxtReaderActivity>(renderer, mappedInput, std::move(txt)));
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
  PdfStatus status{};
  auto document = loadPdfHalReflowDocumentNoThrow(initialBookPath.c_str(), "/.crosspoint", &status);
  if (document) {
    onGoToReflowReader(std::move(document));
    return true;
  }
  if (status.error == PdfError::InsufficientMemory || status.error == PdfError::IoFailure ||
      status.error == PdfError::InvalidArgument) {
    LOG_ERR("READER", "PDF cache check failed before preparation: error=%u", static_cast<unsigned>(status.error));
    onGoBack();
    return false;
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
  auto epub = loadEpub(initialBookPath);
  if (!epub) {
    onGoBack();
    return false;
  }
  onGoToEpubReader(std::move(epub));
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
