#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "WordRef.h"

namespace ClipSelectionPaging {

constexpr int TOUCH_DRAG_MOVEMENT_PX = 4;
constexpr uint32_t TOUCH_PAGE_ADVANCE_HOLD_MS = 1000;
constexpr int TOUCH_PAGE_END_DWELL_SLOP_PX = 8;

inline bool hasDraggedFrom(const int startX, const int startY, const int x, const int y) {
  const int deltaX = x - startX;
  const int deltaY = y - startY;
  return deltaX >= TOUCH_DRAG_MOVEMENT_PX || deltaX <= -TOUCH_DRAG_MOVEMENT_PX || deltaY >= TOUCH_DRAG_MOVEMENT_PX ||
         deltaY <= -TOUCH_DRAG_MOVEMENT_PX;
}

inline bool hasHeldPageEndLongEnough(const uint32_t now, const uint32_t heldSince) {
  return static_cast<uint32_t>(now - heldSince) >= TOUCH_PAGE_ADVANCE_HOLD_MS;
}

inline bool isWithinPageEndDwellSlop(const WordRef& word, const int x, const int y) {
  return x >= word.x - TOUCH_PAGE_END_DWELL_SLOP_PX && x < word.x + word.w + TOUCH_PAGE_END_DWELL_SLOP_PX &&
         y >= word.y - TOUCH_PAGE_END_DWELL_SLOP_PX && y < word.y + word.h + TOUCH_PAGE_END_DWELL_SLOP_PX;
}

// Returns the first selectable word on the next loaded page only when the
// cursor is already on the final word of its current page.
inline int nextPageStartIndex(const std::vector<WordRef>& words, const uint16_t* readingOrder,
                              const size_t readingOrderSize, const int cursorIdx) {
  if (!readingOrder || cursorIdx < 0 || cursorIdx >= static_cast<int>(readingOrderSize)) return -1;

  const uint16_t currentWordIndex = readingOrder[cursorIdx];
  if (currentWordIndex >= words.size()) return -1;

  const int currentPage = words[currentWordIndex].pageIdx;
  for (size_t orderIdx = static_cast<size_t>(cursorIdx) + 1; orderIdx < readingOrderSize; ++orderIdx) {
    const uint16_t wordIndex = readingOrder[orderIdx];
    if (wordIndex >= words.size()) return -1;

    const int pageIdx = words[wordIndex].pageIdx;
    if (pageIdx == currentPage) return -1;
    return pageIdx > currentPage ? static_cast<int>(orderIdx) : -1;
  }
  return -1;
}

// A moved touch contact may advance as soon as it reaches the final word; do
// not require a second held sample at that word before changing pages.
inline int nextPageStartIndexForTouchDrag(const bool hasDragged, const int startMarkIdx,
                                          const std::vector<WordRef>& words, const uint16_t* readingOrder,
                                          const size_t readingOrderSize, const int cursorIdx) {
  if (!hasDragged || cursorIdx < startMarkIdx) return -1;
  return nextPageStartIndex(words, readingOrder, readingOrderSize, cursorIdx);
}


// ---------------------------------------------------------------------------
// Soft advance ("跨页时不直接翻页，而是多加载一行").
//
// Loaded pages are stacked in one continuous content column: page N starts at
// N * pageStride (the reader viewport height), so content-space y is
//   word.y - marginTop + word.pageIdx * pageStride.
// The selector shows a viewport-high window of that column starting at
// `scrollY`. Moving the cursor below the window scrolls it by whole lines, so
// crossing a page boundary reveals exactly one more line at the bottom instead
// of replacing the whole screen with the next page.
// ---------------------------------------------------------------------------

inline int contentTop(const WordRef& word, const int marginTop, const int pageStride) {
  return word.y - marginTop + word.pageIdx * pageStride;
}

inline int contentBottom(const WordRef& word, const int marginTop, const int pageStride) {
  return contentTop(word, marginTop, pageStride) + word.h;
}

// Returns the scroll offset that keeps `cursorWordIdx` fully inside
// [scrollY, scrollY + viewportHeight). Forward moves pick the smallest loaded
// line top that still shows the cursor line (one line per step for uniform
// lines); backward moves make the cursor line the top line. Never negative.
inline int softScrollFor(const std::vector<WordRef>& words, const int marginTop, const int pageStride,
                         const int viewportHeight, const int scrollY, const int cursorWordIdx) {
  if (cursorWordIdx < 0 || cursorWordIdx >= static_cast<int>(words.size()) || viewportHeight <= 0) return scrollY;
  const WordRef& cursor = words[cursorWordIdx];
  const int top = contentTop(cursor, marginTop, pageStride);
  const int bottom = top + cursor.h;
  int next = scrollY;
  if (top < scrollY) {
    next = top;
  } else if (bottom > scrollY + viewportHeight) {
    const int minTop = bottom - viewportHeight;
    int best = top;
    for (const WordRef& word : words) {
      const int lineTop = contentTop(word, marginTop, pageStride);
      if (lineTop >= minTop && lineTop < best) best = lineTop;
    }
    next = best;
  }
  return next < 0 ? 0 : next;
}

// True when the word lies completely inside the visible window.
inline bool isWordInWindow(const WordRef& word, const int marginTop, const int pageStride, const int viewportHeight,
                           const int scrollY) {
  const int top = contentTop(word, marginTop, pageStride);
  return top >= scrollY && top + word.h <= scrollY + viewportHeight;
}

// Number of leading words that may be evicted to make room for `needed` more
// words without touching the selection: eviction stops at a line boundary and
// never reaches `keepFromIdx` (the first word of the line holding the start
// mark / cursor). Returns 0 when no line-aligned eviction is possible.
inline size_t evictableLeadingWords(const std::vector<WordRef>& words, const size_t needed, const size_t keepFromIdx) {
  if (needed == 0 || words.empty()) return 0;
  size_t count = needed < words.size() ? needed : words.size();
  // Round up to the end of the line containing words[count - 1].
  while (count < words.size() && words[count].pageIdx == words[count - 1].pageIdx &&
         words[count].y == words[count - 1].y) {
    ++count;
  }
  return count <= keepFromIdx ? count : 0;
}

// Trailing counterpart of evictableLeadingWords: evicts whole lines from the
// end, never reaching `keepThroughIdx` (the last word of the line holding the
// start mark / cursor).
inline size_t evictableTrailingWords(const std::vector<WordRef>& words, const size_t needed,
                                     const size_t keepThroughIdx) {
  if (needed == 0 || words.empty()) return 0;
  size_t keep = words.size() - (needed < words.size() ? needed : words.size());
  // Round down to the start of the line containing words[keep].
  while (keep > 0 && keep < words.size() && words[keep].pageIdx == words[keep - 1].pageIdx &&
         words[keep].y == words[keep - 1].y) {
    --keep;
  }
  return keep > keepThroughIdx ? words.size() - keep : 0;
}

// Last word index of the line containing `idx`.
inline size_t lineEndIndex(const std::vector<WordRef>& words, size_t idx) {
  if (idx >= words.size()) return words.empty() ? 0 : words.size() - 1;
  while (idx + 1 < words.size() && words[idx + 1].pageIdx == words[idx].pageIdx && words[idx + 1].y == words[idx].y)
    ++idx;
  return idx;
}

// First word index of the line containing `idx`.
inline size_t lineStartIndex(const std::vector<WordRef>& words, size_t idx) {
  if (idx >= words.size()) return words.size();
  while (idx > 0 && words[idx - 1].pageIdx == words[idx].pageIdx && words[idx - 1].y == words[idx].y) --idx;
  return idx;
}

}  // namespace ClipSelectionPaging
