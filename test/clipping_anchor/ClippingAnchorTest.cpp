#include <gtest/gtest.h>

#include <string>
#include <vector>

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
