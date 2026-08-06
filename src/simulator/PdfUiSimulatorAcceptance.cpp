#ifdef SIMULATOR

#include "PdfUiSimulatorAcceptance.h"

#include <HalStorage.h>
#include <PdfTypes.h>
#include <PdfCacheIo.h>
#include <PdfCacheStore.h>
#include <PdfHalCacheIo.h>
#include <PdfSourceIdentity.h>

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "BookmarkStore.h"
#include "ClippingStore.h"
#include "I18nKeys.h"
#include "MappedInputManager.h"
#include "activities/Activity.h"
#include "activities/ActivityManager.h"
#include "activities/reader/PdfPrepareAcceptanceObserver.h"
#include "fontIds.h"

extern ActivityManager activityManager;
extern GfxRenderer renderer;
extern MappedInputManager mappedInputManager;

namespace {

using Button = MappedInputManager::Button;

constexpr char kEnableVariable[] = "CROSSINK_SIMULATOR_PDF_UI_ACCEPTANCE";
constexpr char kBookVariable[] = "CROSSINK_SIMULATOR_PDF_UI_BOOK";
constexpr char kEarlyErrorBookVariable[] = "CROSSINK_SIMULATOR_PDF_UI_EARLY_ERROR_BOOK";
constexpr char kWarningBackBookVariable[] = "CROSSINK_SIMULATOR_PDF_UI_WARNING_BACK_BOOK";
constexpr char kWarningBookVariable[] = "CROSSINK_SIMULATOR_PDF_UI_WARNING_BOOK";
constexpr char kEncryptedErrorBookVariable[] = "CROSSINK_SIMULATOR_PDF_UI_ENCRYPTED_ERROR_BOOK";
constexpr char kErrorBookVariable[] = "CROSSINK_SIMULATOR_PDF_UI_ERROR_BOOK";
constexpr char kNonceVariable[] = "CROSSINK_SIMULATOR_PDF_UI_NONCE";
constexpr char kCacheDirectory[] = "/.crosspoint";
constexpr char kResetMarker[] = "SIM_PDF_UI_RESET ";
constexpr char kEventMarker[] = "SIM_PDF_UI_EVENT ";
constexpr char kResultMarker[] = "SIM_PDF_UI_RESULT ";
constexpr char kPassMarker[] = "PDF_SIMULATOR_UI_ACCEPTANCE_PASS";
constexpr uint32_t kStateTickLimit = 20000;
constexpr int kBrowserSettleFrames = 3;
constexpr int kInputSettleFrames = 3;
constexpr const char* kCheckpointNames[] = {"build.a", "build.b"};

constexpr Button kConfirm[] = {Button::Confirm};
constexpr Button kBack[] = {Button::Back};
constexpr Button kPageTurn[] = {Button::PageForward};
constexpr Button kContentsNavigation[] = {
    Button::Confirm, Button::Down, Button::Confirm, Button::Down, Button::Confirm,
};
constexpr Button kAddBookmark[] = {
    Button::Confirm, Button::Confirm, Button::Down, Button::Down, Button::Confirm,
};
constexpr Button kAddClipping[] = {
    Button::Confirm, Button::Confirm, Button::Down, Button::Confirm, Button::Confirm, Button::Right, Button::Confirm,
};

enum class Step : uint8_t {
  Start,
  WaitHome,
  SelectUncached,
  WaitUncachedSelection,
  WaitPrepareVisible,
  WaitBeforeCancel,
  WaitCancelled,
  WaitHomeAfterCancel,
  SelectResume,
  WaitResumeSelection,
  WaitResumePrepare,
  WaitReader,
  WaitPageTurn,
  WaitContentsNavigation,
  WaitBookmark,
  WaitClipping,
  WaitProgressHome,
  SelectCached,
  WaitCachedSelection,
  WaitCachedReader,
  WaitHomeBeforeEarlyError,
  SelectEarlyError,
  WaitEarlyErrorSelection,
  WaitEarlyError,
  WaitHomeBeforeWarning,
  SelectWarningBack,
  WaitWarningBackSelection,
  WaitWarningBack,
  WaitWarningBackHome,
  SelectWarning,
  WaitWarningSelection,
  WaitWarning,
  WaitWarningReader,
  WaitHomeBeforeWarningCached,
  SelectWarningCached,
  WaitWarningCachedSelection,
  WaitWarningCachedReader,
  WaitHomeBeforeEncryptedError,
  SelectEncryptedError,
  WaitEncryptedErrorSelection,
  WaitEncryptedError,
  WaitHomeBeforeError,
  SelectError,
  WaitErrorSelection,
  WaitError,
  Complete,
};

enum class InputPhase : uint8_t { Press, Release, Settle };

std::string containingDirectory(const char* const path) {
  const std::string value(path);
  const size_t separator = value.find_last_of('/');
  if (separator == std::string::npos || separator == 0) {
    return "/";
  }
  return value.substr(0, separator);
}

bool isBoundNonce(const char* const nonce) {
  if (nonce == nullptr || std::strlen(nonce) != 32) {
    return false;
  }
  for (const char* cursor = nonce; *cursor != '\0'; ++cursor) {
    if (std::isxdigit(static_cast<unsigned char>(*cursor)) == 0) {
      return false;
    }
  }
  return true;
}

class PdfUiSimulatorAcceptance {
 public:
  void tick() {
    mappedInputManager.simulatorClearInputFrame();
    if (inputButtons_ != nullptr) {
      runInputSequence();
      return;
    }
    if (delayFrames_ > 0) {
      --delayFrames_;
      return;
    }
    if (++stateTicks_ > kStateTickLimit) {
      fail("timed out in UI acceptance step %u", static_cast<unsigned>(step_));
    }
    runStep();
  }

 private:
  Step step_ = Step::Start;
  uint32_t stateTicks_ = 0;
  int delayFrames_ = 0;
  const Button* inputButtons_ = nullptr;
  size_t inputButtonCount_ = 0;
  size_t inputButtonIndex_ = 0;
  InputPhase inputPhase_ = InputPhase::Press;
  int inputSettleFrames_ = 0;
  Step afterInput_ = Step::Start;

  const char* bookPath_ = nullptr;
  const char* earlyErrorBookPath_ = nullptr;
  const char* warningBackBookPath_ = nullptr;
  const char* warningBookPath_ = nullptr;
  const char* encryptedErrorBookPath_ = nullptr;
  const char* errorBookPath_ = nullptr;
  const char* nonce_ = nullptr;
  std::string bookDirectory_;
  std::string earlyErrorBookDirectory_;
  std::string warningBackBookDirectory_;
  std::string warningBookDirectory_;
  std::string encryptedErrorBookDirectory_;
  std::string errorBookDirectory_;
  size_t eventCount_ = 0;
  size_t bookmarkCountBefore_ = 0;
  size_t clippingCountBefore_ = 0;
  uint64_t prepareFrameHash_ = 0;
  uint64_t prepareDetailHash_ = 0;
  uint64_t cancelledFrameHash_ = 0;
  uint64_t resumedFrameHash_ = 0;
  uint64_t earlyErrorFrameHash_ = 0;
  uint64_t warningBackFrameHash_ = 0;
  uint64_t warningFrameHash_ = 0;
  uint64_t encryptedErrorFrameHash_ = 0;
  // Simulator-only durable-checkpoint inspection storage. Keeping the 4 KiB
  // fingerprint workspace here avoids stack pressure and per-tick heap churn.
  PdfHalCacheIoContext checkpointIoContext_{};
  PdfCacheStore checkpointStore_{};
  uint8_t checkpointIdentityWorkspace_[PDF_SOURCE_FINGERPRINT_BYTES]{};
  PdfBuildCheckpoint cancelledCheckpoint_{};
  ScreenshotInfo readerOpenPosition_{};
  ScreenshotInfo pageTurnPosition_{};
  ScreenshotInfo contentsPosition_{};
  ScreenshotInfo savedPosition_{};
  ScreenshotInfo warningPosition_{};

  template <typename... Args>
  [[noreturn]] static void fail(const char* const format, Args... args) {
    std::fprintf(stderr, "PDF UI simulator acceptance failed: ");
    if constexpr (sizeof...(Args) == 0) {
      std::fputs(format, stderr);
    } else {
      std::fprintf(stderr, format, args...);
    }
    std::fprintf(stderr, "\n");
    std::fflush(stderr);
    std::_Exit(2);
  }

  void transition(const Step next) {
    step_ = next;
    stateTicks_ = 0;
  }

  template <size_t Count>
  void startInputSequence(const Button (&buttons)[Count], const Step afterInput) {
    inputButtons_ = buttons;
    inputButtonCount_ = Count;
    inputButtonIndex_ = 0;
    inputPhase_ = InputPhase::Press;
    inputSettleFrames_ = 0;
    afterInput_ = afterInput;
    stateTicks_ = 0;
  }

  void runInputSequence() {
    const Button button = inputButtons_[inputButtonIndex_];
    switch (inputPhase_) {
      case InputPhase::Press:
        mappedInputManager.simulatorInjectPress(button);
        inputPhase_ = InputPhase::Release;
        return;
      case InputPhase::Release:
        mappedInputManager.simulatorInjectRelease(button);
        inputPhase_ = InputPhase::Settle;
        inputSettleFrames_ = kInputSettleFrames;
        return;
      case InputPhase::Settle:
        if (inputSettleFrames_ > 0) {
          --inputSettleFrames_;
          return;
        }
        ++inputButtonIndex_;
        if (inputButtonIndex_ < inputButtonCount_) {
          inputPhase_ = InputPhase::Press;
          return;
        }
        inputButtons_ = nullptr;
        inputButtonCount_ = 0;
        inputButtonIndex_ = 0;
        transition(afterInput_);
        return;
    }
  }

  bool currentBookIs(const char* const expected) const { return activityManager.getCurrentBookPath() == expected; }

  static bool samePosition(const ScreenshotInfo& left, const ScreenshotInfo& right) {
    return left.spineIndex == right.spineIndex && left.currentPage == right.currentPage &&
           left.totalPages == right.totalPages && left.progressPercent == right.progressPercent;
  }

  static bool usablePdfPosition(const ScreenshotInfo& info) {
    return info.readerType == ScreenshotInfo::ReaderType::Pdf && info.spineIndex >= 0 && info.currentPage > 0 &&
           info.totalPages > 0;
  }

  uint64_t renderFrame() const {
    if (activityManager.requestUpdateAndWait() != RequestUpdateResult::Rendered) {
      fail("render request was rejected");
    }
    // Observe the renderer-owned buffer in place. The acceptance harness never borrows,
    // copies, writes, or reallocates framebuffer storage.
    const uint8_t* const frameBuffer = renderer.getFrameBuffer();
    const size_t frameBufferSize = renderer.getBufferSize();
    if (frameBuffer == nullptr || frameBufferSize == 0) {
      fail("renderer has no framebuffer");
    }
    uint64_t hash = 14695981039346656037ULL;
    for (size_t index = 0; index < frameBufferSize; ++index) {
      hash ^= frameBuffer[index];
      hash *= 1099511628211ULL;
    }
    return hash;
  }

  uint64_t hashPreparationDetail() const {
    const uint8_t* const frameBuffer = renderer.getFrameBuffer();
    const size_t frameBufferSize = renderer.getBufferSize();
    const int logicalWidth = renderer.getScreenWidth();
    const int logicalHeight = renderer.getScreenHeight();
    const int detailTop = logicalHeight / 2 - 6;
    const int detailHeight = renderer.getLineHeight(UI_10_FONT_ID);
    const int panelWidth = renderer.getDisplayWidth();
    const int panelHeight = renderer.getDisplayHeight();
    const int panelWidthBytes = renderer.getDisplayWidthBytes();
    if (frameBuffer == nullptr || frameBufferSize == 0 || logicalWidth <= 0 || logicalHeight <= 0 || detailTop < 0 ||
        detailHeight <= 0 || detailTop + detailHeight > logicalHeight || panelWidth <= 0 || panelHeight <= 0 ||
        panelWidthBytes <= 0) {
      fail("cannot observe preparation detail pixels");
    }

    // Hash only the logical rows occupied by the preparation detail text. This
    // reads renderer-owned pixels in place and excludes the independently
    // changing progress line below it.
    uint64_t hash = 14695981039346656037ULL;
    for (int logicalY = detailTop; logicalY < detailTop + detailHeight; ++logicalY) {
      for (int logicalX = 0; logicalX < logicalWidth; ++logicalX) {
        int physicalX = 0;
        int physicalY = 0;
        switch (renderer.getOrientation()) {
          case GfxRenderer::Portrait:
            physicalX = logicalY;
            physicalY = panelHeight - 1 - logicalX;
            break;
          case GfxRenderer::LandscapeClockwise:
            physicalX = panelWidth - 1 - logicalX;
            physicalY = panelHeight - 1 - logicalY;
            break;
          case GfxRenderer::PortraitInverted:
            physicalX = panelWidth - 1 - logicalY;
            physicalY = logicalX;
            break;
          case GfxRenderer::LandscapeCounterClockwise:
            physicalX = logicalX;
            physicalY = logicalY;
            break;
        }
        const size_t byteIndex =
            static_cast<size_t>(physicalY) * static_cast<size_t>(panelWidthBytes) + static_cast<size_t>(physicalX / 8);
        if (physicalX < 0 || physicalX >= panelWidth || physicalY < 0 || physicalY >= panelHeight ||
            byteIndex >= frameBufferSize) {
          fail("preparation detail pixels exceeded framebuffer bounds");
        }
        const uint8_t bit = static_cast<uint8_t>((frameBuffer[byteIndex] >> (7 - (physicalX % 8))) & 1U);
        hash ^= bit;
        hash *= 1099511628211ULL;
      }
    }
    return hash;
  }

  bool checkpointExists() const {
    char root[PDF_CACHE_PATH_CAPACITY]{};
    if (!pdfFormatCacheRoot(kCacheDirectory, bookPath_, root, sizeof(root))) {
      return false;
    }
    char path[PDF_CACHE_PATH_CAPACITY]{};
    for (const char* const leaf : kCheckpointNames) {
      const int length = std::snprintf(path, sizeof(path), "%s/%s", root, leaf);
      if (length > 0 && static_cast<size_t>(length) < sizeof(path) && Storage.exists(path)) {
        return true;
      }
    }
    return false;
  }

  bool loadDurableCheckpoint(PdfBuildCheckpoint& checkpoint) {
    const PdfCacheIo io = pdfHalCacheIo(checkpointIoContext_);
    PdfSourceIdentity source{};
    PdfStatus status = pdfComputeSourceIdentity(io, bookPath_, checkpointIdentityWorkspace_,
                                                sizeof(checkpointIdentityWorkspace_), &source);
    char root[PDF_CACHE_PATH_CAPACITY]{};
    if (!status || !pdfFormatCacheRoot(kCacheDirectory, bookPath_, root, sizeof(root))) {
      return false;
    }
    status = checkpointStore_.initialize(io, root);
    PdfBuildCheckpointSelection selection{};
    if (status) {
      status = checkpointStore_.loadCheckpointSlots(source, &selection);
    }
    if (!status || !selection.selected) {
      return false;
    }
    checkpoint = selection.checkpoint;
    return checkpoint.phase == PdfBuildPhase::Cancelled && checkpoint.resumePhase == PdfBuildResumePhase::AfterPage &&
           checkpoint.lastVerifiedPage > 0 && checkpoint.generation != 0 && checkpoint.journalBytes != 0;
  }

  void emitSimple(const char* const name) {
    std::printf("%s{\"schema_version\":1,\"runner_nonce\":\"%s\",\"event\":\"%s\"}\n", kEventMarker, nonce_, name);
    ++eventCount_;
    std::fflush(stdout);
  }

  void emitFrame(const char* const name, const uint64_t hash) {
    std::printf("%s{\"schema_version\":1,\"runner_nonce\":\"%s\",\"event\":\"%s\",\"frame_hash\":\"%016llX\"}\n",
                kEventMarker, nonce_, name, static_cast<unsigned long long>(hash));
    ++eventCount_;
    std::fflush(stdout);
  }

  void emitEncryptedErrorFrame(const uint64_t hash, const PdfError error,
                               const StrId translationKey) {
    const char* const errorReceipt =
        error == PdfError::Encrypted ? "PdfError::Encrypted" : "Unexpected";
    const char* const translationReceipt =
        translationKey == StrId::STR_PDF_ENCRYPTED ? "STR_PDF_ENCRYPTED"
                                                   : "Unexpected";
    std::printf(
        "%s{\"schema_version\":1,\"runner_nonce\":\"%s\",\"event\":\"encrypted_error_visible\","
        "\"frame_hash\":\"%016llX\",\"pdf_error\":\"%s\",\"translation_key\":\"%s\"}\n",
        kEventMarker, nonce_, static_cast<unsigned long long>(hash),
        errorReceipt, translationReceipt);
    ++eventCount_;
    std::fflush(stdout);
  }

  void emitCheckpointFrame(const char* const name, const uint64_t hash, const PdfBuildCheckpoint& checkpoint) {
    std::printf(
        "%s{\"schema_version\":1,\"runner_nonce\":\"%s\",\"event\":\"%s\",\"frame_hash\":\"%016llX\","
        "\"checkpoint_exists\":true,\"resume_phase\":\"after_page\",\"last_verified_page\":%lu,"
        "\"generation\":%lu}\n",
        kEventMarker, nonce_, name, static_cast<unsigned long long>(hash),
        static_cast<unsigned long>(checkpoint.lastVerifiedPage), static_cast<unsigned long>(checkpoint.generation));
    ++eventCount_;
    std::fflush(stdout);
  }

  void emitDetailFrame(const char* const name, const uint64_t frameHash, const uint64_t detailHash,
                       const bool checkpointExists) {
    std::printf(
        "%s{\"schema_version\":1,\"runner_nonce\":\"%s\",\"event\":\"%s\",\"frame_hash\":\"%016llX\","
        "\"detail_hash\":\"%016llX\",\"checkpoint_exists\":%s}\n",
        kEventMarker, nonce_, name, static_cast<unsigned long long>(frameHash),
        static_cast<unsigned long long>(detailHash), checkpointExists ? "true" : "false");
    ++eventCount_;
    std::fflush(stdout);
  }

  void emitResumedDetailFrame(const uint64_t frameHash, const uint64_t detailHash,
                              const PdfBuildCheckpoint& checkpoint) {
    std::printf(
        "%s{\"schema_version\":1,\"runner_nonce\":\"%s\",\"event\":\"prepare_resumed\","
        "\"frame_hash\":\"%016llX\",\"detail_hash\":\"%016llX\",\"checkpoint_exists\":true,"
        "\"resume_phase\":\"after_page\",\"last_verified_page\":%lu,\"generation\":%lu}\n",
        kEventMarker, nonce_, static_cast<unsigned long long>(frameHash), static_cast<unsigned long long>(detailHash),
        static_cast<unsigned long>(checkpoint.lastVerifiedPage), static_cast<unsigned long>(checkpoint.generation));
    ++eventCount_;
    std::fflush(stdout);
  }

  void emitPosition(const char* const name, const ScreenshotInfo& info) {
    std::printf(
        "%s{\"schema_version\":1,\"runner_nonce\":\"%s\",\"event\":\"%s\",\"spine\":%d,\"page\":%d,"
        "\"pages\":%d,\"progress\":%d}\n",
        kEventMarker, nonce_, name, info.spineIndex, info.currentPage, info.totalPages, info.progressPercent);
    ++eventCount_;
    std::fflush(stdout);
  }

  void emitPositionFrame(const char* const name, const ScreenshotInfo& info, const uint64_t hash) {
    std::printf(
        "%s{\"schema_version\":1,\"runner_nonce\":\"%s\",\"event\":\"%s\",\"frame_hash\":\"%016llX\","
        "\"spine\":%d,\"page\":%d,\"pages\":%d,\"progress\":%d}\n",
        kEventMarker, nonce_, name, static_cast<unsigned long long>(hash), info.spineIndex, info.currentPage,
        info.totalPages, info.progressPercent);
    ++eventCount_;
    std::fflush(stdout);
  }

  void emitCount(const char* const name, const size_t before, const size_t after) {
    std::printf("%s{\"schema_version\":1,\"runner_nonce\":\"%s\",\"event\":\"%s\",\"before\":%u,\"after\":%u}\n",
                kEventMarker, nonce_, name, static_cast<unsigned>(before), static_cast<unsigned>(after));
    ++eventCount_;
    std::fflush(stdout);
  }

  void openBookDirectory(const std::string& directory, const Step selectStep) {
    activityManager.goToFileBrowser(directory);
    delayFrames_ = kBrowserSettleFrames;
    transition(selectStep);
  }

  void runStep() {
    switch (step_) {
      case Step::Start:
        bookPath_ = std::getenv(kBookVariable);
        earlyErrorBookPath_ = std::getenv(kEarlyErrorBookVariable);
        warningBackBookPath_ = std::getenv(kWarningBackBookVariable);
        warningBookPath_ = std::getenv(kWarningBookVariable);
        encryptedErrorBookPath_ = std::getenv(kEncryptedErrorBookVariable);
        errorBookPath_ = std::getenv(kErrorBookVariable);
        nonce_ = std::getenv(kNonceVariable);
        if (bookPath_ == nullptr || bookPath_[0] == '\0' || earlyErrorBookPath_ == nullptr ||
            earlyErrorBookPath_[0] == '\0' || warningBackBookPath_ == nullptr || warningBackBookPath_[0] == '\0' ||
            warningBookPath_ == nullptr || warningBookPath_[0] == '\0' || encryptedErrorBookPath_ == nullptr ||
            encryptedErrorBookPath_[0] == '\0' || errorBookPath_ == nullptr || errorBookPath_[0] == '\0' ||
            !isBoundNonce(nonce_)) {
          fail("missing or invalid UI acceptance environment");
        }
        if (!Storage.exists(bookPath_) || !Storage.exists(earlyErrorBookPath_) ||
            !Storage.exists(warningBackBookPath_) || !Storage.exists(warningBookPath_) ||
            !Storage.exists(encryptedErrorBookPath_) || !Storage.exists(errorBookPath_)) {
          fail("staged UI fixture is missing");
        }
        bookDirectory_ = containingDirectory(bookPath_);
        earlyErrorBookDirectory_ = containingDirectory(earlyErrorBookPath_);
        warningBackBookDirectory_ = containingDirectory(warningBackBookPath_);
        warningBookDirectory_ = containingDirectory(warningBookPath_);
        encryptedErrorBookDirectory_ = containingDirectory(encryptedErrorBookPath_);
        errorBookDirectory_ = containingDirectory(errorBookPath_);
        std::printf("%s{\"schema_version\":1,\"runner_nonce\":\"%s\"}\n", kResetMarker, nonce_);
        std::fflush(stdout);
        activityManager.goHome();
        transition(Step::WaitHome);
        return;

      case Step::WaitHome:
        if (!activityManager.isHomeActivity()) {
          return;
        }
        emitFrame("home", renderFrame());
        openBookDirectory(bookDirectory_, Step::SelectUncached);
        return;

      case Step::SelectUncached:
        startInputSequence(kConfirm, Step::WaitUncachedSelection);
        return;

      case Step::WaitUncachedSelection:
        if (!currentBookIs(bookPath_)) {
          return;
        }
        if (activityManager.getScreenshotInfo().readerType == ScreenshotInfo::ReaderType::Pdf) {
          fail("uncached PDF reached the reader before cancellation");
        }
        emitSimple("uncached_file_selected");
        transition(Step::WaitPrepareVisible);
        return;

      case Step::WaitPrepareVisible:
        if (!currentBookIs(bookPath_)) {
          fail("uncached PDF route lost its source path");
        }
        if (!activityManager.skipLoopDelay()) {
          if (activityManager.getScreenshotInfo().readerType == ScreenshotInfo::ReaderType::Pdf) {
            fail("uncached PDF completed before its prepare screen was observed");
          }
          return;
        }
        prepareFrameHash_ = renderFrame();
        prepareDetailHash_ = hashPreparationDetail();
        emitDetailFrame("prepare_visible", prepareFrameHash_, prepareDetailHash_, false);
        transition(Step::WaitBeforeCancel);
        return;

      case Step::WaitBeforeCancel:
        if (activityManager.getScreenshotInfo().readerType == ScreenshotInfo::ReaderType::Pdf) {
          fail("uncached PDF completed before cancellation input");
        }
        if (!checkpointExists()) {
          return;
        }
        startInputSequence(kBack, Step::WaitCancelled);
        return;

      case Step::WaitCancelled:
        if (!currentBookIs(bookPath_) || activityManager.skipLoopDelay() || activityManager.isReaderActivity()) {
          return;
        }
        if (!loadDurableCheckpoint(cancelledCheckpoint_)) {
          fail("cancelled preparation left no durable page-resume checkpoint");
        }
        cancelledFrameHash_ = renderFrame();
        emitCheckpointFrame("cancelled_checkpoint", cancelledFrameHash_, cancelledCheckpoint_);
        startInputSequence(kConfirm, Step::WaitHomeAfterCancel);
        return;

      case Step::WaitHomeAfterCancel:
        if (!activityManager.isHomeActivity()) {
          return;
        }
        emitSimple("home_after_cancel");
        openBookDirectory(bookDirectory_, Step::SelectResume);
        return;

      case Step::SelectResume:
        startInputSequence(kConfirm, Step::WaitResumeSelection);
        return;

      case Step::WaitResumeSelection:
        if (!currentBookIs(bookPath_)) {
          return;
        }
        emitSimple("resume_file_selected");
        transition(Step::WaitResumePrepare);
        return;

      case Step::WaitResumePrepare: {
        if (!currentBookIs(bookPath_)) {
          fail("resumed PDF route lost its source path");
        }
        if (!activityManager.skipLoopDelay()) {
          if (activityManager.getScreenshotInfo().readerType == ScreenshotInfo::ReaderType::Pdf) {
            fail("resumed PDF completed before resume detail text was observed");
          }
          return;
        }
        if (!checkpointExists()) {
          fail("resumed preparation did not observe its checkpoint");
        }
        resumedFrameHash_ = renderFrame();
        const uint64_t resumedDetailHash = hashPreparationDetail();
        if (resumedDetailHash == prepareDetailHash_) {
          return;
        }
        emitResumedDetailFrame(resumedFrameHash_, resumedDetailHash, cancelledCheckpoint_);
        transition(Step::WaitReader);
        return;
      }

      case Step::WaitReader: {
        ScreenshotInfo info = activityManager.getScreenshotInfo();
        if (!usablePdfPosition(info)) {
          return;
        }
        const uint64_t hash = renderFrame();
        info = activityManager.getScreenshotInfo();
        if (!usablePdfPosition(info)) {
          return;
        }
        readerOpenPosition_ = info;
        emitPositionFrame("reader_open", info, hash);
        startInputSequence(kPageTurn, Step::WaitPageTurn);
        return;
      }

      case Step::WaitPageTurn: {
        if (activityManager.getScreenshotInfo().readerType != ScreenshotInfo::ReaderType::Pdf) {
          return;
        }
        const uint64_t hash = renderFrame();
        const ScreenshotInfo info = activityManager.getScreenshotInfo();
        if (!usablePdfPosition(info) || samePosition(info, readerOpenPosition_)) {
          return;
        }
        pageTurnPosition_ = info;
        emitPositionFrame("page_turned", info, hash);
        startInputSequence(kContentsNavigation, Step::WaitContentsNavigation);
        return;
      }

      case Step::WaitContentsNavigation: {
        if (activityManager.getScreenshotInfo().readerType != ScreenshotInfo::ReaderType::Pdf) {
          return;
        }
        const uint64_t hash = renderFrame();
        const ScreenshotInfo info = activityManager.getScreenshotInfo();
        if (!usablePdfPosition(info) || info.spineIndex == pageTurnPosition_.spineIndex) {
          return;
        }
        contentsPosition_ = info;
        emitPositionFrame("contents_navigated", info, hash);
        bookmarkCountBefore_ = BOOKMARKS.getBookmarks().size();
        startInputSequence(kAddBookmark, Step::WaitBookmark);
        return;
      }

      case Step::WaitBookmark: {
        const size_t after = BOOKMARKS.getBookmarks().size();
        if (activityManager.getScreenshotInfo().readerType != ScreenshotInfo::ReaderType::Pdf ||
            after <= bookmarkCountBefore_) {
          return;
        }
        emitCount("bookmark_added", bookmarkCountBefore_, after);
        clippingCountBefore_ = CLIPPINGS.getClippings().size();
        startInputSequence(kAddClipping, Step::WaitClipping);
        return;
      }

      case Step::WaitClipping: {
        const size_t after = CLIPPINGS.getClippings().size();
        if (activityManager.getScreenshotInfo().readerType != ScreenshotInfo::ReaderType::Pdf ||
            after <= clippingCountBefore_) {
          return;
        }
        emitCount("clipping_added", clippingCountBefore_, after);
        savedPosition_ = activityManager.getScreenshotInfo();
        if (!usablePdfPosition(savedPosition_)) {
          fail("reader position disappeared after clipping");
        }
        startInputSequence(kBack, Step::WaitProgressHome);
        return;
      }

      case Step::WaitProgressHome:
        if (!activityManager.isHomeActivity()) {
          return;
        }
        emitPosition("progress_saved", savedPosition_);
        openBookDirectory(bookDirectory_, Step::SelectCached);
        return;

      case Step::SelectCached:
        startInputSequence(kConfirm, Step::WaitCachedSelection);
        return;

      case Step::WaitCachedSelection:
        if (!currentBookIs(bookPath_)) {
          return;
        }
        emitSimple("cached_file_selected");
        transition(Step::WaitCachedReader);
        return;

      case Step::WaitCachedReader: {
        if (activityManager.skipLoopDelay()) {
          fail("cached reopen entered PdfPrepare");
        }
        ScreenshotInfo info = activityManager.getScreenshotInfo();
        if (!usablePdfPosition(info)) {
          return;
        }
        const uint64_t hash = renderFrame();
        info = activityManager.getScreenshotInfo();
        if (!samePosition(info, savedPosition_)) {
          fail("cached reopen did not restore saved progress");
        }
        emitPositionFrame("cached_reopen", info, hash);
        startInputSequence(kBack, Step::WaitHomeBeforeEarlyError);
        return;
      }

      case Step::WaitHomeBeforeEarlyError:
        if (!activityManager.isHomeActivity()) {
          return;
        }
        emitSimple("home_before_early_error");
        openBookDirectory(earlyErrorBookDirectory_, Step::SelectEarlyError);
        return;

      case Step::SelectEarlyError:
        startInputSequence(kConfirm, Step::WaitEarlyErrorSelection);
        return;

      case Step::WaitEarlyErrorSelection:
        if (!currentBookIs(earlyErrorBookPath_)) {
          return;
        }
        emitSimple("early_error_file_selected");
        transition(Step::WaitEarlyError);
        return;

      case Step::WaitEarlyError:
        if (!currentBookIs(earlyErrorBookPath_) || activityManager.skipLoopDelay() ||
            activityManager.isReaderActivity()) {
          return;
        }
        earlyErrorFrameHash_ = renderFrame();
        if (earlyErrorFrameHash_ == prepareFrameHash_ || earlyErrorFrameHash_ == cancelledFrameHash_ ||
            earlyErrorFrameHash_ == resumedFrameHash_) {
          fail("early cache error screen did not render distinct pixels");
        }
        emitFrame("early_error_visible", earlyErrorFrameHash_);
        startInputSequence(kBack, Step::WaitHomeBeforeWarning);
        return;

      case Step::WaitHomeBeforeWarning:
        if (!activityManager.isHomeActivity()) {
          return;
        }
        emitSimple("home_before_warning");
        openBookDirectory(warningBackBookDirectory_, Step::SelectWarningBack);
        return;

      case Step::SelectWarningBack:
        startInputSequence(kConfirm, Step::WaitWarningBackSelection);
        return;

      case Step::WaitWarningBackSelection:
        if (!currentBookIs(warningBackBookPath_)) {
          return;
        }
        emitSimple("warning_back_file_selected");
        transition(Step::WaitWarningBack);
        return;

      case Step::WaitWarningBack:
        if (!currentBookIs(warningBackBookPath_) || activityManager.skipLoopDelay() ||
            activityManager.isReaderActivity()) {
          return;
        }
        warningBackFrameHash_ = renderFrame();
        if (warningBackFrameHash_ == earlyErrorFrameHash_ || warningBackFrameHash_ == prepareFrameHash_) {
          fail("optional-content warning Back screen did not render distinct pixels");
        }
        emitFrame("warning_back_visible", warningBackFrameHash_);
        startInputSequence(kBack, Step::WaitWarningBackHome);
        return;

      case Step::WaitWarningBackHome:
        if (!activityManager.isHomeActivity()) {
          return;
        }
        emitSimple("warning_back_home");
        openBookDirectory(warningBookDirectory_, Step::SelectWarning);
        return;

      case Step::SelectWarning:
        startInputSequence(kConfirm, Step::WaitWarningSelection);
        return;

      case Step::WaitWarningSelection:
        if (!currentBookIs(warningBookPath_)) {
          return;
        }
        emitSimple("warning_file_selected");
        transition(Step::WaitWarning);
        return;

      case Step::WaitWarning:
        if (!currentBookIs(warningBookPath_) || activityManager.skipLoopDelay() || activityManager.isReaderActivity()) {
          return;
        }
        warningFrameHash_ = renderFrame();
        if (warningFrameHash_ == earlyErrorFrameHash_ || warningFrameHash_ == prepareFrameHash_) {
          fail("optional-content warning did not render distinct pixels");
        }
        emitFrame("warning_visible", warningFrameHash_);
        startInputSequence(kConfirm, Step::WaitWarningReader);
        return;

      case Step::WaitWarningReader: {
        ScreenshotInfo info = activityManager.getScreenshotInfo();
        if (!usablePdfPosition(info)) {
          return;
        }
        const uint64_t hash = renderFrame();
        info = activityManager.getScreenshotInfo();
        if (!usablePdfPosition(info)) {
          return;
        }
        if (hash == warningFrameHash_) {
          fail("warning continue did not render the reader");
        }
        warningPosition_ = info;
        emitPositionFrame("warning_reader_open", info, hash);
        startInputSequence(kBack, Step::WaitHomeBeforeWarningCached);
        return;
      }

      case Step::WaitHomeBeforeWarningCached:
        if (!activityManager.isHomeActivity()) {
          return;
        }
        emitSimple("home_before_warning_cached");
        openBookDirectory(warningBookDirectory_, Step::SelectWarningCached);
        return;

      case Step::SelectWarningCached:
        startInputSequence(kConfirm, Step::WaitWarningCachedSelection);
        return;

      case Step::WaitWarningCachedSelection:
        if (!currentBookIs(warningBookPath_)) {
          return;
        }
        emitSimple("warning_cached_file_selected");
        transition(Step::WaitWarningCachedReader);
        return;

      case Step::WaitWarningCachedReader: {
        if (activityManager.skipLoopDelay()) {
          fail("warning cached reopen entered PdfPrepare");
        }
        ScreenshotInfo info = activityManager.getScreenshotInfo();
        if (!usablePdfPosition(info)) {
          return;
        }
        const uint64_t hash = renderFrame();
        info = activityManager.getScreenshotInfo();
        if (!samePosition(info, warningPosition_)) {
          fail("warning cached reopen did not restore reader position");
        }
        emitPositionFrame("warning_cached_reopen", info, hash);
        startInputSequence(kBack, Step::WaitHomeBeforeEncryptedError);
        return;
      }

      case Step::WaitHomeBeforeEncryptedError:
        if (!activityManager.isHomeActivity()) {
          return;
        }
        emitSimple("home_before_encrypted_error");
        openBookDirectory(encryptedErrorBookDirectory_, Step::SelectEncryptedError);
        return;

      case Step::SelectEncryptedError:
        startInputSequence(kConfirm, Step::WaitEncryptedErrorSelection);
        return;

      case Step::WaitEncryptedErrorSelection:
        if (!currentBookIs(encryptedErrorBookPath_)) {
          return;
        }
        emitSimple("encrypted_error_file_selected");
        transition(Step::WaitEncryptedError);
        return;

      case Step::WaitEncryptedError:
        if (!currentBookIs(encryptedErrorBookPath_) || activityManager.skipLoopDelay() ||
            activityManager.isReaderActivity()) {
          return;
        }
        encryptedErrorFrameHash_ = renderFrame();
        if (encryptedErrorFrameHash_ == prepareFrameHash_ || encryptedErrorFrameHash_ == cancelledFrameHash_ ||
            encryptedErrorFrameHash_ == resumedFrameHash_ || encryptedErrorFrameHash_ == earlyErrorFrameHash_ ||
            encryptedErrorFrameHash_ == warningBackFrameHash_ || encryptedErrorFrameHash_ == warningFrameHash_) {
          fail("encrypted PDF translated error screen did not render distinct pixels");
        }
        PdfPrepareAcceptanceObservation observation{};
        if (!pdfObserveActivePrepareFailure(&observation)) {
          fail("encrypted PDF screen has no live PdfPrepare failure observation");
        }
        emitEncryptedErrorFrame(encryptedErrorFrameHash_, observation.error, observation.translationKey);
        startInputSequence(kBack, Step::WaitHomeBeforeError);
        return;

      case Step::WaitHomeBeforeError:
        if (!activityManager.isHomeActivity()) {
          return;
        }
        emitSimple("home_before_error");
        openBookDirectory(errorBookDirectory_, Step::SelectError);
        return;

      case Step::SelectError:
        startInputSequence(kConfirm, Step::WaitErrorSelection);
        return;

      case Step::WaitErrorSelection:
        if (!currentBookIs(errorBookPath_)) {
          return;
        }
        emitSimple("error_file_selected");
        transition(Step::WaitError);
        return;

      case Step::WaitError:
        if (!currentBookIs(errorBookPath_) || activityManager.skipLoopDelay() || activityManager.isReaderActivity()) {
          return;
        }
        {
          const uint64_t hash = renderFrame();
          if (hash == prepareFrameHash_ || hash == cancelledFrameHash_ || hash == resumedFrameHash_ ||
              hash == earlyErrorFrameHash_ || hash == warningBackFrameHash_ || hash == warningFrameHash_ ||
              hash == encryptedErrorFrameHash_) {
            fail("translated error screen did not render distinct pixels");
          }
          emitFrame("error_visible", hash);
          transition(Step::Complete);
        }
        return;

      case Step::Complete:
        emitSimple("complete");
        std::printf("%s{\"schema_version\":1,\"runner_nonce\":\"%s\",\"event_count\":%u,\"completed\":true}\n",
                    kResultMarker, nonce_, static_cast<unsigned>(eventCount_));
        std::printf("%s\n", kPassMarker);
        std::fflush(stdout);
        std::_Exit(0);
    }
  }
};

PdfUiSimulatorAcceptance acceptance;

}  // namespace

bool pdfUiSimulatorAcceptanceEnabled() { return std::getenv(kEnableVariable) != nullptr; }

void runPdfUiSimulatorAcceptanceTick() { acceptance.tick(); }

#endif
