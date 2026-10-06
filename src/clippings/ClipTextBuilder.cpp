#include "ClipTextBuilder.h"

#include <Logging.h>

#include <algorithm>
#include <cctype>
#include <climits>

#include "ClippingTextAnchor.h"

namespace ClipTextBuilder {
namespace {

bool hasEmSpace(const char* word) { return word[0] == '\xe2' && word[1] == '\x80' && word[2] == '\x83'; }

bool isUtf8SpaceAt(const std::string& text, const size_t index, size_t& advance) {
  const auto c = static_cast<unsigned char>(text[index]);
  if (c == 0xC2 && index + 1 < text.size() && static_cast<unsigned char>(text[index + 1]) == 0xA0) {
    advance = 2;
    return true;
  }
  if (c == 0xE2 && index + 2 < text.size() && static_cast<unsigned char>(text[index + 1]) == 0x80) {
    const auto c2 = static_cast<unsigned char>(text[index + 2]);
    if (c2 == 0x83 || c2 == 0xAF) {
      advance = 3;
      return true;
    }
  }
  return false;
}

std::string cleanWordText(const std::string& word) {
  std::string out;
  out.reserve(word.size());
  for (size_t i = 0; i < word.size();) {
    size_t advance = 0;
    if (isUtf8SpaceAt(word, i, advance)) {
      if (!out.empty() && out.back() != ' ') {
        out += ' ';
      }
      i += advance;
      continue;
    }
    const char c = word[i++];
    if (c == '\r' || c == '\n' || c == '\t') {
      if (!out.empty() && out.back() != ' ') {
        out += ' ';
      }
      continue;
    }
    out += c;
  }

  while (!out.empty() && out.front() == ' ') {
    out.erase(out.begin());
  }
  while (!out.empty() && out.back() == ' ') {
    out.pop_back();
  }
  return out;
}

std::string stripTrailingInsertedHyphen(std::string word, const bool insertedHyphen) {
  if (insertedHyphen && !word.empty() && word.back() == '-') {
    word.pop_back();
  }
  return word;
}

bool onSameLine(const WordRef& a, const WordRef& b) { return a.pageIdx == b.pageIdx && a.y == b.y; }

// Pixel gap between two words on one line, in either direction.
int lineGap(const WordRef& previousWord, const WordRef& word) {
  return word.x >= previousWord.x ? word.x - (previousWord.x + previousWord.w)
                                  : previousWord.x - (word.x + word.w);
}

bool endsWithJoinCjk(const char* text) {
  uint32_t cp = 0;
  return ClippingTextAnchor::lastCodepoint(text, cp) && ClippingTextAnchor::isJoinCjk(cp);
}

bool startsWithJoinCjk(const char* text) {
  uint32_t cp = 0;
  return ClippingTextAnchor::firstCodepoint(text, cp) && ClippingTextAnchor::isJoinCjk(cp);
}

// Smallest gap between adjacent CJK words on the line: the zero-width gap plus any justification stretch.
int cjkGapOnLine(const ClipWordStore& wordStore, const WordRef& lineWord) {
  int smallest = INT_MAX;
  const WordRef* previous = nullptr;
  for (const WordRef& word : wordStore.words) {
    if (!onSameLine(word, lineWord)) {
      previous = nullptr;
      continue;
    }
    if (previous && word.pageWordIndex == previous->pageWordIndex + 1 &&
        endsWithJoinCjk(wordStore.text(*previous)) && startsWithJoinCjk(wordStore.text(word))) {
      smallest = std::min(smallest, lineGap(*previous, word));
    }
    previous = &word;
  }
  return smallest == INT_MAX ? 0 : smallest;
}

// CJK pairs join; a CJK line break joins; other same-line pairs join unless the gap is wider
// than the line's CJK gap (0 without one) by more than 2 px, which marks an authored space.
bool joinWords(const ClipWordStore& wordStore, const WordRef& previousWord, const std::string& previousText,
               const WordRef& word, const std::string& wordText) {
  const bool previousCjk = endsWithJoinCjk(previousText.c_str());
  const bool nextCjk = startsWithJoinCjk(wordText.c_str());
  if (previousCjk && nextCjk) return true;
  if (!onSameLine(previousWord, word)) return previousCjk || nextCjk;
  const int cjkGap = previousCjk || nextCjk ? cjkGapOnLine(wordStore, word) : 0;
  return lineGap(previousWord, word) <= cjkGap + 2;
}

std::string selectedWordText(const ClipWordStore& wordStore, const WordRef& word,
                             const SelectionBounds* selectionBounds) {
  const bool isFirstBound = selectionBounds && word.pageIdx == selectionBounds->firstPageIdx &&
                            word.pageWordIndex == selectionBounds->firstPageWordOrdinal;
  const bool isLastBound = selectionBounds && word.pageIdx == selectionBounds->lastPageIdx &&
                           word.pageWordIndex == selectionBounds->lastPageWordOrdinal;
  if (!selectionBounds || (!isFirstBound && !isLastBound)) {
    return cleanWordText(wordStore.text(word));
  }

  const size_t textLength = word.textLength;
  const size_t begin = isFirstBound ? std::min<size_t>(selectionBounds->firstWordByteOffset, textLength) : 0;
  const size_t end = isLastBound ? std::min<size_t>(selectionBounds->lastWordByteEndOffset, textLength) : textLength;
  if (end < begin) return {};
  return cleanWordText(std::string(wordStore.text(word) + begin, end - begin));
}

bool isSelectedTableWord(const WordRef& word, const uint16_t tableSelection) {
  return tableSelection == UINT16_MAX || word.tableSelection == tableSelection;
}

}  // namespace

ClippingResult build(const ClipWordStore& wordStore, const uint16_t* wordOrder, const int fromOrder, const int toOrder,
                     const int startPageInSection, const int sectionPageCount, const SelectionBounds* selectionBounds,
                     const uint16_t tableSelection) {
  const auto& words = wordStore.words;
  std::string text;
  text.reserve(256);

  const WordRef& firstWord = words[wordOrder[fromOrder]];
  const WordRef& lastWord = words[wordOrder[toOrder]];
  uint16_t startPageWordIndex = firstWord.pageWordIndex;
  uint16_t endPageWordIndex = lastWord.pageWordIndex;
  uint16_t selectedWordCount = 0;
  for (int orderIdx = fromOrder; orderIdx <= toOrder; ++orderIdx) {
    const WordRef& word = words[wordOrder[orderIdx]];
    if (!isSelectedTableWord(word, tableSelection)) continue;
    selectedWordCount++;
    if (word.pageIdx == firstWord.pageIdx) {
      startPageWordIndex = std::min(startPageWordIndex, word.pageWordIndex);
    }
    if (word.pageIdx == lastWord.pageIdx) {
      endPageWordIndex = std::max(endPageWordIndex, word.pageWordIndex);
    }
  }

  const WordRef* previousWord = nullptr;
  std::string previousClean;

  for (int orderIdx = fromOrder; orderIdx <= toOrder; ++orderIdx) {
    const WordRef& word = words[wordOrder[orderIdx]];
    if (!isSelectedTableWord(word, tableSelection)) continue;
    const auto cleanText = selectedWordText(wordStore, word, selectionBounds);
    const auto wordText = stripTrailingInsertedHyphen(cleanText, word.endsWithInsertedHyphen);
    if (wordText.empty()) {
      previousWord = &word;
      previousClean = cleanText;
      continue;
    }
    const bool joinsInsertedHyphen =
        previousWord && previousWord->endsWithInsertedHyphen && !previousClean.empty() && previousClean.back() == '-';
    if (joinsInsertedHyphen) {
      text += wordText;
      previousWord = &word;
      previousClean = cleanText;
      continue;
    }

    const bool yGap =
        previousWord && word.pageIdx == previousWord->pageIdx && word.y > previousWord->y + previousWord->h;
    const bool paragraphStart = previousWord && (hasEmSpace(wordStore.text(word)) || word.paragraphStart || yGap);

    if (previousWord && !text.empty() && !paragraphStart) {
      if (!previousClean.empty() && previousClean.back() == '-' &&
          !std::isspace(static_cast<unsigned char>(wordText[0])) &&
          !std::ispunct(static_cast<unsigned char>(wordText[0]))) {
        text += wordText;
        previousWord = &word;
        previousClean = cleanText;
        continue;
      }
    }

    if (paragraphStart) {
      text += '\n';
    } else if (!text.empty()) {
      // Hangul is not join-CJK, so it keeps its spaces.
      if (!previousWord || !joinWords(wordStore, *previousWord, previousClean, word, wordText)) {
        text += ' ';
      }
    }
    text += wordText;

    previousWord = &word;
    previousClean = cleanText;
  }

  return ClippingResult{std::move(text),
                        static_cast<uint16_t>(startPageInSection + firstWord.pageIdx),
                        static_cast<uint16_t>(startPageInSection + lastWord.pageIdx),
                        static_cast<uint16_t>(std::max(1, sectionPageCount)),
                        startPageWordIndex,
                        endPageWordIndex,
                        UINT16_MAX,
                        tableSelection,
                        selectedWordCount};
}

}  // namespace ClipTextBuilder
