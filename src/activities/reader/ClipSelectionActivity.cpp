#include "ClipSelectionActivity.h"

#include <FontCacheManager.h>
#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>

#include <algorithm>
#include <climits>
#include <cstring>

#include "MappedInputManager.h"
#include "activities/ActivityResult.h"
#include "clippings/ClipTextBuilder.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {

constexpr unsigned long WORD_REPEAT_START_MS = 500;
constexpr unsigned long WORD_REPEAT_INTERVAL_MS = 500;

bool hasEmSpace(const char* text) { return text && text[0] == '\xe2' && text[1] == '\x80' && text[2] == '\x83'; }

}  // namespace

ClipSelectionActivity::ClipSelectionActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                             ClipWordStore wordStore, std::unique_ptr<Page> page, const int fontId,
                                             const int marginLeft, const int marginTop, const int sectionPage,
                                             const int sectionPageCount)
    : Activity("ClipSelection", renderer, mappedInput),
      wordStore(std::move(wordStore)),
      page(std::move(page)),
      renderFontId(fontId),
      marginLeft(marginLeft),
      marginTop(marginTop),
      sectionPage(sectionPage),
      sectionPageCount(std::max(1, sectionPageCount)) {}

void ClipSelectionActivity::onEnter() {
  Activity::onEnter();
  lineHeight = renderer.getLineHeight(renderFontId);
  snapshot = makeUniqueNoThrow<uint8_t[]>(SNAPSHOT_CAPACITY);

  if (wordStore.words.empty()) {
    LOG_ERR("CLIP", "No words available for selection");
    ActivityResult result;
    result.isCancelled = true;
    setResult(std::move(result));
    finish();
    return;
  }

  buildReadingOrder();
  if (readingOrderSize == 0) {
    LOG_ERR("CLIP", "No readable word order available");
    ActivityResult result;
    result.isCancelled = true;
    setResult(std::move(result));
    finish();
    return;
  }

  positionCursorAtInitialCenter();
  requestUpdate();
}

bool ClipSelectionActivity::handleHomeGesture() {
  ActivityResult result;
  result.isCancelled = true;
  setResult(std::move(result));
  finish();
  return true;
}

void ClipSelectionActivity::buildReadingOrder() {
  readingOrderSize = 0;
  for (size_t i = 0; i < wordStore.words.size() && readingOrderSize < MAX_READING_ORDER_WORDS; ++i) {
    readingOrder[readingOrderSize++] = static_cast<uint16_t>(i);
  }
}

void ClipSelectionActivity::positionCursorAtInitialCenter() {
  if (readingOrderSize == 0) {
    cursorIdx = 0;
    return;
  }
  const int midY = renderer.getScreenHeight() / 2;
  const int midX = renderer.getScreenWidth() / 2;
  int best = 0;
  int bestDist = INT_MAX;
  for (size_t i = 0; i < readingOrderSize; ++i) {
    const WordRef& word = wordStore.words[readingOrder[i]];
    const int dist = std::abs(word.y + word.h / 2 - midY) * 2 + std::abs(word.x + word.w / 2 - midX);
    if (dist < bestDist) {
      bestDist = dist;
      best = static_cast<int>(i);
    }
  }
  cursorIdx = best;
}

int ClipSelectionActivity::wordAt(const int x, const int y) const {
  constexpr int SLOP = 4;
  for (size_t i = 0; i < readingOrderSize; ++i) {
    const WordRef& word = wordStore.words[readingOrder[i]];
    if (x >= word.x - SLOP && x < word.x + word.w + SLOP && y >= word.y - SLOP && y < word.y + word.h + SLOP) {
      return static_cast<int>(i);
    }
  }
  return -1;
}

void ClipSelectionActivity::moveVertical(const int direction) {
  if (readingOrderSize == 0) return;
  const WordRef& current = wordStore.words[readingOrder[cursorIdx]];
  int best = -1;
  int bestScore = INT_MAX;
  for (size_t i = 0; i < readingOrderSize; ++i) {
    const WordRef& word = wordStore.words[readingOrder[i]];
    if (direction > 0) {
      if (word.y <= current.y) continue;
    } else {
      if (word.y >= current.y) continue;
    }
    const int rowDelta = std::abs(word.y - current.y);
    const int xDelta = std::abs(word.x + word.w / 2 - (current.x + current.w / 2));
    const int score = rowDelta * 1000 + xDelta;
    if (score < bestScore) {
      bestScore = score;
      best = static_cast<int>(i);
    }
  }
  if (best >= 0 && best != cursorIdx) {
    cursorIdx = best;
    requestUpdate();
  }
}

void ClipSelectionActivity::confirmSelection() {
  if (startMarkIdx == -1) {
    startMarkIdx = cursorIdx;
    snapshotToken = -1;
    requestUpdate();
    return;
  }

  const int from = std::min(startMarkIdx, cursorIdx);
  const int to = std::max(startMarkIdx, cursorIdx);
  auto result =
      ClipTextBuilder::build(wordStore, readingOrder.data(), from, to, sectionPage, sectionPageCount, nullptr);
  setResult(std::move(result));
  finish();
}

void ClipSelectionActivity::drawSelectionHighlights() {
  const int from = startMarkIdx >= 0 ? std::min(startMarkIdx, cursorIdx) : cursorIdx;
  const int to = startMarkIdx >= 0 ? std::max(startMarkIdx, cursorIdx) : cursorIdx;
  for (int i = from; i <= to; ++i) {
    const WordRef& word = wordStore.words[readingOrder[i]];
    const auto textStyle = static_cast<EpdFontFamily::Style>(word.style & ~EpdFontFamily::UNDERLINE);
    const int skipX =
        hasEmSpace(wordStore.text(word)) ? renderer.getTextAdvanceX(renderFontId, "\xe2\x80\x83", textStyle) : 0;
    const int drawX = word.x + skipX;
    const int drawW = word.w - skipX;
    if (drawW <= 0) continue;
    for (int y = word.y; y < word.y + word.h; y += 2) {
      for (int x = drawX; x < drawX + drawW; x += 2) {
        renderer.drawPixel(x, y, true);
      }
    }
    if (i == cursorIdx) {
      renderer.drawRect(drawX - 1, word.y - 1, drawW + 2, word.h + 2, true);
    }
  }
}

bool ClipSelectionActivity::captureAndDrawHighlights() {
  const int from = startMarkIdx >= 0 ? std::min(startMarkIdx, cursorIdx) : cursorIdx;
  const int to = startMarkIdx >= 0 ? std::max(startMarkIdx, cursorIdx) : cursorIdx;
  int minX = INT_MAX, minY = INT_MAX, maxX = 0, maxY = 0;
  for (int i = from; i <= to; ++i) {
    const WordRef& word = wordStore.words[readingOrder[i]];
    minX = std::min(minX, word.x - 2);
    minY = std::min(minY, word.y - 2);
    maxX = std::max(maxX, word.x + word.w + 2);
    maxY = std::max(maxY, word.y + word.h + 2);
  }
  if (minX < 0) minX = 0;
  if (minY < 0) minY = 0;
  const int w = maxX - minX;
  const int h = maxY - minY;
  bool saved = false;
  if (snapshot && w > 0 && h > 0) {
    saved = renderer.readFramebufferRegion(minX, minY, w, h, snapshot.get(), SNAPSHOT_CAPACITY) > 0;
  }
  snapshotX = static_cast<int16_t>(minX);
  snapshotY = static_cast<int16_t>(minY);
  snapshotW = static_cast<int16_t>(w);
  snapshotH = static_cast<int16_t>(h);
  snapshotToken = saved ? (cursorIdx + (startMarkIdx + 1) * 1000) : -1;
  drawSelectionHighlights();
  return saved;
}

void ClipSelectionActivity::drawHints() const {
  const auto confirmLabel = startMarkIdx == -1 ? tr(STR_SELECT) : tr(STR_DONE);
  const auto labels = mappedInput.mapDirectionalLabels(tr(STR_BACK), confirmLabel, tr(STR_DIR_LEFT), tr(STR_DIR_RIGHT),
                                                       tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void ClipSelectionActivity::loop() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    ActivityResult result;
    result.isCancelled = true;
    setResult(std::move(result));
    finish();
    return;
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm) ||
      mappedInput.wasReleased(MappedInputManager::Button::Power)) {
    confirmSelection();
    return;
  }

  if (readingOrderSize == 0) return;

  int tx = 0;
  int ty = 0;
  if (mappedInput.wasScreenTouchDown(tx, ty)) {
    const int hit = wordAt(tx, ty);
    if (hit >= 0 && hit != cursorIdx) {
      cursorIdx = hit;
      requestUpdate();
    }
    return;
  }
  if (mappedInput.wasScreenTapped(tx, ty)) {
    const int hit = wordAt(tx, ty);
    if (hit >= 0) {
      cursorIdx = hit;
      if (startMarkIdx < 0) {
        startMarkIdx = cursorIdx;
        snapshotToken = -1;
        requestUpdate();
      } else {
        confirmSelection();
      }
    }
    return;
  }

  const unsigned long now = millis();
  const bool repeat =
      mappedInput.getHeldTime() >= WORD_REPEAT_START_MS && now - lastHorizontalMoveTime >= WORD_REPEAT_INTERVAL_MS;
  const bool moveLeft = mappedInput.wasPressed(MappedInputManager::Button::ScreenLeft) ||
                        (repeat && mappedInput.isPressed(MappedInputManager::Button::ScreenLeft));
  const bool moveRight = mappedInput.wasPressed(MappedInputManager::Button::ScreenRight) ||
                         (repeat && mappedInput.isPressed(MappedInputManager::Button::ScreenRight));
  if (moveLeft && cursorIdx > 0) {
    cursorIdx--;
    lastHorizontalMoveTime = now;
    requestUpdate();
  } else if (moveRight && cursorIdx + 1 < static_cast<int>(readingOrderSize)) {
    cursorIdx++;
    lastHorizontalMoveTime = now;
    requestUpdate();
  } else if (mappedInput.wasPressed(MappedInputManager::Button::ScreenUp)) {
    moveVertical(-1);
  } else if (mappedInput.wasPressed(MappedInputManager::Button::ScreenDown)) {
    moveVertical(1);
  }
}

void ClipSelectionActivity::render(RenderLock&&) {
  const int token = cursorIdx + (startMarkIdx + 1) * 1000;
  if (snapshotToken >= 0 && snapshotToken != token && snapshot) {
    renderer.writeFramebufferRegion(snapshotX, snapshotY, snapshotW, snapshotH, snapshot.get());
    if (captureAndDrawHighlights()) {
      drawHints();
      renderer.displayBuffer(HalDisplay::FAST_REFRESH);
      return;
    }
  }

  renderer.clearScreen();
  if (page) {
    auto* fcm = renderer.getFontCacheManager();
    if (fcm) {
      auto scope = fcm->createPrewarmScope();
      page->render(renderer, renderFontId, marginLeft, marginTop);
      scope.endScanAndPrewarm();
      page->render(renderer, renderFontId, marginLeft, marginTop);
    } else {
      page->render(renderer, renderFontId, marginLeft, marginTop);
    }
  }

  if (readingOrderSize > 0) {
    captureAndDrawHighlights();
  }
  drawHints();
  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
}
