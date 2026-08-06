#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

#include "ContractCacheIo.h"
#include "PdfCacheFormat.h"
#include "PdfPreparation.h"

namespace {

void writeLe32(std::vector<uint8_t>& bytes, const size_t offset, const uint32_t value) {
  ASSERT_LE(offset + sizeof(value), bytes.size());
  for (uint8_t index = 0; index < sizeof(value); ++index) {
    bytes[offset + index] = static_cast<uint8_t>(value >> (index * 8U));
  }
}

void writeLe64(std::vector<uint8_t>& bytes, const size_t offset, const uint64_t value) {
  ASSERT_LE(offset + sizeof(value), bytes.size());
  for (uint8_t index = 0; index < sizeof(value); ++index) {
    bytes[offset + index] = static_cast<uint8_t>(value >> (index * 8U));
  }
}

class ClassicPdf {
 public:
  void add(const uint32_t objectNumber, std::string body) {
    ASSERT_GT(objectNumber, 0U);
    ASSERT_FALSE(objects_.contains(objectNumber));
    objects_.emplace(objectNumber, std::move(body));
  }

  std::vector<uint8_t> render() const {
    std::string output = "%PDF-1.7\n%\xE2\xE3\xCF\xD3\n";
    std::map<uint32_t, size_t> offsets;
    for (const auto& [number, body] : objects_) {
      offsets[number] = output.size();
      output += std::to_string(number) + " 0 obj\n" + body + "\nendobj\n";
    }

    const size_t xrefOffset = output.size();
    const uint32_t size = objects_.empty() ? 1U : objects_.rbegin()->first + 1U;
    output += "xref\n0 " + std::to_string(size) + "\n0000000000 65535 f \n";
    char entry[32]{};
    for (uint32_t number = 1; number < size; ++number) {
      const auto found = offsets.find(number);
      if (found == offsets.end()) {
        output += "0000000000 00000 f \n";
        continue;
      }
      const int length = std::snprintf(entry, sizeof(entry), "%010zu 00000 n \n", found->second);
      EXPECT_GT(length, 0);
      EXPECT_LT(static_cast<size_t>(length), sizeof(entry));
      output.append(entry, static_cast<size_t>(length));
    }
    output += "trailer\n<< /Size " + std::to_string(size) +
              " /Root 1 0 R >>\nstartxref\n" + std::to_string(xrefOffset) + "\n%%EOF\n";
    return {output.begin(), output.end()};
  }

 private:
  std::map<uint32_t, std::string> objects_;
};

std::string streamObject(const std::string& stream) {
  return "<< /Length " + std::to_string(stream.size()) + " >>\nstream\n" + stream + "\nendstream";
}

std::string fontObject() {
  return "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica /Encoding /WinAnsiEncoding >>";
}

std::vector<uint8_t> makePageCountPdf(const uint16_t pageCount, const uint32_t minimumXrefEntries = 0) {
  ClassicPdf pdf;
  const uint32_t font = 3U + static_cast<uint32_t>(pageCount) * 2U;
  std::string kids;
  for (uint16_t index = 0; index < pageCount; ++index) {
    const uint32_t page = 3U + static_cast<uint32_t>(index) * 2U;
    const uint32_t content = page + 1U;
    kids += std::to_string(page) + " 0 R ";
    char word[16]{};
    std::snprintf(word, sizeof(word), "Page%03u", static_cast<unsigned>(index + 1U));
    const std::string contentStream = "BT /F1 12 Tf 72 720 Td (" + std::string(word) + ") Tj ET";
    pdf.add(page, "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] "
                  "/Resources << /Font << /F1 " +
                      std::to_string(font) + " 0 R >> >> /Contents " + std::to_string(content) + " 0 R >>");
    pdf.add(content, streamObject(contentStream));
  }
  pdf.add(1, "<< /Type /Catalog /Pages 2 0 R >>");
  pdf.add(2, "<< /Type /Pages /Count " + std::to_string(pageCount) + " /Kids [" + kids + "] >>");
  pdf.add(font, fontObject());
  for (uint32_t number = font + 1U; number < minimumXrefEntries; ++number) {
    pdf.add(number, "<< /ContractPadding true >>");
  }
  return pdf.render();
}

std::vector<uint8_t> makeXrefEntryCountPdf(const uint32_t entryCount) {
  EXPECT_GE(entryCount, 6U);
  ClassicPdf pdf;
  const std::string content = "BT /F1 12 Tf 72 720 Td (Xref boundary control) Tj ET";
  pdf.add(1, "<< /Type /Catalog /Pages 2 0 R >>");
  pdf.add(2, "<< /Type /Pages /Count 1 /Kids [3 0 R] >>");
  pdf.add(3, "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] "
             "/Resources << /Font << /F1 5 0 R >> >> /Contents 4 0 R >>");
  pdf.add(4, streamObject(content));
  pdf.add(5, fontObject());
  for (uint32_t number = 6; number < entryCount; ++number) {
    pdf.add(number, "<< /ContractPadding true >>");
  }
  return pdf.render();
}

std::vector<uint8_t> makeIncrementalXrefPdf(const uint32_t entryCount = 71U) {
  EXPECT_GE(entryCount, 6U);
  std::string output = "%PDF-1.7\n";
  std::vector<size_t> offsets(entryCount);
  auto appendObject = [&](const uint32_t number, const std::string& body) {
    offsets[number] = output.size();
    output += std::to_string(number) + " 0 obj\n" + body + "\nendobj\n";
  };
  appendObject(1, "<< /Type /Catalog /Pages 2 0 R >>");
  appendObject(2, "<< /Type /Pages /Count 1 /Kids [3 0 R] >>");
  appendObject(3, "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] "
                  "/Resources << /Font << /F1 5 0 R >> >> /Contents 4 0 R >>");
  appendObject(4, streamObject("BT /F1 12 Tf 72 720 Td (Old incremental content) Tj ET"));
  appendObject(5, fontObject());
  const size_t baseXref = output.size();
  output += "xref\n0 " + std::to_string(entryCount) + "\n0000000000 65535 f \n";
  char entry[32]{};
  for (uint32_t number = 1; number < entryCount; ++number) {
    if (offsets[number] == 0) {
      output += "0000000000 00000 f \n";
    } else {
      const int length = std::snprintf(entry, sizeof(entry), "%010zu 00000 n \n", offsets[number]);
      EXPECT_GT(length, 0);
      output.append(entry, static_cast<size_t>(length));
    }
  }
  output += "trailer\n<< /Size " + std::to_string(entryCount) +
            " /Root 1 0 R >>\nstartxref\n" + std::to_string(baseXref) + "\n%%EOF\n";
  appendObject(4, streamObject("BT /F1 12 Tf 72 720 Td (Newest incremental content) Tj ET"));
  const size_t incrementalXref = output.size();
  output += "xref\n0 " + std::to_string(entryCount) + "\n0000000000 65535 f \n";
  for (uint32_t number = 1; number < entryCount; ++number) {
    if (offsets[number] == 0) {
      output += "0000000000 00000 f \n";
    } else {
      const int length = std::snprintf(entry, sizeof(entry), "%010zu 00000 n \n", offsets[number]);
      EXPECT_GT(length, 0);
      output.append(entry, static_cast<size_t>(length));
    }
  }
  output += "trailer\n<< /Size " + std::to_string(entryCount) + " /Root 1 0 R /Prev " +
            std::to_string(baseXref) +
            " >>\nstartxref\n" + std::to_string(incrementalXref) + "\n%%EOF\n";
  return {output.begin(), output.end()};
}

struct PreparationHarness {
  ContractCacheIo storage;
  PdfResourceSnapshot resources{128U * 1024U, 96U * 1024U, 8U * 1024U};
  uint32_t nowMs = 0;

  static uint32_t now(void* context) { return static_cast<PreparationHarness*>(context)->nowMs; }
  static PdfResourceSnapshot measure(void* context) {
    return static_cast<PreparationHarness*>(context)->resources;
  }
  static void resourceEvent(void*, const PdfResourceEvent&) {}

  PdfPreparationConfig config(const char* sourcePath) {
    return {storage.io(), sourcePath, "/.crosspoint", this, now, {this, measure, resourceEvent},
            storage.renameCallback(), 800, 480};
  }
};

PdfStepResult runToTerminal(PdfPreparation& preparation, PreparationHarness& harness,
                            const uint32_t maxSteps = 2'000'000U, uint32_t* const completedSteps = nullptr) {
  for (uint32_t step = 0; step < maxSteps; ++step) {
    const PdfStepResult result = preparation.step();
    ++harness.nowMs;
    if (!result.yielded()) {
      if (completedSteps != nullptr) {
        *completedSteps = step + 1U;
      }
      return result;
    }
  }
  if (completedSteps != nullptr) {
    *completedSteps = maxSteps;
  }
  return PdfStepResult::failure(PdfStatus::failure(PdfError::BudgetExhausted, maxSteps));
}

std::string generationRoot(const PdfPreparation& preparation) {
  return std::string(preparation.cacheRoot()) + "/gen_" + std::to_string(preparation.generation());
}

void expectCommittedProduct(const PreparationHarness& harness, const PdfPreparation& preparation) {
  const std::string root = generationRoot(preparation);
  EXPECT_TRUE(harness.storage.exists(root + "/metadata.bin"));
  EXPECT_TRUE(harness.storage.exists(root + "/sections/000000.xhtml"));
  EXPECT_EQ(harness.storage.openHandleCount(), 0U);
  EXPECT_LE(harness.storage.maximumReadHandleCount(), 1U) << harness.storage.lastReaderOverlap();
}

testing::AssertionResult preparationCompleted(const PdfStepResult& result, const PdfPreparation& preparation) {
  if (result.complete()) {
    return testing::AssertionSuccess();
  }
  return testing::AssertionFailure() << "error=" << static_cast<unsigned>(result.status.error)
                                     << " offset=" << result.status.offset
                                     << " phase=" << static_cast<unsigned>(preparation.phase());
}

TEST(PdfDocumentScalePositiveControl, ThirtyTwoPageClassicPdfCommitsPreparedOutput) {
  PreparationHarness harness;
  harness.storage.addFile("/books/32-pages.pdf", makePageCountPdf(32));
  PdfPreparation preparation;
  ASSERT_TRUE(preparation.begin(harness.config("/books/32-pages.pdf")).ok());

  const PdfStepResult result = runToTerminal(preparation, harness);

  ASSERT_TRUE(preparationCompleted(result, preparation));
  expectCommittedProduct(harness, preparation);
  EXPECT_EQ(preparation.workCounters().pagesWalked, 32U);
}

TEST(PdfDocumentScaleContract, ThirtyThreePageClassicPdfCommitsPreparedOutput) {
  PreparationHarness harness;
  harness.storage.addFile("/books/33-pages.pdf", makePageCountPdf(33));
  PdfPreparation preparation;
  ASSERT_TRUE(preparation.begin(harness.config("/books/33-pages.pdf")).ok());

  const PdfStepResult result = runToTerminal(preparation, harness);

  ASSERT_TRUE(preparationCompleted(result, preparation));
  expectCommittedProduct(harness, preparation);
  EXPECT_EQ(preparation.workCounters().pagesWalked, 33U);
}

TEST(PdfDocumentScaleContract, SixtyFivePageFlatTreeCommitsInKidsOrder) {
  PreparationHarness harness;
  harness.storage.addFile("/books/65-pages.pdf", makePageCountPdf(65));
  PdfPreparation preparation;
  ASSERT_TRUE(preparation.begin(harness.config("/books/65-pages.pdf")).ok());

  const PdfStepResult result = runToTerminal(preparation, harness);

  ASSERT_TRUE(preparationCompleted(result, preparation));
  expectCommittedProduct(harness, preparation);
  EXPECT_EQ(preparation.workCounters().pagesWalked, 65U);
  const std::string root = generationRoot(preparation);
  ASSERT_TRUE(harness.storage.exists(root + "/sections/000000.xhtml"));
  EXPECT_FALSE(harness.storage.exists(root + "/sections/000001.xhtml"));
  const auto& section = harness.storage.bytes(root + "/sections/000000.xhtml");
  const std::string text(section.begin(), section.end());
  const size_t first = text.find("Page001");
  const size_t last = text.find("Page065");
  ASSERT_NE(first, std::string::npos);
  ASSERT_NE(last, std::string::npos);
  EXPECT_LT(first, last);
}

TEST(PdfDocumentSliceBudgetContract, SixtyFivePageRunBoundsEveryDiscoveryStepAndAvoidsRedundantTempSyncs) {
  PreparationHarness harness;
  harness.storage.addFile("/books/65-pages-budget.pdf", makePageCountPdf(65));
  PdfPreparation preparation;
  ASSERT_TRUE(preparation.begin(harness.config("/books/65-pages-budget.pdf")).ok());

  uint64_t maximumOperations = 0;
  uint64_t maximumBytes = 0;
  uint32_t maximumMilliseconds = 0;
  PdfPreparationPhase operationsPhase = PdfPreparationPhase::Idle;
  PdfPreparationPhase bytesPhase = PdfPreparationPhase::Idle;
  PdfPreparationPhase timePhase = PdfPreparationPhase::Idle;
  PdfPreparationPhase terminalPhase = PdfPreparationPhase::Idle;
  ContractCacheIo::Metrics slowStep{};
  std::string slowStepTrace;
  PdfStepResult result = PdfStepResult::paused();
  for (uint32_t step = 0; step < 2'000'000U && result.yielded(); ++step) {
    const PdfPreparationPhase phase = preparation.phase();
    terminalPhase = phase;
    const bool boundedPhase = phase == PdfPreparationPhase::ParseXref || phase == PdfPreparationPhase::SortXref ||
                              phase == PdfPreparationPhase::ResolveCatalog ||
                              phase == PdfPreparationPhase::InitializePageTree ||
                              phase == PdfPreparationPhase::WalkPages ||
                              phase == PdfPreparationPhase::FinalizePageTree ||
                              phase == PdfPreparationPhase::ResolveNavigation;
    harness.storage.advanceClockOnOperation(boundedPhase ? &harness.nowMs : nullptr);
    const uint64_t operationsBefore = harness.storage.metrics().operations;
    const uint64_t bytesBefore = harness.storage.ioBytes();
    const uint32_t timeBefore = harness.nowMs;
    const ContractCacheIo::Metrics metricsBefore = harness.storage.metrics();
    harness.storage.clearOperationTrace();
    result = preparation.step();
    const uint64_t operations = harness.storage.metrics().operations - operationsBefore;
    const uint64_t bytes = harness.storage.ioBytes() - bytesBefore;
    const uint32_t milliseconds = harness.nowMs - timeBefore;
    if (boundedPhase && operations > maximumOperations) {
      maximumOperations = operations;
      operationsPhase = phase;
    }
    if (boundedPhase && bytes > maximumBytes) {
      maximumBytes = bytes;
      bytesPhase = phase;
    }
    if (boundedPhase && milliseconds > maximumMilliseconds) {
      maximumMilliseconds = milliseconds;
      timePhase = phase;
      const ContractCacheIo::Metrics metricsAfter = harness.storage.metrics();
      slowStep.operations = metricsAfter.operations - metricsBefore.operations;
      slowStep.bytesRead = metricsAfter.bytesRead - metricsBefore.bytesRead;
      slowStep.bytesWritten = metricsAfter.bytesWritten - metricsBefore.bytesWritten;
      slowStep.opens = metricsAfter.opens - metricsBefore.opens;
      slowStep.reads = metricsAfter.reads - metricsBefore.reads;
      slowStep.writes = metricsAfter.writes - metricsBefore.writes;
      slowStep.closes = metricsAfter.closes - metricsBefore.closes;
      slowStepTrace.clear();
      for (const std::string& operation : harness.storage.operationTrace()) {
        if (!slowStepTrace.empty()) {
          slowStepTrace += ",";
        }
        slowStepTrace += operation;
      }
    }
    ++harness.nowMs;
  }

  ASSERT_TRUE(preparationCompleted(result, preparation)) << "terminalPhase=" << static_cast<unsigned>(terminalPhase);
  EXPECT_LE(maximumOperations, 32U) << "phase=" << static_cast<unsigned>(operationsPhase);
  EXPECT_LE(maximumBytes, 4096U) << "phase=" << static_cast<unsigned>(bytesPhase);
  EXPECT_LE(maximumMilliseconds, 5U)
      << "phase=" << static_cast<unsigned>(timePhase) << " opens=" << slowStep.opens
      << " reads=" << slowStep.reads << " writes=" << slowStep.writes << " closes=" << slowStep.closes
      << " readBytes=" << slowStep.bytesRead << " writeBytes=" << slowStep.bytesWritten
      << " trace=" << slowStepTrace;
  const std::string root = generationRoot(preparation);
  EXPECT_EQ(harness.storage.syncCount(root + "/build.xref.a"), 0U);
  EXPECT_EQ(harness.storage.syncCount(root + "/build.xref.b"), 0U);
  EXPECT_EQ(harness.storage.syncCount(root + "/build.pages"), 0U);
  EXPECT_EQ(harness.storage.syncCount(root + "/build.page-records"), 0U);
  EXPECT_EQ(harness.storage.syncCount(root + "/resume.journal"), 0U);
}

TEST(PdfDocumentScalePositiveControl, TwoHundredFiftySixXrefEntriesCommitPreparedOutput) {
  PreparationHarness harness;
  harness.storage.addFile("/books/256-xref.pdf", makeXrefEntryCountPdf(256));
  PdfPreparation preparation;
  ASSERT_TRUE(preparation.begin(harness.config("/books/256-xref.pdf")).ok());

  const PdfStepResult result = runToTerminal(preparation, harness);

  ASSERT_TRUE(preparationCompleted(result, preparation));
  expectCommittedProduct(harness, preparation);
  EXPECT_EQ(preparation.workCounters().pagesWalked, 1U);
  EXPECT_EQ(preparation.workCounters().xrefSpoolRecordsRead, 0U);
  EXPECT_EQ(preparation.workCounters().xrefSpoolRecordsWritten, 256U);
}

TEST(PdfDocumentScaleContract, TwoHundredFiftySevenXrefEntriesCommitPreparedOutput) {
  PreparationHarness harness;
  harness.storage.addFile("/books/257-xref.pdf", makeXrefEntryCountPdf(257));
  PdfPreparation preparation;
  ASSERT_TRUE(preparation.begin(harness.config("/books/257-xref.pdf")).ok());

  const PdfStepResult result = runToTerminal(preparation, harness);

  ASSERT_TRUE(preparationCompleted(result, preparation));
  expectCommittedProduct(harness, preparation);
  EXPECT_EQ(preparation.workCounters().pagesWalked, 1U);
  EXPECT_EQ(preparation.workCounters().xrefSpoolRecordsRead, 0U);
  EXPECT_EQ(preparation.workCounters().xrefSpoolRecordsWritten, 257U);
}

TEST(PdfDocumentScaleContract, ObjectZeroPlusMaximumIndirectObjectsCommitsPreparedOutput) {
  PreparationHarness harness;
  harness.storage.addFile("/books/100001-xref.pdf", makeXrefEntryCountPdf(100001));
  PdfPreparation preparation;
  ASSERT_TRUE(preparation.begin(harness.config("/books/100001-xref.pdf")).ok());

  const PdfStepResult result = runToTerminal(preparation, harness, 4'000'000U);

  ASSERT_TRUE(preparationCompleted(result, preparation));
  expectCommittedProduct(harness, preparation);
  EXPECT_EQ(preparation.workCounters().xrefSpoolRecordsRead, 0U);
  EXPECT_EQ(preparation.workCounters().xrefSpoolRecordsWritten, 100001U);
}

TEST(PdfDocumentXrefFilterContract, CancelAfterCrossingSegmentBoundaryCanRestartSamePreparation) {
  PreparationHarness harness;
  harness.storage.addFile("/books/segmented-filter-cancel.pdf", makeXrefEntryCountPdf(100'001U));
  PdfPreparation preparation;
  ASSERT_TRUE(preparation.begin(harness.config("/books/segmented-filter-cancel.pdf")).ok());

  for (uint32_t step = 0;
       step < 2'000'000U && preparation.phase() != PdfPreparationPhase::ParseXref; ++step) {
    const PdfStepResult result = preparation.step();
    ++harness.nowMs;
    ASSERT_TRUE(result.yielded()) << static_cast<unsigned>(result.status.error) << "@" << result.status.offset;
  }
  ASSERT_EQ(preparation.phase(), PdfPreparationPhase::ParseXref);
  // Object 98,304 is the first bit in the second span. Xref rows are 20 bytes,
  // so this source-read threshold is beyond that row while parsing is active.
  for (uint32_t step = 0;
       step < 2'000'000U && preparation.phase() == PdfPreparationPhase::ParseXref &&
       preparation.workCounters().sourceBytesRead < 1'980'000U;
       ++step) {
    const PdfStepResult result = preparation.step();
    ++harness.nowMs;
    ASSERT_TRUE(result.yielded()) << static_cast<unsigned>(result.status.error) << "@" << result.status.offset;
  }
  ASSERT_EQ(preparation.phase(), PdfPreparationPhase::ParseXref);
  ASSERT_GE(preparation.workCounters().sourceBytesRead, 1'980'000U);
  preparation.requestCancel();
  const PdfStepResult cancelled = runToTerminal(preparation, harness, 1'024U);
  ASSERT_TRUE(cancelled.failed());
  ASSERT_EQ(cancelled.status.error, PdfError::Cancelled);

  ASSERT_TRUE(preparation.begin(harness.config("/books/segmented-filter-cancel.pdf")).ok());
  const PdfStepResult restarted = runToTerminal(preparation, harness, 4'000'000U);
  ASSERT_TRUE(preparationCompleted(restarted, preparation));
  expectCommittedProduct(harness, preparation);
  EXPECT_EQ(preparation.workCounters().xrefSpoolRecordsWritten, 100'001U);
}

TEST(PdfDocumentResumeScaleContract, TwoHundredFiftySevenXrefEntriesResumeWithoutReparse) {
  PreparationHarness harness;
  harness.storage.addFile("/books/257-xref-resume.pdf", makePageCountPdf(2, 257));
  PdfPreparation interrupted;
  ASSERT_TRUE(interrupted.begin(harness.config("/books/257-xref-resume.pdf")).ok());
  for (uint32_t step = 0; step < 500'000U && interrupted.durableResumePage() < 1U; ++step) {
    const PdfStepResult result = interrupted.step();
    ++harness.nowMs;
    ASSERT_TRUE(result.yielded()) << static_cast<unsigned>(result.status.error) << "@" << result.status.offset;
  }
  ASSERT_EQ(interrupted.durableResumePhase(), PdfBuildResumePhase::AfterPage);
  ASSERT_EQ(interrupted.durableResumePage(), 1U);
  const uint32_t generation = interrupted.generation();
  interrupted.requestCancel();
  const PdfStepResult cancelled = runToTerminal(interrupted, harness, 1024U);
  ASSERT_TRUE(cancelled.failed());
  ASSERT_EQ(cancelled.status.error, PdfError::Cancelled);

  PdfPreparation resumed;
  ASSERT_TRUE(resumed.begin(harness.config("/books/257-xref-resume.pdf")).ok());
  const PdfStepResult result = runToTerminal(resumed, harness);

  ASSERT_TRUE(preparationCompleted(result, resumed));
  EXPECT_TRUE(resumed.resumedFromCheckpoint());
  EXPECT_EQ(resumed.resumedPhase(), PdfBuildResumePhase::AfterPage);
  EXPECT_EQ(resumed.generation(), generation);
  EXPECT_EQ(resumed.workCounters().xrefSteps, 0U);
  EXPECT_EQ(resumed.workCounters().pagesWalked, 0U);
  expectCommittedProduct(harness, resumed);
}

TEST(PdfDocumentResumeScaleContract, JournalAboveLegacySlotLimitRestoresInBatchedSteps) {
  constexpr uint32_t kXrefEntries = 22'000U;
  PreparationHarness harness;
  harness.storage.addFile("/books/large-journal-resume.pdf", makePageCountPdf(2, kXrefEntries));
  PdfPreparation interrupted;
  ASSERT_TRUE(interrupted.begin(harness.config("/books/large-journal-resume.pdf")).ok());
  for (uint32_t step = 0; step < 500'000U && interrupted.durableResumePage() < 1U; ++step) {
    const PdfStepResult result = interrupted.step();
    ++harness.nowMs;
    ASSERT_TRUE(result.yielded()) << static_cast<unsigned>(result.status.error) << "@" << result.status.offset;
  }
  ASSERT_EQ(interrupted.durableResumePhase(), PdfBuildResumePhase::AfterPage);
  ASSERT_EQ(interrupted.durableResumePage(), 1U);
  const uint32_t generation = interrupted.generation();
  interrupted.requestCancel();
  ASSERT_TRUE(runToTerminal(interrupted, harness, 1024U).failed());

  PdfPreparation resumed;
  ASSERT_TRUE(resumed.begin(harness.config("/books/large-journal-resume.pdf")).ok());
  uint32_t resumeSteps = 0;
  uint64_t maximumResumeOperations = 0;
  uint64_t maximumResumeBytes = 0;
  uint32_t maximumResumeMilliseconds = 0;
  std::string slowResumeTrace;
  PdfStepResult result = PdfStepResult::paused();
  for (; resumeSteps < 100'000U && result.yielded(); ++resumeSteps) {
    const bool resumeSetup = resumed.phase() == PdfPreparationPhase::PrepareCache;
    harness.storage.advanceClockOnOperation(resumeSetup ? &harness.nowMs : nullptr);
    const uint64_t operationsBefore = harness.storage.metrics().operations;
    const uint64_t bytesBefore = harness.storage.ioBytes();
    const uint32_t timeBefore = harness.nowMs;
    harness.storage.clearOperationTrace();
    result = resumed.step();
    const uint64_t operations = harness.storage.metrics().operations - operationsBefore;
    const uint64_t bytes = harness.storage.ioBytes() - bytesBefore;
    const uint32_t milliseconds = harness.nowMs - timeBefore;
    harness.storage.advanceClockOnOperation(nullptr);
    if (resumeSetup) {
      maximumResumeOperations = std::max(maximumResumeOperations, operations);
      maximumResumeBytes = std::max(maximumResumeBytes, bytes);
      if (milliseconds > maximumResumeMilliseconds) {
        maximumResumeMilliseconds = milliseconds;
        slowResumeTrace.clear();
        for (const std::string& operation : harness.storage.operationTrace()) {
          if (!slowResumeTrace.empty()) {
            slowResumeTrace += ",";
          }
          slowResumeTrace += operation;
        }
      }
      EXPECT_LE(operations, 32U) << "resume step=" << resumeSteps;
      EXPECT_LE(bytes, 4096U) << "resume step=" << resumeSteps;
    }
    ++harness.nowMs;
  }

  ASSERT_TRUE(preparationCompleted(result, resumed));
  EXPECT_TRUE(resumed.resumedFromCheckpoint());
  EXPECT_EQ(resumed.generation(), generation);
  EXPECT_EQ(resumed.workCounters().xrefSteps, 0U);
  EXPECT_EQ(resumed.workCounters().pagesWalked, 0U);
  EXPECT_LT(resumeSteps, 4'000U);
  EXPECT_LE(maximumResumeOperations, 32U);
  EXPECT_LE(maximumResumeBytes, 4096U);
  EXPECT_LE(maximumResumeMilliseconds, 8U) << slowResumeTrace;
  expectCommittedProduct(harness, resumed);
}

TEST(PdfDocumentResumeScaleContract, SameBatchDuplicateWithRecomputedIntegrityIsRejected) {
  constexpr uint32_t kXrefEntries = 257U;
  constexpr uint16_t kPageCount = 2U;
  constexpr size_t kHeaderBytes = 192U;
  constexpr size_t kXrefRecordBytes = 24U;
  constexpr size_t kPageRecordBytes = 244U;
  constexpr size_t kTrailerBytes = 72U;
  constexpr size_t kRecordCrcOffset = 20U;
  constexpr size_t kTrailerCrcOffset = 68U;
  PreparationHarness harness;
  harness.storage.addFile("/books/tampered-batch-resume.pdf", makePageCountPdf(kPageCount, kXrefEntries));
  PdfPreparation interrupted;
  ASSERT_TRUE(interrupted.begin(harness.config("/books/tampered-batch-resume.pdf")).ok());
  for (uint32_t step = 0; step < 500'000U && interrupted.durableResumePage() < 1U; ++step) {
    const PdfStepResult result = interrupted.step();
    ++harness.nowMs;
    ASSERT_TRUE(result.yielded()) << static_cast<unsigned>(result.status.error) << "@" << result.status.offset;
  }
  ASSERT_EQ(interrupted.durableResumePage(), 1U);
  interrupted.requestCancel();
  ASSERT_TRUE(runToTerminal(interrupted, harness, 1024U).failed());

  const std::string journalPath = generationRoot(interrupted) + "/resume.journal";
  std::vector<uint8_t> journal = harness.storage.bytes(journalPath);
  const size_t recordsBytes = static_cast<size_t>(kXrefEntries) * kXrefRecordBytes +
                              static_cast<size_t>(kPageCount) * kPageRecordBytes;
  const size_t trailerOffset = kHeaderBytes + recordsBytes;
  ASSERT_GE(journal.size(), trailerOffset + kTrailerBytes);
  const size_t previousRecord = kHeaderBytes + 9U * kXrefRecordBytes;
  const size_t duplicateRecord = kHeaderBytes + 10U * kXrefRecordBytes;
  std::copy_n(journal.begin() + static_cast<ptrdiff_t>(previousRecord), sizeof(uint32_t),
              journal.begin() + static_cast<ptrdiff_t>(duplicateRecord));
  writeLe32(journal, duplicateRecord + kRecordCrcOffset,
            pdfCacheCrc32(journal.data() + duplicateRecord, kRecordCrcOffset));
  writeLe32(journal, trailerOffset + 28U,
            pdfCacheCrc32(journal.data() + kHeaderBytes, recordsBytes));
  writeLe64(journal, trailerOffset + 32U,
            pdfCacheFnv64(journal.data() + kHeaderBytes, recordsBytes));
  writeLe32(journal, trailerOffset + kTrailerCrcOffset,
            pdfCacheCrc32(journal.data() + trailerOffset, kTrailerCrcOffset));
  harness.storage.addFile(journalPath, journal);

  PdfPreparation resumed;
  ASSERT_TRUE(resumed.begin(harness.config("/books/tampered-batch-resume.pdf")).ok());
  const PdfStepResult result = runToTerminal(resumed, harness);

  ASSERT_TRUE(preparationCompleted(result, resumed));
  EXPECT_FALSE(resumed.resumedFromCheckpoint());
  EXPECT_GT(resumed.workCounters().xrefSteps, 0U);
  expectCommittedProduct(harness, resumed);
}

TEST(PdfDocumentResumeScaleContract, CancelledBatchedRestoreClosesHandlesAndPreservesPriorResumePoint) {
  constexpr uint32_t kXrefEntries = 22'000U;
  PreparationHarness harness;
  harness.storage.addFile("/books/cancel-batched-restore.pdf", makePageCountPdf(2, kXrefEntries));
  PdfPreparation interrupted;
  ASSERT_TRUE(interrupted.begin(harness.config("/books/cancel-batched-restore.pdf")).ok());
  for (uint32_t step = 0; step < 500'000U && interrupted.durableResumePage() < 1U; ++step) {
    const PdfStepResult result = interrupted.step();
    ++harness.nowMs;
    ASSERT_TRUE(result.yielded()) << static_cast<unsigned>(result.status.error) << "@" << result.status.offset;
  }
  ASSERT_EQ(interrupted.durableResumePage(), 1U);
  const uint32_t generation = interrupted.generation();
  interrupted.requestCancel();
  ASSERT_TRUE(runToTerminal(interrupted, harness, 1024U).failed());
  ASSERT_EQ(harness.storage.openHandleCount(), 0U);

  PdfPreparation restoring;
  ASSERT_TRUE(restoring.begin(harness.config("/books/cancel-batched-restore.pdf")).ok());
  const std::string expectedWrite = "write:" + generationRoot(interrupted) + "/build.xref.a";
  bool observedBatchWrite = false;
  for (uint32_t step = 0; step < 100'000U && !observedBatchWrite; ++step) {
    harness.storage.clearOperationTrace();
    const PdfStepResult result = restoring.step();
    ++harness.nowMs;
    ASSERT_TRUE(result.yielded()) << static_cast<unsigned>(result.status.error) << "@" << result.status.offset;
    observedBatchWrite =
        std::find(harness.storage.operationTrace().begin(), harness.storage.operationTrace().end(), expectedWrite) !=
        harness.storage.operationTrace().end();
  }
  ASSERT_TRUE(observedBatchWrite);
  restoring.requestCancel();
  const PdfStepResult cancelled = runToTerminal(restoring, harness, 4096U);
  ASSERT_TRUE(cancelled.failed());
  EXPECT_EQ(cancelled.status.error, PdfError::Cancelled);
  EXPECT_EQ(harness.storage.openHandleCount(), 0U);

  PdfPreparation recovered;
  ASSERT_TRUE(recovered.begin(harness.config("/books/cancel-batched-restore.pdf")).ok());
  const PdfStepResult result = runToTerminal(recovered, harness);

  ASSERT_TRUE(preparationCompleted(result, recovered));
  EXPECT_TRUE(recovered.resumedFromCheckpoint());
  EXPECT_EQ(recovered.resumedPhase(), PdfBuildResumePhase::AfterPage);
  EXPECT_EQ(recovered.generation(), generation);
  EXPECT_EQ(recovered.workCounters().xrefSteps, 0U);
  EXPECT_EQ(recovered.workCounters().pagesWalked, 0U);
  EXPECT_EQ(harness.storage.openHandleCount(), 0U);
  expectCommittedProduct(harness, recovered);
}

TEST(PdfDocumentXrefEnergyContract, TwoFullRevisionsCrossRunDuplicateUsesNewestObject) {
  PreparationHarness harness;
  harness.storage.addFile("/books/incremental.pdf", makeIncrementalXrefPdf());
  PdfPreparation preparation;
  ASSERT_TRUE(preparation.begin(harness.config("/books/incremental.pdf")).ok());

  const PdfStepResult result = runToTerminal(preparation, harness);

  ASSERT_TRUE(preparationCompleted(result, preparation));
  expectCommittedProduct(harness, preparation);
  const auto& section = harness.storage.bytes(generationRoot(preparation) + "/sections/000000.xhtml");
  const std::string text(section.begin(), section.end());
  EXPECT_NE(text.find("Newest incremental content"), std::string::npos);
  EXPECT_EQ(text.find("Old incremental content"), std::string::npos);
  EXPECT_EQ(preparation.workCounters().xrefSpoolRecordsRead, 0U);
  EXPECT_EQ(preparation.workCounters().xrefSpoolRecordsWritten, 71U);
}

TEST(PdfDocumentXrefScaleContract, TwoFullMaximumRevisionsStayWithinUniqueObjectBound) {
  PreparationHarness harness;
  harness.storage.addFile("/books/max-incremental.pdf", makeIncrementalXrefPdf(100'001U));
  PdfPreparation preparation;
  ASSERT_TRUE(preparation.begin(harness.config("/books/max-incremental.pdf")).ok());

  const PdfStepResult result = runToTerminal(preparation, harness, 4'000'000U);

  ASSERT_TRUE(preparationCompleted(result, preparation));
  expectCommittedProduct(harness, preparation);
  const auto& section = harness.storage.bytes(generationRoot(preparation) + "/sections/000000.xhtml");
  const std::string text(section.begin(), section.end());
  EXPECT_NE(text.find("Newest incremental content"), std::string::npos);
  EXPECT_EQ(text.find("Old incremental content"), std::string::npos);
  EXPECT_EQ(preparation.workCounters().xrefSpoolRecordsRead, 0U);
  EXPECT_EQ(preparation.workCounters().xrefSpoolRecordsWritten, 100'001U);
}

}  // namespace
