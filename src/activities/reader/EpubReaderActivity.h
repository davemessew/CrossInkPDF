#pragma once
#include <Epub/FootnoteEntry.h>
#include <Epub/Section.h>
#include <ReflowDocument.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <array>
#include <atomic>
#include <memory>
#include <optional>
#include <string>

#include "BookReadingStats.h"
#include "BookmarkStore.h"
#include "EndOfBookOptions.h"
#include "EpubReaderMenuActivity.h"
#include "GlobalReadingStats.h"
#include "PdfReaderProgressState.h"
#include "PdfSavedItemsSession.h"
#include "activities/Activity.h"

struct ToastRect {
  int x = 0;
  int y = 0;
  int w = 0;
  int h = 0;
};

class EpubReaderActivity final : public Activity {
 public:
  bool usesFullScreenReaderVerticalSwipes() const override { return true; }

  struct ReaderSettingsSnapshot {
    uint8_t fontFamily = 0;
    uint8_t fontSize = 0;
    uint8_t lineHeightPercent = 100;
    uint8_t wordSpacing = 0;
    uint8_t orientation = 0;
    uint8_t screenMargin = 5;
    uint8_t publisherPageNumbers = 0;
    uint8_t paragraphAlignment = 0;
    uint8_t embeddedStyle = 1;
    uint8_t hyphenationEnabled = 0;
    uint8_t textAntiAliasing = 1;
    uint8_t readerDarkMode = 0;
    uint8_t imageRendering = 0;
    uint8_t extraParagraphSpacing = 1;
    uint8_t forceParagraphIndents = 0;
    uint8_t bionicReadingEnabled = 0;
    uint8_t guideReadingEnabled = 0;
    uint8_t epubRenderMode = 0;
    uint8_t indexingMethod = CrossPointSettings::INDEXING_FULL_SECTION;
    char sdFontFamilyName[64] = "";
  };

 private:
  std::shared_ptr<ReflowDocument> document;
  std::unique_ptr<Section> section = nullptr;
  int currentSpineIndex = 0;
  int nextPageNumber = 0;
  int activeSectionFontId = 0;
  std::optional<uint16_t> pendingPageJump;
  // Set when navigating to a footnote href with a fragment (e.g. #note1).
  // Cleared on the next render after the new section loads and resolves it to a page.
  std::string pendingAnchor;
  std::string pendingFootnotePreviewAnchor;
  bool activeFootnotePreview = false;
  int pagesUntilFullRefresh = 0;
  int cachedSpineIndex = 0;
  int cachedChapterPageNumber = 0;
  int cachedChapterTotalPageCount = 0;
  int cachedChapterPageWatermark = 0;
  struct ChapterGroupEstimateCache {
    int currentSpineIndex = -1;
    int firstSpineIndex = -1;
    int lastSpineIndex = -1;
    uint32_t settingsSignature = 0;
    uint32_t knownSiblingPages = 0;
    uint32_t knownSiblingBytes = 0;
    uint32_t unknownSiblingBytes = 0;
    uint32_t precedingKnownPages = 0;
    uint32_t precedingUnknownBytes = 0;
    uint16_t unknownSiblingCount = 0;
    uint16_t precedingUnknownCount = 0;
    bool siblingEstimateUsed = false;
    bool valid = false;
  } chapterGroupEstimate;
  bool pendingRelayoutReposition = false;
  uint16_t cachedPageParagraphIndex = UINT16_MAX;
  uint16_t cachedPageParagraphOffset = 0;
  uint16_t cachedPageParagraphSpan = 0;
  struct PdfReaderSessionState;
  // The complete PDF-only state, including its 128 fixed saved-item records,
  // is allocated once on PDF entry. Ordinary EPUB readers carry only this
  // pointer and perform no PDF-state allocation.
  std::unique_ptr<PdfReaderSessionState> pdfReaderSession;
#if UINTPTR_MAX == UINT32_MAX
  static_assert(sizeof(pdfReaderSession) == 4, "RV32 PDF reader-state handle must remain one pointer");
#endif
  unsigned long lastPageTurnTime = 0UL;
  unsigned long pageTurnDuration = 0UL;
  unsigned long pageShownAtMs = 0UL;
  bool paceSampleWarmupPending = true;
  uint32_t sessionPaceSampleSeconds = 0;
  uint16_t sessionPaceSampleCount = 0;
  uint32_t sessionReadingSeconds = 0;
  uint16_t lastAutoPageTurnIntervalSeconds = 0;
  bool bookHasCustomReaderSettings = false;
  bool bookHasAutoPageTurnInterval = false;
  bool bookHasRenderModeOverride = false;
  bool restoreGlobalReaderSettingsOnExit = false;
  ReaderSettingsSnapshot globalReaderSettingsBeforeBook;
  bool bookReaderSettingsSuspendedForGlobalEdit = false;
  ReaderSettingsSnapshot suspendedBookReaderSettings;
  BookReadingStats stats;
  GlobalReadingStats globalStats;
  ReadingStatsDateTime sessionStartLocalDateTime;
  bool hasSessionStartLocalDateTime = false;
  // Signals that the next render should reposition within the newly loaded section
  // based on a cross-book percentage jump.
  bool pendingPercentJump = false;
  // Normalized 0.0-1.0 progress within the target spine item, computed from book percentage.
  float pendingSpineProgress = 0.0f;
  uint16_t pendingParagraphIndex = UINT16_MAX;
  uint16_t pendingClippingIndex = UINT16_MAX;
  bool pendingScreenshot = false;
  bool pendingSyncSaveError = false;
  bool skipNextButtonCheck = false;  // Skip button processing for one frame after subactivity exit
  bool automaticPageTurnActive = false;
  bool longPressMenuHandled = false;
  bool longPressBackHandled = false;
  bool longPowerButtonHandled = false;
  bool sideButtonLongPressHandled = false;
  bool frontButtonLongPressHandled = false;
  bool touchDictionaryLookupHandled = false;
  int pageLoadRetryCount = 0;
  enum class BookmarkFeedbackType : uint8_t {
    Added,
    Removed,
    LimitReached,
  };
  bool pendingBookmarkFeedback = false;
  BookmarkFeedbackType bookmarkFeedbackType = BookmarkFeedbackType::Added;
  unsigned long bookmarkFeedbackShowTime = 0UL;
  bool pendingCompletedFeedback = false;
  bool completedFeedbackIsFinished = false;
  unsigned long completedFeedbackShowTime = 0UL;
  bool pendingTiltPageTurnFeedback = false;
  bool tiltPageTurnFeedbackEnabled = false;
  unsigned long tiltPageTurnFeedbackShowTime = 0UL;
  bool pendingRenderModeToast = false;
  bool renderModeToastShown = false;
  bool pendingSafeModeToast = false;
  bool safeModeToastShown = false;
  uint8_t renderModeToastMode = 0;
  unsigned long renderModeToastShowTime = 0UL;
  std::unique_ptr<uint8_t[]> renderModeToastRegionBuffer;
  size_t renderModeToastRegionBufferSize = 0;
  ToastRect renderModeToastRegion;
  bool renderModeToastRegionSaved = false;
  int completionTriggerSpineIndex = -1;
  float completionTriggerSpineProgress = 1.0f;
  bool completionPromptQueued = false;
  bool completionPromptShown = false;
  bool completionTriggerSeenBelow = false;
  bool completionTriggerCrossed = false;
  bool lastAtOrPastCompletionTrigger = false;

  // Tracks whether this book is currently removed from Recent Books by the
  // removeReadBooksFromRecents feature (set at End-of-Book, cleared if paged back in).
  bool recentsEntryRemoved = false;
  // Set when the reader is left at end-of-book and SETTINGS.moveFinishedToReadFolder is on.
  // Consumed in onExit() to relocate the finished book into /Read/.
  bool pendingReadFolderMove = false;
  // Next-book suggestion menu for the End-of-Book screen
  EndOfBookOptions endOfBookOptions;

  // Footnote support
  std::vector<FootnoteEntry> currentPageFootnotes;
#if CROSSINK_APP_CAP_TOUCH
  struct FootnoteTouchTarget {
    int16_t x = 0;
    int16_t y = 0;
    int16_t width = 0;
    int16_t height = 0;
  };
  std::array<FootnoteTouchTarget, EPUB_MAX_FOOTNOTES_PER_PAGE> currentPageFootnoteTouchTargets{};
#endif
  struct SavedPosition {
    int spineIndex;
    int pageNumber;
    PdfExactReadingOrigin exactPdfOrigin;
  };
  static_assert(sizeof(SavedPosition) <= 56, "footnote return position exceeded its bounded allocation");
  static constexpr int MAX_FOOTNOTE_DEPTH = 3;
  SavedPosition savedPositions[MAX_FOOTNOTE_DEPTH] = {};
  int footnoteDepth = 0;

  // Viewport of the last render(), captured so loop()'s lazy partial-extension start
  // builds with identical layout parameters to the pages already rendered.
  uint16_t buildViewportWidth = 0;
  uint16_t buildViewportHeight = 0;
  // Set when the lazy extension start failed, so loop() does not retry every tick.
  bool partialRebuildStartFailed = false;
  // Set when a background extension build aborted for low heap. startBuild() still succeeds in
  // that state -- it is the layout inside buildSomeMore() that runs out of memory -- so without
  // this flag loop() would restart the same doomed build every tick, and skipLoopDelay() would
  // hold the loop at full speed while it did. The reader keeps the pages already laid out; a
  // build is only re-attempted from render() if the reader actually pages past the watermark.
  bool partialRebuildAbortedForLowMemory = false;
  // One-shot guard for the silent restart used only when a forward page turn reaches the first
  // unbuilt page after a confirmed low-memory partial-build abort.
  bool lowMemoryPartialRestartAttempted = false;
  bool backgroundBuildPausedForLowMemory = false;
  std::atomic<bool> sectionBuildCancelRequested{false};
  std::atomic<bool> goHomeAfterBuildCancel{false};

  // Last position successfully persisted by saveProgress, used to skip redundant
  // writeAtomic calls on no-op re-renders.
  int lastSavedSpineIndex = -1;
  int lastSavedPage = -1;
  int lastSavedPageCount = -1;

  void renderContents(std::unique_ptr<Page> page, int fontId, int orientedMarginTop, int orientedMarginRight,
                      int orientedMarginBottom, int orientedMarginLeft);
  void drawClippingHighlights(const Page& page, int fontId, int orientedMarginTop, int orientedMarginLeft) const;
  void renderStatusBar() const;
  void refreshChapterGroupEstimate(uint16_t viewportWidth, uint16_t viewportHeight);
  bool resolveChapterGroupPageProgress(int& currentPage, int& pageCount, float& chapterProgress,
                                       bool& pageCountEstimated) const;
  bool shouldUseFootnotePreview(int targetSpineIndex, const std::string& anchor) const;
  std::string footnotePreviewCacheSuffix(EpubRenderMode renderMode, const std::string& anchor) const;
  void clearFootnotePreviewState();
  bool captureSavedPosition(SavedPosition& savedPosition);
  void applySavedNavigationPosition(const SavedPosition& savedPosition);
  // Remap the cached relative reading position once the section's real page count is known
  // (used after a settings change re-paginates a chapter). Returns true if currentPage moved.
  bool isRelayoutCatchUpComplete() const;
  bool applyDeferredReposition();
  bool saveProgress(int spineIndex, int currentPage, int pageCount, bool force = false);
  void acceptPdfNavigation();
  bool cacheCurrentSectionPosition();
  void refreshCurrentPageSemanticRange();
  const ReflowPageSemanticRange* currentPdfPageSemanticRange() const;
  const PdfSavedItem* currentPdfPageBookmark() const;
  bool supportsSavedItems() const;
  bool initializePdfSavedItems();
  bool reloadPdfSavedItemsAfterMutation(PdfSavedItemsSessionResult result);
  bool applyPendingPdfSavedItemJump();
  static PdfStatus loadPdfSavedItems(void* context, PdfSavedItemsBuffer* output);
  static PdfStatus savePdfSavedItems(void* context, const PdfSavedItem* items, uint16_t count);
  static PdfStatus validatePdfSavedItem(void* context, const PdfSavedItem& item);
  static bool countPdfBookmarks(void* context, uint16_t* output);
  static bool readPdfBookmarkId(void* context, uint16_t index, uint16_t* output);
  static PdfSavedItemsLegacyMutationResult addPdfBookmark(void* context, uint16_t itemId);
  static PdfSavedItemsLegacyMutationResult removePdfBookmark(void* context, uint16_t itemId);
  static PdfSavedItemsLegacyMutationResult clearPdfBookmarks(void* context);
  static bool countPdfClippings(void* context, uint16_t* output);
  static bool readPdfClippingId(void* context, uint16_t index, uint16_t* output);
  static PdfSavedItemsLegacyMutationResult addPdfClipping(void* context, uint16_t itemId);
  static PdfSavedItemsLegacyMutationResult removePdfClipping(void* context, uint16_t itemId);
  static PdfSavedItemsLegacyMutationResult clearPdfClippings(void* context);
  static bool removePdfBookmarkFromList(void* context, uint16_t itemId);
  static bool removePdfClippingFromList(void* context, uint16_t itemId);
  void pauseReadingPaceTimer(const char* reason = "unknown");
  void resumeReadingPaceTimer(const char* reason = "unknown");
  void armReadingPaceWarmup(const char* reason = "unknown");
  bool forwardPageReadElapsed(uint32_t& seconds, const char* source) const;
  bool currentPageReadingSecondsForStats(uint32_t& seconds, const char* source) const;
  void recordCurrentPageReadingTime(const char* source = "unknown");
  void recordForwardPagePaceSample(uint32_t seconds, const char* source);
  bool getSessionAveragePaceSeconds(uint16_t& avgSeconds) const;
  void recoverStoredPaceFromSession(const char* reason = "unknown");
  bool getTimeLeftPaceSeconds(uint16_t& avgSeconds, const char*& source, uint16_t& sampleCount) const;
  bool estimateRemainingTimeLeftPages(bool bookEstimate, float& remainingPages) const;
  bool estimateProgressTimeLeftSeconds(uint32_t& seconds) const;
  bool estimateTimeLeftSeconds(bool bookEstimate, uint32_t& seconds) const;
  bool formatTimeLeftLabel(char* buf, size_t len) const;
  void refreshCachedTimeLeftEstimate();
  void applyBookStatsEditsFromDisk();
  void handleBookStatsReturn();
  void resetCurrentBookStatsAfterDelete();
  void openFileTransfer();
  void openAutoPageTurnIntervalPicker(bool ignoreInitialConfirmRelease = false);
  void startClipSelection();
  void resetReadingPaceData();
  void captureGlobalReaderSettings();
  void restoreGlobalReaderSettings();
  void loadBookReaderSettings();
  void saveCurrentBookReaderSettings();
  void saveGlobalSettingsPreservingBookOverrides();
  void beginGlobalSettingsEdit();
  void endGlobalSettingsEdit();
  static void saveReaderOptionsForBook(void* ctx);
  static void saveGlobalSettingsForBookReader(void* ctx);
  static void beginGlobalSettingsEditForBookReader(void* ctx);
  static void endGlobalSettingsEditForBookReader(void* ctx);
  // Jump to a percentage of the book (0-100), mapping it to spine and page.
  void jumpToPercent(int percent);
  void reindexCurrentSection();
  void executeReaderQuickAction(CrossPointSettings::LONG_PRESS_MENU_ACTION action);
  bool quickActionUsesConfirmRelease(CrossPointSettings::LONG_PRESS_MENU_ACTION action) const;
  bool quickActionUsesPowerRelease(CrossPointSettings::LONG_PRESS_MENU_ACTION action) const;
  void suppressConfirmShortcutRelease(CrossPointSettings::LONG_PRESS_MENU_ACTION action);
  void executeFootnoteQuickAction(bool suppressInitialPowerRelease = false);
#if CROSSINK_APP_CAP_TOUCH
  void buildFootnoteTouchTargets(const Page& page, int fontId, int orientedMarginTop, int orientedMarginLeft);
  bool handleTouchFootnoteLink(int touchX, int touchY);
#endif
  void suppressPowerShortcutRelease();
  bool consumeLongPowerButtonRelease();
  bool consumeLongPowerButtonHold();
  bool executeShortPowerButtonAction();
  bool executeLongPowerButtonAction();
  void handleClippingJump(const ClippingJumpResult& clipping);
  bool handleTouchDictionaryLookup();
  void openWordSelect(bool framebufferContainsPage, int initialTouchX = -1, int initialTouchY = -1,
                      bool autoLookupInitialWord = false);
  void onReaderMenuConfirm(EpubReaderMenuActivity::MenuAction action);
  // Opens the reader menu for the current position (short-press Confirm)
  void openReaderMenu();
  void applyOrientation(uint8_t orientation);
  void pageTurn(bool isForwardTurn, const char* source = "unknown");
  float getCurrentBookProgressPercent() const;
  void initializeCompletionPromptTrigger();
  bool isAtOrPastCompletionTrigger() const;
  bool shouldQueueCompletionPromptOnChapterExit() const;
  void queueCompletionPromptIfNeeded();
  void setBookCompleted(bool isCompleted);
  void showCompletedFeedback(bool isCompleted);
  void showTiltPageTurnFeedback(bool enabled);
  void showRenderModeToast(uint8_t renderMode);
  void showSafeModeToast();
  bool storeRenderModeToastRegion(const char* msg);
  void drawRenderModeToastBuffer(const char* msg);
  bool restoreRenderModeToastRegion();

  // Footnote navigation
  void navigateToHref(const std::string& href, bool savePosition = false, bool preferFootnotePreview = false);
  void restoreSavedPosition();

 public:
  explicit EpubReaderActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                              std::unique_ptr<ReflowDocument> document);
  ~EpubReaderActivity() override;
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&& lock) override;
  bool prepareManualRefresh() override {
    pagesUntilFullRefresh = 1;
    return true;
  }
  bool preventAutoSleep() override { return automaticPageTurnActive; }
  // Hold the loop hot only while the build has work this loop would do: a kept-alive
  // build sitting outside the lookahead window is dormant, and reporting it here would
  // pin the CPU at full clock (no power saving, yield-only loop) for the whole read.
  // Mirrors the tick condition in loop(): catch-up phase, or watermark inside the window.
  bool sectionBuildWantsTick() const {
    return section && section->isBuilding() &&
           (!section->activeBuildHasCaughtReadablePages() ||
            static_cast<int>(section->pageCount) < section->currentPage + BUILD_WINDOW_AHEAD);
  }
  bool backgroundSectionBuildHasHeap();
  bool skipLoopDelay() override { return sectionBuildWantsTick() && !backgroundBuildPausedForLowMemory; }
  bool isReaderActivity() const override { return true; }
  bool canSnapshotForSleepOverlay() const override { return true; }
  std::string getCurrentBookPath() const override { return document ? document->getPath() : std::string{}; }
  void setAutoPageTurnIntervalSeconds(uint16_t seconds);
  uint16_t getAutoPageTurnIntervalSeconds() const;

  // Renders the last saved page to the frame buffer without flushing to display.
  // Used by SleepActivity to prepare the background for the overlay sleep mode.
  // Returns false if the page cannot be loaded (missing cache / file error).
  static bool drawCurrentPageToBuffer(const std::string& filePath, GfxRenderer& renderer);
  static uint8_t loadBookRenderMode(const std::string& filePath);
  static bool saveBookRenderMode(const std::string& filePath, uint8_t renderMode);
  static bool resetBookReaderSettings(const std::string& filePath);
  ScreenshotInfo getScreenshotInfo() const override;
};
