#pragma once

#include <GfxRenderer.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "EpubReaderMenuActivity.h"

class EpubReaderActivity;
class Page;
struct Clipping;

// EPUB clippings (摘录) for EpubReaderActivity; the reader only forwards lifecycle, menu and render hooks here.
class ClippingController {
 public:
  explicit ClippingController(EpubReaderActivity& reader) : reader(reader) {}

  // Adds "Create clipping", plus "View clippings" when the open book has any.
  static void appendMenuItems(std::vector<EpubReaderMenuActivity::MenuItem>& items);

  // Loads the open book's clippings.
  void onBookLoaded();
  // Releases scratch buffers and unloads the store.
  void onExit();
  // Hides the saved/failed popup once its display time has passed.
  void loop();
  // Handles CREATE_CLIPPING / VIEW_CLIPPINGS.
  void onMenuAction(EpubReaderMenuActivity::MenuAction action);
  // Moves to a clipping picked from the list once its section layout is complete.
  void applyPendingJump();
  // Draws the saved/failed popup while it is shown.
  void drawMessage() const;
  // Dithers clipped words on the current page.
  void drawHighlights(const Page& page, int fontId, int orientedMarginTop, int orientedMarginLeft) const;
  // Same, after switching to the render mode of a cached page plane.
  void drawHighlights(const Page& page, int fontId, int orientedMarginTop, int orientedMarginLeft,
                      GfxRenderer::RenderMode mode) const;

 private:
  EpubReaderActivity& reader;
  bool showMessage = false;
  unsigned long messageTime = 0UL;
  const char* messageText = nullptr;
  // Reused across highlight passes; released in onExit().
  mutable std::vector<const char*> wordScratch;
  mutable std::vector<const char*> combinedScratch;
  mutable std::string textScratch;
  uint16_t pendingJump = UINT16_MAX;

  void startSelection();
  void openList();
  uint32_t currentClippingLayoutSignature() const;
  void collectClippingWords(const Page& page, std::vector<const char*>& out) const;
  bool matchClippingOnPage(uint16_t pageIndex, const Page& page, const char* text, uint16_t& startWord,
                           uint16_t& endWord, bool* startsAtClipStart, bool* reachesClipEnd) const;
  bool clippingRangeOnPage(size_t clippingIndex, const Clipping& clipping, const Page& page, uint16_t currentPage,
                           uint16_t currentPageCount, uint32_t layoutSignature, uint16_t& startWord,
                           uint16_t& endWord) const;
  uint16_t resolveClippingJumpPage(const Clipping& clipping) const;
};
