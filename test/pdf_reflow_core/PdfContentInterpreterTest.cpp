#include <gtest/gtest.h>

#include <array>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "PdfContentInterpreter.h"
#include "PdfDocumentTextClassifier.h"
#include "PdfHiddenText.h"
#include "PdfPageTree.h"
#include "PdfSemanticWriter.h"
#include "PdfTestIo.h"

namespace {

struct InterpreterHarness {
  std::array<uint8_t, 512> sourceBuffer{};
  std::array<PdfContentOperand, 16> operands{};
  std::array<PdfContentArrayItem, 32> arrayItems{};
  std::array<uint8_t, 768> scratchText{};
  std::array<uint8_t, 512> markedText{};
  std::array<uint8_t, 4096> pageText{};
  std::array<PdfTextRun, 64> runs{};
  std::array<PdfImagePlacement, 16> images{};
  uint32_t documentOperatorCount = 0;
  PdfPageModel model;
  PdfContentInterpreter interpreter;

  explicit InterpreterHarness(const PdfRectangle pageBounds = {}, const PdfMatrix pageTransform = {},
                              const uint16_t runCapacity = 64U)
      : model({pageText.data(), pageText.size(), runs.data(), runCapacity, images.data(),
               static_cast<uint16_t>(images.size())}),
        interpreter({sourceBuffer.data(), sourceBuffer.size(), operands.data(), static_cast<uint8_t>(operands.size()),
                     arrayItems.data(), static_cast<uint8_t>(arrayItems.size()), scratchText.data(),
                     static_cast<uint16_t>(scratchText.size()), markedText.data(),
                     static_cast<uint16_t>(markedText.size()), &documentOperatorCount, pageTransform, pageBounds,
                     pageBounds.xMax > pageBounds.xMin && pageBounds.yMax > pageBounds.yMin}) {}
};

struct TestResourceTable {
  PdfFontMap* font = nullptr;
  PdfContentXObject image{};
  PdfContentXObject form{};
  PdfContentXObject secondaryForm{};
  PdfContentXObject loop{};
  PdfContentResources descriptor{};
  uint64_t inlineImageEncodedLength = 0;

  TestResourceTable() {
    descriptor.context = this;
    descriptor.resolveFont = resolveFont;
    descriptor.resolveXObject = resolveXObject;
    descriptor.consumeInlineImageToken = consumeInlineImageToken;
    descriptor.finishInlineImage = finishInlineImage;
  }

  static PdfStatus resolveFont(void* context, const uint8_t* name, const size_t length, PdfFontMap** font) {
    if (context == nullptr || name == nullptr || font == nullptr || length != 2 || name[0] != 'F' || name[1] != '1') {
      return PdfStatus::failure(PdfError::UnsupportedEncoding);
    }
    *font = static_cast<TestResourceTable*>(context)->font;
    return *font == nullptr ? PdfStatus::failure(PdfError::UnsupportedEncoding) : PdfStatus::success();
  }

  static PdfStatus resolveXObject(void* context, const uint8_t* name, const size_t length, PdfContentXObject* object) {
    if (context == nullptr || name == nullptr || object == nullptr) {
      return PdfStatus::failure(PdfError::InvalidArgument);
    }
    auto& table = *static_cast<TestResourceTable*>(context);
    if (length == 3 && std::memcmp(name, "Im1", 3) == 0) {
      *object = table.image;
      return PdfStatus::success();
    }
    if (length == 3 && std::memcmp(name, "Fm1", 3) == 0) {
      *object = table.form;
      return PdfStatus::success();
    }
    if (length == 3 && std::memcmp(name, "Fm2", 3) == 0) {
      *object = table.secondaryForm;
      return PdfStatus::success();
    }
    if (length == 4 && std::memcmp(name, "Loop", 4) == 0) {
      *object = table.loop;
      return PdfStatus::success();
    }
    return PdfStatus::failure(PdfError::Malformed);
  }

  static PdfStatus consumeInlineImageToken(void* context, const PdfToken&) {
    return context == nullptr ? PdfStatus::failure(PdfError::InvalidArgument) : PdfStatus::success();
  }

  static PdfStepResult finishInlineImage(void* context, const PdfByteSource& source, const uint64_t idEndOffset,
                                         PdfWorkBudget& budget, uint64_t* resumeOffset, PdfContentXObject*) {
    if (context == nullptr || resumeOffset == nullptr || !source.valid()) {
      return PdfStepResult::failure(PdfStatus::failure(PdfError::InvalidArgument, idEndOffset));
    }
    const uint64_t encodedLength = static_cast<TestResourceTable*>(context)->inlineImageEncodedLength;
    if (encodedLength == 0) {
      return PdfStepResult::failure(PdfStatus::failure(PdfError::UnsupportedFilter, idEndOffset));
    }
    if (!budget.consumeOperation() || budget.takeBytes(5) != 5) {
      return PdfStepResult::paused();
    }
    uint8_t separator = 0;
    size_t bytesRead = 0;
    PdfStatus status = source.readAt(source.context, idEndOffset, &separator, 1, &bytesRead);
    if (!status.ok() || bytesRead != 1 || (separator != ' ' && separator != '\t' && separator != '\r' &&
                                           separator != '\n' && separator != '\f' && separator != 0)) {
      return PdfStepResult::failure(status.ok() ? PdfStatus::failure(PdfError::Malformed, idEndOffset) : status);
    }
    uint64_t dataOffset = idEndOffset + 1;
    if (separator == '\r') {
      uint8_t lineFeed = 0;
      status = source.readAt(source.context, dataOffset, &lineFeed, 1, &bytesRead);
      if (!status.ok() || bytesRead != 1) {
        return PdfStepResult::failure(status.ok() ? PdfStatus::failure(PdfError::UnexpectedEof, dataOffset) : status);
      }
      if (lineFeed == '\n') {
        ++dataOffset;
      }
    }
    uint8_t terminator[4]{};
    status = source.readAt(source.context, dataOffset + encodedLength, terminator, sizeof(terminator), &bytesRead);
    const auto whitespace = [](const uint8_t byte) {
      return byte == ' ' || byte == '\t' || byte == '\r' || byte == '\n' || byte == '\f' || byte == 0;
    };
    if (!status.ok() || bytesRead != sizeof(terminator) || !whitespace(terminator[0]) || terminator[1] != 'E' ||
        terminator[2] != 'I' || !whitespace(terminator[3])) {
      return PdfStepResult::failure(status.ok() ? PdfStatus::failure(PdfError::Malformed, dataOffset + encodedLength)
                                                : status);
    }
    *resumeOffset = dataOffset + encodedLength + sizeof(terminator);
    return PdfStepResult::completed();
  }
};

struct DefaultFont {
  std::array<PdfEncodingDifference, 2> differences{};
  std::array<PdfFontWidthRecord, 4> widths{};
  PdfSimpleEncoding encoding;
  PdfFontMap font;

  DefaultFont()
      : encoding({differences.data(), static_cast<uint16_t>(differences.size())}),
        font({widths.data(), static_cast<uint16_t>(widths.size())}) {
    EXPECT_TRUE(encoding.begin(PdfBaseEncoding::Standard).ok());
    EXPECT_TRUE(font.begin(1, false, nullptr, &encoding, 500).ok());
    EXPECT_TRUE(font.addWidth(0, 255, 500).ok());
  }
};

PdfStepResult runInterpreter(PdfContentInterpreter& interpreter) {
  PdfStepResult result;
  do {
    PdfWorkBudget budget{4, 64};
    result = interpreter.step(budget);
  } while (result.yielded());
  return result;
}

template <typename Stepper>
PdfStepResult runBudgetOne(Stepper& stepper) {
  for (uint32_t step = 0; step < 65536U; ++step) {
    PdfWorkBudget budget{1, sizeof(PdfXrefEntry)};
    const PdfStepResult result = stepper.step(budget);
    if (!result.yielded()) {
      return result;
    }
  }
  return PdfStepResult::failure(PdfStatus::failure(PdfError::BudgetExhausted));
}

PdfStepResult runPageTree(PdfPageTreeWalker& walker) {
  for (uint16_t step = 0; step < 256U; ++step) {
    PdfWorkBudget budget{32, 4096};
    const PdfStepResult result = walker.step(budget);
    if (!result.yielded()) {
      return result;
    }
  }
  return PdfStepResult::failure(PdfStatus::failure(PdfError::BudgetExhausted));
}

PdfStepResult runResolver(PdfObjectResolver& resolver) {
  for (uint16_t step = 0; step < 256U; ++step) {
    PdfWorkBudget budget{32, 4096};
    const PdfStepResult result = resolver.step(budget);
    if (!result.yielded()) {
      return result;
    }
  }
  return PdfStepResult::failure(PdfStatus::failure(PdfError::BudgetExhausted));
}

std::string transcript(const PdfPageModel& model) {
  std::string result;
  for (uint16_t index = 0; index < model.runCount(); ++index) {
    const PdfTextRun& run = model.runs()[index];
    if (!result.empty()) {
      result.push_back(' ');
    }
    result.append(reinterpret_cast<const char*>(model.text() + run.textOffset), run.textLength);
  }
  return result;
}

std::vector<uint8_t> bytes(const std::string& value) { return {value.begin(), value.end()}; }

struct FixtureWorkspace {
  FixtureWorkspace() { traversalStorage.forbidReadsWhile(&traversalReadForbidden); }

  std::array<uint8_t, 4096> sourceBuffer{};
  std::array<PdfValue, 128> values{};
  std::array<PdfDictionaryEntry, 128> dictionaries{};
  std::array<PdfArrayItem, 128> arrays{};
  std::array<uint8_t, 2048> text{};
  PdfObjectArena arena{
      values.data(),       static_cast<uint16_t>(values.size()),
      dictionaries.data(), static_cast<uint16_t>(dictionaries.size()),
      arrays.data(),       static_cast<uint16_t>(arrays.size()),
      text.data(),         static_cast<uint16_t>(text.size()),
  };
  PdfTestRecordStore xrefStorage{sizeof(PdfXrefEntry), 128};
  PdfXrefTable xref{xrefStorage.store()};
  PdfTestRecordStore traversalStorage{sizeof(PdfPageTreeRecord), 64};
  PdfPageInfo page{};
  uint32_t pageCount = 0;
  bool sourceOpen = true;
  bool xrefBlocked = true;
  bool traversalOpen = false;
  bool traversalReadForbidden = true;
  uint32_t traversalOpenCount = 0;
  uint32_t traversalCloseCount = 0;

  static PdfStatus capturePage(void* context, const PdfPageInfo& page) {
    auto& workspace = *static_cast<FixtureWorkspace*>(context);
    if (workspace.pageCount == 0) {
      workspace.page = page;
    }
    ++workspace.pageCount;
    return PdfStatus::success();
  }

  static PdfStatus setTraversalAccess(void* context, const bool required) {
    if (context == nullptr) {
      return PdfStatus::failure(PdfError::InvalidArgument);
    }
    auto& workspace = *static_cast<FixtureWorkspace*>(context);
    workspace.traversalOpen = required;
    workspace.traversalReadForbidden = !required;
    workspace.sourceOpen = false;
    workspace.xrefBlocked = true;
    required ? ++workspace.traversalOpenCount : ++workspace.traversalCloseCount;
    return PdfStatus::success();
  }
};

struct FixtureTextResult {
  PdfStatus status{};
  std::string text;
  std::vector<uint8_t> pageText;
  std::vector<PdfTextRun> runs;
  std::vector<PdfImagePlacement> images;
};

struct FixtureSemanticResult {
  PdfStatus status{};
  std::string xhtml;
  std::vector<PdfSemanticBlockRecord> blocks;
  uint32_t totalWords = 0;

  static PdfStatus emit(void* context, const PdfSemanticBlockRecord& record) {
    if (context == nullptr) {
      return PdfStatus::failure(PdfError::InvalidArgument);
    }
    static_cast<FixtureSemanticResult*>(context)->blocks.push_back(record);
    return PdfStatus::success();
  }
};

FixtureTextResult fixtureFailure(const PdfStatus status) {
  FixtureTextResult result;
  result.status = status;
  return result;
}

FixtureSemanticResult writeFixtureSemantic(const FixtureTextResult& fixture) {
  FixtureSemanticResult result;
  PdfTestByteSink output;
  std::array<uint8_t, PdfSemanticWriterLimits::MinimumOutputBufferBytes> buffer{};
  PdfSemanticWriter writer;
  result.status = writer.begin(output.sink(), {&result, FixtureSemanticResult::emit}, {buffer.data(), buffer.size()});
  if (result.status.ok()) {
    result.status = writer.beginBlock({PdfSemanticBlockKind::Paragraph, 0, 0});
  }
  if (result.status.ok()) {
    result.status = writer.writeText(reinterpret_cast<const uint8_t*>(fixture.text.data()), fixture.text.size());
  }
  if (result.status.ok()) {
    result.status = writer.endBlock();
  }
  if (result.status.ok()) {
    result.status = writer.finish();
  }
  result.totalWords = writer.totalWords();
  result.xhtml.assign(output.bytes().begin(), output.bytes().end());
  return result;
}

FixtureTextResult interpretFontFixture(const char* filename) {
  const std::filesystem::path path = std::filesystem::path(__FILE__).parent_path() / "fixtures" / filename;
  std::ifstream input(path, std::ios::binary);
  std::vector<uint8_t> fixture{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
  PdfTestByteSource memory(fixture);
  const PdfByteSource source = memory.source();
  FixtureWorkspace workspace;
  PdfXrefParser xrefParser(source, workspace.sourceBuffer.data(), workspace.sourceBuffer.size(), workspace.arena,
                           workspace.xref);
  xrefParser.begin();
  PdfStepResult result = runBudgetOne(xrefParser);
  if (!result.complete()) {
    return fixtureFailure(result.status);
  }
  PdfObjectReference catalog;
  if (!workspace.xref.root(&catalog)) {
    return fixtureFailure(PdfStatus::failure(PdfError::Malformed));
  }
  PdfObjectResolver resolver(source, workspace.xref, workspace.sourceBuffer.data(), workspace.sourceBuffer.size(),
                             workspace.arena);
  PdfStatus status = resolver.begin(catalog);
  if (!status.ok() || !(result = runResolver(resolver)).complete()) {
    return fixtureFailure(status.ok() ? result.status : status);
  }
  uint16_t pagesIndex = PDF_INVALID_INDEX;
  if (!pdfDictionaryFind(workspace.arena, resolver.result().rootIndex, "Pages", &pagesIndex)) {
    return fixtureFailure(PdfStatus::failure(PdfError::Malformed));
  }
  const PdfValue pages = workspace.arena.values[pagesIndex];
  PdfPageTreeWalker walker(resolver, workspace.arena, workspace.traversalStorage.store(), FixtureWorkspace::capturePage,
                           &workspace, FixtureWorkspace::setTraversalAccess, &workspace, &workspace.page,
                           {}, PdfLimits::MaxPages);
  status = walker.begin({pages.objectNumber, pages.generation});
  if (!status.ok() || !(result = runPageTree(walker)).complete()) {
    return fixtureFailure(status.ok() ? result.status : status);
  }
  if (workspace.pageCount != 1 || workspace.page.contentCount != 1 || workspace.traversalOpenCount == 0 ||
      workspace.traversalOpenCount != workspace.traversalCloseCount || workspace.traversalOpen ||
      !workspace.traversalReadForbidden || workspace.sourceOpen || !workspace.xrefBlocked) {
    return fixtureFailure(PdfStatus::failure(PdfError::Malformed));
  }
  status = resolver.begin(workspace.page.contents[0]);
  if (!status.ok() || !(result = runResolver(resolver)).complete() || !resolver.result().hasStream) {
    return fixtureFailure(status.ok() ? result.status : status);
  }
  PdfByteRange range;
  status = pdfInitializeByteRange(source, resolver.result().streamOffset, resolver.result().streamLength, &range);
  if (!status.ok()) {
    return fixtureFailure(status);
  }
  const PdfByteSource content = pdfByteRangeSource(range);
  DefaultFont defaultFont;
  TestResourceTable resources;
  resources.font = &defaultFont.font;
  resources.image.kind = PdfContentXObjectKind::Image;
  resources.image.reference = {6, 0};
  resources.image.pixelWidth = 4;
  resources.image.pixelHeight = 4;
  InterpreterHarness harness;
  status = harness.interpreter.begin(&content, 1, resources.descriptor, harness.model);
  if (!status.ok() || !(result = runInterpreter(harness.interpreter)).complete()) {
    return fixtureFailure(status.ok() ? result.status : status);
  }
  return {
      PdfStatus::success(),
      transcript(harness.model),
      {harness.model.text(), harness.model.text() + harness.model.textLength()},
      {harness.model.runs(), harness.model.runs() + harness.model.runCount()},
      {harness.model.images(), harness.model.images() + harness.model.imageCount()},
  };
}

}  // namespace

TEST(PdfContentInterpreterTest, HandlesTextStateShowingOperatorsAndContentArrays) {
  DefaultFont defaultFont;
  TestResourceTable resources;
  resources.font = &defaultFont.font;
  PdfTestByteSource first(
      bytes("BT /F1 6 Tf 1 Tc 2 Tw 100 Tz 14 TL 1 Ts 0 Tr "
            "1 0 0 1 10 700 Tm (A) Tj 0 -20 Td (B) ' "
            "3 4 (C) \" [(D) -120 (E)] TJ 0 -20 TD T* (F) Tj ET"));
  PdfTestByteSource second(bytes("BT /F1 72 Tf 1 0 0 1 10 500 Tm (G)"));
  PdfTestByteSource third(bytes("Tj ET"));
  const std::array<PdfByteSource, 3> sources{first.source(), second.source(), third.source()};
  InterpreterHarness harness;
  ASSERT_TRUE(harness.interpreter.begin(sources.data(), sources.size(), resources.descriptor, harness.model).ok());
  const PdfStepResult result = runInterpreter(harness.interpreter);
  ASSERT_TRUE(result.complete()) << static_cast<int>(result.status.error);
  EXPECT_EQ(transcript(harness.model), "A B C D E F G");
  EXPECT_EQ(harness.model.runCount(), 7u);
  EXPECT_GT(harness.model.runs()[4].xMin, harness.model.runs()[3].xMin);
  EXPECT_GT(harness.interpreter.operatorCount(), 10u);
}

TEST(PdfContentInterpreterTest, StreamsTextArraysLargerThanTheFixedOperandWorkspace) {
  DefaultFont defaultFont;
  TestResourceTable resources;
  resources.font = &defaultFont.font;
  std::string content = "BT /F1 10 Tf 1 0 0 1 10 20 Tm [";
  std::string expected;
  for (uint8_t index = 0; index < 48; ++index) {
    content += "(a) -10 ";
    expected += 'a';
  }
  content += "] TJ ET";
  PdfTestByteSource page(bytes(content));
  const PdfByteSource source = page.source();
  InterpreterHarness harness;
  ASSERT_TRUE(harness.interpreter.begin(&source, 1, resources.descriptor, harness.model).ok());

  const PdfStepResult result = runInterpreter(harness.interpreter);

  ASSERT_TRUE(result.complete()) << static_cast<int>(result.status.error);
  EXPECT_EQ(transcript(harness.model), expected);
}

TEST(PdfContentInterpreterTest, UsesTjAdjustmentsInsteadOfFallbackWidthsForWordBoundaries) {
  DefaultFont defaultFont;
  TestResourceTable resources;
  resources.font = &defaultFont.font;
  PdfTestByteSource page(bytes(
      "BT /F1 10 Tf 1 0 0 1 10 20 Tm "
      "[(S) 13 (ynthesis) -345 (of) -333 (Azido) -334 (T) 82 (ertiary)] TJ ET"));
  const PdfByteSource source = page.source();
  InterpreterHarness harness;
  ASSERT_TRUE(harness.interpreter.begin(&source, 1, resources.descriptor, harness.model).ok());

  const PdfStepResult result = runInterpreter(harness.interpreter);

  ASSERT_TRUE(result.complete()) << static_cast<int>(result.status.error);
  EXPECT_EQ(transcript(harness.model), "Synthesis of Azido Tertiary");
  ASSERT_EQ(harness.model.runCount(), 1U);
}

TEST(PdfContentInterpreterTest, PreservesSoftHyphenWordJoinAfterRunWorkspaceFills) {
  std::array<uint8_t, 64> text{};
  std::array<PdfTextRun, 1> runs{};
  std::array<PdfImagePlacement, 1> images{};
  PdfPageModel model({text.data(), text.size(), runs.data(), static_cast<uint16_t>(runs.size()), images.data(),
                      static_cast<uint16_t>(images.size())});
  ASSERT_TRUE(model.reset().ok());

  PdfTextRun run{};
  ASSERT_TRUE(model.beginTextRun(run).ok());
  ASSERT_TRUE(model.appendText(reinterpret_cast<const uint8_t*>("degra"), 5U).ok());
  ASSERT_TRUE(model.finishTextRun().ok());

  uint16_t runIndex = 0;
  ASSERT_TRUE(model.beginOverflowTextRun(run, &runIndex).ok());
  const uint8_t softHyphen[]{0xC2U, 0xADU};
  ASSERT_TRUE(model.appendOverflowText(softHyphen, sizeof(softHyphen)).ok());
  ASSERT_TRUE(model.beginOverflowTextRun(run, &runIndex).ok());
  ASSERT_TRUE(model.appendOverflowText(reinterpret_cast<const uint8_t*>("dation"), 6U).ok());

  EXPECT_EQ(std::string(reinterpret_cast<const char*>(model.text()), model.textLength()),
            std::string("degra\xC2\xAD" "dation"));
}

TEST(PdfContentInterpreterTest, RetainsActualTextSoftHyphenAfterRunWorkspaceFills) {
  DefaultFont defaultFont;
  TestResourceTable resources;
  resources.font = &defaultFont.font;
  PdfTestByteSource page(bytes(
      "BT /F1 10 Tf 1 0 0 1 10 20 Tm (degra) Tj "
      "/Span << /ActualText <FEFF00AD> >> BDC (-) Tj EMC "
      "1 0 0 1 10 10 Tm (dation) Tj ET"));
  const PdfByteSource source = page.source();
  InterpreterHarness harness({}, {}, 1U);
  ASSERT_TRUE(harness.interpreter.begin(&source, 1, resources.descriptor, harness.model).ok());

  const PdfStepResult result = runInterpreter(harness.interpreter);

  ASSERT_TRUE(result.complete()) << static_cast<int>(result.status.error);
  ASSERT_EQ(harness.model.runCount(), 1U);
  EXPECT_EQ(transcript(harness.model), std::string("degra\xC2\xAD" "dation"));
}

TEST(PdfContentInterpreterTest, DoesNotInventSpacesWhenTheMaterializedFontContainsRealSpaces) {
  std::array<PdfDecodedGlyph, 1> glyphs{};
  PdfFontMap font({nullptr, 0, {}, nullptr, nullptr, glyphs.data(), static_cast<uint16_t>(glyphs.size())});
  ASSERT_TRUE(font.beginMaterialized(1, false).ok());
  PdfDecodedGlyph space{};
  space.sourceCode = ' ';
  space.sourceLength = 1;
  space.unicode.bytes[0] = ' ';
  space.unicode.length = 1;
  space.width = 250;
  ASSERT_TRUE(font.addMaterializedGlyph(space).ok());
  ASSERT_TRUE(font.hasExplicitWhitespace());

  TestResourceTable resources;
  resources.font = &font;
  PdfTestByteSource page(bytes("BT /F1 10 Tf 1 0 0 1 10 20 Tm (P) Tj 7 0 Td (enguin supports) Tj ET"));
  const PdfByteSource source = page.source();
  InterpreterHarness harness;
  ASSERT_TRUE(harness.interpreter.begin(&source, 1, resources.descriptor, harness.model).ok());

  const PdfStepResult result = runInterpreter(harness.interpreter);

  ASSERT_TRUE(result.complete()) << static_cast<int>(result.status.error);
  ASSERT_EQ(harness.model.runCount(), 1U);
  EXPECT_EQ(transcript(harness.model), "Penguin supports");
}

TEST(PdfContentInterpreterTest, UsesOnlyLargePositionalGapsAsMissingWordSpaces) {
  DefaultFont defaultFont;
  TestResourceTable resources;
  resources.font = &defaultFont.font;
  PdfTestByteSource page(bytes("BT /F1 25 Tf "
                               "1 0 0 1 10 20 Tm (A) Tj "
                               "1 0 0 1 27 20 Tm (cknow) Tj "
                               "1 0 0 1 110 20 Tm (Notes) Tj ET"));
  const PdfByteSource source = page.source();
  InterpreterHarness harness;
  ASSERT_TRUE(harness.interpreter.begin(&source, 1, resources.descriptor, harness.model).ok());

  const PdfStepResult result = runInterpreter(harness.interpreter);

  ASSERT_TRUE(result.complete()) << static_cast<int>(result.status.error);
  ASSERT_EQ(harness.model.runCount(), 1U);
  EXPECT_EQ(transcript(harness.model), "Acknow Notes");
}

TEST(PdfContentInterpreterTest, ResolvesFormAndActualTextBeforeVisualGlyphs) {
  DefaultFont defaultFont;
  PdfTestByteSource page(
      bytes("q 1 0 0 1 0 0 cm /Fm1 Do Q "
            "BT /F1 10 Tf 1 0 0 1 72 680 Tm "
            "/Span << /ActualText (Accessible replacement) >> BDC "
            "(Visual) Tj ( glyphs) Tj EMC ET"));
  PdfTestByteSource form(bytes("BT /F1 18 Tf 20 40 Td [(Form) -120 (heading)] TJ ET"));
  TestResourceTable resources;
  resources.font = &defaultFont.font;
  resources.form.kind = PdfContentXObjectKind::Form;
  resources.form.reference = {6, 0};
  resources.form.content = form.source();
  resources.form.resources = &resources.descriptor;
  resources.form.bbox = {0, 0, PdfFixed16::fromInteger(612).raw, PdfFixed16::fromInteger(792).raw};
  resources.form.hasBBox = true;
  const PdfByteSource source = page.source();
  InterpreterHarness harness;
  ASSERT_TRUE(harness.interpreter.begin(&source, 1, resources.descriptor, harness.model).ok());
  const PdfStepResult result = runInterpreter(harness.interpreter);
  ASSERT_TRUE(result.complete()) << static_cast<int>(result.status.error);
  EXPECT_EQ(transcript(harness.model), "Form heading Accessible replacement");
  ASSERT_EQ(harness.model.runCount(), 3u);
  EXPECT_NE(harness.model.runs()[2].flags & PdfTextActualText, 0u);
  EXPECT_EQ(harness.model.runs()[2].xMin, PdfFixed16::fromInteger(72).raw);
  EXPECT_EQ(harness.model.runs()[2].xMax, PdfFixed16::fromInteger(137).raw);
  EXPECT_EQ(harness.model.runs()[2].yMin, PdfFixed16::fromInteger(680).raw);
  EXPECT_EQ(harness.model.runs()[2].yMax, PdfFixed16::fromInteger(690).raw);
  EXPECT_EQ(harness.model.runs()[2].baselineDx, PdfFixed16::fromInteger(65).raw);
  EXPECT_EQ(harness.model.runs()[2].baselineDy, 0);
  EXPECT_EQ(harness.interpreter.maximumFormDepth(), 1u);
}

TEST(PdfContentInterpreterTest, BoundsRunsFromVisualAdvanceAndTransformedAscent) {
  DefaultFont defaultFont;
  TestResourceTable resources;
  resources.font = &defaultFont.font;
  PdfTestByteSource page(bytes("BT /F1 10 Tf 0 1 -1 0 100 100 Tm (AB) Tj ET"));
  const PdfByteSource source = page.source();
  InterpreterHarness harness;
  ASSERT_TRUE(harness.interpreter.begin(&source, 1, resources.descriptor, harness.model).ok());
  const PdfStepResult result = runInterpreter(harness.interpreter);
  ASSERT_TRUE(result.complete()) << static_cast<int>(result.status.error);
  ASSERT_EQ(harness.model.runCount(), 1u);
  EXPECT_EQ(harness.model.runs()[0].xMin, PdfFixed16::fromInteger(90).raw);
  EXPECT_EQ(harness.model.runs()[0].xMax, PdfFixed16::fromInteger(100).raw);
  EXPECT_EQ(harness.model.runs()[0].yMin, PdfFixed16::fromInteger(100).raw);
  EXPECT_EQ(harness.model.runs()[0].yMax, PdfFixed16::fromInteger(110).raw);
  EXPECT_EQ(harness.model.runs()[0].baselineDx, 0);
  EXPECT_EQ(harness.model.runs()[0].baselineDy, PdfFixed16::fromInteger(10).raw);
}

TEST(PdfContentInterpreterTest, RecordsImagePlacementsSkipsInlineDataAndKeepsVectorCaption) {
  DefaultFont defaultFont;
  PdfTestByteSource page(
      bytes("BT /F1 9 Tf 72 700 Td (Before vector.) Tj ET "
            "q 100 -50 70 80 10 20 cm /Im1 Do Q "
            "0 0 0 RG 2 w 72 500 m 240 620 l 410 500 l h S "
            "BI /W 2 /H 1 /CS /G /BPC 8 ID abc EIx def EI\n"
            "BT /F1 11 Tf 72 470 Td (Figure one: bounded vector caption.) Tj ET"));
  TestResourceTable resources;
  resources.font = &defaultFont.font;
  resources.inlineImageEncodedLength = 11;
  resources.image.kind = PdfContentXObjectKind::Image;
  resources.image.reference = {9, 0};
  resources.image.pixelWidth = 4;
  resources.image.pixelHeight = 4;
  const PdfByteSource source = page.source();
  InterpreterHarness harness;
  ASSERT_TRUE(harness.interpreter.begin(&source, 1, resources.descriptor, harness.model).ok());
  const PdfStepResult result = runInterpreter(harness.interpreter);
  ASSERT_TRUE(result.complete()) << static_cast<int>(result.status.error);
  EXPECT_EQ(transcript(harness.model), "Before vector. Figure one: bounded vector caption.");
  ASSERT_EQ(harness.model.imageCount(), 2u);
  EXPECT_EQ(harness.model.images()[0].reference.objectNumber, 9u);
  EXPECT_EQ(harness.model.images()[0].xMin, PdfFixed16::fromInteger(10).raw);
  EXPECT_EQ(harness.model.images()[0].xMax, PdfFixed16::fromInteger(180).raw);
  EXPECT_EQ(harness.model.images()[0].yMin, PdfFixed16::fromInteger(-30).raw);
  EXPECT_EQ(harness.model.images()[0].yMax, PdfFixed16::fromInteger(100).raw);
  EXPECT_EQ(harness.model.images()[1].flags & PdfImageInline, PdfImageInline);
  EXPECT_EQ(harness.model.images()[1].pixelWidth, 2u);
  EXPECT_EQ(harness.model.images()[1].pixelHeight, 1u);
  EXPECT_NE(static_cast<uint16_t>(harness.model.warnings()) & static_cast<uint16_t>(PdfPageWarning::VectorArtOmitted),
            0u);
}

TEST(PdfContentInterpreterTest, UsesExactInlineImageBoundaryWhenBinaryContainsEiAndTextOperators) {
  DefaultFont defaultFont;
  TestResourceTable resources;
  resources.font = &defaultFont.font;
  resources.inlineImageEncodedLength = 16;
  PdfTestByteSource page(
      bytes("BT /F1 10 Tf 1 0 0 1 10 10 Tm "
            "BI /W 16 /H 1 /CS /G /BPC 8 ID AA EI (LEAK) Tj  EI "
            "(Visible) Tj ET"));
  const PdfByteSource source = page.source();
  InterpreterHarness harness;
  ASSERT_TRUE(harness.interpreter.begin(&source, 1, resources.descriptor, harness.model).ok());

  const PdfStepResult result = runInterpreter(harness.interpreter);

  ASSERT_TRUE(result.complete()) << static_cast<int>(result.status.error) << "@" << result.status.offset;
  EXPECT_EQ(transcript(harness.model), "Visible");
  ASSERT_EQ(harness.model.imageCount(), 1U);
  EXPECT_EQ(harness.model.images()[0].flags & PdfImageInline, PdfImageInline);
}

TEST(PdfContentInterpreterTest, EmptyStringsAndEmptyActualTextAreNoOps) {
  DefaultFont defaultFont;
  TestResourceTable resources;
  resources.font = &defaultFont.font;
  PdfTestByteSource page(
      bytes("BT /F1 10 Tf 72 680 Td () Tj "
            "/Span << /ActualText () >> BDC (Suppressed) Tj EMC "
            "(Readable) Tj ET"));
  const PdfByteSource source = page.source();
  InterpreterHarness harness;
  ASSERT_TRUE(harness.interpreter.begin(&source, 1, resources.descriptor, harness.model).ok());
  const PdfStepResult result = runInterpreter(harness.interpreter);
  ASSERT_TRUE(result.complete()) << static_cast<int>(result.status.error);
  EXPECT_EQ(transcript(harness.model), "Readable");
}

TEST(PdfContentInterpreterTest, OmitsClipOnlyOffPageAndCollapsedTextIncludingActualText) {
  DefaultFont defaultFont;
  TestResourceTable resources;
  resources.font = &defaultFont.font;
  PdfTestByteSource page(
      bytes("BT /F1 10 Tf "
            "1 0 0 1 72 700 Tm 7 Tr (UNPAINTED) Tj "
            "/Span << /ActualText (ACTUAL LEAK) >> BDC (Visual leak) Tj EMC "
            "0 Tr 1 0 0 1 1000 700 Tm (OFF PAGE) Tj "
            "1 0 0 1 72 650 Tm 0 Tz (COLLAPSED) Tj "
            "100 Tz 1 0 0 1 72 620 Tm (Visible) Tj ET"));
  const PdfByteSource source = page.source();
  const PdfRectangle pageBounds{0, 0, PdfFixed16::fromInteger(612).raw, PdfFixed16::fromInteger(792).raw};
  InterpreterHarness harness(pageBounds);
  ASSERT_TRUE(harness.interpreter.begin(&source, 1, resources.descriptor, harness.model).ok());

  const PdfStepResult result = runInterpreter(harness.interpreter);

  ASSERT_TRUE(result.complete()) << static_cast<int>(result.status.error) << "@" << result.status.offset;
  EXPECT_EQ(transcript(harness.model), "Visible");
}

TEST(PdfContentInterpreterTest, AppliesTextRenderModeClippingAtEndOfTextObject) {
  DefaultFont defaultFont;
  TestResourceTable resources;
  resources.font = &defaultFont.font;
  PdfTestByteSource page(
      bytes("q BT /F1 10 Tf 1 0 0 1 10 10 Tm 4 Tr (Painted four) Tj ET "
            "BT /F1 10 Tf 1 0 0 1 10 30 Tm 0 Tr (LEAK FOUR) Tj ET Q "
            "q BT /F1 10 Tf 1 0 0 1 10 50 Tm 5 Tr (Painted five) Tj ET "
            "BT /F1 10 Tf 1 0 0 1 10 70 Tm 0 Tr (LEAK FIVE) Tj ET Q "
            "q BT /F1 10 Tf 1 0 0 1 10 90 Tm 6 Tr (Painted six) Tj ET "
            "BT /F1 10 Tf 1 0 0 1 10 110 Tm 0 Tr (LEAK SIX) Tj ET Q "
            "q BT /F1 10 Tf 1 0 0 1 10 130 Tm 7 Tr (UNPAINTED SEVEN) Tj ET "
            "BT /F1 10 Tf 1 0 0 1 10 150 Tm 0 Tr (LEAK SEVEN) Tj ET Q "
            "BT /F1 10 Tf 1 0 0 1 10 180 Tm (Visible) Tj ET"));
  const PdfByteSource source = page.source();
  const PdfRectangle pageBounds{0, 0, PdfFixed16::fromInteger(200).raw, PdfFixed16::fromInteger(200).raw};
  InterpreterHarness harness(pageBounds);
  ASSERT_TRUE(harness.interpreter.begin(&source, 1, resources.descriptor, harness.model).ok());

  const PdfStepResult result = runInterpreter(harness.interpreter);

  ASSERT_TRUE(result.complete()) << static_cast<int>(result.status.error) << "@" << result.status.offset;
  EXPECT_EQ(transcript(harness.model), "Painted four Painted five Painted six Visible");
}

TEST(PdfContentInterpreterTest, GraphicsRestoreDoesNotRewindTextPositionInsideTextObject) {
  DefaultFont defaultFont;
  TestResourceTable resources;
  resources.font = &defaultFont.font;
  PdfTestByteSource page(bytes("BT /F1 10 Tf 1 0 0 1 10 20 Tm (A) Tj q 20 Tc (B) Tj Q (C) Tj ET"));
  const PdfByteSource source = page.source();
  InterpreterHarness harness;
  ASSERT_TRUE(harness.interpreter.begin(&source, 1, resources.descriptor, harness.model).ok());

  const PdfStepResult result = runInterpreter(harness.interpreter);

  ASSERT_TRUE(result.complete()) << static_cast<int>(result.status.error) << "@" << result.status.offset;
  ASSERT_EQ(harness.model.runCount(), 3U);
  EXPECT_EQ(transcript(harness.model), "A B C");
  EXPECT_EQ(harness.model.runs()[2].xMin, harness.model.runs()[1].xMax);
}

TEST(PdfContentInterpreterTest, GraphicsRestoreKeepsPendingTextClipUntilEndOfTextObject) {
  DefaultFont defaultFont;
  TestResourceTable resources;
  resources.font = &defaultFont.font;
  PdfTestByteSource page(
      bytes("BT /F1 10 Tf 1 0 0 1 10 20 Tm q 7 Tr (UNPAINTED) Tj Q (Painted) Tj ET "
            "BT /F1 10 Tf 1 0 0 1 10 40 Tm (LEAK) Tj ET"));
  const PdfByteSource source = page.source();
  InterpreterHarness harness;
  ASSERT_TRUE(harness.interpreter.begin(&source, 1, resources.descriptor, harness.model).ok());

  const PdfStepResult result = runInterpreter(harness.interpreter);

  ASSERT_TRUE(result.complete()) << static_cast<int>(result.status.error) << "@" << result.status.offset;
  EXPECT_EQ(transcript(harness.model), "Painted");
}

TEST(PdfContentInterpreterTest, SuppressesArtifactsOptionalContentAndUnknownGraphicsVisibility) {
  DefaultFont defaultFont;
  TestResourceTable resources;
  resources.font = &defaultFont.font;
  resources.image.kind = PdfContentXObjectKind::Image;
  resources.image.reference = {9, 0};
  resources.image.pixelWidth = 20;
  resources.image.pixelHeight = 10;
  PdfTestByteSource page(
      bytes("/Artifact BMC "
            "BT /F1 10 Tf 1 0 0 1 10 10 Tm (HEADER) Tj /Span BMC (NESTED HEADER) Tj EMC ET /Im1 Do EMC "
            "/Artifact << /Type /Pagination >> BDC "
            "BT /F1 10 Tf 1 0 0 1 10 30 Tm (FOOTER) Tj ET EMC "
            "/OC /Layer BDC "
            "BT /F1 10 Tf 1 0 0 1 10 50 Tm (HIDDEN LAYER) Tj ET /Im1 Do EMC "
            "BT /F1 10 Tf 1 0 0 1 10 70 Tm (Body before) Tj "
            "q /GS0 gs (ALPHA HIDDEN) Tj Q (Body after) Tj ET /Im1 Do"));
  const PdfByteSource source = page.source();
  InterpreterHarness harness;
  ASSERT_TRUE(harness.interpreter.begin(&source, 1, resources.descriptor, harness.model).ok());

  const PdfStepResult result = runInterpreter(harness.interpreter);

  ASSERT_TRUE(result.complete()) << static_cast<int>(result.status.error) << "@" << result.status.offset;
  EXPECT_EQ(transcript(harness.model), "Body before Body after");
  ASSERT_EQ(harness.model.runCount(), 2U);
  EXPECT_GT(harness.model.runs()[1].xMin, harness.model.runs()[0].xMax);
  EXPECT_EQ(harness.model.imageCount(), 1U);
}

TEST(PdfContentInterpreterTest, RejectsNestedAndUnbalancedTextObjectsBeforeClipStateCanBeCleared) {
  DefaultFont defaultFont;
  TestResourceTable resources;
  resources.font = &defaultFont.font;
  PdfTestByteSource nested(
      bytes("BT /F1 10 Tf 1 0 0 1 10 10 Tm 7 Tr (clip glyphs) Tj "
            "BT ET BT /F1 10 Tf 1 0 0 1 10 30 Tm (LEAK) Tj ET"));
  const PdfByteSource nestedSource = nested.source();
  InterpreterHarness nestedHarness;
  ASSERT_TRUE(nestedHarness.interpreter.begin(&nestedSource, 1, resources.descriptor, nestedHarness.model).ok());
  const PdfStepResult nestedResult = runInterpreter(nestedHarness.interpreter);
  ASSERT_TRUE(nestedResult.failed());
  EXPECT_EQ(nestedResult.status.error, PdfError::Malformed);

  PdfTestByteSource unbalanced(bytes("ET"));
  const PdfByteSource unbalancedSource = unbalanced.source();
  InterpreterHarness unbalancedHarness;
  ASSERT_TRUE(
      unbalancedHarness.interpreter.begin(&unbalancedSource, 1, resources.descriptor, unbalancedHarness.model).ok());
  const PdfStepResult unbalancedResult = runInterpreter(unbalancedHarness.interpreter);
  ASSERT_TRUE(unbalancedResult.failed());
  EXPECT_EQ(unbalancedResult.status.error, PdfError::Malformed);
}

TEST(PdfContentInterpreterTest, AppliesRectangularClipAndFailsClosedForUnrepresentableClip) {
  DefaultFont defaultFont;
  TestResourceTable resources;
  resources.font = &defaultFont.font;
  PdfTestByteSource page(
      bytes("q 0 0 50 50 re W n "
            "BT /F1 10 Tf 1 0 0 1 10 10 Tm (Inside) Tj 1 0 0 1 100 100 Tm (CLIPPED) Tj ET Q "
            "q 0 0 m 50 0 l 50 50 l h W n "
            "BT /F1 10 Tf 1 0 0 1 10 10 Tm (UNKNOWN CLIP) Tj ET Q "
            "BT /F1 10 Tf 1 0 0 1 10 80 Tm (Visible after Q) Tj ET"));
  const PdfByteSource source = page.source();
  const PdfRectangle pageBounds{0, 0, PdfFixed16::fromInteger(200).raw, PdfFixed16::fromInteger(200).raw};
  InterpreterHarness harness(pageBounds);
  ASSERT_TRUE(harness.interpreter.begin(&source, 1, resources.descriptor, harness.model).ok());

  const PdfStepResult result = runInterpreter(harness.interpreter);

  ASSERT_TRUE(result.complete()) << static_cast<int>(result.status.error) << "@" << result.status.offset;
  EXPECT_EQ(transcript(harness.model), "Inside Visible after Q");
}

TEST(PdfContentInterpreterTest, FailsClosedForSkewedRectangleClipAndMissingOrSkewedFormBbox) {
  DefaultFont defaultFont;
  PdfTestByteSource page(
      bytes("q 1 0.5 0 1 0 0 cm 0 0 50 50 re W n "
            "BT /F1 10 Tf 1 0 0 1 10 10 Tm (SKEWED CLIP LEAK) Tj ET Q "
            "/Fm1 Do /Fm2 Do "
            "BT /F1 10 Tf 1 0 0 1 10 80 Tm (Visible) Tj ET"));
  PdfTestByteSource missingBboxForm(bytes("BT /F1 10 Tf 1 0 0 1 10 10 Tm (MISSING BBOX LEAK) Tj ET"));
  PdfTestByteSource skewedBboxForm(bytes("BT /F1 10 Tf 1 0 0 1 10 10 Tm (SKEWED BBOX LEAK) Tj ET"));
  TestResourceTable resources;
  resources.font = &defaultFont.font;
  resources.form.kind = PdfContentXObjectKind::Form;
  resources.form.reference = {6, 0};
  resources.form.content = missingBboxForm.source();
  resources.form.resources = &resources.descriptor;
  resources.secondaryForm.kind = PdfContentXObjectKind::Form;
  resources.secondaryForm.reference = {7, 0};
  resources.secondaryForm.content = skewedBboxForm.source();
  resources.secondaryForm.resources = &resources.descriptor;
  resources.secondaryForm.bbox = {0, 0, PdfFixed16::fromInteger(50).raw, PdfFixed16::fromInteger(50).raw};
  resources.secondaryForm.hasBBox = true;
  resources.secondaryForm.matrix = {PdfFixed16::fromInteger(1), {32768}, {}, PdfFixed16::fromInteger(1), {}, {}};
  const PdfByteSource source = page.source();
  const PdfRectangle pageBounds{0, 0, PdfFixed16::fromInteger(200).raw, PdfFixed16::fromInteger(200).raw};
  InterpreterHarness harness(pageBounds);
  ASSERT_TRUE(harness.interpreter.begin(&source, 1, resources.descriptor, harness.model).ok());

  const PdfStepResult result = runInterpreter(harness.interpreter);

  ASSERT_TRUE(result.complete()) << static_cast<int>(result.status.error) << "@" << result.status.offset;
  EXPECT_EQ(transcript(harness.model), "Visible");
}

TEST(PdfContentInterpreterTest, AppliesFormBboxClipAndPageTransformToTextAndImages) {
  DefaultFont defaultFont;
  PdfTestByteSource page(bytes("q 100 0 0 50 10 20 cm /Im1 Do Q /Fm1 Do"));
  PdfTestByteSource form(bytes("BT /F1 10 Tf 1 0 0 1 20 30 Tm (Inside form) Tj "
                               "1 0 0 1 150 80 Tm (OUTSIDE BBOX) Tj ET"));
  TestResourceTable resources;
  resources.font = &defaultFont.font;
  resources.image.kind = PdfContentXObjectKind::Image;
  resources.image.reference = {9, 0};
  resources.image.pixelWidth = 100;
  resources.image.pixelHeight = 50;
  resources.form.kind = PdfContentXObjectKind::Form;
  resources.form.reference = {6, 0};
  resources.form.content = form.source();
  resources.form.resources = &resources.descriptor;
  resources.form.bbox = {PdfFixed16::fromInteger(10).raw, PdfFixed16::fromInteger(20).raw,
                         PdfFixed16::fromInteger(110).raw, PdfFixed16::fromInteger(70).raw};
  resources.form.hasBBox = true;
  const PdfByteSource source = page.source();
  const PdfRectangle pageBounds{0, 0, PdfFixed16::fromInteger(100).raw, PdfFixed16::fromInteger(200).raw};
  const PdfMatrix pageTransform{{}, PdfFixed16::fromInteger(-1), PdfFixed16::fromInteger(1), {},
                                PdfFixed16::fromInteger(-20), PdfFixed16::fromInteger(210)};
  InterpreterHarness harness(pageBounds, pageTransform);
  ASSERT_TRUE(harness.interpreter.begin(&source, 1, resources.descriptor, harness.model).ok());

  const PdfStepResult result = runInterpreter(harness.interpreter);

  ASSERT_TRUE(result.complete()) << static_cast<int>(result.status.error) << "@" << result.status.offset;
  EXPECT_EQ(transcript(harness.model), "Inside form");
  ASSERT_EQ(harness.model.imageCount(), 1U);
  const PdfImagePlacement& image = harness.model.images()[0];
  EXPECT_EQ(image.xMin, PdfFixed16::fromInteger(0).raw);
  EXPECT_EQ(image.xMax, PdfFixed16::fromInteger(50).raw);
  EXPECT_EQ(image.yMin, PdfFixed16::fromInteger(100).raw);
  EXPECT_EQ(image.yMax, PdfFixed16::fromInteger(200).raw);
  ASSERT_EQ(harness.model.runCount(), 1U);
  const PdfTextRun& caption = harness.model.runs()[0];
  EXPECT_GT(caption.xMax, image.xMin);
  EXPECT_LT(caption.xMin, image.xMax);
  EXPECT_GT(caption.yMax, image.yMin);
  EXPECT_LT(caption.yMin, image.yMax);
}

TEST(PdfContentInterpreterTest, ReplacesUnmappedCidAndHonorsActualText) {
  std::array<PdfFontWidthRecord, 2> widths{};
  PdfFontMap cidFont({widths.data(), static_cast<uint16_t>(widths.size())});
  ASSERT_TRUE(cidFont.begin(2, true, nullptr, nullptr, 1000).ok());
  TestResourceTable resources;
  resources.font = &cidFont;

  PdfTestByteSource unsupported(bytes("BT /F1 12 Tf 72 700 Td <0041> Tj ET"));
  const PdfByteSource unsupportedSource = unsupported.source();
  InterpreterHarness unsupportedHarness;
  ASSERT_TRUE(
      unsupportedHarness.interpreter.begin(&unsupportedSource, 1, resources.descriptor, unsupportedHarness.model).ok());
  const PdfStepResult unsupportedResult = runInterpreter(unsupportedHarness.interpreter);
  ASSERT_TRUE(unsupportedResult.complete()) << static_cast<int>(unsupportedResult.status.error);
  EXPECT_EQ(transcript(unsupportedHarness.model), "\xEF\xBF\xBD");

  PdfTestByteSource accessible(
      bytes("BT /F1 12 Tf 72 700 Td /Span << /ActualText (Readable) >> BDC "
            "<0041> Tj EMC ET"));
  const PdfByteSource accessibleSource = accessible.source();
  InterpreterHarness accessibleHarness;
  ASSERT_TRUE(
      accessibleHarness.interpreter.begin(&accessibleSource, 1, resources.descriptor, accessibleHarness.model).ok());
  const PdfStepResult accessibleResult = runInterpreter(accessibleHarness.interpreter);
  ASSERT_TRUE(accessibleResult.complete()) << static_cast<int>(accessibleResult.status.error);
  EXPECT_EQ(transcript(accessibleHarness.model), "Readable");
}

TEST(PdfContentInterpreterTest, RejectsGraphicsOverflowButOmitsRecursiveForms) {
  DefaultFont defaultFont;
  TestResourceTable resources;
  resources.font = &defaultFont.font;
  PdfTestByteSource graphics(
      bytes("q q q q q q q q q q q q q q q q q "
            "BT /F1 12 Tf (unreachable) Tj ET"));
  const PdfByteSource graphicsSource = graphics.source();
  InterpreterHarness graphicsHarness;
  ASSERT_TRUE(graphicsHarness.interpreter.begin(&graphicsSource, 1, resources.descriptor, graphicsHarness.model).ok());
  const PdfStepResult graphicsResult = runInterpreter(graphicsHarness.interpreter);
  ASSERT_TRUE(graphicsResult.failed());
  EXPECT_EQ(graphicsResult.status.error, PdfError::LimitExceeded);

  PdfTestByteSource page(bytes("/Loop Do BT /F1 12 Tf (after) Tj ET"));
  PdfTestByteSource loop(bytes("/Loop Do"));
  resources.loop.kind = PdfContentXObjectKind::Form;
  resources.loop.reference = {99, 0};
  resources.loop.content = loop.source();
  resources.loop.resources = &resources.descriptor;
  resources.loop.bbox = {0, 0, PdfFixed16::fromInteger(612).raw, PdfFixed16::fromInteger(792).raw};
  resources.loop.hasBBox = true;
  const PdfByteSource pageSource = page.source();
  InterpreterHarness cycleHarness;
  ASSERT_TRUE(cycleHarness.interpreter.begin(&pageSource, 1, resources.descriptor, cycleHarness.model).ok());
  const PdfStepResult cycleResult = runInterpreter(cycleHarness.interpreter);
  ASSERT_TRUE(cycleResult.complete()) << static_cast<int>(cycleResult.status.error);
  EXPECT_EQ(transcript(cycleHarness.model), "after");
  EXPECT_NE(static_cast<uint16_t>(cycleHarness.model.warnings()) &
                static_cast<uint16_t>(PdfPageWarning::VectorArtOmitted),
            0U);
}

TEST(PdfContentInterpreterTest, OperatorBudgetEndsOnlyTheCurrentPage) {
  DefaultFont defaultFont;
  TestResourceTable resources;
  resources.font = &defaultFont.font;
  PdfTestByteSource exhaustedPage(bytes("BT /F1 12 Tf (omitted) Tj ET"));
  const PdfByteSource exhaustedSource = exhaustedPage.source();
  InterpreterHarness exhaustedHarness;
  exhaustedHarness.documentOperatorCount = PdfLimits::MaxOperatorsPerDocument;
  ASSERT_TRUE(
      exhaustedHarness.interpreter.begin(&exhaustedSource, 1, resources.descriptor, exhaustedHarness.model).ok());

  const PdfStepResult exhaustedResult = runInterpreter(exhaustedHarness.interpreter);

  ASSERT_TRUE(exhaustedResult.complete()) << static_cast<int>(exhaustedResult.status.error);
  EXPECT_EQ(transcript(exhaustedHarness.model), "");
  EXPECT_EQ(exhaustedHarness.documentOperatorCount, 0U);
  EXPECT_NE(static_cast<uint16_t>(exhaustedHarness.model.warnings()) &
                static_cast<uint16_t>(PdfPageWarning::VectorArtOmitted),
            0U);

  PdfTestByteSource nextPage(bytes("BT /F1 12 Tf (after budget) Tj ET"));
  const PdfByteSource nextSource = nextPage.source();
  InterpreterHarness nextHarness;
  ASSERT_TRUE(nextHarness.interpreter.begin(&nextSource, 1, resources.descriptor, nextHarness.model).ok());
  const PdfStepResult nextResult = runInterpreter(nextHarness.interpreter);
  ASSERT_TRUE(nextResult.complete()) << static_cast<int>(nextResult.status.error);
  EXPECT_EQ(transcript(nextHarness.model), "after budget");
}

TEST(PdfContentInterpreterTest, PdfFontSizesSixAndSeventyTwoProduceIdenticalSemanticText) {
  const FixtureTextResult small = interpretFontFixture("font_size_6.pdf");
  const FixtureTextResult large = interpretFontFixture("font_size_72.pdf");
  ASSERT_TRUE(small.status.ok()) << static_cast<int>(small.status.error);
  ASSERT_TRUE(large.status.ok()) << static_cast<int>(large.status.error);
  EXPECT_EQ(small.text, "Typography uses device defaults.");
  EXPECT_EQ(large.text, small.text);

  const FixtureSemanticResult smallSemantic = writeFixtureSemantic(small);
  const FixtureSemanticResult largeSemantic = writeFixtureSemantic(large);
  ASSERT_TRUE(smallSemantic.status.ok()) << static_cast<int>(smallSemantic.status.error);
  ASSERT_TRUE(largeSemantic.status.ok()) << static_cast<int>(largeSemantic.status.error);
  EXPECT_EQ(largeSemantic.xhtml, smallSemantic.xhtml);
  EXPECT_EQ(smallSemantic.xhtml.find("font-size"), std::string::npos);
  EXPECT_EQ(smallSemantic.xhtml.find("font-family"), std::string::npos);
  ASSERT_EQ(smallSemantic.blocks.size(), 1u);
  ASSERT_EQ(largeSemantic.blocks.size(), 1u);
  EXPECT_STREQ(smallSemantic.blocks[0].anchor, "b00000000");
  EXPECT_STREQ(largeSemantic.blocks[0].anchor, smallSemantic.blocks[0].anchor);
  EXPECT_EQ(largeSemantic.blocks[0].anchorOrdinal, smallSemantic.blocks[0].anchorOrdinal);
  EXPECT_EQ(largeSemantic.blocks[0].cumulativeWordStart, smallSemantic.blocks[0].cumulativeWordStart);
  EXPECT_EQ(largeSemantic.blocks[0].wordCount, smallSemantic.blocks[0].wordCount);
  EXPECT_EQ(smallSemantic.totalWords, 4u);
  EXPECT_EQ(largeSemantic.totalWords, smallSemantic.totalWords);
}

TEST(PdfContentInterpreterTest, ExistingVectorFixtureRetainsItsCaption) {
  const FixtureTextResult fixture = interpretFontFixture("vector_caption.pdf");
  ASSERT_TRUE(fixture.status.ok()) << static_cast<int>(fixture.status.error);
  EXPECT_EQ(fixture.text, "Figure one: bounded vector caption.");
}

TEST(PdfContentInterpreterTest, GeneratedHiddenOcrFixturesQualifyAndDeduplicate) {
  const PdfRectangle page{PdfFixed16::fromInteger(0).raw, PdfFixed16::fromInteger(0).raw,
                          PdfFixed16::fromInteger(612).raw, PdfFixed16::fromInteger(792).raw};
  const FixtureTextResult hidden = interpretFontFixture("hidden_ocr.pdf");
  ASSERT_TRUE(hidden.status.ok()) << static_cast<int>(hidden.status.error);
  ASSERT_EQ(hidden.runs.size(), 1u);
  ASSERT_EQ(hidden.images.size(), 1u);
  ASSERT_NE(hidden.runs[0].flags & PdfTextHidden, 0u);
  const PdfHiddenTextContext hiddenContext{
      page,
      hidden.runs.data(),
      static_cast<uint16_t>(hidden.runs.size()),
      hidden.pageText.data(),
      hidden.pageText.size(),
      hidden.images.data(),
      static_cast<uint16_t>(hidden.images.size()),
  };
  EXPECT_EQ(pdfClassifyHiddenText(hiddenContext, 0), PdfHiddenTextDecision::Qualified);

  const FixtureTextResult duplicate = interpretFontFixture("hidden_ocr_visible_duplicate.pdf");
  ASSERT_TRUE(duplicate.status.ok()) << static_cast<int>(duplicate.status.error);
  ASSERT_EQ(duplicate.runs.size(), 2u);
  ASSERT_EQ(duplicate.images.size(), 1u);
  const uint16_t hiddenIndex = (duplicate.runs[0].flags & PdfTextHidden) != 0 ? 0 : 1;
  const PdfHiddenTextContext duplicateContext{
      page,
      duplicate.runs.data(),
      static_cast<uint16_t>(duplicate.runs.size()),
      duplicate.pageText.data(),
      duplicate.pageText.size(),
      duplicate.images.data(),
      static_cast<uint16_t>(duplicate.images.size()),
  };
  EXPECT_EQ(pdfClassifyHiddenText(duplicateContext, hiddenIndex), PdfHiddenTextDecision::DuplicateVisible);
}

TEST(PdfContentInterpreterTest, GeneratedScanOnlyFixtureWaitsForFullExtractionBeforeNoReadableText) {
  const FixtureTextResult scan = interpretFontFixture("scan_only.pdf");
  ASSERT_TRUE(scan.status.ok()) << static_cast<int>(scan.status.error);
  ASSERT_TRUE(scan.runs.empty());
  ASSERT_EQ(scan.images.size(), 1u);

  PdfDocumentTextClassifier classifier;
  ASSERT_TRUE(classifier.begin(1).ok());
  ASSERT_TRUE(classifier.observePage(0, {0, 0, static_cast<uint16_t>(scan.images.size())}).ok());
  EXPECT_EQ(classifier.sampledKind(), PdfDocumentTextKind::ImageOnlyCandidate);
  EXPECT_EQ(classifier.finish(PdfStatus::success()).error, PdfError::NoReadableText);
}
