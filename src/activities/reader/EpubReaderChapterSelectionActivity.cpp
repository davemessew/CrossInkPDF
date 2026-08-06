#include "EpubReaderChapterSelectionActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include "MappedInputManager.h"
#include "components/TouchHeaderBackButton.h"
#include "components/UITheme.h"
#include "components/UIThemeTokens.h"
#include "components/UiAppHelpers.h"

namespace fui = freeink::ui;

namespace {
constexpr fui::ActionId ACTION_ROW = 1;
}

EpubReaderChapterSelectionActivity::EpubReaderChapterSelectionActivity(GfxRenderer& renderer,
                                                                       MappedInputManager& mappedInput,
                                                                       const std::shared_ptr<Epub>& epub,
                                                                       const std::string& epubPath,
                                                                       const int currentSpineIndex)
    : Activity("EpubReaderChapterSelection", renderer, mappedInput),
      epub(epub),
      epubPath(epubPath),
      currentSpineIndex(currentSpineIndex),
      uiTarget(makeUiTarget(renderer)),
      app(uiTarget, uiTarget.deviceContext()) {}

int EpubReaderChapterSelectionActivity::getTotalItems() const { return document ? document->getTocEntryCount() : 0; }

void EpubReaderChapterSelectionActivity::onEnter() {
  Activity::onEnter();

  if (!document) {
    return;
  }

  selectorIndex = document->getTocIndexForSectionIndex(currentSpineIndex);
  if (selectorIndex == -1) {
    selectorIndex = 0;
  }

  // Trigger first update
  requestUpdate();
}

void EpubReaderChapterSelectionActivity::onExit() {
  mappedInput.setReaderTouchscreenOverride(false);
  Activity::onExit();
}

void EpubReaderChapterSelectionActivity::selectChapter() {
  const auto tocItem = epub->getTocItem(selectorIndex);
  if (tocItem.spineIndex < 0) {
    ActivityResult result;
    result.isCancelled = true;
    setResult(std::move(result));
  } else {
    setResult(ChapterResult{tocItem.spineIndex, tocItem.anchor});
  }
  finish();
}

void EpubReaderChapterSelectionActivity::onRowEvent(const fui::ActionEvent& event, void* user) {
  auto* self = static_cast<EpubReaderChapterSelectionActivity*>(user);
  if (event.value < 0 || event.value >= self->getTotalItems()) return;
  self->selectorIndex = event.value;
  self->app.clearTapFlash();
  self->selectChapter();
}

void EpubReaderChapterSelectionActivity::loop() {
  const int totalItems = getTotalItems();

  if (!document || totalItems <= 0) {
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm) ||
        mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      ActivityResult result;
      result.isCancelled = true;
      setResult(std::move(result));
      finish();
    }
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    const auto tocItem = document->getTocEntry(selectorIndex);
    if (tocItem.sectionIndex == -1) {
      ActivityResult result;
      result.isCancelled = true;
      setResult(std::move(result));
      finish();
    } else {
      setResult(ChapterResult{tocItem.sectionIndex, tocItem.anchor});
      finish();
    }
  } else if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    ActivityResult result;
    result.isCancelled = true;
    setResult(std::move(result));
    finish();
    return;
  }

  if (uiReady) {
    const fui::InputSnapshot snap = touchSnapshotFrom(mappedInput);
    if (snap.touchPressed || snap.touchReleased) {
      const auto event = app.route(snap);
      if (app.invalidated()) requestUpdate();
      if (event) return;
    }
  }

  const auto swipe = mappedInput.wasSwipe();
  if (swipe == MappedInputManager::SwipeDir::Up || swipe == MappedInputManager::SwipeDir::Down) {
    const int delta = swipe == MappedInputManager::SwipeDir::Up ? visibleRows : -visibleRows;
    const int next = scrollListBy(topIndex, delta, visibleRows, totalItems);
    if (next != topIndex) {
      topIndex = next;
      requestUpdate();
    }
    return;
  }

  const auto moveSelection = [this, totalItems](const int index) {
    selectorIndex = index;
    topIndex = followListSelection(selectorIndex, topIndex, visibleRows, totalItems);
    requestUpdate();
  };
  buttonNavigator.onNextRelease(
      [this, totalItems, &moveSelection] { moveSelection(ButtonNavigator::nextIndex(selectorIndex, totalItems)); });
  buttonNavigator.onPreviousRelease(
      [this, totalItems, &moveSelection] { moveSelection(ButtonNavigator::previousIndex(selectorIndex, totalItems)); });
  buttonNavigator.onNextContinuous([this, totalItems, &moveSelection] {
    moveSelection(ButtonNavigator::nextPageIndex(selectorIndex, totalItems, visibleRows));
  });
  buttonNavigator.onPreviousContinuous([this, totalItems, &moveSelection] {
    moveSelection(ButtonNavigator::previousPageIndex(selectorIndex, totalItems, visibleRows));
  });

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) selectChapter();
}

void EpubReaderChapterSelectionActivity::chapterScreen(UiApp::ScreenType& screen, void* user) {
  static_cast<EpubReaderChapterSelectionActivity*>(user)->buildChapterScreen(screen);
}

void EpubReaderChapterSelectionActivity::buildChapterScreen(UiApp::ScreenType& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  screen.setContentMargin(fui::Insets{
      static_cast<int16_t>(safe.y + metrics.topPadding + TouchHeaderBackButton::height(metrics, mappedInput)),
      static_cast<int16_t>(renderer.getScreenWidth() - safe.x - safe.width),
      static_cast<int16_t>(renderer.getScreenHeight() - safe.y - safe.height), static_cast<int16_t>(safe.x)});
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));

  const int totalItems = getTotalItems();
  std::vector<std::string> labels(totalItems);
  std::vector<fui::ListItem> items;
  items.reserve(totalItems);
  for (int i = 0; i < totalItems; ++i) {
    const auto item = epub->getTocItem(i);
    labels[i] = std::string((item.level - 1) * 2, ' ') + item.title;
    fui::ListItem row;
    row.label = labels[i].c_str();
    row.actionValue = static_cast<int16_t>(i);
    items.push_back(row);
  }

  fui::ListProps props;
  props.items = items.data();
  props.count = static_cast<uint16_t>(items.size());
  props.selectedIndex = static_cast<int16_t>(selectorIndex);
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;
  props.labelText = screen.theme().bodyText;
  const auto rows = configureUiList(props, screen.theme(), screen.body());
  visibleRows = rows > 0 ? rows : 1;
  topIndex = initialViewportPending ? followListSelection(selectorIndex, 0, visibleRows, totalItems)
                                    : scrollListBy(topIndex, 0, visibleRows, totalItems);
  initialViewportPending = false;
  props.topIndex = static_cast<uint16_t>(topIndex);
  screen.list(props);
}

void EpubReaderChapterSelectionActivity::render(RenderLock&&) {
  renderer.clearScreen();

  auto metrics = UITheme::getInstance().getMetrics();
  Rect screen = UITheme::getInstance().getScreenSafeArea(renderer, true, false);

  GUI.drawHeader(renderer, Rect{screen.x, screen.y + metrics.topPadding, screen.width, metrics.headerHeight},
                 tr(STR_SELECT_CHAPTER), nullptr, true);

  const int contentTop = screen.y + metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight = screen.height - contentTop - metrics.verticalSpacing;

  const int totalItems = getTotalItems();
  GUI.drawList(renderer, Rect{screen.x, contentTop, screen.width, contentHeight}, totalItems, selectorIndex,
               [this](int index) {
                 auto item = document->getTocEntry(index);
                 const int indentLevel = item.level > 0 ? static_cast<int>(item.level) - 1 : 0;
                 std::string indent(static_cast<size_t>(indentLevel * 2), ' ');
                 return indent + item.title;
               });

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4, true);
  renderer.displayBuffer();
}
