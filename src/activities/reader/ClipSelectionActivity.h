#pragma once

#include <Epub/Page.h>
#include <I18n.h>
#include <Memory.h>

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

#include "ClipSelectionLimits.h"
#include "WordRef.h"
#include "activities/Activity.h"

class Section;

// Multi-word EPUB clipping selector.
//
// Left/Right step words, Up/Down jump rows, Confirm marks the start then saves
// the range, Back clears the start mark (or cancels). Touch: tap moves the
// cursor and marks/saves, press-and-drag moves the cursor, holding the bottom
// visible line soft-advances one line per second.
//
// Selection can continue past the end of the reader page without a page flip:
// words are loaded one line at a time from the section (following pages get a
// real pageIdx), and the view scrolls by whole lines so crossing into the next
// page reveals one more line at the bottom ("soft advance"). Memory is bounded
// by ClipSelectionLimits (PSRAM-aware); when the word cap is hit, lines above
// the selection are evicted so the window can keep sliding forward.
class ClipSelectionActivity final : public Activity {
 public:
  struct Layout {
    int fontId = 0;
    int marginLeft = 0;
    int marginTop = 0;
    int viewportHeight = 0;  // reader content height == page stride
    int linePitch = 0;       // laid-out row height (line height * line compression)
    int contentRight = 0;    // right edge of the text column (0 = unknown)
  };

  ClipSelectionActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, Section& section, int startPage,
                        Layout layout, ClipSelectionLimits limits);

  // Picks the PSRAM-aware budget for the running device.
  static ClipSelectionLimits currentLimits();

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(RenderLock&&) override;
  bool isReaderActivity() const override { return true; }
  bool handleHomeGesture() override;

 private:
  static constexpr size_t SNAPSHOT_CAPACITY = 8192;
  // Two slots cover every viewport position (a window one stride high spans at
  // most two adjacent pages); the loader frontier reuses whichever is older.
  static constexpr size_t PAGE_CACHE_SLOTS = 2;

  struct CachedPage {
    int pageIdx = -1;  // relative to startPage
    uint32_t lastUse = 0;
    std::unique_ptr<Page> page;
  };

  Section& section;
  int startPage = 0;
  int sectionPageCount = 1;
  Layout layout;
  ClipSelectionLimits limits;
  int lineHeight = 0;

  ClipWordStore wordStore;
  std::vector<uint16_t> readingOrder;  // identity order over wordStore.words

  std::array<CachedPage, PAGE_CACHE_SLOTS> pageCache{};
  uint32_t pageCacheClock = 0;

  // Loader frontier: next unread element on `frontierPage`.
  int frontierPage = 0;
  size_t frontierElement = 0;
  uint16_t frontierWordOrdinal = 0;
  bool frontierExhausted = false;
  std::vector<WordRef> lineScratch;  // one line of words being loaded

  int scrollY = 0;  // content-space offset of the visible window
  int cursorIdx = 0;
  int startMarkIdx = -1;
  unsigned long lastHorizontalMoveTime = 0;

  // Touch soft-advance state.
  int touchDwellIdx = -1;
  uint32_t touchDwellSince = 0;
  bool swallowTouchRelease = false;
  bool touchContactActive = false;

  std::unique_ptr<uint8_t[]> snapshot;
  int16_t snapshotX = 0;
  int16_t snapshotY = 0;
  int16_t snapshotW = 0;
  int16_t snapshotH = 0;
  int snapshotToken = -1;  // -1 forces full page redraw
  int snapshotScrollY = -1;

  // Loading (all under RenderLock: render() reads the same window)
  enum class LoadResult : uint8_t { Appended, BeyondLimit, Capped, Exhausted };
  Page* cachedPage(int pageIdx);
  void prepareFontForPage(const Page& page) const;
  static const TextBlock* lineBlock(const PageElement& element);
  static uint16_t visibleWordCount(const TextBlock& block);
  static bool locateLine(const Page& page, uint16_t ordinal, size_t& elementIdx, uint16_t& lineStartOrdinal);
  bool buildLineWords(const PageLine& line, int pageIdx, uint16_t firstOrdinal);
  void markParagraphStart(size_t lineFirstIdx);
  void compactPool();
  bool makeRoom(size_t lineWords, bool allowEvict, bool forAppend);
  LoadResult appendNextLine(bool allowEvict, int maxContentTop);
  LoadResult prependPreviousLine(bool allowEvict, int minContentTop);
  bool appendOneMoreLine();
  bool prependOneMoreLine();
  void seekInitialWindow();
  void loadVisibleLines();
  bool evictLeading(size_t needed);
  bool evictTrailing(size_t needed);
  void rebuildReadingOrder();
  int firstLoadedPage() const;
  int lastLoadedPage() const;

  // Viewport
  int contentTopOf(const WordRef& word) const;
  int screenYOf(const WordRef& word) const;
  bool isVisible(const WordRef& word) const;
  int visibleClipBottom();
  void ensureVisiblePagesCached();
  bool updateScrollForCursor();

  // Interaction
  void moveCursorTo(int idx);
  bool moveVertical(int direction);
  void positionCursorAtInitialCenter();
  void confirmSelection();
  int wordAt(int x, int y) const;
  bool handleTouch();

  // Drawing
  void drawHints() const;
  void drawSelectionHighlights();
  bool captureAndDrawHighlights();
  void renderPages();
  int renderToken() const;
};
