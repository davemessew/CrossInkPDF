#pragma once
#include <ReflowDocument.h>

#include <memory>

#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

class EpubReaderChapterSelectionActivity final : public Activity {
  std::shared_ptr<ReflowDocument> document;
  ButtonNavigator buttonNavigator;
  int currentSpineIndex = 0;
  int selectorIndex = 0;

  // Number of items that fit on a page, derived from logical screen height.
  // This adapts automatically when switching between portrait and landscape.
  int getPageItems() const;

  // Total TOC items count
  int getTotalItems() const;

 public:
  explicit EpubReaderChapterSelectionActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                              const std::shared_ptr<ReflowDocument>& document,
                                              const int currentSpineIndex)
      : Activity("EpubReaderChapterSelection", renderer, mappedInput),
        document(document),
        currentSpineIndex(currentSpineIndex) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool isReaderActivity() const override { return true; }
  bool allowPowerAsConfirmInReaderMode() const override { return true; }
};
