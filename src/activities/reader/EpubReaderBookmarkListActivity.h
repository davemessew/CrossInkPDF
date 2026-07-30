#pragma once

#include <vector>

#include "../Activity.h"
#include "BookmarkStore.h"
#include "util/ButtonNavigator.h"

class EpubReaderBookmarkListActivity final : public Activity {
 public:
  using DeleteCallback = bool (*)(void* context, uint16_t itemId);

  explicit EpubReaderBookmarkListActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                           const std::vector<Bookmark>& bookmarks,
                                           DeleteCallback deleteCallback = nullptr, void* deleteContext = nullptr)
      : Activity("EpubReaderBookmarkList", renderer, mappedInput),
        bookmarks(bookmarks),
        deleteCallback(deleteCallback),
        deleteContext(deleteContext) {}

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool isReaderActivity() const override { return true; }
  bool allowPowerAsConfirmInReaderMode() const override { return true; }

 private:
  std::vector<Bookmark> bookmarks;
  int selectedIndex = 0;
  bool longPressConfirmHandled = false;
  DeleteCallback deleteCallback = nullptr;
  void* deleteContext = nullptr;
  ButtonNavigator buttonNavigator;

  void deleteSelectedBookmark();
  void showBookmarkActionMenu(bool ignoreInitialConfirmRelease = false);
  int getPageItems() const;
};
