#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "ClippingStore.h"
#include "clippings/ClippingMatchTracker.h"
#include "clippings/ClippingTextAnchor.h"

namespace {

ClippingTextAnchor::AnchorMatch find(const std::vector<const char*>& words, const char* text,
                                     const ClippingTextAnchor::AnchorSearch search =
                                         ClippingTextAnchor::AnchorSearch::ReliableOnly) {
  ClippingTextAnchor::AnchorMatch match;
  EXPECT_TRUE(ClippingTextAnchor::findClippingAnchor(words.data(), static_cast<uint16_t>(words.size()), text, match,
                                                     search));
  return match;
}

using Words = std::vector<std::string>;

// One page word per codepoint, like CJK layout.
Words chars(const std::string& text) {
  Words out;
  for (size_t i = 0; i < text.size();) {
    uint32_t cp = 0;
    size_t advance = 1;
    ClippingTextAnchor::decodeAt(text.data(), text.size(), i, cp, advance);
    out.emplace_back(text.substr(i, advance));
    i += advance;
  }
  return out;
}

Words concat(Words a, const Words& b) {
  a.insert(a.end(), b.begin(), b.end());
  return a;
}

std::vector<const char*> view(const Words& words) {
  std::vector<const char*> out;
  for (const auto& word : words) out.push_back(word.c_str());
  return out;
}

bool findIn(const Words& words, const char* text, ClippingTextAnchor::AnchorMatch& match,
            const ClippingTextAnchor::AnchorSearch search = ClippingTextAnchor::AnchorSearch::ReliableOnly) {
  const auto ptrs = view(words);
  return ClippingTextAnchor::findClippingAnchor(ptrs.data(), static_cast<uint16_t>(ptrs.size()), text, match, search);
}

// Mirrors ClippingController::matchClippingOnPage: reliable match, else a short anchored
// match confirmed against the previous or next page.
bool matchOnPage(const Words* previous, const Words& page, const Words* next, const char* text,
                 ClippingTextAnchor::AnchorMatch& out) {
  if (findIn(page, text, out)) return true;
  ClippingTextAnchor::AnchorMatch shortMatch;
  if (!findIn(page, text, shortMatch, ClippingTextAnchor::AnchorSearch::IncludeShort)) return false;
  for (const int direction : {-1, 1}) {
    const Words* neighbor = direction < 0 ? previous : next;
    if (!neighbor || neighbor->empty()) continue;
    const bool pageIsFirst = direction > 0;
    const Words combinedWords = pageIsFirst ? concat(page, *neighbor) : concat(*neighbor, page);
    const auto boundary = static_cast<uint16_t>(pageIsFirst ? page.size() : neighbor->size());
    ClippingTextAnchor::AnchorMatch combined;
    if (!findIn(combinedWords, text, combined) || combined.startWord >= boundary || combined.endWord < boundary) {
      continue;
    }
    if (ClippingTextAnchor::clipAnchorToPage(combined, boundary, pageIsFirst, out)) return true;
  }
  return false;
}

struct Range {
  bool found = false;
  uint16_t startWord = 0;
  uint16_t endWord = 0;
  bool fromStoredRange = false;
};

// Mirrors ClippingController::clippingRangeOnPage: stored range when the layout matches, else text.
Range rangeOnPage(const Clipping& clipping, const Words& page, const uint16_t currentPage, const uint16_t pageCount,
                  const uint32_t layoutSignature, const char* savedText) {
  Range range;
  if (clippingStoredRangeMatchesLayout(clipping, pageCount, layoutSignature)) {
    range.fromStoredRange = true;
    if (currentPage < clipping.startPage || currentPage > clipping.endPage) return range;
    range.startWord = currentPage == clipping.startPage ? clipping.startWordIndex : 0;
    range.endWord = currentPage == clipping.endPage ? clipping.endWordIndex : UINT16_MAX;
    range.found = range.startWord <= range.endWord;
    return range;
  }
  ClippingTextAnchor::AnchorMatch match;
  if (!matchOnPage(nullptr, page, nullptr, savedText, match)) return range;
  range.found = true;
  range.startWord = match.startWord;
  range.endWord = match.endWord;
  return range;
}

}  // namespace

TEST(ClippingAnchor, EnglishPhraseKeepsSurroundingWords) {
  const std::vector<const char*> words = {"See", "the", "brave", "new", "world", "today"};
  const auto match = find(words, "the brave new world");
  EXPECT_EQ(match.startWord, 1u);
  EXPECT_EQ(match.endWord, 4u);
  EXPECT_TRUE(match.startsAtClipStart);
  EXPECT_TRUE(match.reachesClipEnd);
  EXPECT_TRUE(match.reliable);
}

TEST(ClippingAnchor, EnglishIgnoresExtraSavedWhitespace) {
  const std::vector<const char*> words = {"the", "brave", "new"};
  const auto match = find(words, "the\nbrave  new");
  EXPECT_EQ(match.startWord, 0u);
  EXPECT_EQ(match.endWord, 2u);
  EXPECT_TRUE(match.reachesClipEnd);
}

TEST(ClippingAnchor, ShortEnglishFragmentIsNotReliableUntilTheClipEnds) {
  const std::vector<const char*> words = {"alpha", "beta"};
  ClippingTextAnchor::AnchorMatch match;
  EXPECT_FALSE(ClippingTextAnchor::findClippingAnchor(words.data(), 2, "alpha beta gamma delta", match));
  EXPECT_TRUE(ClippingTextAnchor::findClippingAnchor(words.data(), 2, "alpha beta gamma delta", match,
                                                     ClippingTextAnchor::AnchorSearch::IncludeShort));
  EXPECT_FALSE(match.reliable);
  EXPECT_TRUE(match.startsAtClipStart);
  EXPECT_FALSE(match.reachesClipEnd);
  EXPECT_EQ(match.endWord, 1u);
}

TEST(ClippingAnchor, RepeatedShortPhraseOnOnePageIsAmbiguous) {
  const std::vector<const char*> words = {"the", "cat", "and", "the", "dog"};
  ClippingTextAnchor::AnchorMatch match;
  EXPECT_FALSE(ClippingTextAnchor::findClippingAnchor(words.data(), 5, "the", match));
}

TEST(ClippingAnchor, ChineseCharactersMatchAcrossStraySpaces) {
  const std::vector<const char*> words = {"今", "天", "天", "气", "很", "好"};
  const auto match = find(words, "今 天天气很好");
  EXPECT_EQ(match.startWord, 0u);
  EXPECT_EQ(match.endWord, 5u);
  EXPECT_TRUE(match.startsAtClipStart);
  EXPECT_TRUE(match.reachesClipEnd);
}

TEST(ClippingAnchor, ChineseRelayoutStartsMidOldLine) {
  const std::vector<const char*> words = {"很", "好", "我", "们", "出", "去"};
  const auto match = find(words, "今天天气很好我们出去散步");
  EXPECT_EQ(match.startWord, 0u);
  EXPECT_EQ(match.endWord, 5u);
  EXPECT_FALSE(match.startsAtClipStart);
  EXPECT_FALSE(match.reachesClipEnd);
  EXPECT_GE(match.units, 3u);
}

TEST(ClippingAnchor, ChineseContinuationPageReachesClipEnd) {
  const std::vector<const char*> words = {"散", "步", "了", "。", "后", "来"};
  const auto match = find(words, "今天天气很好我们出去散步了");
  EXPECT_EQ(match.startWord, 0u);
  EXPECT_EQ(match.endWord, 2u);
  EXPECT_FALSE(match.startsAtClipStart);
  EXPECT_TRUE(match.reachesClipEnd);
}

TEST(ClippingAnchor, ChineseStartPageStopsAtPageEnd) {
  const std::vector<const char*> words = {"今", "天", "天", "气", "很"};
  const auto match = find(words, "今天天气很好我们出去");
  EXPECT_EQ(match.startWord, 0u);
  EXPECT_EQ(match.endWord, 4u);
  EXPECT_TRUE(match.startsAtClipStart);
  EXPECT_FALSE(match.reachesClipEnd);
}

TEST(ClippingAnchor, CrossPageConfirmationKeepsOnlyThisPage) {
  const std::vector<const char*> combined = {"今", "天", "天", "气", "很", "好", "我", "们"};
  ClippingTextAnchor::AnchorMatch combinedMatch;
  ASSERT_TRUE(ClippingTextAnchor::findClippingAnchor(combined.data(), 8, "今天天气很好我们出去", combinedMatch));
  EXPECT_TRUE(combinedMatch.startsAtClipStart);
  EXPECT_FALSE(combinedMatch.reachesClipEnd);

  ClippingTextAnchor::AnchorMatch firstPage;
  ASSERT_TRUE(ClippingTextAnchor::clipAnchorToPage(combinedMatch, 5, true, firstPage));
  EXPECT_EQ(firstPage.startWord, 0u);
  EXPECT_EQ(firstPage.endWord, 4u);
  EXPECT_TRUE(firstPage.startsAtClipStart);
  EXPECT_FALSE(firstPage.reachesClipEnd);

  ClippingTextAnchor::AnchorMatch secondPage;
  ASSERT_TRUE(ClippingTextAnchor::clipAnchorToPage(combinedMatch, 5, false, secondPage));
  EXPECT_EQ(secondPage.startWord, 0u);
  EXPECT_EQ(secondPage.endWord, 2u);
  EXPECT_FALSE(secondPage.startsAtClipStart);
}

TEST(ClippingAnchor, LongerMatchWinsOverAShorterSuffix) {
  const std::vector<const char*> words = {"天", "气", "今", "天", "天", "气", "很", "好"};
  const auto match = find(words, "今天天气很好");
  EXPECT_EQ(match.startWord, 2u);
  EXPECT_EQ(match.endWord, 7u);
  EXPECT_TRUE(match.reachesClipEnd);
}

TEST(ClippingAnchor, InsertedHyphenStillMatchesTheSavedWord) {
  const std::vector<const char*> words = {"hel-", "lo", "world"};
  const auto match = find(words, "hello world");
  EXPECT_EQ(match.startWord, 0u);
  EXPECT_EQ(match.endWord, 2u);
  EXPECT_TRUE(match.reachesClipEnd);
}

TEST(ClippingAnchor, CjkJoinSkipsSpacesButKeepsHangulSpacing) {
  EXPECT_TRUE(ClippingTextAnchor::joinsWithoutSpace("天", "气"));
  EXPECT_FALSE(ClippingTextAnchor::joinsWithoutSpace("hello", "world"));
  EXPECT_FALSE(ClippingTextAnchor::joinsWithoutSpace("안녕", "하세요"));
}

// --- Relayout re-anchoring ---

// 1. Single-line Chinese clipping is found again when every CJK character is its own page word.
TEST(ClippingAnchor, ChineseSingleLineRefoundAfterRelayout) {
  const Words page = chars("他走进屋里。今天天气很好，我们出去散步吧。她点点头。");
  ClippingTextAnchor::AnchorMatch match;
  ASSERT_TRUE(findIn(page, "今天天气很好，我们出去散步吧。", match));
  EXPECT_EQ(match.startWord, 6u);
  EXPECT_EQ(match.endWord, 20u);
  EXPECT_STREQ(page[match.endWord].c_str(), "。");
  EXPECT_TRUE(match.startsAtClipStart);
  EXPECT_TRUE(match.reachesClipEnd);
  EXPECT_TRUE(match.reliable);
}

// 2. Old visual line breaks (spaces/newlines) in saved Chinese text do not block a new layout.
TEST(ClippingAnchor, ChineseSavedLineBreaksIgnoredInNewLayout) {
  const Words page = chars("他走进屋里。今天天气很好，我们出去散步吧。她点点头。");
  ClippingTextAnchor::AnchorMatch match;
  ASSERT_TRUE(findIn(page, "\n今天天气很\n好，我们出 去散步\r\n吧。 ", match));
  EXPECT_EQ(match.startWord, 6u);
  EXPECT_EQ(match.endWord, 20u);
  EXPECT_TRUE(match.startsAtClipStart);
  EXPECT_TRUE(match.reachesClipEnd);
}

// 3. Cross-page Chinese clipping: start page runs off the end; continuation page starts mid old line.
TEST(ClippingAnchor, ChineseCrossPageStartAndContinuationRanges) {
  const char* saved = "我们出去散步了。路上遇见\n了老王，他说明天要下\n雨了。";
  const Words startPage = chars("她点点头。我们出去散步了。路上遇见");
  const Words continuation = chars("了老王，他说明天要下雨了。回家后天就黑了。");

  ClippingTextAnchor::AnchorMatch first;
  ASSERT_TRUE(matchOnPage(nullptr, startPage, &continuation, saved, first));
  EXPECT_EQ(first.startWord, 5u);
  EXPECT_EQ(first.endWord, startPage.size() - 1);
  EXPECT_TRUE(first.startsAtClipStart);
  EXPECT_FALSE(first.reachesClipEnd);

  // "了" occurs three times in the clip; only the run after "遇见" fits this page.
  ClippingTextAnchor::AnchorMatch second;
  ASSERT_TRUE(matchOnPage(&startPage, continuation, nullptr, saved, second));
  EXPECT_EQ(second.startWord, 0u);
  EXPECT_EQ(second.endWord, 12u);
  EXPECT_STREQ(continuation[second.endWord].c_str(), "。");
  EXPECT_STREQ(continuation[second.endWord + 1].c_str(), "回");
  EXPECT_FALSE(second.startsAtClipStart);
  EXPECT_TRUE(second.reachesClipEnd);
}

// 4a. A two-character tail at the top of a page needs the previous page to confirm it.
TEST(ClippingAnchor, ShortTailFragmentNeedsPreviousPageConfirmation) {
  const char* saved = "今天天气很好我们出去散步了。";
  const Words page = chars("了。后来下雨了。");
  const Words previous = chars("他说今天天气很好我们出去散步");
  const Words unrelated = chars("他走进屋里坐下。");

  ClippingTextAnchor::AnchorMatch match;
  EXPECT_FALSE(findIn(page, saved, match));
  ASSERT_TRUE(findIn(page, saved, match, ClippingTextAnchor::AnchorSearch::IncludeShort));
  EXPECT_FALSE(match.reliable);

  ASSERT_TRUE(matchOnPage(&previous, page, &unrelated, saved, match));
  EXPECT_EQ(match.startWord, 0u);
  EXPECT_EQ(match.endWord, 1u);
  EXPECT_TRUE(match.reachesClipEnd);

  EXPECT_FALSE(matchOnPage(&unrelated, page, &unrelated, saved, match));
  EXPECT_FALSE(matchOnPage(nullptr, page, nullptr, saved, match));
}

// 4b. A two-character head at the bottom of a page needs the next page to confirm it.
TEST(ClippingAnchor, ShortHeadFragmentNeedsNextPageConfirmation) {
  const char* saved = "今天天气很好。";
  const Words page = chars("他走进屋里。今天");
  const Words next = chars("天气很好。她点点头。");
  const Words unrelated = chars("她点点头。");

  ClippingTextAnchor::AnchorMatch match;
  EXPECT_FALSE(findIn(page, saved, match));
  ASSERT_TRUE(matchOnPage(&unrelated, page, &next, saved, match));
  EXPECT_EQ(match.startWord, 6u);
  EXPECT_EQ(match.endWord, 7u);
  EXPECT_TRUE(match.startsAtClipStart);
  EXPECT_FALSE(match.reachesClipEnd);

  EXPECT_FALSE(matchOnPage(&unrelated, page, &unrelated, saved, match));
}

namespace {
// Page tokens with em-space paragraph starts and glued Chinese punctuation.
const Words kDialogPage = {"\xE2\x80\x83他", "低", "声", "说：", "“我", "们", "走", "吧，", "我", "说……”",
                           "\xE2\x80\x83她", "没", "有", "回", "答。",
                           "\xE2\x80\x83“字", "写", "得", "真", "好。”", "他", "又", "说。"};
}  // namespace

// 5a. Multi-paragraph clipping with “”……：，。 and glued tokens matches the whole dialog.
TEST(ClippingAnchor, ChineseMultiParagraphPunctuationMatches) {
  ClippingTextAnchor::AnchorMatch match;
  ASSERT_TRUE(findIn(kDialogPage, "他低声说：“我们走吧，我说……”\n她没有回答。\n“字写得真好。”他又说。", match));
  EXPECT_EQ(match.startWord, 0u);
  EXPECT_EQ(match.endWord, kDialogPage.size() - 1);
  EXPECT_TRUE(match.startsAtClipStart);
  EXPECT_TRUE(match.reachesClipEnd);
}

// 5b. Clippings that start at “字 or end at 说……” anchor on the glued tokens.
TEST(ClippingAnchor, ChineseGluedPunctuationTokensBoundClipping) {
  ClippingTextAnchor::AnchorMatch match;
  ASSERT_TRUE(findIn(kDialogPage, "“字写得真好。”", match));
  EXPECT_EQ(match.startWord, 15u);
  EXPECT_EQ(match.endWord, 19u);
  EXPECT_TRUE(match.reachesClipEnd);

  ASSERT_TRUE(findIn(kDialogPage, "“我们走吧，我说……”\n她没有", match));
  EXPECT_EQ(match.startWord, 4u);
  EXPECT_EQ(match.endWord, 12u);
  EXPECT_TRUE(match.startsAtClipStart);
  EXPECT_TRUE(match.reachesClipEnd);

  // Saved text holds whole tokens; a start inside the glued “我 token is not anchored.
  EXPECT_FALSE(findIn(kDialogPage, "我们走吧，我说……”", match));
}

// 6a. Text repeated on one page is ambiguous and not highlighted.
TEST(ClippingAnchor, ChineseRepeatedSentenceIsNotHighlighted) {
  const Words page = chars("我爱你。他说。我爱你。");
  ClippingTextAnchor::AnchorMatch match;
  EXPECT_FALSE(findIn(page, "我爱你。", match));
  EXPECT_FALSE(findIn(page, "我爱你。", match, ClippingTextAnchor::AnchorSearch::IncludeShort));
  // Unique surrounding context still resolves.
  ASSERT_TRUE(findIn(page, "他说。我爱你。", match));
  EXPECT_EQ(match.startWord, 4u);
  EXPECT_EQ(match.endWord, 10u);
}

// 6b. The match tracker reports a repeated range as found but not unique.
TEST(ClippingAnchor, MatchTrackerFlagsDifferentRangesAsAmbiguous) {
  ClippingMatchTracker same;
  EXPECT_TRUE(same.record(3, 5));
  EXPECT_FALSE(same.record(3, 5));
  EXPECT_TRUE(same.unique());

  ClippingMatchTracker repeated;
  repeated.record(0, 3);
  repeated.record(7, 10);
  EXPECT_TRUE(repeated.found());
  EXPECT_FALSE(repeated.unique());
  EXPECT_EQ(repeated.startWord(), 0u);
}

// 7a. Highlight stops at the saved "，" and a later clipping on the page resolves on its own.
TEST(ClippingAnchor, HighlightStopsAtSavedEndAndSecondClipIsIndependent) {
  const Words page = chars("她的脸被晚霞染得血红，一声不响地走了。我说她太累了，她却笑了。");
  ClippingTextAnchor::AnchorMatch first;
  ASSERT_TRUE(findIn(page, "被晚霞染得血红，", first));
  EXPECT_EQ(first.startWord, 3u);
  EXPECT_EQ(first.endWord, 10u);
  EXPECT_STREQ(page[first.endWord].c_str(), "，");
  EXPECT_STREQ(page[first.endWord + 1].c_str(), "一");
  EXPECT_TRUE(first.reachesClipEnd);

  ClippingTextAnchor::AnchorMatch second;
  ASSERT_TRUE(findIn(page, "我说她太累了，", second));
  EXPECT_EQ(second.startWord, 19u);
  EXPECT_EQ(second.endWord, 25u);
  EXPECT_GT(second.startWord, first.endWord);
}

// 7b. Same end boundary when the comma is glued to the preceding character token.
TEST(ClippingAnchor, HighlightStopsAtGluedCommaToken) {
  const Words page = {"她", "的", "脸", "被", "晚", "霞", "染", "得", "血", "红，", "一", "声", "不", "响"};
  ClippingTextAnchor::AnchorMatch match;
  ASSERT_TRUE(findIn(page, "被晚霞染得血红，", match));
  EXPECT_EQ(match.startWord, 3u);
  EXPECT_EQ(match.endWord, 9u);
}

// 8a. Legacy record (signature 0, stale pageCount) ignores stale word indices and re-anchors from text.
TEST(ClippingAnchor, LegacyRecordReanchorsFromText) {
  const uint32_t layout = clippingWordLayoutSignature(0x1234u);
  const Words page = chars("他走进屋里。今天天气很好，我们出去散步吧。");
  Clipping legacy;
  legacy.layoutSignature = 0;
  legacy.pageCount = 9;
  legacy.startPage = 4;
  legacy.endPage = 4;
  legacy.startWordIndex = 0;
  legacy.endWordIndex = 2;
  EXPECT_FALSE(clippingStoredRangeMatchesLayout(legacy, 12, layout));
  legacy.pageCount = 12;
  EXPECT_FALSE(clippingStoredRangeMatchesLayout(legacy, 12, layout));

  const Range range = rangeOnPage(legacy, page, 4, 12, layout, "今天天气很好，我们出去散步吧。");
  EXPECT_TRUE(range.found);
  EXPECT_FALSE(range.fromStoredRange);
  EXPECT_EQ(range.startWord, 6u);
  EXPECT_EQ(range.endWord, 20u);
}

// 8b. Matching signature and pageCount use the stored word range directly.
TEST(ClippingAnchor, MatchingLayoutUsesStoredWordRange) {
  const uint32_t layout = clippingWordLayoutSignature(0x1234u);
  const Words page = chars("今天天气很好，我们出去散步吧。");
  Clipping stored;
  stored.layoutSignature = layout;
  stored.pageCount = 12;
  stored.startPage = 4;
  stored.endPage = 5;
  stored.startWordIndex = 2;
  stored.endWordIndex = 7;
  ASSERT_TRUE(clippingStoredRangeMatchesLayout(stored, 12, layout));
  EXPECT_FALSE(clippingStoredRangeMatchesLayout(stored, 12, 0));

  Range range = rangeOnPage(stored, page, 4, 12, layout, "unused");
  EXPECT_TRUE(range.fromStoredRange);
  EXPECT_EQ(range.startWord, 2u);
  EXPECT_EQ(range.endWord, UINT16_MAX);
  range = rangeOnPage(stored, page, 5, 12, layout, "unused");
  EXPECT_EQ(range.startWord, 0u);
  EXPECT_EQ(range.endWord, 7u);
  range = rangeOnPage(stored, page, 6, 12, layout, "unused");
  EXPECT_FALSE(range.found);
}

// 8c. A mismatched signature (or stale pageCount) falls back to the saved text.
TEST(ClippingAnchor, MismatchedLayoutFallsBackToText) {
  const uint32_t layout = clippingWordLayoutSignature(0x1234u);
  const Words page = chars("他走进屋里。今天天气很好，我们出去散步吧。");
  Clipping stored;
  stored.layoutSignature = clippingWordLayoutSignature(0x9999u);
  stored.pageCount = 12;
  stored.startPage = 4;
  stored.endPage = 4;
  stored.startWordIndex = 0;
  stored.endWordIndex = 1;
  Range range = rangeOnPage(stored, page, 4, 12, layout, "今天天气很好，我们出去散步吧。");
  EXPECT_FALSE(range.fromStoredRange);
  EXPECT_EQ(range.startWord, 6u);
  EXPECT_EQ(range.endWord, 20u);

  stored.layoutSignature = layout;
  range = rangeOnPage(stored, page, 4, 13, layout, "今天天气很好，我们出去散步吧。");
  EXPECT_FALSE(range.fromStoredRange);
  EXPECT_EQ(range.startWord, 6u);
}

// 8d. A record saved with the pre-version raw reader signature is flagged as legacy word layout.
TEST(ClippingAnchor, RawReaderSignatureIsLegacyWordLayout) {
  Clipping clipping;
  clipping.pageCount = 12;
  clipping.layoutSignature = 0x1234u;
  EXPECT_TRUE(clippingUsesLegacyWordLayout(clipping, 12, 0x1234u));
  clipping.layoutSignature = clippingWordLayoutSignature(0x1234u);
  EXPECT_FALSE(clippingUsesLegacyWordLayout(clipping, 12, 0x1234u));
}

// 8e. An authoritative text match is replayed only for its own layout signature.
TEST(ClippingAnchor, AuthoritativeTextMatchOnlyForSameSignature) {
  Clipping clipping;
  clipping.textMatchSignature = 77;
  clipping.textMatchStartPage = 3;
  clipping.textMatchEndPage = 4;
  clipping.textMatchStartWord = 10;
  clipping.textMatchEndWord = 5;
  clipping.textMatchBoundaries = CLIPPING_LAYOUT_BOUNDARIES_RESOLVED | CLIPPING_TEXT_MATCH_AUTHORITATIVE;
  uint16_t start = 0;
  uint16_t end = 0;
  ASSERT_TRUE(clippingTextMatchOnPage(clipping, 3, 77, start, end));
  EXPECT_EQ(start, 10u);
  EXPECT_EQ(end, UINT16_MAX);
  ASSERT_TRUE(clippingTextMatchOnPage(clipping, 4, 77, start, end));
  EXPECT_EQ(start, 0u);
  EXPECT_EQ(end, 5u);
  EXPECT_FALSE(clippingTextMatchOnPage(clipping, 3, 78, start, end));
  EXPECT_FALSE(clippingTextMatchOnPage(clipping, 5, 77, start, end));
}

// 9a. English clipping re-found after relayout: whitespace ignored, punctuation kept.
TEST(ClippingAnchor, EnglishRelayoutKeepsPunctuation) {
  const Words page = {"It", "was", "the", "best", "of", "times,", "it", "was", "the", "worst", "of", "times."};
  ClippingTextAnchor::AnchorMatch match;
  ASSERT_TRUE(findIn(page, "it was the worst\nof times.", match));
  EXPECT_EQ(match.startWord, 6u);
  EXPECT_EQ(match.endWord, 11u);
  EXPECT_TRUE(match.reachesClipEnd);
}

// 9b. Current rule: matching is case- and punctuation-exact.
TEST(ClippingAnchor, EnglishMatchIsCaseAndPunctuationExact) {
  const Words page = {"It", "was", "the", "best", "of", "times,", "it", "was", "the", "worst", "of", "times."};
  ClippingTextAnchor::AnchorMatch match;
  EXPECT_FALSE(findIn(page, "It was the worst of times.", match));
  EXPECT_FALSE(findIn(page, "the worst of times", match));
}

// 9c. English continuation page across a relayout reaches the clip end.
TEST(ClippingAnchor, EnglishContinuationPageAfterRelayout) {
  const Words page = {"worst", "of", "times.", "Then", "came"};
  ClippingTextAnchor::AnchorMatch match;
  ASSERT_TRUE(findIn(page, "It was the best of times, it was the worst of times.", match));
  EXPECT_EQ(match.startWord, 0u);
  EXPECT_EQ(match.endWord, 2u);
  EXPECT_FALSE(match.startsAtClipStart);
  EXPECT_TRUE(match.reachesClipEnd);
}

// 10. Hangul words keep their spacing in the join rule and still anchor.
TEST(ClippingAnchor, HangulKeepsSpacesAndAnchors) {
  EXPECT_FALSE(ClippingTextAnchor::joinsWithoutSpace("하세요", "여러분"));
  EXPECT_TRUE(ClippingTextAnchor::joinsWithoutSpace("안녕", "。"));
  const Words page = {"오늘은", "안녕", "하세요", "여러분", "반가워요"};
  ClippingTextAnchor::AnchorMatch match;
  ASSERT_TRUE(findIn(page, "안녕 하세요\n여러분", match));
  EXPECT_EQ(match.startWord, 1u);
  EXPECT_EQ(match.endWord, 3u);
  EXPECT_EQ(ClippingTextAnchor::countUnits("안녕 하세요 여러분"), 3u);
}

// 12. Mixed CJK, digits and Latin anchor with or without spaces in the saved text.
TEST(ClippingAnchor, MixedCjkAndDigitsAnchor) {
  const Words page = {"他", "生", "于", "一", "九", "七", "八", "年", "1978", "年", "的", "春", "天", "。"};
  ClippingTextAnchor::AnchorMatch match;
  ASSERT_TRUE(findIn(page, "一九七八年 1978 年", match));
  EXPECT_EQ(match.startWord, 3u);
  EXPECT_EQ(match.endWord, 9u);
  EXPECT_TRUE(match.reachesClipEnd);
  ASSERT_TRUE(findIn(page, "一九七八年1978年", match));
  EXPECT_EQ(match.startWord, 3u);
  EXPECT_EQ(match.endWord, 9u);
  EXPECT_EQ(ClippingTextAnchor::countUnits("一九七八年 1978 年"), 7u);

  const Words latin = {"我", "用", "iPhone", "拍", "照", "。"};
  ASSERT_TRUE(findIn(latin, "用 iPhone 拍照", match));
  EXPECT_EQ(match.startWord, 1u);
  EXPECT_EQ(match.endWord, 4u);
}
