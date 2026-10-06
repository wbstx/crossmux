#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>

#include "ClippingTextMatcher.h"

namespace ClippingTextAnchor {

struct AnchorMatch {
  uint16_t startWord = 0;
  uint16_t endWord = 0;
  uint16_t units = 0;
  bool startsAtClipStart = false;
  bool reachesClipEnd = false;
  bool reliable = false;
};

enum class AnchorSearch : uint8_t { ReliableOnly, IncludeShort };

// Han, kana, and fullwidth forms join without a space. Hangul stays spaced.
inline bool isJoinCjk(const uint32_t cp) {
  return (cp >= 0x2E80 && cp <= 0x9FFF) || (cp >= 0xF900 && cp <= 0xFAFF) || (cp >= 0xFE30 && cp <= 0xFE4F) ||
         (cp >= 0xFF00 && cp <= 0xFFEF) || (cp >= 0x20000 && cp <= 0x3FFFF);
}

inline bool decodeAt(const char* text, const size_t length, const size_t index, uint32_t& cp, size_t& advance) {
  if (!text || index >= length) return false;
  const auto c0 = static_cast<unsigned char>(text[index]);
  if (c0 < 0x80) {
    cp = c0;
    advance = 1;
    return true;
  }
  const size_t need = c0 >= 0xF0 ? 4 : c0 >= 0xE0 ? 3 : c0 >= 0xC0 ? 2 : 1;
  if (need == 1 || index + need > length) {
    cp = c0;
    advance = 1;
    return true;
  }
  cp = c0 & (0x7Fu >> need);
  for (size_t i = 1; i < need; ++i) {
    const auto cont = static_cast<unsigned char>(text[index + i]);
    if ((cont & 0xC0u) != 0x80u) {
      cp = c0;
      advance = 1;
      return true;
    }
    cp = (cp << 6) | (cont & 0x3Fu);
  }
  advance = need;
  return true;
}

inline bool clipSpaceAdvance(const char* text, const size_t length, const size_t index, size_t& advance) {
  if (!text || index >= length) return false;
  const auto c = static_cast<unsigned char>(text[index]);
  if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v') {
    advance = 1;
    return true;
  }
  if (c == 0xC2 && index + 1 < length && static_cast<unsigned char>(text[index + 1]) == 0xA0) {
    advance = 2;
    return true;
  }
  if (c == 0xE2 && index + 2 < length && static_cast<unsigned char>(text[index + 1]) == 0x80) {
    const auto c2 = static_cast<unsigned char>(text[index + 2]);
    if (c2 == 0x83 || c2 == 0xAF) {
      advance = 3;
      return true;
    }
  }
  return false;
}

inline size_t skipClipSpace(const char* text, const size_t length, size_t index) {
  size_t advance = 0;
  while (clipSpaceAdvance(text, length, index, advance)) index += advance;
  return index;
}

inline size_t boundedLength(const char* text) {
  size_t length = 0;
  if (!text) return 0;
  while (text[length] != '\0') length++;
  return length;
}

inline bool lastCodepoint(const char* text, uint32_t& cp) {
  const size_t length = boundedLength(text);
  if (length == 0) return false;
  size_t index = 0;
  bool any = false;
  while (index < length) {
    uint32_t current = 0;
    size_t advance = 0;
    if (!decodeAt(text, length, index, current, advance)) break;
    cp = current;
    any = true;
    index += advance;
  }
  return any;
}

inline bool firstCodepoint(const char* text, uint32_t& cp) {
  size_t advance = 0;
  return decodeAt(text, boundedLength(text), 0, cp, advance);
}

inline bool joinsWithoutSpace(const char* previous, const char* next) {
  uint32_t prev = 0;
  uint32_t nxt = 0;
  if (!lastCodepoint(previous, prev) || !firstCodepoint(next, nxt)) return false;
  return isJoinCjk(prev) || isJoinCjk(nxt);
}

inline uint16_t countUnits(const char* text) {
  if (!text) return 0;
  const size_t length = static_cast<size_t>(-1) / 2;
  uint16_t units = 0;
  bool inLatin = false;
  for (size_t index = 0; text[index] != '\0';) {
    size_t space = 0;
    if (clipSpaceAdvance(text, length, index, space)) {
      inLatin = false;
      index += space;
      continue;
    }
    uint32_t cp = 0;
    size_t advance = 0;
    if (!decodeAt(text, length, index, cp, advance)) break;
    if (isJoinCjk(cp)) {
      if (units < UINT16_MAX) units++;
      inLatin = false;
    } else if (!inLatin) {
      if (units < UINT16_MAX) units++;
      inLatin = true;
    }
    index += advance;
  }
  return units;
}

namespace detail {

inline size_t textLength(const char* text) {
  size_t length = 0;
  if (!text) return 0;
  while (text[length] != '\0') length++;
  return length;
}

enum class WordFit : uint8_t { Fail, Empty, Match };

inline WordFit matchWord(const char* clip, const size_t clipLen, const size_t clipPos, const char* word,
                         size_t& consumed, uint16_t& units) {
  consumed = 0;
  units = 0;
  if (!word) return WordFit::Fail;
  size_t wordPos = 0;
  if (static_cast<unsigned char>(word[0]) == 0xE2 && static_cast<unsigned char>(word[1]) == 0x80 &&
      static_cast<unsigned char>(word[2]) == 0x83) {
    wordPos = 3;
  }
  size_t pos = clipPos;
  bool inLatin = false;
  bool any = false;
  while (word[wordPos] != '\0') {
    size_t wordSpace = 0;
    if (clipSpaceAdvance(word, textLength(word), wordPos, wordSpace)) {
      wordPos += wordSpace;
      inLatin = false;
      continue;
    }
    uint32_t wordCp = 0;
    size_t wordAdvance = 0;
    if (!decodeAt(word, textLength(word), wordPos, wordCp, wordAdvance)) return WordFit::Fail;
    if (wordCp == '-' && (pos >= clipLen || static_cast<unsigned char>(clip[pos]) != '-')) {
      wordPos += wordAdvance;
      continue;
    }
    if (pos >= clipLen) return WordFit::Fail;
    uint32_t clipCp = 0;
    size_t clipAdvance = 0;
    if (!decodeAt(clip, clipLen, pos, clipCp, clipAdvance) || wordCp != clipCp) return WordFit::Fail;
    if (isJoinCjk(wordCp)) {
      if (units < UINT16_MAX) units++;
      inLatin = false;
    } else if (!inLatin) {
      if (units < UINT16_MAX) units++;
      inLatin = true;
    }
    wordPos += wordAdvance;
    pos += clipAdvance;
    consumed += clipAdvance;
    any = true;
  }
  return any ? WordFit::Match : WordFit::Empty;
}

struct Run {
  bool matched = false;
  bool mismatch = false;
  uint16_t startWord = 0;
  uint16_t endWord = 0;
  uint16_t units = 0;
  bool reachedClipEnd = false;
};

inline Run matchRun(const char* const* words, const uint16_t wordCount, const uint16_t startWord, const char* clip,
                    const size_t clipLen, size_t clipPos) {
  Run run;
  run.startWord = startWord;
  bool started = false;
  for (uint16_t wordIndex = startWord; wordIndex < wordCount; ++wordIndex) {
    clipPos = skipClipSpace(clip, clipLen, clipPos);
    if (clipPos >= clipLen) {
      run.reachedClipEnd = started;
      return run;
    }
    size_t consumed = 0;
    uint16_t wordUnits = 0;
    const WordFit fit = matchWord(clip, clipLen, clipPos, words[wordIndex], consumed, wordUnits);
    if (fit == WordFit::Empty) continue;
    if (fit == WordFit::Fail) {
      run.mismatch = started;
      run.matched = false;
      return run;
    }
    if (!started) {
      run.startWord = wordIndex;
      started = true;
      run.matched = true;
    }
    clipPos += consumed;
    run.endWord = wordIndex;
    run.units = static_cast<uint16_t>(std::min<unsigned>(UINT16_MAX, static_cast<unsigned>(run.units) + wordUnits));
  }
  clipPos = skipClipSpace(clip, clipLen, clipPos);
  run.reachedClipEnd = started && clipPos >= clipLen;
  return run;
}

inline uint32_t firstComparable(const char* word) {
  if (!word) return 0;
  size_t index = 0;
  if (static_cast<unsigned char>(word[0]) == 0xE2 && static_cast<unsigned char>(word[1]) == 0x80 &&
      static_cast<unsigned char>(word[2]) == 0x83) {
    index = 3;
  }
  const size_t length = textLength(word);
  while (index < length) {
    size_t space = 0;
    if (clipSpaceAdvance(word, length, index, space)) {
      index += space;
      continue;
    }
    uint32_t cp = 0;
    size_t advance = 0;
    if (!decodeAt(word, length, index, cp, advance)) return 0;
    if (cp == '-') {
      index += advance;
      continue;
    }
    return cp;
  }
  return 0;
}

}  // namespace detail

inline bool findClippingAnchor(const char* const* words, const uint16_t wordCount, const char* clippingText,
                               AnchorMatch& match, const AnchorSearch search = AnchorSearch::ReliableOnly) {
  match = {};
  if (!words || wordCount == 0 || !clippingText || clippingText[0] == '\0') return false;

  const size_t clipLen = detail::textLength(clippingText);
  const size_t clipBegin = skipClipSpace(clippingText, clipLen, 0);
  if (clipBegin >= clipLen) return false;
  const uint16_t totalUnits = countUnits(clippingText);
  if (totalUnits == 0) return false;
  const uint16_t minPartial = std::min<uint16_t>(totalUnits, 3);

  AnchorMatch bestReliable;
  bool reliableAmbiguous = false;
  bool haveReliable = false;
  AnchorMatch bestShort;
  bool shortAmbiguous = false;
  bool haveShort = false;

  const auto consider = [&](const uint16_t startWord, const size_t clipPos, const bool atClipStart) {
    const detail::Run run = detail::matchRun(words, wordCount, startWord, clippingText, clipLen, clipPos);
    if (!run.matched || run.mismatch || run.units == 0) return;
    const bool reliable =
        ClippingTextMatcher::isReliableRun(atClipStart ? 0 : 1, run.reachedClipEnd, run.units, minPartial);
    const bool anchoredShort = !reliable && (atClipStart || startWord == 0);
    if (!reliable && (search == AnchorSearch::ReliableOnly || !anchoredShort)) return;

    AnchorMatch candidate;
    candidate.startWord = run.startWord;
    candidate.endWord = run.endWord;
    candidate.units = run.units;
    candidate.startsAtClipStart = atClipStart;
    candidate.reachesClipEnd = run.reachedClipEnd;
    candidate.reliable = reliable;

    auto& best = reliable ? bestReliable : bestShort;
    auto& have = reliable ? haveReliable : haveShort;
    auto& ambiguous = reliable ? reliableAmbiguous : shortAmbiguous;
    if (!have || candidate.units > best.units) {
      best = candidate;
      have = true;
      ambiguous = false;
      return;
    }
    if (candidate.units == best.units &&
        (candidate.startWord != best.startWord || candidate.endWord != best.endWord)) {
      ambiguous = true;
    }
  };

  uint32_t clipStartCp = 0;
  size_t clipStartAdvance = 0;
  decodeAt(clippingText, clipLen, clipBegin, clipStartCp, clipStartAdvance);
  for (uint16_t wordIndex = 0; wordIndex < wordCount; ++wordIndex) {
    if (detail::firstComparable(words[wordIndex]) != clipStartCp) continue;
    consider(wordIndex, clipBegin, true);
  }

  if (wordCount > 0) {
    const uint32_t pageStartCp = detail::firstComparable(words[0]);
    if (pageStartCp != 0) {
      for (size_t index = 0; index < clipLen;) {
        uint32_t cp = 0;
        size_t advance = 0;
        if (!decodeAt(clippingText, clipLen, index, cp, advance)) break;
        if (index != clipBegin && cp == pageStartCp) consider(0, index, false);
        index += advance;
      }
    }
  }

  if (haveReliable && !reliableAmbiguous) {
    match = bestReliable;
    return true;
  }
  if (haveReliable && reliableAmbiguous) return false;
  if (haveShort && !shortAmbiguous) {
    match = bestShort;
    return true;
  }
  return false;
}

// combined words are the earlier page followed by the later page.
inline bool clipAnchorToPage(const AnchorMatch& combined, const uint16_t boundary, const bool pageIsFirst,
                             AnchorMatch& pageMatch) {
  if (boundary == 0 || combined.startWord > combined.endWord) return false;
  if (pageIsFirst) {
    if (combined.startWord >= boundary) return false;
    pageMatch = combined;
    pageMatch.endWord = combined.endWord < boundary ? combined.endWord : static_cast<uint16_t>(boundary - 1);
    pageMatch.reachesClipEnd = combined.reachesClipEnd && combined.endWord < boundary;
    pageMatch.reliable = true;
    return pageMatch.startWord <= pageMatch.endWord;
  }
  if (combined.endWord < boundary) return false;
  pageMatch = combined;
  pageMatch.startsAtClipStart = combined.startsAtClipStart && combined.startWord >= boundary;
  pageMatch.startWord = combined.startWord >= boundary ? static_cast<uint16_t>(combined.startWord - boundary) : 0;
  pageMatch.endWord = static_cast<uint16_t>(combined.endWord - boundary);
  pageMatch.reliable = true;
  return true;
}

}  // namespace ClippingTextAnchor
