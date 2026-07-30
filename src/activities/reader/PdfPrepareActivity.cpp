#include "PdfPrepareActivity.h"

#include <Arduino.h>
#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>
#include <Memory.h>
#include <PdfHalReflowDocument.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <cstdio>

#include "EpubReaderActivity.h"
#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {

constexpr char kCacheDirectory[] = "/.crosspoint";

uint32_t currentStackMargin() {
  // ESP-IDF reports this high-water mark in bytes (unlike upstream FreeRTOS).
  return static_cast<uint32_t>(uxTaskGetStackHighWaterMark(nullptr));
}

const char* resourceName(const PdfResourceKind kind) {
  switch (kind) {
    case PdfResourceKind::InflateDictionary:
      return "inflate_dictionary";
    case PdfResourceKind::SourceWindow:
      return "source_window";
    case PdfResourceKind::DecoderOutput:
      return "decoder_output";
    case PdfResourceKind::PageText:
      return "page_text";
    case PdfResourceKind::RunRecords:
      return "run_records";
    case PdfResourceKind::OperandScratch:
      return "operand_scratch";
  }
  return "unknown";
}

}  // namespace

uint32_t PdfPrepareActivity::nowMs(void*) { return millis(); }

PdfResourceSnapshot PdfPrepareActivity::measureResources(void*) {
  return {ESP.getFreeHeap(), ESP.getMaxAllocHeap(), currentStackMargin()};
}

void PdfPrepareActivity::resourceEvent(void*, const PdfResourceEvent& event) {
  LOG_DBG("PDF", "resource event=%u name=%s bytes=%u current=%u peak=%u free=%u max_alloc=%u stack=%u",
          static_cast<unsigned>(event.event), resourceName(event.resource), static_cast<unsigned>(event.bytes),
          static_cast<unsigned>(event.currentBytes), static_cast<unsigned>(event.peakBytes),
          static_cast<unsigned>(event.snapshot.freeHeap), static_cast<unsigned>(event.snapshot.largestBlock),
          static_cast<unsigned>(event.snapshot.stackMargin));
}

const char* PdfPrepareActivity::errorMessage(const PdfError error) {
  switch (error) {
    case PdfError::NoReadableText:
      return tr(STR_PDF_NO_READABLE_TEXT);
    case PdfError::Encrypted:
      return tr(STR_PDF_ENCRYPTED);
    case PdfError::UnsupportedFilter:
      return tr(STR_PDF_UNSUPPORTED_FILTER);
    case PdfError::UnsupportedEncoding:
      return tr(STR_PDF_UNSUPPORTED_ENCODING);
    case PdfError::InsufficientMemory:
      return tr(STR_PDF_INSUFFICIENT_MEMORY);
    case PdfError::InsufficientStorage:
      return tr(STR_PDF_INSUFFICIENT_STORAGE);
    case PdfError::Cancelled:
      return tr(STR_PDF_PREPARATION_PAUSED);
    case PdfError::ExpansionLimit:
    case PdfError::LimitExceeded:
    case PdfError::Malformed:
    case PdfError::InvalidOffset:
    case PdfError::UnexpectedEof:
      return tr(STR_PDF_DAMAGED_OR_UNSAFE);
    case PdfError::Unsupported:
      return tr(STR_PDF_UNSUPPORTED);
    default:
      return tr(STR_PDF_PREPARATION_FAILED);
  }
}

void PdfPrepareActivity::onEnter() {
  Activity::onEnter();
  preparation_ = makeUniqueNoThrow<PdfPreparation>();
  if (!preparation_) {
    setFailure(PdfStatus::failure(PdfError::InsufficientMemory));
    requestUpdate();
    return;
  }

  const PdfPreparationConfig config{
      pdfHalCacheIo(ioContext_),
      sourcePath_.c_str(),
      kCacheDirectory,
      this,
      nowMs,
      {this, measureResources, resourceEvent},
  };
  const PdfStatus status = preparation_->begin(config);
  if (!status) {
    setFailure(status);
  }
  requestUpdate();
}

void PdfPrepareActivity::onExit() {
  preparation_.reset();
  Activity::onExit();
}

bool PdfPrepareActivity::skipLoopDelay() { return state_ == State::Preparing && preparation_ != nullptr; }
bool PdfPrepareActivity::preventAutoSleep() { return state_ == State::Preparing && preparation_ != nullptr; }

void PdfPrepareActivity::setFailure(const PdfStatus status) {
  failure_ = status;
  state_ = status.error == PdfError::Cancelled ? State::Paused : State::Failed;
  preparation_.reset();
  requestUpdate();
}

void PdfPrepareActivity::finishPreparation() {
  const size_t peak = preparation_->resourcePeakBytes();
  const PdfResourceSnapshot resources = measureResources(this);
  LOG_INF("PDF", "PDF_RESOURCE_FINAL peak=%u free=%u max_alloc=%u stack_margin=%u", static_cast<unsigned>(peak),
          static_cast<unsigned>(resources.freeHeap), static_cast<unsigned>(resources.largestBlock),
          static_cast<unsigned>(resources.stackMargin));
  preparation_.reset();

  PdfStatus status{};
  auto document = loadPdfHalReflowDocumentNoThrow(sourcePath_.c_str(), kCacheDirectory, &status);
  if (!document) {
    setFailure(status);
    return;
  }
  std::unique_ptr<ReflowDocument> reflowDocument = std::move(document);
  auto reader = makeUniqueNoThrow<EpubReaderActivity>(renderer, mappedInput, std::move(reflowDocument));
  if (!reader) {
    setFailure(PdfStatus::failure(PdfError::InsufficientMemory));
    return;
  }
  activityManager.replaceActivity(std::move(reader));
}

void PdfPrepareActivity::loop() {
  if (state_ != State::Preparing || !preparation_) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Back) ||
        mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      finish();
    }
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    preparation_->requestCancel();
  }

  const PdfStepResult result = preparation_->step();
  if (result.complete()) {
    finishPreparation();
    return;
  }
  if (result.failed()) {
    setFailure(result.status);
    return;
  }
  if (paintGate_.shouldPaint(preparation_->progressPercent(), millis())) {
    requestUpdate();
  }
}

void PdfPrepareActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int width = renderer.getScreenWidth();
  const int height = renderer.getScreenHeight();
  renderer.clearScreen();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, width, metrics.headerHeight}, tr(STR_PDF_PREPARATION_TITLE));

  if (state_ == State::Preparing && preparation_) {
    renderer.drawCenteredText(UI_10_FONT_ID, height / 2 - 34, tr(STR_PDF_PREPARING), true, EpdFontFamily::BOLD);
    renderer.drawCenteredText(
        UI_10_FONT_ID, height / 2 - 6,
        preparation_->resumedFromCheckpoint() ? tr(STR_PDF_RESUMING) : tr(STR_PDF_PREPARING_DETAIL));
    char progress[24]{};
    std::snprintf(progress, sizeof(progress), "%u%%", static_cast<unsigned>(preparation_->progressPercent()));
    renderer.drawCenteredText(UI_10_FONT_ID, height / 2 + 22, progress);
    renderer.drawCenteredText(UI_10_FONT_ID, height / 2 + 48, tr(STR_PDF_CANCEL_RESUME));
    const auto labels = mappedInput.mapLabels(tr(STR_CANCEL), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  } else {
    renderer.drawCenteredText(UI_10_FONT_ID, height / 2 - 12, errorMessage(failure_.error), true, EpdFontFamily::BOLD);
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  }
  renderer.displayBuffer();
}
