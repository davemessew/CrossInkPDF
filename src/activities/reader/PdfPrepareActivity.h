#pragma once

#include <PdfHalCacheIo.h>
#include <PdfPreparation.h>
#include <ReflowDocument.h>

#include <memory>
#include <string>
#include <utility>

#include "activities/Activity.h"
#include "PdfPrepareAcceptanceObserver.h"

class PdfPrepareActivity final : public Activity {
 public:
  PdfPrepareActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string sourcePath,
                     PdfStatus initialFailure = PdfStatus::success())
      : Activity("PdfPrepare", renderer, mappedInput),
        sourcePath_(std::move(sourcePath)),
        initialFailure_(initialFailure) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&& lock) override;
  bool skipLoopDelay() override;
  bool preventAutoSleep() override;
  std::string getCurrentBookPath() const override { return sourcePath_; }

#if defined(SIMULATOR) || defined(CROSSINK_QEMU)
  bool acceptanceObserveFailure(PdfPrepareAcceptanceObservation* observation) const;
#endif

 private:
  enum class State : uint8_t {
    Preparing,
    Warning,
    Paused,
    Failed,
  };

  static uint32_t nowMs(void* context);
  static PdfResourceSnapshot measureResources(void* context);
  static void resourceEvent(void* context, const PdfResourceEvent& event);
  static const char* errorMessage(PdfError error);

  void finishPreparation();
  void openPreparedDocument(std::unique_ptr<ReflowDocument> document);
  void setFailure(PdfStatus status);

  std::string sourcePath_;
  PdfHalCacheIoContext ioContext_{};
  std::unique_ptr<PdfPreparation> preparation_;
  std::unique_ptr<ReflowDocument> pendingDocument_;
  PdfPreparationPaintGate paintGate_;
  State state_ = State::Preparing;
  PdfStatus initialFailure_{};
  PdfStatus failure_{};
};
