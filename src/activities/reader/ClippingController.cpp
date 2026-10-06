#include "ClippingController.h"

#include <Epub/Page.h>
#include <Epub/blocks/TextBlock.h>
#include <GfxRenderer.h>
#include <I18n.h>
#include <Logging.h>
#include <Memory.h>

#include <algorithm>
#include <cstring>
#include <optional>

#include "ClipSelectionActivity.h"
#include "ClippingStore.h"
#include "CrossPointSettings.h"
#include "EpubReaderActivity.h"
#include "EpubReaderClippingListActivity.h"
#include "ReaderUtils.h"
#include "ReadingStatsStore.h"
#include "clippings/ClippingTextAnchor.h"
#include "clippings/ClippingsManager.h"
#include "components/UITheme.h"

void ClippingController::appendMenuItems(std::vector<EpubReaderMenuActivity::MenuItem>& items) {
  items.push_back({EpubReaderMenuActivity::MenuAction::CREATE_CLIPPING, StrId::STR_SAVE_CLIPPING});
  if (CLIPPINGS.hasClippings()) {
    items.push_back({EpubReaderMenuActivity::MenuAction::VIEW_CLIPPINGS, StrId::STR_VIEW_CLIPPINGS});
  }
}

void ClippingController::onBookLoaded() {
  CLIPPINGS.loadForBook(reader.epub->getPath(), reader.epub->getTitle(), reader.epub->getAuthor(), "epub");
}

void ClippingController::onExit() {
  wordScratch.clear();
  wordScratch.shrink_to_fit();
  combinedScratch.clear();
  combinedScratch.shrink_to_fit();
  textScratch.clear();
  textScratch.shrink_to_fit();
  CLIPPINGS.unload();
}

void ClippingController::loop() {
  if (showMessage && (millis() - messageTime) >= ReaderUtils::BOOKMARK_MESSAGE_DURATION_MS) {
    showMessage = false;
    reader.requestUpdate();
  }
}

void ClippingController::onMenuAction(const EpubReaderMenuActivity::MenuAction action) {
  if (action == EpubReaderMenuActivity::MenuAction::CREATE_CLIPPING) {
    startSelection();
  } else if (action == EpubReaderMenuActivity::MenuAction::VIEW_CLIPPINGS) {
    openList();
  }
}

void ClippingController::applyPendingJump() {
  auto& section = reader.section;
  if (pendingJump == UINT16_MAX || section->isBuilding() || section->isPartial() || section->pageCount <= 0) return;
  if (const Clipping* clipping = CLIPPINGS.clippingAt(pendingJump)) {
    if (clipping->spineIndex == static_cast<uint16_t>(reader.currentSpineIndex)) {
      section->currentPage = resolveClippingJumpPage(*clipping);
    }
  }
  pendingJump = UINT16_MAX;
  if (section->currentPage < 0) section->currentPage = 0;
  if (section->currentPage >= section->pageCount) section->currentPage = section->pageCount - 1;
}

void ClippingController::drawMessage() const {
  if (showMessage && messageText) {
    GUI.drawPopup(reader.renderer, messageText);
  }
}

void ClippingController::drawHighlights(const Page& page, const int fontId, const int orientedMarginTop,
                                        const int orientedMarginLeft, const GfxRenderer::RenderMode mode) const {
  // Cached page planes do not store highlights, so they are drawn over the copied plane.
  reader.renderer.setRenderMode(mode);
  drawHighlights(page, fontId, orientedMarginTop, orientedMarginLeft);
}

void ClippingController::startSelection() {
  if (!reader.section || !reader.epub || reader.section->pageCount <= 0) {
    reader.requestUpdate();
    return;
  }

  int orientedMarginTop, orientedMarginRight, orientedMarginBottom, orientedMarginLeft;
  reader.renderer.getOrientedViewableTRBL(&orientedMarginTop, &orientedMarginRight, &orientedMarginBottom,
                                          &orientedMarginLeft);
  orientedMarginTop += SETTINGS.screenMargin;
  orientedMarginLeft += SETTINGS.screenMargin;

  // The selector loads words lazily, one line at a time, from this and the
  // following section pages (real pageIdx); see ClipSelectionActivity.
  ClipSelectionActivity::Layout layout;
  layout.fontId = SETTINGS.getReaderFontId();
  layout.marginLeft = orientedMarginLeft;
  layout.marginTop = orientedMarginTop;
  layout.contentRight = reader.renderer.getScreenWidth() - orientedMarginRight - SETTINGS.screenMargin;
  // Page stride for the continuous soft-advance column: the content height the
  // section was laid out for.
  layout.viewportHeight = reader.buildViewportHeight > 0
                              ? reader.buildViewportHeight
                              : reader.renderer.getScreenHeight() - orientedMarginTop -
                                    std::max(static_cast<int>(SETTINGS.screenMargin),
                                             static_cast<int>(orientedMarginBottom));
  layout.linePitch = reader.renderer.getLineHeight(
      layout.fontId,
      reader.effectiveRenderSpec(reader.buildViewportWidth, static_cast<uint16_t>(layout.viewportHeight))
          .lineCompression);
  const ClipSelectionLimits limits = ClipSelectionActivity::currentLimits();

  std::string bookTitle = reader.epub->getTitle();
  std::string author = reader.epub->getAuthor();
  std::string chapterTitle;
  const int tocIndex = reader.epub->getTocIndexForSpineIndex(reader.currentSpineIndex);
  if (tocIndex >= 0) chapterTitle = reader.epub->getTocItem(tocIndex).title;

  auto clipSelection = makeUniqueNoThrow<ClipSelectionActivity>(reader.renderer, reader.mappedInput, *reader.section,
                                                                reader.section->currentPage, layout, limits);
  if (!clipSelection) {
    LOG_ERR("CLIP", "OOM: ClipSelectionActivity");
    reader.requestUpdate();
    return;
  }

  reader.startActivityForResult(
      std::move(clipSelection),
      [this, bookTitle = std::move(bookTitle), author = std::move(author),
       chapterTitle = std::move(chapterTitle)](const ActivityResult& result) {
        READING_STATS.resumeSession();
        messageText = nullptr;
        showMessage = false;
        if (!result.isCancelled) {
          const auto& clip = std::get<ClippingResult>(result.data);
          if (!clip.text.empty()) {
            const size_t clippingIndex = CLIPPINGS.clippingCount();
            const auto addResult = CLIPPINGS.addClipping(
                static_cast<uint16_t>(reader.currentSpineIndex), clip.sectionPage, clip.endSectionPage,
                clip.sectionPageCount, clip.startPageWordIndex, clip.endPageWordIndex, clip.wordCount,
                chapterTitle.c_str(), clip.paragraphIndex, clip.text, clip.tableSelection,
                clippingWordLayoutSignature(currentClippingLayoutSignature()));
            bool exported = false;
            if (addResult == ClippingStore::AddResult::Added) {
              exported = ClippingsManager::saveClipping(bookTitle, author, chapterTitle,
                                                        static_cast<int>(clip.sectionPage) + 1, clip.text);
              if (!exported && !CLIPPINGS.removeClippingAt(clippingIndex)) {
                LOG_ERR("CLIP", "Failed to roll back clipping after export failure");
              }
            }
            const bool saved = addResult == ClippingStore::AddResult::Added && exported;
            messageText = addResult == ClippingStore::AddResult::LimitReached ? tr(STR_CLIPPING_LIMIT_REACHED)
                          : saved                                             ? tr(STR_CLIPPING_SAVED)
                                                                              : tr(STR_CLIPPING_FAILED);
            showMessage = true;
            messageTime = millis();
          }
        }
        reader.requestUpdate();
      });
}

void ClippingController::openList() {
  if (!CLIPPINGS.hasClippings()) {
    reader.requestUpdate();
    return;
  }
  reader.startActivityForResultWith<EpubReaderClippingListActivity>(
      [this](const ActivityResult& result) {
        READING_STATS.resumeSession();
        if (result.isCancelled) {
          reader.openReaderMenu();
          return;
        }
        if (!std::holds_alternative<ClippingJumpResult>(result.data)) {
          reader.requestUpdate();
          return;
        }
        const auto& jump = std::get<ClippingJumpResult>(result.data);
        RenderLock lock;
        reader.clearDeferredReposition();
        const Clipping* clipping =
            jump.clippingIndex != UINT16_MAX ? CLIPPINGS.clippingAt(jump.clippingIndex) : nullptr;
        const bool sameSpine = reader.section && static_cast<int>(jump.spineIndex) == reader.currentSpineIndex;
        const bool layoutReady = sameSpine && !reader.section->isBuilding() && !reader.section->isPartial() &&
                                 reader.section->pageCount > 0;
        if (clipping && layoutReady && clipping->spineIndex == jump.spineIndex) {
          reader.section->currentPage = resolveClippingJumpPage(*clipping);
          pendingJump = UINT16_MAX;
        } else {
          pendingJump = jump.clippingIndex;
          if (!sameSpine) {
            reader.currentSpineIndex = jump.spineIndex;
            reader.nextPageNumber = jump.page;
            reader.section.reset();
          } else if (reader.section) {
            reader.section->currentPage = jump.page;
          } else {
            reader.nextPageNumber = jump.page;
          }
        }
        reader.requestUpdate();
      },
      reader.epub);
}

namespace {

bool clippingWordVisible(const char* text) {
  if (!text) return false;
  for (const auto* p = reinterpret_cast<const uint8_t*>(text); *p; ++p) {
    if (*p > 0x20) return true;
  }
  return false;
}

uint16_t approximateRelayoutPage(const Clipping& clipping, const uint16_t currentPageCount) {
  if (currentPageCount == 0) return 0;
  if (clipping.pageCount <= 1) return 0;
  const uint32_t oldLastPage = static_cast<uint32_t>(clipping.pageCount - 1);
  const uint32_t newLastPage = static_cast<uint32_t>(currentPageCount - 1);
  const uint32_t scaled =
      (static_cast<uint32_t>(clipping.startPage) * newLastPage + oldLastPage / 2U) / oldLastPage;
  return static_cast<uint16_t>(std::min<uint32_t>(scaled, static_cast<uint32_t>(currentPageCount - 1)));
}

}  // namespace

uint32_t ClippingController::currentClippingLayoutSignature() const {
  if (reader.buildViewportWidth == 0 || reader.buildViewportHeight == 0) return 0;
  const ReaderRenderSpec spec = reader.effectiveRenderSpec(reader.buildViewportWidth, reader.buildViewportHeight);
  uint32_t hash = 2166136261u;
  const auto mix = [&hash](const uint32_t value) {
    hash ^= value;
    hash *= 16777619u;
  };
  mix(static_cast<uint32_t>(spec.fontId));
  uint32_t lineBits = 0;
  memcpy(&lineBits, &spec.lineCompression, sizeof(lineBits));
  mix(lineBits);
  mix(spec.extraParagraphSpacing);
  mix(spec.firstLineIndent);
  mix(spec.paragraphIndentSpaces);
  mix(static_cast<uint32_t>(static_cast<int32_t>(spec.characterSpacing)));
  mix(spec.wordSpacingPercent);
  mix(spec.paragraphAlignment);
  mix(spec.viewportWidth);
  mix(spec.viewportHeight);
  mix(spec.hyphenationEnabled ? 1u : 0u);
  mix(spec.embeddedStyle ? 1u : 0u);
  mix(spec.imageRendering);
  mix(spec.focusReadingEnabled ? 1u : 0u);
  return hash == 0 ? 1u : hash;
}

void ClippingController::collectClippingWords(const Page& page, std::vector<const char*>& out) const {
  out.clear();
  uint16_t count = 0;
  for (const auto& element : page.elements) {
    if (element->getTag() != TAG_PageLine) continue;
    const auto* block = static_cast<const PageLine*>(element.get())->getBlock().get();
    if (!block || !block->valid()) continue;
    for (uint16_t i = 0; i < block->wordCount(); ++i) {
      if (clippingWordVisible(block->wordText(i))) count++;
    }
  }
  if (count == 0) return;
  if (out.capacity() < count) out.reserve(count);
  for (const auto& element : page.elements) {
    if (element->getTag() != TAG_PageLine) continue;
    const auto* block = static_cast<const PageLine*>(element.get())->getBlock().get();
    if (!block || !block->valid()) continue;
    for (uint16_t i = 0; i < block->wordCount(); ++i) {
      const char* text = block->wordText(i);
      if (clippingWordVisible(text)) out.push_back(text);
    }
  }
}

bool ClippingController::matchClippingOnPage(const uint16_t pageIndex, const Page& page, const char* text,
                                              uint16_t& startWord, uint16_t& endWord, bool* startsAtClipStart,
                                              bool* reachesClipEnd) const {
  const auto& section = reader.section;
  if (!section || !text || text[0] == '\0') return false;
  collectClippingWords(page, wordScratch);
  if (wordScratch.empty() || wordScratch.size() > UINT16_MAX) return false;

  const auto apply = [&](const ClippingTextAnchor::AnchorMatch& found) {
    startWord = found.startWord;
    endWord = found.endWord;
    if (startsAtClipStart) *startsAtClipStart = found.startsAtClipStart;
    if (reachesClipEnd) *reachesClipEnd = found.reachesClipEnd;
  };

  ClippingTextAnchor::AnchorMatch found;
  const auto* words = wordScratch.data();
  const auto wordCount = static_cast<uint16_t>(wordScratch.size());
  if (ClippingTextAnchor::findClippingAnchor(words, wordCount, text, found)) {
    apply(found);
    return true;
  }

  if (!ClippingTextAnchor::findClippingAnchor(words, wordCount, text, found,
                                              ClippingTextAnchor::AnchorSearch::IncludeShort)) {
    return false;
  }

  const uint16_t pageWords = wordCount;
  for (const int direction : {-1, 1}) {
    const int neighborIndex = static_cast<int>(pageIndex) + direction;
    if (neighborIndex < 0 || neighborIndex >= section->pageCount) continue;
    auto neighbor = section->loadPage(neighborIndex);
    if (!neighbor) {
      LOG_ERR("CLIP", "Could not verify clipping across page %d", neighborIndex);
      continue;
    }
    const bool pageIsFirst = direction > 0;
    uint16_t boundary = 0;
    if (!pageIsFirst) {
      collectClippingWords(*neighbor, combinedScratch);
      if (combinedScratch.empty()) continue;
      boundary = static_cast<uint16_t>(combinedScratch.size());
      if (static_cast<size_t>(boundary) + wordScratch.size() > UINT16_MAX) continue;
      if (combinedScratch.capacity() < static_cast<size_t>(boundary) + wordScratch.size()) {
        combinedScratch.reserve(static_cast<size_t>(boundary) + wordScratch.size());
      }
      combinedScratch.insert(combinedScratch.end(), wordScratch.begin(), wordScratch.end());
    } else {
      // Copy this page before collectClippingWords reuses wordScratch for the neighbor.
      combinedScratch.assign(wordScratch.begin(), wordScratch.end());
      collectClippingWords(*neighbor, wordScratch);
      if (wordScratch.empty()) continue;
      boundary = pageWords;
      if (static_cast<size_t>(boundary) + wordScratch.size() > UINT16_MAX) continue;
      if (combinedScratch.capacity() < static_cast<size_t>(boundary) + wordScratch.size()) {
        combinedScratch.reserve(static_cast<size_t>(boundary) + wordScratch.size());
      }
      combinedScratch.insert(combinedScratch.end(), wordScratch.begin(), wordScratch.end());
    }

    ClippingTextAnchor::AnchorMatch combined;
    if (!ClippingTextAnchor::findClippingAnchor(combinedScratch.data(), static_cast<uint16_t>(combinedScratch.size()),
                                                text, combined) ||
        combined.startWord >= boundary || combined.endWord < boundary) {
      continue;
    }
    ClippingTextAnchor::AnchorMatch pageMatch;
    if (!ClippingTextAnchor::clipAnchorToPage(combined, boundary, pageIsFirst, pageMatch)) continue;
    apply(pageMatch);
    return true;
  }
  return false;
}

bool ClippingController::clippingRangeOnPage(const size_t clippingIndex, const Clipping& clipping, const Page& page,
                                             const uint16_t currentPage, const uint16_t currentPageCount,
                                             const uint32_t layoutSignature, uint16_t& startWord,
                                             uint16_t& endWord) const {
  if (clippingStoredRangeMatchesLayout(clipping, currentPageCount, layoutSignature)) {
    if (currentPage < clipping.startPage || currentPage > clipping.endPage) return false;
    startWord = currentPage == clipping.startPage ? clipping.startWordIndex : 0;
    endWord = currentPage == clipping.endPage ? clipping.endWordIndex : UINT16_MAX;
    return startWord <= endWord;
  }
  if ((clipping.textMatchBoundaries & CLIPPING_TEXT_MATCH_AUTHORITATIVE) != 0 &&
      clipping.textMatchSignature == layoutSignature) {
    return clippingTextMatchOnPage(clipping, currentPage, layoutSignature, startWord, endWord);
  }

  if (!CLIPPINGS.readClippingText(clipping, textScratch) || textScratch.empty()) return false;
  bool startsAtClipStart = false;
  bool reachesClipEnd = false;
  if (!matchClippingOnPage(currentPage, page, textScratch.c_str(), startWord, endWord, &startsAtClipStart,
                           &reachesClipEnd)) {
    return false;
  }
  CLIPPINGS.noteTextMatch(clippingIndex, currentPage, startWord, endWord, startsAtClipStart, reachesClipEnd,
                          ClippingTextAnchor::countUnits(textScratch.c_str()), layoutSignature);
  return true;
}

uint16_t ClippingController::resolveClippingJumpPage(const Clipping& clipping) const {
  const auto& section = reader.section;
  if (!section || section->pageCount <= 0) return clipping.startPage;
  const uint16_t pageCount = static_cast<uint16_t>(std::min<int>(section->pageCount, UINT16_MAX));
  const uint32_t layoutSignature = clippingWordLayoutSignature(currentClippingLayoutSignature());
  if (clippingStoredRangeMatchesLayout(clipping, pageCount, layoutSignature)) {
    return std::min(clipping.startPage, static_cast<uint16_t>(pageCount - 1));
  }

  const uint16_t approximate = approximateRelayoutPage(clipping, pageCount);
  if (!CLIPPINGS.readClippingText(clipping, textScratch) || textScratch.empty()) return approximate;

  const auto pageContains = [&](const uint16_t page, ClippingTextAnchor::AnchorMatch& match) {
    auto loaded = section->loadPage(page);
    if (!loaded) return false;
    uint16_t startWord = 0;
    uint16_t endWord = 0;
    bool starts = false;
    bool reaches = false;
    if (!matchClippingOnPage(page, *loaded, textScratch.c_str(), startWord, endWord, &starts, &reaches)) {
      return false;
    }
    match.startWord = startWord;
    match.endWord = endWord;
    match.startsAtClipStart = starts;
    match.reachesClipEnd = reaches;
    return true;
  };

  constexpr uint16_t kSearchRadius = 8;
  const auto searchNear = [&](const uint16_t center) -> std::optional<uint16_t> {
    const auto accept = [&](const uint16_t page) -> std::optional<uint16_t> {
      ClippingTextAnchor::AnchorMatch match;
      if (!pageContains(page, match)) return std::nullopt;
      uint16_t best = page;
      // Walk back to the page where this clipping starts.
      constexpr uint16_t kWalkBackPages = 16;
      for (uint16_t cursor = page; cursor > 0 && page - cursor < kWalkBackPages; --cursor) {
        ClippingTextAnchor::AnchorMatch previous;
        if (!pageContains(static_cast<uint16_t>(cursor - 1), previous)) break;
        best = static_cast<uint16_t>(cursor - 1);
        if (previous.startsAtClipStart) return best;
      }
      return best;
    };
    const uint16_t clamped = std::min(center, static_cast<uint16_t>(pageCount - 1));
    if (const auto hit = accept(clamped)) return hit;
    for (uint16_t distance = 1; distance <= kSearchRadius; ++distance) {
      if (clamped >= distance) {
        if (const auto hit = accept(static_cast<uint16_t>(clamped - distance))) return hit;
      }
      if (static_cast<uint32_t>(clamped) + distance < pageCount) {
        if (const auto hit = accept(static_cast<uint16_t>(clamped + distance))) return hit;
      }
    }
    return std::nullopt;
  };

  if (const auto hit = searchNear(approximate)) return *hit;
  if (clipping.paragraphIndex != UINT16_MAX) {
    if (const auto paragraphPage = section->getPageForParagraphIndex(clipping.paragraphIndex)) {
      if (const auto hit = searchNear(std::min(*paragraphPage, static_cast<uint16_t>(pageCount - 1)))) return *hit;
    }
  }
  return approximate;
}

void ClippingController::drawHighlights(const Page& page, const int fontId, const int orientedMarginTop,
                                        const int orientedMarginLeft) const {
  const auto& section = reader.section;
  GfxRenderer& renderer = reader.renderer;
  const int currentSpineIndex = reader.currentSpineIndex;
  if (!section || !CLIPPINGS.hasClippings()) return;
  if (section->pageCount <= 0 || section->currentPage < 0 || section->currentPage >= section->pageCount) return;

  const uint16_t currentPage = static_cast<uint16_t>(section->currentPage);
  const uint16_t currentPageCount = static_cast<uint16_t>(section->pageCount);
  const uint32_t layoutSignature = clippingWordLayoutSignature(currentClippingLayoutSignature());
  const int lineHeight = renderer.getLineHeight(fontId);

  struct Range {
    uint16_t startWord;
    uint16_t endWord;
  };
  Range ranges[CLIPPING_MAX_PAGE_MATCHES];
  uint16_t rangeCount = 0;

  const auto& clippings = CLIPPINGS.getClippings();
  for (size_t clippingIndex = 0; clippingIndex < clippings.size(); ++clippingIndex) {
    const Clipping& clipping = clippings[clippingIndex];
    if (clipping.spineIndex != static_cast<uint16_t>(currentSpineIndex)) continue;
    uint16_t startWord = 0;
    uint16_t endWord = 0;
    if (!clippingRangeOnPage(clippingIndex, clipping, page, currentPage, currentPageCount, layoutSignature, startWord,
                             endWord)) {
      continue;
    }
    if (startWord <= endWord && rangeCount < CLIPPING_MAX_PAGE_MATCHES) {
      ranges[rangeCount++] = {startWord, endWord};
    }
  }
  if (rangeCount == 0) return;

  const auto isHighlighted = [&](uint16_t pageWordIndex) {
    for (uint16_t i = 0; i < rangeCount; ++i) {
      if (pageWordIndex >= ranges[i].startWord && pageWordIndex <= ranges[i].endWord) return true;
    }
    return false;
  };

  uint16_t pageWordIndex = 0;
  for (const auto& element : page.elements) {
    if (element->getTag() != TAG_PageLine) continue;
    const auto* line = static_cast<const PageLine*>(element.get());
    const auto* block = line->getBlock().get();
    if (!block || !block->valid()) continue;
    const int ascender = renderer.getFontAscenderSize(fontId);
    const int rubyShift = block->getRubyShift(ascender);
    for (uint16_t i = 0; i < block->wordCount(); ++i) {
      const char* text = block->wordText(i);
      if (!clippingWordVisible(text)) continue;
      const uint16_t thisWord = pageWordIndex++;
      if (!isHighlighted(thisWord)) continue;

      const auto style = block->wordStyle(i);
      const int wordX = orientedMarginLeft + line->xPos + block->wordXpos(i);
      const int wordY = orientedMarginTop + line->yPos + rubyShift;
      const int wordW = renderer.getTextAdvanceX(fontId, text, style);
      const int wordH = lineHeight;
      if (wordW <= 0) continue;
      for (int y = wordY; y < wordY + wordH; y += 2) {
        for (int x = wordX; x < wordX + wordW; x += 2) {
          renderer.drawPixel(x, y, true);
        }
      }
    }
  }
}
