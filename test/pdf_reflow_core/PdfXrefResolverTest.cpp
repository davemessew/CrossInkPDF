#include <gtest/gtest.h>

#include <array>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "PdfObjectResolver.h"
#include "PdfTestIo.h"
#include "PdfXref.h"

namespace {

std::vector<uint8_t> loadFixture(const char* name) {
  const std::filesystem::path path = std::filesystem::path(__FILE__).parent_path() / "fixtures" / name;
  std::ifstream input(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

struct XrefHarness {
  XrefHarness()
      : streamDecoder({decoderSourceBuffer.data(), decoderSourceBuffer.size(), decoderOutputBuffer.data(),
                       decoderOutputBuffer.size(), inflateDictionary.data(), inflateDictionary.size()}) {
    objectStreamStorage.forbidReadsWhile(&externalReaderActive);
  }

  static PdfStepResult setSourceAccess(void* context, const PdfObjectResolverReader reader, PdfWorkBudget& budget) {
    if (context == nullptr) {
      return PdfStepResult::failure(PdfStatus::failure(PdfError::InvalidArgument));
    }
    auto& harness = *static_cast<XrefHarness*>(context);
    if (budget.cancelRequested()) {
      return PdfStepResult::failure(PdfStatus::failure(PdfError::Cancelled));
    }
    if (!budget.consumeOperation()) {
      return PdfStepResult::paused();
    }
    ++harness.sourceAccessCalls;
    if (reader == PdfObjectResolverReader::ObjectStoreWriter) {
      ++harness.objectStoreWriterCalls;
      if (harness.objectStoreWriterYieldsRemaining != 0) {
        --harness.objectStoreWriterYieldsRemaining;
        return PdfStepResult::paused();
      }
      ++harness.objectStoreWriterSelections;
    }
    harness.reader = reader;
    harness.sourceActive =
        reader == PdfObjectResolverReader::Source || reader == PdfObjectResolverReader::ObjectStoreWriter;
    harness.xrefActive = reader == PdfObjectResolverReader::Xref;
    if (harness.xrefActive) {
      ++harness.xrefSelections;
    }
    harness.externalReaderActive = harness.sourceActive || harness.xrefActive;
    if (reader == PdfObjectResolverReader::ObjectStore) {
      ++harness.objectStoreSelections;
    }
    ++harness.sourceTransitions;
    return PdfStepResult::completed();
  }

  std::array<uint8_t, 4096> sourceBuffer{};
  std::array<uint8_t, 4096> decoderSourceBuffer{};
  std::array<uint8_t, 4096> decoderOutputBuffer{};
  std::array<uint8_t, 32768> inflateDictionary{};
  PdfStreamDecoder streamDecoder;
  std::array<PdfValue, 64> values{};
  std::array<PdfDictionaryEntry, 64> dictionaries{};
  std::array<PdfArrayItem, 64> arrays{};
  std::array<uint8_t, 1024> text{};
  PdfObjectArena arena{
      values.data(),       static_cast<uint16_t>(values.size()),
      dictionaries.data(), static_cast<uint16_t>(dictionaries.size()),
      arrays.data(),       static_cast<uint16_t>(arrays.size()),
      text.data(),         static_cast<uint16_t>(text.size()),
  };
  PdfTestRecordStore recordStorage{sizeof(PdfXrefEntry), 256};
  PdfXrefTable table{recordStorage.store()};
  PdfObjectResolverReader reader = PdfObjectResolverReader::Source;
  bool sourceActive = true;
  bool xrefActive = false;
  bool externalReaderActive = true;
  uint32_t sourceTransitions = 0;
  uint32_t sourceAccessCalls = 0;
  uint32_t xrefSelections = 0;
  uint32_t objectStoreWriterCalls = 0;
  uint32_t objectStoreWriterSelections = 0;
  uint32_t objectStoreSelections = 0;
  uint8_t objectStoreWriterYieldsRemaining = 0;
  PdfStreamDecodeLimits decodeLimits{};
  PdfTestByteStore objectStreamStorage{64 * 1024};

  PdfObjectResolverWorkspace resolverWorkspace() {
    return {
        &streamDecoder, objectStreamStorage.store(), this, setSourceAccess, decodeLimits,
    };
  }
};

PdfStepResult parseXref(const PdfByteSource& source, XrefHarness& harness, const bool budgetOne = false) {
  PdfXrefParser parserWithStreams(source, harness.sourceBuffer.data(), harness.sourceBuffer.size(), harness.arena,
                                  harness.table, &harness.streamDecoder);
  parserWithStreams.begin();
  const uint32_t maximumSteps = budgetOne ? 65536U : 4096U;
  for (uint32_t step = 0; step < maximumSteps; ++step) {
    PdfWorkBudget budget{budgetOne ? 1U : 32U, budgetOne ? sizeof(PdfXrefEntry) : 4096U};
    const PdfStepResult result = parserWithStreams.step(budget);
    if (!result.yielded()) {
      return result;
    }
  }
  return PdfStepResult::failure(PdfStatus::failure(PdfError::BudgetExhausted));
}

PdfStepResult parseXrefWithBuffer(const PdfByteSource& source, XrefHarness& harness, uint8_t* const buffer,
                                  const size_t bufferSize) {
  PdfXrefParser parser(source, buffer, bufferSize, harness.arena, harness.table, &harness.streamDecoder);
  parser.begin();
  for (uint32_t step = 0; step < 65536U; ++step) {
    PdfWorkBudget budget{1, sizeof(PdfXrefEntry)};
    const PdfStepResult result = parser.step(budget);
    if (!result.yielded()) {
      return result;
    }
  }
  return PdfStepResult::failure(PdfStatus::failure(PdfError::BudgetExhausted));
}

PdfStepResult resolveObject(const PdfByteSource& source, XrefHarness& harness, const PdfObjectReference reference,
                            PdfResolvedObject* resolved) {
  PdfObjectResolver resolver(source, harness.table, harness.sourceBuffer.data(), harness.sourceBuffer.size(),
                             harness.arena, harness.resolverWorkspace());
  const PdfStatus beginStatus = resolver.begin(reference);
  if (!beginStatus.ok()) {
    return PdfStepResult::failure(beginStatus);
  }
  for (uint16_t step = 0; step < 256U; ++step) {
    PdfWorkBudget budget{32, 4096};
    const PdfStepResult result = resolver.step(budget);
    if (!result.yielded()) {
      if (result.complete() && resolved != nullptr) {
        *resolved = resolver.result();
      }
      return result;
    }
  }
  return PdfStepResult::failure(PdfStatus::failure(PdfError::BudgetExhausted));
}

PdfStepResult resolveObject(PdfObjectResolver& resolver, const PdfObjectReference reference,
                            PdfResolvedObject* resolved) {
  const PdfStatus beginStatus = resolver.begin(reference);
  if (!beginStatus.ok()) {
    return PdfStepResult::failure(beginStatus);
  }
  for (uint16_t step = 0; step < 256U; ++step) {
    PdfWorkBudget budget{32, 4096};
    const PdfStepResult result = resolver.step(budget);
    if (!result.yielded()) {
      if (result.complete() && resolved != nullptr) {
        *resolved = resolver.result();
      }
      return result;
    }
  }
  return PdfStepResult::failure(PdfStatus::failure(PdfError::BudgetExhausted));
}

void appendBigEndian(std::string* const output, const uint64_t value, const uint8_t width) {
  for (uint8_t byte = width; byte-- > 0;) {
    output->push_back(static_cast<char>(value >> (byte * 8U)));
  }
}

void appendXrefStreamEntry(std::string* const output, const uint8_t type, const uint32_t fieldTwo,
                           const uint16_t fieldThree) {
  appendBigEndian(output, type, 1);
  appendBigEndian(output, fieldTwo, 4);
  appendBigEndian(output, fieldThree, 2);
}

void appendNineByteField(std::string* const output, const uint8_t leadingByte, const uint64_t value) {
  output->push_back(static_cast<char>(leadingByte));
  appendBigEndian(output, value, 8);
}

std::vector<uint8_t> wideXrefStreamPdf(const bool overflow) {
  std::string pdf = "%PDF-1.5\n";
  const uint32_t xrefOffset = static_cast<uint32_t>(pdf.size());
  pdf += "1 0 obj\n<< /Type /XRef /Size 2 /Root 1 0 R /W [9 9 9] /Index [1 1] /Length 27 >>\nstream\n";
  appendNineByteField(&pdf, overflow ? 1U : 0U, 1U);
  appendNineByteField(&pdf, 0U, xrefOffset);
  appendNineByteField(&pdf, 0U, 0U);
  pdf += "\nendstream\nendobj\nstartxref\n" + std::to_string(xrefOffset) + "\n%%EOF\n";
  return {pdf.begin(), pdf.end()};
}

std::vector<uint8_t> manyPrevRevisionsPdf(const uint32_t revisionCount) {
  std::string pdf = "%PDF-1.4\n1 0 obj\n<< /Type /Catalog >>\nendobj\n";
  const uint32_t rootOffset = 9U;
  uint64_t previousXref = 0;
  for (uint32_t revision = 0; revision <= revisionCount; ++revision) {
    const uint64_t xrefOffset = pdf.size();
    if (revision == 0) {
      pdf += "xref\n1 1\n";
      char entry[32]{};
      std::snprintf(entry, sizeof(entry), "%010u 00000 n \n", rootOffset);
      pdf += entry;
      pdf += "trailer\n<< /Size 2 /Root 1 0 R >>\n";
    } else {
      pdf += "xref\n0 1\n0000000000 65535 f \ntrailer\n<< /Size 2 /Root 1 0 R /Prev " +
             std::to_string(previousXref) + " >>\n";
    }
    pdf += "startxref\n" + std::to_string(xrefOffset) + "\n%%EOF\n";
    previousXref = xrefOffset;
  }
  return {pdf.begin(), pdf.end()};
}

std::vector<uint8_t> truncatedLargeClassicSubsectionPdf() {
  std::string pdf = "%PDF-1.4\n";
  const uint64_t xrefOffset = pdf.size();
  pdf += "xref\n0 262145\n0000000000 65535 f \ntrailer\n<< /Size 262145 /Root 1 0 R >>\nstartxref\n" +
         std::to_string(xrefOffset) + "\n%%EOF\n";
  return {pdf.begin(), pdf.end()};
}

uint32_t predictorAdler32(const std::vector<uint8_t>& bytes) {
  uint32_t a = 1;
  uint32_t b = 0;
  for (const uint8_t byte : bytes) {
    a = (a + byte) % 65521U;
    b = (b + a) % 65521U;
  }
  return (b << 16U) | a;
}

std::vector<uint8_t> predictorStoredZlib(const std::vector<uint8_t>& input) {
  std::vector<uint8_t> output{0x78, 0x01, 0x01, static_cast<uint8_t>(input.size()), 0x00,
                              static_cast<uint8_t>(~input.size()), 0xff};
  output.insert(output.end(), input.begin(), input.end());
  const uint32_t checksum = predictorAdler32(input);
  output.push_back(static_cast<uint8_t>(checksum >> 24U));
  output.push_back(static_cast<uint8_t>(checksum >> 16U));
  output.push_back(static_cast<uint8_t>(checksum >> 8U));
  output.push_back(static_cast<uint8_t>(checksum));
  return output;
}

std::vector<uint8_t> predictorTwelveXrefPdf() {
  std::string pdf = "%PDF-1.5\n";
  const uint32_t xrefOffset = static_cast<uint32_t>(pdf.size());
  const std::array<std::array<uint8_t, 5>, 2> rows{{
      {{1, 0, 0, static_cast<uint8_t>(xrefOffset), 0}},
      {{0, 0, 0, 0, 0}},
  }};
  std::array<uint8_t, 5> previous{};
  std::vector<uint8_t> predicted;
  predicted.reserve(rows.size() * 6U);
  for (const auto& row : rows) {
    predicted.push_back(2);
    for (size_t column = 0; column < row.size(); ++column) {
      predicted.push_back(static_cast<uint8_t>(row[column] - previous[column]));
      previous[column] = row[column];
    }
  }
  const std::vector<uint8_t> compressed = predictorStoredZlib(predicted);
  pdf += "1 0 obj\n<< /Type /XRef /Size 3 /Root 1 0 R /W [1 3 1] /Index [1 2] /Length " +
         std::to_string(compressed.size()) +
         " /Filter /FlateDecode /DecodeParms << /Predictor 12 /Columns 5 >> >>\nstream\n";
  pdf.append(reinterpret_cast<const char*>(compressed.data()), compressed.size());
  pdf += "\nendstream\nendobj\nstartxref\n" + std::to_string(xrefOffset) + "\n%%EOF\n";
  return {pdf.begin(), pdf.end()};
}

std::vector<uint8_t> sparseHighObjectXrefPdf() {
  std::string pdf = "%PDF-1.5\n";
  const uint32_t xrefOffset = static_cast<uint32_t>(pdf.size());
  pdf += "126440 0 obj\n<< /Type /XRef /Size 126441 /Root 126440 0 R /W [1 4 2] "
         "/Index [126440 1] /Length 7 >>\nstream\n";
  appendXrefStreamEntry(&pdf, 1, xrefOffset, 0);
  pdf += "\nendstream\nendobj\nstartxref\n" + std::to_string(xrefOffset) + "\n%%EOF\n";
  return {pdf.begin(), pdf.end()};
}

std::vector<uint8_t> manyIndexPairsXrefPdf() {
  constexpr uint32_t kPairs = 65U;
  std::string pdf = "%PDF-1.5\n";
  const uint32_t xrefOffset = static_cast<uint32_t>(pdf.size());
  std::string index = "[";
  for (uint32_t object = 1; object <= kPairs; ++object) {
    index += std::to_string(object) + " 1 ";
  }
  index += "]";
  pdf += "1 0 obj\n<< /Type /XRef /Size 66 /Root 1 0 R /W [1 4 2] /Index " + index +
         " /Length 455 >>\nstream\n";
  appendXrefStreamEntry(&pdf, 1, xrefOffset, 0);
  for (uint32_t object = 2; object <= kPairs; ++object) {
    appendXrefStreamEntry(&pdf, 0, 0, 0);
  }
  pdf += "\nendstream\nendobj\nstartxref\n" + std::to_string(xrefOffset) + "\n%%EOF\n";
  return {pdf.begin(), pdf.end()};
}

std::vector<uint8_t> compressedIndirectLengthPdf() {
  std::string pdf = "%PDF-1.5\n";
  const uint32_t streamOffset = static_cast<uint32_t>(pdf.size());
  pdf += "1 0 obj\n<< /Length 2 0 R >>\nstream\nHello\nendstream\nendobj\n";
  const uint32_t objectStreamOffset = static_cast<uint32_t>(pdf.size());
  pdf += "3 0 obj\n<< /Type /ObjStm /N 1 /First 4 /Length 5 >>\nstream\n2 0 5\nendstream\nendobj\n";
  const uint32_t xrefOffset = static_cast<uint32_t>(pdf.size());
  pdf += "4 0 obj\n<< /Type /XRef /Size 5 /Root 1 0 R /W [1 4 2] /Index [0 5] /Length 35 >>\nstream\n";
  appendXrefStreamEntry(&pdf, 0, 0, UINT16_MAX);
  appendXrefStreamEntry(&pdf, 1, streamOffset, 0);
  appendXrefStreamEntry(&pdf, 2, 3, 0);
  appendXrefStreamEntry(&pdf, 1, objectStreamOffset, 0);
  appendXrefStreamEntry(&pdf, 1, xrefOffset, 0);
  pdf += "\nendstream\nendobj\nstartxref\n" + std::to_string(xrefOffset) + "\n%%EOF\n";
  return {pdf.begin(), pdf.end()};
}

struct RawObjectStreamFixture {
  std::vector<uint8_t> pdf;
  uint32_t objectStreamOffset = 0;
  uint64_t streamLength = 0;
};

RawObjectStreamFixture rawObjectStreamPdf() {
  const std::string firstObject = "<< /Value 11 >>";
  const std::string secondObject = "<< /Value 22 >>";
  const std::string index = "1 0 2 " + std::to_string(firstObject.size() + 1U) + " ";
  const std::string stream = index + firstObject + " " + secondObject;

  RawObjectStreamFixture fixture;
  std::string pdf = "%PDF-1.5\n";
  fixture.objectStreamOffset = static_cast<uint32_t>(pdf.size());
  pdf += "5 0 obj\n<< /Type /ObjStm /N 2 /First " + std::to_string(index.size()) + " /Length " +
         std::to_string(stream.size()) + " >>\nstream\n" + stream + "\nendstream\nendobj\n";
  fixture.streamLength = stream.size();
  fixture.pdf.assign(pdf.begin(), pdf.end());
  return fixture;
}

RawObjectStreamFixture highRatioObjectStreamPdf() {
  static constexpr uint8_t compressed[]{
      0x78, 0xda, 0xed, 0xc1, 0xb1, 0x11, 0x00, 0x10, 0x00, 0x04, 0xb0, 0x55, 0x7e, 0x03, 0xf4, 0xce, 0x18, 0x7a,
      0x85, 0x4e, 0x6b, 0x7f, 0x7b, 0xb8, 0x24, 0x2d, 0x35, 0xbd, 0xa7, 0xcc, 0x75, 0xee, 0x4e, 0xcb, 0x18, 0x01,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xe0, 0x2b, 0x0f, 0x6b, 0xad, 0x6f, 0xbb};
  RawObjectStreamFixture fixture;
  std::string prefix = "%PDF-1.5\n";
  fixture.objectStreamOffset = static_cast<uint32_t>(prefix.size());
  prefix += "5 0 obj\n<< /Type /ObjStm /N 1 /First 4 /Length 90 /Filter /FlateDecode >>\nstream\n";
  fixture.pdf.assign(prefix.begin(), prefix.end());
  fixture.pdf.insert(fixture.pdf.end(), std::begin(compressed), std::end(compressed));
  const std::string suffix = "\nendstream\nendobj\n";
  fixture.pdf.insert(fixture.pdf.end(), suffix.begin(), suffix.end());
  fixture.streamLength = 50018;
  return fixture;
}

bool stopFromFlag(void* context) { return context != nullptr && *static_cast<bool*>(context); }

}  // namespace

TEST(PdfXrefTest, ParsesClassicTableAndRoot) {
  PdfTestByteSource memory(loadFixture("classic_text.pdf"));
  const PdfByteSource source = memory.source();
  XrefHarness harness;

  const PdfStepResult result = parseXref(source, harness);

  ASSERT_TRUE(result.complete());
  PdfObjectReference root;
  ASSERT_TRUE(harness.table.root(&root));
  EXPECT_EQ(root, (PdfObjectReference{1, 0}));
  PdfXrefEntry content;
  ASSERT_TRUE(harness.table.find(4, &content).ok());
  EXPECT_EQ(content.type, PdfXrefEntryType::Uncompressed);
  EXPECT_LT(content.offset, source.size);
}

TEST(PdfXrefTest, PreflightsClassicSubsectionBeforeSmallStoreWrites) {
  PdfTestByteSource memory(loadFixture("classic_text.pdf"));
  XrefHarness harness;
  PdfTestRecordStore records(sizeof(PdfXrefEntry), 1);
  PdfXrefTable table(records.store());
  PdfXrefParser parser(memory.source(), harness.sourceBuffer.data(), harness.sourceBuffer.size(), harness.arena,
                       table, &harness.streamDecoder);
  parser.begin();

  PdfStepResult result = PdfStepResult::paused();
  for (uint16_t step = 0; step < 4096U && result.yielded(); ++step) {
    PdfWorkBudget budget{32, 4096};
    result = parser.step(budget);
  }

  ASSERT_TRUE(result.failed());
  EXPECT_EQ(result.status.error, PdfError::InsufficientStorage);
  EXPECT_EQ(result.status.offset, 6U * sizeof(PdfXrefEntry));
  EXPECT_EQ(table.entryCount(), 0U);
}

TEST(PdfXrefTest, CollidedNewestFilterRequestsRevisionBoundaryCompaction) {
  std::string pdf = "%PDF-1.4\n1 0 obj\n<< /Type /Catalog >>\nendobj\n";
  const uint64_t baseXref = pdf.size();
  pdf += "xref\n0 2\n0000000000 65535 f \n0000000009 00000 n \n"
         "trailer\n<< /Size 2 /Root 1 0 R >>\nstartxref\n" +
         std::to_string(baseXref) + "\n%%EOF\n";
  const uint64_t newestXref = pdf.size();
  pdf += "xref\n";
  for (uint32_t index = 0; index < 9U; ++index) {
    pdf += std::to_string(256U + index * 8U) + " 1\n0000000000 65535 f \n";
  }
  pdf += "trailer\n<< /Size 321 /Root 1 0 R /Prev " + std::to_string(baseXref) +
         " >>\nstartxref\n" + std::to_string(newestXref) + "\n%%EOF\n";

  PdfTestByteSource memory({pdf.begin(), pdf.end()});
  XrefHarness harness;
  PdfTestRecordStore records(sizeof(PdfXrefEntry), 32);
  PdfXrefTable table(records.store());
  std::array<uint8_t, 8U * sizeof(uint32_t)> filter{};
  PdfXrefParser parser(memory.source(), harness.sourceBuffer.data(), harness.sourceBuffer.size(), harness.arena,
                       table, &harness.streamDecoder);
  parser.begin();
  ASSERT_TRUE(table.configureNewestObjectFilter(filter.data(), filter.size(), nullptr, 0).ok());

  for (uint16_t step = 0; step < 4096U && !parser.compactionRequested(); ++step) {
    PdfWorkBudget budget{32, 4096};
    const PdfStepResult result = parser.step(budget);
    ASSERT_FALSE(result.failed()) << static_cast<int>(result.status.error) << '@' << result.status.offset;
  }

  EXPECT_TRUE(parser.compactionRequested());
  EXPECT_TRUE(table.sectionCompactionRequired());
  EXPECT_EQ(table.entryCount(), 9U);
}

TEST(PdfXrefTest, NewestIncrementalRevisionWinsWithBudgetOne) {
  PdfTestByteSource memory(loadFixture("incremental_update.pdf"));
  const PdfByteSource source = memory.source();
  XrefHarness harness;

  const PdfStepResult result = parseXref(source, harness, true);

  ASSERT_TRUE(result.complete());
  PdfXrefEntry page;
  ASSERT_TRUE(harness.table.find(3, &page).ok());
  std::array<uint8_t, 7> header{};
  ASSERT_TRUE(pdfReadExact(source, page.offset, header.data(), header.size()).ok());
  EXPECT_EQ(std::string(header.begin(), header.end()), "3 0 obj");
  EXPECT_GT(page.offset, source.size / 2);
}

TEST(PdfXrefTest, ParsesSparseXrefStreamWidthsAndCompressedEntries) {
  PdfTestByteSource memory(loadFixture("xref_stream_objstm.pdf"));
  const PdfByteSource source = memory.source();
  XrefHarness harness;

  const PdfStepResult result = parseXref(source, harness, true);

  ASSERT_TRUE(result.complete());
  PdfObjectReference root;
  ASSERT_TRUE(harness.table.root(&root));
  EXPECT_EQ(root, (PdfObjectReference{1, 0}));
  PdfXrefEntry catalog;
  ASSERT_TRUE(harness.table.find(1, &catalog).ok());
  EXPECT_EQ(catalog.type, PdfXrefEntryType::Compressed);
  EXPECT_EQ(catalog.offset, 6u);
  EXPECT_EQ(catalog.objectStreamIndex, 0u);
  PdfXrefEntry content;
  ASSERT_TRUE(harness.table.find(5, &content).ok());
  EXPECT_EQ(content.type, PdfXrefEntryType::Uncompressed);
  EXPECT_LT(content.offset, source.size);
}

TEST(PdfXrefTest, ParsesSparseHighObjectNumberWithoutTreatingSizeAsRecordCount) {
  PdfTestByteSource memory(sparseHighObjectXrefPdf());
  XrefHarness harness;

  const PdfStepResult result = parseXref(memory.source(), harness, true);

  ASSERT_TRUE(result.complete()) << static_cast<int>(result.status.error) << "@" << result.status.offset;
  EXPECT_EQ(harness.table.entryCount(), 1U);
  PdfXrefEntry xref{};
  ASSERT_TRUE(harness.table.find(126'440U, &xref).ok());
  EXPECT_EQ(xref.type, PdfXrefEntryType::Uncompressed);
}

TEST(PdfXrefTest, DecodesSonyStylePredictorTwelveXrefStreamCooperatively) {
  PdfTestByteSource memory(predictorTwelveXrefPdf());
  XrefHarness harness;

  const PdfStepResult result = parseXref(memory.source(), harness, true);

  ASSERT_TRUE(result.complete()) << static_cast<int>(result.status.error) << "@" << result.status.offset;
  PdfObjectReference root{};
  ASSERT_TRUE(harness.table.root(&root));
  EXPECT_EQ(root, (PdfObjectReference{1, 0}));
  PdfXrefEntry xref{};
  ASSERT_TRUE(harness.table.find(1, &xref).ok());
  EXPECT_EQ(xref.type, PdfXrefEntryType::Uncompressed);
  EXPECT_EQ(xref.offset, 9U);
}

TEST(PdfXrefTest, AcceptsWideZeroPrefixedFieldsAndRejectsDiscardedNonzeroBytes) {
  {
    PdfTestByteSource memory(wideXrefStreamPdf(false));
    XrefHarness harness;

    const PdfStepResult result = parseXref(memory.source(), harness, true);

    ASSERT_TRUE(result.complete()) << static_cast<int>(result.status.error) << "@" << result.status.offset;
    PdfXrefEntry xref{};
    ASSERT_TRUE(harness.table.find(1U, &xref).ok());
    EXPECT_EQ(xref.type, PdfXrefEntryType::Uncompressed);
    EXPECT_EQ(xref.offset, 9U);
  }
  {
    PdfTestByteSource memory(wideXrefStreamPdf(true));
    XrefHarness harness;

    const PdfStepResult result = parseXref(memory.source(), harness, true);

    ASSERT_TRUE(result.failed());
    EXPECT_EQ(result.status.error, PdfError::LimitExceeded);
  }
}

TEST(PdfXrefTest, FollowsMoreThanThirtyTwoPrevRevisionsAndStillRejectsCycles) {
  PdfTestByteSource memory(manyPrevRevisionsPdf(40U));
  XrefHarness harness;

  const PdfStepResult result = parseXref(memory.source(), harness, true);

  ASSERT_TRUE(result.complete()) << static_cast<int>(result.status.error) << "@" << result.status.offset;
  PdfObjectReference root{};
  ASSERT_TRUE(harness.table.root(&root));
  EXPECT_EQ(root, (PdfObjectReference{1, 0}));

  PdfTestByteSource cycleMemory(loadFixture("xref_prev_cycle.pdf"));
  XrefHarness cycleHarness;
  const PdfStepResult cycle = parseXref(cycleMemory.source(), cycleHarness, true);
  ASSERT_TRUE(cycle.failed());
  EXPECT_EQ(cycle.status.error, PdfError::Malformed);
}

TEST(PdfXrefTest, UsesObjectDomainAndActualStoreInsteadOfLegacyRecordCeiling) {
  EXPECT_EQ(PdfLimits::MaxXrefRecords, PdfLimits::MaxIndirectObjectNumber + 1U);

  PdfTestByteSource memory(truncatedLargeClassicSubsectionPdf());
  XrefHarness harness;
  const PdfStepResult result = parseXref(memory.source(), harness, true);

  ASSERT_TRUE(result.failed());
  EXPECT_EQ(result.status.error, PdfError::InsufficientStorage);
  EXPECT_EQ(result.status.offset, 262145ULL * sizeof(PdfXrefEntry));
  EXPECT_EQ(harness.table.entryCount(), 0U);
}

TEST(PdfXrefTest, StreamsMoreThanSixtyFourIndexPairsWithoutGrowingTheTrailerArena) {
  PdfTestByteSource memory(manyIndexPairsXrefPdf());
  XrefHarness harness;

  const PdfStepResult result = parseXref(memory.source(), harness, true);

  ASSERT_TRUE(result.complete()) << static_cast<int>(result.status.error) << "@" << result.status.offset;
  EXPECT_EQ(harness.table.entryCount(), 65U);
  PdfXrefEntry root{};
  ASSERT_TRUE(harness.table.find(1U, &root).ok());
  EXPECT_EQ(root.type, PdfXrefEntryType::Uncompressed);
  EXPECT_EQ(root.offset, 9U);
}

TEST(PdfXrefTest, EnforcesCallerCapAndReportsCompletedDecodedBytes) {
  const std::vector<uint8_t> fixture = loadFixture("xref_stream_objstm.pdf");
  ASSERT_FALSE(fixture.empty());

  for (const uint64_t limit : {55ULL, 56ULL}) {
    SCOPED_TRACE(limit);
    PdfTestByteSource memory(fixture);
    XrefHarness harness;
    PdfXrefParser parser(memory.source(), harness.sourceBuffer.data(), harness.sourceBuffer.size(), harness.arena,
                         harness.table, &harness.streamDecoder, PdfStreamDecodeLimits{limit, 200});
    parser.begin();

    PdfStepResult result = PdfStepResult::paused();
    for (uint32_t step = 0; step < 65536U && result.yielded(); ++step) {
      PdfWorkBudget budget{1, sizeof(PdfXrefEntry)};
      result = parser.step(budget);
      EXPECT_LE(parser.currentDecodedBytes(), limit);
    }

    if (limit == 55U) {
      ASSERT_TRUE(result.failed());
      EXPECT_EQ(result.status.error, PdfError::ExpansionLimit);
      EXPECT_EQ(parser.decodedBytes(), 0U);
    } else {
      ASSERT_TRUE(result.complete()) << static_cast<int>(result.status.error) << "@" << result.status.offset;
      EXPECT_EQ(parser.decodedBytes(), 56U);
      EXPECT_EQ(parser.currentDecodedBytes(), 56U);
    }
  }
}

TEST(PdfXrefTest, SharesLexerAndDecoderSourceWindowAcrossPrevRevision) {
  PdfTestByteSource memory(loadFixture("incremental_xref_stream.pdf"));
  XrefHarness harness;
  std::array<uint8_t, 4096> output{};
  std::array<uint8_t, 32768> dictionary{};
  PdfStreamDecoder sharedDecoder({harness.sourceBuffer.data(), harness.sourceBuffer.size(), output.data(),
                                  output.size(), dictionary.data(), dictionary.size()});
  PdfXrefParser parser(memory.source(), harness.sourceBuffer.data(), harness.sourceBuffer.size(), harness.arena,
                       harness.table, &sharedDecoder, PdfStreamDecodeLimits{21, 200});
  parser.begin();

  PdfStepResult result = PdfStepResult::paused();
  for (uint32_t step = 0; step < 65536U && result.yielded(); ++step) {
    PdfWorkBudget budget{1, sizeof(PdfXrefEntry)};
    result = parser.step(budget);
  }

  ASSERT_TRUE(result.complete()) << static_cast<int>(result.status.error) << "@" << result.status.offset;
  EXPECT_EQ(parser.decodedBytes(), 21U);
  PdfXrefEntry page;
  ASSERT_TRUE(harness.table.find(3, &page).ok());
  EXPECT_GT(page.offset, memory.source().size / 2U);
}

TEST(PdfXrefTest, DoesNotOverflowTinyLexerBuffers) {
  const std::vector<uint8_t> fixture = loadFixture("xref_stream_objstm.pdf");
  ASSERT_FALSE(fixture.empty());
  for (const size_t bufferSize : {1U, 7U}) {
    SCOPED_TRACE(bufferSize);
    PdfTestByteSource memory(fixture);
    XrefHarness harness;
    std::array<uint8_t, 9> guarded{};
    guarded.fill(0xA5);

    const PdfStepResult result = parseXrefWithBuffer(memory.source(), harness, guarded.data() + 1, bufferSize);

    ASSERT_TRUE(result.failed());
    EXPECT_EQ(result.status.error, PdfError::Malformed);
    EXPECT_EQ(guarded.front(), 0xA5);
    for (size_t index = bufferSize + 1; index < guarded.size(); ++index) {
      EXPECT_EQ(guarded[index], 0xA5) << index;
    }
  }
}

TEST(PdfXrefTest, AcceptsCrlfAndRejectsOverlongXrefStreamBoundary) {
  const std::vector<uint8_t> fixture = loadFixture("xref_stream_objstm.pdf");
  ASSERT_FALSE(fixture.empty());
  const std::string original(fixture.begin(), fixture.end());
  const std::string boundary = "\nendstream\nendobj\nstartxref";
  const size_t boundaryOffset = original.rfind(boundary);
  ASSERT_NE(boundaryOffset, std::string::npos);

  std::string crlf = original;
  crlf.replace(boundaryOffset, boundary.size(), "\r\nendstream\r\nendobj\r\nstartxref");
  PdfTestByteSource crlfMemory(std::vector<uint8_t>(crlf.begin(), crlf.end()));
  XrefHarness crlfHarness;
  const PdfStepResult crlfResult = parseXref(crlfMemory.source(), crlfHarness, true);
  ASSERT_TRUE(crlfResult.complete()) << static_cast<int>(crlfResult.status.error) << "@" << crlfResult.status.offset;

  std::string overlong = original;
  const std::string length = "/Length 41 /Type /XRef";
  const size_t lengthOffset = overlong.find(length);
  ASSERT_NE(lengthOffset, std::string::npos);
  overlong.replace(lengthOffset, length.size(), "/Length 61 /Type /XRef");
  PdfTestByteSource overlongMemory(std::vector<uint8_t>(overlong.begin(), overlong.end()));
  XrefHarness overlongHarness;
  const PdfStepResult overlongResult = parseXref(overlongMemory.source(), overlongHarness, true);
  ASSERT_TRUE(overlongResult.failed());
  EXPECT_EQ(overlongResult.status.error, PdfError::Malformed);
}

TEST(PdfXrefTest, NewestXrefStreamRevisionWinsOverClassicPrev) {
  PdfTestByteSource memory(loadFixture("incremental_xref_stream.pdf"));
  const PdfByteSource source = memory.source();
  XrefHarness harness;

  ASSERT_TRUE(parseXref(source, harness, true).complete());
  PdfXrefEntry page;
  ASSERT_TRUE(harness.table.find(3, &page).ok());
  std::array<uint8_t, 7> header{};
  ASSERT_TRUE(pdfReadExact(source, page.offset, header.data(), header.size()).ok());
  EXPECT_EQ(std::string(header.begin(), header.end()), "3 0 obj");
  EXPECT_GT(page.offset, source.size / 2);
}

TEST(PdfXrefTest, RejectsMalformedXrefStreamWidthsAndIndexes) {
  const std::vector<uint8_t> complete = loadFixture("xref_stream_objstm.pdf");
  for (const auto& replacement : std::array<std::pair<std::string, std::string>, 2>{
           {std::make_pair("/W [1 4 2]", "/W [9 4 2]"),
            std::make_pair("/Index [0 2 2 3 5 3]", "/Index [0 2 2 3 5 4]")}}) {
    std::string mutated(complete.begin(), complete.end());
    const size_t position = mutated.find(replacement.first);
    ASSERT_NE(position, std::string::npos);
    mutated.replace(position, replacement.first.size(), replacement.second);
    PdfTestByteSource memory(std::vector<uint8_t>(mutated.begin(), mutated.end()));
    XrefHarness harness;
    const PdfStepResult result = parseXref(memory.source(), harness);
    EXPECT_TRUE(result.failed()) << replacement.second;
    EXPECT_TRUE(result.status.error == PdfError::LimitExceeded || result.status.error == PdfError::UnexpectedEof ||
                result.status.error == PdfError::Malformed)
        << replacement.second;
  }
}

TEST(PdfXrefTest, ExternalMergeUsesFixedSixtyFourEntryBufferAndNewestDuplicateWins) {
  bool sourceActive = true;
  PdfTestRecordStore primary(sizeof(PdfXrefEntry), 140);
  PdfTestRecordStore scratch(sizeof(PdfXrefEntry), 140);
  primary.forbidReadsWhile(&sourceActive);
  scratch.forbidReadsWhile(&sourceActive);
  PdfXrefTable table(primary.store());
  table.reset();
  ASSERT_TRUE(table.appendNewest({5, 0, PdfXrefEntryType::Uncompressed, 0, 9999, 0}).ok());
  for (uint32_t object = 130; object > 0; --object) {
    ASSERT_TRUE(table.appendNewest({object, 0, PdfXrefEntryType::Uncompressed, 0, object * 10ULL, 0}).ok());
  }
  std::array<PdfXrefEntry, PdfLimits::XrefMergeEntries> mergeBuffer{};

  EXPECT_EQ(table.finalize(scratch.store(), mergeBuffer.data(), PdfLimits::XrefMergeEntries - 1).error,
            PdfError::InvalidArgument);
  EXPECT_EQ(table.finalize(scratch.store(), mergeBuffer.data(), mergeBuffer.size()).error, PdfError::IoFailure);

  sourceActive = false;
  ASSERT_TRUE(table.finalize(scratch.store(), mergeBuffer.data(), mergeBuffer.size()).ok());
  EXPECT_TRUE(table.finalized());
  EXPECT_EQ(table.entryCount(), 130u);
  PdfXrefEntry newest;
  ASSERT_TRUE(table.find(5, &newest).ok());
  EXPECT_EQ(newest.offset, 9999u);
  PdfXrefEntry last;
  ASSERT_TRUE(table.find(130, &last).ok());
  EXPECT_EQ(last.offset, 1300u);
}

TEST(PdfXrefTest, RejectsEncryptionBadStartxrefAndPrevCycle) {
  for (const auto& fixture :
       std::array<std::pair<const char*, PdfError>, 3>{{{"encrypted.pdf", PdfError::Encrypted},
                                                        {"bad_startxref.pdf", PdfError::InvalidOffset},
                                                        {"xref_prev_cycle.pdf", PdfError::Malformed}}}) {
    PdfTestByteSource memory(loadFixture(fixture.first));
    const PdfByteSource source = memory.source();
    XrefHarness harness;
    const PdfStepResult result = parseXref(source, harness);
    EXPECT_TRUE(result.failed()) << fixture.first;
    EXPECT_EQ(result.status.error, fixture.second) << fixture.first;
  }
}

TEST(PdfXrefTest, KeepsEncryptReferenceAndFirstFileIdentifierForSecurityBootstrap) {
  const std::vector<uint8_t> fixture = loadFixture("encrypted.pdf");
  std::string pdf(fixture.begin(), fixture.end());
  const std::string marker = "/Encrypt 6 0 R";
  const size_t position = pdf.find(marker);
  ASSERT_NE(position, std::string::npos);
  pdf.insert(position + marker.size(),
             " /ID [<F71F972E16CFB5D750102A3138AC170F01020304><00112233445566778899AABBCCDDEEFF>]");

  PdfTestByteSource memory(std::vector<uint8_t>(pdf.begin(), pdf.end()));
  XrefHarness harness;
  ASSERT_TRUE(parseXref(memory.source(), harness).complete());

  PdfSecurityTrailer security{};
  ASSERT_TRUE(harness.table.security(&security));
  EXPECT_EQ(security.encryptionReference, (PdfObjectReference{6, 0}));
  constexpr uint8_t expected[] = {0xf7, 0x1f, 0x97, 0x2e, 0x16, 0xcf, 0xb5, 0xd7, 0x50, 0x10,
                                  0x2a, 0x31, 0x38, 0xac, 0x17, 0x0f, 0x01, 0x02, 0x03, 0x04};
  EXPECT_EQ(security.fileIdentifierLength, sizeof(expected));
  EXPECT_EQ(std::memcmp(security.fileIdentifier, expected, sizeof(expected)), 0);
}

TEST(PdfXrefTest, EveryClassicTruncationFailsWithoutOutOfRangeRead) {
  const std::vector<uint8_t> complete = loadFixture("classic_text.pdf");
  size_t meaningfulLength = complete.size();
  while (meaningfulLength != 0 &&
         (complete[meaningfulLength - 1U] == ' ' || complete[meaningfulLength - 1U] == '\t' ||
          complete[meaningfulLength - 1U] == '\r' || complete[meaningfulLength - 1U] == '\n')) {
    --meaningfulLength;
  }
  for (size_t length = 0; length < meaningfulLength; ++length) {
    PdfTestByteSource memory(std::vector<uint8_t>(complete.begin(), complete.begin() + length));
    const PdfByteSource source = memory.source();
    XrefHarness harness;
    const PdfStepResult result = parseXref(source, harness);
    EXPECT_TRUE(result.failed()) << "length=" << length;
    EXPECT_TRUE(result.status.error == PdfError::UnexpectedEof || result.status.error == PdfError::InvalidOffset ||
                result.status.error == PdfError::Malformed)
        << "length=" << length;
  }
}

TEST(PdfObjectResolverTest, ResolvesCatalogPageAndContentStreamEndToEnd) {
  PdfTestByteSource memory(loadFixture("classic_text.pdf"));
  const PdfByteSource source = memory.source();
  XrefHarness harness;
  ASSERT_TRUE(parseXref(source, harness).complete());

  PdfObjectReference root;
  ASSERT_TRUE(harness.table.root(&root));
  PdfResolvedObject catalog;
  ASSERT_TRUE(resolveObject(source, harness, root, &catalog).complete());
  uint16_t pagesIndex = PDF_INVALID_INDEX;
  ASSERT_TRUE(pdfDictionaryFind(harness.arena, catalog.rootIndex, "Pages", &pagesIndex));
  const PdfValue pages = harness.arena.values[pagesIndex];
  ASSERT_EQ(pages.kind, PdfValueKind::Reference);

  PdfResolvedObject pagesObject;
  ASSERT_TRUE(resolveObject(source, harness, {pages.objectNumber, pages.generation}, &pagesObject).complete());
  uint16_t kidsIndex = PDF_INVALID_INDEX;
  ASSERT_TRUE(pdfDictionaryFind(harness.arena, pagesObject.rootIndex, "Kids", &kidsIndex));
  uint16_t pageIndex = PDF_INVALID_INDEX;
  ASSERT_TRUE(pdfArrayAt(harness.arena, kidsIndex, 0, &pageIndex));
  const PdfValue pageReference = harness.arena.values[pageIndex];

  PdfResolvedObject page;
  ASSERT_TRUE(resolveObject(source, harness, {pageReference.objectNumber, pageReference.generation}, &page).complete());
  uint16_t contentIndex = PDF_INVALID_INDEX;
  ASSERT_TRUE(pdfDictionaryFind(harness.arena, page.rootIndex, "Contents", &contentIndex));
  const PdfValue contentReference = harness.arena.values[contentIndex];

  PdfResolvedObject content;
  ASSERT_TRUE(resolveObject(source, harness, {contentReference.objectNumber, contentReference.generation}, &content)
                  .complete());
  ASSERT_TRUE(content.hasStream);
  std::vector<uint8_t> bytes(static_cast<size_t>(content.streamLength));
  ASSERT_TRUE(pdfReadExact(source, content.streamOffset, bytes.data(), bytes.size()).ok());
  EXPECT_NE(std::string(bytes.begin(), bytes.end()).find("(Hello PDF)"), std::string::npos);
}

TEST(PdfObjectResolverTest, CachedAdjacentXrefEntryKeepsSourceReaderSelected) {
  std::string pdf = "1 0 obj\n42\nendobj\n";
  const uint64_t secondOffset = pdf.size();
  pdf += "2 0 obj\n43\nendobj\n";
  const uint64_t thirdOffset = pdf.size();
  pdf += "3 0 obj\n44\nendobj\n";
  const uint64_t fourthOffset = pdf.size();
  pdf += "4 0 obj\n45\nendobj\n";
  const uint64_t fifthOffset = pdf.size();
  pdf += "5 0 obj\n46\nendobj\n";
  PdfTestByteSource memory({pdf.begin(), pdf.end()});
  const PdfByteSource source = memory.source();
  XrefHarness harness;
  ASSERT_TRUE(harness.table.appendNewest({1, 0, PdfXrefEntryType::Uncompressed, 0, 0, 0}).ok());
  ASSERT_TRUE(harness.table.appendNewest({2, 0, PdfXrefEntryType::Uncompressed, 0, secondOffset, 0}).ok());
  ASSERT_TRUE(harness.table.appendNewest({3, 0, PdfXrefEntryType::Uncompressed, 0, thirdOffset, 0}).ok());
  ASSERT_TRUE(harness.table.appendNewest({4, 0, PdfXrefEntryType::Uncompressed, 0, fourthOffset, 0}).ok());
  ASSERT_TRUE(harness.table.appendNewest({5, 0, PdfXrefEntryType::Uncompressed, 0, fifthOffset, 0}).ok());
  std::array<PdfXrefEntry, PdfLimits::XrefMergeEntries> mergeBuffer{};
  PdfTestRecordStore scratch(sizeof(PdfXrefEntry), 5);
  ASSERT_TRUE(harness.table.finalize(scratch.store(), mergeBuffer.data(), mergeBuffer.size()).ok());
  PdfObjectResolver resolver(source, harness.table, harness.sourceBuffer.data(), harness.sourceBuffer.size(),
                             harness.arena, harness.resolverWorkspace());

  PdfResolvedObject first;
  ASSERT_TRUE(resolveObject(resolver, {1, 0}, &first).complete());
  PdfResolvedObject second;
  ASSERT_TRUE(resolveObject(resolver, {2, 0}, &second).complete());
  PdfResolvedObject third;
  ASSERT_TRUE(resolveObject(resolver, {3, 0}, &third).complete());
  PdfResolvedObject fourth;
  ASSERT_TRUE(resolveObject(resolver, {4, 0}, &fourth).complete());
  ASSERT_EQ(harness.reader, PdfObjectResolverReader::Source);
  const uint32_t xrefSelections = harness.xrefSelections;

  PdfResolvedObject fifth;
  ASSERT_TRUE(resolveObject(resolver, {5, 0}, &fifth).complete());
  EXPECT_EQ(harness.xrefSelections, xrefSelections);
  EXPECT_EQ(harness.reader, PdfObjectResolverReader::Source);
}

TEST(PdfObjectResolverTest, CachedBeginImmediatelyPublishesRequestedReference) {
  const std::string pdf = "4 0 obj\n45\nendobj\n";
  PdfTestByteSource memory({pdf.begin(), pdf.end()});
  const PdfByteSource source = memory.source();
  XrefHarness harness;
  ASSERT_TRUE(harness.table.appendNewest({4, 0, PdfXrefEntryType::Uncompressed, 0, 0, 0}).ok());
  std::array<PdfXrefEntry, PdfLimits::XrefMergeEntries> mergeBuffer{};
  PdfTestRecordStore scratch(sizeof(PdfXrefEntry), 1);
  ASSERT_TRUE(harness.table.finalize(scratch.store(), mergeBuffer.data(), mergeBuffer.size()).ok());

  PdfXrefEntry cached{};
  ASSERT_TRUE(harness.table.find(4, &cached).ok());
  ASSERT_EQ(cached.objectNumber, 4U);

  PdfObjectResolver resolver(source, harness.table, harness.sourceBuffer.data(), harness.sourceBuffer.size(),
                             harness.arena, harness.resolverWorkspace());
  const PdfObjectReference requested{4, 0};
  ASSERT_TRUE(resolver.begin(requested).ok());
  EXPECT_EQ(resolver.result().reference, requested);

  PdfStepResult terminal = PdfStepResult::paused();
  for (uint16_t step = 0; step < 256U && terminal.yielded(); ++step) {
    PdfWorkBudget budget{32, 4096};
    terminal = resolver.step(budget);
    EXPECT_EQ(resolver.result().reference, requested);
  }
  ASSERT_TRUE(terminal.complete()) << static_cast<int>(terminal.status.error) << '@' << terminal.status.offset;
}

TEST(PdfObjectResolverTest, ResolvesUncompressedIndirectStreamLengthAndRestoresParentArena) {
  std::string pdf = "1 0 obj\n<< /Length 2 0 R /Filter /FlateDecode >>\nstream\nHello\nendstream\nendobj\n";
  const size_t lengthOffset = pdf.size();
  pdf += "2 0 obj\n5\nendobj\n";
  PdfTestByteSource memory(std::vector<uint8_t>(pdf.begin(), pdf.end()));
  const PdfByteSource source = memory.source();
  XrefHarness harness;
  harness.table.reset();
  ASSERT_TRUE(harness.table.appendNewest({1, 0, PdfXrefEntryType::Uncompressed, 0, 0, 0}).ok());
  ASSERT_TRUE(harness.table.appendNewest({2, 0, PdfXrefEntryType::Uncompressed, 0, lengthOffset, 0}).ok());

  PdfResolvedObject stream;
  const PdfStepResult result = resolveObject(source, harness, {1, 0}, &stream);

  ASSERT_TRUE(result.complete()) << static_cast<int>(result.status.error) << "@" << result.status.offset;
  ASSERT_TRUE(stream.hasStream);
  EXPECT_EQ(stream.streamLength, 5U);
  std::array<uint8_t, 5> payload{};
  ASSERT_TRUE(pdfReadExact(source, stream.streamOffset, payload.data(), payload.size()).ok());
  EXPECT_EQ(std::string(payload.begin(), payload.end()), "Hello");
  uint16_t lengthIndex = PDF_INVALID_INDEX;
  ASSERT_TRUE(pdfDictionaryFind(harness.arena, stream.rootIndex, "Length", &lengthIndex));
  ASSERT_LT(lengthIndex, harness.arena.valueCount);
  EXPECT_EQ(harness.arena.values[lengthIndex].kind, PdfValueKind::Reference);
  uint16_t filterIndex = PDF_INVALID_INDEX;
  ASSERT_TRUE(pdfDictionaryFind(harness.arena, stream.rootIndex, "Filter", &filterIndex));
  ASSERT_LT(filterIndex, harness.arena.valueCount);
  EXPECT_TRUE(pdfTextEquals(harness.arena, harness.arena.values[filterIndex], "FlateDecode"));
}

TEST(PdfObjectResolverTest, ResolvesCompressedIndirectStreamLengthWithExclusiveReaderOwnership) {
  PdfTestByteSource memory(compressedIndirectLengthPdf());
  const PdfByteSource source = memory.source();
  XrefHarness harness;
  ASSERT_TRUE(parseXref(source, harness).complete());
  PdfObjectResolver resolver(source, harness.table, harness.sourceBuffer.data(), harness.sourceBuffer.size(),
                             harness.arena, harness.resolverWorkspace());

  PdfResolvedObject stream;
  const PdfStepResult result = resolveObject(resolver, {1, 0}, &stream);

  ASSERT_TRUE(result.complete()) << static_cast<int>(result.status.error) << "@" << result.status.offset;
  ASSERT_TRUE(stream.hasStream);
  EXPECT_EQ(stream.streamLength, 5U);
  std::array<uint8_t, 5> payload{};
  ASSERT_TRUE(pdfReadExact(source, stream.streamOffset, payload.data(), payload.size()).ok());
  EXPECT_EQ(std::string(payload.begin(), payload.end()), "Hello");
  EXPECT_EQ(harness.objectStreamStorage.resetCount(), 0U);
  EXPECT_EQ(harness.objectStoreWriterCalls, 0U);
  EXPECT_EQ(harness.objectStoreSelections, 0U);
  EXPECT_EQ(resolver.takeCompletedStreamBytes(), 5U);
  uint16_t lengthIndex = PDF_INVALID_INDEX;
  ASSERT_TRUE(pdfDictionaryFind(harness.arena, stream.rootIndex, "Length", &lengthIndex));
  ASSERT_LT(lengthIndex, harness.arena.valueCount);
  EXPECT_EQ(harness.arena.values[lengthIndex].kind, PdfValueKind::Reference);
}

TEST(PdfObjectResolverTest, RejectsAmbiguousDuplicateStreamLength) {
  const std::string pdf = "1 0 obj\n<< /Length 5 /Length 99 >>\nstream\nHello\nendstream\nendobj\n";
  PdfTestByteSource memory(std::vector<uint8_t>(pdf.begin(), pdf.end()));
  const PdfByteSource source = memory.source();
  XrefHarness harness;
  harness.table.reset();
  ASSERT_TRUE(harness.table.appendNewest({1, 0, PdfXrefEntryType::Uncompressed, 0, 0, 0}).ok());

  const PdfStepResult result = resolveObject(source, harness, {1, 0}, nullptr);

  ASSERT_TRUE(result.failed());
  EXPECT_EQ(result.status.error, PdfError::Malformed);
}

TEST(PdfObjectResolverTest, ResolvesCompressedObjectsFromOneCachedFlateObjectStream) {
  PdfTestByteSource memory(loadFixture("xref_stream_objstm.pdf"));
  const PdfByteSource source = memory.source();
  XrefHarness harness;
  ASSERT_TRUE(parseXref(source, harness).complete());
  PdfObjectResolver resolver(source, harness.table, harness.sourceBuffer.data(), harness.sourceBuffer.size(),
                             harness.arena, harness.resolverWorkspace());

  PdfResolvedObject catalog;
  ASSERT_TRUE(resolveObject(resolver, {1, 0}, &catalog).complete());
  uint16_t pagesIndex = PDF_INVALID_INDEX;
  ASSERT_TRUE(pdfDictionaryFind(harness.arena, catalog.rootIndex, "Pages", &pagesIndex));
  const PdfObjectReference pagesRef{harness.arena.values[pagesIndex].objectNumber,
                                    harness.arena.values[pagesIndex].generation};
  EXPECT_EQ(harness.objectStreamStorage.resetCount(), 1u);

  PdfResolvedObject pages;
  ASSERT_TRUE(resolveObject(resolver, pagesRef, &pages).complete());
  uint16_t kidsIndex = PDF_INVALID_INDEX;
  ASSERT_TRUE(pdfDictionaryFind(harness.arena, pages.rootIndex, "Kids", &kidsIndex));
  uint16_t pageItem = PDF_INVALID_INDEX;
  ASSERT_TRUE(pdfArrayAt(harness.arena, kidsIndex, 0, &pageItem));
  const PdfObjectReference pageRef{harness.arena.values[pageItem].objectNumber,
                                   harness.arena.values[pageItem].generation};

  PdfResolvedObject page;
  ASSERT_TRUE(resolveObject(resolver, pageRef, &page).complete());
  EXPECT_EQ(harness.objectStreamStorage.resetCount(), 1u);
  EXPECT_FALSE(harness.sourceActive);
  EXPECT_FALSE(harness.xrefActive);
  EXPECT_EQ(harness.reader, PdfObjectResolverReader::ObjectStore);
  EXPECT_GE(harness.sourceTransitions, 1u);
  uint16_t contentsIndex = PDF_INVALID_INDEX;
  ASSERT_TRUE(pdfDictionaryFind(harness.arena, page.rootIndex, "Contents", &contentsIndex));
  const PdfObjectReference contentsRef{harness.arena.values[contentsIndex].objectNumber,
                                       harness.arena.values[contentsIndex].generation};

  PdfResolvedObject content;
  ASSERT_TRUE(resolveObject(resolver, contentsRef, &content).complete());
  EXPECT_TRUE(harness.sourceActive);
  ASSERT_TRUE(content.hasStream);
  std::vector<uint8_t> contentBytes(static_cast<size_t>(content.streamLength));
  ASSERT_TRUE(pdfReadExact(source, content.streamOffset, contentBytes.data(), contentBytes.size()).ok());
  EXPECT_NE(std::string(contentBytes.begin(), contentBytes.end()).find("Compressed object stream text."),
            std::string::npos);
}

TEST(PdfObjectResolverTest, DirectRawObjectStreamNeedsNoDecoderOrStoreAndPublishesBytesOnce) {
  const RawObjectStreamFixture fixture = rawObjectStreamPdf();
  PdfTestByteSource memory(fixture.pdf);
  XrefHarness harness;
  harness.table.reset();
  ASSERT_TRUE(harness.table.appendNewest({1, 0, PdfXrefEntryType::Compressed, 0, 5, 0}).ok());
  ASSERT_TRUE(harness.table.appendNewest({2, 0, PdfXrefEntryType::Compressed, 0, 5, 1}).ok());
  ASSERT_TRUE(
      harness.table.appendNewest({5, 0, PdfXrefEntryType::Uncompressed, 0, fixture.objectStreamOffset, 0}).ok());
  const PdfObjectResolverWorkspace workspace{
      nullptr, {}, &harness, XrefHarness::setSourceAccess, PdfStreamDecodeLimits{fixture.streamLength, 200}};
  PdfObjectResolver resolver(memory.source(), harness.table, harness.sourceBuffer.data(), harness.sourceBuffer.size(),
                             harness.arena, workspace);

  PdfResolvedObject first;
  ASSERT_TRUE(resolveObject(resolver, {1, 0}, &first).complete());
  uint16_t valueIndex = PDF_INVALID_INDEX;
  ASSERT_TRUE(pdfDictionaryFind(harness.arena, first.rootIndex, "Value", &valueIndex));
  ASSERT_LT(valueIndex, harness.arena.valueCount);
  EXPECT_EQ(harness.arena.values[valueIndex].integerValue, 11);
  EXPECT_EQ(resolver.takeCompletedStreamBytes(), fixture.streamLength);
  EXPECT_EQ(resolver.takeCompletedStreamBytes(), 0U);

  PdfResolvedObject second;
  ASSERT_TRUE(resolveObject(resolver, {2, 0}, &second).complete());
  ASSERT_TRUE(pdfDictionaryFind(harness.arena, second.rootIndex, "Value", &valueIndex));
  ASSERT_LT(valueIndex, harness.arena.valueCount);
  EXPECT_EQ(harness.arena.values[valueIndex].integerValue, 22);
  EXPECT_EQ(resolver.takeCompletedStreamBytes(), 0U);
  EXPECT_EQ(harness.objectStreamStorage.resetCount(), 0U);
  EXPECT_EQ(harness.objectStoreWriterCalls, 0U);
  EXPECT_EQ(harness.objectStoreSelections, 0U);
}

TEST(PdfObjectResolverTest, StopsObjectStreamIndexAfterTargetBoundary) {
  constexpr uint32_t objectCount = 512;
  std::string index;
  std::string body;
  for (uint32_t ordinal = 0; ordinal < objectCount; ++ordinal) {
    index += std::to_string(ordinal + 1U) + " " + std::to_string(body.size()) + " ";
    body += "0 ";
  }
  const std::string stream = index + body;
  std::string pdf = "%PDF-1.5\n";
  const uint32_t objectStreamOffset = static_cast<uint32_t>(pdf.size());
  pdf += "5 0 obj\n<< /Type /ObjStm /N " + std::to_string(objectCount) + " /First " +
         std::to_string(index.size()) + " /Length " + std::to_string(stream.size()) + " >>\nstream\n" + stream +
         "\nendstream\nendobj\n";

  PdfTestByteSource memory({pdf.begin(), pdf.end()});
  XrefHarness harness;
  harness.table.reset();
  ASSERT_TRUE(harness.table.appendNewest({1, 0, PdfXrefEntryType::Compressed, 0, 5, 0}).ok());
  ASSERT_TRUE(
      harness.table.appendNewest({5, 0, PdfXrefEntryType::Uncompressed, 0, objectStreamOffset, 0}).ok());
  const PdfObjectResolverWorkspace workspace{
      nullptr, {}, &harness, XrefHarness::setSourceAccess, PdfStreamDecodeLimits{stream.size(), 200}};
  PdfObjectResolver resolver(memory.source(), harness.table, harness.sourceBuffer.data(), harness.sourceBuffer.size(),
                             harness.arena, workspace);
  ASSERT_TRUE(resolver.begin({1, 0}).ok());

  PdfStepResult result = PdfStepResult::paused();
  uint16_t steps = 0;
  while (steps < 128U && result.yielded()) {
    PdfWorkBudget budget{1, 4096};
    result = resolver.step(budget);
    ++steps;
  }

  ASSERT_TRUE(result.complete()) << "steps=" << steps << " error=" << static_cast<int>(result.status.error) << '@'
                                 << result.status.offset;
  EXPECT_LT(steps, 128U);
  ASSERT_LT(resolver.result().rootIndex, harness.arena.valueCount);
  EXPECT_EQ(harness.arena.values[resolver.result().rootIndex].kind, PdfValueKind::Integer);
  EXPECT_EQ(harness.arena.values[resolver.result().rootIndex].integerValue, 0);
}

TEST(PdfObjectResolverTest, CooperativelyPreparesSharedWindowFlateStreamAndReusesOneCache) {
  PdfTestByteSource memory(loadFixture("xref_stream_objstm.pdf"));
  const PdfByteSource source = memory.source();
  XrefHarness harness;
  ASSERT_TRUE(parseXref(source, harness).complete());
  PdfStreamDecoder sharedDecoder({harness.sourceBuffer.data(), harness.sourceBuffer.size(),
                                  harness.decoderOutputBuffer.data(), harness.decoderOutputBuffer.size(),
                                  harness.inflateDictionary.data(), harness.inflateDictionary.size()});
  harness.decodeLimits = {201, 200};
  harness.objectStoreWriterYieldsRemaining = 3;
  PdfObjectResolverWorkspace workspace = harness.resolverWorkspace();
  workspace.streamDecoder = &sharedDecoder;
  PdfObjectResolver resolver(source, harness.table, harness.sourceBuffer.data(), harness.sourceBuffer.size(),
                             harness.arena, workspace);
  ASSERT_TRUE(resolver.begin({1, 0}).ok());

  PdfStepResult result = PdfStepResult::paused();
  uint64_t maximumCurrentBytes = 0;
  for (uint32_t step = 0; step < 65536U && result.yielded(); ++step) {
    PdfWorkBudget budget{1, 4096};
    const uint32_t operationsBefore = budget.operationsRemaining;
    const size_t bytesBefore = budget.bytesRemaining;
    result = resolver.step(budget);
    EXPECT_LE(operationsBefore - budget.operationsRemaining, 1U);
    EXPECT_LE(bytesBefore - budget.bytesRemaining, 4096U);
    maximumCurrentBytes = std::max(maximumCurrentBytes, resolver.currentStreamBytes());
    if (harness.objectStoreWriterCalls != 0 && harness.objectStoreWriterSelections == 0) {
      EXPECT_EQ(harness.objectStreamStorage.resetCount(), 0U);
    }
  }

  ASSERT_TRUE(result.complete()) << static_cast<int>(result.status.error) << "@" << result.status.offset;
  EXPECT_EQ(harness.objectStoreWriterCalls, 4U);
  EXPECT_EQ(harness.objectStoreWriterSelections, 1U);
  EXPECT_EQ(harness.objectStreamStorage.resetCount(), 1U);
  EXPECT_EQ(harness.objectStoreSelections, 1U);
  EXPECT_EQ(maximumCurrentBytes, 201U);
  EXPECT_EQ(resolver.takeCompletedStreamBytes(), 201U);
  EXPECT_EQ(resolver.takeCompletedStreamBytes(), 0U);

  ASSERT_TRUE(resolver.begin({2, 0}).ok());
  result = PdfStepResult::paused();
  for (uint32_t step = 0; step < 65536U && result.yielded(); ++step) {
    PdfWorkBudget budget{1, sizeof(PdfXrefEntry)};
    const uint32_t operationsBefore = budget.operationsRemaining;
    const size_t bytesBefore = budget.bytesRemaining;
    result = resolver.step(budget);
    EXPECT_LE(operationsBefore - budget.operationsRemaining, 1U);
    EXPECT_LE(bytesBefore - budget.bytesRemaining, sizeof(PdfXrefEntry));
  }
  ASSERT_TRUE(result.complete()) << static_cast<int>(result.status.error) << "@" << result.status.offset;
  const PdfResolvedObject pages = resolver.result();
  uint16_t kidsIndex = PDF_INVALID_INDEX;
  ASSERT_TRUE(pdfDictionaryFind(harness.arena, pages.rootIndex, "Kids", &kidsIndex));
  EXPECT_EQ(harness.objectStreamStorage.resetCount(), 1U);
  EXPECT_EQ(harness.objectStoreWriterSelections, 1U);
  EXPECT_EQ(resolver.takeCompletedStreamBytes(), 0U);
}

TEST(PdfObjectResolverTest, RejectsObjectStreamAbsoluteAndRatioLimitsBeforePublication) {
  const std::vector<uint8_t> fixture = loadFixture("xref_stream_objstm.pdf");
  ASSERT_FALSE(fixture.empty());

  for (const PdfStreamDecodeLimits limits : {PdfStreamDecodeLimits{200, 200}, PdfStreamDecodeLimits{512, 1}}) {
    SCOPED_TRACE(limits.maxExpandedBytes);
    SCOPED_TRACE(limits.maxExpansionRatio);
    PdfTestByteSource memory(fixture);
    XrefHarness harness;
    ASSERT_TRUE(parseXref(memory.source(), harness).complete());
    harness.decodeLimits = limits;
    PdfObjectResolver resolver(memory.source(), harness.table, harness.sourceBuffer.data(), harness.sourceBuffer.size(),
                               harness.arena, harness.resolverWorkspace());

    const PdfStepResult result = resolveObject(resolver, {1, 0}, nullptr);

    ASSERT_TRUE(result.failed());
    EXPECT_EQ(result.status.error, PdfError::ExpansionLimit);
    EXPECT_EQ(resolver.takeCompletedStreamBytes(), 0U);
  }
}

TEST(PdfObjectResolverTest, NeverAllowsCallerToRelaxAbsoluteTwoHundredToOneRatio) {
  const RawObjectStreamFixture fixture = highRatioObjectStreamPdf();
  PdfTestByteSource memory(fixture.pdf);
  XrefHarness harness;
  harness.table.reset();
  ASSERT_TRUE(harness.table.appendNewest({1, 0, PdfXrefEntryType::Compressed, 0, 5, 0}).ok());
  ASSERT_TRUE(
      harness.table.appendNewest({5, 0, PdfXrefEntryType::Uncompressed, 0, fixture.objectStreamOffset, 0}).ok());
  harness.decodeLimits = {PdfLimits::MaxExpandedRequiredStreamBytes, UINT16_MAX};
  PdfObjectResolver resolver(memory.source(), harness.table, harness.sourceBuffer.data(), harness.sourceBuffer.size(),
                             harness.arena, harness.resolverWorkspace());

  const PdfStepResult result = resolveObject(resolver, {1, 0}, nullptr);

  ASSERT_TRUE(result.failed());
  EXPECT_EQ(result.status.error, PdfError::ExpansionLimit);
  EXPECT_EQ(resolver.takeCompletedStreamBytes(), 0U);
}

TEST(PdfObjectResolverTest, CancellationDoesNotPublishPartialObjectStreamCache) {
  PdfTestByteSource memory(loadFixture("xref_stream_objstm.pdf"));
  const PdfByteSource source = memory.source();
  XrefHarness harness;
  ASSERT_TRUE(parseXref(source, harness).complete());
  harness.decodeLimits = {201, 200};
  PdfObjectResolver resolver(source, harness.table, harness.sourceBuffer.data(), harness.sourceBuffer.size(),
                             harness.arena, harness.resolverWorkspace());
  ASSERT_TRUE(resolver.begin({1, 0}).ok());

  PdfStepResult result = PdfStepResult::paused();
  for (uint32_t step = 0; step < 65536U && result.yielded() && harness.objectStreamStorage.bytes().empty(); ++step) {
    PdfWorkBudget budget{1, 4096};
    result = resolver.step(budget);
  }
  ASSERT_TRUE(result.yielded());
  ASSERT_FALSE(harness.objectStreamStorage.bytes().empty());
  EXPECT_EQ(resolver.takeCompletedStreamBytes(), 0U);

  bool cancel = true;
  PdfWorkBudget cancelBudget{1, 4096, &cancel, stopFromFlag};
  result = resolver.step(cancelBudget);
  ASSERT_TRUE(result.failed());
  EXPECT_EQ(result.status.error, PdfError::Cancelled);
  EXPECT_EQ(resolver.takeCompletedStreamBytes(), 0U);

  cancel = false;
  PdfResolvedObject catalog;
  ASSERT_TRUE(resolveObject(resolver, {1, 0}, &catalog).complete());
  EXPECT_EQ(harness.objectStreamStorage.resetCount(), 2U);
  EXPECT_EQ(resolver.takeCompletedStreamBytes(), 201U);
}

TEST(PdfObjectResolverTest, SingleReaderGuardRejectsStoreReadUntilSourceBoundaryCloses) {
  bool sourceActive = true;
  PdfTestByteStore storage(16);
  storage.forbidReadsWhile(&sourceActive);
  PdfByteStore store = storage.store();
  ASSERT_TRUE(store.reset(store.context).ok());
  const std::array<uint8_t, 3> expected{1, 2, 3};
  ASSERT_TRUE(pdfWriteExact(pdfByteStoreSink(store), expected.data(), expected.size()).ok());
  PdfByteSource source = pdfByteStoreSource(store);
  std::array<uint8_t, 3> output{};

  EXPECT_EQ(pdfReadExact(source, 0, output.data(), output.size()).error, PdfError::IoFailure);
  sourceActive = false;
  ASSERT_TRUE(pdfReadExact(source, 0, output.data(), output.size()).ok());
  EXPECT_EQ(output, expected);
}

TEST(PdfObjectResolverTest, RejectsCompressedObjectStreamRecursionAndCycle) {
  PdfTestByteSource memory({1, 2, 3, 4});
  XrefHarness harness;
  harness.table.reset();
  ASSERT_TRUE(harness.table.appendNewest({7, 0, PdfXrefEntryType::Compressed, 0, 7, 0}).ok());
  PdfObjectResolver cycle(memory.source(), harness.table, harness.sourceBuffer.data(), harness.sourceBuffer.size(),
                          harness.arena, harness.resolverWorkspace());
  const PdfStepResult cycleResult = resolveObject(cycle, {7, 0}, nullptr);
  ASSERT_TRUE(cycleResult.failed());
  EXPECT_EQ(cycleResult.status.error, PdfError::Malformed);

  harness.table.reset();
  ASSERT_TRUE(harness.table.appendNewest({7, 0, PdfXrefEntryType::Compressed, 0, 8, 0}).ok());
  ASSERT_TRUE(harness.table.appendNewest({8, 0, PdfXrefEntryType::Compressed, 0, 9, 0}).ok());
  PdfObjectResolver recursive(memory.source(), harness.table, harness.sourceBuffer.data(), harness.sourceBuffer.size(),
                              harness.arena, harness.resolverWorkspace());
  const PdfStepResult recursiveResult = resolveObject(recursive, {7, 0}, nullptr);
  ASSERT_TRUE(recursiveResult.failed());
  EXPECT_EQ(recursiveResult.status.error, PdfError::Malformed);
}

TEST(PdfObjectResolverTest, RejectsBadOffsetBeforeReading) {
  PdfTestByteSource memory({1, 2, 3, 4});
  memory.setFailureOffset(0);
  const PdfByteSource source = memory.source();
  XrefHarness harness;
  harness.table.reset();
  ASSERT_TRUE(harness.table.appendNewest({7, 0, PdfXrefEntryType::Uncompressed, 0, source.size, 0}).ok());
  PdfObjectResolver resolver(source, harness.table, harness.sourceBuffer.data(), harness.sourceBuffer.size(),
                             harness.arena);

  const PdfStepResult result = resolveObject(resolver, {7, 0}, nullptr);

  ASSERT_TRUE(result.failed());
  EXPECT_EQ(result.status.error, PdfError::InvalidOffset);
  EXPECT_EQ(result.status.offset, source.size);
}
