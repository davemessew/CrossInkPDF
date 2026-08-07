#include "EpubReaderClippingListActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include <algorithm>
#include <cstdio>

#include "MappedInputManager.h"
#include "activities/ActivityResult.h"
#include "activities/home/FileBrowserActionActivity.h"
#include "components/TouchHeaderBackButton.h"
#include "components/UITheme.h"
#include "components/UIThemeTokens.h"
#include "components/UiAppHelpers.h"
#include "fontIds.h"

namespace fui = freeink::ui;

namespace {
constexpr fui::ActionId ACTION_ROW = 1;
constexpr int DETAIL_START_Y = 70;
constexpr int DETAIL_SIDE_MARGIN = 20;
constexpr int DETAIL_BOTTOM_RESERVE = 55;
constexpr int DETAIL_LINE_GAP = 6;
constexpr unsigned long CLIPPING_DELETE_HOLD_MS = 1000;

bool isUtf8SpaceAt(const std::string& text, const size_t index, size_t& advance) {
  const auto c = static_cast<unsigned char>(text[index]);
  if (c == 0xC2 && index + 1 < text.size() && static_cast<unsigned char>(text[index + 1]) == 0xA0) {
    advance = 2;
    return true;
  }
  if (c == 0xE2 && index + 2 < text.size() && static_cast<unsigned char>(text[index + 1]) == 0x80) {
    const auto c2 = static_cast<unsigned char>(text[index + 2]);
    if (c2 == 0x83 || c2 == 0xAF) {
      advance = 3;
      return true;
    }
  }
  return false;
}

void buildOneLineSnippetText(const std::string& text, std::string& out) {
  out.clear();
  bool lastWasSpace = true;
  for (size_t i = 0; i < text.size();) {
    size_t advance = 0;
    if (isUtf8SpaceAt(text, i, advance)) {
      if (!lastWasSpace) {
        out += ' ';
        lastWasSpace = true;
      }
      i += advance;
      continue;
    }

    const char c = text[i++];
    if (c == '\r' || c == '\n' || c == '\t') {
      if (!lastWasSpace) {
        out += ' ';
        lastWasSpace = true;
      }
      continue;
    }
    out += c;
    lastWasSpace = c == ' ';
  }
  while (!out.empty() && out.back() == ' ') {
    out.pop_back();
  }
}

size_t utf8CharLen(const std::string& text, const size_t index) {
  const auto c = static_cast<unsigned char>(text[index]);
  if ((c & 0x80) == 0) return 1;
  if ((c & 0xE0) == 0xC0 && index + 1 < text.size()) return 2;
  if ((c & 0xF0) == 0xE0 && index + 2 < text.size()) return 3;
  if ((c & 0xF8) == 0xF0 && index + 3 < text.size()) return 4;
  return 1;
}

void appendLongWordLines(const GfxRenderer& renderer, const int fontId, const std::string& word, const int maxWidth,
                         std::vector<std::string>& out) {
  std::string line;
  for (size_t i = 0; i < word.size();) {
    const size_t charLen = utf8CharLen(word, i);
    const std::string next = word.substr(i, charLen);
    const std::string candidate = line + next;
    if (!line.empty() && renderer.getTextWidth(fontId, candidate.c_str()) > maxWidth) {
      out.push_back(line);
      line = next;
    } else {
      line = candidate;
    }
    i += charLen;
  }
  if (!line.empty()) out.push_back(line);
}

void appendWrappedWord(const GfxRenderer& renderer, const int fontId, const std::string& word, const int maxWidth,
                       std::string& currentLine, std::vector<std::string>& out) {
  if (word.empty()) return;

  if (renderer.getTextWidth(fontId, word.c_str()) > maxWidth) {
    if (!currentLine.empty()) {
      out.push_back(currentLine);
      currentLine.clear();
    }
    appendLongWordLines(renderer, fontId, word, maxWidth, out);
    return;
  }

  const std::string candidate = currentLine.empty() ? word : currentLine + " " + word;
  if (renderer.getTextWidth(fontId, candidate.c_str()) <= maxWidth) {
    currentLine = candidate;
    return;
  }

  if (!currentLine.empty()) out.push_back(currentLine);
  currentLine = word;
}

void buildWrappedDetailLines(const GfxRenderer& renderer, const int fontId, const std::string& text, const int maxWidth,
                             std::vector<std::string>& out) {
  out.clear();
  if (maxWidth <= 0) return;

  std::string currentLine;
  size_t wordStart = 0;
  while (wordStart < text.size()) {
    while (wordStart < text.size() && text[wordStart] == ' ') {
      wordStart++;
    }
    if (wordStart >= text.size()) break;

    size_t wordEnd = wordStart;
    while (wordEnd < text.size() && text[wordEnd] != ' ') {
      wordEnd++;
    }

    appendWrappedWord(renderer, fontId, text.substr(wordStart, wordEnd - wordStart), maxWidth, currentLine, out);
    wordStart = wordEnd;
  }

  if (!currentLine.empty()) out.push_back(currentLine);
}
}  // namespace

EpubReaderClippingListActivity::EpubReaderClippingListActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : Activity("EpubClippingList", renderer, mappedInput),
      uiTarget(makeUiTarget(renderer)),
      app(uiTarget, uiTarget.deviceContext()) {}

void EpubReaderClippingListActivity::onEnter() {
  Activity::onEnter();
  selectedIndex = 0;
  topIndex = 0;
  visibleRows = 1;
  uiReady = false;
  app.setTheme(uiThemeTokens(uiTarget));
  app.on(ACTION_ROW, &EpubReaderClippingListActivity::onRowEvent, this);
  app.setScreen(&EpubReaderClippingListActivity::listScreen, this);
  detailText.reserve(CLIPPING_TEXT_MAX);
  detailLines.reserve(32);
  uiItems.resize(CLIPPINGS.clippingCount());
  requestUpdate();
}

int EpubReaderClippingListActivity::getDetailTextWidth() const {
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  return std::max(1, safe.width - DETAIL_SIDE_MARGIN * 2);
}

int EpubReaderClippingListActivity::getDetailLinesPerPage() const {
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  const int lineStep = renderer.getLineHeight(UI_10_FONT_ID) + DETAIL_LINE_GAP;
  const int available = safe.height - DETAIL_START_Y - DETAIL_BOTTOM_RESERVE;
  return std::max(1, available / std::max(1, lineStep));
}

int EpubReaderClippingListActivity::getDetailPageCount() const {
  if (detailLinesPerPage <= 0) return 1;
  return std::max(1, static_cast<int>((detailLines.size() + detailLinesPerPage - 1) / detailLinesPerPage));
}

void EpubReaderClippingListActivity::closeDetail() {
  detailMode = false;
  detailPage = 0;
  detailText.clear();
  detailLines.clear();
  detailLayoutWidth = 0;
  detailLinesPerPage = 0;
  requestUpdate();
}

void EpubReaderClippingListActivity::jumpToSelectedClipping() {
  if (selectedIndex < 0 || selectedIndex >= static_cast<int>(CLIPPINGS.clippingCount())) return;

  const Clipping* clipping = CLIPPINGS.clippingAt(static_cast<size_t>(selectedIndex));
  if (!clipping) return;
  setResult(ClippingJumpResult{clipping->spineIndex, clipping->startPage, clipping->pageCount, clipping->paragraphIndex,
                               static_cast<uint16_t>(selectedIndex)});
  finish();
}

void EpubReaderClippingListActivity::openSelectedDetail() {
  if (selectedIndex < 0 || selectedIndex >= static_cast<int>(CLIPPINGS.clippingCount())) return;

  std::string text;
  text.reserve(CLIPPING_TEXT_MAX);
  if (!CLIPPINGS.readClippingText(static_cast<size_t>(selectedIndex), text)) {
    text.clear();
  }
  buildOneLineSnippetText(text, detailText);
  detailMode = true;
  detailPage = 0;
  detailLayoutWidth = 0;
  detailLinesPerPage = 0;
  rebuildDetailLayoutIfNeeded();
  requestUpdate();
}

void EpubReaderClippingListActivity::rebuildDetailLayoutIfNeeded() {
  const int textWidth = getDetailTextWidth();
  const int linesPerPage = getDetailLinesPerPage();
  if (textWidth == detailLayoutWidth && linesPerPage == detailLinesPerPage && !detailLines.empty()) return;

  buildWrappedDetailLines(renderer, UI_10_FONT_ID, detailText, textWidth, detailLines);
  if (detailLines.empty()) detailLines.push_back("");
  detailLayoutWidth = textWidth;
  detailLinesPerPage = linesPerPage;
  detailPage = std::min(detailPage, getDetailPageCount() - 1);
}

void EpubReaderClippingListActivity::deleteSelectedClipping() {
  if (selectedIndex < 0 || selectedIndex >= static_cast<int>(CLIPPINGS.clippingCount())) return;

  bool removed = false;
  if (deleteCallback == nullptr) {
    removed = CLIPPINGS.removeClippingAt(static_cast<size_t>(selectedIndex));
  } else {
    removed = deleteCallback(deleteContext, clippings[selectedIndex].paragraphIndex);
  }
  if (!removed) return;
  if (deleteCallback != nullptr && selectedIndex < static_cast<int>(clippings.size())) {
    clippings.erase(clippings.begin() + selectedIndex);
  }

  detailMode = false;
  detailText.clear();
  detailLines.clear();
  detailLayoutWidth = 0;
  detailLinesPerPage = 0;
  if (CLIPPINGS.clippingCount() == 0) {
    selectedIndex = 0;
  } else if (selectedIndex >= static_cast<int>(CLIPPINGS.clippingCount())) {
    selectedIndex = static_cast<int>(CLIPPINGS.clippingCount()) - 1;
  }
  topIndex = followListSelection(selectedIndex, topIndex, visibleRows, static_cast<int>(CLIPPINGS.clippingCount()));
  uiItems.resize(CLIPPINGS.clippingCount());
  requestUpdate();
}

void EpubReaderClippingListActivity::showClippingActionMenu(const bool ignoreInitialConfirmRelease) {
  if (selectedIndex < 0 || selectedIndex >= static_cast<int>(CLIPPINGS.clippingCount())) return;

  const Clipping* selectedClipping = CLIPPINGS.clippingAt(static_cast<size_t>(selectedIndex));
  if (!selectedClipping) return;
  const char* title = selectedClipping->chapterTitle[0] != '\0' ? selectedClipping->chapterTitle : tr(STR_CLIPPINGS);
  const uint16_t selectedSpineIndex = selectedClipping->spineIndex;
  const uint16_t selectedStartPage = selectedClipping->startPage;
  const uint16_t selectedStartWordIndex = selectedClipping->startWordIndex;
  const uint32_t selectedTimestamp = selectedClipping->timestamp;
  std::vector<FileBrowserActionActivity::MenuItem> items;
  items.reserve(1);
  items.push_back({FileBrowserAction::Delete, StrId::STR_DELETE});

  startActivityForResult(
      std::make_unique<FileBrowserActionActivity>(renderer, mappedInput, title, std::move(items),
                                                  ignoreInitialConfirmRelease),
      [this, selectedSpineIndex, selectedStartPage, selectedStartWordIndex,
       selectedTimestamp](const ActivityResult& result) {
        longPressConfirmHandled = false;
        if (result.isCancelled) {
          requestUpdate();
          return;
        }

        const auto* actionResult = std::get_if<FileBrowserActionResult>(&result.data);
        if (!actionResult || static_cast<FileBrowserAction>(actionResult->action) != FileBrowserAction::Delete) {
          requestUpdate();
          return;
        }

        for (size_t i = 0; i < CLIPPINGS.clippingCount(); ++i) {
          const Clipping* clipping = CLIPPINGS.clippingAt(i);
          if (!clipping) continue;
          if (clipping->spineIndex == selectedSpineIndex && clipping->startPage == selectedStartPage &&
              clipping->startWordIndex == selectedStartWordIndex && clipping->timestamp == selectedTimestamp) {
            selectedIndex = static_cast<int>(i);
            deleteSelectedClipping();
            return;
          }
        }
        requestUpdate();
      });
}

void EpubReaderClippingListActivity::loop() {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  const Rect header{safe.x, safe.y + metrics.topPadding, safe.width,
                    TouchHeaderBackButton::height(metrics, mappedInput)};
  if (!detailMode && TouchHeaderBackButton::wasTapped(mappedInput, header)) {
    ActivityResult result;
    result.isCancelled = true;
    setResult(std::move(result));
    finish();
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    if (detailMode) {
      closeDetail();
      return;
    }
    ActivityResult result;
    result.isCancelled = true;
    setResult(std::move(result));
    finish();
    return;
  }

  if (CLIPPINGS.clippingCount() > 0 && !longPressConfirmHandled &&
      mappedInput.isPressed(MappedInputManager::Button::Confirm) &&
      mappedInput.getHeldTime() >= CLIPPING_DELETE_HOLD_MS) {
    longPressConfirmHandled = true;
    showClippingActionMenu(true);
    return;
  }

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    if (longPressConfirmHandled) {
      longPressConfirmHandled = false;
      return;
    }
    if (CLIPPINGS.clippingCount() > 0 && selectedIndex >= 0 &&
        selectedIndex < static_cast<int>(CLIPPINGS.clippingCount())) {
      if (detailMode) {
        jumpToSelectedClipping();
      } else {
        openSelectedDetail();
      }
    }
    return;
  }

  const int total = static_cast<int>(CLIPPINGS.clippingCount());
  if (total == 0) return;

  if (detailMode) {
    int touchX = 0;
    int touchY = 0;
    const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
    if (!longPressConfirmHandled && mappedInput.isScreenTouchLongPress(touchX, touchY, CLIPPING_DELETE_HOLD_MS) &&
        touchY >= safe.y + DETAIL_START_Y && touchY < safe.y + safe.height - DETAIL_BOTTOM_RESERVE) {
      mappedInput.suppressNextTouchTap();
      longPressConfirmHandled = true;
      showClippingActionMenu(false);
      return;
    }
    rebuildDetailLayoutIfNeeded();
    const int detailPageCount = getDetailPageCount();
    const auto swipe = mappedInput.wasSwipe();
    if (swipe == MappedInputManager::SwipeDir::Up && detailPage < detailPageCount - 1) {
      detailPage++;
      requestUpdate();
      return;
    }
    if (swipe == MappedInputManager::SwipeDir::Down && detailPage > 0) {
      detailPage--;
      requestUpdate();
      return;
    }
    buttonNavigator.onNextRelease([this, detailPageCount] {
      if (detailPage < detailPageCount - 1) {
        detailPage++;
        requestUpdate();
      }
    });
    buttonNavigator.onPreviousRelease([this] {
      if (detailPage > 0) {
        detailPage--;
        requestUpdate();
      }
    });
    buttonNavigator.onNextContinuous([this, detailPageCount] {
      if (detailPage < detailPageCount - 1) {
        detailPage++;
        requestUpdate();
      }
    });
    buttonNavigator.onPreviousContinuous([this] {
      if (detailPage > 0) {
        detailPage--;
        requestUpdate();
      }
    });
    return;
  }

  int tx = 0;
  int ty = 0;
  if (!longPressConfirmHandled && mappedInput.isScreenTouchLongPress(tx, ty, CLIPPING_DELETE_HOLD_MS) &&
      listRowStep > 0 && ty >= listTop && ty < listBottom) {
    const int offset = ty - listTop;
    const int row = offset / listRowStep;
    const int touchedIndex = topIndex + row;
    if (row < visibleRows && offset % listRowStep < listRowHeight && touchedIndex < total) {
      selectedIndex = touchedIndex;
      mappedInput.suppressNextTouchTap();
      longPressConfirmHandled = true;
      showClippingActionMenu(false);
    }
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
    const int next = scrollListBy(topIndex, swipe == MappedInputManager::SwipeDir::Up ? visibleRows : -visibleRows,
                                  visibleRows, total);
    if (next != topIndex) {
      topIndex = next;
      requestUpdate();
    }
    return;
  }

  const auto moveSelection = [this, total](const int next) {
    selectedIndex = next;
    topIndex = followListSelection(selectedIndex, topIndex, visibleRows, total);
    requestUpdate();
  };
  buttonNavigator.onNextRelease(
      [this, total, &moveSelection] { moveSelection(ButtonNavigator::nextIndex(selectedIndex, total)); });
  buttonNavigator.onPreviousRelease(
      [this, total, &moveSelection] { moveSelection(ButtonNavigator::previousIndex(selectedIndex, total)); });
  buttonNavigator.onNextContinuous([this, total, &moveSelection] {
    moveSelection(ButtonNavigator::nextPageIndex(selectedIndex, total, visibleRows));
  });
  buttonNavigator.onPreviousContinuous([this, total, &moveSelection] {
    moveSelection(ButtonNavigator::previousPageIndex(selectedIndex, total, visibleRows));
  });
}

void EpubReaderClippingListActivity::onRowEvent(const fui::ActionEvent& event, void* user) {
  auto* self = static_cast<EpubReaderClippingListActivity*>(user);
  if (event.value < 0 || event.value >= static_cast<int16_t>(CLIPPINGS.clippingCount())) return;
  self->selectedIndex = event.value;
  self->app.clearTapFlash();
  self->openSelectedDetail();
}

void EpubReaderClippingListActivity::listScreen(UiApp::ScreenType& screen, void* user) {
  static_cast<EpubReaderClippingListActivity*>(user)->buildListScreen(screen);
}

void EpubReaderClippingListActivity::buildListScreen(UiApp::ScreenType& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  screen.setContentMargin(fui::Insets{
      static_cast<int16_t>(safe.y + metrics.topPadding + TouchHeaderBackButton::height(metrics, mappedInput)),
      static_cast<int16_t>(renderer.getScreenWidth() - safe.x - safe.width),
      static_cast<int16_t>(renderer.getScreenHeight() - safe.y - safe.height), static_cast<int16_t>(safe.x)});
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));
  const size_t count = CLIPPINGS.clippingCount();
  if (count == 0) {
    screen.centeredText(tr(STR_NO_CLIPPINGS), screen.theme().bodyText);
    return;
  }
  fui::ListProps props;
  props.items = uiItems.data();
  props.count = static_cast<uint16_t>(uiItems.size());
  props.selectedIndex = static_cast<int16_t>(selectedIndex);
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;
  const fui::Rect bounds = screen.body();
  listTop = bounds.y;
  listBottom = bounds.bottom();
  const auto rows = configureUiList(props, screen.theme(), bounds, UiListRowType::WithSubtitle);
  listRowHeight = props.rowHeight;
  listRowStep = props.rowHeight + props.rowGap;
  visibleRows = rows > 0 ? rows : 1;
  topIndex = scrollListBy(topIndex, 0, visibleRows, static_cast<int>(count));
  props.topIndex = static_cast<uint16_t>(topIndex);
  const int end = std::min(static_cast<int>(count), topIndex + visibleRows);
  for (int i = topIndex; i < end; ++i) {
    const size_t slot = static_cast<size_t>(i - topIndex);
    uiRawText[slot].clear();
    CLIPPINGS.readClippingText(static_cast<size_t>(i), uiRawText[slot]);
    buildOneLineSnippetText(uiRawText[slot], uiLabels[slot]);
    const Clipping* clipping = CLIPPINGS.clippingAt(static_cast<size_t>(i));
    fui::ListItem& item = uiItems[static_cast<size_t>(i)];
    item = fui::ListItem{};
    item.label = uiLabels[slot].c_str();
    if (clipping) item.subtitle = clipping->chapterTitle[0] != '\0' ? clipping->chapterTitle : tr(STR_UNKNOWN_CHAPTER);
    item.actionValue = static_cast<int16_t>(i);
  }
  screen.list(props);
}

void EpubReaderClippingListActivity::renderDetail() {
  rebuildDetailLayoutIfNeeded();

  // Front-button hints move to a different physical edge for each reader
  // orientation. Keep all detail content inside the same safe area used by
  // the list view so it cannot render beneath that hint band.
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  const int contentX = safe.x;
  const int contentWidth = safe.width;
  const int contentY = safe.y;

  const char* chapter = tr(STR_CLIPPINGS);
  const Clipping* selectedClipping =
      selectedIndex >= 0 ? CLIPPINGS.clippingAt(static_cast<size_t>(selectedIndex)) : nullptr;
  if (selectedClipping && selectedClipping->chapterTitle[0] != '\0') {
    chapter = selectedClipping->chapterTitle;
  }

  const std::string title =
      renderer.truncatedText(UI_12_FONT_ID, chapter, contentWidth - DETAIL_SIDE_MARGIN * 2, EpdFontFamily::BOLD);
  const int titleX =
      contentX + (contentWidth - renderer.getTextWidth(UI_12_FONT_ID, title.c_str(), EpdFontFamily::BOLD)) / 2;
  renderer.drawText(UI_12_FONT_ID, titleX, 15 + contentY, title.c_str(), true, EpdFontFamily::BOLD);

  const int lineStep = renderer.getLineHeight(UI_10_FONT_ID) + DETAIL_LINE_GAP;
  const int textX = contentX + DETAIL_SIDE_MARGIN;
  const int firstLine = detailPage * detailLinesPerPage;
  const int lastLine = std::min(static_cast<int>(detailLines.size()), firstLine + detailLinesPerPage);
  int y = DETAIL_START_Y + contentY;
  for (int i = firstLine; i < lastLine; i++) {
    renderer.drawText(UI_10_FONT_ID, textX, y, detailLines[i].c_str());
    y += lineStep;
  }

  const int detailPageCount = getDetailPageCount();
  if (detailPageCount > 1) {
    char pageBuf[16];
    snprintf(pageBuf, sizeof(pageBuf), "%d/%d", detailPage + 1, detailPageCount);
    const int pageLabelWidth = renderer.getTextWidth(SMALL_FONT_ID, pageBuf);
    renderer.drawText(SMALL_FONT_ID, contentX + contentWidth - DETAIL_SIDE_MARGIN - pageLabelWidth,
                      safe.y + safe.height - 35, pageBuf);
  }

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_OPEN), detailPage > 0 ? tr(STR_DIR_UP) : "",
                                            detailPage < detailPageCount - 1 ? tr(STR_DIR_DOWN) : "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4, true);
}

void EpubReaderClippingListActivity::render(RenderLock&&) {
  renderer.clearScreen();

  if (detailMode) {
    renderDetail();
    renderer.displayBuffer();
    return;
  }

  const auto& metrics = UITheme::getInstance().getMetrics();
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  const Rect header{safe.x, safe.y + metrics.topPadding, safe.width,
                    TouchHeaderBackButton::height(metrics, mappedInput)};
  if (mappedInput.hasTouchHardware()) {
    TouchHeaderBackButton::draw(renderer, uiTarget, header, tr(STR_CLIPPINGS), true);
  } else {
    GUI.drawHeader(renderer, header, tr(STR_CLIPPINGS), nullptr, true);
  }
  uiReady = false;
  app.render();
  uiReady = true;

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), CLIPPINGS.clippingCount() == 0 ? "" : tr(STR_OPEN),
                                            tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4, true);
  renderer.displayBuffer();
}
