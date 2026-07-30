#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <limits>
#include <string>

#include "PdfWordCounter.h"

namespace {

PdfStatus countInChunks(const std::string& text, const size_t split, uint32_t* const words) {
  PdfWordCounter counter;
  PdfStatus status = counter.reset();
  if (status.ok()) {
    status = counter.consume(reinterpret_cast<const uint8_t*>(text.data()), split);
  }
  if (status.ok()) {
    status = counter.consume(reinterpret_cast<const uint8_t*>(text.data() + split), text.size() - split);
  }
  if (status.ok()) {
    status = counter.finish();
  }
  if (status.ok() && words != nullptr) {
    *words = counter.words();
  }
  return status;
}

}  // namespace

TEST(WordCounterTest, CountsLatinDigitsAndOnlyInternalApostrophesOrHyphensAsOneWord) {
  static constexpr char TEXT[] = "One two2 can't re-enter end- -start rock\xE2\x80\x99n\xE2\x80\x99roll";
  PdfWordCounter counter;
  ASSERT_TRUE(counter.reset().ok());
  for (size_t index = 0; index < sizeof(TEXT) - 1; ++index) {
    ASSERT_TRUE(counter.consume(reinterpret_cast<const uint8_t*>(TEXT) + index, 1).ok()) << index;
  }
  ASSERT_TRUE(counter.finish().ok());
  EXPECT_EQ(counter.words(), 7u);
}

TEST(WordCounterTest, CountsEachCjkReadingScalarAndIgnoresPunctuationOnlyText) {
  const std::string text = "Latin\xE4\xB8\xAD\xE6\x96\x87\xE3\x81\x8B\xE3\x81\xAA\xED\x95\x9C";
  PdfWordCounter counter;
  ASSERT_TRUE(counter.reset().ok());
  ASSERT_TRUE(counter.consume(reinterpret_cast<const uint8_t*>(text.data()), text.size()).ok());
  ASSERT_TRUE(counter.finish().ok());
  EXPECT_EQ(counter.words(), 6u);

  ASSERT_TRUE(counter.reset().ok());
  static constexpr char PUNCTUATION[] = " -- \xE2\x80\x99 ... !!! ";
  ASSERT_TRUE(counter.consume(reinterpret_cast<const uint8_t*>(PUNCTUATION), sizeof(PUNCTUATION) - 1).ok());
  ASSERT_TRUE(counter.finish().ok());
  EXPECT_EQ(counter.words(), 0u);
}

TEST(WordCounterTest, ProducesTheSameCountAtEveryMultibyteSplit) {
  const std::string text =
      "A\xC3\xA9\xE4\xB8\xAD\xF0\xA0\x80\x80"
      "B";
  for (size_t split = 0; split <= text.size(); ++split) {
    uint32_t words = 0;
    ASSERT_TRUE(countInChunks(text, split, &words).ok()) << split;
    EXPECT_EQ(words, 4u) << split;
  }
}

TEST(WordCounterTest, RejectsMalformedOverlongAndTruncatedUtf8) {
  for (const std::string& text : {
           std::string("\xC3(", 2),
           std::string("\xC0\xAF", 2),
           std::string("\xED\xA0\x80", 3),
       }) {
    PdfWordCounter counter;
    ASSERT_TRUE(counter.reset().ok());
    EXPECT_EQ(counter.consume(reinterpret_cast<const uint8_t*>(text.data()), text.size()).error, PdfError::Malformed);
  }

  PdfWordCounter truncated;
  ASSERT_TRUE(truncated.reset().ok());
  static constexpr uint8_t PREFIX[] = {0xF0, 0xA0, 0x80};
  ASSERT_TRUE(truncated.consume(PREFIX, sizeof(PREFIX)).ok());
  EXPECT_EQ(truncated.finish().error, PdfError::Malformed);
}

TEST(WordCounterTest, AcceptsMaximumCountAndRejectsRollover) {
  PdfWordCounter maximum;
  ASSERT_TRUE(maximum.reset(std::numeric_limits<uint32_t>::max()).ok());
  static constexpr char PUNCTUATION[] = "...";
  ASSERT_TRUE(maximum.consume(reinterpret_cast<const uint8_t*>(PUNCTUATION), sizeof(PUNCTUATION) - 1).ok());
  ASSERT_TRUE(maximum.finish().ok());
  EXPECT_EQ(maximum.words(), std::numeric_limits<uint32_t>::max());

  PdfWordCounter lastWord;
  ASSERT_TRUE(lastWord.reset(std::numeric_limits<uint32_t>::max() - 1).ok());
  ASSERT_TRUE(lastWord.consume(reinterpret_cast<const uint8_t*>("word"), 4).ok());
  ASSERT_TRUE(lastWord.finish().ok());
  EXPECT_EQ(lastWord.words(), std::numeric_limits<uint32_t>::max());

  PdfWordCounter overflow;
  ASSERT_TRUE(overflow.reset(std::numeric_limits<uint32_t>::max()).ok());
  EXPECT_EQ(overflow.consume(reinterpret_cast<const uint8_t*>("word"), 4).error, PdfError::LimitExceeded);
}
