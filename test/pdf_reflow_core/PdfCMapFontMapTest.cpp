#include <gtest/gtest.h>

#include <array>
#include <cstring>
#include <iomanip>
#include <sstream>
#include <string>
#include <vector>

#include "PdfCMap.h"
#include "PdfEncoding.h"
#include "PdfFontMap.h"
#include "PdfObjectParser.h"
#include "PdfTestIo.h"

namespace {

struct SourcePhase {
  bool sourceActive = false;
  uint32_t closeCount = 0;
  uint32_t openCount = 0;

  static PdfStatus set(void* context, const bool sourceRequired) {
    if (context == nullptr) {
      return PdfStatus::failure(PdfError::InvalidArgument);
    }
    auto& phase = *static_cast<SourcePhase*>(context);
    if (sourceRequired && !phase.sourceActive) {
      phase.sourceActive = true;
      ++phase.openCount;
    } else if (!sourceRequired && phase.sourceActive) {
      phase.sourceActive = false;
      ++phase.closeCount;
    }
    return PdfStatus::success();
  }
};

struct WriteOnlyCMapObserver {
  uint32_t writes = 0;

  static PdfStatus write(void* const context, const uint32_t, const void*, const size_t) {
    if (context == nullptr) {
      return PdfStatus::failure(PdfError::InvalidArgument);
    }
    ++static_cast<WriteOnlyCMapObserver*>(context)->writes;
    return PdfStatus::success();
  }

  PdfFixedRecordStore store() {
    return {this, 0, sizeof(PdfCMapRecord), nullptr, write};
  }
};

struct ArenaStorage {
  std::array<PdfValue, 64> values{};
  std::array<PdfDictionaryEntry, 32> dictionaries{};
  std::array<PdfArrayItem, 64> arrays{};
  std::array<uint8_t, 512> text{};

  PdfObjectArena arena() {
    return {
        values.data(),       static_cast<uint16_t>(values.size()),
        dictionaries.data(), static_cast<uint16_t>(dictionaries.size()),
        arrays.data(),       static_cast<uint16_t>(arrays.size()),
        text.data(),         static_cast<uint16_t>(text.size()),
    };
  }
};

PdfStepResult runCMap(PdfCMap& cmap) {
  PdfStepResult result;
  do {
    PdfWorkBudget budget{8, 64};
    result = cmap.step(budget);
  } while (result.yielded());
  return result;
}

std::string utf8(const PdfUtf8Value& value) { return {reinterpret_cast<const char*>(value.bytes), value.length}; }

PdfStepResult parseObject(PdfTestByteSource& input, PdfObjectArena& arena, uint16_t* rootIndex) {
  std::array<uint8_t, 128> sourceBuffer{};
  PdfLexer lexer(input.source(), sourceBuffer.data(), sourceBuffer.size());
  PdfObjectParser parser(lexer, arena);
  parser.begin();
  PdfStepResult result;
  do {
    PdfWorkBudget budget{32, 256};
    result = parser.step(budget);
  } while (result.yielded());
  if (result.complete() && rootIndex != nullptr) {
    *rootIndex = parser.rootIndex();
  }
  return result;
}

}  // namespace

TEST(PdfCMapTest, ParsesOneToFourByteCodesBfcharRangesArraysAndSurrogates) {
  const std::string source =
      "begincmap\n"
      "4 begincodespacerange\n"
      "<00> <7F>\n<8100> <81FF>\n<820000> <82FFFF>\n<83000000> <83FFFFFF>\n"
      "endcodespacerange\n"
      "4 beginbfchar\n"
      "<41> <0041>\n<8142> <03A9>\n<820043> <D83DDE00>\n<83000044> <00660069>\n"
      "endbfchar\n"
      "1 beginbfrange\n<50> <52> <0061>\nendbfrange\n"
      "1 beginbfrange\n<8160> <8161> [<03C0> <03A3>]\nendbfrange\n"
      "endcmap\n";
  PdfTestByteSource input(std::vector<uint8_t>(source.begin(), source.end()));
  std::array<uint8_t, 64> sourceBuffer{};
  std::array<PdfCMapRecord, 8> records{};
  PdfCMap cmap(sourceBuffer.data(), sourceBuffer.size(), {records.data(), records.size()});
  ASSERT_TRUE(cmap.begin(input.source()).ok());
  const PdfStepResult result = runCMap(cmap);
  ASSERT_TRUE(result.complete()) << static_cast<int>(result.status.error);
  EXPECT_EQ(cmap.codeSpaceCount(), 4u);
  EXPECT_EQ(cmap.mappingCount(), 7u);

  const std::array<std::pair<std::vector<uint8_t>, std::string>, 8> cases{{
      {{0x41}, "A"},
      {{0x81, 0x42}, "\xCE\xA9"},
      {{0x82, 0x00, 0x43}, "\xF0\x9F\x98\x80"},
      {{0x83, 0x00, 0x00, 0x44}, "fi"},
      {{0x50}, "a"},
      {{0x52}, "c"},
      {{0x81, 0x60}, "\xCF\x80"},
      {{0x81, 0x61}, "\xCE\xA3"},
  }};
  for (const auto& [encoded, expected] : cases) {
    PdfCMapLookup lookup;
    ASSERT_TRUE(cmap.lookup(encoded.data(), encoded.size(), &lookup).ok());
    EXPECT_EQ(utf8(lookup.unicode), expected);
    EXPECT_EQ(lookup.sourceLength, encoded.size());
  }
}

TEST(PdfCMapTest, CodeSpaceOnlyScanCollectsEveryBlockThroughEndCMapWithoutMaterializingMappings) {
  const std::string source =
      "begincmap "
      "1 begincodespacerange <00> <7F> endcodespacerange "
      "1 beginbfchar <41> <0041> endbfchar "
      "1 begincodespacerange <8100> <81FF> endcodespacerange "
      "1 beginbfchar <8142> <03A9> endbfchar "
      "endcmap end end";
  PdfTestByteSource input(std::vector<uint8_t>(source.begin(), source.end()));
  std::array<uint8_t, 64> sourceBuffer{};
  std::array<PdfCMapRecord, 1> scratchRecord{};
  WriteOnlyCMapObserver observer;
  PdfCMap cmap(sourceBuffer.data(), sourceBuffer.size(),
               {scratchRecord.data(), scratchRecord.size(), observer.store()});
  ASSERT_TRUE(cmap.begin(input.source()).ok());

  const PdfStepResult result = runCMap(cmap);

  ASSERT_TRUE(result.complete()) << static_cast<int>(result.status.error) << "@" << result.status.offset;
  std::array<PdfCMapCodeSpace, 2> codeSpaces{};
  uint8_t codeSpaceCount = 0;
  ASSERT_TRUE(cmap.copyCodeSpaces(codeSpaces.data(), codeSpaces.size(), &codeSpaceCount).ok());
  ASSERT_EQ(codeSpaceCount, 2U);
  EXPECT_EQ(codeSpaces[0].first, 0x00U);
  EXPECT_EQ(codeSpaces[0].last, 0x7FU);
  EXPECT_EQ(codeSpaces[0].length, 1U);
  EXPECT_EQ(codeSpaces[1].first, 0x8100U);
  EXPECT_EQ(codeSpaces[1].last, 0x81FFU);
  EXPECT_EQ(codeSpaces[1].length, 2U);
  EXPECT_EQ(cmap.mappingCount(), 2U);
  EXPECT_EQ(observer.writes, 0U);
}

TEST(PdfCMapTest, CodeSpaceOnlyScanStillRejectsMalformedMappings) {
  const std::string source =
      "begincmap "
      "1 begincodespacerange <00> <FF> endcodespacerange "
      "1 beginbfchar <41> /NotAHexDestination endbfchar "
      "endcmap";
  PdfTestByteSource input(std::vector<uint8_t>(source.begin(), source.end()));
  std::array<uint8_t, 64> sourceBuffer{};
  std::array<PdfCMapRecord, 1> scratchRecord{};
  WriteOnlyCMapObserver observer;
  PdfCMap cmap(sourceBuffer.data(), sourceBuffer.size(),
               {scratchRecord.data(), scratchRecord.size(), observer.store()});
  ASSERT_TRUE(cmap.begin(input.source()).ok());

  const PdfStepResult result = runCMap(cmap);

  ASSERT_TRUE(result.failed());
  EXPECT_EQ(result.status.error, PdfError::Malformed);
  EXPECT_EQ(observer.writes, 0U);
}

TEST(PdfCMapTest, SpillLookupClosesSourceAndSingleReaderGuardCatchesNegativeWitness) {
  const std::string source =
      "1 begincodespacerange <00> <FF> endcodespacerange "
      "2 beginbfchar <41> <0041> <42> <0042> endbfchar";
  PdfTestByteSource input(std::vector<uint8_t>(source.begin(), source.end()));
  std::array<uint8_t, 64> sourceBuffer{};
  std::array<PdfCMapRecord, 1> records{};
  PdfTestRecordStore spill(sizeof(PdfCMapRecord), 1);
  SourcePhase phase;
  spill.forbidReadsWhile(&phase.sourceActive);
  PdfCMap cmap(sourceBuffer.data(), sourceBuffer.size(),
               {records.data(), records.size(), spill.store(), &phase, SourcePhase::set});
  ASSERT_TRUE(cmap.begin(input.source()).ok());
  ASSERT_TRUE(runCMap(cmap).complete());
  ASSERT_TRUE(phase.sourceActive);

  PdfCMapRecord negativeWitness;
  EXPECT_EQ(pdfReadRecord(spill.store(), 0, &negativeWitness).error, PdfError::IoFailure);

  const uint8_t encoded = 0x42;
  PdfCMapLookup lookup;
  ASSERT_TRUE(cmap.lookup(&encoded, 1, &lookup).ok());
  EXPECT_EQ(utf8(lookup.unicode), "B");
  EXPECT_FALSE(phase.sourceActive);
  EXPECT_EQ(phase.closeCount, 1u);
}

TEST(PdfCMapTest, OmitsMappingBeyondAvailableStorage) {
  const std::string source =
      "1 begincodespacerange <00> <FF> endcodespacerange "
      "2 beginbfchar <41> <0041> <42> <0042> endbfchar";
  PdfTestByteSource input(std::vector<uint8_t>(source.begin(), source.end()));
  std::array<uint8_t, 64> sourceBuffer{};
  std::array<PdfCMapRecord, 1> records{};
  PdfTestRecordStore oneRecord(sizeof(PdfCMapRecord), 1);
  PdfCMap exact(sourceBuffer.data(), sourceBuffer.size(), {records.data(), records.size(), oneRecord.store()});
  ASSERT_TRUE(exact.begin(input.source()).ok());
  EXPECT_TRUE(runCMap(exact).complete());

  PdfTestRecordStore noRecords(sizeof(PdfCMapRecord), 0);
  PdfCMap shortByOne(sourceBuffer.data(), sourceBuffer.size(), {records.data(), records.size(), noRecords.store()});
  ASSERT_TRUE(shortByOne.begin(input.source()).ok());
  const PdfStepResult result = runCMap(shortByOne);
  ASSERT_TRUE(result.complete()) << static_cast<int>(result.status.error);
  EXPECT_EQ(shortByOne.mappingCount(), 1U);
  const uint8_t first = 0x41;
  const uint8_t omitted = 0x42;
  PdfCMapLookup lookup;
  ASSERT_TRUE(shortByOne.lookup(&first, 1, &lookup).ok());
  EXPECT_EQ(utf8(lookup.unicode), "A");
  EXPECT_EQ(shortByOne.lookup(&omitted, 1, &lookup).error, PdfError::UnsupportedEncoding);
}

TEST(PdfCMapTest, ArrayRangeAtMaximumFourByteCodeTerminatesWithoutWraparound) {
  const std::string source =
      "1 begincodespacerange <00000000> <FFFFFFFF> endcodespacerange "
      "1 beginbfrange <FFFFFFFF> <FFFFFFFF> [<0041>] endbfrange";
  PdfTestByteSource input(std::vector<uint8_t>(source.begin(), source.end()));
  std::array<uint8_t, 64> sourceBuffer{};
  std::array<PdfCMapRecord, 1> records{};
  PdfCMap cmap(sourceBuffer.data(), sourceBuffer.size(), {records.data(), records.size()});
  ASSERT_TRUE(cmap.begin(input.source()).ok());
  ASSERT_TRUE(runCMap(cmap).complete());
  const std::array<uint8_t, 4> encoded{0xFF, 0xFF, 0xFF, 0xFF};
  PdfCMapLookup lookup;
  ASSERT_TRUE(cmap.lookup(encoded.data(), encoded.size(), &lookup).ok());
  EXPECT_EQ(utf8(lookup.unicode), "A");
}

TEST(PdfCMapTest, RejectsSequentialRangeThatCrossesUtf16Surrogates) {
  const std::string source =
      "1 begincodespacerange <0000> <FFFF> endcodespacerange "
      "1 beginbfrange <0000> <0801> <D7FF> endbfrange";
  PdfTestByteSource input(std::vector<uint8_t>(source.begin(), source.end()));
  std::array<uint8_t, 64> sourceBuffer{};
  std::array<PdfCMapRecord, 1> records{};
  PdfCMap cmap(sourceBuffer.data(), sourceBuffer.size(), {records.data(), records.size()});
  ASSERT_TRUE(cmap.begin(input.source()).ok());
  const PdfStepResult result = runCMap(cmap);
  ASSERT_TRUE(result.failed());
  EXPECT_EQ(result.status.error, PdfError::Malformed);
}

TEST(PdfCMapTest, StoresLargeSequentialRangeAsOneCompactRecord) {
  const std::string source =
      "1 begincodespacerange <0000> <FFFF> endcodespacerange "
      "1 beginbfrange <0000> <3000> <0000> endbfrange";
  PdfTestByteSource input(std::vector<uint8_t>(source.begin(), source.end()));
  std::array<uint8_t, 64> sourceBuffer{};
  std::array<PdfCMapRecord, 1> records{};
  PdfCMap cmap(sourceBuffer.data(), sourceBuffer.size(), {records.data(), records.size()});
  ASSERT_TRUE(cmap.begin(input.source()).ok());
  ASSERT_TRUE(runCMap(cmap).complete());
  EXPECT_EQ(cmap.mappingCount(), 1U);
  const std::array<uint8_t, 2> encoded{0x30, 0x00};
  PdfCMapLookup lookup;
  ASSERT_TRUE(cmap.lookup(encoded.data(), encoded.size(), &lookup).ok());
  EXPECT_EQ(utf8(lookup.unicode), "\xE3\x80\x80");
}

TEST(PdfCMapTest, SortedSpillUsesLogarithmicReadsAndCachesTheLastRange) {
  std::ostringstream builder;
  builder << "1 begincodespacerange <0000> <FFFF> endcodespacerange "
          << "257 beginbfchar ";
  for (uint32_t code = 0; code <= 0x100; ++code) {
    builder << '<' << std::uppercase << std::hex << std::setw(4) << std::setfill('0') << code << "> <" << std::setw(4)
            << (0x0400 + code) << "> ";
  }
  builder << "endbfchar";
  const std::string source = builder.str();
  PdfTestByteSource input(std::vector<uint8_t>(source.begin(), source.end()));
  std::array<uint8_t, 64> sourceBuffer{};
  std::array<PdfCMapRecord, 1> records{};
  PdfTestRecordStore spill(sizeof(PdfCMapRecord), 256);
  SourcePhase phase;
  spill.forbidReadsWhile(&phase.sourceActive);
  PdfCMap cmap(sourceBuffer.data(), sourceBuffer.size(),
               {records.data(), records.size(), spill.store(), &phase, SourcePhase::set});
  ASSERT_TRUE(cmap.begin(input.source()).ok());
  ASSERT_TRUE(runCMap(cmap).complete());

  const std::array<uint8_t, 2> encoded{0x01, 0x00};
  PdfCMapLookup lookup;
  ASSERT_TRUE(cmap.lookup(encoded.data(), encoded.size(), &lookup).ok());
  EXPECT_EQ(utf8(lookup.unicode), "\xD4\x80");
  EXPECT_LE(spill.readCount(), 12u);
  const uint32_t readsAfterFirstLookup = spill.readCount();
  ASSERT_TRUE(cmap.lookup(encoded.data(), encoded.size(), &lookup).ok());
  EXPECT_EQ(spill.readCount(), readsAfterFirstLookup);
}

TEST(PdfCMapTest, OmitsUnsortedMappingsBeyondResidentStorage) {
  const std::string sourceText = "1 begincodespacerange <00> <ff> endcodespacerange "
                                 "1 beginbfchar <02> <0042> endbfchar "
                                 "1 beginbfchar <01> <0041> endbfchar";
  PdfTestByteSource source(std::vector<uint8_t>(sourceText.begin(), sourceText.end()));
  std::array<uint8_t, 128> sourceBuffer{};
  std::array<PdfCMapRecord, 1> records{};
  PdfTestRecordStore spill(sizeof(PdfCMapRecord), 2);
  PdfCMap cmap(sourceBuffer.data(), sourceBuffer.size(), {records.data(), records.size(), spill.store()});
  ASSERT_TRUE(cmap.begin(source.source()).ok());

  const PdfStepResult result = runCMap(cmap);

  ASSERT_TRUE(result.complete()) << static_cast<int>(result.status.error);
  EXPECT_EQ(cmap.mappingCount(), 1U);
  EXPECT_EQ(spill.readCount(), 0U);
  const uint8_t stored = 0x02;
  const uint8_t omitted = 0x01;
  PdfCMapLookup lookup;
  ASSERT_TRUE(cmap.lookup(&stored, 1, &lookup).ok());
  EXPECT_EQ(utf8(lookup.unicode), "B");
  EXPECT_EQ(cmap.lookup(&omitted, 1, &lookup).error, PdfError::UnsupportedEncoding);
}

TEST(PdfEncodingTest, AppliesDifferencesAndKeepsCommonEncodingTablesInFlash) {
  const std::string object = "[48 /zero 65 /Aacute /eacute 97 /adieresis 90 /Z.alt 128 /Euro]";
  PdfTestByteSource input(std::vector<uint8_t>(object.begin(), object.end()));
  ArenaStorage storage;
  PdfObjectArena arena = storage.arena();
  uint16_t rootIndex = PDF_INVALID_INDEX;
  ASSERT_TRUE(parseObject(input, arena, &rootIndex).complete());

  std::array<PdfEncodingDifference, 8> differences{};
  PdfSimpleEncoding encoding({differences.data(), differences.size()});
  ASSERT_TRUE(encoding.begin(PdfBaseEncoding::WinAnsi).ok());
  ASSERT_TRUE(encoding.applyDifferences(arena, rootIndex).ok());

  PdfUtf8Value value;
  ASSERT_TRUE(encoding.decode(48, &value).ok());
  EXPECT_EQ(utf8(value), "0");
  ASSERT_TRUE(encoding.decode(65, &value).ok());
  EXPECT_EQ(utf8(value), "\xC3\x81");
  ASSERT_TRUE(encoding.decode(66, &value).ok());
  EXPECT_EQ(utf8(value), "\xC3\xA9");
  ASSERT_TRUE(encoding.decode(97, &value).ok());
  EXPECT_EQ(utf8(value), "\xC3\xA4");
  ASSERT_TRUE(encoding.decode(90, &value).ok());
  EXPECT_EQ(utf8(value), "Z");
  ASSERT_TRUE(encoding.decode(128, &value).ok());
  EXPECT_EQ(utf8(value), "\xE2\x82\xAC");
  ASSERT_TRUE(encoding.decode(0xE9, &value).ok());
  EXPECT_EQ(utf8(value), "\xC3\xA9");

  PdfSimpleEncoding mac({differences.data(), differences.size()});
  ASSERT_TRUE(mac.begin(PdfBaseEncoding::MacRoman).ok());
  ASSERT_TRUE(mac.decode(0x80, &value).ok());
  EXPECT_EQ(utf8(value), "\xC3\x84");

  PdfSimpleEncoding pdfDoc({differences.data(), differences.size()});
  ASSERT_TRUE(pdfDoc.begin(PdfBaseEncoding::PdfDoc).ok());
  ASSERT_TRUE(pdfDoc.decode(0x80, &value).ok());
  EXPECT_EQ(utf8(value), "\xE2\x80\xA2");

  PdfSimpleEncoding standard({differences.data(), differences.size()});
  ASSERT_TRUE(standard.begin(PdfBaseEncoding::Standard).ok());
  ASSERT_TRUE(standard.decode(0xAE, &value).ok());
  EXPECT_EQ(utf8(value), "\xEF\xAC\x81");
  ASSERT_TRUE(standard.decode(0x27, &value).ok());
  EXPECT_EQ(utf8(value), "\xE2\x80\x99");

  PdfSimpleEncoding advPsMath({differences.data(), differences.size()});
  ASSERT_TRUE(advPsMath.begin(PdfBaseEncoding::AdvPSMP10).ok());
  ASSERT_TRUE(advPsMath.decode('c', &value).ok());
  EXPECT_EQ(utf8(value), "\xCE\xB3");
  ASSERT_TRUE(advPsMath.decode('d', &value).ok());
  EXPECT_EQ(utf8(value), "\xCE\xB4");
  ASSERT_TRUE(advPsMath.decode('l', &value).ok());
  EXPECT_EQ(utf8(value), "\xCE\xBB");
}

TEST(PdfEncodingTest, EmptyActualTextStringIsAValidSuppression) {
  PdfTestByteSink sink;
  const uint8_t empty = 0;
  EXPECT_TRUE(pdfDecodePdfTextString(&empty, 0, sink.sink()).ok());
  EXPECT_TRUE(sink.bytes().empty());
}

TEST(PdfFontMapTest, ToUnicodeWinsThenSimpleEncodingFallsBackConservatively) {
  const std::string cmapText =
      "1 begincodespacerange <00> <FF> endcodespacerange "
      "1 beginbfchar <41> <03A9> endbfchar";
  PdfTestByteSource cmapInput(std::vector<uint8_t>(cmapText.begin(), cmapText.end()));
  std::array<uint8_t, 64> cmapBuffer{};
  std::array<PdfCMapRecord, 2> cmapRecords{};
  PdfCMap cmap(cmapBuffer.data(), cmapBuffer.size(), {cmapRecords.data(), cmapRecords.size()});
  ASSERT_TRUE(cmap.begin(cmapInput.source()).ok());
  ASSERT_TRUE(runCMap(cmap).complete());

  std::array<PdfEncodingDifference, 2> differences{};
  PdfSimpleEncoding encoding({differences.data(), differences.size()});
  ASSERT_TRUE(encoding.begin(PdfBaseEncoding::Standard).ok());
  std::array<PdfFontWidthRecord, 4> widths{};
  PdfFontMap font({widths.data(), widths.size()});
  ASSERT_TRUE(font.begin(7, false, &cmap, &encoding).ok());
  ASSERT_TRUE(font.addWidth(65, 65, 610).ok());

  const uint8_t encoded = 65;
  PdfDecodedGlyph glyph;
  ASSERT_TRUE(font.decodeNext(&encoded, 1, &glyph).ok());
  EXPECT_EQ(utf8(glyph.unicode), "\xCE\xA9");
  EXPECT_EQ(glyph.width, 610);
  EXPECT_EQ(glyph.sourceLength, 1u);

  PdfFontMap fallbackFont({widths.data(), widths.size()});
  ASSERT_TRUE(fallbackFont.begin(8, false, nullptr, nullptr).ok());
  ASSERT_TRUE(fallbackFont.decodeNext(&encoded, 1, &glyph).ok());
  EXPECT_EQ(utf8(glyph.unicode), "A");

  PdfFontMap unsupportedCid({widths.data(), widths.size()});
  ASSERT_TRUE(unsupportedCid.begin(9, true, nullptr, nullptr, 1000).ok());
  EXPECT_EQ(unsupportedCid.decodeNext(&encoded, 1, &glyph).error, PdfError::UnsupportedEncoding);
}

TEST(PdfFontMapTest, LoadsSimpleAndCidWidthsAndSpillLookupClosesSource) {
  const std::string simpleObject = "[250 300]";
  PdfTestByteSource simpleInput(std::vector<uint8_t>(simpleObject.begin(), simpleObject.end()));
  ArenaStorage simpleStorage;
  PdfObjectArena simpleArena = simpleStorage.arena();
  uint16_t simpleRoot = PDF_INVALID_INDEX;
  ASSERT_TRUE(parseObject(simpleInput, simpleArena, &simpleRoot).complete());

  std::array<PdfFontWidthRecord, 1> widths{};
  PdfTestRecordStore spill(sizeof(PdfFontWidthRecord), 8);
  SourcePhase phase;
  spill.forbidReadsWhile(&phase.sourceActive);
  PdfFontMap font({widths.data(), widths.size(), spill.store(), &phase, SourcePhase::set});
  ASSERT_TRUE(font.begin(1, false, nullptr, nullptr, 500).ok());
  ASSERT_TRUE(font.loadSimpleWidths(simpleArena, 32, simpleRoot).ok());
  ASSERT_TRUE(phase.sourceActive);

  PdfFontWidthRecord negativeWitness;
  EXPECT_EQ(pdfReadRecord(spill.store(), 0, &negativeWitness).error, PdfError::IoFailure);
  int32_t width = 0;
  ASSERT_TRUE(font.widthFor(33, &width).ok());
  EXPECT_EQ(width, 300);
  EXPECT_FALSE(phase.sourceActive);
  const uint32_t readsAfterFirstWidth = spill.readCount();
  ASSERT_TRUE(font.widthFor(33, &width).ok());
  EXPECT_EQ(spill.readCount(), readsAfterFirstWidth);

  const std::string cidObject = "[1 [500 600] 10 12 700]";
  PdfTestByteSource cidInput(std::vector<uint8_t>(cidObject.begin(), cidObject.end()));
  ArenaStorage cidStorage;
  PdfObjectArena cidArena = cidStorage.arena();
  uint16_t cidRoot = PDF_INVALID_INDEX;
  ASSERT_TRUE(parseObject(cidInput, cidArena, &cidRoot).complete());
  std::array<PdfFontWidthRecord, 8> cidWidths{};
  PdfFontMap cidFont({cidWidths.data(), cidWidths.size()});
  ASSERT_TRUE(cidFont.begin(2, true, nullptr, nullptr, 1000).ok());
  ASSERT_TRUE(cidFont.loadCidWidths(cidArena, cidRoot).ok());
  ASSERT_TRUE(cidFont.widthFor(1, &width).ok());
  EXPECT_EQ(width, 500);
  ASSERT_TRUE(cidFont.widthFor(2, &width).ok());
  EXPECT_EQ(width, 600);
  ASSERT_TRUE(cidFont.widthFor(11, &width).ok());
  EXPECT_EQ(width, 700);
  ASSERT_TRUE(cidFont.widthFor(99, &width).ok());
  EXPECT_EQ(width, 1000);
}

TEST(PdfFontMapTest, RequiresOneSpillRecordAndDoesNotMaskEncodingIoFailure) {
  std::array<PdfFontWidthRecord, 1> widths{};
  PdfTestRecordStore oneWidth(sizeof(PdfFontWidthRecord), 1);
  PdfFontMap exact({widths.data(), widths.size(), oneWidth.store()});
  ASSERT_TRUE(exact.begin(1, false, nullptr, nullptr).ok());
  ASSERT_TRUE(exact.addWidth(1, 1, 500).ok());
  EXPECT_TRUE(exact.addWidth(2, 2, 600).ok());

  PdfFontMap shortByOne({widths.data(), widths.size()});
  ASSERT_TRUE(shortByOne.begin(2, false, nullptr, nullptr).ok());
  ASSERT_TRUE(shortByOne.addWidth(1, 1, 500).ok());
  EXPECT_EQ(shortByOne.addWidth(2, 2, 600).error, PdfError::LimitExceeded);

  const std::string differencesObject = "[65 /Omega 65 /A]";
  PdfTestByteSource input(std::vector<uint8_t>(differencesObject.begin(), differencesObject.end()));
  ArenaStorage storage;
  PdfObjectArena arena = storage.arena();
  uint16_t rootIndex = PDF_INVALID_INDEX;
  ASSERT_TRUE(parseObject(input, arena, &rootIndex).complete());
  std::array<PdfEncodingDifference, 1> differences{};
  PdfTestRecordStore differenceSpill(sizeof(PdfEncodingDifference), 1);
  bool sourceActive = true;
  differenceSpill.forbidReadsWhile(&sourceActive);
  PdfSimpleEncoding encoding({differences.data(), differences.size(), differenceSpill.store()});
  ASSERT_TRUE(encoding.begin(PdfBaseEncoding::Standard).ok());
  ASSERT_TRUE(encoding.applyDifferences(arena, rootIndex).ok());
  PdfFontMap font({widths.data(), widths.size()});
  ASSERT_TRUE(font.begin(3, false, nullptr, &encoding).ok());
  const uint8_t encoded = 'A';
  PdfDecodedGlyph glyph;
  EXPECT_EQ(font.decodeNext(&encoded, 1, &glyph).error, PdfError::IoFailure);
}

TEST(PdfFontMapTest, RejectsUnsortedWidthsBeforeTheyReachSpillStorage) {
  std::array<PdfFontWidthRecord, 2> widths{};
  PdfTestRecordStore spill(sizeof(PdfFontWidthRecord), 2);
  PdfFontMap font({widths.data(), widths.size(), spill.store()});
  ASSERT_TRUE(font.begin(7, true, nullptr, nullptr, 500).ok());
  ASSERT_TRUE(font.addWidth(20, 20, 500).ok());
  ASSERT_TRUE(font.addWidth(10, 10, 600).ok());

  const PdfStatus status = font.addWidth(30, 30, 700);

  EXPECT_EQ(status.error, PdfError::LimitExceeded);
  EXPECT_EQ(spill.readCount(), 0U);
}

TEST(PdfFontMapTest, RejectsNegativeFractionalWidthsBeforeTruncation) {
  const std::string widthsObject = "[-0.5]";
  PdfTestByteSource input(std::vector<uint8_t>(widthsObject.begin(), widthsObject.end()));
  ArenaStorage storage;
  PdfObjectArena arena = storage.arena();
  uint16_t rootIndex = PDF_INVALID_INDEX;
  ASSERT_TRUE(parseObject(input, arena, &rootIndex).complete());
  std::array<PdfFontWidthRecord, 1> widths{};
  PdfFontMap font({widths.data(), widths.size()});
  ASSERT_TRUE(font.begin(1, false, nullptr, nullptr).ok());
  EXPECT_EQ(font.loadSimpleWidths(arena, 32, rootIndex).error, PdfError::Malformed);
}

TEST(PdfFontMapTest, KeepsUnmaterializedSimpleAsciiReadableAfterTheGlyphBudgetIsFull) {
  std::array<PdfDecodedGlyph, 1> glyphs{};
  PdfFontMapWorkspace workspace{};
  workspace.materializedGlyphs = glyphs.data();
  workspace.materializedGlyphCapacity = static_cast<uint16_t>(glyphs.size());
  PdfFontMap font(workspace);
  ASSERT_TRUE(font.beginMaterialized(1, false).ok());

  PdfDecodedGlyph retained{};
  retained.sourceCode = 'A';
  retained.sourceLength = 1;
  retained.unicode.bytes[0] = 'A';
  retained.unicode.length = 1;
  retained.width = 500;
  ASSERT_TRUE(font.addMaterializedGlyph(retained).ok());

  const uint8_t encoded = 'B';
  PdfDecodedGlyph decoded{};
  ASSERT_TRUE(font.decodeNext(&encoded, 1, &decoded).ok());
  EXPECT_EQ(decoded.sourceCode, static_cast<uint32_t>('B'));
  EXPECT_EQ(decoded.sourceLength, 1U);
  EXPECT_EQ(utf8(decoded.unicode), "B");
  EXPECT_EQ(decoded.width, 500);

  const uint8_t encodedWinAnsi = 0xFC;
  ASSERT_TRUE(font.decodeNext(&encodedWinAnsi, 1, &decoded).ok());
  EXPECT_EQ(decoded.sourceCode, 0xFCU);
  EXPECT_EQ(decoded.sourceLength, 1U);
  EXPECT_EQ(utf8(decoded.unicode), "\xC3\xBC");
  EXPECT_EQ(decoded.width, 500);
}

TEST(PdfFontMapTest, KeepsIdentityCMapGlyphsOutOfTheMaterializedBudget) {
  std::array<PdfDecodedGlyph, 1> glyphs{};
  PdfFontMapWorkspace workspace{};
  workspace.materializedGlyphs = glyphs.data();
  workspace.materializedGlyphCapacity = static_cast<uint16_t>(glyphs.size());
  PdfFontMap font(workspace);
  ASSERT_TRUE(font.beginMaterialized(1, true, false,
                                     PdfMaterializedFallback::EstimatedIdentity)
                  .ok());

  PdfDecodedGlyph identity{};
  identity.sourceCode = 'A';
  identity.sourceLength = 1;
  identity.unicode.bytes[0] = 'A';
  identity.unicode.length = 1;
  identity.width = pdfEstimateGlyphWidth(identity.unicode);
  ASSERT_TRUE(font.addMaterializedGlyph(identity).ok());
  EXPECT_EQ(font.materializedGlyphCount(), 0U);

  const uint8_t encoded = 'A';
  PdfDecodedGlyph decoded{};
  ASSERT_TRUE(font.decodeNext(&encoded, 1, &decoded).ok());
  EXPECT_EQ(utf8(decoded.unicode), "A");
  EXPECT_EQ(decoded.width, 667);
}
