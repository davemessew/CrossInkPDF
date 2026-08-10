#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
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
#include "PdfCacheFormat.h"
#include "PdfTestCacheIo.h"

struct PdfPreparationTestAccess {
  struct LiveNavigationShape {
    uint16_t snapshotBytes = 0;
    uint16_t pageWindowBytes = 0;
    uint16_t linkCount = 0;
    uint8_t xObjectCount = 0;
    uint8_t pageLabelCount = 0;
    uint8_t imageCacheEntryCount = 0;
    uint8_t imageCandidateCount = 0;
  };

  static uint8_t snapshotStage(const PdfPreparation& preparation) {
    return static_cast<uint8_t>(preparation.inlineNavigationSpillStage_);
  }

  static uint32_t snapshotOffset(const PdfPreparation& preparation) {
    return preparation.inlineNavigationSpoolOffset_;
  }

  static uint8_t* dictionary(PdfPreparation& preparation) { return preparation.dictionary_.get(); }
  static uint8_t* runRecords(PdfPreparation& preparation) { return preparation.runRecords_.get(); }
  static uint8_t* operandScratch(PdfPreparation& preparation) { return preparation.operandScratch_.get(); }
  static bool placementActive(const PdfPreparation& preparation) { return preparation.placement_ != nullptr; }
  static size_t compactSnapshotBytes(const PdfPreparation& preparation) {
    return preparation.fontNavigationSnapshotBytes_;
  }
  static LiveNavigationShape liveNavigationShape(const PdfPreparation& preparation) {
    return {
        preparation.fontNavigationSnapshotBytes_,
        preparation.fontNavigationPageWindowBytes_,
        preparation.fontNavigationLinkCount_,
        preparation.fontNavigationXObjectCount_,
        preparation.fontNavigationPageLabelCount_,
        preparation.fontNavigationImageCacheEntryCount_,
        preparation.fontNavigationImageCandidateCount_,
    };
  }
  static std::vector<uint8_t> liveNavigationBytes(PdfPreparation& preparation,
                                                  const LiveNavigationShape shape) {
    std::vector<uint8_t> bytes;
    if (preparation.navigation_ == nullptr || shape.snapshotBytes == 0) {
      return bytes;
    }
    const LiveNavigationShape saved = liveNavigationShape(preparation);
    preparation.fontNavigationSnapshotBytes_ = shape.snapshotBytes;
    preparation.fontNavigationPageWindowBytes_ = shape.pageWindowBytes;
    preparation.fontNavigationLinkCount_ = shape.linkCount;
    preparation.fontNavigationXObjectCount_ = shape.xObjectCount;
    preparation.fontNavigationPageLabelCount_ = shape.pageLabelCount;
    preparation.fontNavigationImageCacheEntryCount_ = shape.imageCacheEntryCount;
    preparation.fontNavigationImageCandidateCount_ = shape.imageCandidateCount;
    bytes.reserve(shape.snapshotBytes);
    size_t offset = 0;
    while (offset < shape.snapshotBytes) {
      size_t contiguous = 0;
      uint8_t* const source = preparation.fontNavigationSnapshotFieldBytes(
          preparation.navigation_, offset, &contiguous);
      if (source == nullptr || contiguous == 0 || contiguous > shape.snapshotBytes - offset) {
        bytes.clear();
        break;
      }
      bytes.insert(bytes.end(), source, source + contiguous);
      offset += contiguous;
    }
    preparation.fontNavigationSnapshotBytes_ = saved.snapshotBytes;
    preparation.fontNavigationPageWindowBytes_ = saved.pageWindowBytes;
    preparation.fontNavigationLinkCount_ = saved.linkCount;
    preparation.fontNavigationXObjectCount_ = saved.xObjectCount;
    preparation.fontNavigationPageLabelCount_ = saved.pageLabelCount;
    preparation.fontNavigationImageCacheEntryCount_ = saved.imageCacheEntryCount;
    preparation.fontNavigationImageCandidateCount_ = saved.imageCandidateCount;
    return bytes;
  }
  static bool runtimeConstructed(const PdfPreparation& preparation) {
    return preparation.preparedContentRuntimeConstructed();
  }
  static uint32_t expandedRequiredBytes(const PdfPreparation& preparation) {
    return preparation.expandedRequiredBytes_;
  }
  static void setExpandedRequiredBytes(PdfPreparation& preparation, const uint32_t bytes) {
    preparation.expandedRequiredBytes_ = bytes;
  }
  static bool rasterRuntimeActive(const PdfPreparation& preparation) {
    return preparation.rasterRuntimeActive_;
  }
  static bool sectionRepairRuntimeActive(const PdfPreparation& preparation) {
    return preparation.sectionRepairRuntimeActive_;
  }
  static uint64_t failedRasterImages(const PdfPreparation& preparation) {
    return preparation.failedRasterImages_;
  }

  static size_t navigationBytes(PdfPreparation& preparation) {
    size_t offset = 0;
    for (uint8_t span = 0; span < 3; ++span) {
      size_t contiguous = 0;
      if (preparation.preparedNavigationSpillBytes(offset, &contiguous) == nullptr) {
        return contiguous == 0 ? offset : 0;
      }
      if (contiguous == 0) {
        return 0;
      }
      offset += contiguous;
    }
    size_t endBytes = 1;
    return preparation.preparedNavigationSpillBytes(offset, &endBytes) == nullptr && endBytes == 0 ? offset : 0;
  }

  static size_t runtimePrefixBytes(PdfPreparation& preparation) {
    size_t contiguous = 0;
    uint8_t* const tail =
        preparation.preparedNavigationSpillBytes(PdfLimits::PageTextBytes, &contiguous);
    return tail == nullptr ? 0 : static_cast<size_t>(tail - preparation.runRecords_.get());
  }

  static size_t placementPrefixBytes(PdfPreparation& preparation) {
    size_t runTailBytes = 0;
    if (preparation.preparedNavigationSpillBytes(PdfLimits::PageTextBytes, &runTailBytes) == nullptr) {
      return 0;
    }
    size_t operandBytes = 0;
    uint8_t* const operand = preparation.preparedNavigationSpillBytes(
        PdfLimits::PageTextBytes + runTailBytes, &operandBytes);
    return operand == nullptr ? 0 : static_cast<size_t>(operand - preparation.operandScratch_.get());
  }
};

namespace {

constexpr uint32_t kAdlerModulus = 65521U;

std::vector<uint8_t> loadFixture(const char* const name) {
  const std::filesystem::path path = std::filesystem::path(__FILE__).parent_path().parent_path() /
                                     "pdf_reflow_core" / "fixtures" / name;
  std::ifstream input(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
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

std::string streamObject(const std::string& bytes, const char* const filter = nullptr) {
  std::string object = "<< /Length " + std::to_string(bytes.size());
  if (filter != nullptr) {
    object += " /Filter /";
    object += filter;
  }
  object += " >>\nstream\n";
  object.append(bytes);
  object += "\nendstream";
  return object;
}

std::string streamObjectWithDictionary(const std::string& bytes, const std::string& dictionary) {
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

std::string crlfStreamObject(const std::string& bytes) {
  std::string object = "<< /Length " + std::to_string(bytes.size()) + " >>\r\nstream\r\n";
  object.append(bytes);
  object += "\r\nendstream";
  return object;
}

std::string overlongStreamObject(const std::string& painted, const std::string& decoy) {
  static constexpr std::string_view falseBoundary = "\nendstream\n";
  const size_t declaredLength = painted.size() + falseBoundary.size() + decoy.size();
  std::string object = "<< /Length " + std::to_string(declaredLength) + " >>\nstream\n";
  object.append(painted);
  object.append(falseBoundary);
  object.append(decoy);
  return object;
}

std::vector<uint8_t> onePagePdf(const std::string& contents,
                                const std::vector<std::string>& contentObjects) {
  const uint32_t fontObject = static_cast<uint32_t>(4U + contentObjects.size());
  std::vector<std::string> objects{
      "<< /Type /Catalog /Pages 2 0 R >>",
      "<< /Type /Pages /Count 1 /Kids [3 0 R] >>",
      "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] /Resources << /Font << /F1 " +
          std::to_string(fontObject) + " 0 R >> >> /Contents " + contents + " >>",
  };
  objects.insert(objects.end(), contentObjects.begin(), contentObjects.end());
  objects.emplace_back("<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>");

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

struct TwoPageObjectStreamFixture {
  std::vector<uint8_t> pdf;
  uint32_t objectStreamDecodedBytes = 0;
  uint32_t contentBytes = 0;
  uint32_t xrefDecodedBytes = 0;
};

void appendXrefEntry(std::string* const output, const uint8_t type, const uint32_t field2,
                     const uint16_t field3) {
  ASSERT_NE(output, nullptr);
  output->push_back(static_cast<char>(type));
  output->push_back(static_cast<char>(field2 >> 24U));
  output->push_back(static_cast<char>(field2 >> 16U));
  output->push_back(static_cast<char>(field2 >> 8U));
  output->push_back(static_cast<char>(field2));
  output->push_back(static_cast<char>(field3 >> 8U));
  output->push_back(static_cast<char>(field3));
}

TwoPageObjectStreamFixture twoPageObjectStreamPdf() {
  static constexpr std::string_view firstContent =
      "BT /F1 12 Tf 72 720 Td (First object stream page.) Tj ET";
  static constexpr std::string_view secondContent =
      "BT /F1 12 Tf 72 720 Td (Second object stream page.) Tj ET";
  const std::array<std::pair<uint32_t, std::string>, 4> compressedObjects{{
      {1, "<< /Type /Catalog /Pages 2 0 R >>"},
      {2, "<< /Type /Pages /Kids [3 0 R 4 0 R] /Count 2 >>"},
      {3, "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] "
          "/Resources << /Font << /F1 7 0 R >> >> /Contents 5 0 R >>"},
      {4, "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] "
          "/Resources << /Font << /F1 7 0 R >> >> /Contents 6 0 R >>"},
  }};

  std::string objectData;
  std::string index;
  for (const auto& [number, body] : compressedObjects) {
    index += std::to_string(number) + " " + std::to_string(objectData.size()) + " ";
    objectData.append(body);
    objectData.push_back(' ');
  }
  const std::string decodedObjectStream = index + objectData;
  const std::string encodedObjectStream = zlibStored(decodedObjectStream);
  const std::string objectStream = streamObjectWithDictionary(
      encodedObjectStream, "/Type /ObjStm /N 4 /First " + std::to_string(index.size()) + " /Filter /FlateDecode");

  std::string pdf = "%PDF-1.7\n";
  std::array<uint32_t, 10> offsets{};
  const auto appendObject = [&pdf, &offsets](const uint32_t number, const std::string& body) {
    offsets[number] = static_cast<uint32_t>(pdf.size());
    pdf += std::to_string(number) + " 0 obj\n" + body + "\nendobj\n";
  };
  appendObject(5, streamObject(std::string(firstContent)));
  appendObject(6, streamObject(std::string(secondContent)));
  appendObject(7, "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>");
  appendObject(8, objectStream);
  const uint32_t xrefOffset = static_cast<uint32_t>(pdf.size());

  std::string xrefData;
  appendXrefEntry(&xrefData, 0, 0, UINT16_MAX);
  for (uint32_t object = 1; object <= 4; ++object) {
    appendXrefEntry(&xrefData, 2, 8, static_cast<uint16_t>(object - 1U));
  }
  for (uint32_t object = 5; object <= 8; ++object) {
    appendXrefEntry(&xrefData, 1, offsets[object], 0);
  }
  appendXrefEntry(&xrefData, 1, xrefOffset, 0);
  const std::string encodedXref = zlibStored(xrefData);
  appendObject(9, streamObjectWithDictionary(
                      encodedXref,
                      "/Type /XRef /Size 10 /Root 1 0 R /W [1 4 2] /Index [0 10] /Filter /FlateDecode"));
  pdf += "startxref\n" + std::to_string(xrefOffset) + "\n%%EOF\n";

  return {{pdf.begin(), pdf.end()}, static_cast<uint32_t>(decodedObjectStream.size()),
          static_cast<uint32_t>(firstContent.size() + secondContent.size()),
          static_cast<uint32_t>(xrefData.size())};
}

std::vector<uint8_t> twoPagePdf(const std::string& firstContentObject,
                                const std::string& secondContentObject) {
  const std::vector<std::string> objects{
      "<< /Type /Catalog /Pages 2 0 R >>",
      "<< /Type /Pages /Count 2 /Kids [3 0 R 5 0 R] >>",
      "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] /Resources << /Font << /F1 7 0 R >> >> /Contents 4 0 R >>",
      firstContentObject,
      "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] /Resources << /Font << /F1 7 0 R >> >> /Contents 6 0 R >>",
      secondContentObject,
      "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>",
  };
  std::string pdf = "%PDF-1.4\n";
  std::vector<size_t> offsets;
  offsets.reserve(objects.size());
  for (size_t index = 0; index < objects.size(); ++index) {
    offsets.push_back(pdf.size());
    pdf += std::to_string(index + 1U) + " 0 obj\n" + objects[index] + "\nendobj\n";
  }
  const size_t xrefOffset = pdf.size();
  pdf += "xref\n0 8\n0000000000 65535 f \n";
  char entry[32]{};
  for (const size_t offset : offsets) {
    const int length = std::snprintf(entry, sizeof(entry), "%010zu 00000 n \n", offset);
    if (length <= 0 || static_cast<size_t>(length) >= sizeof(entry)) {
      return {};
    }
    pdf.append(entry, static_cast<size_t>(length));
  }
  pdf += "trailer\n<< /Size 8 /Root 1 0 R >>\nstartxref\n" + std::to_string(xrefOffset) + "\n%%EOF\n";
  return {pdf.begin(), pdf.end()};
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

PdfStepResult runToTerminal(PdfPreparation& preparation, PreparationHarness& harness) {
  for (uint32_t step = 0; step < 20000; ++step) {
    const uint32_t operationsBefore = harness.storage.operationCalls();
    const uint64_t bytesReadBefore = harness.storage.bytesReadTotal();
    const uint64_t bytesWrittenBefore = harness.storage.bytesWrittenTotal();
    const PdfStepResult result = preparation.step();
    EXPECT_LE(harness.storage.operationCalls() - operationsBefore, 32U) << "step=" << step;
    EXPECT_LE(harness.storage.bytesReadTotal() - bytesReadBefore, PdfLimits::InterpreterSourceBufferBytes)
        << "step=" << step;
    EXPECT_LE(harness.storage.bytesWrittenTotal() - bytesWrittenBefore, PdfLimits::InterpreterSourceBufferBytes)
        << "step=" << step;
    ++harness.nowMs;
    if (!result.yielded()) {
      return result;
    }
  }
  return PdfStepResult::failure(PdfStatus::failure(PdfError::BudgetExhausted));
}

PdfStepResult stepWithPublicBounds(PdfPreparation& preparation, PreparationHarness& harness,
                                   const uint32_t step) {
  const uint32_t operationsBefore = harness.storage.operationCalls();
  const uint64_t bytesReadBefore = harness.storage.bytesReadTotal();
  const uint64_t bytesWrittenBefore = harness.storage.bytesWrittenTotal();
  const auto startedAt = std::chrono::steady_clock::now();
  const PdfStepResult result = preparation.step();
  const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - startedAt);
  EXPECT_LE(elapsed.count(), 8) << "step=" << step;
  EXPECT_LE(harness.storage.operationCalls() - operationsBefore, 32U) << "step=" << step;
  EXPECT_LE(harness.storage.bytesReadTotal() - bytesReadBefore, PdfLimits::InterpreterSourceBufferBytes)
      << "step=" << step;
  EXPECT_LE(harness.storage.bytesWrittenTotal() - bytesWrittenBefore, PdfLimits::InterpreterSourceBufferBytes)
      << "step=" << step;
  ++harness.nowMs;
  return result;
}

bool advanceToPhase(PdfPreparation& preparation, PreparationHarness& harness,
                    const PdfPreparationPhase phase, uint32_t* const step) {
  while (preparation.phase() != phase && *step < 20000U) {
    const PdfStepResult result = stepWithPublicBounds(preparation, harness, (*step)++);
    if (!result.yielded()) {
      return false;
    }
  }
  return preparation.phase() == phase;
}

PdfStepResult cancelToTerminalWithPublicBounds(PdfPreparation& preparation, PreparationHarness& harness,
                                               uint32_t* const step) {
  preparation.requestCancel();
  for (uint32_t cancelStep = 0; cancelStep < 1024U; ++cancelStep) {
    const PdfStepResult result = stepWithPublicBounds(preparation, harness, (*step)++);
    if (!result.yielded()) {
      return result;
    }
  }
  return PdfStepResult::failure(PdfStatus::failure(PdfError::BudgetExhausted));
}

std::vector<uint8_t> copyBytes(const uint8_t* const bytes, const size_t count) {
  return bytes == nullptr ? std::vector<uint8_t>{} : std::vector<uint8_t>(bytes, bytes + count);
}

void writeLe32(std::vector<uint8_t>* const bytes, const size_t offset, const uint32_t value) {
  ASSERT_NE(bytes, nullptr);
  ASSERT_LE(offset + sizeof(value), bytes->size());
  for (uint8_t index = 0; index < sizeof(value); ++index) {
    (*bytes)[offset + index] = static_cast<uint8_t>(value >> (index * 8U));
  }
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

std::string contentStorePath(const PdfPreparation& preparation) {
  return std::string(preparation.cacheRoot()) + "/gen_" + std::to_string(preparation.generation()) +
         "/build.content";
}

std::string objectStorePath(const PdfPreparation& preparation) {
  return std::string(preparation.cacheRoot()) + "/gen_" + std::to_string(preparation.generation()) +
         "/build.objstm";
}

std::vector<PdfCacheOpenMode> openModesForPath(const PdfTestCacheIo& storage, const std::string& path) {
  std::vector<PdfCacheOpenMode> modes;
  for (const PdfTestOpenObservation& observation : storage.openObservations()) {
    if (observation.path == path) {
      modes.push_back(observation.mode);
    }
  }
  return modes;
}

TEST(PdfPreparationContentStreams, AccountsAndRetiresObjectStreamStoreBeforeContentDecode) {
  const std::vector<uint8_t> pdf = loadFixture("xref_stream_objstm.pdf");
  ASSERT_FALSE(pdf.empty());

  constexpr char sourcePath[] = "/books/object-stream-content-handoff.pdf";
  PreparationHarness harness;
  harness.storage.setMaximumReadHandles(1);
  harness.storage.addFile(sourcePath, pdf, 1234, true);
  PdfPreparation preparation;
  ASSERT_TRUE(preparation.begin(harness.config(sourcePath)).ok());

  const PdfStepResult result = runToTerminal(preparation, harness);

  ASSERT_TRUE(result.complete()) << static_cast<int>(result.status.error) << "@" << result.status.offset;
  const std::string storePath = objectStorePath(preparation);
  const std::vector<PdfCacheOpenMode> modes = openModesForPath(harness.storage, storePath);
  EXPECT_EQ(std::count(modes.begin(), modes.end(), PdfCacheOpenMode::WriteTruncate), 1);
  EXPECT_EQ(std::count(harness.storage.removeObservations().begin(), harness.storage.removeObservations().end(),
                       storePath),
            1);
  EXPECT_EQ(PdfPreparationTestAccess::expandedRequiredBytes(preparation), 56U + 61U);
  EXPECT_FALSE(harness.storage.exists(storePath));
  EXPECT_EQ(harness.storage.openHandleCount(), 0U);
}

TEST(PdfPreparationContentStreams, ReusesCachedObjectStreamWhenALaterPageNeedsIt) {
  const TwoPageObjectStreamFixture fixture = twoPageObjectStreamPdf();
  ASSERT_FALSE(fixture.pdf.empty());

  constexpr char sourcePath[] = "/books/object-stream-two-page-handoff.pdf";
  PreparationHarness harness;
  harness.storage.setMaximumReadHandles(1);
  harness.storage.addFile(sourcePath, fixture.pdf, 1234, true);
  PdfPreparation preparation;
  ASSERT_TRUE(preparation.begin(harness.config(sourcePath)).ok());

  const PdfStepResult result = runToTerminal(preparation, harness);

  ASSERT_TRUE(result.complete()) << static_cast<int>(result.status.error) << "@" << result.status.offset;
  const std::string storePath = objectStorePath(preparation);
  const std::vector<PdfCacheOpenMode> modes = openModesForPath(harness.storage, storePath);
  EXPECT_EQ(std::count(modes.begin(), modes.end(), PdfCacheOpenMode::WriteTruncate), 1);
  EXPECT_EQ(std::count(harness.storage.removeObservations().begin(), harness.storage.removeObservations().end(),
                       storePath),
             1);
  EXPECT_EQ(PdfPreparationTestAccess::expandedRequiredBytes(preparation),
            fixture.xrefDecodedBytes + fixture.contentBytes);
  EXPECT_FALSE(harness.storage.exists(storePath));
  EXPECT_EQ(harness.storage.openHandleCount(), 0U);
}

TEST(PdfPreparationContentStreams, DecodesFlateContentBeforeWritingSemanticXhtml) {
  static constexpr std::array<uint8_t, 61> compressed{
      0x78, 0xda, 0x73, 0x0a, 0x51, 0xd0, 0x77, 0x33, 0x54, 0x30, 0x34, 0x52, 0x08, 0x49, 0x53, 0x30,
      0x37, 0x02, 0x22, 0x03, 0x85, 0x90, 0x14, 0x05, 0x0d, 0xe7, 0xfc, 0xdc, 0x82, 0xa2, 0xd4, 0xe2,
      0xe2, 0xd4, 0x14, 0x85, 0xe2, 0xd4, 0xdc, 0xc4, 0xbc, 0x92, 0xcc, 0x64, 0x85, 0x92, 0xd4, 0x8a,
      0x12, 0x4d, 0x85, 0x90, 0x2c, 0x05, 0xd7, 0x10, 0x00, 0x9c, 0xf3, 0x10, 0x4a,
  };
  const std::string encoded(reinterpret_cast<const char*>(compressed.data()), compressed.size());
  const std::vector<uint8_t> pdf = onePagePdf("4 0 R", {streamObject(encoded, "FlateDecode")});
  ASSERT_FALSE(pdf.empty());

  constexpr char sourcePath[] = "/books/flate-content.pdf";
  PreparationHarness harness;
  harness.storage.setMaximumReadHandles(1);
  harness.storage.addFile(sourcePath, pdf, 1234, true);
  PdfPreparation preparation;
  ASSERT_TRUE(preparation.begin(harness.config(sourcePath)).ok());

  const PdfStepResult result = runToTerminal(preparation, harness);

  ASSERT_TRUE(result.complete()) << "error=" << static_cast<int>(result.status.error)
                                 << " offset=" << result.status.offset;
  EXPECT_EQ(preparation.totalWords(), 3U);
  const std::string section = preparedSection(preparation, harness);
  EXPECT_NE(section.find(">Compressed semantic text</p>"), std::string::npos) << section;

  const std::string storePath = contentStorePath(preparation);
  EXPECT_EQ(openModesForPath(harness.storage, storePath),
            (std::vector<PdfCacheOpenMode>{PdfCacheOpenMode::WriteTruncate, PdfCacheOpenMode::Read}));
  const auto& events = harness.storage.events();
  const auto writerOpen = std::find(events.begin(), events.end(), "open:" + storePath);
  ASSERT_NE(writerOpen, events.end());
  const auto writerClose = std::find(writerOpen + 1, events.end(), "close:" + storePath);
  ASSERT_NE(writerClose, events.end());
  const auto sourceClose = std::find(writerClose + 1, events.end(), "close:" + std::string(sourcePath));
  ASSERT_NE(sourceClose, events.end());
  const auto readerOpen = std::find(sourceClose + 1, events.end(), "open:" + storePath);
  ASSERT_NE(readerOpen, events.end());
  EXPECT_EQ(std::find_if(events.begin(), events.end(), [](const std::string& event) {
              return event.find("build.content-nav") != std::string::npos;
            }),
            events.end());
}

TEST(PdfPreparationContentStreams, ConcatenatesContentsArrayInDeclaredOrder) {
  const std::string first = "BT /F1 12 Tf 72 720 Td (First ordered) Tj ET";
  const std::string second = "BT /F1 12 Tf 72 690 Td (Second ordered) Tj ET";
  const std::vector<uint8_t> pdf =
      onePagePdf("[4 0 R 5 0 R]", {streamObject(first), streamObject(second)});
  ASSERT_FALSE(pdf.empty());

  constexpr char sourcePath[] = "/books/contents-array.pdf";
  PreparationHarness harness;
  harness.storage.setMaximumReadHandles(1);
  harness.storage.addFile(sourcePath, pdf, 1234, true);
  PdfPreparation preparation;
  ASSERT_TRUE(preparation.begin(harness.config(sourcePath)).ok());

  const PdfStepResult result = runToTerminal(preparation, harness);

  ASSERT_TRUE(result.complete()) << "error=" << static_cast<int>(result.status.error)
                                 << " offset=" << result.status.offset;
  EXPECT_EQ(preparation.totalWords(), 4U);
  const std::string section = preparedSection(preparation, harness);
  const size_t firstOffset = section.find("First ordered");
  const size_t secondOffset = section.find("Second ordered");
  ASSERT_NE(firstOffset, std::string::npos) << section;
  ASSERT_NE(secondOffset, std::string::npos) << section;
  EXPECT_LT(firstOffset, secondOffset) << section;
  EXPECT_TRUE(openModesForPath(harness.storage, contentStorePath(preparation)).empty());
}

TEST(PdfPreparationContentStreams, BoundsMultiChunkFlateDecodeAndKeepsOnePdfReader) {
  std::string content(10U * 1024U, ' ');
  content += "BT /F1 12 Tf 72 720 Td (Chunked decode) Tj ET";
  const std::string encoded = zlibStored(content);
  ASSERT_FALSE(encoded.empty());
  const std::vector<uint8_t> pdf = onePagePdf("4 0 R", {streamObject(encoded, "FlateDecode")});
  ASSERT_FALSE(pdf.empty());

  constexpr char sourcePath[] = "/books/chunked-flate-content.pdf";
  PreparationHarness harness;
  harness.storage.setMaximumReadHandles(1);
  harness.storage.addFile(sourcePath, pdf, 1234, true);
  PdfPreparation preparation;
  ASSERT_TRUE(preparation.begin(harness.config(sourcePath)).ok());

  const PdfStepResult result = runToTerminal(preparation, harness);

  ASSERT_TRUE(result.complete()) << "error=" << static_cast<int>(result.status.error)
                                 << " offset=" << result.status.offset;
  EXPECT_EQ(preparation.totalWords(), 2U);
  EXPECT_NE(preparedSection(preparation, harness).find("Chunked decode"), std::string::npos);
  EXPECT_LE(harness.storage.maximumReadRequest(), PdfLimits::InterpreterSourceBufferBytes);
  EXPECT_LE(harness.storage.maximumWriteRequest(), PdfLimits::InterpreterSourceBufferBytes);
  EXPECT_EQ(openModesForPath(harness.storage, contentStorePath(preparation)),
            (std::vector<PdfCacheOpenMode>{PdfCacheOpenMode::WriteTruncate, PdfCacheOpenMode::Read}));
  EXPECT_EQ(harness.storage.openHandleCount(), 0U);
}

TEST(PdfPreparationContentStreams, EnforcesExactDocumentCapForRawAndFilteredStreams) {
  const std::string content = "BT /F1 12 Tf 72 720 Td (Exact document cap) Tj ET";
  for (const bool filtered : {false, true}) {
    for (const bool oneByteOver : {false, true}) {
      SCOPED_TRACE(::testing::Message() << "filtered=" << filtered << " over=" << oneByteOver);
      const std::string encoded = filtered ? zlibStored(content) : content;
      const std::vector<uint8_t> pdf = onePagePdf(
          "4 0 R", {streamObject(encoded, filtered ? "FlateDecode" : nullptr)});
      ASSERT_FALSE(pdf.empty());

      constexpr char sourcePath[] = "/books/document-cap.pdf";
      PreparationHarness harness;
      harness.storage.setMaximumReadHandles(1);
      harness.storage.addFile(sourcePath, pdf, 1234, true);
      PdfPreparation preparation;
      ASSERT_TRUE(preparation.begin(harness.config(sourcePath)).ok());
      const uint32_t baseline = static_cast<uint32_t>(PdfLimits::MaxExpandedRequiredStreamBytes - content.size() +
                                                      (oneByteOver ? 1U : 0U));
      PdfPreparationTestAccess::setExpandedRequiredBytes(preparation, baseline);

      const PdfStepResult result = runToTerminal(preparation, harness);

      if (oneByteOver) {
        EXPECT_TRUE(result.failed());
        EXPECT_TRUE(result.status.error == PdfError::LimitExceeded ||
                    result.status.error == PdfError::InsufficientStorage);
      } else {
        ASSERT_TRUE(result.complete()) << static_cast<int>(result.status.error) << "@" << result.status.offset;
        EXPECT_EQ(PdfPreparationTestAccess::expandedRequiredBytes(preparation),
                  PdfLimits::MaxExpandedRequiredStreamBytes);
      }
      EXPECT_EQ(harness.storage.openHandleCount(), 0U);
    }
  }
}

TEST(PdfPreparationContentStreams, AggregatesRawAndFilteredStreamsAcrossPages) {
  const std::string raw = "BT /F1 12 Tf 72 720 Td (Raw first page) Tj ET";
  const std::string decoded = "BT /F1 12 Tf 72 720 Td (Filtered second page) Tj ET";
  const std::vector<uint8_t> pdf = twoPagePdf(streamObject(raw), streamObject(zlibStored(decoded), "FlateDecode"));
  ASSERT_FALSE(pdf.empty());

  constexpr char sourcePath[] = "/books/raw-filtered-aggregate.pdf";
  PreparationHarness harness;
  harness.storage.setMaximumReadHandles(1);
  harness.storage.addFile(sourcePath, pdf, 1234, true);
  PdfPreparation preparation;
  ASSERT_TRUE(preparation.begin(harness.config(sourcePath)).ok());

  const PdfStepResult result = runToTerminal(preparation, harness);

  ASSERT_TRUE(result.complete()) << static_cast<int>(result.status.error) << "@" << result.status.offset;
  EXPECT_EQ(PdfPreparationTestAccess::expandedRequiredBytes(preparation), raw.size() + decoded.size());
  EXPECT_EQ(harness.storage.capacityCalls(), 2U);
  EXPECT_EQ(harness.storage.openHandleCount(), 0U);
}

TEST(PdfPreparationContentStreams, RestoresV2DocumentBudgetAndRejectsV1ResumeRecord) {
  const std::string first = "BT /F1 12 Tf 72 720 Td (Durable raw page) Tj ET";
  const std::string second = "BT /F1 12 Tf 72 720 Td (Resumed filtered page) Tj ET";
  const std::vector<uint8_t> pdf =
      twoPagePdf(streamObject(first), streamObject(zlibStored(second), "FlateDecode"));
  ASSERT_FALSE(pdf.empty());

  constexpr char sourcePath[] = "/books/document-budget-resume.pdf";
  PreparationHarness harness;
  harness.storage.setMaximumReadHandles(1);
  harness.storage.addFile(sourcePath, pdf, 1234, true);
  PdfPreparation interrupted;
  ASSERT_TRUE(interrupted.begin(harness.config(sourcePath)).ok());
  uint32_t step = 0;
  while (interrupted.durableResumePhase() != PdfBuildResumePhase::AfterPage && step < 20000U) {
    ASSERT_TRUE(stepWithPublicBounds(interrupted, harness, step++).yielded());
  }
  ASSERT_EQ(interrupted.durableResumePhase(), PdfBuildResumePhase::AfterPage);
  ASSERT_EQ(interrupted.durableResumePage(), 1U);
  EXPECT_EQ(PdfPreparationTestAccess::expandedRequiredBytes(interrupted), first.size());
  const uint32_t generation = interrupted.generation();
  const std::string journalPath = std::string(interrupted.cacheRoot()) + "/gen_" +
                                  std::to_string(generation) + "/resume.journal";
  interrupted.requestCancel();
  const PdfStepResult cancelled = runToTerminal(interrupted, harness);
  ASSERT_TRUE(cancelled.failed());
  ASSERT_EQ(cancelled.status.error, PdfError::Cancelled);
  const PreparationHarness cancelledBaseline = harness;

  PdfPreparation resumed;
  ASSERT_TRUE(resumed.begin(harness.config(sourcePath)).ok());
  while (resumed.resumedPhase() == PdfBuildResumePhase::None && step < 40000U) {
    const PdfStepResult resumeStep = stepWithPublicBounds(resumed, harness, step++);
    ASSERT_TRUE(resumeStep.yielded()) << static_cast<int>(resumeStep.status.error);
  }
  ASSERT_EQ(resumed.resumedPhase(), PdfBuildResumePhase::AfterPage);
  EXPECT_EQ(PdfPreparationTestAccess::expandedRequiredBytes(resumed), first.size());
  const PdfStepResult resumedResult = runToTerminal(resumed, harness);
  ASSERT_TRUE(resumedResult.complete()) << static_cast<int>(resumedResult.status.error);
  EXPECT_TRUE(resumed.resumedFromCheckpoint());
  EXPECT_EQ(PdfPreparationTestAccess::expandedRequiredBytes(resumed), first.size() + second.size());

  PreparationHarness rejected = cancelledBaseline;
  ASSERT_TRUE(rejected.storage.exists(journalPath));
  std::vector<uint8_t> journal = rejected.storage.bytes(journalPath);
  const std::array<uint8_t, 4> magic{'P', 'R', 'J', 'R'};
  const auto record = std::search(journal.begin(), journal.end(), magic.begin(), magic.end());
  ASSERT_NE(record, journal.end());
  const size_t recordOffset = static_cast<size_t>(record - journal.begin());
  ASSERT_LE(recordOffset + 512U, journal.size());
  journal[recordOffset + 4U] = 1U;
  journal[recordOffset + 5U] = 0U;
  writeLe32(&journal, recordOffset + 508U, pdfCacheCrc32(journal.data() + recordOffset, 508U));
  rejected.storage.addFile(journalPath, journal);
  PdfPreparation rebuilt;
  ASSERT_TRUE(rebuilt.begin(rejected.config(sourcePath)).ok());
  const PdfStepResult rebuiltResult = runToTerminal(rebuilt, rejected);
  ASSERT_TRUE(rebuiltResult.complete()) << static_cast<int>(rebuiltResult.status.error);
  EXPECT_FALSE(rebuilt.resumedFromCheckpoint());
  EXPECT_EQ(PdfPreparationTestAccess::expandedRequiredBytes(rebuilt), first.size() + second.size());
  EXPECT_EQ(rejected.storage.openHandleCount(), 0U);
}

TEST(PdfPreparationContentStreams, ChecksFreshFreeSpaceAndCleansFailedTemporaryStore) {
  std::string content(10U * 1024U, ' ');
  content += "BT /F1 12 Tf 72 720 Td (Capacity cleanup) Tj ET";
  const std::vector<uint8_t> pdf =
      onePagePdf("4 0 R", {streamObject(zlibStored(content), "FlateDecode")});
  ASSERT_FALSE(pdf.empty());
  constexpr uint64_t totalBytes = 256ULL * 1024ULL * 1024ULL;

  for (const uint64_t transientBytes : {uint64_t{0}, uint64_t{128}}) {
    SCOPED_TRACE(::testing::Message() << "transientBytes=" << transientBytes);
    constexpr char sourcePath[] = "/books/content-capacity.pdf";
    PreparationHarness harness;
    harness.storage.setMaximumReadHandles(1);
    harness.storage.setCapacity(totalBytes, 128ULL * 1024ULL * 1024ULL);
    harness.storage.addFile(sourcePath, pdf, 1234, true);
    PdfPreparation preparation;
    ASSERT_TRUE(preparation.begin(harness.config(sourcePath)).ok());
    uint32_t step = 0;
    ASSERT_TRUE(advanceToPhase(preparation, harness, PdfPreparationPhase::CheckContentCapacity, &step));
    const uint32_t capacityCalls = harness.storage.capacityCalls();
    harness.storage.setCapacity(totalBytes, PDF_CACHE_MIN_FREE_RESERVE_BYTES + transientBytes);
    const std::string storePath = contentStorePath(preparation);

    const PdfStepResult result = runToTerminal(preparation, harness);

    ASSERT_TRUE(result.failed());
    EXPECT_EQ(result.status.error, PdfError::InsufficientStorage);
    EXPECT_EQ(harness.storage.capacityCalls(), capacityCalls + 1U);
    EXPECT_FALSE(harness.storage.exists(storePath));
    EXPECT_EQ(harness.storage.openHandleCount(), 0U);
    const std::vector<PdfCacheOpenMode> modes = openModesForPath(harness.storage, storePath);
    if (transientBytes == 0) {
      EXPECT_TRUE(modes.empty());
    } else {
      EXPECT_EQ(modes, (std::vector<PdfCacheOpenMode>{PdfCacheOpenMode::WriteTruncate}));
    }
  }
}

TEST(PdfPreparationContentStreams, RoundTripsLiveNavigationStateWithoutTouchingRuntime) {
  const std::string content = "BT /F1 12 Tf 72 720 Td (Snapshot round trip) Tj ET";
  const std::string encoded = zlibStored(content);
  const std::vector<uint8_t> pdf = onePagePdf("4 0 R", {streamObject(encoded, "FlateDecode")});
  ASSERT_FALSE(pdf.empty());

  constexpr char sourcePath[] = "/books/navigation-snapshot.pdf";
  PreparationHarness harness;
  harness.storage.setMaximumReadHandles(1);
  harness.storage.addFile(sourcePath, pdf, 1234, true);
  PdfPreparation preparation;
  ASSERT_TRUE(preparation.begin(harness.config(sourcePath)).ok());
  uint32_t step = 0;
  ASSERT_TRUE(advanceToPhase(preparation, harness, PdfPreparationPhase::SuspendContentNavigation, &step));

  ASSERT_TRUE(stepWithPublicBounds(preparation, harness, step++).yielded());
  ASSERT_EQ(PdfPreparationTestAccess::snapshotStage(preparation), 1U);
  ASSERT_EQ(PdfPreparationTestAccess::snapshotOffset(preparation), 0U);
  const PdfPreparationTestAccess::LiveNavigationShape navigationShape =
      PdfPreparationTestAccess::liveNavigationShape(preparation);
  const std::vector<uint8_t> originalNavigation =
      PdfPreparationTestAccess::liveNavigationBytes(preparation, navigationShape);
  const size_t runtimePrefixBytes = PdfPreparationTestAccess::runtimePrefixBytes(preparation);
  const size_t placementPrefixBytes = PdfPreparationTestAccess::placementPrefixBytes(preparation);
  ASSERT_FALSE(originalNavigation.empty());
  EXPECT_EQ(originalNavigation.size(), PdfPreparationTestAccess::compactSnapshotBytes(preparation));
  ASSERT_NE(runtimePrefixBytes, 0U);
  ASSERT_EQ(placementPrefixBytes, 0U);
  ASSERT_FALSE(PdfPreparationTestAccess::placementActive(preparation));
#if UINTPTR_MAX == UINT32_MAX
  EXPECT_EQ(runtimePrefixBytes, 5288U);
  EXPECT_EQ(placementPrefixBytes, 0U);
#endif
  const std::vector<uint8_t> runtimeBeforeSuspend =
      copyBytes(PdfPreparationTestAccess::runRecords(preparation), runtimePrefixBytes);
  const std::vector<uint8_t> placementBeforeSuspend =
      copyBytes(PdfPreparationTestAccess::operandScratch(preparation), placementPrefixBytes);

  while (preparation.phase() == PdfPreparationPhase::SuspendContentNavigation) {
    ASSERT_TRUE(stepWithPublicBounds(preparation, harness, step++).yielded());
  }
  EXPECT_EQ(PdfPreparationTestAccess::snapshotStage(preparation), 2U);
  EXPECT_EQ(copyBytes(PdfPreparationTestAccess::runRecords(preparation), runtimePrefixBytes),
            runtimeBeforeSuspend);
  EXPECT_EQ(copyBytes(PdfPreparationTestAccess::operandScratch(preparation), placementPrefixBytes),
            placementBeforeSuspend);

  ASSERT_TRUE(advanceToPhase(preparation, harness, PdfPreparationPhase::RestoreContentNavigation, &step));
  const std::vector<uint8_t> runtimeBeforeRestore =
      copyBytes(PdfPreparationTestAccess::runRecords(preparation), runtimePrefixBytes);
  const std::vector<uint8_t> placementBeforeRestore =
      copyBytes(PdfPreparationTestAccess::operandScratch(preparation), placementPrefixBytes);
  while (preparation.phase() == PdfPreparationPhase::RestoreContentNavigation) {
    ASSERT_TRUE(stepWithPublicBounds(preparation, harness, step++).yielded());
  }

  ASSERT_EQ(preparation.phase(), PdfPreparationPhase::OpenDecodedContent);
  EXPECT_EQ(PdfPreparationTestAccess::snapshotStage(preparation), 0U);
  EXPECT_EQ(PdfPreparationTestAccess::liveNavigationBytes(preparation, navigationShape), originalNavigation);
  EXPECT_EQ(copyBytes(PdfPreparationTestAccess::runRecords(preparation), runtimePrefixBytes),
            runtimeBeforeRestore);
  EXPECT_EQ(copyBytes(PdfPreparationTestAccess::operandScratch(preparation), placementPrefixBytes),
            placementBeforeRestore);
}

TEST(PdfPreparationContentStreams, CancelsEachNavigationSnapshotStageWithinPublicBounds) {
  const std::string content(10U * 1024U, ' ');
  const std::string encoded = zlibStored(content + "BT /F1 12 Tf 72 720 Td (Cancel snapshot) Tj ET");
  const std::vector<uint8_t> pdf = onePagePdf("4 0 R", {streamObject(encoded, "FlateDecode")});
  ASSERT_FALSE(pdf.empty());

  for (const uint8_t targetStage : {uint8_t{1}, uint8_t{2}, uint8_t{3}}) {
    SCOPED_TRACE(::testing::Message() << "snapshotStage=" << static_cast<unsigned>(targetStage));
    constexpr char sourcePath[] = "/books/cancel-navigation-snapshot.pdf";
    PreparationHarness harness;
    harness.storage.setMaximumReadHandles(1);
    harness.storage.addFile(sourcePath, pdf, 1234, true);
    PdfPreparation preparation;
    ASSERT_TRUE(preparation.begin(harness.config(sourcePath)).ok());
    uint32_t step = 0;
    ASSERT_TRUE(advanceToPhase(preparation, harness, PdfPreparationPhase::SuspendContentNavigation, &step));

    while (step < 20000U) {
      const PdfStepResult result = stepWithPublicBounds(preparation, harness, step++);
      ASSERT_TRUE(result.yielded());
      const bool writerReady = preparation.phase() == PdfPreparationPhase::DecodeContent;
      const bool partialSnapshot = PdfPreparationTestAccess::snapshotOffset(preparation) != 0;
      if (PdfPreparationTestAccess::snapshotStage(preparation) == targetStage &&
          ((targetStage == 1U && partialSnapshot) || (targetStage == 2U && writerReady) ||
           (targetStage == 3U && partialSnapshot))) {
        break;
      }
    }
    ASSERT_EQ(PdfPreparationTestAccess::snapshotStage(preparation), targetStage);

    const std::string storePath = contentStorePath(preparation);
    const bool storeOpened = !openModesForPath(harness.storage, storePath).empty();
    const size_t runtimePrefixBytes = PdfPreparationTestAccess::runtimePrefixBytes(preparation);
    ASSERT_NE(runtimePrefixBytes, 0U);
    const std::vector<uint8_t> runtimeBeforeCancel =
        copyBytes(PdfPreparationTestAccess::runRecords(preparation), runtimePrefixBytes);
    const size_t eventStart = harness.storage.events().size();
    const size_t removeStart = harness.storage.removeObservations().size();
    preparation.requestCancel();

    uint32_t runtimeDestroyedStep = PdfPreparationTestAccess::runtimeConstructed(preparation) ? UINT32_MAX : 0U;
    uint32_t storeClosedStep = UINT32_MAX;
    uint32_t storeRemovedStep = UINT32_MAX;
    uint32_t snapshotResetStep = UINT32_MAX;
    PdfStepResult terminal = PdfStepResult::paused();
    for (uint32_t cancelStep = 0; cancelStep < 512U; ++cancelStep) {
      terminal = stepWithPublicBounds(preparation, harness, step++);
      if (runtimeDestroyedStep == UINT32_MAX && !PdfPreparationTestAccess::runtimeConstructed(preparation) &&
          PdfPreparationTestAccess::runRecords(preparation) != nullptr &&
          copyBytes(PdfPreparationTestAccess::runRecords(preparation), runtimePrefixBytes) != runtimeBeforeCancel) {
        runtimeDestroyedStep = cancelStep;
      }
      if (storeClosedStep == UINT32_MAX &&
          std::find(harness.storage.events().begin() + static_cast<std::ptrdiff_t>(eventStart),
                    harness.storage.events().end(), "close:" + storePath) != harness.storage.events().end()) {
        storeClosedStep = cancelStep;
      }
      if (storeRemovedStep == UINT32_MAX &&
          std::find(harness.storage.removeObservations().begin() + static_cast<std::ptrdiff_t>(removeStart),
                    harness.storage.removeObservations().end(), storePath) !=
              harness.storage.removeObservations().end()) {
        storeRemovedStep = cancelStep;
      }
      if (snapshotResetStep == UINT32_MAX && PdfPreparationTestAccess::snapshotStage(preparation) == 0U) {
        snapshotResetStep = cancelStep;
      }
      if (!terminal.yielded()) {
        break;
      }
    }

    ASSERT_TRUE(terminal.failed());
    EXPECT_EQ(terminal.status.error, PdfError::Cancelled);
    EXPECT_NE(runtimeDestroyedStep, UINT32_MAX);
    EXPECT_NE(snapshotResetStep, UINT32_MAX);
    EXPECT_LT(runtimeDestroyedStep, snapshotResetStep);
    if (storeOpened) {
      EXPECT_NE(storeClosedStep, UINT32_MAX);
      EXPECT_NE(storeRemovedStep, UINT32_MAX);
      EXPECT_LT(runtimeDestroyedStep, storeClosedStep);
      EXPECT_LT(storeClosedStep, storeRemovedStep);
      EXPECT_LT(storeRemovedStep, snapshotResetStep);
    }
    EXPECT_FALSE(harness.storage.exists(storePath));
    EXPECT_EQ(harness.storage.openHandleCount(), 0U);
    EXPECT_EQ(preparation.resourceCurrentBytes(), 0U);
  }
}

TEST(PdfPreparationContentStreams, CancelsWithAnActiveOptionalRasterWriterWithinPublicBounds) {
  constexpr char sourcePath[] = "/books/cancel-active-raster-writer.pdf";
  const std::vector<uint8_t> pdf = loadFixture("flate_gray_caption.pdf");
  ASSERT_FALSE(pdf.empty());

  PreparationHarness harness;
  harness.storage.setMaximumReadHandles(1);
  harness.storage.addFile(sourcePath, pdf, 1234, true);
  PdfPreparation preparation;
  ASSERT_TRUE(preparation.begin(harness.config(sourcePath)).ok());

  uint32_t step = 0;
  std::string partialImagePath;
  while (step < 20000U && partialImagePath.empty()) {
    const PdfStepResult result = stepWithPublicBounds(preparation, harness, step++);
    ASSERT_TRUE(result.yielded())
        << static_cast<int>(result.status.error) << "@" << result.status.offset;
    if (!PdfPreparationTestAccess::rasterRuntimeActive(preparation)) {
      continue;
    }
    for (const std::string& path : harness.storage.openHandlePaths()) {
      if (path.ends_with(".pxc")) {
        partialImagePath = path;
        break;
      }
    }
  }

  ASSERT_TRUE(PdfPreparationTestAccess::rasterRuntimeActive(preparation));
  ASSERT_FALSE(partialImagePath.empty());
  ASSERT_TRUE(harness.storage.exists(partialImagePath));

  const size_t eventStart = harness.storage.events().size();
  const size_t removeStart = harness.storage.removeObservations().size();
  const PdfStepResult cancelled = cancelToTerminalWithPublicBounds(preparation, harness, &step);

  ASSERT_TRUE(cancelled.failed());
  EXPECT_EQ(cancelled.status.error, PdfError::Cancelled);
  EXPECT_FALSE(PdfPreparationTestAccess::rasterRuntimeActive(preparation));
  EXPECT_NE(std::find(harness.storage.events().begin() + static_cast<std::ptrdiff_t>(eventStart),
                      harness.storage.events().end(), "close:" + partialImagePath),
            harness.storage.events().end());
  EXPECT_NE(std::find(harness.storage.removeObservations().begin() + static_cast<std::ptrdiff_t>(removeStart),
                      harness.storage.removeObservations().end(), partialImagePath),
            harness.storage.removeObservations().end());
  EXPECT_FALSE(harness.storage.exists(partialImagePath));
  EXPECT_EQ(harness.storage.openHandleCount(), 0U);
  EXPECT_EQ(preparation.resourceCurrentBytes(), 0U);
}

TEST(PdfPreparationContentStreams, CancelsWithAnActiveSectionRepairRuntimeWithinPublicBounds) {
  constexpr char sourcePath[] = "/books/cancel-active-section-repair.pdf";
  const std::vector<uint8_t> pdf = loadFixture("flate_gray_caption.pdf");
  ASSERT_FALSE(pdf.empty());

  PreparationHarness harness;
  harness.storage.setMaximumReadHandles(1);
  harness.storage.addFile(sourcePath, pdf, 1234, true);
  PdfPreparation preparation;
  ASSERT_TRUE(preparation.begin(harness.config(sourcePath)).ok());

  uint32_t step = 0;
  std::string failedImagePath;
  while (step < 20000U && failedImagePath.empty()) {
    const PdfStepResult result = stepWithPublicBounds(preparation, harness, step++);
    ASSERT_TRUE(result.yielded())
        << static_cast<int>(result.status.error) << "@" << result.status.offset;
    if (!PdfPreparationTestAccess::rasterRuntimeActive(preparation)) {
      continue;
    }
    for (const std::string& path : harness.storage.openHandlePaths()) {
      if (path.ends_with(".pxc")) {
        failedImagePath = path;
        break;
      }
    }
  }
  ASSERT_FALSE(failedImagePath.empty());

  harness.storage.fail(PdfTestFaultPoint::Write);
  while (step < 20128U && PdfPreparationTestAccess::failedRasterImages(preparation) == 0) {
    const PdfStepResult result = stepWithPublicBounds(preparation, harness, step++);
    ASSERT_TRUE(result.yielded())
        << static_cast<int>(result.status.error) << "@" << result.status.offset;
  }
  harness.storage.clearFault();
  ASSERT_NE(PdfPreparationTestAccess::failedRasterImages(preparation), 0U);
  ASSERT_FALSE(PdfPreparationTestAccess::rasterRuntimeActive(preparation));
  EXPECT_FALSE(harness.storage.exists(failedImagePath));

  std::string repairPath;
  while (step < 40000U && repairPath.empty()) {
    const PdfStepResult result = stepWithPublicBounds(preparation, harness, step++);
    ASSERT_TRUE(result.yielded())
        << static_cast<int>(result.status.error) << "@" << result.status.offset;
    if (!PdfPreparationTestAccess::sectionRepairRuntimeActive(preparation)) {
      continue;
    }
    for (const std::string& path : harness.storage.openHandlePaths()) {
      if (path.ends_with("/build.section-repair")) {
        repairPath = path;
        break;
      }
    }
  }

  ASSERT_TRUE(PdfPreparationTestAccess::sectionRepairRuntimeActive(preparation));
  ASSERT_FALSE(repairPath.empty());
  ASSERT_TRUE(harness.storage.exists(repairPath));
  const size_t eventStart = harness.storage.events().size();
  const size_t removeStart = harness.storage.removeObservations().size();

  const PdfStepResult cancelled = cancelToTerminalWithPublicBounds(preparation, harness, &step);

  ASSERT_TRUE(cancelled.failed());
  EXPECT_EQ(cancelled.status.error, PdfError::Cancelled);
  EXPECT_FALSE(PdfPreparationTestAccess::sectionRepairRuntimeActive(preparation));
  EXPECT_NE(std::find(harness.storage.events().begin() + static_cast<std::ptrdiff_t>(eventStart),
                      harness.storage.events().end(), "close:" + repairPath),
            harness.storage.events().end());
  EXPECT_NE(std::find(harness.storage.removeObservations().begin() + static_cast<std::ptrdiff_t>(removeStart),
                      harness.storage.removeObservations().end(), repairPath),
            harness.storage.removeObservations().end());
  EXPECT_FALSE(harness.storage.exists(repairPath));
  EXPECT_EQ(harness.storage.openHandleCount(), 0U);
  EXPECT_EQ(preparation.resourceCurrentBytes(), 0U);
}

TEST(PdfPreparationContentStreams, AcceptsCrlfStreamBoundaryBeforeSemanticExtraction) {
  const std::string content = "BT /F1 12 Tf 72 720 Td (CRLF boundary) Tj ET";
  const std::vector<uint8_t> pdf = onePagePdf("4 0 R", {crlfStreamObject(content)});
  ASSERT_FALSE(pdf.empty());

  constexpr char sourcePath[] = "/books/crlf-content.pdf";
  PreparationHarness harness;
  harness.storage.setMaximumReadHandles(1);
  harness.storage.addFile(sourcePath, pdf, 1234, true);
  PdfPreparation preparation;
  ASSERT_TRUE(preparation.begin(harness.config(sourcePath)).ok());

  const PdfStepResult result = runToTerminal(preparation, harness);

  ASSERT_TRUE(result.complete()) << "error=" << static_cast<int>(result.status.error)
                                 << " offset=" << result.status.offset;
  EXPECT_NE(preparedSection(preparation, harness).find("CRLF boundary"), std::string::npos);
}

TEST(PdfPreparationContentStreams, RejectsOverlongLengthBeforeDecoyTextCanReachXhtml) {
  const std::string painted = "BT /F1 12 Tf 72 720 Td (Safe text) Tj ET";
  const std::string decoy = "BT /F1 12 Tf 72 690 Td (LEAKED DECOY) Tj ET";
  const std::vector<uint8_t> pdf = onePagePdf("4 0 R", {overlongStreamObject(painted, decoy)});
  ASSERT_FALSE(pdf.empty());

  constexpr char sourcePath[] = "/books/overlong-content.pdf";
  PreparationHarness harness;
  harness.storage.setMaximumReadHandles(1);
  harness.storage.addFile(sourcePath, pdf, 1234, true);
  PdfPreparation preparation;
  ASSERT_TRUE(preparation.begin(harness.config(sourcePath)).ok());

  const PdfStepResult result = runToTerminal(preparation, harness);

  ASSERT_TRUE(result.failed());
  EXPECT_EQ(result.status.error, PdfError::Malformed);
  EXPECT_TRUE(preparedSection(preparation, harness).empty());
  EXPECT_EQ(harness.storage.openHandleCount(), 0U);
  for (const std::string& path : harness.storage.paths()) {
    EXPECT_EQ(path.find("LEAKED DECOY"), std::string::npos);
    EXPECT_EQ(path.find(".tmp"), std::string::npos);
  }
}

}  // namespace
