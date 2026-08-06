#pragma once

#include <FreeInkApp.h>
#include <FreeInkUIGfxRenderer.h>

#include <array>
#include <atomic>
#include <string>
#include <vector>

#include "ClippingStore.h"
#include "activities/Activity.h"
#include "util/ButtonNavigator.h"

class EpubReaderClippingListActivity final : public Activity {
 public:
  using DeleteCallback = bool (*)(void* context, uint16_t itemId);

  EpubReaderClippingListActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                 std::vector<Clipping> clippings, DeleteCallback deleteCallback = nullptr,
                                 void* deleteContext = nullptr)
      : Activity("EpubClippingList", renderer, mappedInput),
        clippings(std::move(clippings)),
        deleteCallback(deleteCallback),
        deleteContext(deleteContext) {}

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  std::string detailText;
  std::vector<std::string> detailLines;
  ButtonNavigator buttonNavigator;
  int selectedIndex = 0;
  int detailPage = 0;
  int detailLayoutWidth = 0;
  int detailLinesPerPage = 0;
  bool longPressConfirmHandled = false;
  bool detailMode = false;
  DeleteCallback deleteCallback = nullptr;
  void* deleteContext = nullptr;

  static void listScreen(UiApp::ScreenType& screen, void* user);
  static void onRowEvent(const freeink::ui::ActionEvent& event, void* user);
  void buildListScreen(UiApp::ScreenType& screen);
  int getDetailTextWidth() const;
  int getDetailLinesPerPage() const;
  int getDetailPageCount() const;
  void deleteSelectedClipping();
  void closeDetail();
  void jumpToSelectedClipping();
  void openSelectedDetail();
  void rebuildDetailLayoutIfNeeded();
  void showClippingActionMenu(bool ignoreInitialConfirmRelease);
  void renderDetail();
};
