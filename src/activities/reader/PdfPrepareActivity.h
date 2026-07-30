#pragma once

#include <PdfHalCacheIo.h>
#include <PdfPreparation.h>

#include <memory>
#include <string>
#include <utility>

#include "activities/Activity.h"

class PdfPrepareActivity final : public Activity {
 public:
  PdfPrepareActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string sourcePath)
      : Activity("PdfPrepare", renderer, mappedInput), sourcePath_(std::move(sourcePath)) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&& lock) override;
  bool skipLoopDelay() override;
  bool preventAutoSleep() override;
  std::string getCurrentBookPath() const override { return sourcePath_; }

 private:
  enum class State : uint8_t {
    Preparing,
    Paused,
    Failed,
  };

  static uint32_t nowMs(void* context);
  static PdfResourceSnapshot measureResources(void* context);
  static void resourceEvent(void* context, const PdfResourceEvent& event);
  static const char* errorMessage(PdfError error);

  void finishPreparation();
  void setFailure(PdfStatus status);

  std::string sourcePath_;
  PdfHalCacheIoContext ioContext_{};
  std::unique_ptr<PdfPreparation> preparation_;
  PdfPreparationPaintGate paintGate_;
  State state_ = State::Preparing;
  PdfStatus failure_{};
};
