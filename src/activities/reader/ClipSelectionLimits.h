#pragma once

#include <cstddef>
#include <cstdint>

#include "WordRef.h"

// Memory budget for the EPUB clipping selector.
//
// The selector keeps a sliding window of selectable words (WordRef metadata +
// a NUL-separated text pool). The window, and therefore the longest possible
// clipping, is bounded by `maxWords`; the number of distinct section pages a
// window may span is bounded by `maxPages`.
//
// * Classic ESP32-C3 (no PSRAM) keeps CrossInk's proven budget: 3 pages and
//   240 words. 240 * sizeof(WordRef) plus a ~2 KiB pool stays around 10 KiB of
//   internal DRAM while the reader's section and fonts are resident.
// * PSRAM targets (BOARD_HAS_PSRAM with PSRAM actually registered) use 8 pages
//   and 1024 words. Allocations of that size go through default-capability
//   malloc, which ESP-IDF serves from PSRAM (CONFIG_SPIRAM_USE_MALLOC), so the
//   larger window does not compete with internal DRAM. 1024 CJK tokens are
//   ~3 KiB of UTF-8, just under the 4 KiB stored-clipping text cap.
// * A heavily fragmented classic heap degrades to a one-page / 120-word window
//   instead of refusing to open the selector.
struct ClipSelectionLimits {
  uint8_t maxPages = 0;
  uint16_t maxWords = 0;
  uint16_t textPoolReserve = 0;
  bool psramTier = false;
};

namespace ClipSelectionLimitPolicy {

inline constexpr ClipSelectionLimits CLASSIC{3, 240, 2048, false};
inline constexpr ClipSelectionLimits PSRAM{8, 1024, 8192, true};
inline constexpr ClipSelectionLimits LOW_HEAP{1, 120, 1024, false};

// Internal DRAM that must stay free after reserving the classic window.
inline constexpr size_t CLASSIC_DRAM_HEADROOM = 16U * 1024U;
// PSRAM that must stay free after reserving the large window.
inline constexpr size_t PSRAM_HEADROOM = 64U * 1024U;

constexpr size_t reserveBytes(const ClipSelectionLimits& limits) {
  return static_cast<size_t>(limits.maxWords) * (sizeof(WordRef) + sizeof(uint16_t)) + limits.textPoolReserve;
}

// Pure decision function so the policy can be exercised on the host.
constexpr ClipSelectionLimits choose(const bool psramAvailable, const size_t psramLargestBlock,
                                     const size_t defaultLargestBlock) {
  if (psramAvailable && psramLargestBlock >= reserveBytes(PSRAM) + PSRAM_HEADROOM) return PSRAM;
  if (defaultLargestBlock >= reserveBytes(CLASSIC) + CLASSIC_DRAM_HEADROOM) return CLASSIC;
  return LOW_HEAP;
}

static_assert(choose(true, 8U * 1024U * 1024U, 0).psramTier);
static_assert(choose(true, 8U * 1024U * 1024U, 0).maxWords > CLASSIC.maxWords);
static_assert(choose(true, 8U * 1024U * 1024U, 0).maxPages > CLASSIC.maxPages);
static_assert(!choose(false, 8U * 1024U * 1024U, 200U * 1024U).psramTier);
static_assert(choose(false, 0, 200U * 1024U).maxWords == CLASSIC.maxWords);
static_assert(choose(true, 1024U, 200U * 1024U).maxWords == CLASSIC.maxWords);
static_assert(choose(false, 0, 4U * 1024U).maxWords == LOW_HEAP.maxWords);

}  // namespace ClipSelectionLimitPolicy
