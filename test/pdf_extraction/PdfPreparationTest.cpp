#include <gtest/gtest.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

#include "PdfCacheStore.h"
#include "PdfMetadataStore.h"
#include "PdfOutline.h"
#include "PdfPreparation.h"
#include "PdfTestCacheIo.h"
#include "PdfTestIo.h"

namespace {

struct PdfSetupIoSnapshot {
  uint32_t open = 0;
  uint32_t read = 0;
  uint32_t write = 0;
  uint32_t flush = 0;
  uint32_t sync = 0;
  uint32_t close = 0;
  uint32_t remove = 0;
  uint32_t rename = 0;
  uint32_t mkdir = 0;
  uint32_t list = 0;
  uint32_t capacity = 0;
  uint32_t metadata = 0;
  size_t events = 0;
  std::vector<std::string> handles;
};

constexpr uint32_t kPdfPreparationSliceOperationLimit = 32;

uint32_t pdfSetupIoOperationDelta(const PdfSetupIoSnapshot& before, const PdfSetupIoSnapshot& after) {
  return after.open - before.open + after.read - before.read + after.write - before.write +
         after.flush - before.flush + after.sync - before.sync + after.close - before.close +
         after.remove - before.remove + after.rename - before.rename + after.mkdir - before.mkdir +
         after.list - before.list + after.capacity - before.capacity + after.metadata - before.metadata;
}

bool pdfManifestVerifierSliceIsBounded(const PdfSetupIoSnapshot& before, const PdfSetupIoSnapshot& after,
                                       const std::string& verifierPath) {
  const auto hasOnlyVerifier = [&](const std::vector<std::string>& handles) {
    return handles.empty() || (handles.size() == 1U && handles.front() == verifierPath);
  };
  const uint32_t operations = pdfSetupIoOperationDelta(before, after);
  return operations != 0 && operations <= kPdfPreparationSliceOperationLimit && hasOnlyVerifier(before.handles) &&
         hasOnlyVerifier(after.handles);
}

bool pdfHasSingleTruncatingOpen(const std::vector<PdfTestOpenObservation>& observations,
                                const std::string& path) {
  size_t matching = 0;
  for (const PdfTestOpenObservation& observation : observations) {
    if (observation.path != path) {
      continue;
    }
    ++matching;
    if (observation.mode != PdfCacheOpenMode::WriteTruncate) {
      return false;
    }
  }
  return matching == 1U;
}

PdfSetupIoSnapshot pdfSetupIoSnapshot(const PdfTestCacheIo& storage) {
  return {
      storage.openCalls(),      storage.readCalls(),    storage.writeCalls(),    storage.flushCalls(),
      storage.syncCalls(),      storage.closeCalls(),   storage.removeCalls(),   storage.renameCalls(),
      storage.mkdirCalls(),     storage.listCalls(),    storage.capacityCalls(), storage.metadataCalls(),
      storage.events().size(),  storage.openHandlePaths(),
  };
}

std::string pdfSetupIoTraceLine(const PdfSetupIoSnapshot& before, const PdfSetupIoSnapshot& after,
                                const PdfTestCacheIo& storage, const uint8_t progress, const PdfStepResult result) {
  char counts[192]{};
  const char state = result.failed() ? 'F' : result.complete() ? 'C' : 'Y';
  std::snprintf(counts, sizeof(counts),
                "o%lu r%lu w%lu f%lu s%lu c%lu d%lu n%lu m%lu l%lu p%lu t%lu @%u %c",
                static_cast<unsigned long>(after.open - before.open),
                static_cast<unsigned long>(after.read - before.read),
                static_cast<unsigned long>(after.write - before.write),
                static_cast<unsigned long>(after.flush - before.flush),
                static_cast<unsigned long>(after.sync - before.sync),
                static_cast<unsigned long>(after.close - before.close),
                static_cast<unsigned long>(after.remove - before.remove),
                static_cast<unsigned long>(after.rename - before.rename),
                static_cast<unsigned long>(after.mkdir - before.mkdir),
                static_cast<unsigned long>(after.list - before.list),
                static_cast<unsigned long>(after.capacity - before.capacity),
                static_cast<unsigned long>(after.metadata - before.metadata), progress, state);
  std::string trace(counts);
  if (after.read != before.read || after.metadata != before.metadata) {
    for (const std::string& path : before.handles) {
      trace += "|active:";
      trace += path;
    }
  }
  for (size_t index = before.events; index < after.events; ++index) {
    trace += '|';
    trace += storage.events()[index];
  }
  return trace;
}

struct PdfSetupTraceResult {
  std::vector<std::string> lines;
  PdfStepResult result = PdfStepResult::paused();
  bool entered = false;
  bool left = false;
};

PdfSetupTraceResult runPdfSetupTrace(PdfPreparation& preparation, PdfTestCacheIo& storage, uint32_t* nowMs,
                                     const uint32_t maxSteps = 4096) {
  PdfSetupTraceResult trace;
  for (uint32_t step = 0; step < maxSteps && !trace.left; ++step) {
    const uint8_t progressBefore = preparation.progressPercent();
    const PdfSetupIoSnapshot before = pdfSetupIoSnapshot(storage);
    trace.result = preparation.step();
    const PdfSetupIoSnapshot after = pdfSetupIoSnapshot(storage);
    const uint8_t progressAfter = preparation.progressPercent();
    if (nowMs != nullptr) {
      ++*nowMs;
    }
    if (progressBefore == 14U) {
      trace.entered = true;
      trace.lines.push_back(pdfSetupIoTraceLine(before, after, storage, progressAfter, trace.result));
      trace.left = progressAfter != 14U || trace.result.failed();
    }
    if (trace.result.failed()) {
      break;
    }
  }
  return trace;
}

std::vector<uint8_t> loadClassicFixture() {
  const std::filesystem::path path =
      std::filesystem::path(__FILE__).parent_path().parent_path() / "pdf_reflow_core" / "fixtures" / "classic_text.pdf";
  std::ifstream input(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

std::vector<uint8_t> loadNavigationFixture() {
  const std::filesystem::path path = std::filesystem::path(__FILE__).parent_path().parent_path() / "pdf_reflow_core" /
                                     "fixtures" / "navigation_outline.pdf";
  std::ifstream input(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

std::vector<uint8_t> loadFixture(const char* name) {
  const std::filesystem::path path =
      std::filesystem::path(__FILE__).parent_path().parent_path() / "pdf_reflow_core" / "fixtures" / name;
  std::ifstream input(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

std::vector<uint8_t> makeTwoPageTextPdf(const bool unreadableFirst = false, const bool includeThird = false,
                                        const char* const secondPageContent = nullptr,
                                        const char* const baseFont = "/Helvetica") {
  const std::string firstStream =
      unreadableFirst ? "q Q" : "BT /F1 12 Tf 72 720 Td (First durable page) Tj ET";
  const std::string secondStream = secondPageContent != nullptr
                                       ? secondPageContent
                                       : "BT /F1 12 Tf 72 720 Td (Second resumed page) Tj ET";
  const std::string thirdStream = "BT /F1 12 Tf 72 720 Td (Third durable page) Tj ET";
  std::vector<std::string> objects = {
      "<< /Type /Catalog /Pages 2 0 R >>",
      includeThird ? "<< /Type /Pages /Count 3 /Kids [3 0 R 5 0 R 8 0 R] >>"
                   : "<< /Type /Pages /Count 2 /Kids [3 0 R 5 0 R] >>",
      "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] /Resources << /Font << /F1 7 0 R >> >> /Contents 4 0 R >>",
      "<< /Length " + std::to_string(firstStream.size()) + " >>\nstream\n" + firstStream + "\nendstream",
      "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] /Resources << /Font << /F1 7 0 R >> >> /Contents 6 0 R >>",
      "<< /Length " + std::to_string(secondStream.size()) + " >>\nstream\n" + secondStream + "\nendstream",
      std::string("<< /Type /Font /Subtype /Type1 /BaseFont ") + baseFont + " >>",
  };
  if (includeThird) {
    objects.push_back(
        "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] /Resources << /Font << /F1 7 0 R >> >> /Contents 9 0 R >>");
    objects.push_back("<< /Length " + std::to_string(thirdStream.size()) + " >>\nstream\n" + thirdStream +
                      "\nendstream");
  }
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

std::vector<uint8_t> makeOnePageTextPdf(const std::string& contentStream, const uint16_t propertyCount = 0,
                                        const char* const baseFont = "/Helvetica") {
  std::string resources = "<< /Font << /F1 5 0 R >>";
  if (propertyCount != 0) {
    resources += " /Properties <<";
    for (uint16_t index = 0; index < propertyCount; ++index) {
      resources += " /MC" + std::to_string(index) + " 5 0 R";
    }
    resources += " >>";
  }
  resources += " >>";
  const std::vector<std::string> objects = {
      "<< /Type /Catalog /Pages 2 0 R >>",
      "<< /Type /Pages /Count 1 /Kids [3 0 R] >>",
      "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] /Resources " + resources + " /Contents 4 0 R >>",
      "<< /Length " + std::to_string(contentStream.size()) + " >>\nstream\n" + contentStream + "\nendstream",
      std::string("<< /Type /Font /Subtype /Type1 /BaseFont ") + baseFont + " >>",
  };
  std::string pdf = "%PDF-1.4\n";
  std::vector<size_t> offsets;
  offsets.reserve(objects.size());
  for (size_t index = 0; index < objects.size(); ++index) {
    offsets.push_back(pdf.size());
    pdf += std::to_string(index + 1U) + " 0 obj\n" + objects[index] + "\nendobj\n";
  }
  const size_t xrefOffset = pdf.size();
  pdf += "xref\n0 6\n0000000000 65535 f \n";
  char entry[32]{};
  for (const size_t offset : offsets) {
    const int length = std::snprintf(entry, sizeof(entry), "%010zu 00000 n \n", offset);
    if (length <= 0 || static_cast<size_t>(length) >= sizeof(entry)) {
      return {};
    }
    pdf.append(entry, static_cast<size_t>(length));
  }
  pdf += "trailer\n<< /Size 6 /Root 1 0 R >>\nstartxref\n" + std::to_string(xrefOffset) + "\n%%EOF\n";
  return {pdf.begin(), pdf.end()};
}

std::vector<uint8_t> makeCollidedIncrementalXrefPdf() {
  const std::string content = "BT /F1 12 Tf 72 720 Td (Collision proof) Tj ET";
  const std::vector<uint8_t> base = makeOnePageTextPdf(content);
  std::string pdf(base.begin(), base.end());
  const size_t marker = pdf.rfind("startxref\n");
  if (marker == std::string::npos) {
    return {};
  }
  const size_t offsetStart = marker + std::strlen("startxref\n");
  const size_t offsetEnd = pdf.find('\n', offsetStart);
  if (offsetEnd == std::string::npos) {
    return {};
  }
  uint64_t previousXref = std::stoull(pdf.substr(offsetStart, offsetEnd - offsetStart));
  constexpr uint32_t filterSlots =
      (PdfLimits::PageRunBytes + PdfLimits::OperandOrderHistogramBytes) / sizeof(uint32_t);
  constexpr uint32_t firstCollisionObject = 100;
  for (uint8_t revision = 0; revision < 3U; ++revision) {
    const uint64_t newestXref = pdf.size();
    pdf += "xref\n";
    for (uint32_t index = 0; index < 9U; ++index) {
      pdf += std::to_string(firstCollisionObject + index * filterSlots) + " 1\n0000000000 65535 f \n";
    }
    const uint32_t size = firstCollisionObject + 8U * filterSlots + 1U;
    pdf += "trailer\n<< /Size " + std::to_string(size) + " /Root 1 0 R /Prev " +
           std::to_string(previousXref) + " >>\nstartxref\n" + std::to_string(newestXref) + "\n%%EOF\n";
    previousXref = newestXref;
  }
  return {pdf.begin(), pdf.end()};
}

std::vector<uint8_t> assembleClassicPdf(const std::vector<std::string>& objects) {
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
  pdf += "trailer\n<< /Size " + std::to_string(objects.size() + 1U) + " /Root 1 0 R >>\nstartxref\n" +
         std::to_string(xrefOffset) + "\n%%EOF\n";
  return {pdf.begin(), pdf.end()};
}

std::vector<uint8_t> makeManyContentStreamsPdf(const uint8_t streamCount) {
  if (streamCount == 0) {
    return {};
  }
  const uint32_t fontObject = 4U + streamCount;
  std::string contents = "[";
  for (uint8_t index = 0; index < streamCount; ++index) {
    contents += std::to_string(4U + index) + " 0 R ";
  }
  contents += "]";
  std::vector<std::string> objects = {
      "<< /Type /Catalog /Pages 2 0 R >>",
      "<< /Type /Pages /Count 1 /Kids [3 0 R] >>",
      "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] /Resources << /Font << /F1 " +
          std::to_string(fontObject) + " 0 R >> >> /Contents " + contents + " >>",
  };
  for (uint8_t index = 0; index < streamCount; ++index) {
    const std::string text = "Overflow" + (index < 10 ? std::string("0") : std::string()) +
                             std::to_string(index);
    const std::string stream = "BT /F1 12 Tf 72 " + std::to_string(720 - index * 18) + " Td (" + text + ") Tj ET";
    objects.push_back("<< /Length " + std::to_string(stream.size()) + " >>\nstream\n" + stream +
                      "\nendstream");
  }
  objects.push_back("<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>");
  return assembleClassicPdf(objects);
}

std::vector<uint8_t> makeNamedDestinationTreeOutlinePdf() {
  const std::string firstContent = "BT /F1 12 Tf 72 720 Td (First named page) Tj ET";
  const std::string secondContent = "BT /F1 12 Tf 72 720 Td (Second named page) Tj ET";
  return assembleClassicPdf({
      "<< /Type /Catalog /Pages 2 0 R /Outlines 8 0 R /Names 16 0 R >>",
      "<< /Type /Pages /Count 2 /Kids [3 0 R 5 0 R] >>",
      "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] /Resources << /Font << /F1 7 0 R >> >> "
      "/Contents 4 0 R >>",
      "<< /Length " + std::to_string(firstContent.size()) + ">>\nstream\n" + firstContent + "\nendstream",
      "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] /Resources << /Font << /F1 7 0 R >> >> "
      "/Contents 6 0 R >>",
      "<< /Length " + std::to_string(secondContent.size()) + ">>\nstream\n" + secondContent + "\nendstream",
      "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>",
      "<< /Type /Outlines /Count 2 /First 9 0 R /Last 10 0 R >>",
      "<< /Title (First named chapter) /Dest (A) /Parent 8 0 R /Next 10 0 R >>",
      "<< /Title (Second named chapter) /Dest (Z) /Parent 8 0 R /Prev 9 0 R >>",
      "<< /Kids [12 0 R 13 0 R] >>",
      "<< /Limits [(A) (M)] /Names [(A) 14 0 R (M) 14 0 R] >>",
      "<< /Limits [(N) (Z)] /Names [(N) 15 0 R (Z) 15 0 R] >>",
      "<< /D [3 0 R /Fit] >>",
      "<< /D [5 0 R /Fit] >>",
      "<< /Dests 11 0 R >>",
  });
}

std::vector<uint8_t> makeIndirectSeparationImagePdf() {
  const std::string content =
      "q 10 0 0 10 72 700 cm /Im1 Do Q BT /F1 12 Tf 72 650 Td (Readable page text) Tj ET";
  return assembleClassicPdf({
      "<< /Type /Catalog /Pages 2 0 R >>",
      "<< /Type /Pages /Count 1 /Kids [3 0 R] >>",
      "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] /Resources << /Font << /F1 5 0 R >> "
      "/XObject << /Im1 6 0 R >> >> /Contents 4 0 R >>",
      "<< /Length " + std::to_string(content.size()) + " >>\nstream\n" + content + "\nendstream",
      "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>",
      "<< /Type /XObject /Subtype /Image /Width 1 /Height 1 /ColorSpace 7 0 R /BitsPerComponent 8 "
      "/Length 1 >>\nstream\nx\nendstream",
      "[/Separation /Spot /DeviceCMYK 8 0 R]",
      "<< /FunctionType 2 /Domain [0 1] /C0 [0 0 0 0] /C1 [0 0 0 1] /N 1 >>",
  });
}

void writeLittleEndian32(std::vector<uint8_t>* const bytes, const size_t offset, const uint32_t value) {
  ASSERT_NE(bytes, nullptr);
  ASSERT_LE(offset + sizeof(value), bytes->size());
  for (uint8_t index = 0; index < sizeof(value); ++index) {
    (*bytes)[offset + index] = static_cast<uint8_t>(value >> (index * 8U));
  }
}

uint16_t readLittleEndian16(const std::vector<uint8_t>& bytes, const size_t offset) {
  EXPECT_LE(offset + sizeof(uint16_t), bytes.size());
  return static_cast<uint16_t>(bytes[offset]) | static_cast<uint16_t>(bytes[offset + 1U] << 8U);
}

uint32_t readLittleEndian32(const std::vector<uint8_t>& bytes, const size_t offset) {
  EXPECT_LE(offset + sizeof(uint32_t), bytes.size());
  uint32_t value = 0;
  for (uint8_t index = 0; index < sizeof(value); ++index) {
    value |= static_cast<uint32_t>(bytes[offset + index]) << (index * 8U);
  }
  return value;
}

void writeLittleEndian16(std::vector<uint8_t>* const bytes, const size_t offset, const uint16_t value) {
  ASSERT_NE(bytes, nullptr);
  ASSERT_LE(offset + sizeof(value), bytes->size());
  for (uint8_t index = 0; index < sizeof(value); ++index) {
    (*bytes)[offset + index] = static_cast<uint8_t>(value >> (index * 8U));
  }
}

void writeLittleEndian64(std::vector<uint8_t>* const bytes, const size_t offset, const uint64_t value) {
  ASSERT_NE(bytes, nullptr);
  ASSERT_LE(offset + sizeof(value), bytes->size());
  for (uint8_t index = 0; index < sizeof(value); ++index) {
    (*bytes)[offset + index] = static_cast<uint8_t>(value >> (index * 8U));
  }
}

void resealTrailingCrc(std::vector<uint8_t>* const bytes) {
  ASSERT_NE(bytes, nullptr);
  ASSERT_GE(bytes->size(), sizeof(uint32_t));
  writeLittleEndian32(bytes, bytes->size() - sizeof(uint32_t),
                      pdfCacheCrc32(bytes->data(), bytes->size() - sizeof(uint32_t)));
}

void setDiscoveryPageRotationAndReseal(std::vector<uint8_t>* const bytes, const uint16_t rotation) {
  constexpr size_t discoveryXrefRecordBytes = 24;
  constexpr size_t discoveryPageRecordBytes = 256;
  constexpr size_t discoveryTrailerBytes = 72;
  constexpr size_t discoveryPageRotationOffset = 234;
  constexpr size_t discoveryPageCrcOffset = 252;
  ASSERT_NE(bytes, nullptr);
  ASSERT_GE(bytes->size(), 8U);
  ASSERT_EQ(std::memcmp(bytes->data(), "PDRH", 4), 0);
  const size_t discoveryHeaderBytes = readLittleEndian16(*bytes, 6);
  ASSERT_GE(bytes->size(), discoveryHeaderBytes + discoveryPageRecordBytes + discoveryTrailerBytes);
  const uint32_t xrefCount = readLittleEndian32(*bytes, 52);
  const uint16_t pageCount = readLittleEndian16(*bytes, 56);
  ASSERT_GT(xrefCount, 0U);
  ASSERT_LE(xrefCount, 256U);
  ASSERT_GT(pageCount, 0U);
  ASSERT_LE(pageCount, 16U);
  const size_t bodyOffset = discoveryHeaderBytes;
  const size_t firstPageOffset = bodyOffset + static_cast<size_t>(xrefCount) * discoveryXrefRecordBytes;
  const size_t bodyBytes = static_cast<size_t>(xrefCount) * discoveryXrefRecordBytes +
                           static_cast<size_t>(pageCount) * discoveryPageRecordBytes;
  const size_t trailerOffset = bodyOffset + bodyBytes;
  ASSERT_LE(trailerOffset + discoveryTrailerBytes, bytes->size());
  ASSERT_EQ(std::memcmp(bytes->data() + firstPageOffset, "PDRP", 4), 0);
  ASSERT_EQ(std::memcmp(bytes->data() + trailerOffset, "PDRT", 4), 0);

  writeLittleEndian16(bytes, firstPageOffset + discoveryPageRotationOffset, rotation);
  writeLittleEndian32(bytes, firstPageOffset + discoveryPageCrcOffset,
                      pdfCacheCrc32(bytes->data() + firstPageOffset, discoveryPageCrcOffset));
  writeLittleEndian32(bytes, trailerOffset + 28, pdfCacheCrc32(bytes->data() + bodyOffset, bodyBytes));
  writeLittleEndian64(bytes, trailerOffset + 32, pdfCacheFnv64(bytes->data() + bodyOffset, bodyBytes));
  writeLittleEndian32(bytes, trailerOffset + discoveryTrailerBytes - sizeof(uint32_t),
                      pdfCacheCrc32(bytes->data() + trailerOffset, discoveryTrailerBytes - sizeof(uint32_t)));
}

struct PreparationHarness {
  PdfTestCacheIo storage;
  PdfResourceSnapshot resources{128U * 1024U, 96U * 1024U, 8U * 1024U};
  uint32_t nowMs = 0;
  size_t resourceEvents = 0;
  bool advanceOnClockRead = false;

  static uint32_t now(void* context) {
    auto& harness = *static_cast<PreparationHarness*>(context);
    return harness.advanceOnClockRead ? harness.nowMs++ : harness.nowMs;
  }

  static PdfResourceSnapshot measure(void* context) { return static_cast<PreparationHarness*>(context)->resources; }

  static void resourceEvent(void* context, const PdfResourceEvent&) {
    ++static_cast<PreparationHarness*>(context)->resourceEvents;
  }

  PdfPreparationConfig config(const char* sourcePath = "/books/minimal.pdf") {
    return {
        storage.io(), sourcePath, "/.crosspoint", this, now, {this, measure, resourceEvent}, storage.renameCallback(),
        800,          480,
    };
  }

  void addFixture() { storage.addFile("/books/minimal.pdf", loadClassicFixture(), 1234, true); }
};

PdfStepResult runToTerminal(PdfPreparation& preparation, PreparationHarness& harness, uint32_t maxSteps = 20000) {
  for (uint32_t step = 0; step < maxSteps; ++step) {
    const uint64_t readBefore = harness.storage.bytesReadTotal();
    const uint64_t writtenBefore = harness.storage.bytesWrittenTotal();
    const PdfStepResult result = preparation.step();
    EXPECT_LE(harness.storage.bytesReadTotal() - readBefore, PdfLimits::InterpreterSourceBufferBytes);
    EXPECT_LE(harness.storage.bytesWrittenTotal() - writtenBefore, PdfLimits::InterpreterSourceBufferBytes);
    ++harness.nowMs;
    if (!result.yielded()) {
      return result;
    }
  }
  return PdfStepResult::failure(PdfStatus::failure(PdfError::BudgetExhausted));
}

PdfStepResult cancelToTerminal(PdfPreparation& preparation, PreparationHarness& harness) {
  preparation.requestCancel();
  return runToTerminal(preparation, harness, 256);
}

struct OneSectionCollector {
  PdfMetadataSection section{};
  uint16_t count = 0;

  static PdfStatus accept(void* context, const uint16_t index, const PdfMetadataSection& record) {
    if (context == nullptr || index != 0) {
      return PdfStatus::failure(PdfError::InvalidArgument);
    }
    auto& self = *static_cast<OneSectionCollector*>(context);
    self.section = record;
    ++self.count;
    return PdfStatus::success();
  }
};

struct OneOutlineCollector {
  PdfOutlineEntry entry{};
  uint16_t count = 0;

  static PdfStatus accept(void* context, const uint16_t index, const PdfOutlineEntry& record) {
    if (context == nullptr || index != 0) {
      return PdfStatus::failure(PdfError::InvalidArgument);
    }
    auto& self = *static_cast<OneOutlineCollector*>(context);
    self.entry = record;
    ++self.count;
    return PdfStatus::success();
  }
};

TEST(PdfPreparation, ConvertsMinimalFixtureIntoCommittedDeviceStyleXhtml) {
  PreparationHarness harness;
  harness.addFixture();

  PdfPreparation preparation;
  ASSERT_TRUE(preparation.begin(harness.config()).ok());
  const PdfStepResult result = runToTerminal(preparation, harness);

  ASSERT_TRUE(result.complete()) << static_cast<int>(result.status.error);
  EXPECT_EQ(preparation.progressPercent(), 100U);
  EXPECT_EQ(preparation.totalWords(), 2U);
  EXPECT_EQ(preparation.resourcePeakBytes(), 63488U);
  EXPECT_EQ(preparation.resourceCurrentBytes(), 0U);
  EXPECT_EQ(harness.storage.openHandleCount(), 0U);
  EXPECT_LE(harness.storage.maximumReadRequest(), 4096U);
  EXPECT_LE(harness.storage.maximumWriteRequest(), 4096U);
  EXPECT_TRUE(harness.storage.isDirectory("/.crosspoint"));

  const std::string sectionPath = std::string(preparation.cacheRoot()) + "/gen_" +
                                  std::to_string(preparation.generation()) + "/sections/000000.xhtml";
  ASSERT_TRUE(harness.storage.exists(sectionPath));
  const auto& sectionBytes = harness.storage.bytes(sectionPath);
  const std::string section(sectionBytes.begin(), sectionBytes.end());
  EXPECT_NE(section.find(">Hello PDF</p>"), std::string::npos);
  EXPECT_EQ(section.find("font-size"), std::string::npos);
  EXPECT_EQ(section.find("font-family"), std::string::npos);
  EXPECT_EQ(section.find("position:"), std::string::npos);

  const std::string generationRoot =
      std::string(preparation.cacheRoot()) + "/gen_" + std::to_string(preparation.generation());
  const std::string metadataPath = generationRoot + "/metadata.bin";
  const std::string outlinePath = generationRoot + "/outline.bin";
  ASSERT_TRUE(harness.storage.exists(metadataPath));
  ASSERT_TRUE(harness.storage.exists(outlinePath));

  PdfTestByteSource metadataSource(harness.storage.bytes(metadataPath));
  PdfMetadata metadata{};
  OneSectionCollector sectionCollector;
  ASSERT_TRUE(
      pdfDecodeMetadata(metadataSource.source(), &metadata, {&sectionCollector, OneSectionCollector::accept}).ok());
  EXPECT_STREQ(metadata.title, "minimal");
  EXPECT_EQ(metadata.totalWords, 2U);
  EXPECT_EQ(metadata.sectionCount, 1U);
  EXPECT_EQ(metadata.outlineCount, 1U);
  EXPECT_EQ(sectionCollector.count, 1U);
  EXPECT_EQ(sectionCollector.section.wordCount, 2U);

  PdfTestByteSource outlineSource(harness.storage.bytes(outlinePath));
  PdfOutlineHeader outlineHeader{};
  OneOutlineCollector outlineCollector;
  ASSERT_TRUE(
      pdfDecodeOutline(outlineSource.source(), &outlineHeader, {&outlineCollector, OneOutlineCollector::accept}).ok());
  EXPECT_EQ(outlineHeader.entryCount, 1U);
  EXPECT_STREQ(outlineCollector.entry.title, "minimal");
  EXPECT_STREQ(outlineCollector.entry.anchor, "b00000000");

  PdfCacheStore cache;
  ASSERT_TRUE(cache.initialize(harness.storage.io(), preparation.cacheRoot()).ok());
  PdfCacheManifestSelection selection;
  ASSERT_TRUE(cache.loadManifestSlots(preparation.sourceIdentity(), &selection).ok());
  ASSERT_TRUE(selection.selected);
  EXPECT_TRUE(selection.manifest.completed);
  EXPECT_EQ(selection.manifest.totalWords, 2U);
  EXPECT_EQ(selection.manifest.requiredFileCount, 5U);
  EXPECT_TRUE(harness.storage.exists(generationRoot + "/cover.bmp"));
  EXPECT_TRUE(harness.storage.exists(generationRoot + "/thumb.bmp"));
}

TEST(PdfPreparation, CompletesTypographyCoverRowsAcrossTimeSliceYields) {
  PreparationHarness harness;
  harness.addFixture();
  harness.advanceOnClockRead = true;

  PdfPreparation preparation;
  ASSERT_TRUE(preparation.begin(harness.config()).ok());
  const PdfStepResult result = runToTerminal(preparation, harness, 100000);

  ASSERT_TRUE(result.complete()) << static_cast<int>(result.status.error) << '@' << result.status.offset;
  const std::string generationRoot =
      std::string(preparation.cacheRoot()) + "/gen_" + std::to_string(preparation.generation());
  ASSERT_TRUE(harness.storage.exists(generationRoot + "/cover.bmp"));
  ASSERT_TRUE(harness.storage.exists(generationRoot + "/thumb.bmp"));
  EXPECT_EQ(harness.storage.bytes(generationRoot + "/cover.bmp").size(), 62U + 32U * 400U);
  EXPECT_EQ(harness.storage.bytes(generationRoot + "/thumb.bmp").size(), 62U + 12U * 160U);
}

TEST(PdfPreparation, CompactsCollidedIncrementalXrefBeforeFollowingPrev) {
  constexpr char sourcePath[] = "/books/collided-incremental-xref.pdf";
  PreparationHarness harness;
  harness.storage.setMaximumReadHandles(1);
  const std::vector<uint8_t> fixture = makeCollidedIncrementalXrefPdf();
  ASSERT_FALSE(fixture.empty());
  harness.storage.addFile(sourcePath, fixture, 1234, true);

  PdfPreparation preparation;
  ASSERT_TRUE(preparation.begin(harness.config(sourcePath)).ok());
  const PdfStepResult result = runToTerminal(preparation, harness);

  ASSERT_TRUE(result.complete()) << static_cast<int>(result.status.error) << '@' << result.status.offset
                                 << " phase=" << static_cast<int>(preparation.phase());
  EXPECT_EQ(preparation.totalWords(), 2U);
  EXPECT_GT(preparation.workCounters().xrefSpoolRecordsRead, 0U);
  EXPECT_GT(preparation.workCounters().xrefSpoolRecordsWritten, 0U);
  EXPECT_EQ(harness.storage.openHandleCount(), 0U);
}

TEST(PdfPreparation, ReflowsMoreThanSixteenPageContentStreamsInSourceOrder) {
  constexpr char sourcePath[] = "/books/content-overflow.pdf";
  PreparationHarness harness;
  harness.storage.setMaximumReadHandles(1);
  const std::vector<uint8_t> fixture = makeManyContentStreamsPdf(20);
  ASSERT_FALSE(fixture.empty());
  harness.storage.addFile(sourcePath, fixture, 1234, true);

  PdfPreparation preparation;
  ASSERT_TRUE(preparation.begin(harness.config(sourcePath)).ok());
  const PdfStepResult result = runToTerminal(preparation, harness);

  ASSERT_TRUE(result.complete()) << static_cast<int>(result.status.error) << "@" << result.status.offset
                                 << " phase=" << static_cast<int>(preparation.phase());
  EXPECT_EQ(preparation.totalWords(), 20U);
  const std::string sectionPath = std::string(preparation.cacheRoot()) + "/gen_" +
                                  std::to_string(preparation.generation()) + "/sections/000000.xhtml";
  ASSERT_TRUE(harness.storage.exists(sectionPath));
  const auto& sectionBytes = harness.storage.bytes(sectionPath);
  const std::string section(sectionBytes.begin(), sectionBytes.end());
  const size_t first = section.find("Overflow00");
  const size_t last = section.find("Overflow19");
  ASSERT_NE(first, std::string::npos);
  ASSERT_NE(last, std::string::npos);
  EXPECT_LT(first, last);
  EXPECT_EQ(harness.storage.openHandleCount(), 0U);
}

TEST(PdfPreparation, RestoresContentOverflowRecordsFromDiscoveryCheckpoint) {
  constexpr char sourcePath[] = "/books/content-overflow-resume.pdf";
  PreparationHarness harness;
  harness.storage.setMaximumReadHandles(1);
  const std::vector<uint8_t> fixture = makeManyContentStreamsPdf(20);
  ASSERT_FALSE(fixture.empty());
  harness.storage.addFile(sourcePath, fixture, 1234, true);

  PdfPreparation interrupted;
  ASSERT_TRUE(interrupted.begin(harness.config(sourcePath)).ok());
  for (uint32_t slice = 0;
       slice < 20000 && interrupted.durableResumePhase() != PdfBuildResumePhase::AfterDiscovery;
       ++slice) {
    const PdfStepResult step = interrupted.step();
    ++harness.nowMs;
    ASSERT_TRUE(step.yielded()) << static_cast<int>(step.status.error) << "@" << step.status.offset;
  }
  ASSERT_EQ(interrupted.durableResumePhase(), PdfBuildResumePhase::AfterDiscovery);
  const uint32_t generation = interrupted.generation();
  const PdfStepResult cancelled = cancelToTerminal(interrupted, harness);
  ASSERT_TRUE(cancelled.failed());
  ASSERT_EQ(cancelled.status.error, PdfError::Cancelled);
  ASSERT_EQ(harness.storage.openHandleCount(), 0U);

  PdfPreparation resumed;
  ASSERT_TRUE(resumed.begin(harness.config(sourcePath)).ok());
  const PdfStepResult result = runToTerminal(resumed, harness);

  ASSERT_TRUE(result.complete()) << static_cast<int>(result.status.error) << "@" << result.status.offset
                                 << " phase=" << static_cast<int>(resumed.phase());
  EXPECT_TRUE(resumed.resumedFromCheckpoint());
  EXPECT_EQ(resumed.resumedPhase(), PdfBuildResumePhase::AfterDiscovery);
  EXPECT_EQ(resumed.generation(), generation);
  EXPECT_EQ(resumed.totalWords(), 20U);
  const std::string sectionPath = std::string(resumed.cacheRoot()) + "/gen_" +
                                  std::to_string(resumed.generation()) + "/sections/000000.xhtml";
  ASSERT_TRUE(harness.storage.exists(sectionPath));
  const auto& sectionBytes = harness.storage.bytes(sectionPath);
  const std::string section(sectionBytes.begin(), sectionBytes.end());
  EXPECT_NE(section.find("Overflow00"), std::string::npos);
  EXPECT_NE(section.find("Overflow19"), std::string::npos);
  EXPECT_EQ(harness.storage.openHandleCount(), 0U);
}

TEST(PdfPreparation, OmitsIndirectSeparationImageWithoutDroppingPageText) {
  constexpr char sourcePath[] = "/books/indirect-separation.pdf";
  PreparationHarness harness;
  harness.storage.setMaximumReadHandles(1);
  const std::vector<uint8_t> fixture = makeIndirectSeparationImagePdf();
  ASSERT_FALSE(fixture.empty());
  harness.storage.addFile(sourcePath, fixture, 1234, true);

  PdfPreparation preparation;
  ASSERT_TRUE(preparation.begin(harness.config(sourcePath)).ok());
  const PdfStepResult result = runToTerminal(preparation, harness);

  ASSERT_TRUE(result.complete()) << static_cast<int>(result.status.error) << "@" << result.status.offset
                                 << " phase=" << static_cast<int>(preparation.phase());
  EXPECT_EQ(preparation.totalWords(), 3U);
  const std::string sectionPath = std::string(preparation.cacheRoot()) + "/gen_" +
                                  std::to_string(preparation.generation()) + "/sections/000000.xhtml";
  ASSERT_TRUE(harness.storage.exists(sectionPath));
  const auto& sectionBytes = harness.storage.bytes(sectionPath);
  const std::string section(sectionBytes.begin(), sectionBytes.end());
  EXPECT_NE(section.find("Readable page text"), std::string::npos);
  EXPECT_EQ(harness.storage.openHandleCount(), 0U);
}

TEST(PdfPreparation, OmitsInlineRasterWithoutDroppingPageText) {
  constexpr char sourcePath[] = "/books/inline-raster.pdf";
  std::string content = "q 1 0 0 1 0 0 cm BI /W 1 /H 1 /CS /G /BPC 8 ID ";
  content.push_back(static_cast<char>(0x80));
  content += " EI Q BT /F1 12 Tf 72 720 Td (Inline image survives) Tj ET";
  PreparationHarness harness;
  harness.storage.setMaximumReadHandles(1);
  harness.storage.addFile(sourcePath, makeOnePageTextPdf(content), 1234, true);

  PdfPreparation preparation;
  ASSERT_TRUE(preparation.begin(harness.config(sourcePath)).ok());
  const PdfStepResult result = runToTerminal(preparation, harness);

  ASSERT_TRUE(result.complete()) << static_cast<int>(result.status.error) << "@" << result.status.offset
                                 << " phase=" << static_cast<int>(preparation.phase());
  EXPECT_EQ(preparation.totalWords(), 3U);
  const std::string sectionPath = std::string(preparation.cacheRoot()) + "/gen_" +
                                  std::to_string(preparation.generation()) + "/sections/000000.xhtml";
  ASSERT_TRUE(harness.storage.exists(sectionPath));
  const auto& sectionBytes = harness.storage.bytes(sectionPath);
  const std::string section(sectionBytes.begin(), sectionBytes.end());
  EXPECT_NE(section.find("Inline image survives"), std::string::npos);
  EXPECT_EQ(section.find("<img"), std::string::npos);
  EXPECT_EQ(harness.storage.openHandleCount(), 0U);
}

TEST(PdfPreparation, PreparesSonySizedInlineResourceDictionaryInsideTheExistingFixedArena) {
  constexpr char sourcePath[] = "/books/large-inline-resources.pdf";
  const std::string content = "BT /F1 12 Tf 72 720 Td (Large resources) Tj ET";
  PreparationHarness harness;
  harness.storage.addFile(sourcePath, makeOnePageTextPdf(content, 358), 1234, true);

  PdfPreparation preparation;
  ASSERT_TRUE(preparation.begin(harness.config(sourcePath)).ok());
  const PdfStepResult result = runToTerminal(preparation, harness);

  ASSERT_TRUE(result.complete()) << static_cast<unsigned>(result.status.error) << '@' << result.status.offset;
  EXPECT_EQ(preparation.workCounters().pagesWalked, 1U);
  EXPECT_EQ(preparation.totalWords(), 2U);
  EXPECT_EQ(preparation.resourcePeakBytes(), 63488U);
}

TEST(PdfPreparation, ManifestVerifierReopenStaysBoundedUnderSingleReaderContract) {
  PreparationHarness harness;
  harness.addFixture();
  harness.storage.setMaximumReadHandles(1);

  PdfPreparation preparation;
  ASSERT_TRUE(preparation.begin(harness.config()).ok());
  std::string verifierPath;
  bool initialVerifierOpenObserved = false;
  bool verifierReopenObserved = false;
  bool oracleControlsExercised = false;
  PdfStepResult result = PdfStepResult::paused();
  for (uint32_t step = 0; step < 20000 && result.yielded(); ++step) {
    const PdfSetupIoSnapshot before = pdfSetupIoSnapshot(harness.storage);
    const size_t opensBefore = harness.storage.openObservations().size();
    result = preparation.step();
    const PdfSetupIoSnapshot after = pdfSetupIoSnapshot(harness.storage);
    ++harness.nowMs;

    const auto& opens = harness.storage.openObservations();
    for (size_t index = opensBefore; index < opens.size(); ++index) {
      const PdfTestOpenObservation& observation = opens[index];
      const bool manifestVerifier = observation.mode == PdfCacheOpenMode::Read &&
                                    (observation.path.ends_with("/manifest.a") ||
                                     observation.path.ends_with("/manifest.b")) &&
                                    harness.storage.exists(observation.path);
      if (!manifestVerifier) {
        continue;
      }
      if (!initialVerifierOpenObserved) {
        initialVerifierOpenObserved = true;
        verifierPath = observation.path;
        EXPECT_TRUE(before.handles.empty());
      } else {
        verifierReopenObserved = true;
        EXPECT_EQ(observation.path, verifierPath);
        ASSERT_EQ(before.handles.size(), 1U);
        EXPECT_EQ(before.handles.front(), verifierPath);
      }
      EXPECT_TRUE(pdfManifestVerifierSliceIsBounded(before, after, verifierPath));

      if (!oracleControlsExercised) {
        PdfSetupIoSnapshot extraReader = after;
        extraReader.handles.push_back("/books/unexpected-second-reader.pdf");
        EXPECT_FALSE(pdfManifestVerifierSliceIsBounded(before, extraReader, verifierPath));

        PdfSetupIoSnapshot overBudget = after;
        overBudget.open += kPdfPreparationSliceOperationLimit + 1U;
        EXPECT_FALSE(pdfManifestVerifierSliceIsBounded(before, overBudget, verifierPath));
        oracleControlsExercised = true;
      }
    }
  }

  ASSERT_TRUE(result.complete()) << static_cast<int>(result.status.error) << "@" << result.status.offset;
  EXPECT_TRUE(initialVerifierOpenObserved);
  EXPECT_TRUE(verifierReopenObserved);
  EXPECT_TRUE(oracleControlsExercised);
}

TEST(PdfPreparation, FreshCacheSetupIoTraceRemainsStableAcrossRefactoring) {
  PreparationHarness harness;
  harness.addFixture();

  PdfPreparation preparation;
  ASSERT_TRUE(preparation.begin(harness.config()).ok());
  const PdfSetupTraceResult setupTrace = runPdfSetupTrace(preparation, harness.storage, &harness.nowMs, 256);
  ASSERT_FALSE(setupTrace.result.failed());
  ASSERT_TRUE(setupTrace.entered);
  ASSERT_TRUE(setupTrace.left);
  const std::vector<std::string> expected = {
      "o0 r0 w0 f0 s0 c0 d0 n0 m0 l0 p0 t0 @14 Y",
      "o0 r0 w0 f0 s0 c1 d0 n0 m0 l0 p0 t0 @14 Y|close:/books/minimal.pdf",
      "o0 r0 w0 f0 s0 c0 d0 n0 m1 l0 p0 t0 @14 Y",
      "o0 r0 w0 f0 s0 c0 d0 n0 m0 l0 p0 t0 @14 Y",
      "o1 r0 w0 f0 s0 c0 d0 n0 m0 l0 p0 t0 @14 Y|open:/.crosspoint/pdf_13995035557984974960/build.a",
      "o1 r0 w0 f0 s0 c0 d0 n0 m0 l0 p0 t0 @14 Y|open:/.crosspoint/pdf_13995035557984974960/build.b",
      "o1 r0 w0 f0 s0 c0 d0 n0 m0 l0 p0 t0 @14 Y|open:/.crosspoint/pdf_13995035557984974960/manifest.a",
      "o1 r0 w0 f0 s0 c0 d0 n0 m0 l0 p0 t0 @14 Y|open:/.crosspoint/pdf_13995035557984974960/manifest.b",
      "o0 r0 w0 f0 s0 c0 d0 n0 m0 l0 p0 t0 @14 Y",
      "o0 r0 w0 f0 s0 c0 d0 n0 m0 l1 p0 t0 @14 Y",
      "o0 r0 w0 f0 s0 c0 d0 n0 m0 l0 p0 t0 @14 Y",
      "o0 r0 w0 f0 s0 c0 d0 n0 m1 l0 p0 t0 @14 Y",
      "o0 r0 w0 f0 s0 c0 d0 n0 m1 l0 p0 t0 @14 Y",
      "o0 r0 w0 f0 s0 c0 d0 n0 m1 l0 p0 t0 @14 Y",
      "o0 r0 w0 f0 s0 c0 d0 n0 m0 l0 p1 t0 @14 Y",
      "o0 r0 w0 f0 s0 c0 d0 n0 m0 l0 p0 t0 @14 Y",
      "o0 r0 w0 f0 s0 c0 d0 n0 m1 l0 p0 t0 @14 Y",
      "o0 r0 w0 f0 s0 c0 d0 n0 m0 l0 p0 t0 @14 Y",
      "o2 r0 w0 f0 s0 c0 d0 n0 m0 l0 p0 t1 @20 Y|open:/books/minimal.pdf|open:/.crosspoint/"
      "pdf_13995035557984974960/gen_864366812/build.xref.a",
  };
  EXPECT_EQ(setupTrace.lines, expected);

  const std::string xrefPath = std::string(preparation.cacheRoot()) + "/gen_" +
                               std::to_string(preparation.generation()) + "/build.xref.a";
  EXPECT_TRUE(pdfHasSingleTruncatingOpen(harness.storage.openObservations(), xrefPath));

  std::vector<PdfTestOpenObservation> wrongMode = harness.storage.openObservations();
  auto xrefOpen = std::find_if(wrongMode.begin(), wrongMode.end(), [&](const PdfTestOpenObservation& observation) {
    return observation.path == xrefPath;
  });
  ASSERT_NE(xrefOpen, wrongMode.end());
  xrefOpen->mode = PdfCacheOpenMode::ReadWrite;
  EXPECT_FALSE(pdfHasSingleTruncatingOpen(wrongMode, xrefPath));

  std::vector<PdfTestOpenObservation> extraReader = harness.storage.openObservations();
  extraReader.push_back({xrefPath, PdfCacheOpenMode::Read});
  EXPECT_FALSE(pdfHasSingleTruncatingOpen(extraReader, xrefPath));
}

TEST(PdfPreparation, SourceFontSizeDoesNotChangePreparedSemanticXhtml) {
  constexpr char sourcePath[] = "/books/font-size.pdf";
  std::vector<uint8_t> preparedSections[2];
  const char* const fixtures[] = {"font_size_6.pdf", "font_size_72.pdf"};
  for (uint8_t index = 0; index < 2; ++index) {
    PreparationHarness harness;
    harness.storage.addFile(sourcePath, loadFixture(fixtures[index]), 1234, true);
    PdfPreparation preparation;
    ASSERT_TRUE(preparation.begin(harness.config(sourcePath)).ok());
    const PdfStepResult result = runToTerminal(preparation, harness);
    ASSERT_TRUE(result.complete()) << fixtures[index] << " error=" << static_cast<int>(result.status.error);
    const std::string sectionPath = std::string(preparation.cacheRoot()) + "/gen_" +
                                    std::to_string(preparation.generation()) + "/sections/000000.xhtml";
    ASSERT_TRUE(harness.storage.exists(sectionPath));
    preparedSections[index] = harness.storage.bytes(sectionPath);
  }

  EXPECT_EQ(preparedSections[0], preparedSections[1]);
  const std::string xhtml(preparedSections[0].begin(), preparedSections[0].end());
  EXPECT_NE(xhtml.find("<p id=\"b00000000\">Typography uses device defaults.</p>"), std::string::npos);
  EXPECT_EQ(xhtml.find("<h1"), std::string::npos);
}

TEST(PdfPreparation, KeepsHierarchicalNumberedHeadingsSeparateAtBodySize) {
  constexpr char sourcePath[] = "/books/numbered-headings.pdf";
  const std::string content =
      "BT "
      "/F1 14 Tf 1 0 0 1 72 720 Tm (2. Materials and methods) Tj "
      "/F1 10 Tf 1 0 0 1 72 694 Tm (2.1.) Tj 1 0 0 1 98 694 Tm (Experimental materials) Tj "
      "/F1 12 Tf 1 0 0 1 72 660 Tm (Ordinary body paragraph establishes normal text size.) Tj "
      "1 0 0 1 72 644 Tm (More ordinary body text follows on another line.) Tj "
      "1 0 0 1 72 628 Tm (A third ordinary body line completes the paragraph.) Tj "
      "/F1 10 Tf 1 0 0 1 72 590 Tm (2.2.) Tj 1 0 0 1 98 590 Tm (H-PDINH preparation) Tj "
      "/F1 12 Tf 1 0 0 1 72 560 Tm (The next paragraph begins here.) Tj ET";
  PreparationHarness harness;
  harness.storage.addFile(sourcePath, makeOnePageTextPdf(content), 1234, true);

  PdfPreparation preparation;
  ASSERT_TRUE(preparation.begin(harness.config(sourcePath)).ok());
  const PdfStepResult result = runToTerminal(preparation, harness);
  ASSERT_TRUE(result.complete()) << static_cast<int>(result.status.error) << "@" << result.status.offset;

  const std::string sectionPath = std::string(preparation.cacheRoot()) + "/gen_" +
                                  std::to_string(preparation.generation()) + "/sections/000000.xhtml";
  ASSERT_TRUE(harness.storage.exists(sectionPath));
  const std::vector<uint8_t>& bytes = harness.storage.bytes(sectionPath);
  const std::string xhtml(bytes.begin(), bytes.end());
  EXPECT_NE(xhtml.find(">2. Materials and methods</h1>"), std::string::npos) << xhtml;
  EXPECT_NE(xhtml.find(">2.1. Experimental materials</h2>"), std::string::npos) << xhtml;
  EXPECT_NE(xhtml.find(">2.2. H-PDINH preparation</h2>"), std::string::npos) << xhtml;
  EXPECT_EQ(xhtml.find("methods 2.1."), std::string::npos) << xhtml;
}

TEST(PdfPreparation, ReflowsVisualLinesIntoHeadingsParagraphsAndDropCaps) {
  constexpr char sourcePath[] = "/books/semantic-lines.pdf";
  const std::string content =
      "BT "
      "/F1 24 Tf 1 0 0 1 72 720 Tm (THE TWO-STEP PROCESS T) Tj "
      "1 0 0 1 382 718 Tm (O CHANGING) Tj "
      "1 0 0 1 72 702 Tm (YOUR IDENTITY) Tj "
      "/F1 18 Tf 1 0 0 1 72 650 Tm (Why Tiny Changes Make a Big) Tj "
      "1 0 0 1 72 632 Tm (Difference) Tj "
      "/F1 32 Tf 1 0 0 1 72 540 Tm (O) Tj "
      "/F1 12 Tf 1 0 0 1 100 550 Tm (N THE FINAL day of school, everything changed.) Tj "
      "1 0 0 1 72 534 Tm (This sentence continues on the next visual line.) Tj "
      "1 0 0 1 90 500 Tm (Next paragraph starts here and) Tj "
      "1 0 0 1 72 484 Tm (continues without a forced line break.) Tj "
      "ET";
  PreparationHarness harness;
  harness.storage.addFile(sourcePath, makeOnePageTextPdf(content), 1234, true);

  PdfPreparation preparation;
  ASSERT_TRUE(preparation.begin(harness.config(sourcePath)).ok());
  const PdfStepResult result = runToTerminal(preparation, harness);
  ASSERT_TRUE(result.complete()) << static_cast<int>(result.status.error) << "@" << result.status.offset;

  const std::string sectionPath = std::string(preparation.cacheRoot()) + "/gen_" +
                                  std::to_string(preparation.generation()) + "/sections/000000.xhtml";
  ASSERT_TRUE(harness.storage.exists(sectionPath));
  const std::vector<uint8_t>& bytes = harness.storage.bytes(sectionPath);
  const std::string xhtml(bytes.begin(), bytes.end());
  EXPECT_NE(xhtml.find("<h1 id=\"b00000000\">THE TWO-STEP PROCESS TO CHANGING YOUR IDENTITY</h1>"),
            std::string::npos)
      << xhtml;
  EXPECT_NE(xhtml.find("<h2 id=\"b00000001\">Why Tiny Changes Make a Big Difference</h2>"),
            std::string::npos)
      << xhtml;
  EXPECT_NE(xhtml.find("<p id=\"b00000002\">ON THE FINAL day of school, everything changed. This sentence "
                       "continues on the next visual line.</p>"),
            std::string::npos)
      << xhtml;
  EXPECT_NE(xhtml.find("<p id=\"b00000003\">Next paragraph starts here and continues without a forced line "
                       "break.</p>"),
            std::string::npos)
      << xhtml;
  EXPECT_EQ(preparation.totalWords(), 41U);
}

TEST(PdfPreparation, RecognizesBodySizeBoldAllCapsSubheadingsInTheTopContentBand) {
  struct Case {
    const char* headingContent;
    const char* expected;
  };
  constexpr Case cases[] = {
      {"1 0 0 1 72 720 Tm (THE REAL REASON HABITS MA) Tj 1 0 0 1 270 720 Tm (TTER) Tj ",
       "THE REAL REASON HABITS MATTER"},
      {"1 0 0 1 72 720 Tm (HOW LONG DOES IT ACTUALL Y) Tj ", "HOW LONG DOES IT ACTUALLY"},
  };
  for (const Case& testCase : cases) {
    constexpr char sourcePath[] = "/books/top-band-heading.pdf";
    const std::string content = std::string("BT /F1 12 Tf ") + testCase.headingContent +
                                "1 0 0 1 72 650 Tm (Ordinary body paragraph text.) Tj ET";
    PreparationHarness harness;
    harness.storage.addFile(sourcePath, makeOnePageTextPdf(content, 0, "/Helvetica-Bold"), 1234, true);

    PdfPreparation preparation;
    ASSERT_TRUE(preparation.begin(harness.config(sourcePath)).ok());
    const PdfStepResult result = runToTerminal(preparation, harness);
    ASSERT_TRUE(result.complete()) << static_cast<int>(result.status.error) << "@" << result.status.offset;

    const std::string sectionPath = std::string(preparation.cacheRoot()) + "/gen_" +
                                    std::to_string(preparation.generation()) + "/sections/000000.xhtml";
    ASSERT_TRUE(harness.storage.exists(sectionPath));
    const std::vector<uint8_t>& bytes = harness.storage.bytes(sectionPath);
    const std::string xhtml(bytes.begin(), bytes.end());
    const std::string expected = std::string(">") + testCase.expected + "</h1>";
    EXPECT_NE(xhtml.find(expected), std::string::npos) << xhtml;
    EXPECT_NE(xhtml.find(">Ordinary body paragraph text.</p>"), std::string::npos) << xhtml;
  }
}

TEST(PdfPreparation, KeepsTerminalPunctuationOnASecondHeadingLine) {
  constexpr char sourcePath[] = "/books/two-line-terminal-heading.pdf";
  const std::string content =
      "BT "
      "/F1 15 Tf 1 0 0 1 72 720 Tm (HOW LONG DOES IT ACTUALLY TAKE TO FORM A NEW) Tj "
      "1 0 0 1 278 702 Tm (HABIT?) Tj "
      "/F1 11 Tf 1 0 0 1 72 650 Tm (Ordinary body paragraph text.) Tj "
      "1 0 0 1 72 634 Tm (More ordinary body text follows here.) Tj "
      "1 0 0 1 72 618 Tm (Another ordinary body line follows here.) Tj "
      "1 0 0 1 72 602 Tm (The body text establishes its normal size.) Tj ET";
  PreparationHarness harness;
  harness.storage.addFile(sourcePath, makeTwoPageTextPdf(false, false, content.c_str(), "/Helvetica-Bold"), 1234,
                          true);

  PdfPreparation preparation;
  ASSERT_TRUE(preparation.begin(harness.config(sourcePath)).ok());
  const PdfStepResult result = runToTerminal(preparation, harness);
  ASSERT_TRUE(result.complete()) << static_cast<int>(result.status.error) << "@" << result.status.offset;

  const std::string sectionPath = std::string(preparation.cacheRoot()) + "/gen_" +
                                  std::to_string(preparation.generation()) + "/sections/000000.xhtml";
  ASSERT_TRUE(harness.storage.exists(sectionPath));
  const std::vector<uint8_t>& bytes = harness.storage.bytes(sectionPath);
  const std::string xhtml(bytes.begin(), bytes.end());
  EXPECT_NE(xhtml.find(">HOW LONG DOES IT ACTUALLY TAKE TO FORM A NEW HABIT?</h2>"), std::string::npos)
      << xhtml;
  EXPECT_NE(xhtml.find(">Ordinary body paragraph text."), std::string::npos) << xhtml;
}

TEST(PdfPreparation, KeepsWidelySpacedCoverTitleLinesInOneHeading) {
  constexpr char sourcePath[] = "/books/widely-spaced-cover-title.pdf";
  const std::string content =
      "BT "
      "/F1 45 Tf 1 0 0 1 45 650 Tm (Corporate) Tj "
      "1 0 0 1 45 605 Tm (Design) Tj "
      "1 0 0 1 45 560 Tm (Manual) Tj "
      "/F1 11 Tf 1 0 0 1 72 500 Tm (Ordinary body paragraph text.) Tj "
      "1 0 0 1 72 484 Tm (More ordinary body text follows here.) Tj "
      "1 0 0 1 72 468 Tm (Another ordinary body line follows here.) Tj "
      "1 0 0 1 72 452 Tm (The body text establishes its normal size.) Tj ET";
  PreparationHarness harness;
  harness.storage.addFile(sourcePath, makeOnePageTextPdf(content), 1234, true);

  PdfPreparation preparation;
  ASSERT_TRUE(preparation.begin(harness.config(sourcePath)).ok());
  const PdfStepResult result = runToTerminal(preparation, harness);
  ASSERT_TRUE(result.complete()) << static_cast<int>(result.status.error) << "@" << result.status.offset;

  const std::string sectionPath = std::string(preparation.cacheRoot()) + "/gen_" +
                                  std::to_string(preparation.generation()) + "/sections/000000.xhtml";
  ASSERT_TRUE(harness.storage.exists(sectionPath));
  const std::vector<uint8_t>& bytes = harness.storage.bytes(sectionPath);
  const std::string xhtml(bytes.begin(), bytes.end());
  EXPECT_NE(xhtml.find(">Corporate Design Manual</h1>"), std::string::npos) << xhtml;
  EXPECT_EQ(xhtml.find(">Corporate</h1>"), std::string::npos) << xhtml;
  EXPECT_EQ(xhtml.find(">Design</h2>"), std::string::npos) << xhtml;
  EXPECT_EQ(xhtml.find(">Manual</h2>"), std::string::npos) << xhtml;
}

TEST(PdfPreparation, KeepsALongFirstPageTitleWithALowercaseSecondLineAsOneHeading) {
  constexpr char sourcePath[] = "/books/lowercase-title-continuation.pdf";
  const std::string content =
      "BT "
      "/F1 16 Tf 1 0 0 1 72 690 Tm (Novel composite materials for photodegradation under simulated) Tj "
      "1 0 0 1 72 670 Tm (sunshine irradiation) Tj "
      "/F1 11 Tf 1 0 0 1 72 620 Tm (Ordinary body paragraph text starts here.) Tj "
      "1 0 0 1 72 604 Tm (More ordinary body text follows here.) Tj "
      "1 0 0 1 72 588 Tm (The body text establishes its normal size.) Tj ET";
  PreparationHarness harness;
  harness.storage.addFile(sourcePath, makeOnePageTextPdf(content), 1234, true);

  PdfPreparation preparation;
  ASSERT_TRUE(preparation.begin(harness.config(sourcePath)).ok());
  const PdfStepResult result = runToTerminal(preparation, harness);
  ASSERT_TRUE(result.complete()) << static_cast<int>(result.status.error) << "@" << result.status.offset;

  const std::string sectionPath = std::string(preparation.cacheRoot()) + "/gen_" +
                                  std::to_string(preparation.generation()) + "/sections/000000.xhtml";
  ASSERT_TRUE(harness.storage.exists(sectionPath));
  const std::vector<uint8_t>& bytes = harness.storage.bytes(sectionPath);
  const std::string xhtml(bytes.begin(), bytes.end());
  EXPECT_NE(xhtml.find(">Novel composite materials for photodegradation under simulated sunshine "
                       "irradiation</h1>"),
            std::string::npos)
      << xhtml;
}

TEST(PdfPreparation, DoesNotPromoteACenteredBodyFragmentToASectionHeading) {
  constexpr char sourcePath[] = "/books/centered-body-fragment.pdf";
  const std::string content =
      "BT "
      "/F1 15 Tf 1 0 0 1 76 700 Tm (Find updated endnotes and corrections at) Tj "
      "1 0 0 1 244 700 Tm (atomichabits.com/endnotes) Tj "
      "1 0 0 1 408 700 Tm (.) Tj "
      "/F1 11 Tf 1 0 0 1 262 670 Tm (INTRODUCTION) Tj "
      "1 0 0 1 72 640 Tm (Ordinary body paragraph text.) Tj "
      "1 0 0 1 72 624 Tm (More ordinary body text follows here.) Tj "
      "1 0 0 1 72 608 Tm (The body text establishes its normal size.) Tj ET";
  PreparationHarness harness;
  harness.storage.addFile(sourcePath, makeOnePageTextPdf(content, 0, "/Helvetica-Bold"), 1234, true);

  PdfPreparation preparation;
  ASSERT_TRUE(preparation.begin(harness.config(sourcePath)).ok());
  const PdfStepResult result = runToTerminal(preparation, harness);
  ASSERT_TRUE(result.complete()) << static_cast<int>(result.status.error) << "@" << result.status.offset;

  const std::string sectionPath = std::string(preparation.cacheRoot()) + "/gen_" +
                                  std::to_string(preparation.generation()) + "/sections/000000.xhtml";
  ASSERT_TRUE(harness.storage.exists(sectionPath));
  const std::vector<uint8_t>& bytes = harness.storage.bytes(sectionPath);
  const std::string xhtml(bytes.begin(), bytes.end());
  EXPECT_NE(xhtml.find("atomichabits.com/endnotes.</p>"), std::string::npos) << xhtml;
  EXPECT_NE(xhtml.find(">INTRODUCTION</h2>"), std::string::npos) << xhtml;
  EXPECT_EQ(xhtml.find("atomichabits.com/endnotes.</h"), std::string::npos) << xhtml;
}

TEST(PdfPreparation, RejoinsAWordSplitAtTheStartOfAHeadingFragment) {
  constexpr char sourcePath[] = "/books/split-word-heading-fragment.pdf";
  const std::string content =
      "BT "
      "/F1 15 Tf 1 0 0 1 110 720 Tm (THE MISMATCH BETWEEN IMMEDIA) Tj "
      "1 0 0 1 355 720 Tm (TE AND DELAYED) Tj "
      "1 0 0 1 267 702 Tm (REWARDS) Tj "
      "/F1 11 Tf 1 0 0 1 72 650 Tm (Ordinary body paragraph text.) Tj "
      "1 0 0 1 72 634 Tm (More ordinary body text follows here.) Tj "
      "1 0 0 1 72 618 Tm (Another ordinary body line follows here.) Tj "
      "1 0 0 1 72 602 Tm (The body text establishes its normal size.) Tj ET";
  PreparationHarness harness;
  harness.storage.addFile(sourcePath, makeTwoPageTextPdf(false, false, content.c_str(), "/Helvetica-Bold"), 1234,
                          true);

  PdfPreparation preparation;
  ASSERT_TRUE(preparation.begin(harness.config(sourcePath)).ok());
  const PdfStepResult result = runToTerminal(preparation, harness);
  ASSERT_TRUE(result.complete()) << static_cast<int>(result.status.error) << "@" << result.status.offset;

  const std::string sectionPath = std::string(preparation.cacheRoot()) + "/gen_" +
                                  std::to_string(preparation.generation()) + "/sections/000000.xhtml";
  ASSERT_TRUE(harness.storage.exists(sectionPath));
  const std::vector<uint8_t>& bytes = harness.storage.bytes(sectionPath);
  const std::string xhtml(bytes.begin(), bytes.end());
  EXPECT_NE(xhtml.find(">THE MISMATCH BETWEEN IMMEDIATE AND DELAYED REWARDS</h2>"), std::string::npos)
      << xhtml;
}

TEST(PdfPreparation, DistinguishesStandaloneDropCapIFromIn) {
  struct Case {
    const char* target;
    const char* expected;
  };
  constexpr Case cases[] = {
      {"HAVE RELIED HEAVILY on others.", "I HAVE RELIED HEAVILY on others."},
      {"N 2001, habits changed.", "IN 2001, habits changed."},
  };
  for (const Case& testCase : cases) {
    constexpr char sourcePath[] = "/books/drop-cap-i.pdf";
    const std::string content = std::string("BT ") +
                                "/F1 32 Tf 1 0 0 1 72 540 Tm (I) Tj " +
                                "/F1 12 Tf 1 0 0 1 100 550 Tm (" + testCase.target + ") Tj " +
                                "1 0 0 1 72 534 Tm (This line completes the paragraph.) Tj ET";
    PreparationHarness harness;
    harness.storage.addFile(sourcePath, makeOnePageTextPdf(content), 1234, true);

    PdfPreparation preparation;
    ASSERT_TRUE(preparation.begin(harness.config(sourcePath)).ok());
    const PdfStepResult result = runToTerminal(preparation, harness);
    ASSERT_TRUE(result.complete()) << static_cast<int>(result.status.error) << "@" << result.status.offset;

    const std::string sectionPath = std::string(preparation.cacheRoot()) + "/gen_" +
                                    std::to_string(preparation.generation()) + "/sections/000000.xhtml";
    ASSERT_TRUE(harness.storage.exists(sectionPath));
    const std::vector<uint8_t>& bytes = harness.storage.bytes(sectionPath);
    const std::string xhtml(bytes.begin(), bytes.end());
    const std::string expected = std::string("<p id=\"b00000000\">") + testCase.expected +
                                 " This line completes the paragraph.</p>";
    EXPECT_NE(xhtml.find(expected), std::string::npos) << xhtml;
  }
}

TEST(PdfPreparation, KeepsSplitDropCapOpeningLineInTheParagraph) {
  constexpr char sourcePath[] = "/books/drop-cap-split-line.pdf";
  const std::string content =
      "BT "
      "/F1 32 Tf 1 0 0 1 72 540 Tm (H) Tj "
      "/F1 12 Tf 1 0 0 1 100 550 Tm (ABITS CREAT) Tj "
      "1 0 0 1 180 550 Tm (E THE FOUNDATION FOR MASTERY.) Tj ET";
  PreparationHarness harness;
  harness.storage.addFile(sourcePath, makeOnePageTextPdf(content), 1234, true);

  PdfPreparation preparation;
  ASSERT_TRUE(preparation.begin(harness.config(sourcePath)).ok());
  const PdfStepResult result = runToTerminal(preparation, harness);
  ASSERT_TRUE(result.complete()) << static_cast<int>(result.status.error) << "@" << result.status.offset;

  const std::string sectionPath = std::string(preparation.cacheRoot()) + "/gen_" +
                                  std::to_string(preparation.generation()) + "/sections/000000.xhtml";
  ASSERT_TRUE(harness.storage.exists(sectionPath));
  const std::vector<uint8_t>& bytes = harness.storage.bytes(sectionPath);
  const std::string xhtml(bytes.begin(), bytes.end());
  EXPECT_NE(xhtml.find("<p id=\"b00000000\">HABITS CREATE THE FOUNDATION FOR MASTERY.</p>"),
            std::string::npos)
      << xhtml;
  EXPECT_EQ(xhtml.find("<h1"), std::string::npos) << xhtml;
  EXPECT_EQ(xhtml.find("<h2"), std::string::npos) << xhtml;
}

TEST(PdfPreparation, KeepsSplitFullWidthHeadingAheadOfColumnText) {
  constexpr char sourcePath[] = "/books/split-heading-columns.pdf";
  std::string content =
      "BT /F1 12 Tf "
      "1 0 0 1 72 720 Tm (ONETIME ACTIONS THA) Tj "
      "1 0 0 1 300 720 Tm (T LOCK IN GOOD HABITS) Tj ";
  for (uint8_t row = 0; row < 6; ++row) {
    char line[128]{};
    const int length = std::snprintf(line, sizeof(line),
                                     "1 0 0 1 72 %u Tm (left item %u) Tj "
                                     "1 0 0 1 300 %u Tm (right item %u) Tj ",
                                     static_cast<unsigned>(680U - row * 16U), static_cast<unsigned>(row),
                                     static_cast<unsigned>(680U - row * 16U), static_cast<unsigned>(row));
    ASSERT_GT(length, 0);
    ASSERT_LT(static_cast<size_t>(length), sizeof(line));
    content.append(line, static_cast<size_t>(length));
  }
  content += "ET";
  PreparationHarness harness;
  harness.storage.addFile(sourcePath, makeOnePageTextPdf(content), 1234, true);

  PdfPreparation preparation;
  ASSERT_TRUE(preparation.begin(harness.config(sourcePath)).ok());
  const PdfStepResult result = runToTerminal(preparation, harness);
  ASSERT_TRUE(result.complete()) << static_cast<int>(result.status.error) << "@" << result.status.offset;

  const std::string sectionPath = std::string(preparation.cacheRoot()) + "/gen_" +
                                  std::to_string(preparation.generation()) + "/sections/000000.xhtml";
  ASSERT_TRUE(harness.storage.exists(sectionPath));
  const std::vector<uint8_t>& bytes = harness.storage.bytes(sectionPath);
  const std::string xhtml(bytes.begin(), bytes.end());
  const size_t heading = xhtml.find("<h1 id=\"b00000000\">ONETIME ACTIONS THAT LOCK IN GOOD HABITS</h1>");
  ASSERT_NE(heading, std::string::npos) << xhtml;
  EXPECT_LT(heading, xhtml.find("left item 0")) << xhtml;
  EXPECT_EQ(xhtml.find("<h2"), std::string::npos) << xhtml;
}

TEST(PdfPreparation, JoinsStackedChapterNumberAndTitleAtSectionStart) {
  constexpr char sourcePath[] = "/books/stacked-chapter-title.pdf";
  const std::string content =
      "BT "
      "/F1 25 Tf 1 0 0 1 300 650 Tm (1) Tj "
      "1 0 0 1 90 600 Tm (The Surprising Power of Atomic Habits) Tj "
      "/F1 12 Tf 1 0 0 1 72 540 Tm (Body text starts here.) Tj ET";
  PreparationHarness harness;
  harness.storage.addFile(sourcePath, makeOnePageTextPdf(content), 1234, true);

  PdfPreparation preparation;
  ASSERT_TRUE(preparation.begin(harness.config(sourcePath)).ok());
  const PdfStepResult result = runToTerminal(preparation, harness);
  ASSERT_TRUE(result.complete()) << static_cast<int>(result.status.error) << "@" << result.status.offset;

  const std::string sectionPath = std::string(preparation.cacheRoot()) + "/gen_" +
                                  std::to_string(preparation.generation()) + "/sections/000000.xhtml";
  ASSERT_TRUE(harness.storage.exists(sectionPath));
  const std::vector<uint8_t>& bytes = harness.storage.bytes(sectionPath);
  const std::string xhtml(bytes.begin(), bytes.end());
  EXPECT_NE(xhtml.find("<h1 id=\"b00000000\">1 The Surprising Power of Atomic Habits</h1>"),
            std::string::npos)
      << xhtml;
  EXPECT_EQ(xhtml.find("<h2"), std::string::npos) << xhtml;
}

TEST(PdfPreparation, RejoinsAWordSplitByAnAbsoluteSameLineTextPosition) {
  constexpr char sourcePath[] = "/books/positioned-word-split.pdf";
  const std::string content =
      "BT /F1 12 Tf "
      "1 0 0 1 72 600 Tm (the system si) Tj "
      "1 0 0 1 159 600 Tm (gnal format is readable.) Tj "
      "1 0 0 1 72 560 Tm (XKS-G17) Tj "
      "1 0 0 1 116 560 Tm (00 Legacy Interface Board) Tj ET";
  PreparationHarness harness;
  harness.storage.addFile(sourcePath, makeOnePageTextPdf(content), 1234, true);

  PdfPreparation preparation;
  ASSERT_TRUE(preparation.begin(harness.config(sourcePath)).ok());
  const PdfStepResult result = runToTerminal(preparation, harness);
  ASSERT_TRUE(result.complete()) << static_cast<int>(result.status.error) << "@" << result.status.offset;

  const std::string sectionPath = std::string(preparation.cacheRoot()) + "/gen_" +
                                  std::to_string(preparation.generation()) + "/sections/000000.xhtml";
  ASSERT_TRUE(harness.storage.exists(sectionPath));
  const std::vector<uint8_t>& bytes = harness.storage.bytes(sectionPath);
  const std::string xhtml(bytes.begin(), bytes.end());
  EXPECT_NE(xhtml.find(">the system signal format is readable.</p>"), std::string::npos) << xhtml;
  EXPECT_NE(xhtml.find(">XKS-G1700 Legacy Interface Board</p>"), std::string::npos) << xhtml;
}

TEST(PdfPreparation, OrdersColumnsDownBeforePreservingRowMajorTableCells) {
  constexpr char sourcePath[] = "/books/columns-table.pdf";
  PreparationHarness harness;
  harness.storage.addFile(sourcePath, loadFixture("columns_table.pdf"), 1234, true);

  PdfPreparation preparation;
  ASSERT_TRUE(preparation.begin(harness.config(sourcePath)).ok());
  const PdfStepResult result = runToTerminal(preparation, harness);
  ASSERT_TRUE(result.complete()) << static_cast<int>(result.status.error) << "@" << result.status.offset;
  EXPECT_EQ(preparation.totalWords(), 12U);

  const std::string sectionPath = std::string(preparation.cacheRoot()) + "/gen_" +
                                  std::to_string(preparation.generation()) + "/sections/000000.xhtml";
  ASSERT_TRUE(harness.storage.exists(sectionPath));
  const std::vector<uint8_t>& bytes = harness.storage.bytes(sectionPath);
  const std::string xhtml(bytes.begin(), bytes.end());
  const char* const expectedBlocks[] = {
      "<p id=\"b00000000\">Left one. Left two.</p>",
      "<p id=\"b00000001\">Right one. Right two.</p>",
      "<table><tbody><tr><td id=\"b00000002\">Name</td>",
      "<td id=\"b00000003\">Value</td></tr><tr>",
      "<td id=\"b00000004\">Alpha</td>",
      "<td id=\"b00000005\">10</td></tr></tbody></table>",
  };
  size_t previous = 0;
  for (const char* const expected : expectedBlocks) {
    const size_t position = xhtml.find(expected, previous);
    ASSERT_NE(position, std::string::npos) << expected << '\n' << xhtml;
    previous = position + std::strlen(expected);
  }

  PdfTestByteSource metadataSource(
      harness.storage.bytes(std::string(preparation.cacheRoot()) + "/gen_" +
                            std::to_string(preparation.generation()) + "/metadata.bin"));
  PdfMetadata metadata{};
  std::vector<PdfMetadataSection> sections;
  ASSERT_TRUE(pdfDecodeMetadata(
                  metadataSource.source(), &metadata,
                  {&sections,
                   [](void* context, uint16_t, const PdfMetadataSection& section) {
                     static_cast<std::vector<PdfMetadataSection>*>(context)->push_back(section);
                     return PdfStatus::success();
                   }})
                  .ok());
  ASSERT_EQ(sections.size(), 1U);
  EXPECT_EQ(sections[0].firstWordOrdinal, 0U);
  EXPECT_EQ(sections[0].wordCount, 12U);
  EXPECT_EQ(sections[0].firstAnchorOrdinal, 0U);
}

TEST(PdfPreparation, DoesNotTreatSparseInlineFragmentsAsASecondColumn) {
  constexpr char sourcePath[] = "/books/inline-fragments-not-columns.pdf";
  std::string content = "BT /F1 11 Tf ";
  for (uint8_t line = 0; line < 31U; ++line) {
    char text[192]{};
    const unsigned y = 760U - static_cast<unsigned>(line) * 16U;
    const int length = line < 6U
                           ? std::snprintf(text, sizeof(text),
                                           "1 0 0 1 72 %u Tm (Inline left %u continues ) Tj "
                                           "1 0 0 1 380 %u Tm (right %u.) Tj ",
                                           y, static_cast<unsigned>(line), y, static_cast<unsigned>(line))
                           : std::snprintf(text, sizeof(text),
                                           "1 0 0 1 72 %u Tm (Ordinary line %u remains in the main text.) Tj ",
                                           y, static_cast<unsigned>(line));
    ASSERT_GT(length, 0);
    ASSERT_LT(static_cast<size_t>(length), sizeof(text));
    content.append(text, static_cast<size_t>(length));
  }
  content += "ET";
  PreparationHarness harness;
  harness.storage.addFile(sourcePath, makeOnePageTextPdf(content), 1234, true);

  PdfPreparation preparation;
  ASSERT_TRUE(preparation.begin(harness.config(sourcePath)).ok());
  const PdfStepResult result = runToTerminal(preparation, harness);
  ASSERT_TRUE(result.complete()) << static_cast<int>(result.status.error) << "@" << result.status.offset;

  const std::string sectionPath = std::string(preparation.cacheRoot()) + "/gen_" +
                                  std::to_string(preparation.generation()) + "/sections/000000.xhtml";
  ASSERT_TRUE(harness.storage.exists(sectionPath));
  const std::vector<uint8_t>& bytes = harness.storage.bytes(sectionPath);
  const std::string xhtml(bytes.begin(), bytes.end());
  const size_t left0 = xhtml.find("Inline left 0 continues");
  const size_t right0 = xhtml.find("right 0.");
  const size_t left1 = xhtml.find("Inline left 1 continues");
  ASSERT_NE(left0, std::string::npos) << xhtml;
  ASSERT_NE(right0, std::string::npos) << xhtml;
  ASSERT_NE(left1, std::string::npos) << xhtml;
  EXPECT_LT(left0, right0) << xhtml;
  EXPECT_LT(right0, left1) << xhtml;
}

TEST(PdfPreparation, DoesNotTreatCenteredSingleColumnCreditsAsASecondColumn) {
  constexpr char sourcePath[] = "/books/centered-credits-not-columns.pdf";
  std::string content =
      "BT /F1 11 Tf "
      "1 0 0 1 72 620 Tm (The body begins here and) Tj "
      "1 0 0 1 72 604 Tm (continues without) Tj "
      "1 0 0 1 80 588 Tm (a paragraph break before) Tj "
      "1 0 0 1 72 572 Tm (its next visual line and) Tj "
      "1 0 0 1 80 556 Tm (finishes by) Tj "
      "1 0 0 1 180 540 Tm (ending on its centered final line.) Tj ";
  // Deliberately put the lower, left-aligned body into the content stream
  // before the centered credits. Reading order must follow page geometry.
  for (uint8_t line = 0; line < 6U; ++line) {
    char text[96]{};
    const int length = std::snprintf(text, sizeof(text),
                                     "1 0 0 1 250 %u Tm (Credit line %u.) Tj ",
                                     static_cast<unsigned>(740U - line * 16U),
                                     static_cast<unsigned>(line));
    ASSERT_GT(length, 0);
    ASSERT_LT(static_cast<size_t>(length), sizeof(text));
    content.append(text, static_cast<size_t>(length));
  }
  content += "ET";

  PreparationHarness harness;
  harness.storage.addFile(sourcePath, makeOnePageTextPdf(content), 1234, true);

  PdfPreparation preparation;
  ASSERT_TRUE(preparation.begin(harness.config(sourcePath)).ok());
  const PdfStepResult result = runToTerminal(preparation, harness);
  ASSERT_TRUE(result.complete()) << static_cast<int>(result.status.error) << "@" << result.status.offset;

  const std::string sectionPath = std::string(preparation.cacheRoot()) + "/gen_" +
                                  std::to_string(preparation.generation()) + "/sections/000000.xhtml";
  ASSERT_TRUE(harness.storage.exists(sectionPath));
  const std::vector<uint8_t>& bytes = harness.storage.bytes(sectionPath);
  const std::string xhtml(bytes.begin(), bytes.end());
  const size_t firstCredit = xhtml.find("Credit line 0.");
  const size_t lastCredit = xhtml.find("Credit line 5.");
  const size_t firstBody = xhtml.find("The body begins here and");
  ASSERT_NE(firstCredit, std::string::npos) << xhtml;
  ASSERT_NE(lastCredit, std::string::npos) << xhtml;
  ASSERT_NE(firstBody, std::string::npos) << xhtml;
  EXPECT_LT(firstCredit, lastCredit) << xhtml;
  EXPECT_LT(lastCredit, firstBody) << xhtml;
  EXPECT_NE(xhtml.find(">The body begins here and continues without a paragraph break before its next visual "
                       "line and finishes by ending on its centered final line.</p>"),
            std::string::npos)
      << xhtml;
}

TEST(PdfPreparation, DoesNotTreatStyledProseFragmentsAsWideTableCells) {
  constexpr char sourcePath[] = "/books/styled-prose-not-table.pdf";
  const std::string content =
      "BT /F1 11 Tf "
      "1 0 0 1 72 720 Tm (The first three laws increase the odds that a behavior will be) Tj "
      "1 0 0 1 72 702 Tm (performed ) Tj "
      "/F2 11 Tf 1 0 0 1 72 702 Tm (this) Tj "
      "/F1 11 Tf 1 0 0 1 72 702 Tm ( time. The fourth law continues ) Tj "
      "/F2 11 Tf 1 0 0 1 425 702 Tm (make it satisfying.) Tj ET";
  PreparationHarness harness;
  harness.storage.addFile(
      sourcePath,
      assembleClassicPdf({
          "<< /Type /Catalog /Pages 2 0 R >>",
          "<< /Type /Pages /Count 1 /Kids [3 0 R] >>",
          "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] "
          "/Resources << /Font << /F1 5 0 R /F2 6 0 R >> >> /Contents 4 0 R >>",
          "<< /Length " + std::to_string(content.size()) + ">>\nstream\n" + content + "\nendstream",
          "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>",
          "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica-Oblique >>",
      }),
      1234, true);

  PdfPreparation preparation;
  ASSERT_TRUE(preparation.begin(harness.config(sourcePath)).ok());
  const PdfStepResult result = runToTerminal(preparation, harness);
  ASSERT_TRUE(result.complete()) << static_cast<int>(result.status.error) << "@" << result.status.offset;

  const std::string sectionPath = std::string(preparation.cacheRoot()) + "/gen_" +
                                  std::to_string(preparation.generation()) + "/sections/000000.xhtml";
  ASSERT_TRUE(harness.storage.exists(sectionPath));
  const std::vector<uint8_t>& bytes = harness.storage.bytes(sectionPath);
  const std::string xhtml(bytes.begin(), bytes.end());
  EXPECT_NE(xhtml.find(">The first three laws increase the odds that a behavior will be performed this time. "
                       "The fourth law continues make it satisfying.</p>"),
            std::string::npos)
      << xhtml;
}

TEST(PdfPreparation, KeepsALeadingSpaceStyledFragmentInItsLeftColumnLine) {
  constexpr char sourcePath[] = "/books/styled-left-column-continuation.pdf";
  std::string content =
      "BT /F1 11 Tf 1 0 0 1 72 720 Tm (Designs des Kantons St.Gallen) Tj "
      "/F2 11 Tf 1 0 0 1 250 720 Tm ( macht die ) Tj "
      "1 0 0 1 360 720 Tm (Right column zero.) Tj ";
  for (uint8_t row = 1; row <= 6U; ++row) {
    char lines[160]{};
    const unsigned y = 720U - static_cast<unsigned>(row) * 18U;
    const int length = std::snprintf(lines, sizeof(lines),
                                     "1 0 0 1 72 %u Tm (Left column row %u.) Tj "
                                     "1 0 0 1 360 %u Tm (Right column row %u.) Tj ",
                                     y, static_cast<unsigned>(row), y, static_cast<unsigned>(row));
    ASSERT_GT(length, 0);
    ASSERT_LT(static_cast<size_t>(length), sizeof(lines));
    content.append(lines, static_cast<size_t>(length));
  }
  content += "ET";

  PreparationHarness harness;
  harness.storage.addFile(
      sourcePath,
      assembleClassicPdf({
          "<< /Type /Catalog /Pages 2 0 R >>",
          "<< /Type /Pages /Count 1 /Kids [3 0 R] >>",
          "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] "
          "/Resources << /Font << /F1 5 0 R /F2 6 0 R >> >> /Contents 4 0 R >>",
          "<< /Length " + std::to_string(content.size()) + ">>\nstream\n" + content + "\nendstream",
          "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica-Bold >>",
          "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>",
      }),
      1234, true);

  PdfPreparation preparation;
  ASSERT_TRUE(preparation.begin(harness.config(sourcePath)).ok());
  const PdfStepResult result = runToTerminal(preparation, harness);
  ASSERT_TRUE(result.complete()) << static_cast<int>(result.status.error) << "@" << result.status.offset;

  const std::string sectionPath = std::string(preparation.cacheRoot()) + "/gen_" +
                                  std::to_string(preparation.generation()) + "/sections/000000.xhtml";
  ASSERT_TRUE(harness.storage.exists(sectionPath));
  const std::vector<uint8_t>& bytes = harness.storage.bytes(sectionPath);
  const std::string xhtml(bytes.begin(), bytes.end());
  const size_t styledLine = xhtml.find("Designs des Kantons St.Gallen macht die");
  const size_t lastLeft = xhtml.find("Left column row 6.");
  const size_t firstRight = xhtml.find("Right column zero.");
  ASSERT_NE(styledLine, std::string::npos) << xhtml;
  ASSERT_NE(lastLeft, std::string::npos) << xhtml;
  ASSERT_NE(firstRight, std::string::npos) << xhtml;
  EXPECT_LT(styledLine, lastLeft) << xhtml;
  EXPECT_LT(lastLeft, firstRight) << xhtml;
}

TEST(PdfPreparation, SeparatesWideSameLineTableCellsWhenTheFontAlsoContainsSpaces) {
  constexpr char sourcePath[] = "/books/wide-table-cells.pdf";
  std::string content = "BT /F1 10 Tf ";
  for (uint8_t line = 0; line < 12; ++line) {
    char body[96]{};
    const int length = std::snprintf(body, sizeof(body),
                                     "1 0 0 1 72 %u Tm (Body line %u keeps prose in one column.) Tj ",
                                     static_cast<unsigned>(760U - line * 12U), static_cast<unsigned>(line));
    ASSERT_GT(length, 0);
    ASSERT_LT(static_cast<size_t>(length), sizeof(body));
    content.append(body, static_cast<size_t>(length));
  }
  content +=
      "1 0 0 1 72 580 Tm (Very easy) Tj "
      "1 0 0 1 180 580 Tm (Easy) Tj "
      "1 0 0 1 270 580 Tm (Moderate) Tj "
      "1 0 0 1 380 580 Tm (Hard) Tj "
      "1 0 0 1 470 580 Tm (Very hard) Tj "
      "1 0 0 1 72 568 Tm (Put on shoes) Tj "
      "1 0 0 1 180 568 Tm (Walk ten minutes) Tj "
      "1 0 0 1 270 568 Tm (Walk ten thousand steps) Tj "
      "1 0 0 1 380 568 Tm (Run a 5K) Tj "
      "1 0 0 1 470 568 Tm (Run a marathon) Tj "
      "1 0 0 1 72 548 Tm (Open your) Tj "
      "1 0 0 1 72 536 Tm (notes) Tj "
      "1 0 0 1 180 548 Tm (Study for ten) Tj "
      "1 0 0 1 180 536 Tm (minutes) Tj "
      "1 0 0 1 300 548 Tm (Earn a) Tj "
      "1 0 0 1 300 536 Tm (degree) Tj "
      "1 0 0 1 430 548 Tm (Write a) Tj "
      "1 0 0 1 430 536 Tm (book) Tj "
      "1 0 0 1 90 490 Tm (Continue with the surrounding paragraph and) Tj "
      "1 0 0 1 72 478 Tm (join its next visual line normally.) Tj ET";
  PreparationHarness harness;
  harness.storage.addFile(sourcePath, makeOnePageTextPdf(content), 1234, true);

  PdfPreparation preparation;
  ASSERT_TRUE(preparation.begin(harness.config(sourcePath)).ok());
  const PdfStepResult result = runToTerminal(preparation, harness);
  ASSERT_TRUE(result.complete()) << static_cast<int>(result.status.error) << "@" << result.status.offset;

  const std::string sectionPath = std::string(preparation.cacheRoot()) + "/gen_" +
                                  std::to_string(preparation.generation()) + "/sections/000000.xhtml";
  ASSERT_TRUE(harness.storage.exists(sectionPath));
  const std::vector<uint8_t>& bytes = harness.storage.bytes(sectionPath);
  const std::string xhtml(bytes.begin(), bytes.end());
  EXPECT_NE(xhtml.find("<table><tbody><tr><td id=\"b00000001\">Very easy</td>"), std::string::npos)
      << xhtml;
  EXPECT_NE(xhtml.find(">Very hard</td></tr><tr><td id=\"b00000006\">Put on shoes</td>"),
            std::string::npos)
      << xhtml;
  EXPECT_NE(xhtml.find(">Run a marathon</td></tr><tr><td id=\"b0000000b\">Open your notes</td>"),
            std::string::npos)
      << xhtml;
  EXPECT_NE(xhtml.find(">Study for ten minutes</td>"), std::string::npos) << xhtml;
  EXPECT_NE(xhtml.find(">Earn a degree</td>"), std::string::npos) << xhtml;
  EXPECT_NE(xhtml.find(">Write a book</td></tr></tbody></table>"), std::string::npos) << xhtml;
  EXPECT_NE(xhtml.find(">Continue with the surrounding paragraph and join its next visual line normally.</p>"),
            std::string::npos)
      << xhtml;
}

TEST(PdfPreparation, EmitsMaximumAlignedThreeColumnGridAsTableAcrossAtLeastFourBoundedSlices) {
  constexpr char sourcePath[] = "/books/three-columns-256.pdf";
  std::string content = "BT /F1 8 Tf\n";
  uint16_t emitted = 0;
  for (uint16_t row = 0; row < 86 && emitted < 256; ++row) {
    for (uint8_t column = 0; column < 3 && emitted < 256; ++column) {
      char line[64]{};
      const int length = std::snprintf(line, sizeof(line), "1 0 0 1 %u %u Tm (%c%03u) Tj\n",
                                       static_cast<unsigned>(72U + column * 180U),
                                       static_cast<unsigned>(760U - row * 8U), static_cast<char>('A' + column),
                                       static_cast<unsigned>(row));
      ASSERT_GT(length, 0);
      ASSERT_LT(static_cast<size_t>(length), sizeof(line));
      content.append(line, static_cast<size_t>(length));
      ++emitted;
    }
  }
  content += "ET";
  ASSERT_EQ(emitted, 256U);

  PreparationHarness harness;
  harness.storage.addFile(sourcePath, makeOnePageTextPdf(content), 1234, true);
  PdfPreparation preparation;
  ASSERT_TRUE(preparation.begin(harness.config(sourcePath)).ok());

  PdfStepResult result = PdfStepResult::paused();
  uint16_t orderingSlices = 0;
  for (uint32_t step = 0; step < 20000 && result.yielded(); ++step) {
    if (preparation.phase() == PdfPreparationPhase::OrderText) {
      ++orderingSlices;
    }
    result = preparation.step();
    ++harness.nowMs;
  }
  ASSERT_TRUE(result.complete()) << static_cast<int>(result.status.error) << '@' << result.status.offset;
  EXPECT_GE(orderingSlices, 4U);
  EXPECT_EQ(preparation.totalWords(), 256U);

  const std::string sectionPath = std::string(preparation.cacheRoot()) + "/gen_" +
                                  std::to_string(preparation.generation()) + "/sections/000000.xhtml";
  ASSERT_TRUE(harness.storage.exists(sectionPath));
  const auto& sectionBytes = harness.storage.bytes(sectionPath);
  const std::string xhtml(sectionBytes.begin(), sectionBytes.end());
  ASSERT_NE(xhtml.find("<table>"), std::string::npos) << xhtml;
  size_t previous = 0;
  uint16_t expectedCells = 0;
  for (uint16_t row = 0; row < 86U && expectedCells < 256U; ++row) {
    for (uint8_t column = 0; column < 3U && expectedCells < 256U; ++column) {
      char expected[64]{};
      const int length = std::snprintf(expected, sizeof(expected), "%c%03u", static_cast<char>('A' + column),
                                       static_cast<unsigned>(row));
      ASSERT_GT(length, 0);
      ASSERT_LT(static_cast<size_t>(length), sizeof(expected));
      const size_t position = xhtml.find(expected, previous);
      ASSERT_NE(position, std::string::npos) << expected << '\n' << xhtml;
      previous = position + static_cast<size_t>(length);
      ++expectedCells;
    }
  }
  EXPECT_EQ(expectedCells, 256U);
  EXPECT_NE(xhtml.find("</table>", previous), std::string::npos) << xhtml;

  PdfTestByteSource metadataSource(
      harness.storage.bytes(std::string(preparation.cacheRoot()) + "/gen_" +
                            std::to_string(preparation.generation()) + "/metadata.bin"));
  PdfMetadata metadata{};
  std::vector<PdfMetadataSection> sections;
  ASSERT_TRUE(pdfDecodeMetadata(
                  metadataSource.source(), &metadata,
                  {&sections,
                   [](void* context, uint16_t, const PdfMetadataSection& section) {
                     static_cast<std::vector<PdfMetadataSection>*>(context)->push_back(section);
                     return PdfStatus::success();
                   }})
                  .ok());
  ASSERT_EQ(sections.size(), 1U);
  EXPECT_EQ(sections[0].firstWordOrdinal, 0U);
  EXPECT_EQ(sections[0].wordCount, 256U);
  EXPECT_EQ(sections[0].firstAnchorOrdinal, 0U);
}

TEST(PdfPreparation, PreservesGeneratedNavigationInCommittedReflowCache) {
  PreparationHarness harness;
  harness.storage.addFile("/books/minimal.pdf", loadNavigationFixture(), 1234, true);

  PdfPreparation preparation;
  ASSERT_TRUE(preparation.begin(harness.config()).ok());
  const PdfStepResult result = runToTerminal(preparation, harness);
  ASSERT_TRUE(result.complete()) << static_cast<int>(result.status.error);
  ASSERT_EQ(preparation.coverCandidateSourceCount(), 2U);
  PdfCoverCandidateSource coverSource{};
  ASSERT_TRUE(preparation.coverCandidateSource(0, &coverSource));
  EXPECT_EQ(coverSource.reference.objectNumber, 3U);
  EXPECT_EQ(coverSource.sourcePageIndex, 0U);
  EXPECT_FALSE(coverSource.referenceIsResourceDictionary);
  ASSERT_TRUE(preparation.coverCandidateSource(1, &coverSource));
  EXPECT_EQ(coverSource.reference.objectNumber, 6U);
  EXPECT_EQ(coverSource.sourcePageIndex, 1U);
  EXPECT_FALSE(coverSource.referenceIsResourceDictionary);
  EXPECT_FALSE(preparation.coverCandidateSource(2, &coverSource));

  const std::string generationRoot =
      std::string(preparation.cacheRoot()) + "/gen_" + std::to_string(preparation.generation());
  const std::string section0Path = generationRoot + "/sections/000000.xhtml";
  const std::string section1Path = generationRoot + "/sections/000001.xhtml";
  ASSERT_TRUE(harness.storage.exists(section0Path));
  ASSERT_TRUE(harness.storage.exists(section1Path));
  const std::string section0(harness.storage.bytes(section0Path).begin(), harness.storage.bytes(section0Path).end());
  const std::string section1(harness.storage.bytes(section1Path).begin(), harness.storage.bytes(section1Path).end());
  EXPECT_NE(section0.find("aria-label=\"i\""), std::string::npos);
  EXPECT_NE(section1.find("aria-label=\"A-1\""), std::string::npos);
  EXPECT_NE(section0.find(">Contents</h1>"), std::string::npos);
  EXPECT_NE(section0.find("<a href=\"sections/000001.xhtml#p00000001\">Chapter Two</a>"), std::string::npos);
  EXPECT_NE(section1.find(">Index</p>"), std::string::npos);

  PdfTestByteSource metadataSource(harness.storage.bytes(generationRoot + "/metadata.bin"));
  PdfMetadata metadata{};
  std::vector<PdfMetadataSection> sections;
  const PdfMetadataSectionVisitor sectionVisitor{
      &sections,
      [](void* context, uint16_t, const PdfMetadataSection& section) {
        static_cast<std::vector<PdfMetadataSection>*>(context)->push_back(section);
        return PdfStatus::success();
      },
  };
  ASSERT_TRUE(pdfDecodeMetadata(metadataSource.source(), &metadata, sectionVisitor).ok());
  EXPECT_STREQ(metadata.title, "XMP Navigation");
  EXPECT_STREQ(metadata.author, "XMP Author");
  EXPECT_STREQ(metadata.language, "de-CH");
  EXPECT_EQ(metadata.sectionCount, 2U);
  EXPECT_EQ(metadata.outlineCount, 3U);
  EXPECT_EQ(metadata.totalWords, 10U);
  ASSERT_EQ(sections.size(), 2U);
  EXPECT_EQ(sections[1].firstAnchorOrdinal, 3U);

  PdfTestByteSource outlineSource(harness.storage.bytes(generationRoot + "/outline.bin"));
  PdfOutlineHeader outlineHeader{};
  std::vector<PdfOutlineEntry> outline;
  const PdfOutlineEntryVisitor outlineVisitor{
      &outline,
      [](void* context, uint16_t, const PdfOutlineEntry& entry) {
        static_cast<std::vector<PdfOutlineEntry>*>(context)->push_back(entry);
        return PdfStatus::success();
      },
  };
  ASSERT_TRUE(pdfDecodeOutline(outlineSource.source(), &outlineHeader, outlineVisitor).ok());
  ASSERT_EQ(outline.size(), 3U);
  EXPECT_STREQ(outline[0].title, "Part One");
  EXPECT_EQ(outline[0].level, 1U);
  EXPECT_STREQ(outline[1].title, "Chapter One");
  EXPECT_EQ(outline[1].parentIndex, 0);
  EXPECT_STREQ(outline[2].title, "Chapter Two");
  EXPECT_EQ(outline[2].sectionIndex, 1U);
  EXPECT_STREQ(outline[2].anchor, "b00000003");

  PdfCacheStore cache;
  ASSERT_TRUE(cache.initialize(harness.storage.io(), preparation.cacheRoot()).ok());
  PdfCacheManifestSelection selection;
  ASSERT_TRUE(cache.loadManifestSlots(preparation.sourceIdentity(), &selection).ok());
  ASSERT_TRUE(selection.selected);
  EXPECT_EQ(selection.manifest.requiredFileCount, 6U);
  EXPECT_TRUE(harness.storage.exists(generationRoot + "/cover.bmp"));
  EXPECT_TRUE(harness.storage.exists(generationRoot + "/thumb.bmp"));
}

TEST(PdfPreparation, ResolvesOutlineDestinationsAcrossANameTreeWithoutCachingEveryName) {
  PreparationHarness harness;
  harness.storage.addFile("/books/name-tree-outline.pdf", makeNamedDestinationTreeOutlinePdf(), 1234, true);

  PdfPreparation preparation;
  ASSERT_TRUE(preparation.begin(harness.config("/books/name-tree-outline.pdf")).ok());
  const PdfStepResult result = runToTerminal(preparation, harness);
  ASSERT_TRUE(result.complete()) << static_cast<int>(result.status.error) << '@' << result.status.offset
                                 << " phase=" << static_cast<int>(preparation.phase())
                                 << " progress=" << static_cast<int>(preparation.progressPercent());

  const std::string generationRoot =
      std::string(preparation.cacheRoot()) + "/gen_" + std::to_string(preparation.generation());
  EXPECT_TRUE(harness.storage.exists(generationRoot + "/sections/000000.xhtml"));
  EXPECT_TRUE(harness.storage.exists(generationRoot + "/sections/000001.xhtml"));

  PdfTestByteSource metadataSource(harness.storage.bytes(generationRoot + "/metadata.bin"));
  PdfMetadata metadata{};
  std::vector<PdfMetadataSection> sections;
  const PdfMetadataSectionVisitor sectionVisitor{
      &sections,
      [](void* context, uint16_t, const PdfMetadataSection& section) {
        static_cast<std::vector<PdfMetadataSection>*>(context)->push_back(section);
        return PdfStatus::success();
      },
  };
  ASSERT_TRUE(pdfDecodeMetadata(metadataSource.source(), &metadata, sectionVisitor).ok());
  EXPECT_EQ(metadata.sectionCount, 2U);
  EXPECT_EQ(metadata.outlineCount, 2U);
  EXPECT_EQ(sections.size(), 2U);
}

TEST(PdfPreparation, FallsBackToDocumentRootWithoutExplicitPdfOutline) {
  for (const char* const fixture : {"navigation_heading_fallback.pdf", "navigation_root_fallback.pdf"}) {
    PreparationHarness harness;
    harness.storage.addFile("/books/minimal.pdf", loadFixture(fixture), 1234, true);
    PdfPreparation preparation;
    ASSERT_TRUE(preparation.begin(harness.config()).ok());
    const PdfStepResult result = runToTerminal(preparation, harness);
    ASSERT_TRUE(result.complete()) << fixture << " " << static_cast<int>(result.status.error);

    const std::string generationRoot =
        std::string(preparation.cacheRoot()) + "/gen_" + std::to_string(preparation.generation());
    PdfTestByteSource outlineSource(harness.storage.bytes(generationRoot + "/outline.bin"));
    PdfOutlineHeader outlineHeader{};
    std::vector<PdfOutlineEntry> outline;
    const PdfOutlineEntryVisitor visitor{
        &outline,
        [](void* context, uint16_t, const PdfOutlineEntry& entry) {
          static_cast<std::vector<PdfOutlineEntry>*>(context)->push_back(entry);
          return PdfStatus::success();
        },
    };
    ASSERT_TRUE(pdfDecodeOutline(outlineSource.source(), &outlineHeader, visitor).ok());
    ASSERT_EQ(outline.size(), 1U) << fixture;
    EXPECT_STREQ(outline[0].title, "minimal");
    EXPECT_EQ(outline[0].sectionIndex, 0U);
    EXPECT_FALSE(harness.storage.exists(generationRoot + "/sections/000001.xhtml"));
  }
}

TEST(PdfPreparation, RejectsCyclicOutlineWithoutCommittingPartialGeneration) {
  PreparationHarness harness;
  harness.storage.addFile("/books/minimal.pdf", loadFixture("navigation_outline_cycle.pdf"), 1234, true);
  PdfPreparation preparation;
  ASSERT_TRUE(preparation.begin(harness.config()).ok());
  const PdfStepResult result = runToTerminal(preparation, harness);
  ASSERT_TRUE(result.failed());
  EXPECT_EQ(result.status.error, PdfError::Malformed);

  PdfCacheStore cache;
  ASSERT_TRUE(cache.initialize(harness.storage.io(), preparation.cacheRoot()).ok());
  PdfCacheManifestSelection selection;
  ASSERT_TRUE(cache.loadManifestSlots(preparation.sourceIdentity(), &selection).ok());
  EXPECT_FALSE(selection.selected);
}

TEST(PdfPreparation, CancelsAtASliceBoundaryClosesEverythingAndRestartsInAFreshGeneration) {
  PreparationHarness harness;
  harness.addFixture();
  uint32_t cancelledGeneration = 0;
  std::string cancelledGenerationRoot;
  PdfSourceIdentity cancelledSource{};

  {
    PdfPreparation preparation;
    ASSERT_TRUE(preparation.begin(harness.config()).ok());
    for (uint32_t step = 0; step < 10; ++step) {
      ASSERT_TRUE(preparation.step().yielded());
      ++harness.nowMs;
    }
    const PdfStepResult cancelled = cancelToTerminal(preparation, harness);
    ASSERT_TRUE(cancelled.failed());
    EXPECT_EQ(cancelled.status.error, PdfError::Cancelled);
    EXPECT_EQ(preparation.resourceCurrentBytes(), 0U);
    EXPECT_EQ(harness.storage.openHandleCount(), 0U);
    cancelledGeneration = preparation.generation();
    cancelledSource = preparation.sourceIdentity();
    cancelledGenerationRoot = std::string(preparation.cacheRoot()) + "/gen_" + std::to_string(cancelledGeneration);

    ASSERT_NE(cancelledGeneration, 0U);
    EXPECT_TRUE(harness.storage.exists(cancelledGenerationRoot));
    for (const std::string& path : harness.storage.paths()) {
      if (!path.starts_with(cancelledGenerationRoot + "/")) {
        continue;
      }
      const std::string relative = path.substr(cancelledGenerationRoot.size() + 1U);
      EXPECT_FALSE(relative.starts_with("build."));
      EXPECT_EQ(relative.find(".tmp"), std::string::npos);
    }

    PdfCacheStore cache;
    ASSERT_TRUE(cache.initialize(harness.storage.io(), preparation.cacheRoot()).ok());
    PdfBuildCheckpointSelection checkpoints{};
    ASSERT_TRUE(cache.loadCheckpointSlots(cancelledSource, &checkpoints).ok());
    ASSERT_TRUE(checkpoints.selected);
    EXPECT_TRUE(checkpoints.slots[static_cast<uint8_t>(checkpoints.selectedSlot)].valid);
    EXPECT_TRUE(checkpoints.slots[static_cast<uint8_t>(checkpoints.selectedSlot)].sourceMatches);
    EXPECT_EQ(checkpoints.checkpoint.phase, PdfBuildPhase::Cancelled);
    EXPECT_EQ(checkpoints.checkpoint.generation, cancelledGeneration);
  }

  PdfPreparation resumed;
  ASSERT_TRUE(resumed.begin(harness.config()).ok());
  const PdfStepResult result = runToTerminal(resumed, harness);
  ASSERT_TRUE(result.complete()) << static_cast<int>(result.status.error);
  EXPECT_FALSE(resumed.resumedFromCheckpoint());
  EXPECT_NE(resumed.generation(), cancelledGeneration);
  EXPECT_EQ(harness.storage.openHandleCount(), 0U);
}

TEST(PdfPreparation, CancellationAfterSectionCloseFinishesResumePointWithinSliceLimit) {
  PreparationHarness harness;
  harness.addFixture();

  PdfPreparation preparation;
  ASSERT_TRUE(preparation.begin(harness.config()).ok());
  for (uint32_t slice = 0; slice < 20000 && preparation.workCounters().sectionsEmitted == 0; ++slice) {
    const PdfStepResult step = preparation.step();
    ++harness.nowMs;
    ASSERT_TRUE(step.yielded()) << static_cast<int>(step.status.error) << "@" << step.status.offset;
  }

  ASSERT_EQ(preparation.workCounters().sectionsEmitted, 1U);
  ASSERT_EQ(preparation.phase(), PdfPreparationPhase::CommitResumePoint);

  const PdfStepResult cancelled = cancelToTerminal(preparation, harness);

  ASSERT_TRUE(cancelled.failed());
  EXPECT_EQ(cancelled.status.error, PdfError::Cancelled);
  EXPECT_EQ(preparation.phase(), PdfPreparationPhase::Cancelled);
  EXPECT_EQ(preparation.resourceCurrentBytes(), 0U);
  EXPECT_EQ(harness.storage.openHandleCount(), 0U);
}

TEST(PdfPreparation, ReportsCheckpointWriteFailureInsteadOfUserCancellation) {
  PreparationHarness harness;
  harness.addFixture();

  PdfPreparation preparation;
  ASSERT_TRUE(preparation.begin(harness.config()).ok());
  for (uint32_t step = 0; step < 10; ++step) {
    ASSERT_TRUE(preparation.step().yielded());
    ++harness.nowMs;
  }

  harness.storage.fail(PdfTestFaultPoint::Write);
  const PdfStepResult result = cancelToTerminal(preparation, harness);

  ASSERT_TRUE(result.failed());
  EXPECT_EQ(result.status.error, PdfError::IoFailure);
  EXPECT_EQ(preparation.phase(), PdfPreparationPhase::Failed);
  EXPECT_EQ(preparation.resourceCurrentBytes(), 0U);
  EXPECT_EQ(harness.storage.openHandleCount(), 0U);

  harness.storage.clearFault();
  PdfCacheStore cache;
  ASSERT_TRUE(cache.initialize(harness.storage.io(), preparation.cacheRoot()).ok());
  PdfBuildCheckpointSelection checkpoints{};
  ASSERT_TRUE(cache.loadCheckpointSlots(preparation.sourceIdentity(), &checkpoints).ok());
  EXPECT_FALSE(checkpoints.selected);
}

TEST(PdfPreparation, ResumeOutputWriteFailureClosesHandleRemovesPartialFileAndPreservesCheckpoint) {
  for (const PdfTestFaultPoint faultPoint :
       {PdfTestFaultPoint::Write, PdfTestFaultPoint::Sync, PdfTestFaultPoint::Close}) {
    for (const char* const failedLeaf : {"metadata.bin", "outline.bin"}) {
      SCOPED_TRACE(static_cast<int>(faultPoint));
      SCOPED_TRACE(failedLeaf);
    PreparationHarness harness;
    harness.addFixture();

    PdfPreparation preparation;
    ASSERT_TRUE(preparation.begin(harness.config()).ok());
    for (uint32_t slice = 0; slice < 20000 && preparation.phase() != PdfPreparationPhase::CloseSource; ++slice) {
      const PdfStepResult step = preparation.step();
      ++harness.nowMs;
      ASSERT_TRUE(step.yielded()) << static_cast<int>(step.status.error) << "@" << step.status.offset;
    }
    ASSERT_EQ(preparation.phase(), PdfPreparationPhase::CloseSource);

    std::vector<std::pair<std::string, std::vector<uint8_t>>> checkpointFiles;
    for (const std::string& path : harness.storage.paths()) {
      if (path == std::string(preparation.cacheRoot()) + "/build.a" ||
          path == std::string(preparation.cacheRoot()) + "/build.b") {
        checkpointFiles.emplace_back(path, harness.storage.bytes(path));
      }
    }
    ASSERT_FALSE(checkpointFiles.empty());

    preparation.requestCancel();
    bool targetOpen = false;
    for (uint32_t slice = 0; slice < 64 && !targetOpen; ++slice) {
      const PdfStepResult step = preparation.step();
      ++harness.nowMs;
      ASSERT_TRUE(step.yielded()) << static_cast<int>(step.status.error) << "@" << step.status.offset;
      const std::vector<std::string> handles = harness.storage.openHandlePaths();
      targetOpen = std::any_of(handles.begin(), handles.end(), [&](const std::string& path) {
        return path.ends_with(std::string("/") + failedLeaf);
      });
    }
    ASSERT_TRUE(targetOpen);

    harness.storage.fail(faultPoint);
    PdfStepResult result = PdfStepResult::paused();
    for (uint32_t slice = 0; slice < 64 && result.yielded(); ++slice) {
      result = preparation.step();
      ++harness.nowMs;
    }

    ASSERT_TRUE(result.failed());
    EXPECT_EQ(result.status.error, PdfError::IoFailure);
    EXPECT_EQ(preparation.phase(), PdfPreparationPhase::Failed);
    EXPECT_EQ(preparation.resourceCurrentBytes(), 0U);
    EXPECT_EQ(harness.storage.openHandleCount(), 0U);
    const std::string failedPath = std::string(preparation.cacheRoot()) + "/gen_" +
                                   std::to_string(preparation.generation()) + "/" + failedLeaf;
    EXPECT_FALSE(harness.storage.exists(failedPath));
    for (const auto& [path, bytes] : checkpointFiles) {
      ASSERT_TRUE(harness.storage.exists(path));
      EXPECT_EQ(harness.storage.bytes(path), bytes);
    }
    }
  }
}

TEST(PdfPreparation, RejectsLowHeapBeforeOpeningThePdfOrAllocatingWorkspaces) {
  PreparationHarness harness;
  harness.addFixture();
  harness.resources.freeHeap = PDF_MIN_FREE_HEAP_BYTES - 1;

  PdfPreparation preparation;
  ASSERT_TRUE(preparation.begin(harness.config()).ok());
  const PdfStepResult result = preparation.step();

  ASSERT_TRUE(result.failed());
  EXPECT_EQ(result.status.error, PdfError::InsufficientMemory);
  EXPECT_EQ(preparation.resourceCurrentBytes(), 0U);
  EXPECT_EQ(harness.storage.openCalls(), 0U);
}

TEST(PdfPreparation, RetainsPageResumeJournalBetweenCheckpoints) {
  constexpr char sourcePath[] = "/books/three-page-checkpoint-gate.pdf";
  const std::vector<uint8_t> fixture = makeTwoPageTextPdf(false, true);
  ASSERT_FALSE(fixture.empty());

  PreparationHarness harness;
  harness.storage.setMaximumReadHandles(1);
  harness.storage.addFile(sourcePath, fixture, 1234, true);
  PdfPreparation preparation;
  ASSERT_TRUE(preparation.begin(harness.config(sourcePath)).ok());

  const PdfStepResult result = runToTerminal(preparation, harness);

  ASSERT_TRUE(result.complete()) << static_cast<int>(result.status.error) << '@' << result.status.offset;
  const std::string journalPath = std::string(preparation.cacheRoot()) + "/gen_" +
                                  std::to_string(preparation.generation()) + "/resume.journal";
  EXPECT_EQ(std::count_if(harness.storage.openObservations().begin(), harness.storage.openObservations().end(),
                          [&](const PdfTestOpenObservation& observation) {
                            return observation.path == journalPath && observation.mode == PdfCacheOpenMode::Write;
                          }),
            3);
  EXPECT_EQ(harness.storage.openHandleCount(), 0U);
}

TEST(PdfPreparation, RecreatedInstanceResumesTheSameGenerationAfterOneVerifiedPage) {
  constexpr char sourcePath[] = "/books/two-page-resume.pdf";
  const std::vector<uint8_t> fixture = makeTwoPageTextPdf();
  ASSERT_FALSE(fixture.empty());

  PreparationHarness freshHarness;
  freshHarness.storage.setMaximumReadHandles(1);
  freshHarness.storage.addFile(sourcePath, fixture, 1234, true);
  PdfPreparation fresh;
  ASSERT_TRUE(fresh.begin(freshHarness.config(sourcePath)).ok());
  const PdfStepResult freshResult = runToTerminal(fresh, freshHarness);
  ASSERT_TRUE(freshResult.complete()) << static_cast<int>(freshResult.status.error) << "@" << freshResult.status.offset
                                      << " phase=" << static_cast<int>(fresh.phase());
  const std::string freshRoot = std::string(fresh.cacheRoot()) + "/gen_" + std::to_string(fresh.generation());
  const std::vector<uint8_t> freshSection = freshHarness.storage.bytes(freshRoot + "/sections/000000.xhtml");
  ASSERT_FALSE(freshHarness.storage.exists(freshRoot + "/sections/000001.xhtml"));
  const PdfPreparationWorkCounters freshWork = fresh.workCounters();

  PreparationHarness harness;
  harness.storage.setMaximumReadHandles(1);
  harness.storage.addFile(sourcePath, fixture, 1234, true);
  PdfPreparation interrupted;
  ASSERT_TRUE(interrupted.begin(harness.config(sourcePath)).ok());
  harness.storage.clearSyncObservations();
  for (uint32_t slice = 0; slice < 20000 && interrupted.durableResumePhase() != PdfBuildResumePhase::AfterPage;
       ++slice) {
    const PdfStepResult step = interrupted.step();
    ++harness.nowMs;
    ASSERT_TRUE(step.yielded()) << static_cast<int>(step.status.error) << "@" << step.status.offset;
  }
  ASSERT_EQ(interrupted.durableResumePhase(), PdfBuildResumePhase::AfterPage);
  ASSERT_EQ(interrupted.durableResumePage(), 1U);
  const uint32_t generation = interrupted.generation();
  const std::string generationRoot = std::string(interrupted.cacheRoot()) + "/gen_" + std::to_string(generation);
  const std::string journalPath = generationRoot + "/resume.journal";
  // One durable slot publish seals discovery; the second seals the verified
  // page. No extra checkpoint sync may occur inside either failure window.
  EXPECT_EQ(std::count_if(harness.storage.syncObservations().begin(), harness.storage.syncObservations().end(),
                          [&](const std::string& path) {
                            return path == std::string(interrupted.cacheRoot()) + "/build.a" ||
                                   path == std::string(interrupted.cacheRoot()) + "/build.b";
                          }),
            2);
  const std::string firstPath = generationRoot + "/sections/000000.xhtml";
  ASSERT_TRUE(harness.storage.exists(firstPath));
  const std::vector<uint8_t> firstBytes = harness.storage.bytes(firstPath);

  for (uint32_t slice = 0; slice < 20000 && interrupted.phase() != PdfPreparationPhase::EmitSection; ++slice) {
    const PdfStepResult step = interrupted.step();
    ++harness.nowMs;
    ASSERT_TRUE(step.yielded()) << static_cast<int>(step.status.error) << "@" << step.status.offset;
  }
  ASSERT_EQ(interrupted.phase(), PdfPreparationPhase::EmitSection);
  const PdfStepResult cancelled = cancelToTerminal(interrupted, harness);
  ASSERT_TRUE(cancelled.failed());
  ASSERT_EQ(cancelled.status.error, PdfError::Cancelled);

  PdfCacheStore cache;
  ASSERT_TRUE(cache.initialize(harness.storage.io(), interrupted.cacheRoot()).ok());
  PdfBuildCheckpointSelection checkpoint{};
  ASSERT_TRUE(cache.loadCheckpointSlots(interrupted.sourceIdentity(), &checkpoint).ok());
  ASSERT_TRUE(checkpoint.selected);
  EXPECT_EQ(checkpoint.checkpoint.resumePhase, PdfBuildResumePhase::AfterPage);
  EXPECT_EQ(checkpoint.checkpoint.lastVerifiedPage, 1U);
  EXPECT_GT(checkpoint.checkpoint.journalBytes, 0U);
  ASSERT_GT(checkpoint.checkpoint.journalBytes, 512U);
  const uint32_t discoveryBytes = checkpoint.checkpoint.journalBytes - 512U;
  ASSERT_TRUE(harness.storage.exists(journalPath));
  EXPECT_EQ(harness.storage.bytes(journalPath).size(), checkpoint.checkpoint.journalBytes);
  for (const char* oldLeaf : {"resume.a", "resume.b", "resume.sections"}) {
    EXPECT_FALSE(harness.storage.exists(generationRoot + "/" + oldLeaf));
  }
  const PreparationHarness cancelledBaseline = harness;

  harness.storage.clearOpenObservations();
  harness.storage.clearRemoveObservations();
  PdfPreparation resumed;
  ASSERT_TRUE(resumed.begin(harness.config(sourcePath)).ok());
  const PdfStepResult result = runToTerminal(resumed, harness);
  ASSERT_TRUE(result.complete()) << static_cast<int>(result.status.error) << "@" << result.status.offset;
  EXPECT_TRUE(resumed.resumedFromCheckpoint());
  EXPECT_EQ(resumed.resumedPhase(), PdfBuildResumePhase::AfterPage);
  EXPECT_EQ(resumed.generation(), generation);
  const std::vector<uint8_t>& resumedSection = harness.storage.bytes(firstPath);
  ASSERT_GE(resumedSection.size(), firstBytes.size());
  EXPECT_TRUE(std::equal(firstBytes.begin(), firstBytes.end(), resumedSection.begin()));
  EXPECT_EQ(resumedSection, freshSection);
  EXPECT_FALSE(harness.storage.exists(generationRoot + "/sections/000001.xhtml"));
  EXPECT_EQ(resumed.workCounters().xrefSteps, 0U);
  EXPECT_EQ(resumed.workCounters().pagesWalked, 0U);
  EXPECT_LT(resumed.workCounters().xrefSteps, freshWork.xrefSteps);
  EXPECT_LT(resumed.workCounters().pagesWalked, freshWork.pagesWalked);
  EXPECT_LT(resumed.workCounters().contentTokens, freshWork.contentTokens);
  EXPECT_LT(resumed.workCounters().sourceBytesRead, freshWork.sourceBytesRead);
  for (const PdfTestOpenObservation& observation : harness.storage.openObservations()) {
    EXPECT_FALSE(observation.path == firstPath && observation.mode == PdfCacheOpenMode::WriteTruncate);
  }
  EXPECT_EQ(
      std::count(harness.storage.removeObservations().begin(), harness.storage.removeObservations().end(), firstPath),
      0);
  EXPECT_LE(harness.storage.maximumWriteRequest(), 3U * 1024U);
  EXPECT_EQ(harness.storage.openHandleCount(), 0U);

  PreparationHarness tailed = cancelledBaseline;
  std::vector<uint8_t> journalWithUncommittedTail = tailed.storage.bytes(journalPath);
  journalWithUncommittedTail.resize(journalWithUncommittedTail.size() + 73U, 0xa5U);
  tailed.storage.addFile(journalPath, journalWithUncommittedTail);
  tailed.storage.clearOpenObservations();
  PdfPreparation tailResume;
  ASSERT_TRUE(tailResume.begin(tailed.config(sourcePath)).ok());
  const PdfStepResult tailResult = runToTerminal(tailResume, tailed);
  ASSERT_TRUE(tailResult.complete()) << static_cast<int>(tailResult.status.error) << "@" << tailResult.status.offset;
  EXPECT_TRUE(tailResume.resumedFromCheckpoint());
  EXPECT_EQ(tailResume.resumedPhase(), PdfBuildResumePhase::AfterPage);
  EXPECT_EQ(tailResume.generation(), generation);
  ASSERT_TRUE(tailed.storage.exists(firstPath));
  EXPECT_FALSE(tailed.storage.exists(generationRoot + "/sections/000001.xhtml"));
  ASSERT_TRUE(tailed.storage.exists(journalPath));
  const auto& tailedSection = tailed.storage.bytes(firstPath);
  ASSERT_GE(tailedSection.size(), firstBytes.size());
  EXPECT_TRUE(std::equal(firstBytes.begin(), firstBytes.end(), tailedSection.begin()));
  EXPECT_EQ(tailedSection, freshSection);
  EXPECT_EQ(tailed.storage.bytes(journalPath).size(), checkpoint.checkpoint.journalBytes + 512U);
  EXPECT_TRUE(std::any_of(tailed.storage.openObservations().begin(), tailed.storage.openObservations().end(),
                          [&](const PdfTestOpenObservation& observation) {
                            return observation.path == journalPath && observation.mode == PdfCacheOpenMode::Write;
                          }));
  EXPECT_TRUE(std::none_of(tailed.storage.openObservations().begin(), tailed.storage.openObservations().end(),
                           [&](const PdfTestOpenObservation& observation) {
                             return observation.path == journalPath &&
                                    observation.mode == PdfCacheOpenMode::WriteTruncate;
                           }));
  EXPECT_EQ(tailed.storage.openHandleCount(), 0U);

  for (const uint16_t rotation : {uint16_t{0}, uint16_t{90}, uint16_t{180}, uint16_t{270}}) {
    SCOPED_TRACE(rotation);
    PreparationHarness accepted = cancelledBaseline;
    std::vector<uint8_t> validRotationJournal = accepted.storage.bytes(journalPath);
    setDiscoveryPageRotationAndReseal(&validRotationJournal, rotation);
    accepted.storage.addFile(journalPath, validRotationJournal);
    PdfPreparation validRotationResume;
    ASSERT_TRUE(validRotationResume.begin(accepted.config(sourcePath)).ok());
    const PdfStepResult validRotationResult = runToTerminal(validRotationResume, accepted);
    ASSERT_TRUE(validRotationResult.complete())
        << static_cast<int>(validRotationResult.status.error) << "@" << validRotationResult.status.offset;
    EXPECT_TRUE(validRotationResume.resumedFromCheckpoint());
    EXPECT_EQ(validRotationResume.generation(), generation);
    EXPECT_EQ(accepted.storage.openHandleCount(), 0U);
  }

  enum class PageResumeMutation : uint8_t {
    DiscoveryRecordCrc,
    PageRecordCrc,
    InvalidRotation,
    TruncatedDiscovery,
    RewoundDiscoveryCursor,
    SourceIdentity,
  };
  for (const PageResumeMutation mutation :
       {PageResumeMutation::DiscoveryRecordCrc, PageResumeMutation::PageRecordCrc, PageResumeMutation::InvalidRotation,
        PageResumeMutation::TruncatedDiscovery, PageResumeMutation::RewoundDiscoveryCursor,
        PageResumeMutation::SourceIdentity}) {
    SCOPED_TRACE(static_cast<int>(mutation));
    PreparationHarness rejected = cancelledBaseline;
    if (mutation == PageResumeMutation::DiscoveryRecordCrc) {
      rejected.storage.corruptByte(journalPath, 200U, 0x01U);
    } else if (mutation == PageResumeMutation::PageRecordCrc) {
      rejected.storage.corruptByte(journalPath, discoveryBytes + 200U, 0x01U);
    } else if (mutation == PageResumeMutation::InvalidRotation) {
      std::vector<uint8_t> invalidRotationJournal = rejected.storage.bytes(journalPath);
      setDiscoveryPageRotationAndReseal(&invalidRotationJournal, 1U);
      rejected.storage.addFile(journalPath, invalidRotationJournal);
    } else if (mutation == PageResumeMutation::TruncatedDiscovery) {
      rejected.storage.truncateFile(journalPath, discoveryBytes - 1U);
    } else if (mutation == PageResumeMutation::RewoundDiscoveryCursor) {
      const std::string checkpointPath =
          std::string(interrupted.cacheRoot()) + (checkpoint.selectedSlot == PdfCacheSlot::A ? "/build.a" : "/build.b");
      std::vector<uint8_t> bytes = rejected.storage.bytes(checkpointPath);
      ASSERT_EQ(bytes.size(), 96U);
      const uint32_t rewoundCursor = discoveryBytes - 1U;
      bytes[21] = static_cast<uint8_t>(rewoundCursor);
      bytes[22] = static_cast<uint8_t>(rewoundCursor >> 8U);
      bytes[23] = static_cast<uint8_t>(rewoundCursor >> 16U);
      resealTrailingCrc(&bytes);
      rejected.storage.addFile(checkpointPath, bytes);
    } else {
      std::vector<uint8_t> mutatedSource = fixture;
      const char needle[] = "First durable page";
      const auto found =
          std::search(mutatedSource.begin(), mutatedSource.end(), std::begin(needle), std::end(needle) - 1);
      ASSERT_NE(found, mutatedSource.end());
      *found = static_cast<uint8_t>('f');
      rejected.storage.addFile(sourcePath, mutatedSource, 1234, true);
    }
    rejected.storage.clearOpenObservations();
    rejected.storage.clearRemoveObservations();
    PdfPreparation freshFallback;
    ASSERT_TRUE(freshFallback.begin(rejected.config(sourcePath)).ok());
    const PdfStepResult fallback = runToTerminal(freshFallback, rejected);
    ASSERT_TRUE(fallback.complete()) << static_cast<int>(fallback.status.error) << "@" << fallback.status.offset;
    EXPECT_FALSE(freshFallback.resumedFromCheckpoint());
    EXPECT_NE(freshFallback.generation(), generation);
    EXPECT_EQ(std::count(rejected.storage.removeObservations().begin(), rejected.storage.removeObservations().end(),
                         firstPath),
              0);
    EXPECT_EQ(std::count_if(rejected.storage.openObservations().begin(), rejected.storage.openObservations().end(),
                            [&](const PdfTestOpenObservation& observation) {
                              return observation.path == firstPath &&
                                     observation.mode == PdfCacheOpenMode::WriteTruncate;
                            }),
              0);
    EXPECT_EQ(rejected.storage.openHandleCount(), 0U);
  }
}

TEST(PdfPreparation, ResumesAfterAnUnreadablePageWithoutInventingJournalRecords) {
  constexpr char sourcePath[] = "/books/skipped-page-resume.pdf";
  const std::vector<uint8_t> fixture = makeTwoPageTextPdf(true, true);
  ASSERT_FALSE(fixture.empty());

  PreparationHarness harness;
  harness.storage.setMaximumReadHandles(1);
  harness.storage.addFile(sourcePath, fixture, 1234, true);
  uint32_t generation = 0;
  {
    PdfPreparation interrupted;
    ASSERT_TRUE(interrupted.begin(harness.config(sourcePath)).ok());
    for (uint32_t slice = 0; slice < 20000 && interrupted.durableResumePage() != 3U; ++slice) {
      const PdfStepResult step = interrupted.step();
      ++harness.nowMs;
      ASSERT_TRUE(step.yielded()) << static_cast<int>(step.status.error) << "@" << step.status.offset;
    }
    ASSERT_EQ(interrupted.durableResumePhase(), PdfBuildResumePhase::AfterPage);
    ASSERT_EQ(interrupted.durableResumePage(), 3U);
    generation = interrupted.generation();
  }

  PdfPreparation resumed;
  ASSERT_TRUE(resumed.begin(harness.config(sourcePath)).ok());
  const PdfStepResult result = runToTerminal(resumed, harness);
  ASSERT_TRUE(result.complete()) << static_cast<int>(result.status.error) << "@" << result.status.offset;
  EXPECT_TRUE(resumed.resumedFromCheckpoint());
  EXPECT_EQ(resumed.resumedPhase(), PdfBuildResumePhase::AfterPage);
  EXPECT_EQ(resumed.generation(), generation);
  EXPECT_EQ(resumed.totalWords(), 6U);
}

TEST(PdfPreparationPaintGate, RequiresBothProgressAndElapsedTimeAndCapsIntermediatePaints) {
  PdfPreparationPaintGate gate;

  EXPECT_FALSE(gate.shouldPaint(10, 14999));
  EXPECT_TRUE(gate.shouldPaint(10, 15000));
  EXPECT_FALSE(gate.shouldPaint(20, 29999));
  EXPECT_TRUE(gate.shouldPaint(20, 30000));

  for (uint8_t paint = 3; paint <= 10; ++paint) {
    EXPECT_TRUE(gate.shouldPaint(static_cast<uint8_t>(paint * 10), static_cast<uint32_t>(paint) * 15000U));
  }
  EXPECT_EQ(gate.intermediatePaintCount(), 10U);
  EXPECT_FALSE(gate.shouldPaint(100, 165000));
}

}  // namespace
