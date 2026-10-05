#pragma once

#include <Epub/Page.h>
#include <I18n.h>
#include <Memory.h>

#include <array>
#include <cstdint>
#include <memory>

#include "WordRef.h"
#include "activities/Activity.h"

// Multi-word EPUB clipping selector for the current page. Left/Right step words,
// Up/Down jump rows, Confirm marks the start then saves the range, Back cancels.
// Touch: tap moves the cursor; tap Confirm-equivalent (second Confirm) saves.
class ClipSelectionActivity final : public Activity {
 public:
  ClipSelectionActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, ClipWordStore wordStore,
                        std::unique_ptr<Page> page, int fontId, int marginLeft, int marginTop, int sectionPage,
                        int sectionPageCount);

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool isReaderActivity() const override { return true; }
  bool handleHomeGesture() override;

 private:
  static constexpr size_t MAX_READING_ORDER_WORDS = 240;
  static constexpr size_t SNAPSHOT_CAPACITY = 8192;

  ClipWordStore wordStore;
  std::unique_ptr<Page> page;
  int renderFontId = 0;
  int marginLeft = 0;
  int marginTop = 0;
  int sectionPage = 0;
  int sectionPageCount = 1;
  int lineHeight = 0;

  int cursorIdx = 0;
  int startMarkIdx = -1;
  unsigned long lastHorizontalMoveTime = 0;

  std::array<uint16_t, MAX_READING_ORDER_WORDS> readingOrder{};
  size_t readingOrderSize = 0;

  std::unique_ptr<uint8_t[]> snapshot;
  int16_t snapshotX = 0;
  int16_t snapshotY = 0;
  int16_t snapshotW = 0;
  int16_t snapshotH = 0;
  int snapshotToken = -1;  // -1 forces full page redraw

  void buildReadingOrder();
  void positionCursorAtInitialCenter();
  void confirmSelection();
  int wordAt(int x, int y) const;
  void moveVertical(int direction);
  void drawHints() const;
  void drawSelectionHighlights();
  bool captureAndDrawHighlights();
};
