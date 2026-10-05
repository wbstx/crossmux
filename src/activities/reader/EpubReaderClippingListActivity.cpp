#include "EpubReaderClippingListActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>

#include <algorithm>

#include "ClippingStore.h"
#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "components/UiAppHelpers.h"
#include "fontIds.h"

namespace fui = freeink::ui;

namespace {
constexpr int ENTER_ACTIONS_MODE_MS = 700;
}

EpubReaderClippingListActivity::EpubReaderClippingListActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                                               const std::shared_ptr<Epub>& epub)
    : UiListActivity("EpubReaderClippings", renderer, mappedInput, /*wantsTouchLongPress=*/true,
                     /*upstreamStyle=*/true),
      epub(epub) {}

void EpubReaderClippingListActivity::onEnter() {
  UiListActivity::onEnter();
  rebuildClippingRowItems();
}

void EpubReaderClippingListActivity::rebuildClippingRowItems() {
  clippingPreviews.clear();
  clippingSubtitles.clear();
  clippingRowItems.clear();
  if (!CLIPPINGS.hasClippings()) return;

  clippingPreviews.reserve(CLIPPINGS.clippingCount());
  clippingSubtitles.reserve(CLIPPINGS.clippingCount());
  clippingRowItems.reserve(CLIPPINGS.clippingCount());

  for (size_t i = 0; i < CLIPPINGS.clippingCount(); ++i) {
    const Clipping* clipping = CLIPPINGS.clippingAt(i);
    if (!clipping) continue;

    std::string preview;
    if (!CLIPPINGS.readClippingPreview(i, preview) || preview.empty()) {
      preview = "...";
    }
    clippingPreviews.push_back(std::move(preview));

    std::string subtitle;
    if (clipping->chapterTitle[0] != '\0') {
      subtitle = clipping->chapterTitle;
      subtitle += " - ";
    }
    subtitle += "p." + std::to_string(clipping->startPage + 1);
    if (clipping->endPage != clipping->startPage) {
      subtitle += "-" + std::to_string(clipping->endPage + 1);
    }
    clippingSubtitles.push_back(std::move(subtitle));

    fui::ListItem item;
    item.label = clippingPreviews.back().c_str();
    item.subtitle = clippingSubtitles.back().c_str();
    item.icon = listIconFor(UIIcon::Bookmark, 32);
    item.actionValue = static_cast<int16_t>(i);
    clippingRowItems.push_back(item);
  }
}

void EpubReaderClippingListActivity::openSelectedClipping() {
  if (clippingRowItems.empty() || nav.selected < 0 || nav.selected >= listCount()) return;
  const size_t index = static_cast<size_t>(clippingRowItems[nav.selected].actionValue);
  const Clipping* clipping = CLIPPINGS.clippingAt(index);
  if (!clipping) return;

  ClippingJumpResult jump;
  jump.spineIndex = clipping->spineIndex;
  jump.page = clipping->startPage;
  jump.pageCount = clipping->pageCount;
  jump.paragraphIndex = clipping->paragraphIndex;
  jump.clippingIndex = static_cast<uint16_t>(index);
  setResult(std::move(jump));
  finish();
}

void EpubReaderClippingListActivity::activateIndex(const int index) {
  if (confirmPopup.isActive()) return;
  if (index < 0 || index >= listCount()) return;
  app.clearTapFlash();
  nav.selected = index;
  openSelectedClipping();
}

void EpubReaderClippingListActivity::onRowLongPress(const int index) {
  if (confirmPopup.isActive()) return;
  if (index < 0 || index >= listCount()) return;
  app.clearTapFlash();
  nav.selected = index;
  showClippingActions();
}

bool EpubReaderClippingListActivity::handleCustomInput() {
  if (confirmPopup.handleInput(mappedInput, [this] { requestUpdate(); })) return true;
  if (confirmingDelete) {
    confirmingDelete = false;
    requestUpdate();
    return true;
  }
  return false;
}

bool EpubReaderClippingListActivity::handleButtons() {
  if (mappedInput.wasLongPressed(MappedInputManager::Button::Confirm, ENTER_ACTIONS_MODE_MS)) {
    showClippingActions();
    return true;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    ActivityResult result;
    result.isCancelled = true;
    setResult(std::move(result));
    finish();
    return true;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    openSelectedClipping();
    return true;
  }
  return false;
}

void EpubReaderClippingListActivity::showClippingActions() {
  if (clippingRowItems.empty() || confirmPopup.isActive()) return;
  const char* options[] = {tr(STR_OPEN), tr(STR_DELETE), tr(STR_CANCEL)};
  confirmPopup.show(tr(STR_CLIPPINGS), options, 3, 0, [this](int idx) {
    if (idx == 0) {
      openSelectedClipping();
      return;
    }
    if (idx == 1) {
      showDeleteConfirmation();
      return;
    }
    requestUpdate();
  });
  requestUpdate();
}

void EpubReaderClippingListActivity::showDeleteConfirmation() {
  if (clippingRowItems.empty() || confirmPopup.isActive()) return;
  confirmingDelete = true;
  const char* options[] = {tr(STR_CANCEL), tr(STR_DELETE)};
  confirmPopup.show(tr(STR_CONFIRM_DELETE_CLIPPING), options, 2, 0, [this](int idx) {
    confirmingDelete = false;
    if (idx == 1) {
      deleteSelectedClipping();
    }
    requestUpdate();
  });
  requestUpdate();
}

void EpubReaderClippingListActivity::deleteSelectedClipping() {
  if (nav.selected < 0 || nav.selected >= listCount()) return;
  const size_t index = static_cast<size_t>(clippingRowItems[nav.selected].actionValue);
  if (!CLIPPINGS.removeClippingAt(index)) {
    LOG_ERR("CLIP", "Failed to delete clipping %u", static_cast<unsigned>(index));
    return;
  }
  rebuildClippingRowItems();
  if (nav.selected >= listCount() && nav.selected > 0) nav.selected--;
  if (clippingRowItems.empty()) {
    ActivityResult result;
    result.isCancelled = true;
    setResult(std::move(result));
    finish();
    return;
  }
  nav.follow(listCount());
  requestUpdate(true);
}

void EpubReaderClippingListActivity::buildScreen(UiScreen& screen) {
  const auto& metrics = uiThemeMetrics(true);
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  screen.setContentMarginFromScreen(fui::Insets{
      static_cast<int16_t>(safe.y + metrics.topPadding + metrics.headerHeight),
      static_cast<int16_t>(renderer.getScreenWidth() - (safe.x + safe.width)),
      static_cast<int16_t>(renderer.getScreenHeight() - (safe.y + safe.height)), static_cast<int16_t>(safe.x)});
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));

  if (clippingRowItems.empty()) {
    screen.centeredText(tr(STR_NO_CLIPPINGS), screen.theme().bodyText);
    return;
  }

  if (!mappedInput.hasTouch()) {
    const int helpLineHeight = renderer.getLineHeight(SMALL_FONT_ID);
    const fui::Rect band = screen.takeBottom(static_cast<int16_t>(helpLineHeight + metrics.verticalSpacing));
    GUI.drawHelpText(renderer, Rect{band.x, band.y + metrics.verticalSpacing, band.width, helpLineHeight},
                     tr(STR_HOLD_OPEN_FOR_ACTIONS));
  }

  fui::ListProps props;
  props.items = clippingRowItems.data();
  props.count = static_cast<uint16_t>(clippingRowItems.size());
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch | fui::InputLongPress;
  syncListViewport(screen, props);
  screen.list(props);
}

void EpubReaderClippingListActivity::render(RenderLock&&) {
  if (confirmPopup.processRender(renderer, mappedInput)) return;

  renderer.clearScreen();
  const auto pageWidth = renderer.getScreenWidth();
  const int titleX = (pageWidth - renderer.getTextWidth(UI_12_FONT_ID, tr(STR_CLIPPINGS), EpdFontFamily::BOLD)) / 2;
  renderer.drawText(UI_12_FONT_ID, titleX, 15, tr(STR_CLIPPINGS), true, EpdFontFamily::BOLD);

  renderUi();
  drawFooter();
  renderer.displayBuffer();
}
