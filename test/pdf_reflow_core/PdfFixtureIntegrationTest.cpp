#include <gtest/gtest.h>

#include <array>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "PdfPageTree.h"
#include "PdfTestIo.h"

namespace {

std::vector<uint8_t> loadClassicFixture() {
  const std::filesystem::path path = std::filesystem::path(__FILE__).parent_path() / "fixtures" / "classic_text.pdf";
  std::ifstream input(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

struct IntegrationWorkspace {
  IntegrationWorkspace() { traversalStorage.forbidReadsWhile(&traversalReadForbidden); }

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
  PdfPageInfo firstPage{};
  uint32_t pageCount = 0;
  bool sourceOpen = true;
  bool xrefBlocked = true;
  bool traversalOpen = false;
  bool traversalReadForbidden = true;
  uint32_t traversalOpenCount = 0;
  uint32_t traversalCloseCount = 0;
};

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

PdfStatus captureFirstPage(void* context, const PdfPageInfo& page) {
  auto& workspace = *static_cast<IntegrationWorkspace*>(context);
  if (workspace.pageCount == 0) {
    workspace.firstPage = page;
  }
  ++workspace.pageCount;
  return PdfStatus::success();
}

PdfStatus setTraversalAccess(void* context, const bool required) {
  if (context == nullptr) {
    return PdfStatus::failure(PdfError::InvalidArgument);
  }
  auto& workspace = *static_cast<IntegrationWorkspace*>(context);
  workspace.traversalOpen = required;
  workspace.traversalReadForbidden = !required;
  workspace.sourceOpen = false;
  workspace.xrefBlocked = true;
  required ? ++workspace.traversalOpenCount : ++workspace.traversalCloseCount;
  return PdfStatus::success();
}

}  // namespace

TEST(PdfFixtureIntegrationTest, ResolvesCatalogPageTreeAndEmitsExactlyHelloPdf) {
  PdfTestByteSource memory(loadClassicFixture());
  memory.setMaximumRead(7);
  const PdfByteSource source = memory.source();
  IntegrationWorkspace workspace;

  PdfXrefParser xrefParser(source, workspace.sourceBuffer.data(), workspace.sourceBuffer.size(), workspace.arena,
                           workspace.xref);
  xrefParser.begin();
  ASSERT_TRUE(runBudgetOne(xrefParser).complete());

  PdfObjectReference catalogReference;
  ASSERT_TRUE(workspace.xref.root(&catalogReference));
  PdfObjectResolver resolver(source, workspace.xref, workspace.sourceBuffer.data(), workspace.sourceBuffer.size(),
                             workspace.arena);
  ASSERT_TRUE(resolver.begin(catalogReference).ok());
  ASSERT_TRUE(runResolver(resolver).complete());
  uint16_t pagesIndex = PDF_INVALID_INDEX;
  ASSERT_TRUE(pdfDictionaryFind(workspace.arena, resolver.result().rootIndex, "Pages", &pagesIndex));
  const PdfValue pages = workspace.arena.values[pagesIndex];
  ASSERT_EQ(pages.kind, PdfValueKind::Reference);

  PdfPageTreeWalker walker(resolver, workspace.arena,
                           workspace.traversalStorage.store(),
                           captureFirstPage, &workspace,
                           setTraversalAccess, &workspace,
                           &workspace.firstPage, {}, PdfLimits::MaxPages);
  ASSERT_TRUE(walker.begin({pages.objectNumber, pages.generation}).ok());
  ASSERT_TRUE(runPageTree(walker).complete());
  ASSERT_EQ(workspace.pageCount, 1u);
  ASSERT_EQ(workspace.firstPage.contentCount, 1u);
  EXPECT_GT(workspace.traversalOpenCount, 0U);
  EXPECT_EQ(workspace.traversalOpenCount, workspace.traversalCloseCount);
  EXPECT_FALSE(workspace.traversalOpen);
  EXPECT_TRUE(workspace.traversalReadForbidden);
  EXPECT_FALSE(workspace.sourceOpen);
  EXPECT_TRUE(workspace.xrefBlocked);

  ASSERT_TRUE(resolver.begin(workspace.firstPage.contents[0]).ok());
  ASSERT_TRUE(runResolver(resolver).complete());
  const PdfResolvedObject content = resolver.result();
  ASSERT_TRUE(content.hasStream);

  PdfByteRange streamRange;
  ASSERT_TRUE(pdfInitializeByteRange(source, content.streamOffset, content.streamLength, &streamRange).ok());
  const PdfByteSource streamSource = pdfByteRangeSource(streamRange);
  PdfLexer contentLexer(streamSource, workspace.sourceBuffer.data(), workspace.sourceBuffer.size());
  std::string transcript;
  while (true) {
    PdfToken token;
    PdfStepResult result;
    do {
      PdfWorkBudget budget{1, 1};
      result = contentLexer.next(token, budget);
    } while (result.yielded());
    ASSERT_TRUE(result.complete());
    if (token.kind == PdfTokenKind::End) {
      break;
    }
    if (token.kind == PdfTokenKind::String) {
      if (!transcript.empty()) {
        transcript.push_back(' ');
      }
      transcript.append(token.bytes, token.length);
    }
  }

  EXPECT_EQ(transcript, "Hello PDF");
}
