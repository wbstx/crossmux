#include "ClipSelectionActivity.h"

#include <Epub/Section.h>
#include <FontCacheManager.h>
#include <GfxRenderer.h>
#include <HalMemory.h>
#include <I18n.h>
#include <Logging.h>

#include <algorithm>
#include <climits>
#include <cstring>
#include <string>

#include "ClipSelectionPaging.h"
#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "activities/ActivityResult.h"
#include "clippings/ClipTextBuilder.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {

constexpr unsigned long WORD_REPEAT_START_MS = 500;
constexpr unsigned long WORD_REPEAT_INTERVAL_MS = 500;

bool hasEmSpace(const char* text) { return text && text[0] == '\xe2' && text[1] == '\x80' && text[2] == '\x83'; }

bool hasVisibleText(const char* text) {
  if (!text) return false;
  for (const auto* p = reinterpret_cast<const uint8_t*>(text); *p; ++p) {
    if (*p > 0x20) return true;
  }
  return false;
}

int elementHeight(const PageElement& element, const int lineHeight) {
  switch (element.getTag()) {
    case TAG_PageImage:
      return static_cast<const PageImage&>(element).getImageBlock().getHeight();
    case TAG_PageLine:
      return lineHeight;
    default:
      return 4;
  }
}

}  // namespace

ClipSelectionLimits ClipSelectionActivity::currentLimits() {
  bool psramAvailable = false;
  size_t psramLargest = 0;
#if defined(SIMULATOR) && defined(SIMULATOR_DEVICE_READPICO) && !defined(BOARD_HAS_PSRAM)
  // The Read Pico hardware has 8 MB PSRAM; its simulator profile omits the
  // flag, so model the device budget instead of the host mock.
  psramAvailable = true;
  psramLargest = 8U * 1024U * 1024U;
#elif defined(BOARD_HAS_PSRAM)
  const auto psram = HalMemory::getPsramHeap();
  psramAvailable = psram.totalBytes > 0;
  psramLargest = psram.largestBlockBytes;
#endif
  const auto heap = HalMemory::getDefaultHeap();
  const ClipSelectionLimits limits =
      ClipSelectionLimitPolicy::choose(psramAvailable, psramLargest, heap.largestBlockBytes);
  LOG_INF("CLIP", "Selection budget: %s tier, %u pages / %u words (psram=%d largest=%u, heap largest=%u)",
          limits.psramTier ? "psram" : (limits.maxWords == ClipSelectionLimitPolicy::CLASSIC.maxWords ? "classic" : "low"),
          static_cast<unsigned>(limits.maxPages), static_cast<unsigned>(limits.maxWords), psramAvailable ? 1 : 0,
          static_cast<unsigned>(psramLargest), static_cast<unsigned>(heap.largestBlockBytes));
  return limits;
}

ClipSelectionActivity::ClipSelectionActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, Section& section,
                                             const int startPage, const Layout layout,
                                             const ClipSelectionLimits limits)
    : Activity("ClipSelection", renderer, mappedInput),
      section(section),
      startPage(startPage),
      sectionPageCount(std::max<int>(1, section.pageCount)),
      layout(layout),
      limits(limits) {}

void ClipSelectionActivity::onEnter() {
  Activity::onEnter();
  bool ok = false;
  {
    RenderLock lock(*this);
    // Rows are laid out at the compressed pitch; using it for word height keeps
    // the last row of a page inside the viewport and highlight bands tiled.
    lineHeight = layout.linePitch > 0 ? layout.linePitch : renderer.getLineHeight(layout.fontId);
    if (layout.viewportHeight <= lineHeight) {
      layout.viewportHeight = renderer.getScreenHeight() - layout.marginTop;
    }
    snapshot = makeUniqueNoThrow<uint8_t[]>(SNAPSHOT_CAPACITY);
    // Reserved once for the whole session; justified by limits (see ClipSelectionLimits.h).
    wordStore.words.reserve(limits.maxWords);
    wordStore.textPool.reserve(limits.textPoolReserve);
    readingOrder.reserve(limits.maxWords);
    lineScratch.reserve(96);  // one text line; grows only for unusually dense lines

    seekInitialWindow();
    loadVisibleLines();
    rebuildReadingOrder();
    ok = !wordStore.words.empty();
    if (ok) positionCursorAtInitialCenter();
    LOG_DBG("CLIP", "Selection opened: page=%d words=%u tier=%s", startPage,
            static_cast<unsigned>(wordStore.words.size()), limits.psramTier ? "psram" : "classic");
  }

  if (!ok) {
    LOG_ERR("CLIP", "No selectable words on current EPUB page");
    ActivityResult result;
    result.isCancelled = true;
    setResult(std::move(result));
    finish();
    return;
  }
  requestUpdate();
}

void ClipSelectionActivity::onExit() {
  {
    RenderLock lock(*this);
    for (auto& slot : pageCache) {
      slot.page.reset();
      slot.pageIdx = -1;
    }
    snapshot.reset();
  }
  Activity::onExit();
}

bool ClipSelectionActivity::handleHomeGesture() {
  ActivityResult result;
  result.isCancelled = true;
  setResult(std::move(result));
  finish();
  return true;
}

// ---------------------------------------------------------------------------
// Loading
// ---------------------------------------------------------------------------

void ClipSelectionActivity::prepareFontForPage(const Page& page) const {
  std::string pageText;
  pageText.reserve(2048);
  uint8_t styleMask = 0;
  for (const auto& element : page.elements) {
    if (element->getTag() != TAG_PageLine) continue;
    const auto* block = static_cast<const PageLine*>(element.get())->getBlock().get();
    if (!block || !block->valid()) continue;
    for (uint16_t i = 0; i < block->wordCount(); ++i) {
      const char* text = block->wordText(i);
      if (!hasVisibleText(text)) continue;
      pageText.append(text);
      pageText.push_back(' ');
      styleMask |= static_cast<uint8_t>(1u << (static_cast<uint8_t>(block->wordStyle(i)) & 0x03));
    }
  }
  if (styleMask == 0) styleMask = 0x01;
  renderer.ensureSdCardFontReady(layout.fontId, pageText.c_str(), styleMask);
}

Page* ClipSelectionActivity::cachedPage(const int pageIdx) {
  if (pageIdx < 0 || startPage + pageIdx >= sectionPageCount) return nullptr;
  for (auto& slot : pageCache) {
    if (slot.page && slot.pageIdx == pageIdx) {
      slot.lastUse = ++pageCacheClock;
      return slot.page.get();
    }
  }
  CachedPage* victim = &pageCache[0];
  for (auto& slot : pageCache) {
    if (!slot.page) {
      victim = &slot;
      break;
    }
    if (slot.lastUse < victim->lastUse) victim = &slot;
  }
  // Release the old page before reading the next so two pages never coexist
  // with a third mid-load on the classic heap.
  victim->page.reset();
  victim->pageIdx = -1;
  auto page = section.loadPage(startPage + pageIdx);
  if (!page) {
    LOG_ERR("CLIP", "Failed to load selection page %d", startPage + pageIdx);
    return nullptr;
  }
  prepareFontForPage(*page);
  victim->page = std::move(page);
  victim->pageIdx = pageIdx;
  victim->lastUse = ++pageCacheClock;
  return victim->page.get();
}

int ClipSelectionActivity::firstLoadedPage() const {
  return wordStore.words.empty() ? -1 : wordStore.words.front().pageIdx;
}

int ClipSelectionActivity::lastLoadedPage() const {
  return wordStore.words.empty() ? -1 : wordStore.words.back().pageIdx;
}

void ClipSelectionActivity::rebuildReadingOrder() {
  readingOrder.resize(wordStore.words.size());
  for (size_t i = 0; i < readingOrder.size(); ++i) readingOrder[i] = static_cast<uint16_t>(i);
}

const TextBlock* ClipSelectionActivity::lineBlock(const PageElement& element) {
  if (element.getTag() != TAG_PageLine) return nullptr;
  const auto* block = static_cast<const PageLine&>(element).getBlock().get();
  return block && block->valid() ? block : nullptr;
}

uint16_t ClipSelectionActivity::visibleWordCount(const TextBlock& block) {
  uint16_t count = 0;
  for (uint16_t i = 0; i < block.wordCount(); ++i) {
    if (hasVisibleText(block.wordText(i))) ++count;
  }
  return count;
}

bool ClipSelectionActivity::locateLine(const Page& page, const uint16_t ordinal, size_t& elementIdx,
                                       uint16_t& lineStartOrdinal) {
  uint16_t seen = 0;
  for (size_t e = 0; e < page.elements.size(); ++e) {
    const TextBlock* block = lineBlock(*page.elements[e]);
    if (!block) continue;
    const uint16_t count = visibleWordCount(*block);
    if (count > 0 && ordinal < seen + count) {
      elementIdx = e;
      lineStartOrdinal = seen;
      return true;
    }
    seen = static_cast<uint16_t>(seen + count);
  }
  return false;
}

void ClipSelectionActivity::compactPool() {
  // Offsets are uint16_t into the pool, so text of evicted words must not keep
  // counting against the 64 KiB space. The temporary copy is bounded by the
  // live pool.
  std::string pool;
  pool.reserve(std::max<size_t>(wordStore.textPool.size(), limits.textPoolReserve));
  for (WordRef& word : wordStore.words) {
    const char* text = wordStore.text(word);
    word.textOffset = static_cast<uint16_t>(pool.size());
    pool.append(text, word.textLength);
    pool.push_back('\0');
  }
  wordStore.textPool.swap(pool);
}

bool ClipSelectionActivity::buildLineWords(const PageLine& line, const int pageIdx, const uint16_t firstOrdinal) {
  lineScratch.clear();
  const auto* block = line.getBlock().get();
  if (!block || !block->valid()) return false;
  const int ascender = renderer.getFontAscenderSize(layout.fontId);
  const int rubyShift = block->getRubyShift(ascender);
  uint16_t ordinal = firstOrdinal;
  for (uint16_t i = 0; i < block->wordCount(); ++i) {
    const char* text = block->wordText(i);
    if (!hasVisibleText(text)) continue;
    WordRef word;
    word.x = line.xPos + block->wordXpos(i) + layout.marginLeft;
    word.y = line.yPos + layout.marginTop + rubyShift;
    word.h = lineHeight;
    word.pageIdx = pageIdx;
    word.pageWordIndex = ordinal++;
    word.style = block->wordStyle(i);
    word.w = renderer.getTextAdvanceX(layout.fontId, text, word.style);
    if (!wordStore.appendText(word, text)) return false;  // capacity pre-checked by callers
    lineScratch.push_back(word);
  }
  return !lineScratch.empty();
}

void ClipSelectionActivity::markParagraphStart(const size_t lineFirstIdx) {
  auto& words = wordStore.words;
  if (lineFirstIdx >= words.size()) return;
  WordRef& first = words[lineFirstIdx];
  const char* firstText = wordStore.text(first);
  bool start = hasEmSpace(firstText);
  if (!start && lineFirstIdx > 0) {
    const WordRef& prevLast = words[lineFirstIdx - 1];
    const WordRef& prevFirst = words[ClipSelectionPaging::lineStartIndex(words, lineFirstIdx - 1)];
    const char* prevText = wordStore.text(prevLast);
    const size_t len = prevLast.textLength;
    const bool prevEndsWithHyphen = len > 0 && prevText[len - 1] == '-';
    size_t lead = len > 0 ? len - 1 : 0;
    while (lead > 0 && (static_cast<uint8_t>(prevText[lead]) & 0xC0) == 0x80) --lead;
    const bool prevEndsWide = len > 0 && (static_cast<uint8_t>(prevText[lead]) & 0xF0) == 0xE0;
    const bool byIndent = first.x > prevFirst.x + lineHeight / 2 && !prevEndsWithHyphen;
    // CJK books often drop first-line indents; a wide-script line that stops
    // more than a glyph short of the column edge ended its paragraph.
    // Restricted to CJK so ragged-right Latin text is not split.
    const bool byShortCjkLine =
        layout.contentRight > 0 && prevEndsWide && prevLast.x + prevLast.w < layout.contentRight - lineHeight;
    start = byIndent || byShortCjkLine;
  }
  first.paragraphStart = start;
}

bool ClipSelectionActivity::evictLeading(const size_t needed) {
  auto& words = wordStore.words;
  if (words.empty()) return false;
  const int anchor = startMarkIdx >= 0 ? std::min(startMarkIdx, cursorIdx) : cursorIdx;
  const size_t keepFrom = ClipSelectionPaging::lineStartIndex(words, static_cast<size_t>(std::max(0, anchor)));
  const size_t count = ClipSelectionPaging::evictableLeadingWords(words, needed, keepFrom);
  if (count == 0) return false;

  words.erase(words.begin(), words.begin() + static_cast<std::ptrdiff_t>(count));
  compactPool();
  cursorIdx -= static_cast<int>(count);
  if (startMarkIdx >= 0) startMarkIdx -= static_cast<int>(count);
  touchDwellIdx = -1;
  snapshotToken = -1;
  rebuildReadingOrder();
  LOG_DBG("CLIP", "Evicted %u leading words; window now %u words from page %d", static_cast<unsigned>(count),
          static_cast<unsigned>(words.size()), firstLoadedPage());
  return true;
}

bool ClipSelectionActivity::evictTrailing(const size_t needed) {
  auto& words = wordStore.words;
  if (words.empty()) return false;
  const int anchor = std::max(startMarkIdx, cursorIdx);
  const size_t keepThrough = ClipSelectionPaging::lineEndIndex(words, static_cast<size_t>(std::max(0, anchor)));
  const size_t count = ClipSelectionPaging::evictableTrailingWords(words, needed, keepThrough);
  if (count == 0) return false;

  // Rewind the forward frontier to the first evicted line so it reloads later.
  const WordRef firstEvicted = words[words.size() - count];
  const Page* page = cachedPage(firstEvicted.pageIdx);
  size_t elementIdx = 0;
  uint16_t lineStartOrdinal = 0;
  if (!page || !locateLine(*page, firstEvicted.pageWordIndex, elementIdx, lineStartOrdinal)) return false;

  words.resize(words.size() - count);
  compactPool();
  frontierPage = firstEvicted.pageIdx;
  frontierElement = elementIdx;
  frontierWordOrdinal = lineStartOrdinal;
  frontierExhausted = false;
  touchDwellIdx = -1;
  snapshotToken = -1;
  rebuildReadingOrder();
  LOG_DBG("CLIP", "Evicted %u trailing words; window now %u words to page %d", static_cast<unsigned>(count),
          static_cast<unsigned>(words.size()), lastLoadedPage());
  return true;
}

ClipSelectionActivity::LoadResult ClipSelectionActivity::appendNextLine(const bool allowEvict,
                                                                        const int maxContentTop) {
  const int stride = layout.viewportHeight;
  auto& words = wordStore.words;
  while (true) {
    if (frontierExhausted) return LoadResult::Exhausted;
    Page* page = cachedPage(frontierPage);
    if (!page) {
      frontierExhausted = true;
      return LoadResult::Exhausted;
    }

    const PageLine* line = nullptr;
    uint16_t lineWords = 0;
    for (; frontierElement < page->elements.size(); ++frontierElement) {
      const TextBlock* block = lineBlock(*page->elements[frontierElement]);
      if (!block) continue;
      lineWords = visibleWordCount(*block);
      if (lineWords == 0) continue;
      line = static_cast<const PageLine*>(page->elements[frontierElement].get());
      break;
    }

    if (!line) {
      const int nextPage = frontierPage + 1;
      if (startPage + nextPage >= sectionPageCount) {
        frontierExhausted = true;
        return LoadResult::Exhausted;
      }
      if (nextPage * stride >= maxContentTop) return LoadResult::BeyondLimit;
      const int first = firstLoadedPage();
      if (first >= 0 && nextPage - first + 1 > limits.maxPages) {
        if (!allowEvict) return LoadResult::Capped;
        size_t onFirstPage = 0;
        while (onFirstPage < words.size() && words[onFirstPage].pageIdx == first) ++onFirstPage;
        if (!evictLeading(onFirstPage)) {
          LOG_DBG("CLIP", "Selection page cap (%u) reached", static_cast<unsigned>(limits.maxPages));
          return LoadResult::Capped;
        }
      }
      frontierPage = nextPage;
      frontierElement = 0;
      frontierWordOrdinal = 0;
      continue;
    }

    if (frontierPage * stride + line->yPos >= maxContentTop) return LoadResult::BeyondLimit;
    if (!makeRoom(lineWords, allowEvict, /*forAppend=*/true)) return LoadResult::Capped;

    if (!buildLineWords(*line, frontierPage, frontierWordOrdinal)) return LoadResult::Capped;
    const size_t lineFirstIdx = words.size();
    words.insert(words.end(), lineScratch.begin(), lineScratch.end());
    markParagraphStart(lineFirstIdx);
    frontierWordOrdinal = static_cast<uint16_t>(frontierWordOrdinal + lineScratch.size());
    ++frontierElement;
    rebuildReadingOrder();
    return LoadResult::Appended;
  }
}

ClipSelectionActivity::LoadResult ClipSelectionActivity::prependPreviousLine(const bool allowEvict,
                                                                             const int minContentTop) {
  const int stride = layout.viewportHeight;
  auto& words = wordStore.words;
  if (words.empty()) return LoadResult::Exhausted;

  const WordRef head = words.front();
  int pageIdx = head.pageIdx;
  Page* page = cachedPage(pageIdx);
  size_t headElement = 0;
  uint16_t headOrdinal = 0;
  if (!page || !locateLine(*page, head.pageWordIndex, headElement, headOrdinal)) return LoadResult::Exhausted;

  // Previous non-empty line on this page, else the last one on the previous
  // page. The selector never reaches before the reader's current page.
  bool found = false;
  size_t lineElement = 0;
  uint16_t lineWords = 0;
  uint16_t lineOrdinal = 0;
  for (size_t e = headElement; e-- > 0;) {
    const TextBlock* block = lineBlock(*page->elements[e]);
    if (!block) continue;
    lineWords = visibleWordCount(*block);
    if (lineWords == 0) continue;
    found = true;
    lineElement = e;
    lineOrdinal = static_cast<uint16_t>(headOrdinal - lineWords);
    break;
  }
  if (!found) {
    if (pageIdx == 0) return LoadResult::Exhausted;
    const int last = lastLoadedPage();
    if (last - (pageIdx - 1) + 1 > limits.maxPages) {
      if (!allowEvict) return LoadResult::Capped;
      size_t onLastPage = 0;
      while (onLastPage < words.size() && words[words.size() - 1 - onLastPage].pageIdx == last) ++onLastPage;
      if (!evictTrailing(onLastPage)) return LoadResult::Capped;
    }
    --pageIdx;
    page = cachedPage(pageIdx);
    if (!page) return LoadResult::Exhausted;
    uint16_t total = 0;
    for (size_t e = 0; e < page->elements.size(); ++e) {
      const TextBlock* block = lineBlock(*page->elements[e]);
      if (!block) continue;
      const uint16_t count = visibleWordCount(*block);
      if (count == 0) continue;
      found = true;
      lineElement = e;
      lineWords = count;
      lineOrdinal = total;
      total = static_cast<uint16_t>(total + count);
    }
    if (!found) return LoadResult::Exhausted;
  }

  if (pageIdx * stride + page->elements[lineElement]->yPos < minContentTop) return LoadResult::BeyondLimit;
  if (!makeRoom(lineWords, allowEvict, /*forAppend=*/false)) return LoadResult::Capped;
  // Trailing eviction may rewind the frontier through the page cache; re-fetch.
  page = cachedPage(pageIdx);
  if (!page || lineElement >= page->elements.size()) return LoadResult::Exhausted;
  const auto& line = static_cast<const PageLine&>(*page->elements[lineElement]);
  if (!buildLineWords(line, pageIdx, lineOrdinal)) return LoadResult::Capped;

  const size_t count = lineScratch.size();
  words.insert(words.begin(), lineScratch.begin(), lineScratch.end());
  cursorIdx += static_cast<int>(count);
  if (startMarkIdx >= 0) startMarkIdx += static_cast<int>(count);
  if (touchDwellIdx >= 0) touchDwellIdx += static_cast<int>(count);
  snapshotToken = -1;
  markParagraphStart(0);
  markParagraphStart(count);  // the old head now has a previous line
  rebuildReadingOrder();
  return LoadResult::Appended;
}

bool ClipSelectionActivity::makeRoom(const size_t lineWords, const bool allowEvict, const bool forAppend) {
  const auto fits = [&] {
    return wordStore.words.size() + lineWords <= limits.maxWords &&
           wordStore.canAppend(lineWords * 64U);  // generous per-token UTF-8 bound
  };
  if (fits()) return true;
  if (wordStore.words.size() + lineWords <= limits.maxWords) {
    compactPool();
    if (fits()) return true;
  }
  if (!allowEvict) return false;
  const size_t over =
      wordStore.words.size() + lineWords > limits.maxWords ? wordStore.words.size() + lineWords - limits.maxWords : 1;
  const bool evicted = forAppend ? evictLeading(over) : evictTrailing(over);
  if (!evicted || !fits()) {
    LOG_DBG("CLIP", "Selection word cap (%u) reached", static_cast<unsigned>(limits.maxWords));
    return false;
  }
  return true;
}

void ClipSelectionActivity::seekInitialWindow() {
  // A page denser than the word cap (CJK on the classic tier) starts with a
  // window centred on the page instead of its first lines, matching the
  // centred initial cursor. Lines above/below load on demand.
  Page* page = cachedPage(0);
  if (!page) return;
  size_t total = 0;
  for (const auto& element : page->elements) {
    if (const TextBlock* block = lineBlock(*element)) total += visibleWordCount(*block);
  }
  if (total <= limits.maxWords) return;
  const size_t skipTarget = (total - limits.maxWords) / 2;
  size_t skipped = 0;
  for (size_t e = 0; e < page->elements.size(); ++e) {
    const TextBlock* block = lineBlock(*page->elements[e]);
    if (!block) continue;
    const uint16_t count = visibleWordCount(*block);
    if (skipped + count > skipTarget) {
      frontierElement = e;
      frontierWordOrdinal = static_cast<uint16_t>(skipped);
      return;
    }
    skipped += count;
  }
}

void ClipSelectionActivity::loadVisibleLines() {
  // Fill the visible window without evicting, so lines that start inside the
  // window are selectable and touchable. Stops at the caps.
  const int bottomLimit = scrollY + layout.viewportHeight;
  while (appendNextLine(false, bottomLimit) == LoadResult::Appended) {
  }
  while (prependPreviousLine(false, scrollY) == LoadResult::Appended) {
  }
}

bool ClipSelectionActivity::appendOneMoreLine() { return appendNextLine(true, INT_MAX) == LoadResult::Appended; }

bool ClipSelectionActivity::prependOneMoreLine() {
  return prependPreviousLine(true, INT_MIN) == LoadResult::Appended;
}

// ---------------------------------------------------------------------------
// Viewport
// ---------------------------------------------------------------------------

int ClipSelectionActivity::contentTopOf(const WordRef& word) const {
  return ClipSelectionPaging::contentTop(word, layout.marginTop, layout.viewportHeight);
}

int ClipSelectionActivity::screenYOf(const WordRef& word) const {
  return layout.marginTop + contentTopOf(word) - scrollY;
}

bool ClipSelectionActivity::isVisible(const WordRef& word) const {
  return ClipSelectionPaging::isWordInWindow(word, layout.marginTop, layout.viewportHeight, layout.viewportHeight,
                                             scrollY);
}

bool ClipSelectionActivity::updateScrollForCursor() {
  if (wordStore.words.empty()) return false;
  const int next = ClipSelectionPaging::softScrollFor(wordStore.words, layout.marginTop, layout.viewportHeight,
                                                      layout.viewportHeight, scrollY, cursorIdx);
  if (next == scrollY) return false;
  LOG_DBG("CLIP", "Soft advance: scroll %d -> %d (cursor page %d)", scrollY, next,
          wordStore.words[cursorIdx].pageIdx);
  scrollY = next;
  snapshotToken = -1;
  loadVisibleLines();
  return true;
}

int ClipSelectionActivity::visibleClipBottom() {
  const int stride = layout.viewportHeight;
  const int viewBottom = layout.marginTop + layout.viewportHeight;
  int cut = viewBottom;
  const int firstPage = scrollY / stride;
  const int lastPage = (scrollY + layout.viewportHeight - 1) / stride;
  for (int p = firstPage; p <= lastPage; ++p) {
    const Page* page = cachedPage(p);
    if (!page) continue;
    const int pageTop = layout.marginTop + p * stride - scrollY;
    for (const auto& element : page->elements) {
      const int top = pageTop + element->yPos;
      const int bottom = top + elementHeight(*element, lineHeight);
      if (top < viewBottom && bottom > viewBottom) cut = std::min(cut, top);
    }
  }
  return std::max(layout.marginTop, cut);
}


void ClipSelectionActivity::ensureVisiblePagesCached() {
  const int stride = layout.viewportHeight;
  if (stride <= 0) return;
  const int firstPage = scrollY / stride;
  const int lastPage = (scrollY + layout.viewportHeight - 1) / stride;
  for (int p = firstPage; p <= lastPage; ++p) (void)cachedPage(p);
}

// ---------------------------------------------------------------------------
// Interaction
// ---------------------------------------------------------------------------

void ClipSelectionActivity::positionCursorAtInitialCenter() {
  const int midY = layout.marginTop + layout.viewportHeight / 2;
  const int midX = renderer.getScreenWidth() / 2;
  int best = 0;
  int bestDist = INT_MAX;
  for (size_t i = 0; i < wordStore.words.size(); ++i) {
    const WordRef& word = wordStore.words[i];
    if (word.pageIdx != 0) break;
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
  for (size_t i = 0; i < wordStore.words.size(); ++i) {
    const WordRef& word = wordStore.words[i];
    if (!isVisible(word)) continue;
    const int wy = screenYOf(word);
    if (x >= word.x - SLOP && x < word.x + word.w + SLOP && y >= wy - SLOP && y < wy + word.h + SLOP) {
      return static_cast<int>(i);
    }
  }
  return -1;
}

void ClipSelectionActivity::moveCursorTo(const int idx) {
  if (idx < 0 || idx >= static_cast<int>(wordStore.words.size())) return;
  if (idx == cursorIdx) return;
  cursorIdx = idx;
  updateScrollForCursor();
  requestUpdate();
}

bool ClipSelectionActivity::moveVertical(const int direction) {
  auto findTarget = [this, direction]() {
    const WordRef& current = wordStore.words[cursorIdx];
    const int currentTop = contentTopOf(current);
    const int currentCenter = current.x + current.w / 2;
    int best = -1;
    int bestScore = INT_MAX;
    for (size_t i = 0; i < wordStore.words.size(); ++i) {
      const WordRef& word = wordStore.words[i];
      const int top = contentTopOf(word);
      if (direction > 0 ? top <= currentTop : top >= currentTop) continue;
      const int rowDelta = std::abs(top - currentTop);
      const int xDelta = std::abs(word.x + word.w / 2 - currentCenter);
      const int score = rowDelta * 1000 + xDelta;
      if (score < bestScore) {
        bestScore = score;
        best = static_cast<int>(i);
      }
    }
    return best;
  };

  if (wordStore.words.empty()) return false;
  int target = findTarget();
  // Past the loaded window: load exactly one more line in that direction.
  if (target < 0 && (direction > 0 ? appendOneMoreLine() : prependOneMoreLine())) target = findTarget();
  if (target < 0 || target == cursorIdx) return false;
  moveCursorTo(target);
  return true;
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
  auto result = ClipTextBuilder::build(wordStore, readingOrder.data(), from, to, startPage, sectionPageCount, nullptr);
  setResult(std::move(result));
  finish();
}

bool ClipSelectionActivity::handleTouch() {
  int tx = 0;
  int ty = 0;
  // Release edge first: while the finger is down, isScreenTouchHeld() owns the
  // contact (wasScreenTouchDown() keeps reporting a stationary contact, so it
  // cannot be used to detect a new press).
  if (mappedInput.wasScreenTapped(tx, ty)) {
    const bool swallow = swallowTouchRelease;
    touchContactActive = false;
    swallowTouchRelease = false;
    touchDwellIdx = -1;
    if (swallow) return true;  // the hold already soft-advanced; lifting must not mark/save
    const int hit = wordAt(tx, ty);
    if (hit >= 0) {
      {
        RenderLock lock(*this);
        cursorIdx = hit;
      }
      confirmSelection();
    }
    return true;
  }
  if (mappedInput.wasScreenTouchReleased()) {
    touchContactActive = false;
    swallowTouchRelease = false;
    touchDwellIdx = -1;
    return true;
  }

  if (!mappedInput.isScreenTouchHeld(tx, ty)) return false;

  RenderLock lock(*this);
  if (!touchContactActive) {
    touchContactActive = true;
    swallowTouchRelease = false;
    touchDwellIdx = -1;
  }
  // Press-and-drag moves the cursor with the finger.
  const int hit = wordAt(tx, ty);
  if (hit >= 0 && hit != cursorIdx) {
    cursorIdx = hit;
    requestUpdate();
  }

  // Holding on the last fully visible line soft-advances one line per
  // TOUCH_PAGE_ADVANCE_HOLD_MS, instead of CrossInk's whole-page switch. After
  // the scroll, the finger rests on the new bottom line, so a continued hold
  // keeps revealing one more line per interval.
  const WordRef& cursor = wordStore.words[cursorIdx];
  const int cursorTop = contentTopOf(cursor);
  bool onBottomLine = isVisible(cursor);
  if (onBottomLine) {
    for (size_t i = static_cast<size_t>(cursorIdx) + 1; i < wordStore.words.size(); ++i) {
      const WordRef& word = wordStore.words[i];
      if (contentTopOf(word) > cursorTop) {
        onBottomLine = !isVisible(word);
        break;
      }
    }
  }
  WordRef screenCursor = cursor;
  screenCursor.y = screenYOf(cursor);
  if (onBottomLine && ClipSelectionPaging::isWithinPageEndDwellSlop(screenCursor, tx, ty)) {
    const uint32_t now = static_cast<uint32_t>(millis());
    if (touchDwellIdx != cursorIdx) {
      touchDwellIdx = cursorIdx;
      touchDwellSince = now;
    } else if (ClipSelectionPaging::hasHeldPageEndLongEnough(now, touchDwellSince)) {
      swallowTouchRelease = true;
      touchDwellIdx = -1;
      moveVertical(1);
    }
  } else {
    touchDwellIdx = -1;
  }
  return true;
}

void ClipSelectionActivity::loop() {
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    if (startMarkIdx != -1) {
      startMarkIdx = -1;
      snapshotToken = -1;
      requestUpdate();
      return;
    }
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

  if (wordStore.words.empty()) return;
  if (handleTouch()) return;

  const unsigned long now = millis();
  const bool repeat =
      mappedInput.getHeldTime() >= WORD_REPEAT_START_MS && now - lastHorizontalMoveTime >= WORD_REPEAT_INTERVAL_MS;
  const bool moveLeft = mappedInput.wasPressed(MappedInputManager::Button::ScreenLeft) ||
                        (repeat && mappedInput.isPressed(MappedInputManager::Button::ScreenLeft));
  const bool moveRight = mappedInput.wasPressed(MappedInputManager::Button::ScreenRight) ||
                         (repeat && mappedInput.isPressed(MappedInputManager::Button::ScreenRight));
  const bool moveUp = mappedInput.wasPressed(MappedInputManager::Button::ScreenUp);
  const bool moveDown = mappedInput.wasPressed(MappedInputManager::Button::ScreenDown);
  if (!moveLeft && !moveRight && !moveUp && !moveDown) return;

  // Loading/eviction mutate the word window that render() reads.
  RenderLock lock(*this);
  if (moveLeft) {
    if (cursorIdx == 0) prependOneMoreLine();
    if (cursorIdx > 0) moveCursorTo(cursorIdx - 1);
    lastHorizontalMoveTime = now;
  } else if (moveRight) {
    // At the end of the loaded window, load exactly one more line (possibly
    // from the next page) rather than flipping the whole page.
    if (cursorIdx + 1 >= static_cast<int>(wordStore.words.size())) appendOneMoreLine();
    if (cursorIdx + 1 < static_cast<int>(wordStore.words.size())) moveCursorTo(cursorIdx + 1);
    lastHorizontalMoveTime = now;
  } else if (moveUp) {
    moveVertical(-1);
  } else if (moveDown) {
    moveVertical(1);
  }
}

// ---------------------------------------------------------------------------
// Drawing
// ---------------------------------------------------------------------------

int ClipSelectionActivity::renderToken() const { return cursorIdx + (startMarkIdx + 1) * 4096; }

void ClipSelectionActivity::drawSelectionHighlights() {
  if (wordStore.words.empty()) return;
  const int from = startMarkIdx >= 0 ? std::min(startMarkIdx, cursorIdx) : cursorIdx;
  const int to = startMarkIdx >= 0 ? std::max(startMarkIdx, cursorIdx) : cursorIdx;
  for (int i = from; i <= to; ++i) {
    const WordRef& word = wordStore.words[readingOrder[i]];
    if (!isVisible(word)) continue;
    const auto textStyle = static_cast<EpdFontFamily::Style>(word.style & ~EpdFontFamily::UNDERLINE);
    const int skipX =
        hasEmSpace(wordStore.text(word)) ? renderer.getTextAdvanceX(layout.fontId, "\xe2\x80\x83", textStyle) : 0;
    const int drawX = word.x + skipX;
    const int drawW = word.w - skipX;
    const int drawY = screenYOf(word);
    if (drawW <= 0) continue;
    for (int y = drawY; y < drawY + word.h; y += 2) {
      for (int x = drawX; x < drawX + drawW; x += 2) {
        renderer.drawPixel(x, y, true);
      }
    }
    if (i == cursorIdx) {
      renderer.drawRect(drawX - 1, drawY - 1, drawW + 2, word.h + 2, true);
    }
  }
}

bool ClipSelectionActivity::captureAndDrawHighlights() {
  const int from = startMarkIdx >= 0 ? std::min(startMarkIdx, cursorIdx) : cursorIdx;
  const int to = startMarkIdx >= 0 ? std::max(startMarkIdx, cursorIdx) : cursorIdx;
  int minX = INT_MAX, minY = INT_MAX, maxX = 0, maxY = 0;
  for (int i = from; i <= to; ++i) {
    const WordRef& word = wordStore.words[readingOrder[i]];
    if (!isVisible(word)) continue;
    const int wy = screenYOf(word);
    minX = std::min(minX, word.x - 2);
    minY = std::min(minY, wy - 2);
    maxX = std::max(maxX, word.x + word.w + 2);
    maxY = std::max(maxY, wy + word.h + 2);
  }
  bool saved = false;
  if (minX != INT_MAX) {
    if (minX < 0) minX = 0;
    if (minY < 0) minY = 0;
    const int w = maxX - minX;
    const int h = maxY - minY;
    if (snapshot && w > 0 && h > 0) {
      saved = renderer.readFramebufferRegion(minX, minY, w, h, snapshot.get(), SNAPSHOT_CAPACITY) > 0;
    }
    snapshotX = static_cast<int16_t>(minX);
    snapshotY = static_cast<int16_t>(minY);
    snapshotW = static_cast<int16_t>(w);
    snapshotH = static_cast<int16_t>(h);
  }
  snapshotToken = saved ? renderToken() : -1;
  snapshotScrollY = scrollY;
  drawSelectionHighlights();
  return saved;
}

void ClipSelectionActivity::drawHints() const {
  const auto confirmLabel = startMarkIdx == -1 ? tr(STR_SELECT) : tr(STR_DONE);
  const auto labels = mappedInput.mapDirectionalLabels(tr(STR_BACK), confirmLabel, tr(STR_DIR_LEFT), tr(STR_DIR_RIGHT),
                                                       tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

void ClipSelectionActivity::renderPages() {
  ensureVisiblePagesCached();
  const int stride = layout.viewportHeight;
  const int screenW = renderer.getScreenWidth();
  const int screenH = renderer.getScreenHeight();
  const int firstPage = scrollY / stride;
  const int lastPage = (scrollY + layout.viewportHeight - 1) / stride;
  // At scrollY == 0 the selector looks exactly like the reader page. Once
  // soft-advanced, the window starts at a line top and partial lines at the
  // bottom edge are clipped so only whole lines are shown.
  const int clipTop = scrollY > 0 ? layout.marginTop : 0;
  const int clipBottom = visibleClipBottom();

  {
    GfxRenderer::SyntheticBoldScope syntheticBold(renderer, SETTINGS.fakeBold);
    GfxRenderer::ClipScope clip(renderer, 0, clipTop, screenW, std::max(0, clipBottom - clipTop));
    auto drawAll = [&] {
      for (int p = firstPage; p <= lastPage; ++p) {
        if (const Page* page = cachedPage(p)) {
          page->render(renderer, layout.fontId, layout.marginLeft, layout.marginTop + p * stride - scrollY);
        }
      }
    };
    auto* fcm = renderer.getFontCacheManager();
    if (fcm) {
      auto scope = fcm->createPrewarmScope();
      drawAll();
      scope.endScanAndPrewarm();
      drawAll();
    } else {
      drawAll();
    }
  }
  if (clipTop > 0) renderer.fillRect(0, 0, screenW, clipTop, false);
  if (clipBottom < screenH) renderer.fillRect(0, clipBottom, screenW, screenH - clipBottom, false);
}

void ClipSelectionActivity::render(RenderLock&&) {
  if (wordStore.words.empty()) return;
  const int token = renderToken();
  if (snapshotToken >= 0 && snapshotToken != token && snapshotScrollY == scrollY && snapshot) {
    renderer.writeFramebufferRegion(snapshotX, snapshotY, snapshotW, snapshotH, snapshot.get());
    if (captureAndDrawHighlights()) {
      drawHints();
      renderer.displayBuffer(HalDisplay::FAST_REFRESH);
      return;
    }
  }

  renderer.clearScreen();
  renderPages();
  captureAndDrawHighlights();
  drawHints();
  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
}
