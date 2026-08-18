#pragma once
#include <memory>

#include "EpubReaderActivity.h"
#include "activities/Activity.h"
#include "activities/home/FileBrowserActivity.h"

class Epub;
class ReflowDocument;
class Xtc;
class Txt;

class ReaderActivity final : public Activity {
  struct EpubOpenResult {
    std::unique_ptr<Epub> epub;
    EpubReaderActivity::BookReaderSettingsData readerSettings;
    Epub::OpenFailure failure = Epub::OpenFailure::InvalidOrUnreadable;
  };

  std::string initialBookPath;
  std::string currentBookPath;  // Track current book path for navigation
  bool suppressInitialBackRelease = false;
  bool allowFastInitialRefresh = false;
  bool cleanImageBaseOnEntry = false;
  // Non-static (unlike the other loaders): draws the first-open indexing popup, which needs the renderer.
  EpubOpenResult loadEpub(const std::string& path);
  static std::unique_ptr<Xtc> loadXtc(const std::string& path);
  static std::unique_ptr<Txt> loadTxt(const std::string& path);
  static bool isXtcFile(const std::string& path);
  static bool isTxtFile(const std::string& path);
  // Whether to paint the "Loading" popup on entry. Skipped for already-cached
  // EPUBs, whose open is ~ms and would otherwise cost an extra full e-ink
  // refresh before the reader paints its first page.
  static bool shouldShowLoadingPopup(const std::string& path);

  void goToLibrary(const std::string& fromBookPath = "");
  void onGoToEpubReader(std::unique_ptr<Epub> epub, EpubReaderActivity::BookReaderSettingsData readerSettings);
  void onGoToReflowReader(std::unique_ptr<ReflowDocument> document);
  void onGoToXtcReader(std::unique_ptr<Xtc> xtc);
  void onGoToTxtReader(std::unique_ptr<Txt> txt);
  void onGoToBmpViewer(const std::string& path);
  void queueEpubOpenAlert(Epub::OpenFailure failure);

  bool openLibraryRoute();
  bool openImageRoute();
  bool openXtcRoute();
  bool openTextRoute();
  bool openPdfRoute();
  bool openEpubRoute();
  static bool dispatchLibrary(void* context);
  static bool dispatchImage(void* context);
  static bool dispatchXtc(void* context);
  static bool dispatchText(void* context);
  static bool dispatchPdf(void* context);
  static bool dispatchEpub(void* context);

  void onGoBack();
  int initialRefreshCountdown() const;

 public:
  explicit ReaderActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string initialBookPath,
                          bool suppressInitialBackRelease = false, bool allowFastInitialRefresh = false,
                          bool cleanImageBaseOnEntry = false)
      : Activity("Reader", renderer, mappedInput),
        initialBookPath(std::move(initialBookPath)),
        suppressInitialBackRelease(suppressInitialBackRelease),
        allowFastInitialRefresh(allowFastInitialRefresh),
        cleanImageBaseOnEntry(cleanImageBaseOnEntry) {}
  void onEnter() override;
  bool isReaderActivity() const override { return true; }
};
