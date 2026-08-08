#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "PdfPreparation.h"
#include "PdfCMap.h"
#include "PdfPreparedContent.h"
#include "PdfTestCacheIo.h"

namespace {

constexpr uint32_t kAdlerModulus = 65521U;
constexpr size_t kDenseCidChunkBytes = 100U;
constexpr size_t kDenseCidChunkCount = 82U;
constexpr size_t kDenseCidPaintedBytes = kDenseCidChunkBytes * kDenseCidChunkCount;
constexpr size_t kObservedJournalCapacity = 8192U - (16U * 32U + 16U + 2U + 2U);
constexpr size_t kMaximumObservedRecordBytes = 2U + sizeof(PdfToken::bytes);
constexpr size_t kSixteenFontCount = 16U;
constexpr size_t kSixteenFontRecordsPerFont = 5U;
constexpr size_t kSixteenFontJournalBytes =
    kSixteenFontCount * kSixteenFontRecordsPerFont * kMaximumObservedRecordBytes;

size_t countOccurrences(const std::string_view text, const std::string_view needle) {
  if (needle.empty()) {
    return 0;
  }
  size_t count = 0;
  size_t offset = 0;
  while ((offset = text.find(needle, offset)) != std::string_view::npos) {
    ++count;
    offset += needle.size();
  }
  return count;
}

std::string zlibStored(const std::string_view input) {
  if (input.size() > UINT16_MAX) {
    return {};
  }
  const uint16_t length = static_cast<uint16_t>(input.size());
  const uint16_t complement = static_cast<uint16_t>(~length);
  std::string encoded;
  encoded.reserve(input.size() + 11U);
  encoded.push_back(static_cast<char>(0x78));
  encoded.push_back(static_cast<char>(0x01));
  encoded.push_back(static_cast<char>(0x01));
  encoded.push_back(static_cast<char>(length & 0xFFU));
  encoded.push_back(static_cast<char>(length >> 8U));
  encoded.push_back(static_cast<char>(complement & 0xFFU));
  encoded.push_back(static_cast<char>(complement >> 8U));
  encoded.append(input);

  uint32_t first = 1;
  uint32_t second = 0;
  for (const unsigned char byte : input) {
    first = (first + byte) % kAdlerModulus;
    second = (second + first) % kAdlerModulus;
  }
  const uint32_t checksum = (second << 16U) | first;
  encoded.push_back(static_cast<char>(checksum >> 24U));
  encoded.push_back(static_cast<char>(checksum >> 16U));
  encoded.push_back(static_cast<char>(checksum >> 8U));
  encoded.push_back(static_cast<char>(checksum));
  return encoded;
}

std::string streamObject(const std::string_view bytes, const std::string_view dictionary = {}) {
  std::string object = "<< /Length " + std::to_string(bytes.size());
  if (!dictionary.empty()) {
    object.push_back(' ');
    object.append(dictionary);
  }
  object += " >>\nstream\n";
  object.append(bytes);
  object += "\nendstream";
  return object;
}

std::vector<uint8_t> classicPdf(const std::vector<std::string>& objects) {
  std::string pdf = "%PDF-1.4\n";
  std::vector<size_t> offsets;
  offsets.reserve(objects.size());
  for (size_t index = 0; index < objects.size(); ++index) {
    offsets.push_back(pdf.size());
    pdf += std::to_string(index + 1U) + " 0 obj\n";
    pdf.append(objects[index]);
    pdf += "\nendobj\n";
  }

  const size_t xrefOffset = pdf.size();
  pdf += "xref\n0 " + std::to_string(objects.size() + 1U) + "\n0000000000 65535 f \n";
  char entry[32]{};
  for (const size_t offset : offsets) {
    const int length = std::snprintf(entry, sizeof(entry), "%010zu 00000 n \n", offset);
    if (length <= 0 || static_cast<size_t>(length) >= sizeof(entry)) {
      return {};
    }
    pdf.append(entry, static_cast<size_t>(length));
  }
  pdf += "trailer\n<< /Size " + std::to_string(objects.size() + 1U) +
         " /Root 1 0 R >>\nstartxref\n" + std::to_string(xrefOffset) + "\n%%EOF\n";
  return {pdf.begin(), pdf.end()};
}

std::vector<uint8_t> actualTextAndToUnicodePdf() {
  static constexpr std::string_view cmap =
      "/CIDInit /ProcSet findresource begin\n"
      "12 dict begin\n"
      "begincmap\n"
      "1 begincodespacerange\n"
      "<00> <FF>\n"
      "endcodespacerange\n"
      "2 beginbfchar\n"
      "<41> <03A9>\n"
      "<42> <0042>\n"
      "endbfchar\n"
      "endcmap\n"
      "end\n"
      "end";
  static constexpr std::string_view content =
      "BT /F1 12 Tf 1 0 0 1 72 720 Tm "
      "<41> Tj "
      "0 -30 Td /Span << /ActualText <FEFF005200650070006C006100630065006D0065006E0074> >> BDC "
      "<42> Tj EMC ET";
  const std::string encodedCmap = zlibStored(cmap);
  return classicPdf({
      "<< /Type /Catalog /Pages 2 0 R >>",
      "<< /Type /Pages /Count 1 /Kids [3 0 R] >>",
      "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] "
      "/Resources << /Font << /F1 5 0 R >> >> /Contents 4 0 R >>",
      streamObject(content),
      "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica /ToUnicode 6 0 R >>",
      streamObject(encodedCmap, "/Filter /FlateDecode"),
  });
}

std::vector<uint8_t> wideCodeSpaceSimpleFontPdf() {
  static constexpr std::string_view cmap =
      "/CIDInit /ProcSet findresource begin\n"
      "12 dict begin\n"
      "begincmap\n"
      "1 begincodespacerange\n"
      "<0000> <FFFF>\n"
      "endcodespacerange\n"
      "7 beginbfchar\n"
      "<43> <0043>\n"
      "<61> <0061>\n"
      "<65> <0065>\n"
      "<6F> <006F>\n"
      "<70> <0070>\n"
      "<72> <0072>\n"
      "<74> <0074>\n"
      "endbfchar\n"
      "endcmap\n"
      "end\n"
      "end";
  static constexpr std::string_view content =
      "BT /F1 12 Tf 1 0 0 1 72 720 Tm (Corporate) Tj ET";
  return classicPdf({
      "<< /Type /Catalog /Pages 2 0 R >>",
      "<< /Type /Pages /Count 1 /Kids [3 0 R] >>",
      "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] "
      "/Resources << /Font << /F1 5 0 R >> >> /Contents 4 0 R >>",
      streamObject(content),
      "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica /ToUnicode 6 0 R >>",
      streamObject(cmap),
  });
}

std::vector<uint8_t> defaultType1EncodingPdf() {
  static constexpr std::string_view content =
      "/Artifact << /Payload <4E6F74207061696E746564> >> DP "
      "BT /F1 12 Tf 1 0 0 1 72 720 Tm <48656C6C6F> Tj ET";
  return classicPdf({
      "<< /Type /Catalog /Pages 2 0 R >>",
      "<< /Type /Pages /Count 1 /Kids [3 0 R] >>",
      "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] "
      "/Resources << /Font << /F1 5 0 R >> >> /Contents 4 0 R >>",
      streamObject(content),
      "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>",
  });
}

std::vector<uint8_t> unobservedToUnicodePdf() {
  static constexpr std::string_view content =
      "BT /F1 12 Tf 1 0 0 1 72 720 Tm <48656C6C6F> Tj ET";
  static constexpr std::string_view malformedCmap = "this-is-not-a-zlib-stream";
  return classicPdf({
      "<< /Type /Catalog /Pages 2 0 R >>",
      "<< /Type /Pages /Count 1 /Kids [3 0 R] >>",
      "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] "
      "/Resources << /Font << /F1 5 0 R /F2 6 0 R >> >> /Contents 4 0 R >>",
      streamObject(content),
      "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>",
      "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica /ToUnicode 7 0 R >>",
      streamObject(malformedCmap, "/Filter /FlateDecode"),
  });
}

std::vector<uint8_t> denseCidWithUnobservedMalformedFontPdf(const bool appendEmptyStrings = false) {
  static constexpr std::string_view cmap =
      "/CIDInit /ProcSet findresource begin\n"
      "12 dict begin\n"
      "begincmap\n"
      "1 begincodespacerange\n"
      "<0000> <FFFF>\n"
      "endcodespacerange\n"
      "1 beginbfchar\n"
      "<0041> <0041>\n"
      "endbfchar\n"
      "endcmap\n"
      "end\n"
      "end";
  static constexpr std::string_view malformedCmap = "this-is-not-a-zlib-stream";

  std::string encodedGlyphs;
  encodedGlyphs.reserve(kDenseCidChunkBytes * 2U);
  for (size_t index = 0; index < kDenseCidChunkBytes / 2U; ++index) {
    encodedGlyphs += "0041";
  }

  std::string content;
  for (size_t index = 0; index < kDenseCidChunkCount; ++index) {
    char replacement[16]{};
    const int replacementLength =
        std::snprintf(replacement, sizeof(replacement), "Dense%02u", static_cast<unsigned>(index));
    if (replacementLength <= 0 || static_cast<size_t>(replacementLength) >= sizeof(replacement)) {
      return {};
    }
    content += "/Span << /ActualText (" + std::string(replacement, static_cast<size_t>(replacementLength)) +
               ") >> BDC BT /F1 12 Tf 1 0 0 1 72 " + std::to_string(760U - index * 8U) + " Tm <" +
                encodedGlyphs + "> Tj ET EMC\n";
  }
  if (appendEmptyStrings) {
    content += "BT /F1 12 Tf () Tj <> Tj ET\n";
  }

  return classicPdf({
      "<< /Type /Catalog /Pages 2 0 R >>",
      "<< /Type /Pages /Count 1 /Kids [3 0 R] >>",
      "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] "
      "/Resources << /Font << /F1 5 0 R /F2 8 0 R >> >> /Contents 4 0 R >>",
      streamObject(content),
      "<< /Type /Font /Subtype /Type0 /BaseFont /DenseCID /Encoding /Identity-H "
      "/DescendantFonts [7 0 R] /ToUnicode 6 0 R >>",
      streamObject(cmap),
      "<< /Type /Font /Subtype /CIDFontType2 /BaseFont /DenseCID "
      "/CIDSystemInfo << /Registry (Fixture) /Ordering (Identity) /Supplement 0 >> /DW 1000 >>",
      "<< /Type /Font /Subtype /Type0 /BaseFont /UnusedCID /Encoding /Identity-H "
      "/DescendantFonts [10 0 R] /ToUnicode 9 0 R >>",
      streamObject(malformedCmap, "/Filter /FlateDecode"),
      "<< /Type /Font /Subtype /CIDFontType2 /BaseFont /UnusedCID "
      "/CIDSystemInfo << /Registry (Fixture) /Ordering (Identity) /Supplement 0 >> /DW 1000 >>",
  });
}

std::vector<uint8_t> cidUniqueGlyphPdf(const size_t glyphCount, const bool addUnpaintedAlias = false) {
  if (glyphCount == 0 || glyphCount > 257U) {
    return {};
  }
  const uint32_t lastCode = static_cast<uint32_t>(glyphCount - 1U);
  char lastCodeHex[8]{};
  const int lastCodeLength = std::snprintf(lastCodeHex, sizeof(lastCodeHex), "%04X", lastCode);
  if (lastCodeLength != 4) {
    return {};
  }
  const std::string cmap =
      "/CIDInit /ProcSet findresource begin\n"
      "12 dict begin\n"
      "begincmap\n"
      "1 begincodespacerange\n"
      "<0000> <FFFF>\n"
      "endcodespacerange\n"
      "1 beginbfrange\n"
      "<0000> <" + std::string(lastCodeHex) + "> <0100>\n"
      "endbfrange\n"
      "endcmap\n"
      "end\n"
      "end";

  std::string content = "BT /F1 12 Tf 1 0 0 1 72 720 Tm ";
  for (size_t start = 0; start < glyphCount; start += 50U) {
    const size_t count = std::min<size_t>(50U, glyphCount - start);
    content.push_back('<');
    for (size_t index = 0; index < count; ++index) {
      char code[8]{};
      const int codeLength =
          std::snprintf(code, sizeof(code), "%04X", static_cast<unsigned>(start + index));
      if (codeLength != 4) {
        return {};
      }
      content.append(code, static_cast<size_t>(codeLength));
    }
    content += "> Tj ";
  }
  if (addUnpaintedAlias) {
    content += "/F2 12 Tf ";
  }
  content += "ET";

  std::string resources = "/Resources << /Font << /F1 5 0 R";
  if (addUnpaintedAlias) {
    resources += " /F2 8 0 R";
  }
  resources += " >> >>";

  std::vector<std::string> objects{
      "<< /Type /Catalog /Pages 2 0 R >>",
      "<< /Type /Pages /Count 1 /Kids [3 0 R] >>",
      "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] " + resources + " /Contents 4 0 R >>",
      streamObject(content),
      "<< /Type /Font /Subtype /Type0 /BaseFont /UniqueCID /Encoding /Identity-H "
      "/DescendantFonts [7 0 R] /ToUnicode 6 0 R >>",
      streamObject(cmap),
      "<< /Type /Font /Subtype /CIDFontType2 /BaseFont /UniqueCID "
      "/CIDSystemInfo << /Registry (Fixture) /Ordering (Identity) /Supplement 0 >> /DW 1000 >>",
  };
  if (addUnpaintedAlias) {
    objects.emplace_back("<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>");
  }
  return classicPdf(objects);
}

std::vector<uint8_t> sixteenPaintedFontsSpilledJournalPdf() {
  std::string resources = "/Resources << /Font <<";
  std::vector<std::string> objects{
      "<< /Type /Catalog /Pages 2 0 R >>",
      "<< /Type /Pages /Count 1 /Kids [3 0 R] >>",
      {},
      {},
  };
  objects.reserve(4U + kSixteenFontCount);
  for (size_t font = 0; font < kSixteenFontCount; ++font) {
    char name[8]{};
    const int nameLength = std::snprintf(name, sizeof(name), "F%02u", static_cast<unsigned>(font));
    if (nameLength != 3) {
      return {};
    }
    resources += " /" + std::string(name, static_cast<size_t>(nameLength)) + " " +
                 std::to_string(5U + font) + " 0 R";
    objects.emplace_back("<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>");
  }
  resources += " >> >>";

  std::string content;
  for (size_t font = 0; font < kSixteenFontCount; ++font) {
    char name[8]{};
    const int nameLength = std::snprintf(name, sizeof(name), "F%02u", static_cast<unsigned>(font));
    if (nameLength != 3) {
      return {};
    }
    for (size_t record = 0; record < kSixteenFontRecordsPerFont; ++record) {
      content += "/Span << /ActualText (A) >> BDC BT /" +
                 std::string(name, static_cast<size_t>(nameLength)) + " 12 Tf 1 0 0 1 72 720 Tm <";
      for (size_t byte = 0; byte < sizeof(PdfToken::bytes); ++byte) {
        content += "41";
      }
      content += "> Tj ET EMC\n";
    }
  }
  objects[2] = "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] " + resources +
               " /Contents 4 0 R >>";
  objects[3] = streamObject(content);
  return classicPdf(objects);
}

std::vector<uint8_t> largeSpilledCMapSingleTokenPdf() {
  constexpr size_t glyphCount = sizeof(PdfToken::bytes);
  std::string cmap =
      "/CIDInit /ProcSet findresource begin\n"
      "12 dict begin\n"
      "begincmap\n"
      "1 begincodespacerange\n"
      "<00> <FF>\n"
      "endcodespacerange\n" +
      std::to_string(glyphCount) + " beginbfchar\n";
  std::string encoded;
  encoded.reserve(glyphCount * 2U);
  for (size_t code = 0; code < glyphCount; ++code) {
    char source[8]{};
    char destination[8]{};
    if (std::snprintf(source, sizeof(source), "%02X", static_cast<unsigned>(code)) != 2 ||
        std::snprintf(destination, sizeof(destination), "%04X",
                      static_cast<unsigned>(0x0100U + code)) != 4) {
      return {};
    }
    cmap += "<" + std::string(source) + "> <" + std::string(destination) + ">\n";
    encoded += source;
  }
  cmap +=
      "endbfchar\n"
      "endcmap\n"
      "end\n"
      "end";
  const std::string content =
      "/Span << /ActualText (Bounded) >> BDC BT /F1 12 Tf 1 0 0 1 72 720 Tm <" + encoded +
      "> Tj ET EMC";
  return classicPdf({
      "<< /Type /Catalog /Pages 2 0 R >>",
      "<< /Type /Pages /Count 1 /Kids [3 0 R] >>",
      "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] "
      "/Resources << /Font << /F1 5 0 R >> >> /Contents 4 0 R >>",
      streamObject(content),
      "<< /Type /Font /Subtype /Type0 /BaseFont /LargeCID /Encoding /Identity-H "
      "/DescendantFonts [7 0 R] /ToUnicode 6 0 R >>",
      streamObject(cmap),
      "<< /Type /Font /Subtype /CIDFontType2 /BaseFont /LargeCID "
      "/CIDSystemInfo << /Registry (Fixture) /Ordering (Identity) /Supplement 0 >> /DW 1000 >>",
  });
}

std::vector<uint8_t> maximumSortedCMapPdf() {
  constexpr size_t mappingCount = 8192U;
  constexpr size_t paintedGlyphCount = sizeof(PdfToken::bytes) / 2U;
  std::string cmap =
      "/CIDInit /ProcSet findresource begin\n"
      "12 dict begin\n"
      "begincmap\n"
      "1 begincodespacerange\n"
      "<0000> <FFFF>\n"
      "endcodespacerange\n" +
      std::to_string(mappingCount) + " beginbfchar\n";
  std::string encoded;
  encoded.reserve(paintedGlyphCount * 4U);
  for (size_t code = 0; code < mappingCount; ++code) {
    char source[8]{};
    char destination[8]{};
    if (std::snprintf(source, sizeof(source), "%04X", static_cast<unsigned>(code)) != 4 ||
        std::snprintf(destination, sizeof(destination), "%04X",
                      static_cast<unsigned>(0x2000U + code)) != 4) {
      return {};
    }
    cmap += "<" + std::string(source) + "> <" + std::string(destination) + ">\n";
    if (code < paintedGlyphCount) {
      encoded += source;
    }
  }
  cmap +=
      "endbfchar\n"
      "endcmap\n"
      "end\n"
      "end\n ";
  const std::string content =
      "/Span << /ActualText (Maximum) >> BDC BT /F1 12 Tf 1 0 0 1 72 720 Tm <" + encoded +
      "> Tj ET EMC";
  return classicPdf({
      "<< /Type /Catalog /Pages 2 0 R >>",
      "<< /Type /Pages /Count 1 /Kids [3 0 R] >>",
      "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] "
      "/Resources << /Font << /F1 5 0 R >> >> /Contents 4 0 R >>",
      streamObject(content),
      "<< /Type /Font /Subtype /Type0 /BaseFont /MaximumCID /Encoding /Identity-H "
      "/DescendantFonts [7 0 R] /ToUnicode 6 0 R >>",
      streamObject(cmap),
      "<< /Type /Font /Subtype /CIDFontType2 /BaseFont /MaximumCID "
      "/CIDSystemInfo << /Registry (Fixture) /Ordering (Identity) /Supplement 0 >> /DW 1000 >>",
  });
}

std::vector<uint8_t> duplicateCMapMappingPdf() {
  static constexpr std::string_view cmap =
      "1 begincodespacerange\n"
      "<00> <FF>\n"
      "endcodespacerange\n"
      "2 beginbfchar\n"
      "<41> <0041>\n"
      "<41> <0042>\n"
      "endbfchar\n";
  return classicPdf({
      "<< /Type /Catalog /Pages 2 0 R >>",
      "<< /Type /Pages /Count 1 /Kids [3 0 R] >>",
      "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] "
      "/Resources << /Font << /F1 5 0 R >> >> /Contents 4 0 R >>",
      streamObject("BT /F1 12 Tf 1 0 0 1 72 720 Tm <41> Tj ET"),
      "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica /ToUnicode 6 0 R >>",
      streamObject(cmap),
  });
}

std::vector<uint8_t> missingCMapMappingPdf() {
  static constexpr std::string_view cmap =
      "1 begincodespacerange\n"
      "<00> <FF>\n"
      "endcodespacerange\n"
      "1 beginbfchar\n"
      "<41> <0041>\n"
      "endbfchar\n";
  return classicPdf({
      "<< /Type /Catalog /Pages 2 0 R >>",
      "<< /Type /Pages /Count 1 /Kids [3 0 R] >>",
      "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] "
      "/Resources << /Font << /F1 5 0 R >> >> /Contents 4 0 R >>",
      streamObject("BT /F1 12 Tf 1 0 0 1 72 720 Tm <42> Tj ET"),
      "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica /ToUnicode 6 0 R >>",
      streamObject(cmap),
  });
}

void appendPaintedHexRecord(std::string* const content, const size_t payloadBytes) {
  ASSERT_NE(content, nullptr);
  ASSERT_GE(payloadBytes, 1U);
  ASSERT_LE(payloadBytes, sizeof(PdfToken::bytes));
  content->append("/Span << /ActualText (A) >> BDC BT /F1 12 Tf 1 0 0 1 72 720 Tm <");
  for (size_t index = 0; index < payloadBytes; ++index) {
    content->append("41");
  }
  content->append("> Tj ET EMC ");
}

std::vector<uint8_t> spillBoundaryPdf(const size_t bytesRemainingBeforeOverflow) {
  if (bytesRemainingBeforeOverflow >= kMaximumObservedRecordBytes) {
    return {};
  }
  size_t targetRecordBytes = kObservedJournalCapacity - bytesRemainingBeforeOverflow;
  std::vector<size_t> recordBytes;
  std::string content;
  while (targetRecordBytes > kMaximumObservedRecordBytes) {
    recordBytes.push_back(kMaximumObservedRecordBytes);
    targetRecordBytes -= kMaximumObservedRecordBytes;
  }
  if (targetRecordBytes != 0 && targetRecordBytes < 3U) {
    if (recordBytes.empty()) {
      return {};
    }
    recordBytes.back() -= 3U - targetRecordBytes;
    targetRecordBytes = 3U;
  }
  if (targetRecordBytes != 0) {
    recordBytes.push_back(targetRecordBytes);
  }
  for (const size_t bytes : recordBytes) {
    appendPaintedHexRecord(&content, bytes - 2U);
  }
  appendPaintedHexRecord(&content, sizeof(PdfToken::bytes));

  return classicPdf({
      "<< /Type /Catalog /Pages 2 0 R >>",
      "<< /Type /Pages /Count 1 /Kids [3 0 R] >>",
      "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] "
      "/Resources << /Font << /F1 5 0 R >> >> /Contents 4 0 R >>",
      streamObject(content),
      "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>",
  });
}

std::vector<uint8_t> rolledBackObservedStringsPdf() {
  std::string content = "BT /F1 12 Tf 1 0 0 1 72 720 Tm ";
  for (size_t group = 0; group < 8U; ++group) {
    for (size_t operand = 0; operand < 16U; ++operand) {
      content += "<";
      for (size_t index = 0; index < 48U; ++index) {
        content += "42";
      }
      content += "> ";
    }
    content += "n ";
  }
  content += "<41> Tj ET";
  return classicPdf({
      "<< /Type /Catalog /Pages 2 0 R >>",
      "<< /Type /Pages /Count 1 /Kids [3 0 R] >>",
      "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] "
      "/Resources << /Font << /F1 5 0 R >> >> /Contents 4 0 R >>",
      streamObject(content),
      "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>",
  });
}

std::vector<uint8_t> emptyPaintedStringsPdf() {
  return classicPdf({
      "<< /Type /Catalog /Pages 2 0 R >>",
      "<< /Type /Pages /Count 1 /Kids [3 0 R] >>",
      "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] "
      "/Resources << /Font << /F1 5 0 R >> >> /Contents 4 0 R >>",
      streamObject("BT /F1 12 Tf 1 0 0 1 72 720 Tm () Tj <> Tj (Visible) Tj ET"),
      "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>",
  });
}

std::vector<uint8_t> splitCodeSpaceBlocksPdf() {
  static constexpr std::string_view cmap =
      "/CIDInit /ProcSet findresource begin\n"
      "12 dict begin\n"
      "begincmap\n"
      "1 begincodespacerange\n"
      "<00> <7F>\n"
      "endcodespacerange\n"
      "1 beginbfchar\n"
      "<41> <0041>\n"
      "endbfchar\n"
      "1 begincodespacerange\n"
      "<8100> <81FF>\n"
      "endcodespacerange\n"
      "1 beginbfchar\n"
      "<8142> <03A9>\n"
      "endbfchar\n"
      "endcmap\n"
      "end\n"
      "end";
  return classicPdf({
      "<< /Type /Catalog /Pages 2 0 R >>",
      "<< /Type /Pages /Count 1 /Kids [3 0 R] >>",
      "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] "
      "/Resources << /Font << /F1 5 0 R >> >> /Contents 4 0 R >>",
      streamObject("BT /F1 12 Tf 1 0 0 1 72 720 Tm <41> Tj <8142> Tj ET"),
      "<< /Type /Font /Subtype /Type0 /BaseFont /SplitCodes /Encoding /Identity-H "
      "/DescendantFonts [7 0 R] /ToUnicode 6 0 R >>",
      streamObject(cmap),
      "<< /Type /Font /Subtype /CIDFontType2 /BaseFont /SplitCodes "
      "/CIDSystemInfo << /Registry (Fixture) /Ordering (Identity) /Supplement 0 >> /DW 1000 >>",
  });
}

std::string singleByteToUnicodeCMap(const std::string_view destination) {
  return "/CIDInit /ProcSet findresource begin\n"
         "12 dict begin\n"
         "begincmap\n"
         "1 begincodespacerange\n"
         "<00> <FF>\n"
         "endcodespacerange\n"
         "1 beginbfchar\n"
         "<41> <" +
         std::string(destination) +
         ">\n"
         "endbfchar\n"
         "endcmap\n"
         "end\n"
         "end";
}

std::vector<uint8_t> formResourceScopePdf() {
  static constexpr std::string_view pageContent =
      "BT /F1 12 Tf 1 0 0 1 72 700 Tm <41> Tj ET "
      "q 1 0 0 1 0 500 cm /Shadow Do Q "
      "q 1 0 0 1 0 350 cm /Inherited Do Q "
      "BT /F1 12 Tf 1 0 0 1 72 250 Tm <41> Tj ET";
  static constexpr std::string_view formContent =
      "BT /F1 12 Tf 1 0 0 1 72 20 Tm <41> Tj ET";
  return classicPdf({
      "<< /Type /Catalog /Pages 2 0 R >>",
      "<< /Type /Pages /Count 1 /Kids [3 0 R] >>",
      "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] "
      "/Resources << /Font << /F1 5 0 R >> "
      "/XObject << /Shadow 7 0 R /Inherited 10 0 R >> >> /Contents 4 0 R >>",
      streamObject(pageContent),
      "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica /ToUnicode 6 0 R >>",
      streamObject(singleByteToUnicodeCMap("03A9")),
      streamObject(formContent,
                   "/Type /XObject /Subtype /Form /FormType 1 /BBox [0 0 200 100] "
                   "/Resources << /Font << /F1 8 0 R >> >>"),
      "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica /ToUnicode 9 0 R >>",
      streamObject(singleByteToUnicodeCMap("03C0")),
      streamObject(formContent, "/Type /XObject /Subtype /Form /FormType 1 /BBox [0 0 200 100]"),
  });
}

std::vector<uint8_t> nestedFormPdf() {
  return classicPdf({
      "<< /Type /Catalog /Pages 2 0 R >>",
      "<< /Type /Pages /Count 1 /Kids [3 0 R] >>",
      "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] "
      "/Resources << /Font << /F1 7 0 R >> /XObject << /Outer 5 0 R >> >> /Contents 4 0 R >>",
      streamObject("/Outer Do"),
      streamObject("/Inner Do",
                   "/Type /XObject /Subtype /Form /FormType 1 /BBox [0 0 612 792] "
                   "/Resources << /Font << /F1 7 0 R >> /XObject << /Inner 6 0 R >> >>"),
      streamObject("BT /F1 12 Tf 1 0 0 1 72 700 Tm (NestedTarget) Tj ET",
                   "/Type /XObject /Subtype /Form /FormType 1 /BBox [0 0 612 792] "
                   "/Resources << /Font << /F1 7 0 R >> >>"),
      "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>",
  });
}

std::vector<uint8_t> formMatrixReadingOrderPdf() {
  static constexpr std::string_view content =
      "BT /F1 12 Tf 1 0 0 1 72 700 Tm (TopMarker) Tj ET "
      "BT /F1 12 Tf 1 0 0 1 72 100 Tm (BottomMarker) Tj ET "
      "/Placed Do";
  return classicPdf({
      "<< /Type /Catalog /Pages 2 0 R >>",
      "<< /Type /Pages /Count 1 /Kids [3 0 R] >>",
      "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] "
      "/Resources << /Font << /F1 6 0 R >> /XObject << /Placed 5 0 R >> >> /Contents 4 0 R >>",
      streamObject(content),
      streamObject("BT /F1 12 Tf 1 0 0 1 36 50 Tm (MatrixMarker) Tj ET",
                   "/Type /XObject /Subtype /Form /FormType 1 /BBox [0 0 200 200] "
                   "/Matrix [2 0 0 2 0 200]"),
      "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>",
  });
}

struct PdfWithTargetSpan {
  std::vector<uint8_t> bytes;
  uint64_t targetOffset = UINT64_MAX;
  uint64_t targetLength = 0;

  bool valid() const {
    return !bytes.empty() && targetOffset != UINT64_MAX && targetLength != 0 &&
           targetOffset + targetLength <= bytes.size();
  }
};

PdfWithTargetSpan locateTargetSpan(std::vector<uint8_t> pdf, const std::string_view marker,
                                   const size_t targetLength) {
  if (pdf.empty() || marker.empty()) {
    return {std::move(pdf)};
  }
  const std::string_view source(reinterpret_cast<const char*>(pdf.data()), pdf.size());
  const size_t offset = source.find(marker);
  if (offset == std::string_view::npos || targetLength > pdf.size() - offset) {
    return {std::move(pdf)};
  }
  return {std::move(pdf), static_cast<uint64_t>(offset), static_cast<uint64_t>(targetLength)};
}

PdfWithTargetSpan malformedFormReadPdf(const bool invokeMalformed) {
  static constexpr std::string_view marker = "%DEAD_FORM_TARGET_SPAN\n";
  const std::string deadBody = std::string(marker) + "BT /F1 12 Tf (";
  const std::string pageContent = std::string("/Live Do ") + (invokeMalformed ? "/Dead Do" : "");
  std::vector<uint8_t> pdf = classicPdf({
      "<< /Type /Catalog /Pages 2 0 R >>",
      "<< /Type /Pages /Count 1 /Kids [3 0 R] >>",
      "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] "
      "/Resources << /Font << /F1 7 0 R >> /XObject << /Dead 5 0 R /Live 6 0 R >> >> /Contents 4 0 R >>",
      streamObject(pageContent),
      streamObject(deadBody, "/Type /XObject /Subtype /Form /FormType 1 /BBox [0 0 612 792]"),
      streamObject("BT /F1 12 Tf 1 0 0 1 72 700 Tm (LiveForm) Tj ET",
                   "/Type /XObject /Subtype /Form /FormType 1 /BBox [0 0 612 792]"),
      "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>",
  });
  return locateTargetSpan(std::move(pdf), marker, deadBody.size());
}

PdfWithTargetSpan repeatedFormReadPdf() {
  static constexpr std::string_view marker = "%FORM_BODY_DECODE_ONCE\n";
  const std::string formBody = std::string(marker) +
                               "BT /F1 12 Tf 1 0 0 1 72 20 Tm (DecodedOnce) Tj ET";
  std::vector<uint8_t> pdf = classicPdf({
      "<< /Type /Catalog /Pages 2 0 R >>",
      "<< /Type /Pages /Count 1 /Kids [3 0 R] >>",
      "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] "
      "/Resources << /Font << /F1 6 0 R >> /XObject << /Repeated 5 0 R >> >> /Contents 4 0 R >>",
      streamObject("q 1 0 0 1 0 600 cm /Repeated Do Q q 1 0 0 1 0 350 cm /Repeated Do Q"),
      streamObject(formBody, "/Type /XObject /Subtype /Form /FormType 1 /BBox [0 0 300 100]"),
      "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>",
  });
  return locateTargetSpan(std::move(pdf), marker, formBody.size());
}

std::vector<uint8_t> cyclicFormPdf() {
  return classicPdf({
      "<< /Type /Catalog /Pages 2 0 R >>",
      "<< /Type /Pages /Count 1 /Kids [3 0 R] >>",
      "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] "
      "/Resources << /XObject << /Loop 5 0 R >> >> /Contents 4 0 R >>",
      streamObject("/Loop Do"),
      streamObject("/Loop Do", "/Type /XObject /Subtype /Form /FormType 1 /BBox [0 0 612 792]"),
  });
}

std::vector<uint8_t> nestedFormDepthPdf(const size_t depth) {
  if (depth == 0 || depth > UINT8_MAX) {
    return {};
  }
  const size_t firstFormObject = 5U;
  const size_t fontObject = firstFormObject + depth;
  std::vector<std::string> objects(5U + depth);
  objects[0] = "<< /Type /Catalog /Pages 2 0 R >>";
  objects[1] = "<< /Type /Pages /Count 1 /Kids [3 0 R] >>";
  objects[2] = "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] "
               "/Resources << /Font << /F1 " +
               std::to_string(fontObject) +
               " 0 R >> /XObject << /First 5 0 R >> >> /Contents 4 0 R >>";
  objects[3] = streamObject("BT /F1 12 Tf 1 0 0 1 72 740 Tm (RootText) Tj ET /First Do");
  for (size_t index = 0; index < depth; ++index) {
    std::string body;
    std::string resources = "/Resources << /Font << /F1 " + std::to_string(fontObject) + " 0 R >>";
    if (index + 1U < depth) {
      body = "/Next Do";
      resources += " /XObject << /Next " + std::to_string(firstFormObject + index + 1U) + " 0 R >>";
    } else {
      body = "BT /F1 12 Tf 1 0 0 1 72 700 Tm (DepthTarget) Tj ET";
    }
    resources += " >>";
    objects[4U + index] =
        streamObject(body, "/Type /XObject /Subtype /Form /FormType 1 /BBox [0 0 612 792] " + resources);
  }
  objects[4U + depth] = "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>";
  return classicPdf(objects);
}

std::vector<uint8_t> formResourceCapPdf(const size_t count) {
  if (count == 0 || count > 99U) {
    return {};
  }
  const size_t firstFormObject = 5U;
  const size_t fontObject = firstFormObject + count;
  std::vector<std::string> objects(5U + count);
  objects[0] = "<< /Type /Catalog /Pages 2 0 R >>";
  objects[1] = "<< /Type /Pages /Count 1 /Kids [3 0 R] >>";
  std::string xobjects = "/XObject <<";
  std::string pageContent;
  for (size_t index = 0; index < count; ++index) {
    char name[8]{};
    const int nameLength = std::snprintf(name, sizeof(name), "F%02u", static_cast<unsigned>(index));
    if (nameLength != 3) {
      return {};
    }
    xobjects += " /" + std::string(name, static_cast<size_t>(nameLength)) + " " +
                std::to_string(firstFormObject + index) + " 0 R";
    pageContent += "q 1 0 0 1 0 " + std::to_string(740U - index * 35U) + " cm /" +
                   std::string(name, static_cast<size_t>(nameLength)) + " Do Q ";
    objects[4U + index] =
        streamObject("BT /F1 12 Tf 1 0 0 1 72 10 Tm (Cap" +
                         std::string(name + 1, static_cast<size_t>(nameLength - 1)) + ") Tj ET",
                     "/Type /XObject /Subtype /Form /FormType 1 /BBox [0 0 300 30]");
  }
  xobjects += " >>";
  objects[2] = "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] /Resources << /Font << /F1 " +
               std::to_string(fontObject) + " 0 R >> " + xobjects + " >> /Contents 4 0 R >>";
  objects[3] = streamObject(pageContent);
  objects[4U + count] = "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>";
  return classicPdf(objects);
}

std::vector<uint8_t> loadFixture(const char* const name) {
  const std::filesystem::path path =
      std::filesystem::path(__FILE__).parent_path().parent_path() / "pdf_reflow_core" / "fixtures" / name;
  std::ifstream input(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

struct PreparationHarness {
  PdfTestCacheIo storage;
  PdfResourceSnapshot resources{128U * 1024U, 96U * 1024U, 8U * 1024U};
  uint32_t nowMs = 0;

  static uint32_t now(void* const context) { return static_cast<PreparationHarness*>(context)->nowMs; }

  static PdfResourceSnapshot measure(void* const context) {
    return static_cast<PreparationHarness*>(context)->resources;
  }

  PdfPreparationConfig config(const char* const path) {
    return {
        storage.io(), path, "/.crosspoint", this, now, {this, measure, nullptr}, storage.renameCallback(), 800, 480,
    };
  }
};

bool isFontStorePath(const std::string& path);

struct CancelOnSequentialCMapReadHarness {
  PreparationHarness base;
  PdfCacheIo backing{};
  PdfCacheRenameFn backingRename = nullptr;
  PdfPreparation* preparation = nullptr;
  uint32_t fontReadsAfterSignal = 0;
  uint32_t parseFontReadsBeforeSignal = 0;
  uint64_t fontBytesAfterSignal = 0;
  bool cancelSignalled = false;

  CancelOnSequentialCMapReadHarness() : backing(base.storage.io()), backingRename(base.storage.renameCallback()) {}

  static CancelOnSequentialCMapReadHarness* self(void* const context) {
    return static_cast<CancelOnSequentialCMapReadHarness*>(context);
  }

  static PdfStatus open(void* const context, const char* const path, const PdfCacheOpenMode mode,
                        PdfCacheHandle* const handle) {
    auto* const harness = self(context);
    return harness->backing.open(harness->backing.context, path, mode, handle);
  }

  static PdfStatus read(void* const context, const PdfCacheHandle handle, const uint64_t offset,
                        uint8_t* const destination, const size_t requested, size_t* const bytesRead) {
    auto* const harness = self(context);
    const PdfStatus status = harness->backing.read(harness->backing.context, handle, offset, destination,
                                                   requested, bytesRead);
    if (!status || harness->preparation == nullptr || harness->base.storage.readObservations().empty()) {
      return status;
    }
    const PdfTestReadObservation& observation = harness->base.storage.readObservations().back();
    if (!isFontStorePath(observation.path) ||
        harness->preparation->phase() != PdfPreparationPhase::ParseFonts) {
      return status;
    }
    ++harness->parseFontReadsBeforeSignal;
    if (!harness->cancelSignalled && harness->parseFontReadsBeforeSignal == 2U) {
      harness->cancelSignalled = true;
      harness->preparation->requestCancel();
    }
    if (harness->cancelSignalled) {
      ++harness->fontReadsAfterSignal;
      harness->fontBytesAfterSignal += observation.bytesRead;
    }
    return status;
  }

  static PdfStatus write(void* const context, const PdfCacheHandle handle, const uint8_t* const source,
                         const size_t requested, size_t* const bytesWritten) {
    auto* const harness = self(context);
    return harness->backing.write(harness->backing.context, handle, source, requested, bytesWritten);
  }

  static PdfStatus flush(void* const context, const PdfCacheHandle handle) {
    auto* const harness = self(context);
    return harness->backing.flush(harness->backing.context, handle);
  }

  static PdfStatus sync(void* const context, const PdfCacheHandle handle) {
    auto* const harness = self(context);
    return harness->backing.sync(harness->backing.context, handle);
  }

  static PdfStatus close(void* const context, PdfCacheHandle* const handle) {
    auto* const harness = self(context);
    return harness->backing.close(harness->backing.context, handle);
  }

  static PdfStatus remove(void* const context, const char* const path, const bool recursive) {
    auto* const harness = self(context);
    return harness->backing.remove(harness->backing.context, path, recursive);
  }

  static PdfStatus mkdir(void* const context, const char* const path) {
    auto* const harness = self(context);
    return harness->backing.mkdir(harness->backing.context, path);
  }

  static PdfStatus list(void* const context, const char* const path, const PdfCacheListVisitor visitor,
                        void* const visitorContext) {
    auto* const harness = self(context);
    return harness->backing.list(harness->backing.context, path, visitor, visitorContext);
  }

  static PdfStatus capacity(void* const context, PdfCacheCapacity* const value) {
    auto* const harness = self(context);
    return harness->backing.capacity(harness->backing.context, value);
  }

  static PdfStatus metadata(void* const context, const PdfCacheHandle handle,
                            PdfCacheFileMetadata* const value) {
    auto* const harness = self(context);
    return harness->backing.metadata(harness->backing.context, handle, value);
  }

  static PdfStatus rename(void* const context, const char* const source, const char* const destination) {
    auto* const harness = self(context);
    return harness->backingRename(harness->backing.context, source, destination);
  }

  PdfPreparationConfig config(const char* const path) {
    PdfCacheIo wrapped{this, open, read, write, flush, sync, close, remove, mkdir, list, capacity, metadata};
    return {wrapped, path, "/.crosspoint", &base, PreparationHarness::now,
            {&base, PreparationHarness::measure, nullptr}, rename, 800, 480};
  }
};

struct ProductReadTrace {
  uint32_t parseFontSteps = 0;
  uint32_t decodeFontFlushCalls = 0;
  uint32_t decodeFontSyncCalls = 0;
  uint32_t decodeFontStoreCloseCalls = 0;
  uint32_t fontStoreReadCalls = 0;
  uint32_t finalInterpretationFontStoreReadCalls = 0;
  uint32_t parseFontStoreReadCalls = 0;
  uint32_t maximumParseFontStoreReadCallsPerStep = 0;
  uint64_t fontStoreReadBytes = 0;
  uint64_t finalInterpretationFontStoreReadBytes = 0;
  uint64_t parseFontStoreReadBytes = 0;
  uint64_t maximumParseFontStoreReadBytesPerStep = 0;
  uint32_t parseFontStoreRecordReadCalls = 0;
  uint32_t parseWriteCalls = 0;
  uint64_t parseWriteBytes = 0;
};

bool isFontStorePath(const std::string& path) {
  static constexpr std::string_view suffix = "/build.font";
  return path.size() >= suffix.size() &&
         path.compare(path.size() - suffix.size(), suffix.size(), suffix) == 0;
}

PdfStepResult runToTerminal(PdfPreparation& preparation, PreparationHarness& harness,
                            PdfPreparationPhase* const terminalEntryPhase = nullptr,
                            ProductReadTrace* const readTrace = nullptr) {
  size_t nextReadObservation = harness.storage.readObservations().size();
  for (uint32_t step = 0; step < 50000; ++step) {
    const PdfPreparationPhase entryPhase = preparation.phase();
    if (readTrace != nullptr && entryPhase == PdfPreparationPhase::ParseFonts) {
      ++readTrace->parseFontSteps;
    }
    const uint32_t writeCallsBefore = harness.storage.writeCalls();
    const uint64_t writeBytesBefore = harness.storage.bytesWrittenTotal();
    const uint32_t flushCallsBefore = harness.storage.flushCalls();
    const uint32_t syncCallsBefore = harness.storage.syncCalls();
    const size_t eventsBefore = harness.storage.events().size();
    const PdfStepResult result = preparation.step();
    ++harness.nowMs;
    if (readTrace != nullptr && entryPhase == PdfPreparationPhase::DecodeFonts) {
      readTrace->decodeFontFlushCalls += harness.storage.flushCalls() - flushCallsBefore;
      readTrace->decodeFontSyncCalls += harness.storage.syncCalls() - syncCallsBefore;
      const std::vector<std::string>& events = harness.storage.events();
      for (size_t event = eventsBefore; event < events.size(); ++event) {
        static constexpr std::string_view prefix = "close:";
        if (events[event].compare(0, prefix.size(), prefix) == 0 &&
            isFontStorePath(events[event].substr(prefix.size()))) {
          ++readTrace->decodeFontStoreCloseCalls;
        }
      }
    }
    const std::vector<PdfTestReadObservation>& reads = harness.storage.readObservations();
    uint32_t parseFontReadsThisStep = 0;
    uint64_t parseFontBytesThisStep = 0;
    while (nextReadObservation < reads.size()) {
      const PdfTestReadObservation& read = reads[nextReadObservation++];
      if (readTrace != nullptr && isFontStorePath(read.path)) {
        ++readTrace->fontStoreReadCalls;
        readTrace->fontStoreReadBytes += read.bytesRead;
        if (entryPhase == PdfPreparationPhase::InterpretContent) {
          ++readTrace->finalInterpretationFontStoreReadCalls;
          readTrace->finalInterpretationFontStoreReadBytes += read.bytesRead;
        }
        if (entryPhase == PdfPreparationPhase::ParseFonts) {
          ++readTrace->parseFontStoreReadCalls;
          readTrace->parseFontStoreReadBytes += read.bytesRead;
          ++parseFontReadsThisStep;
          parseFontBytesThisStep += read.bytesRead;
          if (read.requested == sizeof(PdfCMapRecord)) {
            ++readTrace->parseFontStoreRecordReadCalls;
          }
        }
      }
    }
    if (readTrace != nullptr) {
      if (entryPhase == PdfPreparationPhase::ParseFonts) {
        readTrace->parseWriteCalls += harness.storage.writeCalls() - writeCallsBefore;
        readTrace->parseWriteBytes += harness.storage.bytesWrittenTotal() - writeBytesBefore;
      }
      readTrace->maximumParseFontStoreReadCallsPerStep =
          std::max(readTrace->maximumParseFontStoreReadCallsPerStep, parseFontReadsThisStep);
      readTrace->maximumParseFontStoreReadBytesPerStep =
          std::max(readTrace->maximumParseFontStoreReadBytesPerStep, parseFontBytesThisStep);
    }
    if (!result.yielded()) {
      if (terminalEntryPhase != nullptr) {
        *terminalEntryPhase = entryPhase;
      }
      return result;
    }
  }
  return PdfStepResult::failure(PdfStatus::failure(PdfError::BudgetExhausted));
}

std::string preparedSection(const PdfPreparation& preparation, const PreparationHarness& harness) {
  const std::string path = std::string(preparation.cacheRoot()) + "/gen_" +
                           std::to_string(preparation.generation()) + "/sections/000000.xhtml";
  if (!harness.storage.exists(path)) {
    return {};
  }
  const std::vector<uint8_t>& bytes = harness.storage.bytes(path);
  return {bytes.begin(), bytes.end()};
}

struct ProductObservation {
  PdfStatus beginStatus{};
  PdfStepResult terminal = PdfStepResult::failure(PdfStatus::failure(PdfError::InvalidArgument));
  PdfPreparationPhase terminalEntryPhase = PdfPreparationPhase::Idle;
  uint32_t words = 0;
  uint32_t parseFontSteps = 0;
  uint32_t decodeFontFlushCalls = 0;
  uint32_t decodeFontSyncCalls = 0;
  uint32_t decodeFontStoreCloseCalls = 0;
  uint32_t fontStoreOpenCalls = 0;
  uint32_t fontStoreReadCalls = 0;
  uint32_t finalInterpretationFontStoreReadCalls = 0;
  uint32_t parseFontStoreReadCalls = 0;
  uint32_t maximumParseFontStoreReadCallsPerStep = 0;
  uint64_t fontStoreReadBytes = 0;
  uint64_t finalInterpretationFontStoreReadBytes = 0;
  uint64_t parseFontStoreReadBytes = 0;
  uint64_t maximumParseFontStoreReadBytesPerStep = 0;
  uint32_t parseFontStoreRecordReadCalls = 0;
  uint32_t parseWriteCalls = 0;
  uint64_t parseWriteBytes = 0;
  std::vector<PdfTestReadObservation> sourceReads;
  std::vector<std::string> terminalOpenHandlePaths;
  std::vector<std::string> terminalEvents;
  std::string section;
};

ProductObservation prepareProduct(const char* const sourcePath, const std::vector<uint8_t>& pdf) {
  PreparationHarness harness;
  harness.storage.setMaximumReadHandles(1);
  harness.storage.addFile(sourcePath, pdf, 1234, true);
  PdfPreparation preparation;
  ProductObservation observation;
  observation.beginStatus = preparation.begin(harness.config(sourcePath));
  if (!observation.beginStatus.ok()) {
    return observation;
  }
  ProductReadTrace readTrace{};
  observation.terminal =
      runToTerminal(preparation, harness, &observation.terminalEntryPhase, &readTrace);
  observation.words = preparation.totalWords();
  observation.parseFontSteps = readTrace.parseFontSteps;
  observation.decodeFontFlushCalls = readTrace.decodeFontFlushCalls;
  observation.decodeFontSyncCalls = readTrace.decodeFontSyncCalls;
  observation.decodeFontStoreCloseCalls = readTrace.decodeFontStoreCloseCalls;
  observation.section = preparedSection(preparation, harness);
  const std::string fontStorePath = std::string(preparation.cacheRoot()) + "/gen_" +
                                    std::to_string(preparation.generation()) + "/build.font";
  observation.fontStoreOpenCalls = harness.storage.openCallsForPath(fontStorePath);
  observation.fontStoreReadCalls = readTrace.fontStoreReadCalls;
  observation.finalInterpretationFontStoreReadCalls = readTrace.finalInterpretationFontStoreReadCalls;
  observation.parseFontStoreReadCalls = readTrace.parseFontStoreReadCalls;
  observation.maximumParseFontStoreReadCallsPerStep = readTrace.maximumParseFontStoreReadCallsPerStep;
  observation.fontStoreReadBytes = readTrace.fontStoreReadBytes;
  observation.finalInterpretationFontStoreReadBytes = readTrace.finalInterpretationFontStoreReadBytes;
  observation.parseFontStoreReadBytes = readTrace.parseFontStoreReadBytes;
  observation.maximumParseFontStoreReadBytesPerStep = readTrace.maximumParseFontStoreReadBytesPerStep;
  observation.parseFontStoreRecordReadCalls = readTrace.parseFontStoreRecordReadCalls;
  observation.parseWriteCalls = readTrace.parseWriteCalls;
  observation.parseWriteBytes = readTrace.parseWriteBytes;
  observation.sourceReads = harness.storage.readObservations();
  observation.terminalOpenHandlePaths = harness.storage.openHandlePaths();
  observation.terminalEvents = harness.storage.events();
  return observation;
}

size_t readsStartingInTargetSpan(const ProductObservation& observation, const std::string_view sourcePath,
                                 const PdfWithTargetSpan& pdf) {
  if (!pdf.valid()) {
    return 0;
  }
  const uint64_t targetEnd = pdf.targetOffset + pdf.targetLength;
  return static_cast<size_t>(std::count_if(
      observation.sourceReads.begin(), observation.sourceReads.end(),
      [sourcePath, &pdf, targetEnd](const PdfTestReadObservation& read) {
        return read.path == sourcePath && read.offset >= pdf.targetOffset && read.offset < targetEnd;
      }));
}

::testing::AssertionResult completedWithSection(const ProductObservation& observation) {
  if (!observation.beginStatus.ok()) {
    return ::testing::AssertionFailure() << "begin error=" << static_cast<int>(observation.beginStatus.error);
  }
  if (!observation.terminal.complete()) {
    auto result = ::testing::AssertionFailure()
                  << "terminal error=" << static_cast<int>(observation.terminal.status.error)
                  << " offset=" << observation.terminal.status.offset
                  << " entry phase=" << static_cast<int>(observation.terminalEntryPhase);
    for (const std::string& path : observation.terminalOpenHandlePaths) {
      result << " open=" << path;
    }
    const size_t firstEvent = observation.terminalEvents.size() > 12U
                                  ? observation.terminalEvents.size() - 12U
                                  : 0U;
    for (size_t index = firstEvent; index < observation.terminalEvents.size(); ++index) {
      result << " event=" << observation.terminalEvents[index];
    }
    return result;
  }
  if (observation.section.empty()) {
    return ::testing::AssertionFailure() << "completed preparation did not commit section XHTML";
  }
  return ::testing::AssertionSuccess();
}

TEST(PdfPreparationProductGapRed, ControlClassicTextCommitsExpectedXhtml) {
  const std::vector<uint8_t> pdf = loadFixture("classic_text.pdf");
  ASSERT_FALSE(pdf.empty());

  const ProductObservation observation = prepareProduct("/books/product-gap-control.pdf", pdf);

  ASSERT_TRUE(completedWithSection(observation));
  EXPECT_EQ(observation.words, 2U);
  EXPECT_NE(observation.section.find(">Hello PDF</p>"), std::string::npos) << observation.section;
}

TEST(PdfPreparationProductGapRed, IgnoresValidEmptyLiteralAndHexPaintedStrings) {
  const std::vector<uint8_t> pdf = emptyPaintedStringsPdf();
  ASSERT_FALSE(pdf.empty());

  const ProductObservation observation = prepareProduct("/books/product-gap-empty-strings.pdf", pdf);

  ASSERT_TRUE(completedWithSection(observation));
  EXPECT_EQ(observation.words, 1U);
  EXPECT_NE(observation.section.find(">Visible</p>"), std::string::npos) << observation.section;
}

TEST(PdfPreparationProductGapRed, IgnoresValidEmptyPaintedStringsAfterObservedJournalSpill) {
  const std::vector<uint8_t> pdf = denseCidWithUnobservedMalformedFontPdf(true);
  ASSERT_FALSE(pdf.empty());

  const ProductObservation observation = prepareProduct("/books/product-gap-spilled-empty-strings.pdf", pdf);

  ASSERT_TRUE(completedWithSection(observation));
  EXPECT_EQ(observation.words, kDenseCidChunkCount);
  for (size_t index = 0; index < kDenseCidChunkCount; ++index) {
    char replacement[16]{};
    const int length = std::snprintf(replacement, sizeof(replacement), "Dense%02u", static_cast<unsigned>(index));
    ASSERT_GT(length, 0);
    ASSERT_LT(static_cast<size_t>(length), sizeof(replacement));
    EXPECT_EQ(countOccurrences(observation.section, replacement), 1U) << observation.section;
  }
}

TEST(PdfPreparationProductGapRed, UsesCodesDeclaredInEveryLegalCodeSpaceBlock) {
  const std::vector<uint8_t> pdf = splitCodeSpaceBlocksPdf();
  ASSERT_FALSE(pdf.empty());

  const ProductObservation observation = prepareProduct("/books/product-gap-split-codespaces.pdf", pdf);

  ASSERT_TRUE(completedWithSection(observation));
  const size_t firstCode = observation.section.find(">A</p>");
  const size_t secondCode = observation.section.find(">\xCE\xA9</p>");
  ASSERT_NE(firstCode, std::string::npos) << observation.section;
  ASSERT_NE(secondCode, std::string::npos) << observation.section;
  EXPECT_LT(firstCode, secondCode) << observation.section;
}

TEST(PdfPreparationProductGapRed, DecodesFlateToUnicodeAndUtf16ActualTextIntoCommittedXhtml) {
  const std::vector<uint8_t> pdf = actualTextAndToUnicodePdf();
  ASSERT_FALSE(pdf.empty());
  const std::string source(pdf.begin(), pdf.end());
  ASSERT_NE(source.find("/ToUnicode 6 0 R"), std::string::npos);
  ASSERT_NE(source.find("/Filter /FlateDecode"), std::string::npos);
  ASSERT_NE(source.find("/ActualText <FEFF"), std::string::npos);

  const ProductObservation observation = prepareProduct("/books/product-gap-actualtext.pdf", pdf);

  ASSERT_TRUE(completedWithSection(observation));
  EXPECT_EQ(observation.words, 2U);
  const size_t mappedOffset = observation.section.find("\xCE\xA9");
  const size_t actualTextOffset = observation.section.find("Replacement");
  ASSERT_NE(mappedOffset, std::string::npos) << observation.section;
  ASSERT_NE(actualTextOffset, std::string::npos) << observation.section;
  EXPECT_LT(mappedOffset, actualTextOffset) << observation.section;
  EXPECT_EQ(countOccurrences(observation.section, "Replacement"), 1U) << observation.section;
  EXPECT_EQ(observation.fontStoreOpenCalls, 2U);
  EXPECT_EQ(observation.decodeFontFlushCalls, 0U);
  EXPECT_EQ(observation.decodeFontSyncCalls, 1U);
  EXPECT_EQ(observation.decodeFontStoreCloseCalls, 1U);
  EXPECT_GT(observation.fontStoreReadCalls, 0U);
  EXPECT_GT(observation.fontStoreReadBytes, 0U);
  EXPECT_EQ(observation.finalInterpretationFontStoreReadCalls, 0U);
  EXPECT_EQ(observation.finalInterpretationFontStoreReadBytes, 0U);
}

TEST(PdfPreparationProductGapRed, TreatsSimpleFontStringsAsOneByteCodesDespiteWideToUnicodeCodeSpace) {
  const std::vector<uint8_t> pdf = wideCodeSpaceSimpleFontPdf();
  ASSERT_FALSE(pdf.empty());

  const ProductObservation observation = prepareProduct("/books/product-gap-wide-simple-codespace.pdf", pdf);

  ASSERT_TRUE(completedWithSection(observation));
  EXPECT_EQ(observation.words, 1U);
  EXPECT_NE(observation.section.find(">Corporate</p>"), std::string::npos) << observation.section;
  EXPECT_EQ(observation.section.find("\xEF\xBF\xBD"), std::string::npos) << observation.section;
}

TEST(PdfPreparationProductGapRed, MaterializesObservedOneAndTwoByteToUnicodeCodesIntoCommittedXhtml) {
  const std::vector<uint8_t> pdf = loadFixture("tounicode_simple_and_cid.pdf");
  ASSERT_FALSE(pdf.empty());
  const std::string source(pdf.begin(), pdf.end());
  ASSERT_NE(source.find("/Subtype /Type1"), std::string::npos);
  ASSERT_NE(source.find("/Subtype /Type0"), std::string::npos);
  ASSERT_NE(source.find("/Encoding /Identity-H"), std::string::npos);
  ASSERT_NE(source.find("<00430049004401000101> Tj"), std::string::npos);

  const ProductObservation observation = prepareProduct("/books/product-gap-cid.pdf", pdf);

  ASSERT_TRUE(completedWithSection(observation));
  EXPECT_EQ(observation.words, 2U);
  EXPECT_NE(observation.section.find(">Simple</p>"), std::string::npos) << observation.section;
  EXPECT_NE(observation.section.find(">CID\xCE\xA9\xCF\x80</p>"), std::string::npos) << observation.section;
  EXPECT_EQ(observation.fontStoreOpenCalls, 2U)
      << "all decoded fonts on one page must share the writer opened by SpoolFontNavigation";
  EXPECT_EQ(observation.decodeFontFlushCalls, 0U);
  EXPECT_EQ(observation.decodeFontSyncCalls, 1U);
  EXPECT_EQ(observation.decodeFontStoreCloseCalls, 1U);
  EXPECT_EQ(observation.finalInterpretationFontStoreReadCalls, 0U);
  EXPECT_EQ(observation.finalInterpretationFontStoreReadBytes, 0U);
}

TEST(PdfPreparationProductGapRed, CancellingDecodeFontsClosesAndRemovesTheCarriedFontWriter) {
  constexpr char sourcePath[] = "/books/product-gap-font-cancel.pdf";
  PreparationHarness harness;
  harness.storage.setMaximumReadHandles(1);
  harness.storage.addFile(sourcePath, actualTextAndToUnicodePdf(), 1234, true);
  PdfPreparation preparation;
  ASSERT_TRUE(preparation.begin(harness.config(sourcePath)).ok());

  std::string fontStorePath;
  for (uint32_t step = 0; step < 50000 && fontStorePath.empty(); ++step) {
    const PdfStepResult result = preparation.step();
    ++harness.nowMs;
    ASSERT_TRUE(result.yielded()) << static_cast<int>(result.status.error) << "@" << result.status.offset;
    if (preparation.phase() != PdfPreparationPhase::DecodeFonts) {
      continue;
    }
    for (const std::string& path : harness.storage.openHandlePaths()) {
      if (isFontStorePath(path)) {
        fontStorePath = path;
        break;
      }
    }
  }
  ASSERT_FALSE(fontStorePath.empty());

  preparation.requestCancel();
  const PdfStepResult cancelled = runToTerminal(preparation, harness);

  ASSERT_TRUE(cancelled.failed());
  EXPECT_EQ(cancelled.status.error, PdfError::Cancelled);
  EXPECT_EQ(preparation.phase(), PdfPreparationPhase::Cancelled);
  EXPECT_EQ(harness.storage.openHandleCount(), 0U);
  EXPECT_FALSE(harness.storage.exists(fontStorePath));
}

TEST(PdfPreparationProductGapRed, DecodeFontWriterSyncFailureClosesAndRemovesTheTemporaryStore) {
  constexpr char sourcePath[] = "/books/product-gap-font-sync-failure.pdf";
  PreparationHarness harness;
  harness.storage.setMaximumReadHandles(1);
  harness.storage.addFile(sourcePath, actualTextAndToUnicodePdf(), 1234, true);
  PdfPreparation preparation;
  ASSERT_TRUE(preparation.begin(harness.config(sourcePath)).ok());

  std::string fontStorePath;
  for (uint32_t step = 0; step < 50000 && fontStorePath.empty(); ++step) {
    const PdfStepResult result = preparation.step();
    ++harness.nowMs;
    ASSERT_TRUE(result.yielded()) << static_cast<int>(result.status.error) << "@" << result.status.offset;
    if (preparation.phase() != PdfPreparationPhase::DecodeFonts) {
      continue;
    }
    for (const std::string& path : harness.storage.openHandlePaths()) {
      if (isFontStorePath(path)) {
        fontStorePath = path;
        break;
      }
    }
  }
  ASSERT_FALSE(fontStorePath.empty());

  harness.storage.fail(PdfTestFaultPoint::Sync);
  const PdfStepResult failed = runToTerminal(preparation, harness);

  ASSERT_TRUE(failed.failed());
  EXPECT_EQ(failed.status.error, PdfError::IoFailure);
  EXPECT_EQ(preparation.phase(), PdfPreparationPhase::Failed);
  EXPECT_EQ(harness.storage.openHandleCount(), 0U);
  EXPECT_FALSE(harness.storage.exists(fontStorePath));
}

TEST(PdfPreparationProductGapRed, DenseRepeatedCidTextDoesNotOverflowObservedGlyphTracking) {
  static_assert(kDenseCidPaintedBytes > 7664U);
  const std::vector<uint8_t> pdf = denseCidWithUnobservedMalformedFontPdf();
  ASSERT_FALSE(pdf.empty());
  const std::string source(pdf.begin(), pdf.end());
  ASSERT_NE(source.find("/F1 5 0 R /F2 8 0 R"), std::string::npos);
  ASSERT_NE(source.find("/ToUnicode 9 0 R"), std::string::npos);
  ASSERT_NE(source.find("this-is-not-a-zlib-stream"), std::string::npos);

  const ProductObservation observation = prepareProduct("/books/product-gap-dense-cid.pdf", pdf);

  ASSERT_TRUE(completedWithSection(observation));
  EXPECT_EQ(observation.words, kDenseCidChunkCount);
  for (size_t index = 0; index < kDenseCidChunkCount; ++index) {
    char replacement[16]{};
    const int length = std::snprintf(replacement, sizeof(replacement), "Dense%02u", static_cast<unsigned>(index));
    ASSERT_GT(length, 0);
    ASSERT_LT(static_cast<size_t>(length), sizeof(replacement));
    EXPECT_EQ(countOccurrences(observation.section, replacement), 1U) << observation.section;
  }
  EXPECT_EQ(observation.finalInterpretationFontStoreReadCalls, 0U);
  EXPECT_EQ(observation.finalInterpretationFontStoreReadBytes, 0U);
}

TEST(PdfPreparationProductGapRed, SpillsWithoutSplittingARecordAtAnyRamJournalBoundaryByte) {
  for (size_t bytesRemaining = 0; bytesRemaining < kMaximumObservedRecordBytes; ++bytesRemaining) {
    SCOPED_TRACE(::testing::Message() << "bytes remaining before overflow=" << bytesRemaining);
    const std::vector<uint8_t> pdf = spillBoundaryPdf(bytesRemaining);
    ASSERT_FALSE(pdf.empty());

    const ProductObservation observation = prepareProduct("/books/product-gap-spill-boundary.pdf", pdf);

    ASSERT_TRUE(completedWithSection(observation));
    EXPECT_GT(observation.fontStoreOpenCalls, 0U);
    EXPECT_EQ(observation.finalInterpretationFontStoreReadCalls, 0U);
  }
}

TEST(PdfPreparationProductGapRed, AcceptsExactlyTwoHundredFiftySixUniquePaintedCidCodes) {
  const std::vector<uint8_t> pdf = cidUniqueGlyphPdf(256U);
  ASSERT_FALSE(pdf.empty());

  const ProductObservation observation = prepareProduct("/books/product-gap-256-cid-codes.pdf", pdf);

  ASSERT_TRUE(completedWithSection(observation));
  EXPECT_LE(observation.parseFontSteps, 64U)
      << "materializing successive observed glyphs must consume the existing work slice";
  EXPECT_EQ(observation.finalInterpretationFontStoreReadCalls, 0U);
}

TEST(PdfPreparationProductGapRed, OmitsOnlyTheTwoHundredFiftySeventhUniquePaintedCidCode) {
  const std::vector<uint8_t> pdf = cidUniqueGlyphPdf(257U);
  ASSERT_FALSE(pdf.empty());

  const ProductObservation observation = prepareProduct("/books/product-gap-257-cid-codes.pdf", pdf);

  ASSERT_TRUE(completedWithSection(observation));
  EXPECT_NE(observation.section.find("\xC4\x80"), std::string::npos);
  EXPECT_EQ(observation.section.find("\xC8\x80"), std::string::npos);
  EXPECT_EQ(observation.finalInterpretationFontStoreReadCalls, 0U);
}

TEST(PdfPreparationProductGapRed, ReplaysOneSpilledJournalOnceAcrossSixteenPaintedFonts) {
  static_assert(kSixteenFontJournalBytes > kObservedJournalCapacity);
  const std::vector<uint8_t> pdf = sixteenPaintedFontsSpilledJournalPdf();
  ASSERT_FALSE(pdf.empty());

  const ProductObservation observation = prepareProduct("/books/product-gap-sixteen-fonts.pdf", pdf);

  ASSERT_TRUE(completedWithSection(observation));
  EXPECT_LE(observation.parseFontSteps, 128U)
      << "the observed-string journal must consume the existing work slice instead of yielding per glyph";
  EXPECT_EQ(observation.parseFontStoreReadBytes, kSixteenFontJournalBytes)
      << "the raw observed-string journal must be read globally once, not once per font";
  EXPECT_EQ(observation.finalInterpretationFontStoreReadCalls, 0U);
  EXPECT_EQ(observation.finalInterpretationFontStoreReadBytes, 0U);
}

TEST(PdfPreparationProductGapRed, AcceptsFullPaintedGlyphBudgetBeforeLaterUnpaintedAlias) {
  const std::vector<uint8_t> pdf = cidUniqueGlyphPdf(256U, true);
  ASSERT_FALSE(pdf.empty());

  const ProductObservation observation =
      prepareProduct("/books/product-gap-256-cid-plus-unpainted.pdf", pdf);

  ASSERT_TRUE(completedWithSection(observation));
  EXPECT_EQ(observation.finalInterpretationFontStoreReadCalls, 0U);
  EXPECT_EQ(observation.finalInterpretationFontStoreReadBytes, 0U);
}

TEST(PdfPreparationProductGapRed, BoundsOneHundredTwelveGlyphSpilledCMapWorkPerPublicStep) {
  const std::vector<uint8_t> pdf = largeSpilledCMapSingleTokenPdf();
  ASSERT_FALSE(pdf.empty());

  const ProductObservation observation = prepareProduct("/books/product-gap-large-cmap.pdf", pdf);

  ASSERT_TRUE(completedWithSection(observation));
  EXPECT_GT(observation.parseFontStoreReadCalls, 0U);
  EXPECT_LE(observation.maximumParseFontStoreReadCallsPerStep, 32U)
      << "one public step may not perform uncharged spilled-CMap binary searches for a whole token";
  EXPECT_LE(observation.maximumParseFontStoreReadBytesPerStep, 4U * 1024U)
      << "one public step must remain within the firmware byte slice";
  EXPECT_EQ(observation.finalInterpretationFontStoreReadCalls, 0U);
  EXPECT_EQ(observation.finalInterpretationFontStoreReadBytes, 0U);
}

TEST(PdfPreparationProductGapRed, AvoidsCMapSpillIoForMaximumSortedMap) {
  const std::vector<uint8_t> pdf = maximumSortedCMapPdf();
  ASSERT_FALSE(pdf.empty());

  const ProductObservation observation = prepareProduct("/books/product-gap-maximum-cmap.pdf", pdf);

  ASSERT_TRUE(completedWithSection(observation));
  EXPECT_EQ(observation.parseWriteCalls, 0U)
      << "sequential CMap observation must not append mapping records to build.font";
  EXPECT_EQ(observation.parseWriteBytes, 0U);
  EXPECT_EQ(observation.parseFontStoreRecordReadCalls, 0U)
      << "materialization must not perform random fixed-record reads from build.font";
  EXPECT_LE(observation.maximumParseFontStoreReadCallsPerStep, 32U);
  EXPECT_LE(observation.maximumParseFontStoreReadBytesPerStep, 4U * 1024U);
  EXPECT_EQ(observation.finalInterpretationFontStoreReadCalls, 0U);
}

TEST(PdfPreparationProductGapRed, KeepsFirstMappingForSmallDuplicateCMap) {
  const ProductObservation observation =
      prepareProduct("/books/product-gap-duplicate-cmap.pdf", duplicateCMapMappingPdf());

  ASSERT_TRUE(completedWithSection(observation));
  EXPECT_NE(observation.section.find(">A</p>"), std::string::npos) << observation.section;
  EXPECT_EQ(observation.section.find(">B</p>"), std::string::npos) << observation.section;
}

TEST(PdfPreparationProductGapRed, RejectsPaintedCodeMissingFromCMap) {
  const ProductObservation observation =
      prepareProduct("/books/product-gap-missing-cmap.pdf", missingCMapMappingPdf());

  ASSERT_TRUE(observation.beginStatus.ok());
  ASSERT_TRUE(observation.terminal.failed());
  EXPECT_EQ(observation.terminal.status.error, PdfError::UnsupportedEncoding);
  EXPECT_EQ(observation.terminalEntryPhase, PdfPreparationPhase::ParseFonts);
}

TEST(PdfPreparationProductGapRed, CancelsWithinOneBoundedSequentialCMapParseStep) {
  constexpr char sourcePath[] = "/books/product-gap-large-cmap-cancel.pdf";
  const std::vector<uint8_t> pdf = largeSpilledCMapSingleTokenPdf();
  ASSERT_FALSE(pdf.empty());
  CancelOnSequentialCMapReadHarness harness;
  harness.base.storage.setMaximumReadHandles(1);
  harness.base.storage.addFile(sourcePath, pdf, 1234, true);
  PdfPreparation preparation;
  harness.preparation = &preparation;
  ASSERT_TRUE(preparation.begin(harness.config(sourcePath)).ok());

  PdfStepResult terminal = PdfStepResult::failure(PdfStatus::failure(PdfError::BudgetExhausted));
  for (uint32_t step = 0; step < 50000U; ++step) {
    const PdfStepResult result = preparation.step();
    ++harness.base.nowMs;
    if (!result.yielded()) {
      terminal = result;
      break;
    }
  }

  ASSERT_TRUE(harness.cancelSignalled);
  ASSERT_TRUE(terminal.failed());
  EXPECT_EQ(terminal.status.error, PdfError::Cancelled);
  EXPECT_LE(harness.fontReadsAfterSignal, 32U)
      << "cancellation raised during sequential CMap parsing must stop within one public slice";
  EXPECT_LE(harness.fontBytesAfterSignal, 4U * 1024U);
  for (const std::string& path : harness.base.storage.paths()) {
    EXPECT_FALSE(isFontStorePath(path));
  }
  EXPECT_EQ(harness.base.storage.openHandleCount(), 0U);
  EXPECT_EQ(preparation.resourceCurrentBytes(), 0U);
}

TEST(PdfPreparationProductGapRed, RollsBackNonPaintedStringOperandsBeforeCountingGlyphs) {
  const std::vector<uint8_t> pdf = rolledBackObservedStringsPdf();
  ASSERT_FALSE(pdf.empty());

  const ProductObservation observation = prepareProduct("/books/product-gap-nonpainted-rollback.pdf", pdf);

  ASSERT_TRUE(completedWithSection(observation));
  EXPECT_EQ(observation.words, 1U);
  EXPECT_NE(observation.section.find(">A</p>"), std::string::npos) << observation.section;
  EXPECT_EQ(observation.section.find("BBBB"), std::string::npos) << observation.section;
}

TEST(PdfPreparationProductGapRed, CancellingAnActiveObservedStringSpillClosesAndRemovesBuildFont) {
  constexpr char sourcePath[] = "/books/product-gap-cancel-spill.pdf";
  const std::vector<uint8_t> pdf = denseCidWithUnobservedMalformedFontPdf();
  ASSERT_FALSE(pdf.empty());
  PreparationHarness harness;
  harness.storage.setMaximumReadHandles(1);
  harness.storage.addFile(sourcePath, pdf, 1234, true);
  PdfPreparation preparation;
  ASSERT_TRUE(preparation.begin(harness.config(sourcePath)).ok());

  bool spillOpened = false;
  std::string fontStorePath;
  for (uint32_t step = 0; step < 50000U; ++step) {
    const PdfStepResult result = preparation.step();
    ++harness.nowMs;
    ASSERT_TRUE(result.yielded()) << "error=" << static_cast<int>(result.status.error)
                                  << " offset=" << result.status.offset
                                  << " phase=" << static_cast<int>(preparation.phase());
    for (const std::string& path : harness.storage.paths()) {
      if (isFontStorePath(path)) {
        fontStorePath = path;
        spillOpened = true;
        break;
      }
    }
    if (spillOpened) {
      break;
    }
  }
  ASSERT_TRUE(spillOpened);
  preparation.requestCancel();
  const PdfStepResult terminal = runToTerminal(preparation, harness);

  ASSERT_TRUE(terminal.failed());
  EXPECT_EQ(terminal.status.error, PdfError::Cancelled);
  EXPECT_FALSE(harness.storage.exists(fontStorePath));
  EXPECT_EQ(harness.storage.openHandleCount(), 0U);
  EXPECT_EQ(preparation.resourceCurrentBytes(), 0U);
}

TEST(PdfPreparationProductGapRed, SkipsDecodeAndFontStoreIoForUnobservedToUnicode) {
  const std::vector<uint8_t> pdf = unobservedToUnicodePdf();
  ASSERT_FALSE(pdf.empty());
  const std::string source(pdf.begin(), pdf.end());
  ASSERT_NE(source.find("/F1 5 0 R /F2 6 0 R"), std::string::npos);
  ASSERT_NE(source.find("/F1 12 Tf"), std::string::npos);
  ASSERT_EQ(source.find("/F2 12 Tf"), std::string::npos);
  ASSERT_NE(source.find("/ToUnicode 7 0 R"), std::string::npos);
  ASSERT_NE(source.find("this-is-not-a-zlib-stream"), std::string::npos);

  const ProductObservation observation = prepareProduct("/books/product-gap-unobserved-font.pdf", pdf);

  ASSERT_TRUE(completedWithSection(observation));
  EXPECT_EQ(observation.words, 1U);
  EXPECT_NE(observation.section.find(">Hello</p>"), std::string::npos) << observation.section;
  EXPECT_EQ(observation.fontStoreOpenCalls, 0U);
  EXPECT_EQ(observation.fontStoreReadCalls, 0U);
  EXPECT_EQ(observation.fontStoreReadBytes, 0U);
  EXPECT_EQ(observation.finalInterpretationFontStoreReadCalls, 0U);
  EXPECT_EQ(observation.finalInterpretationFontStoreReadBytes, 0U);
}

TEST(PdfPreparationProductGapRed, UsesDefaultType1EncodingForPaintedHexTextOnly) {
  const std::vector<uint8_t> pdf = defaultType1EncodingPdf();
  ASSERT_FALSE(pdf.empty());
  const std::string source(pdf.begin(), pdf.end());
  ASSERT_NE(source.find("/Subtype /Type1 /BaseFont /Helvetica >>"), std::string::npos);
  ASSERT_EQ(source.find("/Encoding"), std::string::npos);
  ASSERT_EQ(source.find("/Widths"), std::string::npos);
  ASSERT_NE(source.find("<48656C6C6F> Tj"), std::string::npos);
  ASSERT_NE(source.find("<4E6F74207061696E746564> >> DP"), std::string::npos);

  const ProductObservation observation = prepareProduct("/books/product-gap-default-font.pdf", pdf);

  ASSERT_TRUE(completedWithSection(observation));
  EXPECT_EQ(observation.words, 1U);
  EXPECT_NE(observation.section.find(">Hello</p>"), std::string::npos) << observation.section;
  EXPECT_EQ(observation.section.find("Not painted"), std::string::npos) << observation.section;
}

TEST(PdfPreparationProductGapRed, SuppressesHiddenOcrDuplicateAndKeepsPlacedImage) {
  const std::vector<uint8_t> pdf = loadFixture("hidden_ocr_visible_duplicate.pdf");
  ASSERT_FALSE(pdf.empty());
  const std::string source(pdf.begin(), pdf.end());
  ASSERT_NE(source.find("/Subtype /Image"), std::string::npos);
  ASSERT_NE(source.find("3 Tr"), std::string::npos);
  ASSERT_EQ(countOccurrences(source, "Duplicate visible text."), 2U);

  const ProductObservation observation = prepareProduct("/books/product-gap-hidden-ocr.pdf", pdf);

  const auto imageOpen = std::find_if(observation.terminalEvents.begin(), observation.terminalEvents.end(),
                                      [](const std::string& event) {
                                        return event.starts_with("open:") &&
                                               event.ends_with("/build.image-files");
                                      });
  const auto imageClose = std::find_if(observation.terminalEvents.begin(), observation.terminalEvents.end(),
                                       [](const std::string& event) {
                                         return event.starts_with("close:") &&
                                                event.ends_with("/build.image-files");
                                       });
  ASSERT_NE(imageOpen, observation.terminalEvents.end());
  const auto pageRecordsOpen = std::find_if(std::next(imageOpen), observation.terminalEvents.end(),
                                            [](const std::string& event) {
                                              return event.starts_with("open:") &&
                                                     event.ends_with("/build.page-records");
                                            });
  ASSERT_NE(imageClose, observation.terminalEvents.end());
  ASSERT_NE(pageRecordsOpen, observation.terminalEvents.end());
  EXPECT_LT(imageOpen, imageClose);
  EXPECT_LT(imageClose, pageRecordsOpen)
      << "single-reader firmware must retire build.image-files before reading build.page-records";
  ASSERT_TRUE(completedWithSection(observation));
  EXPECT_EQ(observation.words, 3U);
  EXPECT_EQ(countOccurrences(observation.section, "Duplicate visible text."), 1U) << observation.section;
  EXPECT_EQ(countOccurrences(observation.section, "<img src=\""), 1U) << observation.section;
}

TEST(PdfPreparationProductGapRed, InterpretsPlacedFormAndUsesActualTextInCommittedXhtml) {
  const std::vector<uint8_t> pdf = loadFixture("operators_actualtext_forms.pdf");
  ASSERT_FALSE(pdf.empty());
  const std::string source(pdf.begin(), pdf.end());
  ASSERT_NE(source.find("/Subtype /Form"), std::string::npos);
  ASSERT_NE(source.find("/Fm1 Do"), std::string::npos);
  ASSERT_NE(source.find("/ActualText (Accessible replacement)"), std::string::npos);

  const ProductObservation observation = prepareProduct("/books/product-gap-form.pdf", pdf);

  ASSERT_TRUE(completedWithSection(observation));
  EXPECT_EQ(observation.words, 4U);
  const size_t formOffset = observation.section.find(">Form</p>");
  const size_t headingOffset = observation.section.find(">heading</p>");
  const size_t actualTextOffset = observation.section.find("Accessible replacement");
  ASSERT_NE(formOffset, std::string::npos) << observation.section;
  ASSERT_NE(headingOffset, std::string::npos) << observation.section;
  ASSERT_NE(actualTextOffset, std::string::npos) << observation.section;
  EXPECT_LT(formOffset, headingOffset) << observation.section;
  EXPECT_LT(headingOffset, actualTextOffset) << observation.section;
  EXPECT_LT(formOffset, actualTextOffset) << observation.section;
  EXPECT_EQ(observation.section.find("Visual glyphs"), std::string::npos) << observation.section;
}

TEST(PdfPreparationProductGapRed, FormResourcesShadowInheritAndRestoreTheCallingFontScope) {
  const ProductObservation observation =
      prepareProduct("/books/product-gap-form-resource-scope.pdf", formResourceScopePdf());

  ASSERT_TRUE(completedWithSection(observation));
  EXPECT_EQ(countOccurrences(observation.section, "\xCE\xA9"), 3U) << observation.section;
  EXPECT_EQ(countOccurrences(observation.section, "\xCF\x80"), 1U) << observation.section;
}

TEST(PdfPreparationProductGapRed, ResolvesNestedFormFromTheCallingFormsLocalXObjectDictionary) {
  const ProductObservation observation =
      prepareProduct("/books/product-gap-nested-form.pdf", nestedFormPdf());

  ASSERT_TRUE(completedWithSection(observation));
  EXPECT_EQ(observation.words, 1U);
  EXPECT_EQ(countOccurrences(observation.section, "NestedTarget"), 1U) << observation.section;
}

TEST(PdfPreparationProductGapRed, AppliesNonIdentityFormMatrixBeforeFinalReadingOrder) {
  const ProductObservation observation =
      prepareProduct("/books/product-gap-form-matrix.pdf", formMatrixReadingOrderPdf());

  ASSERT_TRUE(completedWithSection(observation));
  const size_t top = observation.section.find("TopMarker");
  const size_t placed = observation.section.find("MatrixMarker");
  const size_t bottom = observation.section.find("BottomMarker");
  ASSERT_NE(top, std::string::npos) << observation.section;
  ASSERT_NE(placed, std::string::npos) << observation.section;
  ASSERT_NE(bottom, std::string::npos) << observation.section;
  EXPECT_LT(top, placed) << observation.section;
  EXPECT_LT(placed, bottom) << observation.section;
}

TEST(PdfPreparationProductGapRed, DoesNotReadAnUninvokedMalformedFormBodyAndTheSpanOracleDetectsInvocation) {
  static constexpr std::string_view deadPath = "/books/product-gap-dead-form.pdf";
  const PdfWithTargetSpan dead = malformedFormReadPdf(false);
  ASSERT_TRUE(dead.valid());
  const ProductObservation deadObservation = prepareProduct(deadPath.data(), dead.bytes);

  EXPECT_TRUE(completedWithSection(deadObservation));
  EXPECT_EQ(countOccurrences(deadObservation.section, "LiveForm"), 1U) << deadObservation.section;
  EXPECT_EQ(readsStartingInTargetSpan(deadObservation, deadPath, dead), 0U)
      << "an uninvoked Form body must not consume SD/CPU decode work";

  static constexpr std::string_view invokedPath = "/books/product-gap-invoked-malformed-form.pdf";
  const PdfWithTargetSpan invoked = malformedFormReadPdf(true);
  ASSERT_TRUE(invoked.valid());
  const ProductObservation invokedObservation = prepareProduct(invokedPath.data(), invoked.bytes);

  ASSERT_TRUE(invokedObservation.beginStatus.ok());
  ASSERT_TRUE(invokedObservation.terminal.failed());
  EXPECT_EQ(invokedObservation.terminal.status.error, PdfError::Malformed);
  EXPECT_GT(readsStartingInTargetSpan(invokedObservation, invokedPath, invoked), 0U)
      << "positive control: invoking the malformed Form must exercise its marked stream span";
}

TEST(PdfPreparationProductGapRed, RejectsAProductFormCycleByObjectIdentity) {
  const ProductObservation observation =
      prepareProduct("/books/product-gap-form-cycle.pdf", cyclicFormPdf());

  ASSERT_TRUE(observation.beginStatus.ok());
  ASSERT_TRUE(observation.terminal.failed());
  EXPECT_EQ(observation.terminal.status.error, PdfError::Malformed);
}

TEST(PdfPreparationProductGapRed, AcceptsExactFormDepthAndOmitsOnlyTheDeeperBranch) {
  const ProductObservation exact = prepareProduct(
      "/books/product-gap-form-depth-exact.pdf", nestedFormDepthPdf(PdfLimits::MaxFormDepth));
  ASSERT_TRUE(completedWithSection(exact));
  EXPECT_EQ(countOccurrences(exact.section, "DepthTarget"), 1U) << exact.section;

  const ProductObservation over = prepareProduct(
      "/books/product-gap-form-depth-over.pdf", nestedFormDepthPdf(PdfLimits::MaxFormDepth + 1U));
  ASSERT_TRUE(completedWithSection(over));
  EXPECT_EQ(countOccurrences(over.section, "RootText"), 1U) << over.section;
  EXPECT_EQ(countOccurrences(over.section, "DepthTarget"), 0U) << over.section;
}

TEST(PdfPreparationProductGapRed, AcceptsExactPerScopeFormResourceCapAndOmitsOnlyTheExcessForm) {
  const ProductObservation exact =
      prepareProduct("/books/product-gap-form-cap-exact.pdf",
                     formResourceCapPdf(PdfPreparedContentResources::MaxXObjects));
  ASSERT_TRUE(completedWithSection(exact));
  EXPECT_EQ(exact.words, PdfPreparedContentResources::MaxXObjects);
  EXPECT_EQ(countOccurrences(exact.section, "Cap00"), 1U) << exact.section;
  EXPECT_EQ(countOccurrences(exact.section, "Cap15"), 1U) << exact.section;

  const ProductObservation over =
      prepareProduct("/books/product-gap-form-cap-over.pdf",
                     formResourceCapPdf(PdfPreparedContentResources::MaxXObjects + 1U));
  ASSERT_TRUE(completedWithSection(over));
  EXPECT_EQ(over.words, PdfPreparedContentResources::MaxXObjects);
  EXPECT_EQ(countOccurrences(over.section, "Cap00"), 1U) << over.section;
  EXPECT_EQ(countOccurrences(over.section, "Cap15"), 1U) << over.section;
  EXPECT_EQ(countOccurrences(over.section, "Cap16"), 0U) << over.section;
}

TEST(PdfPreparationProductGapRed, DecodesOneFormBodyOnceWhenItIsInvokedTwice) {
  static constexpr std::string_view sourcePath = "/books/product-gap-form-decode-once.pdf";
  const PdfWithTargetSpan pdf = repeatedFormReadPdf();
  ASSERT_TRUE(pdf.valid());

  const ProductObservation observation = prepareProduct(sourcePath.data(), pdf.bytes);

  ASSERT_TRUE(completedWithSection(observation));
  EXPECT_EQ(countOccurrences(observation.section, "DecodedOnce"), 2U) << observation.section;
  EXPECT_EQ(readsStartingInTargetSpan(observation, sourcePath, pdf), 1U)
      << "repeated Do operators must share one decoded Form body";
}

TEST(PdfPreparationProductGapRed, ResolvesXrefStreamAndCompressedObjectStreamIntoCommittedXhtml) {
  const std::vector<uint8_t> pdf = loadFixture("xref_stream_objstm.pdf");
  ASSERT_FALSE(pdf.empty());
  const std::string source(pdf.begin(), pdf.end());
  ASSERT_NE(source.find("/Type /XRef"), std::string::npos);
  ASSERT_NE(source.find("/Type /ObjStm"), std::string::npos);

  const ProductObservation observation = prepareProduct("/books/product-gap-xref-objstm.pdf", pdf);

  ASSERT_TRUE(completedWithSection(observation));
  EXPECT_EQ(observation.words, 4U);
  EXPECT_NE(observation.section.find(">Compressed object stream text.</p>"), std::string::npos)
      << observation.section;
}

}  // namespace
