#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "PdfLexer.h"
#include "PdfObjectParser.h"
#include "PdfTestIo.h"

namespace {

struct ArenaStorage {
  std::array<PdfValue, 64> values{};
  std::array<PdfDictionaryEntry, 64> dictionaries{};
  std::array<PdfArrayItem, 64> arrays{};
  std::array<uint8_t, 1024> text{};

  PdfObjectArena arena() {
    return {
        values.data(),       static_cast<uint16_t>(values.size()),
        dictionaries.data(), static_cast<uint16_t>(dictionaries.size()),
        arrays.data(),       static_cast<uint16_t>(arrays.size()),
        text.data(),         static_cast<uint16_t>(text.size()),
    };
  }
};

PdfStepResult nextWithBudgetOne(PdfLexer& lexer, PdfToken& token) {
  while (true) {
    PdfWorkBudget budget{1, 1};
    const PdfStepResult result = lexer.next(token, budget);
    if (!result.yielded()) {
      return result;
    }
  }
}

std::vector<std::pair<PdfTokenKind, std::string>> lexAll(const std::vector<uint8_t>& input, const size_t maximumRead,
                                                         const bool budgetOne) {
  PdfTestByteSource memory(input);
  memory.setMaximumRead(maximumRead);
  const PdfByteSource source = memory.source();
  std::array<uint8_t, 4096> sourceBuffer{};
  PdfLexer lexer(source, sourceBuffer.data(), sourceBuffer.size());
  std::vector<std::pair<PdfTokenKind, std::string>> tokens;
  while (true) {
    PdfToken token;
    PdfStepResult result;
    if (budgetOne) {
      result = nextWithBudgetOne(lexer, token);
    } else {
      do {
        PdfWorkBudget budget{32, 4096};
        result = lexer.next(token, budget);
      } while (result.yielded());
    }
    EXPECT_FALSE(result.failed());
    if (result.failed()) {
      return {};
    }
    if (token.kind == PdfTokenKind::End) {
      return tokens;
    }
    tokens.emplace_back(token.kind, std::string(token.bytes, token.length));
  }
}

}  // namespace

TEST(PdfLexerTest, DecodesNamesCommentsNestedStringsAndOddHex) {
  const std::string input = "% ignored\r\n[/A#20Name (outer (inner\\) value)\\nline) <48656C6C6F2> 12 0 R]";
  const std::vector<uint8_t> bytes(input.begin(), input.end());

  const auto tokens = lexAll(bytes, 3, false);

  ASSERT_EQ(tokens.size(), 8u);
  EXPECT_EQ(tokens[0], std::make_pair(PdfTokenKind::ArrayBegin, std::string()));
  EXPECT_EQ(tokens[1], std::make_pair(PdfTokenKind::Name, std::string("A Name")));
  EXPECT_EQ(tokens[2], std::make_pair(PdfTokenKind::String, std::string("outer (inner) value)\nline")));
  EXPECT_EQ(tokens[3], std::make_pair(PdfTokenKind::HexString, std::string("Hello ")));
  EXPECT_EQ(tokens[4], std::make_pair(PdfTokenKind::Integer, std::string("12")));
  EXPECT_EQ(tokens[5], std::make_pair(PdfTokenKind::Integer, std::string("0")));
  EXPECT_EQ(tokens[6], std::make_pair(PdfTokenKind::Keyword, std::string("R")));
  EXPECT_EQ(tokens[7], std::make_pair(PdfTokenKind::ArrayEnd, std::string()));
}

TEST(PdfLexerTest, ProducesIdenticalTokensWithOneOperationAndOneByteBudgets) {
  const std::string input = "<< /Type /Page /Count 12 /Enabled true /Box [0 0 612 792] >>";
  const std::vector<uint8_t> bytes(input.begin(), input.end());

  EXPECT_EQ(lexAll(bytes, static_cast<size_t>(-1), true), lexAll(bytes, static_cast<size_t>(-1), false));
}

TEST(PdfLexerTest, HandlesEveryTokenSplitAcrossFourKiBBoundary) {
  constexpr char TOKEN[] = "/Split#20Across";
  for (size_t split = 1; split < sizeof(TOKEN) - 1; ++split) {
    std::vector<uint8_t> bytes(4096 - split, ' ');
    bytes.insert(bytes.end(), TOKEN, TOKEN + sizeof(TOKEN) - 1);
    const auto tokens = lexAll(bytes, static_cast<size_t>(-1), false);
    ASSERT_EQ(tokens.size(), 1u) << "split=" << split;
    EXPECT_EQ(tokens[0], std::make_pair(PdfTokenKind::Name, std::string("Split Across")));
  }
}

TEST(PdfLexerTest, ReportsExactEofAndTokenLimitFailures) {
  {
    const std::string input = "(unterminated";
    PdfTestByteSource memory(std::vector<uint8_t>(input.begin(), input.end()));
    auto source = memory.source();
    std::array<uint8_t, 4096> sourceBuffer{};
    PdfLexer lexer(source, sourceBuffer.data(), sourceBuffer.size());
    PdfToken token;
    PdfWorkBudget budget{32, 4096};
    const PdfStepResult result = lexer.next(token, budget);
    EXPECT_TRUE(result.failed());
    EXPECT_EQ(result.status.error, PdfError::UnexpectedEof);
    EXPECT_EQ(result.status.offset, input.size());
  }
  {
    const std::string input = "/" + std::string(113, 'A');
    PdfTestByteSource memory(std::vector<uint8_t>(input.begin(), input.end()));
    auto source = memory.source();
    std::array<uint8_t, 4096> sourceBuffer{};
    PdfLexer lexer(source, sourceBuffer.data(), sourceBuffer.size());
    PdfToken token;
    PdfWorkBudget budget{32, 4096};
    const PdfStepResult result = lexer.next(token, budget);
    EXPECT_TRUE(result.failed());
    EXPECT_EQ(result.status.error, PdfError::LimitExceeded);
  }
}

TEST(PdfObjectParserTest, ParsesReferencesNestedContainersAndDecodedKeys) {
  const std::string input = "<< /Type /Page /Parent 2 0 R /Kids [3 0 R 4 0 R] /Label (A (nested\\) label)) >>";
  PdfTestByteSource memory(std::vector<uint8_t>(input.begin(), input.end()));
  auto source = memory.source();
  std::array<uint8_t, 4096> sourceBuffer{};
  PdfLexer lexer(source, sourceBuffer.data(), sourceBuffer.size());
  ArenaStorage storage;
  PdfObjectArena arena = storage.arena();
  PdfObjectParser parser(lexer, arena);
  parser.begin();

  PdfStepResult result;
  do {
    PdfWorkBudget budget{1, 1};
    result = parser.step(budget);
  } while (result.yielded());

  ASSERT_TRUE(result.complete());
  ASSERT_NE(parser.rootIndex(), PDF_INVALID_INDEX);
  const PdfValue& dictionary = arena.values[parser.rootIndex()];
  ASSERT_EQ(dictionary.kind, PdfValueKind::Dictionary);

  uint16_t valueIndex = PDF_INVALID_INDEX;
  ASSERT_TRUE(pdfDictionaryFind(arena, parser.rootIndex(), "Type", &valueIndex));
  EXPECT_TRUE(pdfTextEquals(arena, arena.values[valueIndex], "Page"));

  ASSERT_TRUE(pdfDictionaryFind(arena, parser.rootIndex(), "Parent", &valueIndex));
  EXPECT_EQ(arena.values[valueIndex].kind, PdfValueKind::Reference);
  EXPECT_EQ(arena.values[valueIndex].objectNumber, 2u);
  EXPECT_EQ(arena.values[valueIndex].generation, 0u);

  ASSERT_TRUE(pdfDictionaryFind(arena, parser.rootIndex(), "Kids", &valueIndex));
  ASSERT_EQ(arena.values[valueIndex].kind, PdfValueKind::Array);
  uint16_t childIndex = PDF_INVALID_INDEX;
  ASSERT_TRUE(pdfArrayAt(arena, valueIndex, 1, &childIndex));
  EXPECT_EQ(arena.values[childIndex].objectNumber, 4u);

  ASSERT_TRUE(pdfDictionaryFind(arena, parser.rootIndex(), "Label", &valueIndex));
  EXPECT_TRUE(pdfTextEquals(arena, arena.values[valueIndex], "A (nested) label)"));
}

TEST(PdfObjectParserTest, PreservesIntegerImmediatelyBeforeAnIndirectReference) {
  const std::string input = "[/Indexed /DeviceRGB 3 23 1 R]";
  PdfTestByteSource memory(std::vector<uint8_t>(input.begin(), input.end()));
  auto source = memory.source();
  std::array<uint8_t, 4096> sourceBuffer{};
  PdfLexer lexer(source, sourceBuffer.data(), sourceBuffer.size());
  ArenaStorage storage;
  PdfObjectArena arena = storage.arena();
  PdfObjectParser parser(lexer, arena);
  parser.begin();

  PdfStepResult result;
  do {
    PdfWorkBudget budget{1, 1};
    result = parser.step(budget);
  } while (result.yielded());

  ASSERT_TRUE(result.complete());
  ASSERT_NE(parser.rootIndex(), PDF_INVALID_INDEX);
  const PdfValue& array = arena.values[parser.rootIndex()];
  ASSERT_EQ(array.kind, PdfValueKind::Array);
  ASSERT_EQ(array.count, 4);

  uint16_t valueIndex = PDF_INVALID_INDEX;
  ASSERT_TRUE(pdfArrayAt(arena, parser.rootIndex(), 2, &valueIndex));
  ASSERT_EQ(arena.values[valueIndex].kind, PdfValueKind::Integer);
  EXPECT_EQ(arena.values[valueIndex].integerValue, 3);

  ASSERT_TRUE(pdfArrayAt(arena, parser.rootIndex(), 3, &valueIndex));
  ASSERT_EQ(arena.values[valueIndex].kind, PdfValueKind::Reference);
  EXPECT_EQ(arena.values[valueIndex].objectNumber, 23u);
  EXPECT_EQ(arena.values[valueIndex].generation, 1u);
}

TEST(PdfObjectParserTest, PreservesArbitraryIntegerRunsBeforeAnIndirectReference) {
  const std::string input = "[1 2 3 4 0 R]";
  PdfTestByteSource memory(std::vector<uint8_t>(input.begin(), input.end()));
  auto source = memory.source();
  std::array<uint8_t, 4096> sourceBuffer{};
  PdfLexer lexer(source, sourceBuffer.data(), sourceBuffer.size());
  ArenaStorage storage;
  PdfObjectArena arena = storage.arena();
  PdfObjectParser parser(lexer, arena);
  parser.begin();

  PdfStepResult result;
  do {
    PdfWorkBudget budget{1, 1};
    result = parser.step(budget);
  } while (result.yielded());

  ASSERT_TRUE(result.complete());
  ASSERT_NE(parser.rootIndex(), PDF_INVALID_INDEX);
  const PdfValue& array = arena.values[parser.rootIndex()];
  ASSERT_EQ(array.kind, PdfValueKind::Array);
  ASSERT_EQ(array.count, 4);

  uint16_t valueIndex = PDF_INVALID_INDEX;
  for (uint16_t ordinal = 0; ordinal < 3; ++ordinal) {
    ASSERT_TRUE(pdfArrayAt(arena, parser.rootIndex(), ordinal, &valueIndex));
    ASSERT_EQ(arena.values[valueIndex].kind, PdfValueKind::Integer);
    EXPECT_EQ(arena.values[valueIndex].integerValue, static_cast<int64_t>(ordinal + 1));
  }

  ASSERT_TRUE(pdfArrayAt(arena, parser.rootIndex(), 3, &valueIndex));
  ASSERT_EQ(arena.values[valueIndex].kind, PdfValueKind::Reference);
  EXPECT_EQ(arena.values[valueIndex].objectNumber, 4u);
  EXPECT_EQ(arena.values[valueIndex].generation, 0u);
}

TEST(PdfObjectParserTest, FlushesPendingIntegersBeforeANonReferenceAndArrayEnd) {
  const std::string input = "[1 2 3 /Stop]";
  PdfTestByteSource memory(std::vector<uint8_t>(input.begin(), input.end()));
  auto source = memory.source();
  std::array<uint8_t, 4096> sourceBuffer{};
  PdfLexer lexer(source, sourceBuffer.data(), sourceBuffer.size());
  ArenaStorage storage;
  PdfObjectArena arena = storage.arena();
  PdfObjectParser parser(lexer, arena);
  parser.begin();

  PdfStepResult result;
  do {
    PdfWorkBudget budget{1, 1};
    result = parser.step(budget);
  } while (result.yielded());

  ASSERT_TRUE(result.complete());
  ASSERT_NE(parser.rootIndex(), PDF_INVALID_INDEX);
  const PdfValue& array = arena.values[parser.rootIndex()];
  ASSERT_EQ(array.kind, PdfValueKind::Array);
  ASSERT_EQ(array.count, 4);

  uint16_t valueIndex = PDF_INVALID_INDEX;
  for (uint16_t ordinal = 0; ordinal < 3; ++ordinal) {
    ASSERT_TRUE(pdfArrayAt(arena, parser.rootIndex(), ordinal, &valueIndex));
    ASSERT_EQ(arena.values[valueIndex].kind, PdfValueKind::Integer);
    EXPECT_EQ(arena.values[valueIndex].integerValue, static_cast<int64_t>(ordinal + 1));
  }
  ASSERT_TRUE(pdfArrayAt(arena, parser.rootIndex(), 3, &valueIndex));
  EXPECT_TRUE(pdfTextEquals(arena, arena.values[valueIndex], "Stop"));
}

TEST(PdfObjectParserTest, CompletesRootIntegerAndLeavesTheFollowingTokenUnread) {
  const std::string input = "17 /After";
  PdfTestByteSource memory(std::vector<uint8_t>(input.begin(), input.end()));
  auto source = memory.source();
  std::array<uint8_t, 4096> sourceBuffer{};
  PdfLexer lexer(source, sourceBuffer.data(), sourceBuffer.size());
  ArenaStorage storage;
  PdfObjectArena arena = storage.arena();
  PdfObjectParser parser(lexer, arena);
  parser.begin();

  PdfStepResult result;
  do {
    PdfWorkBudget budget{1, 1};
    result = parser.step(budget);
  } while (result.yielded());

  ASSERT_TRUE(result.complete());
  ASSERT_NE(parser.rootIndex(), PDF_INVALID_INDEX);
  ASSERT_EQ(arena.values[parser.rootIndex()].kind, PdfValueKind::Integer);
  EXPECT_EQ(arena.values[parser.rootIndex()].integerValue, 17);

  PdfToken following;
  result = nextWithBudgetOne(lexer, following);
  ASSERT_TRUE(result.complete());
  EXPECT_EQ(following.kind, PdfTokenKind::Name);
  EXPECT_EQ(std::string(following.bytes, following.length), "After");
}

TEST(PdfObjectParserTest, RejectsOverflowingReferenceAfterAnIntegerRun) {
  const std::string input = "[1 2 4294967296 0 R]";
  PdfTestByteSource memory(std::vector<uint8_t>(input.begin(), input.end()));
  auto source = memory.source();
  std::array<uint8_t, 4096> sourceBuffer{};
  PdfLexer lexer(source, sourceBuffer.data(), sourceBuffer.size());
  ArenaStorage storage;
  PdfObjectArena arena = storage.arena();
  PdfObjectParser parser(lexer, arena);
  parser.begin();

  PdfStepResult result;
  do {
    PdfWorkBudget budget{1, 1};
    result = parser.step(budget);
  } while (result.yielded());

  EXPECT_TRUE(result.failed());
  EXPECT_EQ(result.status.error, PdfError::Malformed);
}

TEST(PdfObjectParserTest, RejectsMalformedNesting) {
  const std::string input = "<< /Broken [1 2 >>";
  PdfTestByteSource memory(std::vector<uint8_t>(input.begin(), input.end()));
  auto source = memory.source();
  std::array<uint8_t, 4096> sourceBuffer{};
  PdfLexer lexer(source, sourceBuffer.data(), sourceBuffer.size());
  ArenaStorage storage;
  PdfObjectArena arena = storage.arena();
  PdfObjectParser parser(lexer, arena);
  parser.begin();

  PdfStepResult result;
  do {
    PdfWorkBudget budget{32, 4096};
    result = parser.step(budget);
  } while (result.yielded());

  EXPECT_TRUE(result.failed());
  EXPECT_EQ(result.status.error, PdfError::Malformed);
}
