#pragma once

#include <cstdint>
#include <string>

// Result of ClipSelectionActivity: the selected text and where it sits in the section.
struct ClippingResult {
  std::string text;
  uint16_t sectionPage = 0;
  uint16_t endSectionPage = 0;
  uint16_t sectionPageCount = 1;
  uint16_t startPageWordIndex = 0;
  uint16_t endPageWordIndex = 0;
  uint16_t paragraphIndex = UINT16_MAX;
  uint16_t tableSelection = UINT16_MAX;
  uint16_t wordCount = 0;
};

// Result of EpubReaderClippingListActivity: the clipping the reader should open.
struct ClippingJumpResult {
  uint16_t spineIndex = 0;
  uint16_t page = 0;
  uint16_t pageCount = 1;
  uint16_t paragraphIndex = UINT16_MAX;
  uint16_t clippingIndex = UINT16_MAX;
};
