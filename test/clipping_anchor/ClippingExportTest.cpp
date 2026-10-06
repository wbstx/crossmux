#include <HalStorage.h>
#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "ClippingStore.h"
#include "clippings/ClipTextBuilder.h"
#include "clippings/ClippingTextAnchor.h"
#include "clippings/ClippingsManager.h"

namespace {

// Word height is the line pitch, as in ClipSelectionActivity.
constexpr int kLineHeight = 20;

// Selected words with page/line geometry, fed to ClipTextBuilder like ClipSelectionActivity does.
struct Selection {
  ClipWordStore store;
  std::vector<uint16_t> order;
  int lastPage = -1;
  uint16_t pageWords = 0;

  void add(const std::string& text, const int page, const int line, const int x, const int width,
           const bool paragraphStart = false) {
    if (page != lastPage) {
      lastPage = page;
      pageWords = 0;
    }
    WordRef word;
    word.x = x;
    word.y = line * kLineHeight;
    word.w = width;
    word.h = kLineHeight;
    word.pageIdx = page;
    word.pageWordIndex = pageWords++;
    word.paragraphStart = paragraphStart;
    ASSERT_TRUE(store.appendText(word, text.c_str()));
    order.push_back(static_cast<uint16_t>(store.words.size()));
    store.words.push_back(word);
  }

  // Lays out text one codepoint per word, `perLine` glyphs per line, `advance` px apart.
  void addChars(const std::string& text, const int perLine, const int advance = 12, const int linesPerPage = 1000) {
    int col = 0;
    int line = 0;
    for (size_t i = 0; i < text.size();) {
      uint32_t cp = 0;
      size_t len = 1;
      ClippingTextAnchor::decodeAt(text.data(), text.size(), i, cp, len);
      add(text.substr(i, len), line / linesPerPage, line % linesPerPage, col * advance, 12);
      i += len;
      if (++col == perLine) {
        col = 0;
        line++;
      }
    }
  }

  std::string build() const {
    return ClipTextBuilder::build(store, order.data(), 0, static_cast<int>(order.size()) - 1, 0, 10).text;
  }
};

std::string repeatChar(const char* utf8, const size_t count) {
  std::string out;
  for (size_t i = 0; i < count; ++i) out += utf8;
  return out;
}

bool validUtf8(const std::string& text) {
  for (size_t i = 0; i < text.size();) {
    const auto c = static_cast<unsigned char>(text[i]);
    const size_t len = c < 0x80 ? 1 : (c >> 5) == 0x6 ? 2 : (c >> 4) == 0xE ? 3 : (c >> 3) == 0x1E ? 4 : 0;
    if (len == 0 || i + len > text.size()) return false;
    for (size_t k = 1; k < len; ++k) {
      if ((static_cast<unsigned char>(text[i + k]) & 0xC0) != 0x80) return false;
    }
    i += len;
  }
  return true;
}

std::string exportedText(const std::string& selected) {
  Storage.contents.clear();
  EXPECT_TRUE(ClippingsManager::saveClipping("Book", "Author", "Ch 1", 5, selected));
  EXPECT_EQ(Storage.lastPath, ClippingsManager::CLIPPINGS_PATH);
  const std::string header = "Book (Author)\n- Your Highlight on Page 5 | Ch 1\n\n";
  const std::string separator = "\n==========\n";
  const std::string& out = Storage.contents;
  if (out.size() < header.size() + separator.size() || out.compare(0, header.size(), header) != 0 ||
      out.compare(out.size() - separator.size(), separator.size(), separator) != 0) {
    ADD_FAILURE() << "unexpected export framing: " << out.substr(0, 80);
    return {};
  }
  return out.substr(header.size(), out.size() - header.size() - separator.size());
}

}  // namespace

// --- Saved text building and export ---

// 2/3. Chinese wrapped across lines and pages is saved without visual line-break spaces.
TEST(ClippingExport, ChineseLineAndPageBreaksJoinWithoutSpaces) {
  Selection selection;
  selection.addChars("今天天气很好，我们出去散步了。", 5, 12, 2);
  EXPECT_EQ(selection.build(), "今天天气很好，我们出去散步了。");
}

// 2. Justification gaps between CJK glyphs on one line do not insert spaces.
TEST(ClippingExport, ChineseJustifiedGapsJoinWithoutSpaces) {
  Selection selection;
  selection.addChars("今天天气很好", 6, 18);
  EXPECT_EQ(selection.build(), "今天天气很好");
}

// 5. Paragraph starts become newlines; glued punctuation tokens stay intact.
TEST(ClippingExport, ChineseParagraphsAndGluedPunctuation) {
  Selection selection;
  const std::vector<std::string> first = {"\xE2\x80\x83他", "低", "声", "说：", "“我",
                                          "们", "走", "吧，", "我", "说……”"};
  for (size_t i = 0; i < first.size(); ++i) selection.add(first[i], 0, 0, static_cast<int>(i) * 12, 12, i == 0);
  selection.add("\xE2\x80\x83“字", 0, 1, 0, 24, true);
  selection.add("写", 0, 1, 24, 12);
  selection.add("得", 0, 1, 36, 12);
  selection.add("好。”", 0, 2, 0, 36);
  EXPECT_EQ(selection.build(), "他低声说：“我们走吧，我说……”\n“字写得好。”");
}

// 10. Hangul words keep spaces both within a line and across a line break.
TEST(ClippingExport, HangulKeepsSpaces) {
  Selection selection;
  selection.add("안녕", 0, 0, 0, 24);
  selection.add("하세요", 0, 0, 32, 36);
  selection.add("여러분", 0, 1, 0, 36);
  EXPECT_EQ(selection.build(), "안녕 하세요 여러분");
}

// 9. English words keep spaces across line breaks; inserted hyphens are removed.
TEST(ClippingExport, EnglishSpacesAndInsertedHyphen) {
  Selection selection;
  selection.add("the", 0, 0, 0, 30);
  selection.add("brave", 0, 0, 36, 50);
  selection.add("won-", 0, 0, 92, 40);
  selection.store.words.back().endsWithInsertedHyphen = true;
  selection.add("derful", 0, 1, 0, 60);
  selection.add("world.", 0, 1, 66, 60);
  EXPECT_EQ(selection.build(), "the brave wonderful world.");
}

// 12. Digits after CJK across a line break are joined (CJK join rule).
TEST(ClippingExport, MixedCjkDigitsAcrossLineBreakJoin) {
  Selection selection;
  selection.add("年", 0, 0, 0, 12);
  selection.add("1978", 0, 1, 0, 28);
  EXPECT_EQ(selection.build(), "年1978");
}

// Lays out tokens on one line; `spaced[i]` adds a 6 px authored space before token i, `stretch` is the justify gap.
void addLine(Selection& selection, const std::vector<std::string>& tokens, const std::vector<bool>& spaced,
             const int stretch) {
  int x = 0;
  for (size_t i = 0; i < tokens.size(); ++i) {
    if (i > 0) x += stretch + (spaced[i] ? 6 : 0);
    uint32_t cp = 0;
    size_t len = 1;
    ClippingTextAnchor::decodeAt(tokens[i].data(), tokens[i].size(), 0, cp, len);
    const int width = len == tokens[i].size() ? 12 : static_cast<int>(tokens[i].size()) * 7;
    selection.add(tokens[i], 0, 0, x, width);
    x += width;
  }
}

// 12. Authored spaces between CJK and digits on one line are kept, unjustified and justified.
TEST(ClippingExport, MixedCjkDigitsKeepAuthoredSpaceOnOneLine) {
  const std::vector<std::string> tokens = {"一", "九", "七", "八", "年", "1978", "年", "的"};
  const std::vector<bool> spaced = {false, false, false, false, false, true, true, false};
  for (const int stretch : {0, 9}) {
    Selection selection;
    addLine(selection, tokens, spaced, stretch);
    EXPECT_EQ(selection.build(), "一九七八年 1978 年的") << "stretch " << stretch;
  }
}

// 12. Authored spaces around a Latin word inside Chinese are kept, unjustified and justified.
TEST(ClippingExport, MixedCjkLatinKeepAuthoredSpaces) {
  const std::vector<std::string> tokens = {"我", "用", "iPhone", "拍", "照"};
  const std::vector<bool> spaced = {false, false, true, true, false};
  for (const int stretch : {0, 9}) {
    Selection selection;
    addLine(selection, tokens, spaced, stretch);
    EXPECT_EQ(selection.build(), "我用 iPhone 拍照") << "stretch " << stretch;
  }
}

// 12. CJK and Latin set without an authored space stay joined, even on a justified line.
TEST(ClippingExport, MixedCjkLatinWithoutSpaceStaysJoined) {
  const std::vector<std::string> tokens = {"我", "用", "iPhone", "拍", "照"};
  const std::vector<bool> spaced(tokens.size(), false);
  for (const int stretch : {0, 9}) {
    Selection selection;
    addLine(selection, tokens, spaced, stretch);
    EXPECT_EQ(selection.build(), "我用iPhone拍照") << "stretch " << stretch;
  }
}

// 12. A justified line of Latin words around Chinese keeps every authored space.
TEST(ClippingExport, MixedLatinPhraseInJustifiedChinese) {
  const std::vector<std::string> tokens = {"他", "说", "hello", "world", "就", "走", "了"};
  const std::vector<bool> spaced = {false, false, true, true, true, false, false};
  Selection selection;
  addLine(selection, tokens, spaced, 9);
  EXPECT_EQ(selection.build(), "他说 hello world 就走了");
}

// 11a. A 667-character CJK selection (2001 bytes) is saved and exported in full.
TEST(ClippingExport, SelectionJustOver2000BytesExportsInFull) {
  const std::string text = repeatChar("字", 666) + "终";
  ASSERT_EQ(text.size(), 2001u);
  Selection selection;
  selection.addChars(text, 20, 12, 25);
  const std::string saved = selection.build();
  EXPECT_EQ(saved, text);
  EXPECT_EQ(exportedText(saved), text);
}

// 11b. Text above the store cap is exported truncated on a UTF-8 boundary.
TEST(ClippingExport, TextOverCapTruncatesOnUtf8Boundary) {
  const std::string text = repeatChar("字", 1366);
  ASSERT_GT(text.size(), CLIPPING_TEXT_MAX);
  const std::string out = exportedText(text);
  EXPECT_LE(out.size(), CLIPPING_TEXT_MAX);
  EXPECT_EQ(out.size(), 4095u);
  EXPECT_TRUE(validUtf8(out));
  EXPECT_EQ(out, text.substr(0, out.size()));

  // A cap landing exactly on a character boundary keeps every byte up to the cap.
  const std::string aligned = "a" + repeatChar("字", 1366);
  EXPECT_EQ(exportedText(aligned), aligned.substr(0, CLIPPING_TEXT_MAX));
}
