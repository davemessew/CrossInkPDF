#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "PdfObjectParser.h"
#include "PdfStreamDecoder.h"
#include "PdfTestIo.h"

namespace {

uint32_t adler32(const std::vector<uint8_t>& bytes) {
  uint32_t a = 1;
  uint32_t b = 0;
  for (const uint8_t byte : bytes) {
    a = (a + byte) % 65521;
    b = (b + a) % 65521;
  }
  return (b << 16) | a;
}

std::vector<uint8_t> storedZlib(const std::vector<uint8_t>& input) {
  std::vector<uint8_t> output{0x78, 0x01};
  size_t offset = 0;
  do {
    const size_t length = std::min<size_t>(input.size() - offset, 65535);
    const bool final = offset + length == input.size();
    output.push_back(final ? 0x01 : 0x00);
    output.push_back(static_cast<uint8_t>(length));
    output.push_back(static_cast<uint8_t>(length >> 8));
    const uint16_t inverse = static_cast<uint16_t>(~length);
    output.push_back(static_cast<uint8_t>(inverse));
    output.push_back(static_cast<uint8_t>(inverse >> 8));
    output.insert(output.end(), input.begin() + static_cast<ptrdiff_t>(offset),
                  input.begin() + static_cast<ptrdiff_t>(offset + length));
    offset += length;
  } while (offset < input.size());
  const uint32_t checksum = adler32(input);
  output.push_back(static_cast<uint8_t>(checksum >> 24));
  output.push_back(static_cast<uint8_t>(checksum >> 16));
  output.push_back(static_cast<uint8_t>(checksum >> 8));
  output.push_back(static_cast<uint8_t>(checksum));
  return output;
}

std::vector<uint8_t> asciiHexEncode(const std::vector<uint8_t>& input) {
  static constexpr char HEX[] = "0123456789ABCDEF";
  std::vector<uint8_t> output;
  output.reserve(input.size() * 2 + 1);
  for (const uint8_t byte : input) {
    output.push_back(HEX[byte >> 4]);
    output.push_back(HEX[byte & 0x0f]);
  }
  output.push_back('>');
  return output;
}

std::vector<uint8_t> ascii85Encode(const std::vector<uint8_t>& input) {
  std::vector<uint8_t> output;
  for (size_t offset = 0; offset < input.size(); offset += 4) {
    const size_t length = std::min<size_t>(4, input.size() - offset);
    uint32_t value = 0;
    for (size_t index = 0; index < 4; ++index) {
      value <<= 8;
      if (index < length) {
        value |= input[offset + index];
      }
    }
    if (length == 4 && value == 0) {
      output.push_back('z');
      continue;
    }
    std::array<uint8_t, 5> digits{};
    for (size_t index = 5; index-- > 0;) {
      digits[index] = static_cast<uint8_t>(value % 85 + '!');
      value /= 85;
    }
    output.insert(output.end(), digits.begin(), digits.begin() + static_cast<ptrdiff_t>(length + 1));
  }
  output.push_back('~');
  output.push_back('>');
  return output;
}

struct DecodeResult {
  PdfStepResult result{};
  std::vector<uint8_t> bytes;
  uint64_t inputBytes = 0;
  uint64_t consumedInputBytes = 0;
  bool omitted = false;
  bool externalDictionary = false;
};

DecodeResult decode(const std::vector<uint8_t>& encoded, const std::vector<PdfStreamFilter>& filters,
                    const size_t maximumRead = static_cast<size_t>(-1),
                    const size_t maximumWrite = static_cast<size_t>(-1), const PdfStreamDecodeLimits limits = {},
                    const bool required = true) {
  PdfTestByteSource input(encoded);
  input.setMaximumRead(maximumRead);
  PdfTestByteSink output;
  output.setMaximumWrite(maximumWrite);
  std::array<uint8_t, PdfLimits::SourceBufferBytes> sourceBuffer{};
  std::array<uint8_t, PdfLimits::DecoderOutputBytes> outputBuffer{};
  auto dictionary = std::make_unique<uint8_t[]>(PdfLimits::UzlibDictionaryBytes);
  PdfStreamDecoder decoder({sourceBuffer.data(), sourceBuffer.size(), outputBuffer.data(), outputBuffer.size(),
                            dictionary.get(), PdfLimits::UzlibDictionaryBytes});
  DecodeResult decoded;
  const PdfStatus beginStatus = decoder.begin(input.source(), output.sink(), filters.data(),
                                              static_cast<uint8_t>(filters.size()), limits, required);
  if (!beginStatus.ok()) {
    decoded.result = PdfStepResult::failure(beginStatus);
  } else {
    while (true) {
      PdfWorkBudget budget{32, 4096};
      decoded.result = decoder.step(budget);
      if (!decoded.result.yielded()) {
        break;
      }
    }
  }
  decoded.bytes = output.bytes();
  decoded.inputBytes = decoder.inputBytes();
  decoded.consumedInputBytes = decoder.consumedInputBytes();
  decoded.omitted = decoder.omitted();
  decoded.externalDictionary = decoder.usesExternalDictionary();
  return decoded;
}

std::vector<uint8_t> bytes(const std::string& value) { return {value.begin(), value.end()}; }

struct FilterDictionaryHarness {
  explicit FilterDictionaryHarness(const std::string& dictionary)
      : input(bytes(dictionary)), source(input.source()), lexer(source, sourceBuffer.data(), sourceBuffer.size()),
        parser(lexer, arena) {}

  PdfStatus filters(std::array<PdfStreamFilter, PdfLimits::MaxFiltersPerStream>* const output,
                    uint8_t* const count) {
    parser.begin();
    for (uint16_t step = 0; step < 256U; ++step) {
      PdfWorkBudget budget{32, 4096};
      const PdfStepResult result = parser.step(budget);
      if (result.yielded()) {
        continue;
      }
      if (result.failed()) {
        return result.status;
      }
      return pdfStreamFiltersFromDictionary(arena, parser.rootIndex(), output->data(),
                                            static_cast<uint8_t>(output->size()), count);
    }
    return PdfStatus::failure(PdfError::BudgetExhausted);
  }

  PdfTestByteSource input;
  PdfByteSource source{};
  std::array<uint8_t, 512> sourceBuffer{};
  std::array<PdfValue, 32> values{};
  std::array<PdfDictionaryEntry, 24> dictionaries{};
  std::array<PdfArrayItem, 12> arrays{};
  std::array<uint8_t, 512> text{};
  PdfObjectArena arena{
      values.data(),       static_cast<uint16_t>(values.size()),
      dictionaries.data(), static_cast<uint16_t>(dictionaries.size()),
      arrays.data(),       static_cast<uint16_t>(arrays.size()),
      text.data(),         static_cast<uint16_t>(text.size()),
  };
  PdfLexer lexer;
  PdfObjectParser parser;
};

PdfStatus parseStreamFilters(const std::string& dictionary, uint8_t* const count = nullptr) {
  FilterDictionaryHarness harness(dictionary);
  std::array<PdfStreamFilter, PdfLimits::MaxFiltersPerStream> filters{};
  uint8_t localCount = 0;
  const PdfStatus status = harness.filters(&filters, &localCount);
  if (count != nullptr) {
    *count = localCount;
  }
  return status;
}

}  // namespace

TEST(PdfStreamDecoderTest, AcceptsAbsentNullAndBehaviorEquivalentDecodeParameters) {
  for (const char* dictionary : {
           "<< /Filter /FlateDecode >>",
           "<< /Filter /FlateDecode /DecodeParms null >>",
           "<< /Filter /FlateDecode /DecodeParms << >> >>",
           "<< /Filter /FlateDecode /DecodeParms "
           "<< /Predictor 1 /Colors 3 /BitsPerComponent 8 /Columns 17 >> >>",
           "<< /Filter /ASCII85Decode /DecodeParms << >> >>",
       }) {
    uint8_t count = 0;
    EXPECT_TRUE(parseStreamFilters(dictionary, &count).ok()) << dictionary;
    EXPECT_EQ(count, 1U) << dictionary;
  }

  uint8_t count = 0;
  EXPECT_TRUE(parseStreamFilters(
                  "<< /Filter [/ASCII85Decode /FlateDecode] "
                  "/DecodeParms [null << /Predictor 1 /Columns 7 >>] >>",
                  &count)
                  .ok());
  EXPECT_EQ(count, 2U);
}

TEST(PdfStreamDecoderTest, RejectsTransformingMalformedAndMisalignedDecodeParameters) {
  EXPECT_EQ(parseStreamFilters("<< /Filter /FlateDecode /DecodeParms << /Predictor 12 /Columns 7 >> >>").error,
            PdfError::UnsupportedFilter);
  EXPECT_EQ(parseStreamFilters("<< /Filter /FlateDecode /DecodeParms << /EarlyChange 1 >> >>").error,
            PdfError::UnsupportedFilter);
  EXPECT_EQ(parseStreamFilters("<< /Filter /ASCII85Decode /DecodeParms << /Predictor 1 >> >>").error,
            PdfError::UnsupportedFilter);
  EXPECT_EQ(parseStreamFilters(
                "<< /Filter [/ASCII85Decode /FlateDecode] /DecodeParms [<< >>] >>")
                .error,
            PdfError::Malformed);
  EXPECT_EQ(parseStreamFilters("<< /DecodeParms << /Predictor 1 >> >>").error, PdfError::Malformed);
  EXPECT_EQ(parseStreamFilters("<< /Filter /FlateDecode /DecodeParms << /Predictor /One >> >>").error,
            PdfError::Malformed);
  EXPECT_EQ(parseStreamFilters("<< /Filter /FlateDecode /DecodeParms 8 0 R >>").error, PdfError::Malformed);
  EXPECT_EQ(parseStreamFilters("<< /Filter /FlateDecode /Filter /ASCII85Decode >>").error, PdfError::Malformed);
  EXPECT_EQ(parseStreamFilters(
                "<< /Filter /FlateDecode /DecodeParms null /DecodeParms << /Predictor 1 >> >>")
                .error,
            PdfError::Malformed);
}

TEST(PdfStreamDecoderTest, DecodesRawAsciiHexAscii85FlateAndChains) {
  const std::vector<uint8_t> expected = bytes("Hello PDF filters");
  const std::vector<uint8_t> flate = storedZlib(expected);
  const std::vector<uint8_t> ascii85Flate = ascii85Encode(flate);
  std::vector<uint8_t> fourStage = asciiHexEncode(flate);
  fourStage = ascii85Encode(fourStage);
  fourStage = asciiHexEncode(fourStage);

  for (const auto& test : std::array<std::pair<std::vector<uint8_t>, std::vector<PdfStreamFilter>>, 6>{{
           {expected, {}},
           {asciiHexEncode(expected), {PdfStreamFilter::ASCIIHex}},
           {ascii85Encode(expected), {PdfStreamFilter::ASCII85}},
           {flate, {PdfStreamFilter::Flate}},
           {ascii85Flate, {PdfStreamFilter::ASCII85, PdfStreamFilter::Flate}},
           {fourStage,
            {PdfStreamFilter::ASCIIHex, PdfStreamFilter::ASCII85, PdfStreamFilter::ASCIIHex, PdfStreamFilter::Flate}},
       }}) {
    const DecodeResult result = decode(test.first, test.second);
    ASSERT_TRUE(result.result.complete());
    EXPECT_EQ(result.bytes, expected);
    if (std::find(test.second.begin(), test.second.end(), PdfStreamFilter::Flate) != test.second.end()) {
      EXPECT_TRUE(result.externalDictionary);
    }
  }
}

TEST(PdfStreamDecoderTest, HandlesAscii85ZeroTerminatorWhitespaceAndOddHexNibble) {
  const std::vector<uint8_t> ascii85{' ', 'z', '\r', '\n', '!', '<', 'N', '?', '~', '>'};
  const DecodeResult zero = decode(ascii85, {PdfStreamFilter::ASCII85});
  ASSERT_TRUE(zero.result.complete());
  EXPECT_EQ(zero.bytes, (std::vector<uint8_t>{0, 0, 0, 0, 1, 2, 3}));

  const DecodeResult odd = decode(bytes("48656c6c6f2>"), {PdfStreamFilter::ASCIIHex});
  ASSERT_TRUE(odd.result.complete());
  EXPECT_EQ(odd.bytes, (std::vector<uint8_t>{'H', 'e', 'l', 'l', 'o', 0x20}));
}

TEST(PdfStreamDecoderTest, ReportsExactFlateBoundaryWithoutContainerReadAhead) {
  const std::vector<uint8_t> expected = bytes("inline flate");
  const std::vector<uint8_t> encoded = storedZlib(expected);
  std::vector<uint8_t> withFollowingOperators = encoded;
  const std::vector<uint8_t> following = bytes(" EI Q BT (after) Tj ET");
  withFollowingOperators.insert(withFollowingOperators.end(), following.begin(),
                                following.end());

  const DecodeResult result =
      decode(withFollowingOperators, {PdfStreamFilter::Flate});

  ASSERT_TRUE(result.result.complete());
  EXPECT_EQ(result.bytes, expected);
  EXPECT_GT(result.inputBytes, encoded.size());
  EXPECT_EQ(result.consumedInputBytes, encoded.size());
}

TEST(PdfStreamDecoderTest, ProducesIdenticalBytesAcrossEveryInputAndOutputSplit) {
  const std::vector<uint8_t> expected = bytes("split-safe streaming output");
  const std::vector<uint8_t> encoded = ascii85Encode(storedZlib(expected));
  const std::vector<PdfStreamFilter> filters{PdfStreamFilter::ASCII85, PdfStreamFilter::Flate};

  for (size_t split = 1; split <= encoded.size(); ++split) {
    const DecodeResult result = decode(encoded, filters, split);
    ASSERT_TRUE(result.result.complete()) << "input split=" << split;
    EXPECT_EQ(result.bytes, expected) << "input split=" << split;
  }
  for (size_t split = 1; split <= expected.size(); ++split) {
    const DecodeResult result = decode(encoded, filters, static_cast<size_t>(-1), split);
    ASSERT_TRUE(result.result.complete()) << "output split=" << split;
    EXPECT_EQ(result.bytes, expected) << "output split=" << split;
  }
}

TEST(PdfStreamDecoderTest, RejectsTruncatedZlibAndAdlerData) {
  const std::vector<uint8_t> complete = storedZlib(bytes("checksum required"));
  for (size_t removed = 1; removed <= 6; ++removed) {
    const std::vector<uint8_t> truncated(complete.begin(), complete.end() - static_cast<ptrdiff_t>(removed));
    const DecodeResult result = decode(truncated, {PdfStreamFilter::Flate});
    EXPECT_TRUE(result.result.failed()) << "removed=" << removed;
    EXPECT_TRUE(result.result.status.error == PdfError::Malformed ||
                result.result.status.error == PdfError::UnexpectedEof)
        << "removed=" << removed;
  }
  std::vector<uint8_t> corrupt = complete;
  corrupt.back() ^= 0x01;
  EXPECT_EQ(decode(corrupt, {PdfStreamFilter::Flate}).result.status.error, PdfError::Malformed);
}

TEST(PdfStreamDecoderTest, EnforcesExpandedByteAndRatioCaps) {
  const std::vector<uint8_t> expected(300, 'A');
  const std::vector<uint8_t> encoded{0x78, 0xda, 0x73, 0x74, 0x1c, 0x05, 0xc4, 0x02, 0x00, 0xcb, 0x9e, 0x4c, 0x2d};
  PdfStreamDecodeLimits limits;
  limits.maxExpandedBytes = expected.size() - 1;
  EXPECT_EQ(decode(encoded, {PdfStreamFilter::Flate}, static_cast<size_t>(-1), static_cast<size_t>(-1), limits)
                .result.status.error,
            PdfError::ExpansionLimit);

  const std::vector<uint8_t> overTwoHundredToOne{0x78, 0xda, 0xed, 0xc1, 0x01, 0x0d, 0x00, 0x00, 0x00, 0xc2, 0xa0, 0x6c,
                                                 0xef, 0x5f, 0xca, 0x1c, 0x6e, 0x40, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00,
                                                 0x00, 0x00, 0x00, 0x00, 0xc0, 0xbf, 0x01, 0x87, 0xc1, 0xeb, 0x98};
  limits.maxExpandedBytes = 20000;
  limits.maxExpansionRatio = PdfLimits::MaxExpansionRatio;
  EXPECT_EQ(
      decode(overTwoHundredToOne, {PdfStreamFilter::Flate}, static_cast<size_t>(-1), static_cast<size_t>(-1), limits)
          .result.status.error,
      PdfError::ExpansionLimit);

  limits.maxExpandedBytes = 1000;
  limits.maxExpansionRatio = 2;
  EXPECT_EQ(decode(encoded, {PdfStreamFilter::Flate}, static_cast<size_t>(-1), static_cast<size_t>(-1), limits)
                .result.status.error,
            PdfError::ExpansionLimit);
}

TEST(PdfStreamDecoderTest, RejectsFifthAndRequiredUnsupportedFilterWithoutOutput) {
  const std::vector<uint8_t> input = bytes("must not commit");
  const DecodeResult fifth =
      decode(input, {PdfStreamFilter::ASCIIHex, PdfStreamFilter::ASCII85, PdfStreamFilter::ASCIIHex,
                     PdfStreamFilter::ASCII85, PdfStreamFilter::Flate});
  EXPECT_EQ(fifth.result.status.error, PdfError::LimitExceeded);
  EXPECT_TRUE(fifth.bytes.empty());

  const DecodeResult required = decode(input, {PdfStreamFilter::Lzw});
  EXPECT_EQ(required.result.status.error, PdfError::UnsupportedFilter);
  EXPECT_TRUE(required.bytes.empty());

  const DecodeResult optional =
      decode(input, {PdfStreamFilter::Lzw}, static_cast<size_t>(-1), static_cast<size_t>(-1), {}, false);
  EXPECT_TRUE(optional.result.complete());
  EXPECT_TRUE(optional.omitted);
  EXPECT_TRUE(optional.bytes.empty());
}
