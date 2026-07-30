#include <gtest/gtest.h>

#include <array>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "PdfContentInterpreter.h"
#include "PdfPageTree.h"
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

  InterpreterHarness()
      : model({pageText.data(), pageText.size(), runs.data(), static_cast<uint16_t>(runs.size()), images.data(),
               static_cast<uint16_t>(images.size())}),
        interpreter({sourceBuffer.data(), sourceBuffer.size(), operands.data(), static_cast<uint8_t>(operands.size()),
                     arrayItems.data(), static_cast<uint8_t>(arrayItems.size()), scratchText.data(),
                     static_cast<uint16_t>(scratchText.size()), markedText.data(),
                     static_cast<uint16_t>(markedText.size()), &documentOperatorCount}) {}
};

struct TestResourceTable {
  PdfFontMap* font = nullptr;
  PdfContentXObject image{};
  PdfContentXObject form{};
  PdfContentXObject loop{};
  PdfContentResources descriptor{};

  TestResourceTable() {
    descriptor.context = this;
    descriptor.resolveFont = resolveFont;
    descriptor.resolveXObject = resolveXObject;
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
    if (length == 4 && std::memcmp(name, "Loop", 4) == 0) {
      *object = table.loop;
      return PdfStatus::success();
    }
    return PdfStatus::failure(PdfError::Malformed);
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
  while (true) {
    PdfWorkBudget budget{1, 1};
    const PdfStepResult result = stepper.step(budget);
    if (!result.yielded()) {
      return result;
    }
  }
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

  static PdfStatus capturePage(void* context, const PdfPageInfo& page) {
    auto& workspace = *static_cast<FixtureWorkspace*>(context);
    if (workspace.pageCount == 0) {
      workspace.page = page;
    }
    ++workspace.pageCount;
    return PdfStatus::success();
  }
};

struct FixtureTextResult {
  PdfStatus status{};
  std::string text;
};

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
    return {result.status, {}};
  }
  PdfObjectReference catalog;
  if (!workspace.xref.root(&catalog)) {
    return {PdfStatus::failure(PdfError::Malformed), {}};
  }
  PdfObjectResolver resolver(source, workspace.xref, workspace.sourceBuffer.data(), workspace.sourceBuffer.size(),
                             workspace.arena);
  PdfStatus status = resolver.begin(catalog);
  if (!status.ok() || !(result = runBudgetOne(resolver)).complete()) {
    return {status.ok() ? result.status : status, {}};
  }
  uint16_t pagesIndex = PDF_INVALID_INDEX;
  if (!pdfDictionaryFind(workspace.arena, resolver.result().rootIndex, "Pages", &pagesIndex)) {
    return {PdfStatus::failure(PdfError::Malformed), {}};
  }
  const PdfValue pages = workspace.arena.values[pagesIndex];
  PdfPageTreeWalker walker(resolver, workspace.arena, workspace.traversalStorage.store(), FixtureWorkspace::capturePage,
                           &workspace);
  status = walker.begin({pages.objectNumber, pages.generation});
  if (!status.ok() || !(result = runBudgetOne(walker)).complete() || workspace.pageCount != 1 ||
      workspace.page.contentCount != 1) {
    return {status.ok() ? result.status : status, {}};
  }
  status = resolver.begin(workspace.page.contents[0]);
  if (!status.ok() || !(result = runBudgetOne(resolver)).complete() || !resolver.result().hasStream) {
    return {status.ok() ? result.status : status, {}};
  }
  PdfByteRange range;
  status = pdfInitializeByteRange(source, resolver.result().streamOffset, resolver.result().streamLength, &range);
  if (!status.ok()) {
    return {status, {}};
  }
  const PdfByteSource content = pdfByteRangeSource(range);
  DefaultFont defaultFont;
  TestResourceTable resources;
  resources.font = &defaultFont.font;
  InterpreterHarness harness;
  status = harness.interpreter.begin(&content, 1, resources.descriptor, harness.model);
  if (!status.ok() || !(result = runInterpreter(harness.interpreter)).complete()) {
    return {status.ok() ? result.status : status, {}};
  }
  return {PdfStatus::success(), transcript(harness.model)};
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

TEST(PdfContentInterpreterTest, ActualTextOverridesUnmappedCidButMeaningfulCidFailsClearly) {
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
  ASSERT_TRUE(unsupportedResult.failed());
  EXPECT_EQ(unsupportedResult.status.error, PdfError::UnsupportedEncoding);

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

TEST(PdfContentInterpreterTest, RejectsGraphicsOverflowAndFormCyclesAtBoundedDepth) {
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

  PdfTestByteSource page(bytes("/Loop Do"));
  PdfTestByteSource loop(bytes("/Loop Do"));
  resources.loop.kind = PdfContentXObjectKind::Form;
  resources.loop.reference = {99, 0};
  resources.loop.content = loop.source();
  resources.loop.resources = &resources.descriptor;
  const PdfByteSource pageSource = page.source();
  InterpreterHarness cycleHarness;
  ASSERT_TRUE(cycleHarness.interpreter.begin(&pageSource, 1, resources.descriptor, cycleHarness.model).ok());
  const PdfStepResult cycleResult = runInterpreter(cycleHarness.interpreter);
  ASSERT_TRUE(cycleResult.failed());
  EXPECT_EQ(cycleResult.status.error, PdfError::Malformed);
}

TEST(PdfContentInterpreterTest, PdfFontSizesSixAndSeventyTwoProduceIdenticalSemanticText) {
  const FixtureTextResult small = interpretFontFixture("font_size_6.pdf");
  const FixtureTextResult large = interpretFontFixture("font_size_72.pdf");
  ASSERT_TRUE(small.status.ok()) << static_cast<int>(small.status.error);
  ASSERT_TRUE(large.status.ok()) << static_cast<int>(large.status.error);
  EXPECT_EQ(small.text, "Typography uses device defaults.");
  EXPECT_EQ(large.text, small.text);
}

TEST(PdfContentInterpreterTest, ExistingVectorFixtureRetainsItsCaption) {
  const FixtureTextResult fixture = interpretFontFixture("vector_caption.pdf");
  ASSERT_TRUE(fixture.status.ok()) << static_cast<int>(fixture.status.error);
  EXPECT_EQ(fixture.text, "Figure one: bounded vector caption.");
}
