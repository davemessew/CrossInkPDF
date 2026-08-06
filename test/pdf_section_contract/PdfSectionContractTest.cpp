#include <array>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "PdfMetadataStore.h"
#include "PdfOutline.h"
#include "PdfPreparation.h"
#include "PdfTestCacheIo.h"
#include "PdfTestIo.h"

namespace {

constexpr char kNoOutlinePath[] = "/books/two-pages.pdf";
constexpr char kOutlinePath[] = "/books/outlined.pdf";
constexpr char kNonOutlineTargetPath[] = "/books/non-outline-target.pdf";
constexpr char kRectEdgePath[] = "/books/rect-edge.pdf";
constexpr char kRotatedCropPath[] = "/books/rotated-crop.pdf";
constexpr char kManyOutlinesPath[] = "/books/many-outlines.pdf";
constexpr char kLongNoOutlinePath[] = "/books/long-no-outline.pdf";
constexpr char kFirstPageText[] = "First page prose";
constexpr char kSecondPageText[] = "Second page prose";
constexpr char kLeadText[] = "Lead page";
constexpr char kNumericLinkText[] = "7319";
constexpr char kOutsideRectText[] = "Outside rectangle prose";
constexpr char kOutlineTitle[] = "Chapter destination";
constexpr char kDestinationText[] = "Destination body";
constexpr char kDestinationAnchor[] = "b00000003";
constexpr uint32_t kDestinationPageIndex = 2;
constexpr char kNonOutlineLinkText[] = "Jump to ordinary page";
constexpr char kNonOutlineDestinationText[] = "Ordinary target body";
constexpr char kLaterChapterText[] = "Later outlined chapter";
constexpr char kEdgeLinkText[] = "Exact rectangle edge";
constexpr char kEdgeOutsideText[] = "Just outside rectangle";
constexpr char kRotatedLinkText[] = "Rotated crop link";
constexpr char kRotatedDestinationText[] = "Rotated destination";
constexpr uint16_t kLongNoOutlinePages = 18;

struct Reporter {
  int failures = 0;

  void check(const std::string_view id, const bool condition, const std::string_view detail) {
    std::cout << (condition ? "PASS " : "FAIL ") << id << " - " << detail << '\n';
    if (!condition) {
      ++failures;
    }
  }
};

std::vector<uint8_t> makePdf(const std::vector<std::string>& objects) {
  std::string pdf = "%PDF-1.4\n";
  std::vector<size_t> offsets;
  offsets.reserve(objects.size());
  for (size_t index = 0; index < objects.size(); ++index) {
    offsets.push_back(pdf.size());
    pdf += std::to_string(index + 1U) + " 0 obj\n" + objects[index] + "\nendobj\n";
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

std::string streamObject(const std::string& stream) {
  return "<< /Length " + std::to_string(stream.size()) + " >>\nstream\n" + stream + "\nendstream";
}

std::vector<uint8_t> makeNoOutlinePdf() {
  const std::string first = "BT /F1 12 Tf 72 720 Td (" + std::string(kFirstPageText) + ") Tj ET";
  const std::string second = "BT /F1 12 Tf 72 720 Td (" + std::string(kSecondPageText) + ") Tj ET";
  return makePdf({
      "<< /Type /Catalog /Pages 2 0 R >>",
      "<< /Type /Pages /Count 2 /Kids [3 0 R 5 0 R] >>",
      "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] "
      "/Resources << /Font << /F1 7 0 R >> >> /Contents 4 0 R >>",
      streamObject(first),
      "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] "
      "/Resources << /Font << /F1 7 0 R >> >> /Contents 6 0 R >>",
      streamObject(second),
      "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>",
  });
}

std::vector<uint8_t> makeOutlinedLinkPdf() {
  const std::string first = "BT /F1 12 Tf 72 720 Td (" + std::string(kLeadText) + ") Tj ET";
  const std::string second =
      "BT /F1 12 Tf 72 720 Td (" + std::string(kNumericLinkText) + ") Tj ET\n"
      "BT /F1 12 Tf 72 660 Td (" + std::string(kOutsideRectText) + ") Tj ET";
  const std::string third = "BT /F1 12 Tf 72 720 Td (" + std::string(kDestinationText) + ") Tj ET";
  return makePdf({
      "<< /Type /Catalog /Pages 2 0 R /Outlines 10 0 R >>",
      "<< /Type /Pages /Count 3 /Kids [3 0 R 5 0 R 7 0 R] >>",
      "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] "
      "/Resources << /Font << /F1 9 0 R >> >> /Contents 4 0 R >>",
      streamObject(first),
      "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] "
      "/Resources << /Font << /F1 9 0 R >> >> /Contents 6 0 R /Annots [12 0 R] >>",
      streamObject(second),
      "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] "
      "/Resources << /Font << /F1 9 0 R >> >> /Contents 8 0 R >>",
      streamObject(third),
      "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>",
      "<< /Type /Outlines /First 11 0 R /Last 11 0 R /Count 1 >>",
      "<< /Title (" + std::string(kOutlineTitle) + ") /Parent 10 0 R /Dest [7 0 R /XYZ null 720 null] >>",
      "<< /Type /Annot /Subtype /Link /Rect [70 715 105 735] "
      "/Dest [7 0 R /XYZ null 720 null] >>",
  });
}

std::vector<uint8_t> makeNonOutlineTargetPdf() {
  const std::string first = "BT /F1 12 Tf 72 720 Td (" + std::string(kNonOutlineLinkText) + ") Tj ET";
  const std::string second =
      "BT /F1 12 Tf 72 720 Td (" + std::string(kNonOutlineDestinationText) + ") Tj ET";
  const std::string third = "BT /F1 12 Tf 72 720 Td (" + std::string(kLaterChapterText) + ") Tj ET";
  return makePdf({
      "<< /Type /Catalog /Pages 2 0 R /Outlines 10 0 R >>",
      "<< /Type /Pages /Count 3 /Kids [3 0 R 5 0 R 7 0 R] >>",
      "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] "
      "/Resources << /Font << /F1 9 0 R >> >> /Contents 4 0 R /Annots [12 0 R] >>",
      streamObject(first),
      "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] "
      "/Resources << /Font << /F1 9 0 R >> >> /Contents 6 0 R >>",
      streamObject(second),
      "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] "
      "/Resources << /Font << /F1 9 0 R >> >> /Contents 8 0 R >>",
      streamObject(third),
      "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>",
      "<< /Type /Outlines /First 11 0 R /Last 11 0 R /Count 1 >>",
      "<< /Title (Later chapter) /Parent 10 0 R /Dest [7 0 R /XYZ null 720 null] >>",
      "<< /Type /Annot /Subtype /Link /Rect [70 715 230 735] "
      "/Dest [5 0 R /XYZ null 720 null] >>",
  });
}

std::vector<uint8_t> makeRectEdgePdf() {
  const std::string first =
      "BT /F1 12 Tf 70 715 Td (" + std::string(kEdgeLinkText) + ") Tj ET\n" +
      "BT /F1 12 Tf 70 690 Td (" + std::string(kEdgeOutsideText) + ") Tj ET";
  const std::string second = "BT /F1 12 Tf 72 720 Td (" + std::string(kDestinationText) + ") Tj ET";
  return makePdf({
      "<< /Type /Catalog /Pages 2 0 R /Outlines 8 0 R >>",
      "<< /Type /Pages /Count 2 /Kids [3 0 R 5 0 R] >>",
      "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] "
      "/Resources << /Font << /F1 7 0 R >> >> /Contents 4 0 R /Annots [10 0 R] >>",
      streamObject(first),
      "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] "
      "/Resources << /Font << /F1 7 0 R >> >> /Contents 6 0 R >>",
      streamObject(second),
      "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>",
      "<< /Type /Outlines /First 9 0 R /Last 9 0 R /Count 1 >>",
      "<< /Title (Edge destination) /Parent 8 0 R /Dest [5 0 R /XYZ null 720 null] >>",
      "<< /Type /Annot /Subtype /Link /Rect [70 715 230 735] "
      "/Dest [5 0 R /XYZ null 720 null] >>",
  });
}

std::vector<uint8_t> makeRotatedCropPdf() {
  const std::string first = "BT /F1 12 Tf 120 650 Td (" + std::string(kRotatedLinkText) + ") Tj ET";
  const std::string second =
      "BT /F1 12 Tf 72 720 Td (" + std::string(kRotatedDestinationText) + ") Tj ET";
  return makePdf({
      "<< /Type /Catalog /Pages 2 0 R /Outlines 8 0 R >>",
      "<< /Type /Pages /Count 2 /Kids [3 0 R 5 0 R] >>",
      "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] /CropBox [100 200 500 700] /Rotate 90 "
      "/Resources << /Font << /F1 7 0 R >> >> /Contents 4 0 R /Annots [10 0 R] >>",
      streamObject(first),
      "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] "
      "/Resources << /Font << /F1 7 0 R >> >> /Contents 6 0 R >>",
      streamObject(second),
      "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>",
      "<< /Type /Outlines /First 9 0 R /Last 9 0 R /Count 1 >>",
      "<< /Title (Rotated destination) /Parent 8 0 R /Dest [5 0 R /XYZ null 720 null] >>",
      "<< /Type /Annot /Subtype /Link /Rect [115 640 250 670] "
      "/Dest [5 0 R /XYZ null 720 null] >>",
  });
}

std::string manyOutlineTitle(const uint16_t index) {
  char title[32]{};
  const int length = std::snprintf(title, sizeof(title), "Chapter %03u", static_cast<unsigned>(index + 1U));
  return length <= 0 || static_cast<size_t>(length) >= sizeof(title)
             ? std::string{}
             : std::string(title, static_cast<size_t>(length));
}

std::vector<uint8_t> makeManyOutlinesPdf(const uint16_t count) {
  if (count == 0) {
    return {};
  }
  constexpr uint16_t kPagesPerNode = 16;
  const uint16_t pageNodeCount = static_cast<uint16_t>((count + kPagesPerNode - 1U) / kPagesPerNode);
  const uint32_t firstPageNodeObject = 3U;
  const uint32_t firstPageObject = firstPageNodeObject + pageNodeCount;
  const uint32_t fontObject = firstPageObject + static_cast<uint32_t>(count) * 2U;
  const uint32_t outlineRootObject = fontObject + 1U;
  const uint32_t firstOutlineObject = outlineRootObject + 1U;
  std::string rootKids;
  for (uint16_t index = 0; index < pageNodeCount; ++index) {
    rootKids += std::to_string(firstPageNodeObject + index) + " 0 R ";
  }

  std::vector<std::string> objects;
  objects.reserve(static_cast<size_t>(count) * 3U + pageNodeCount + 3U);
  objects.push_back("<< /Type /Catalog /Pages 2 0 R /Outlines " + std::to_string(outlineRootObject) + " 0 R >>");
  objects.push_back("<< /Type /Pages /Count " + std::to_string(count) + " /Kids [" + rootKids + "] >>");
  for (uint16_t nodeIndex = 0; nodeIndex < pageNodeCount; ++nodeIndex) {
    const uint16_t firstPage = static_cast<uint16_t>(nodeIndex * kPagesPerNode);
    const uint16_t nodePages = std::min<uint16_t>(kPagesPerNode, static_cast<uint16_t>(count - firstPage));
    std::string kids;
    for (uint16_t pageOffset = 0; pageOffset < nodePages; ++pageOffset) {
      kids += std::to_string(firstPageObject + static_cast<uint32_t>(firstPage + pageOffset) * 2U) + " 0 R ";
    }
    objects.push_back("<< /Type /Pages /Parent 2 0 R /Count " + std::to_string(nodePages) + " /Kids [" + kids +
                      "] >>");
  }
  for (uint16_t index = 0; index < count; ++index) {
    const uint32_t pageObject = firstPageObject + static_cast<uint32_t>(index) * 2U;
    const uint32_t contentObject = pageObject + 1U;
    const uint32_t parentObject = firstPageNodeObject + index / kPagesPerNode;
    objects.push_back("<< /Type /Page /Parent " + std::to_string(parentObject) +
                      " 0 R /MediaBox [0 0 612 792] /Resources << /Font << /F1 " +
                      std::to_string(fontObject) + " 0 R >> >> /Contents " + std::to_string(contentObject) +
                      " 0 R >>");
    objects.push_back(streamObject("BT /F1 12 Tf 72 720 Td (" + manyOutlineTitle(index) + ") Tj ET"));
  }
  objects.push_back("<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>");
  objects.push_back("<< /Type /Outlines /First " + std::to_string(firstOutlineObject) + " 0 R /Last " +
                    std::to_string(firstOutlineObject + count - 1U) + " 0 R /Count " + std::to_string(count) +
                    " >>");
  for (uint16_t index = 0; index < count; ++index) {
    const uint32_t outlineObject = firstOutlineObject + index;
    const uint32_t pageObject = firstPageObject + static_cast<uint32_t>(index) * 2U;
    std::string links;
    if (index != 0) {
      links += " /Prev " + std::to_string(outlineObject - 1U) + " 0 R";
    }
    if (index + 1U < count) {
      links += " /Next " + std::to_string(outlineObject + 1U) + " 0 R";
    }
    objects.push_back("<< /Title (" + manyOutlineTitle(index) + ") /Parent " +
                      std::to_string(outlineRootObject) + " 0 R" + links + " /Dest [" +
                      std::to_string(pageObject) + " 0 R /XYZ null 720 null] >>");
  }
  return makePdf(objects);
}

std::string longPageText(const uint16_t index) {
  char text[40]{};
  const int length = std::snprintf(text, sizeof(text), "Long page %03u prose", static_cast<unsigned>(index + 1U));
  return length <= 0 || static_cast<size_t>(length) >= sizeof(text)
             ? std::string{}
             : std::string(text, static_cast<size_t>(length));
}

std::vector<uint8_t> makeLongNoOutlinePdf(const uint16_t count) {
  if (count == 0) {
    return {};
  }
  const uint32_t fontObject = 3U + static_cast<uint32_t>(count) * 2U;
  std::string kids;
  for (uint16_t index = 0; index < count; ++index) {
    kids += std::to_string(3U + static_cast<uint32_t>(index) * 2U) + " 0 R ";
  }
  std::vector<std::string> objects;
  objects.reserve(static_cast<size_t>(count) * 2U + 3U);
  objects.push_back("<< /Type /Catalog /Pages 2 0 R >>");
  objects.push_back("<< /Type /Pages /Count " + std::to_string(count) + " /Kids [" + kids + "] >>");
  for (uint16_t index = 0; index < count; ++index) {
    const uint32_t contentObject = 4U + static_cast<uint32_t>(index) * 2U;
    objects.push_back("<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] /Resources << /Font << /F1 " +
                      std::to_string(fontObject) + " 0 R >> >> /Contents " + std::to_string(contentObject) +
                      " 0 R >>");
    objects.push_back(streamObject("BT /F1 12 Tf 72 720 Td (" + longPageText(index) + ") Tj ET"));
  }
  objects.push_back("<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>");
  return makePdf(objects);
}

struct PreparationHarness {
  PdfTestCacheIo storage;
  PdfResourceSnapshot resources{128U * 1024U, 96U * 1024U, 8U * 1024U};
  uint32_t nowMs = 0;

  static uint32_t now(void* context) { return static_cast<PreparationHarness*>(context)->nowMs; }
  static PdfResourceSnapshot measure(void* context) {
    return static_cast<PreparationHarness*>(context)->resources;
  }
  static void resourceEvent(void*, const PdfResourceEvent&) {}

  PdfPreparationConfig config(const char* sourcePath) {
    return {
        storage.io(), sourcePath, "/.crosspoint", this, now, {this, measure, resourceEvent}, storage.renameCallback(),
        800,          480,
    };
  }
};

PdfStepResult runToTerminal(PdfPreparation& preparation, PreparationHarness& harness,
                            const uint32_t maximumSteps = 1000000U, const uint32_t millisecondsPerStep = 1U) {
  for (uint32_t step = 0; step < maximumSteps; ++step) {
    const PdfStepResult result = preparation.step();
    harness.nowMs += millisecondsPerStep;
    if (!result.yielded()) {
      return result;
    }
  }
  return PdfStepResult::failure(PdfStatus::failure(PdfError::BudgetExhausted));
}

struct Observation {
  bool beginSucceeded = false;
  bool completed = false;
  PdfStatus terminalStatus{};
  bool metadataCommitted = false;
  bool metadataDecoded = false;
  bool outlineCommitted = false;
  bool outlineDecoded = false;
  PdfMetadata metadata{};
  std::vector<PdfMetadataSection> sections;
  std::vector<PdfOutlineEntry> outline;
  std::vector<std::string> xhtml;
};

PdfStatus collectSection(void* context, uint16_t, const PdfMetadataSection& section) {
  if (context == nullptr) {
    return PdfStatus::failure(PdfError::InvalidArgument);
  }
  static_cast<std::vector<PdfMetadataSection>*>(context)->push_back(section);
  return PdfStatus::success();
}

PdfStatus collectOutline(void* context, uint16_t, const PdfOutlineEntry& entry) {
  if (context == nullptr) {
    return PdfStatus::failure(PdfError::InvalidArgument);
  }
  static_cast<std::vector<PdfOutlineEntry>*>(context)->push_back(entry);
  return PdfStatus::success();
}

Observation collectObservation(PdfPreparation& preparation, PreparationHarness& harness, const bool beginSucceeded,
                               const PdfStepResult terminal) {
  Observation observation;
  observation.beginSucceeded = beginSucceeded;
  observation.completed = terminal.complete();
  observation.terminalStatus = terminal.status;

  const std::string generationRoot = std::string(preparation.cacheRoot()) + "/gen_" +
                                     std::to_string(preparation.generation());
  const std::string metadataPath = generationRoot + "/metadata.bin";
  observation.metadataCommitted = harness.storage.exists(metadataPath);
  if (observation.metadataCommitted) {
    PdfTestByteSource source(harness.storage.bytes(metadataPath));
    observation.metadataDecoded =
        pdfDecodeMetadata(source.source(), &observation.metadata, {&observation.sections, collectSection}).ok();
  }

  const std::string outlinePath = generationRoot + "/outline.bin";
  observation.outlineCommitted = harness.storage.exists(outlinePath);
  if (observation.outlineCommitted) {
    PdfTestByteSource source(harness.storage.bytes(outlinePath));
    PdfOutlineHeader header{};
    observation.outlineDecoded =
        pdfDecodeOutline(source.source(), &header, {&observation.outline, collectOutline}).ok();
  }

  for (uint16_t index = 0; index < PdfMetadataLimits::MaxSections; ++index) {
    char leaf[64]{};
    const int length = std::snprintf(leaf, sizeof(leaf), "/sections/%06u.xhtml", index);
    if (length <= 0 || static_cast<size_t>(length) >= sizeof(leaf)) {
      break;
    }
    const std::string pathName = generationRoot + leaf;
    if (!harness.storage.exists(pathName)) {
      break;
    }
    const auto& bytes = harness.storage.bytes(pathName);
    observation.xhtml.emplace_back(bytes.begin(), bytes.end());
  }
  return observation;
}

Observation prepare(const char* path, std::vector<uint8_t> pdf) {
  PreparationHarness harness;
  harness.storage.addFile(path, std::move(pdf), 1234, true);

  PdfPreparation preparation;
  const PdfStatus begun = preparation.begin(harness.config(path));
  if (!begun) {
    Observation observation;
    observation.beginSucceeded = false;
    observation.terminalStatus = begun;
    return observation;
  }
  return collectObservation(preparation, harness, true, runToTerminal(preparation, harness));
}

bool contains(const std::string& value, const std::string_view expected) {
  return value.find(expected) != std::string::npos;
}

size_t countOccurrences(const std::string_view value, const std::string_view expected) {
  if (expected.empty()) {
    return 0;
  }
  size_t count = 0;
  size_t offset = 0;
  while ((offset = value.find(expected, offset)) != std::string_view::npos) {
    ++count;
    offset += expected.size();
  }
  return count;
}

bool containsInOrder(const std::string& value, const std::string_view first, const std::string_view second) {
  const size_t firstOffset = value.find(first);
  return firstOffset != std::string::npos && value.find(second, firstOffset + first.size()) != std::string::npos;
}

size_t observationOccurrences(const Observation& observation, const std::string_view text) {
  size_t count = 0;
  for (const std::string& xhtml : observation.xhtml) {
    count += countOccurrences(xhtml, text);
  }
  return count;
}

bool textPresent(const Observation& observation, const std::string_view text) {
  return observationOccurrences(observation, text) != 0;
}

std::string formatSectionHref(const uint16_t sectionIndex, const std::string_view anchor) {
  if (anchor.empty()) {
    return {};
  }
  char href[PdfOutlineLimits::HrefBytes]{};
  const int length = std::snprintf(href, sizeof(href), "sections/%06u.xhtml#%.*s", sectionIndex,
                                   static_cast<int>(anchor.size()), anchor.data());
  if (length <= 0 || static_cast<size_t>(length) >= sizeof(href)) {
    return {};
  }
  return std::string(href, static_cast<size_t>(length));
}

std::string formatPublisherPageAnchor(const uint32_t sourcePageIndex) {
  char anchor[11]{};
  const int length =
      std::snprintf(anchor, sizeof(anchor), "p%08lx", static_cast<unsigned long>(sourcePageIndex));
  return length == 9 ? std::string(anchor, static_cast<size_t>(length)) : std::string{};
}

bool continuousNoOutline(const Observation& observation) {
  return observation.metadataDecoded && observation.metadata.sectionCount == 1U && observation.xhtml.size() == 1U &&
         containsInOrder(observation.xhtml[0], kFirstPageText, kSecondPageText);
}

bool outlineDefinesOnlySectionBoundary(const Observation& observation) {
  return observation.metadataDecoded && observation.metadata.sectionCount == 2U && observation.sections.size() == 2U &&
         observation.sections[0].tocIndex == -1 && observation.sections[1].tocIndex == 0 &&
         observation.xhtml.size() == 2U &&
         containsInOrder(observation.xhtml[0], kLeadText, kNumericLinkText) &&
         containsInOrder(observation.xhtml[0], kNumericLinkText, kOutsideRectText) &&
         contains(observation.xhtml[1], kDestinationText);
}

std::string preparedOutlineDestinationHref(const Observation& observation) {
  const std::string anchorMarker = "id=\"" + std::string(kDestinationAnchor) + "\"";
  if (observationOccurrences(observation, kDestinationText) != 1U ||
      observationOccurrences(observation, anchorMarker) != 1U) {
    return {};
  }

  for (size_t index = 0; index < observation.xhtml.size(); ++index) {
    if (contains(observation.xhtml[index], kDestinationText) && contains(observation.xhtml[index], anchorMarker)) {
      if (index > UINT16_MAX) {
        return {};
      }
      return formatSectionHref(static_cast<uint16_t>(index), kDestinationAnchor);
    }
  }
  return {};
}

std::string preparedPageDestinationHref(const Observation& observation, const uint32_t sourcePageIndex,
                                        const std::string_view destinationText) {
  const std::string anchor = formatPublisherPageAnchor(sourcePageIndex);
  const std::string anchorMarker = "id=\"" + anchor + "\"";
  if (anchor.empty() || observationOccurrences(observation, destinationText) != 1U ||
      observationOccurrences(observation, anchorMarker) != 1U) {
    return {};
  }
  for (size_t index = 0; index < observation.xhtml.size(); ++index) {
    if (contains(observation.xhtml[index], destinationText) && contains(observation.xhtml[index], anchorMarker)) {
      return index <= UINT16_MAX ? formatSectionHref(static_cast<uint16_t>(index), anchor) : std::string{};
    }
  }
  return {};
}

std::string resolvedOutlineHref(const Observation& observation) {
  if (!observation.outlineDecoded || observation.outline.empty() || observation.outline[0].anchor[0] == '\0') {
    return {};
  }
  return formatSectionHref(observation.outline[0].sectionIndex, observation.outline[0].anchor);
}

bool outlineTargetsPreparedDestination(const Observation& observation) {
  const std::string preparedHref = preparedOutlineDestinationHref(observation);
  return !preparedHref.empty() && resolvedOutlineHref(observation) == preparedHref;
}

bool textLinkedToPreparedPageDestination(const Observation& observation, const std::string_view text,
                                         const uint32_t targetPageIndex, const std::string_view destinationText) {
  const std::string href = preparedPageDestinationHref(observation, targetPageIndex, destinationText);
  if (href.empty() || observationOccurrences(observation, text) != 1U) {
    return false;
  }
  const std::string expected = "<a href=\"" + href + "\">" + std::string(text) + "</a>";
  for (const std::string& xhtml : observation.xhtml) {
    if (contains(xhtml, expected)) {
      return true;
    }
  }
  return false;
}

bool textRemainsUnlinked(const Observation& observation, const std::string_view text) {
  if (observationOccurrences(observation, text) != 1U) {
    return false;
  }
  const std::string plain = ">" + std::string(text) + "</p>";
  const std::string linked = ">" + std::string(text) + "</a>";
  bool plainFound = false;
  for (const std::string& xhtml : observation.xhtml) {
    plainFound = plainFound || contains(xhtml, plain);
    if (contains(xhtml, linked)) {
      return false;
    }
  }
  return plainFound;
}

bool outlinedPreparationCommitted(const Observation& observation) {
  return observation.completed && observation.terminalStatus.ok() && observation.metadataCommitted &&
         observation.metadataDecoded && observation.outlineCommitted && observation.outlineDecoded &&
         !observation.outline.empty();
}

Observation syntheticObservation(const std::vector<std::string>& xhtml, const uint16_t sectionCount) {
  Observation observation;
  observation.metadataDecoded = true;
  observation.metadata.sectionCount = sectionCount;
  observation.sections.resize(sectionCount);
  if (sectionCount > 1U) {
    observation.sections[1].tocIndex = 0;
  }
  observation.xhtml = xhtml;
  observation.outlineDecoded = true;
  PdfOutlineEntry entry{};
  entry.sectionIndex = 1;
  std::strcpy(entry.anchor, kDestinationAnchor);
  observation.outline.push_back(entry);
  return observation;
}

struct PreparedSectionLayoutControl {
  PdfMetadataSection section{};
  PdfRequiredFileRecord file{};
  uint32_t firstSourcePage = 0;
  uint32_t lastSourcePageExclusive = 0;
};

struct PreparedLinkLayoutControl {
  uint16_t sourcePage = 0;
  uint16_t targetPage = 0;
  uint16_t xMin = 0;
  uint16_t yMin = 0;
  uint16_t xMax = 0;
  uint16_t yMax = 0;
};

enum class PrefixCrashStage : uint8_t {
  AfterPrefixSyncBeforeJournal,
  AfterJournalBeforeCheckpoint,
  AfterCheckpoint,
};

bool prefixResumeSelectionMatches(const PrefixCrashStage stage, const uint32_t baselinePage,
                                  const uint32_t publishedPage, const uint32_t selectedPage,
                                  const PdfBuildResumePhase selectedPhase) {
  if (selectedPhase != PdfBuildResumePhase::AfterPage) {
    return false;
  }
  if (stage == PrefixCrashStage::AfterCheckpoint) {
    return publishedPage > baselinePage && selectedPage == publishedPage;
  }
  return publishedPage == baselinePage && selectedPage == baselinePage;
}

static_assert(sizeof(PreparedSectionLayoutControl) == 152);
static_assert(sizeof(PreparedLinkLayoutControl) == 12);

std::string readRepositoryText(const std::string_view relativePath) {
  const std::string path = std::string(PDF_SECTION_CONTRACT_REPO_ROOT) + "/" + std::string(relativePath);
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    return {};
  }
  std::ostringstream contents;
  contents << input.rdbuf();
  return contents.str();
}

std::string withoutWhitespace(const std::string_view source) {
  std::string compact;
  compact.reserve(source.size());
  for (const char value : source) {
    if (!std::isspace(static_cast<unsigned char>(value))) {
      compact.push_back(value);
    }
  }
  return compact;
}

bool preparedSectionLayoutDeclared(const std::string_view header, const std::string_view implementation) {
  const std::string compactHeader = withoutWhitespace(header);
  const std::string compactImplementation = withoutWhitespace(implementation);
  return contains(compactHeader, "structPreparedSectionRecord{") &&
         contains(compactHeader, "uint32_tfirstSourcePage=") &&
         contains(compactHeader, "uint32_tlastSourcePageExclusive=") &&
         contains(compactImplementation, "static_assert(sizeof(PreparedSectionRecord)==152");
}

bool preparedLinkLayoutDeclared(const std::string_view implementation) {
  const std::string compact = withoutWhitespace(implementation);
  return contains(compact, "structPreparedLink{") && contains(compact, "uint16_tsourcePage") &&
         contains(compact, "uint16_ttargetPage") && contains(compact, "uint16_txMin") &&
         contains(compact, "uint16_tyMin") && contains(compact, "uint16_txMax") &&
         contains(compact, "uint16_tyMax") && contains(compact, "static_assert(sizeof(PreparedLink)==12");
}

bool navigationLayoutDeclared(const std::string_view implementation) {
  return contains(withoutWhitespace(implementation), "static_assert(sizeof(NavigationWorkspace)==14568");
}

bool navigationSpillMappingDeclared(const std::string_view implementation) {
  const std::string compact = withoutWhitespace(implementation);
  return contains(compact, "static_assert(PdfLimits::PageTextBytes==8192") &&
         contains(compact, "static_assert(runTailBytes==6376") &&
         contains(compact, "static_assert(operandSpillBytes==0");
}

int runLayoutContract() {
  Reporter report;
  const std::string header = readRepositoryText("lib/PdfReflow/PdfPreparation.h");
  const std::string implementation = readRepositoryText("lib/PdfReflow/PdfPreparation.cpp");
  const std::string knownGoodHeader =
      "struct PreparedSectionRecord { uint32_t firstSourcePage = 0; "
      "uint32_t lastSourcePageExclusive = 0; };";
  const std::string knownGoodImplementation =
      "struct PreparedLink { uint16_t sourcePage; uint16_t targetPage; uint16_t xMin; uint16_t yMin; "
      "uint16_t xMax; uint16_t yMax; }; "
      "static_assert(sizeof(PreparedSectionRecord) == 152); static_assert(sizeof(PreparedLink) == 12); "
      "static_assert(sizeof(NavigationWorkspace) == 14568); "
      "static_assert(PdfLimits::PageTextBytes == 8192); static_assert(runTailBytes == 6376); "
      "static_assert(operandSpillBytes == 0);";
  const std::string knownOldHeader =
      "struct PreparedPageRecord { uint32_t sourcePageIndex; uint32_t firstAnchor; };";
  const std::string knownOldImplementation =
      "struct PreparedLink { PdfRawDestination destination; }; static_assert(sizeof(NavigationWorkspace) == 16920);";

  report.check("LAYOUT_ORACLE_POSITIVE",
               preparedSectionLayoutDeclared(knownGoodHeader, knownGoodImplementation) &&
                   preparedLinkLayoutDeclared(knownGoodImplementation) &&
                   navigationLayoutDeclared(knownGoodImplementation) &&
                   navigationSpillMappingDeclared(knownGoodImplementation),
               "the source oracle accepts the exact 152/12/14568/8192+6376+0 contract");
  report.check("LAYOUT_ORACLE_NEGATIVE",
               !preparedSectionLayoutDeclared(knownOldHeader, knownOldImplementation) &&
                   !preparedLinkLayoutDeclared(knownOldImplementation) &&
                   !navigationLayoutDeclared(knownOldImplementation) &&
                   !navigationSpillMappingDeclared(knownOldImplementation),
               "the source oracle rejects the former page record, destination-heavy link, and 16920-byte workspace");
  report.check("LAYOUT_SOURCE_CONTROL", !header.empty() && !implementation.empty(),
               "the contract loaded the production header and implementation");
  report.check("PREPARED_SECTION_LAYOUT_152", preparedSectionLayoutDeclared(header, implementation),
               "production declares and asserts the 152-byte logical-section record");
  report.check("PREPARED_LINK_LAYOUT_12", preparedLinkLayoutDeclared(implementation),
               "production declares and asserts the 12-byte page-and-Rect link record");
  report.check("NAVIGATION_WORKSPACE_LAYOUT_14568", navigationLayoutDeclared(implementation),
               "production asserts the exact reduced RV32 navigation workspace");
  report.check("NAVIGATION_SPILL_MAPPING_8192_6376_0", navigationSpillMappingDeclared(implementation),
               "production asserts page-text, run-tail, and zero operand spill partitions");
  return report.failures == 0 ? 0 : 1;
}

int runCommitControl() {
  Reporter report;
  const Observation observation = prepare(kNoOutlinePath, makeNoOutlinePdf());
  report.check("COMMIT_BEGIN", observation.beginSucceeded, "production preparation accepted the in-memory PDF");
  report.check("COMMIT_TERMINAL", observation.completed, "production preparation reached committed completion");
  report.check("COMMIT_METADATA", observation.metadataCommitted && observation.metadataDecoded,
               "metadata.bin exists and decodes through production format code");
  report.check("COMMIT_XHTML", !observation.xhtml.empty() && contains(observation.xhtml[0], kFirstPageText),
               "at least one committed XHTML section contains extracted PDF text");
  return report.failures == 0 ? 0 : 1;
}

int runOracleControls() {
  Reporter report;
  constexpr uint32_t baselinePage = 8U;
  constexpr uint32_t publishedPage = 16U;
  report.check("CONTROL_PREFIX_AFTER_CHECKPOINT_POSITIVE",
               prefixResumeSelectionMatches(PrefixCrashStage::AfterCheckpoint, baselinePage, publishedPage,
                                             publishedPage, PdfBuildResumePhase::AfterPage),
               "the checkpoint-window oracle accepts the exact newly published page cursor");
  report.check("CONTROL_PREFIX_AFTER_CHECKPOINT_TAIL_NEGATIVE",
               !prefixResumeSelectionMatches(PrefixCrashStage::AfterCheckpoint, baselinePage, publishedPage,
                                              publishedPage + 1U, PdfBuildResumePhase::AfterPage),
               "an unpublished journal tail cannot masquerade as the selected checkpoint");
  report.check("CONTROL_PREFIX_AFTER_JOURNAL_TAIL_NEGATIVE",
               !prefixResumeSelectionMatches(PrefixCrashStage::AfterJournalBeforeCheckpoint, baselinePage,
                                              baselinePage, baselinePage + 1U, PdfBuildResumePhase::AfterPage),
               "a closed journal tail remains unpublished until its checkpoint is committed");
  const std::string publisherAnchor = formatPublisherPageAnchor(kDestinationPageIndex);
  const std::string pageDestinationHref = formatSectionHref(1U, publisherAnchor);
  const std::string destinationXhtml =
      "<span id=\"" + publisherAnchor + "\" role=\"doc-pagebreak\"></span><p id=\"" +
      std::string(kDestinationAnchor) + "\">" + std::string(kDestinationText) + "</p>";
  report.check("CONTROL_PUBLISHER_PAGE_ANCHOR", formatPublisherPageAnchor(0x2aU) == "p0000002a",
               "publisher-page destinations use the production p plus eight-lowercase-hex format");
  const Observation continuous = syntheticObservation(
      {"<html><body><p>" + std::string(kFirstPageText) + "</p><p>" + std::string(kSecondPageText) +
       "</p></body></html>"},
      1);
  report.check("CONTROL_CONTINUOUS_POSITIVE", continuousNoOutline(continuous),
               "known-good continuous observation satisfies the no-outline oracle");
  const Observation forcedPageSplit = syntheticObservation(
      {"<html><body><p>" + std::string(kFirstPageText) + "</p></body></html>",
       "<html><body><p>" + std::string(kSecondPageText) + "</p></body></html>"},
      2);
  report.check("CONTROL_CONTINUOUS_NEGATIVE", !continuousNoOutline(forcedPageSplit),
               "forced per-page split is rejected by the no-outline oracle");
  const Observation reversedContinuous = syntheticObservation(
      {"<html><body><p>" + std::string(kSecondPageText) + "</p><p>" + std::string(kFirstPageText) +
       "</p></body></html>"},
      1);
  report.check("CONTROL_CONTINUOUS_ORDER_NEGATIVE", !continuousNoOutline(reversedContinuous),
               "reversed page order is rejected by the no-outline oracle");

  const std::string outlinedFirst =
      "<p>" + std::string(kLeadText) + "</p><p><a href=\"" + pageDestinationHref + "\">" +
      std::string(kNumericLinkText) + "</a></p><p>" + std::string(kOutsideRectText) + "</p>";
  const Observation outlined = syntheticObservation({outlinedFirst, destinationXhtml}, 2);
  report.check("CONTROL_OUTLINE_POSITIVE", outlineDefinesOnlySectionBoundary(outlined),
               "known-good outline observation joins ordinary pages and splits at the outline");
  const Observation outlinedPerPage = syntheticObservation(
      {"<p>" + std::string(kLeadText) + "</p>",
       "<p>" + std::string(kNumericLinkText) + "</p><p>" + std::string(kOutsideRectText) + "</p>",
       destinationXhtml},
      3);
  report.check("CONTROL_OUTLINE_NEGATIVE", !outlineDefinesOnlySectionBoundary(outlinedPerPage),
               "forced ordinary-page split is rejected by the outline-boundary oracle");
  const Observation reversedOutlined = syntheticObservation(
      {"<p>" + std::string(kOutsideRectText) + "</p><p>" + std::string(kNumericLinkText) + "</p><p>" +
           std::string(kLeadText) + "</p>",
       destinationXhtml},
      2);
  report.check("CONTROL_OUTLINE_ORDER_NEGATIVE", !outlineDefinesOnlySectionBoundary(reversedOutlined),
               "reversed text order is rejected by the outline-boundary oracle");
  report.check("CONTROL_DESTINATION_POSITIVE", outlineTargetsPreparedDestination(outlined),
               "known-good outline href resolves to an anchor in destination XHTML");
  Observation wrongOutline = outlined;
  wrongOutline.outline[0].sectionIndex = 0;
  report.check("CONTROL_DESTINATION_NEGATIVE", !outlineTargetsPreparedDestination(wrongOutline),
               "an outline href that disagrees with destination XHTML is rejected");
  report.check("CONTROL_NUMERIC_LINK_POSITIVE",
               textLinkedToPreparedPageDestination(outlined, kNumericLinkText, kDestinationPageIndex,
                                                     kDestinationText),
               "known-good numeric Rect text resolves to the target source-page anchor");
  const Observation missingNumericLink = syntheticObservation(
      {"<p>" + std::string(kLeadText) + "</p><p>" + std::string(kNumericLinkText) + "</p><p>" +
           std::string(kOutsideRectText) + "</p>",
       destinationXhtml},
      2);
  report.check("CONTROL_NUMERIC_LINK_NEGATIVE",
                !textLinkedToPreparedPageDestination(missingNumericLink, kNumericLinkText, kDestinationPageIndex,
                                                      kDestinationText),
                "plain numeric text is rejected by the link oracle");
  const Observation danglingNumericLink = syntheticObservation(
      {outlinedFirst,
       "<p id=\"" + std::string(kDestinationAnchor) + "\">" + std::string(kDestinationText) + "</p>"},
      2);
  report.check("CONTROL_NUMERIC_DANGLING_NEGATIVE",
                !textLinkedToPreparedPageDestination(danglingNumericLink, kNumericLinkText, kDestinationPageIndex,
                                                      kDestinationText),
                "a numeric link without the matching source-page anchor is rejected");
  report.check("CONTROL_OUTSIDE_POSITIVE", textRemainsUnlinked(outlined, kOutsideRectText),
               "known-good outside-Rect text remains plain XHTML");
  const Observation overlinkedOutside = syntheticObservation(
      {"<p>" + std::string(kLeadText) + "</p><p>" + std::string(kNumericLinkText) +
            "</p><p><a href=\"" + pageDestinationHref + "\">" + std::string(kOutsideRectText) + "</a></p>",
       destinationXhtml},
      2);
  report.check("CONTROL_OUTSIDE_NEGATIVE", !textRemainsUnlinked(overlinkedOutside, kOutsideRectText),
               "outside-Rect text wrapped in a link is rejected by the exclusion oracle");
  const Observation duplicatedOutside = syntheticObservation(
      {"<p>" + std::string(kLeadText) + "</p><p>" + std::string(kNumericLinkText) + "</p><p>" +
            std::string(kOutsideRectText) + "</p><p><a href=\"" + pageDestinationHref + "\">" +
            std::string(kOutsideRectText) + "</a></p>",
       destinationXhtml},
      2);
  report.check("CONTROL_OUTSIDE_MIXED_NEGATIVE", !textRemainsUnlinked(duplicatedOutside, kOutsideRectText),
               "mixed plain and linked copies are rejected by the exclusion oracle");

  Observation committedOutlined = outlined;
  committedOutlined.completed = true;
  committedOutlined.terminalStatus = PdfStatus::success();
  committedOutlined.metadataCommitted = true;
  committedOutlined.outlineCommitted = true;
  report.check("CONTROL_COMMITTED_POSITIVE", outlinedPreparationCommitted(committedOutlined),
               "a completed outlined observation satisfies the commit prerequisite");
  committedOutlined.completed = false;
  report.check("CONTROL_COMMITTED_NEGATIVE", !outlinedPreparationCommitted(committedOutlined),
               "partial outlined output is rejected by the commit prerequisite");

  const std::vector<uint8_t> fixtureBytes = makeOutlinedLinkPdf();
  const std::string fixture(fixtureBytes.begin(), fixtureBytes.end());
  report.check("CONTROL_NUMERIC_FIXTURE_PROVENANCE", countOccurrences(fixture, kNumericLinkText) == 1U,
               "numeric page text has exactly one provenance in the raw PDF fixture");
  report.check("CONTROL_OUTSIDE_FIXTURE_PROVENANCE", countOccurrences(fixture, kOutsideRectText) == 1U,
               "outside-Rect page text has exactly one provenance in the raw PDF fixture");
  return report.failures == 0 ? 0 : 1;
}

int runNoOutlineContract() {
  Reporter report;
  const Observation observation = prepare(kNoOutlinePath, makeNoOutlinePdf());
  report.check("NO_OUTLINE_COMMIT_CONTROL",
               observation.completed && observation.terminalStatus.ok() && observation.metadataCommitted &&
                   observation.metadataDecoded && textPresent(observation, kFirstPageText) &&
                   textPresent(observation, kSecondPageText),
               "the two-page fixture committed metadata and both page texts before section assertions");
  report.check("NO_OUTLINE_CONTINUOUS_SECTION", continuousNoOutline(observation),
               "two ordinary PDF pages must commit as one XHTML reader section");
  std::cout << "OBSERVED sections=" << observation.metadata.sectionCount << " xhtml=" << observation.xhtml.size()
            << '\n';
  return report.failures == 0 ? 0 : 1;
}

int runOutlineBoundaryContract() {
  Reporter report;
  const Observation observation = prepare(kOutlinePath, makeOutlinedLinkPdf());
  report.check("OUTLINE_COMMIT_CONTROL",
               outlinedPreparationCommitted(observation) && textPresent(observation, kLeadText) &&
                   textPresent(observation, kDestinationText),
               "the outlined fixture committed metadata, outline, and endpoint text before section assertions");
  report.check("OUTLINE_ONLY_SECTION_BOUNDARY", outlineDefinesOnlySectionBoundary(observation),
               "page one and page two must stay joined; the page-three outline destination must start section two");
  std::cout << "OBSERVED sections=" << observation.metadata.sectionCount << " xhtml=" << observation.xhtml.size()
            << '\n';
  return report.failures == 0 ? 0 : 1;
}

int runNumericRectLinkContract() {
  Reporter report;
  const Observation observation = prepare(kOutlinePath, makeOutlinedLinkPdf());
  report.check("NUMERIC_COMMIT_CONTROL", outlinedPreparationCommitted(observation),
               "the outlined fixture reached committed completion with decoded metadata and outline");
  report.check("NUMERIC_DESTINATION_CONTROL", outlineTargetsPreparedDestination(observation),
               "the decoded outline resolves to an anchor in the prepared destination XHTML");
  report.check("NUMERIC_RECT_TEXT_PRESENT", textPresent(observation, kNumericLinkText),
               "the numeric annotation text is present in committed XHTML");
  report.check("NUMERIC_RECT_TEXT_LINKED",
               textLinkedToPreparedPageDestination(observation, kNumericLinkText, kDestinationPageIndex,
                                                     kDestinationText),
               "numeric text inside the annotation Rect must link to the target source-page anchor");
  std::cout << "OBSERVED outline_href=" << resolvedOutlineHref(observation)
             << " outline_prepared_href=" << preparedOutlineDestinationHref(observation)
             << " page_prepared_href="
             << preparedPageDestinationHref(observation, kDestinationPageIndex, kDestinationText)
             << " numeric_linked="
             << textLinkedToPreparedPageDestination(observation, kNumericLinkText, kDestinationPageIndex,
                                                     kDestinationText)
             << " outside_linked="
             << textLinkedToPreparedPageDestination(observation, kOutsideRectText, kDestinationPageIndex,
                                                     kDestinationText)
             << '\n';
  return report.failures == 0 ? 0 : 1;
}

int runOutsideRectContract() {
  Reporter report;
  const Observation observation = prepare(kOutlinePath, makeOutlinedLinkPdf());
  report.check("OUTSIDE_RECT_COMMIT_CONTROL", outlinedPreparationCommitted(observation),
               "the outlined fixture reached committed completion with decoded metadata and outline");
  report.check("OUTSIDE_RECT_TEXT_PRESENT", textPresent(observation, kOutsideRectText),
               "the outside-Rect page text is present in committed XHTML");
  report.check("OUTSIDE_RECT_TEXT_UNLINKED", textRemainsUnlinked(observation, kOutsideRectText),
               "page text outside the annotation Rect must remain unlinked");
  std::cout << "OBSERVED outside_linked="
             << textLinkedToPreparedPageDestination(observation, kOutsideRectText, kDestinationPageIndex,
                                                     kDestinationText)
             << " outside_plain=" << textRemainsUnlinked(observation, kOutsideRectText) << '\n';
  return report.failures == 0 ? 0 : 1;
}

int runNonOutlineTargetContract() {
  Reporter report;
  const Observation observation = prepare(kNonOutlineTargetPath, makeNonOutlineTargetPdf());
  report.check("NON_OUTLINE_TARGET_COMMIT_CONTROL",
               outlinedPreparationCommitted(observation) && textPresent(observation, kNonOutlineLinkText) &&
                   textPresent(observation, kNonOutlineDestinationText) && textPresent(observation, kLaterChapterText),
               "the fixture committed source text, ordinary target text, and its later outline chapter");
  report.check("NON_OUTLINE_TARGET_SECTION_CONTROL",
               observation.metadataDecoded && observation.metadata.sectionCount == 2U && observation.xhtml.size() == 2U &&
                   containsInOrder(observation.xhtml[0], kNonOutlineLinkText, kNonOutlineDestinationText) &&
                   contains(observation.xhtml[1], kLaterChapterText),
               "the ordinary target stays in section zero while only the later outline starts section one");
  report.check("NON_OUTLINE_TARGET_PAGE_ANCHOR",
               !preparedPageDestinationHref(observation, 1U, kNonOutlineDestinationText).empty(),
               "the ordinary target page exposes its deterministic publisher-page anchor");
  report.check("NON_OUTLINE_TARGET_LINKED",
               textLinkedToPreparedPageDestination(observation, kNonOutlineLinkText, 1U,
                                                     kNonOutlineDestinationText),
               "an internal link may target a source page that is not an outline boundary");
  std::cout << "OBSERVED sections=" << observation.metadata.sectionCount
            << " target_href=" << preparedPageDestinationHref(observation, 1U, kNonOutlineDestinationText)
            << " linked="
            << textLinkedToPreparedPageDestination(observation, kNonOutlineLinkText, 1U,
                                                    kNonOutlineDestinationText)
            << '\n';
  return report.failures == 0 ? 0 : 1;
}

int runRectEdgeContract() {
  Reporter report;
  const Observation observation = prepare(kRectEdgePath, makeRectEdgePdf());
  report.check("RECT_EDGE_COMMIT_CONTROL",
               outlinedPreparationCommitted(observation) && textPresent(observation, kEdgeLinkText) &&
                   textPresent(observation, kEdgeOutsideText) && textPresent(observation, kDestinationText),
               "the exact-edge fixture committed both source blocks and the destination");
  report.check("RECT_EDGE_INCLUSIVE",
               textLinkedToPreparedPageDestination(observation, kEdgeLinkText, 1U, kDestinationText),
               "a block origin exactly on the annotation lower-left edge is inside the Rect");
  report.check("RECT_EDGE_OUTSIDE_UNLINKED", textRemainsUnlinked(observation, kEdgeOutsideText),
               "a block origin below the annotation Rect remains plain text");
  std::cout << "OBSERVED edge_linked="
            << textLinkedToPreparedPageDestination(observation, kEdgeLinkText, 1U, kDestinationText)
            << " outside_plain=" << textRemainsUnlinked(observation, kEdgeOutsideText) << '\n';
  return report.failures == 0 ? 0 : 1;
}

int runRotatedCropContract() {
  Reporter report;
  const Observation observation = prepare(kRotatedCropPath, makeRotatedCropPdf());
  report.check("ROTATED_CROP_COMMIT_CONTROL",
               outlinedPreparationCommitted(observation) && textPresent(observation, kRotatedLinkText) &&
                   textPresent(observation, kRotatedDestinationText),
               "the rotated CropBox fixture committed source and destination text");
  report.check("ROTATED_CROP_RECT_LINKED",
               textLinkedToPreparedPageDestination(observation, kRotatedLinkText, 1U, kRotatedDestinationText),
               "annotation Rect and extracted origin use the same crop-and-rotation transform");
  std::cout << "OBSERVED rotated_href="
            << preparedPageDestinationHref(observation, 1U, kRotatedDestinationText) << " linked="
            << textLinkedToPreparedPageDestination(observation, kRotatedLinkText, 1U, kRotatedDestinationText)
            << '\n';
  return report.failures == 0 ? 0 : 1;
}

bool manyOutlineOrderPreserved(const Observation& observation, const uint16_t expectedCount) {
  if (!observation.outlineDecoded || observation.outline.size() != expectedCount) {
    return false;
  }
  for (uint16_t index = 0; index < expectedCount; ++index) {
    const PdfOutlineEntry& entry = observation.outline[index];
    const std::string title = manyOutlineTitle(index);
    if (entry.titleLength != title.size() || std::memcmp(entry.title, title.data(), title.size()) != 0 ||
        entry.sourcePageIndex != index || entry.sectionIndex != index || entry.parentIndex != -1 || entry.level != 1) {
      return false;
    }
  }
  return true;
}

int runOutlineCapacityContract() {
  Reporter report;
  constexpr uint16_t expectedCount = PdfOutlineLimits::MaxEntries;
  const Observation observation = prepare(kManyOutlinesPath, makeManyOutlinesPdf(expectedCount));
  report.check("OUTLINE_CAPACITY_STORED_LIMIT_CONTROL", expectedCount == 256U,
               "the contract exercises the complete stored outline capacity");
  report.check("OUTLINE_CAPACITY_COMMIT",
               observation.completed && observation.terminalStatus.ok() && observation.metadataDecoded &&
                   observation.outlineDecoded,
               "preparation commits rather than failing at the former 32-entry workspace limit");
  report.check("OUTLINE_CAPACITY_ALL_ENTRIES", manyOutlineOrderPreserved(observation, expectedCount),
               "all 256 sibling entries preserve source order, hierarchy, and target section");
  report.check("OUTLINE_CAPACITY_SECTIONS",
               observation.metadataDecoded && observation.metadata.sectionCount == expectedCount &&
                   observation.xhtml.size() == expectedCount,
               "256 distinct target pages produce the bounded 256 logical sections");
  std::cout << "OBSERVED completed=" << observation.completed << " error="
            << static_cast<unsigned>(observation.terminalStatus.error)
            << " offset=" << observation.terminalStatus.offset
            << " outline=" << observation.outline.size()
            << " sections=" << observation.metadata.sectionCount << '\n';
  return report.failures == 0 ? 0 : 1;
}

void cloneStorage(const PdfTestCacheIo& source, PdfTestCacheIo* const destination) {
  if (destination == nullptr) {
    return;
  }
  for (const std::string& path : source.paths()) {
    if (source.isDirectory(path)) {
      destination->addDirectory(path);
    } else if (path == kLongNoOutlinePath) {
      destination->addFile(path, source.bytes(path), 1234, true);
    } else {
      destination->addFile(path, source.bytes(path));
    }
  }
}

bool buildPrefixBaseline(PdfTestCacheIo* const snapshot, uint32_t* const durablePage) {
  if (snapshot == nullptr || durablePage == nullptr) {
    return false;
  }
  PreparationHarness harness;
  harness.storage.addFile(kLongNoOutlinePath, makeLongNoOutlinePdf(kLongNoOutlinePages), 1234, true);
  PdfPreparation preparation;
  if (!preparation.begin(harness.config(kLongNoOutlinePath))) {
    return false;
  }
  for (uint32_t step = 0; step < 1000000U; ++step) {
    const PdfStepResult result = preparation.step();
    harness.nowMs += 100U;
    if (preparation.durableResumePage() >= PDF_CACHE_CHECKPOINT_PAGE_INTERVAL) {
      *durablePage = preparation.durableResumePage();
      cloneStorage(harness.storage, snapshot);
      return true;
    }
    if (!result.yielded()) {
      return false;
    }
  }
  return false;
}

bool sectionZeroWasSynced(const PdfTestCacheIo& storage) {
  for (const std::string& path : storage.syncObservations()) {
    if (std::string_view(path).ends_with("/sections/000000.xhtml")) {
      return true;
    }
  }
  return false;
}

bool resumeJournalWasClosed(const PdfTestCacheIo& storage) {
  for (const std::string& event : storage.events()) {
    if (std::string_view(event).starts_with("close:") && std::string_view(event).ends_with("/resume.journal")) {
      return true;
    }
  }
  return false;
}

bool capturePrefixCrash(const PdfTestCacheIo& baseline, const uint32_t baselinePage, const PrefixCrashStage stage,
                        PdfTestCacheIo* const snapshot, uint32_t* const publishedPage) {
  if (snapshot == nullptr || publishedPage == nullptr) {
    return false;
  }
  PreparationHarness harness;
  cloneStorage(baseline, &harness.storage);
  PdfPreparation preparation;
  if (!preparation.begin(harness.config(kLongNoOutlinePath))) {
    return false;
  }
  bool resumed = false;
  for (uint32_t step = 0; step < 1000000U; ++step) {
    const PdfStepResult result = preparation.step();
    harness.nowMs += 100U;
    if (!resumed && preparation.resumedFromCheckpoint()) {
      resumed = true;
      harness.storage.clearSyncObservations();
      harness.storage.clearEvents();
    }
    if (resumed) {
      const bool beforeJournal = stage == PrefixCrashStage::AfterPrefixSyncBeforeJournal &&
                                 preparation.phase() == PdfPreparationPhase::CommitResumePoint &&
                                 preparation.durableResumePage() == baselinePage && sectionZeroWasSynced(harness.storage);
      const bool afterJournal = stage == PrefixCrashStage::AfterJournalBeforeCheckpoint &&
                                preparation.phase() == PdfPreparationPhase::CommitResumePoint &&
                                preparation.durableResumePage() == baselinePage &&
                                resumeJournalWasClosed(harness.storage);
      const bool afterCheckpoint = stage == PrefixCrashStage::AfterCheckpoint &&
                                   preparation.durableResumePage() >=
                                       baselinePage + PDF_CACHE_CHECKPOINT_PAGE_INTERVAL;
      if (beforeJournal || afterJournal || afterCheckpoint) {
        *publishedPage = preparation.durableResumePage();
        cloneStorage(harness.storage, snapshot);
        return true;
      }
    }
    if (!result.yielded()) {
      return false;
    }
  }
  return false;
}

bool longDocumentExactlyOnce(const Observation& observation) {
  if (!observation.completed || !observation.terminalStatus.ok() || !observation.metadataDecoded ||
      observation.metadata.sectionCount != 1U || observation.xhtml.size() != 1U) {
    return false;
  }
  size_t previous = 0;
  for (uint16_t index = 0; index < kLongNoOutlinePages; ++index) {
    const std::string text = longPageText(index);
    if (countOccurrences(observation.xhtml[0], text) != 1U) {
      return false;
    }
    const size_t offset = observation.xhtml[0].find(text, previous);
    if (offset == std::string::npos) {
      return false;
    }
    previous = offset + text.size();
  }
  return true;
}

int runPrefixCrashContract(const PrefixCrashStage stage, const std::string_view id) {
  Reporter report;
  PdfTestCacheIo baseline;
  uint32_t baselinePage = 0;
  const bool baselineReady = buildPrefixBaseline(&baseline, &baselinePage);
  report.check(std::string(id) + "_BASELINE", baselineReady && baselinePage >= PDF_CACHE_CHECKPOINT_PAGE_INTERVAL,
               "a long no-outline build published its first debounced resumable prefix");
  if (!baselineReady) {
    return 1;
  }

  PdfTestCacheIo crashSnapshot;
  uint32_t publishedPage = 0;
  const bool stageReached =
      baselineReady && capturePrefixCrash(baseline, baselinePage, stage, &crashSnapshot, &publishedPage);
  report.check(std::string(id) + "_STAGE_REACHED", stageReached,
               "the harness captured the requested durability window without production hooks");
  if (!stageReached) {
    std::cout << "OBSERVED baseline_page=" << baselinePage << " published_page=0\n";
    return 1;
  }

  Observation recovered;
  bool resumed = false;
  uint32_t resumedPage = 0;
  PdfBuildResumePhase resumedPhase = PdfBuildResumePhase::None;
  if (stageReached) {
    PreparationHarness recovery;
    cloneStorage(crashSnapshot, &recovery.storage);
    PdfPreparation preparation;
    const PdfStatus begun = preparation.begin(recovery.config(kLongNoOutlinePath));
    if (begun) {
      PdfStepResult terminal = PdfStepResult::failure(PdfStatus::failure(PdfError::BudgetExhausted));
      for (uint32_t step = 0; step < 1000000U; ++step) {
        const PdfStepResult result = preparation.step();
        recovery.nowMs += 100U;
        if (!resumed && preparation.resumedFromCheckpoint()) {
          resumed = true;
          resumedPage = preparation.durableResumePage();
          resumedPhase = preparation.resumedPhase();
        }
        if (!result.yielded()) {
          terminal = result;
          break;
        }
      }
      recovered = collectObservation(preparation, recovery, true, terminal);
    } else {
      recovered.terminalStatus = begun;
    }
  }
  report.check(std::string(id) + "_RESUMED", resumed && resumedPhase == PdfBuildResumePhase::AfterPage,
               "restart selected the safe page-prefix checkpoint rather than rebuilding from page zero");
  report.check(std::string(id) + "_CURSOR",
               resumed && prefixResumeSelectionMatches(stage, baselinePage, publishedPage, resumedPage, resumedPhase),
               "the resumed cursor honors the last published PRCP and ignores unpublished tail state");
  report.check(std::string(id) + "_EXACT_ONCE", longDocumentExactlyOnce(recovered),
               "recovery produces one logical XHTML section with all source pages exactly once and in order");
  std::cout << "OBSERVED baseline_page=" << baselinePage << " published_page=" << publishedPage
            << " resumed=" << resumed << " resumed_page=" << resumedPage
            << " sections=" << recovered.metadata.sectionCount << " xhtml=" << recovered.xhtml.size() << '\n';
  return report.failures == 0 ? 0 : 1;
}

}  // namespace

int main(const int argc, char** argv) {
  if (argc != 2 || argv == nullptr || argv[1] == nullptr) {
    std::cerr << "usage: PdfSectionContractTest <mode>\n";
    return 2;
  }
  const std::string_view mode(argv[1]);
  if (mode == "--commit-control") return runCommitControl();
  if (mode == "--oracle-controls") return runOracleControls();
  if (mode == "--no-outline") return runNoOutlineContract();
  if (mode == "--outline-boundary") return runOutlineBoundaryContract();
  if (mode == "--numeric-rect-link") return runNumericRectLinkContract();
  if (mode == "--outside-rect-unlinked") return runOutsideRectContract();
  if (mode == "--non-outline-target") return runNonOutlineTargetContract();
  if (mode == "--rect-edge") return runRectEdgeContract();
  if (mode == "--rotated-crop") return runRotatedCropContract();
  if (mode == "--layout-contract") return runLayoutContract();
  if (mode == "--outline-capacity") return runOutlineCapacityContract();
  if (mode == "--prefix-crash-before-journal") {
    return runPrefixCrashContract(PrefixCrashStage::AfterPrefixSyncBeforeJournal, "PREFIX_BEFORE_JOURNAL");
  }
  if (mode == "--prefix-crash-after-journal") {
    return runPrefixCrashContract(PrefixCrashStage::AfterJournalBeforeCheckpoint, "PREFIX_AFTER_JOURNAL");
  }
  if (mode == "--prefix-crash-after-checkpoint") {
    return runPrefixCrashContract(PrefixCrashStage::AfterCheckpoint, "PREFIX_AFTER_CHECKPOINT");
  }
  std::cerr << "unknown mode: " << mode << '\n';
  return 2;
}
