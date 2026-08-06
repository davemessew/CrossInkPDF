#include <gtest/gtest.h>

#include <GfxRenderer.h>

#include <algorithm>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "Epub/Epub.h"
#include "Epub/Epub/Section.h"
#include "EpubProductionTestState.h"
#include "ZipFile.h"

namespace {

std::vector<std::string> mutationTrace(
    const std::vector<std::string>& operations) {
  std::vector<std::string> mutations;
  for (const std::string& operation : operations) {
    if (operation.starts_with("rename-") ||
        operation.starts_with("remove-dir:")) {
      mutations.push_back(operation);
    }
  }
  return mutations;
}

constexpr char kFirstSpineHref[] = "text/part.xhtml";
constexpr char kSecondSpineHref[] = "text/chapter.xhtml";

bool loggedMessageContaining(const std::string& needle) {
  return std::ranges::any_of(epub_production_test::errorLogs,
                             [&needle](const std::string& message) { return message.find(needle) != std::string::npos; });
}

class PdfFormatEpub final : public Epub {
 public:
  using Epub::Epub;

  ReflowDocumentFormat getFormat() const override { return ReflowDocumentFormat::Pdf; }
};

class EpubWordIndexProbe final : public Epub {
 public:
  using Epub::Epub;

  bool validateLayoutWordIndex(const std::string&, int, uint16_t) const override {
    ++fixedRecordHookCalls_;
    return true;
  }
  bool removeLayoutWordIndex(const std::string&) const override {
    ++fixedRecordHookCalls_;
    return true;
  }
  bool readLayoutWordRange(const std::string&, uint16_t, uint16_t, ReflowPageSemanticRange&) const override {
    ++fixedRecordHookCalls_;
    return true;
  }
  bool findLayoutWordPage(const std::string&, const char*, uint32_t, uint32_t, uint16_t&) const override {
    ++fixedRecordHookCalls_;
    return true;
  }
  bool findLayoutWordCursor(const std::string&, uint32_t, uint16_t&) const override {
    ++fixedRecordHookCalls_;
    return true;
  }

  int fixedRecordHookCalls() const { return fixedRecordHookCalls_; }

 private:
  mutable int fixedRecordHookCalls_ = 0;
};

class EpubLegacyHrefTest : public testing::Test {
 protected:
  void SetUp() override { epub_production_test::resetAll(); }
};

TEST_F(EpubLegacyHrefTest, LoadedEpubReturnsExactRequestedSpineHref) {
  Epub document("/books/fixture.epub", "/cache");
  ASSERT_TRUE(document.load(false, true));
  epub_production_test::metadata.resetQueryCounts();

  std::string href;
  ASSERT_TRUE(document.getSectionHref(1, href));

  EXPECT_EQ(href, kSecondSpineHref);
  EXPECT_EQ(epub_production_test::metadata.spineEntryQueries, 1);
}

TEST_F(EpubLegacyHrefTest, LoadedEpubInvalidSectionReturnsEmptyWithoutSpineLookup) {
  Epub document("/books/fixture.epub", "/cache");
  ASSERT_TRUE(document.load(false, true));
  epub_production_test::metadata.resetQueryCounts();
  epub_production_test::errorLogs.clear();

  for (const int invalidSectionIndex : {-1, 99}) {
    std::string href = "sentinel";
    EXPECT_FALSE(document.getSectionHref(invalidSectionIndex, href));
    EXPECT_TRUE(href.empty());
  }

  EXPECT_EQ(epub_production_test::metadata.spineEntryQueries, 0);
}

TEST_F(EpubLegacyHrefTest, UnloadedEpubRejectsZeroSpineWithoutLookupOrError) {
  Epub document("/books/fixture.epub", "/cache");
  epub_production_test::metadata.resetQueryCounts();
  epub_production_test::errorLogs.clear();

  std::string href = "sentinel";
  EXPECT_FALSE(document.getSectionHref(0, href));

  EXPECT_TRUE(href.empty());
  EXPECT_EQ(epub_production_test::metadata.spineEntryQueries, 0);
  EXPECT_FALSE(loggedMessageContaining("cache not loaded"));
}

TEST_F(EpubLegacyHrefTest, PdfFormatKeepsRejectingInvalidVirtualSectionHref) {
  PdfFormatEpub document("/books/fixture.pdf", "/cache");
  ASSERT_TRUE(document.load(false, true));
  epub_production_test::metadata.resetQueryCounts();

  std::string href = "sentinel";
  EXPECT_FALSE(document.getSectionHref(99, href));

  EXPECT_TRUE(href.empty());
  EXPECT_EQ(epub_production_test::metadata.spineEntryQueries, 0);
}

TEST_F(EpubLegacyHrefTest, EpubSectionNeverCreatesOrCallsFixedRecordWordIndex) {
  auto document = std::make_shared<EpubWordIndexProbe>("/books/fixture.epub", "/cache");
  ASSERT_TRUE(document->load(false, true));
  epub_production_test::parser.mode = epub_production_test::ParserMode::CaptureOnly;
  epub_production_test::storage.addFile(document->getCachePath() + "/html/1.html", {'<', 'p', '/', '>'});
  epub_production_test::storage.openedWritePaths.clear();

  GfxRenderer renderer;
  Section section(document, 1, renderer);
  ASSERT_TRUE(section.createSectionFile(0, 1.0f, false, false, 0, 600, 800, false, false, 0, false, false));
  ASSERT_TRUE(section.loadSectionFile(0, 1.0f, false, false, 0, 600, 800, false, false, 0, false, false,
                                      EpubRenderMode::CrossInkDefault));
  ASSERT_TRUE(section.clearCache());

  EXPECT_EQ(document->fixedRecordHookCalls(), 0);
  EXPECT_TRUE(std::ranges::none_of(epub_production_test::storage.openedWritePaths, [](const std::string& path) {
    return path.ends_with(".pwi") || path.ends_with(".pwi.tmp");
  }));
}

TEST_F(EpubLegacyHrefTest, NoPathAllocationCacheCleanupUsesExactSlicedFNVPath) {
  constexpr char paddedPath[] = "/Books/legacy.epub.trailing";
  const std::string_view exactPath(paddedPath,
                                   sizeof("/Books/legacy.epub") - 1U);
  const std::string currentPath =
      "/cache/epub_" +
      std::to_string(ZipFile::fnvHash64(exactPath.data(), exactPath.size()));
  epub_production_test::storage.addFile(currentPath, {1});
  epub_production_test::storage.boundedPathCapacity = 64U;

  {
    Epub owning(std::string(exactPath), "/cache");
    ASSERT_TRUE(owning.clearCache());
  }
  const auto owningTrace =
      epub_production_test::storage.capturedPathOperations;
  epub_production_test::storage.reset();
  epub_production_test::storage.addFile(currentPath, {1});
  epub_production_test::storage.boundedPathCapacity = 64U;

  ASSERT_TRUE(
      Epub::clearCacheForFilePathNoPathAlloc(exactPath, "/cache"));

  EXPECT_EQ(mutationTrace(epub_production_test::storage.capturedPathOperations),
            mutationTrace(owningTrace));
  EXPECT_FALSE(epub_production_test::storage.files.contains(currentPath));
  EXPECT_TRUE(epub_production_test::storage.boundedPathsWereNulTerminated);
}

TEST_F(EpubLegacyHrefTest, NoPathAllocationCacheCleanupMigratesLegacyOnlyThenDeletes) {
  const std::string exactPath = "/Books/legacy.epub";
  const std::string currentPath =
      "/cache/epub_" +
      std::to_string(ZipFile::fnvHash64(exactPath.data(), exactPath.size()));
  const std::string legacyPath =
      "/cache/epub_" + std::to_string(std::hash<std::string>{}(exactPath));
  epub_production_test::storage.addFile(legacyPath, {2});
  epub_production_test::storage.boundedPathCapacity = 64U;

  {
    Epub owning(exactPath, "/cache");
    ASSERT_TRUE(owning.clearCache());
  }
  const auto owningTrace =
      epub_production_test::storage.capturedPathOperations;
  epub_production_test::storage.reset();
  epub_production_test::storage.addFile(legacyPath, {2});
  epub_production_test::storage.boundedPathCapacity = 64U;

  ASSERT_TRUE(Epub::clearCacheForFilePathNoPathAlloc(exactPath, "/cache"));

  EXPECT_EQ(mutationTrace(epub_production_test::storage.capturedPathOperations),
            mutationTrace(owningTrace));
  EXPECT_FALSE(epub_production_test::storage.files.contains(legacyPath));
  EXPECT_FALSE(epub_production_test::storage.files.contains(currentPath));
}

TEST_F(EpubLegacyHrefTest, NoPathAllocationCacheCleanupPreservesCurrentWinsSemantics) {
  const std::string exactPath = "/Books/legacy.epub";
  const std::string currentPath =
      "/cache/epub_" +
      std::to_string(ZipFile::fnvHash64(exactPath.data(), exactPath.size()));
  const std::string legacyPath =
      "/cache/epub_" + std::to_string(std::hash<std::string>{}(exactPath));
  epub_production_test::storage.addFile(currentPath, {1});
  epub_production_test::storage.addFile(legacyPath, {2});
  epub_production_test::storage.boundedPathCapacity = 64U;

  {
    Epub owning(exactPath, "/cache");
    ASSERT_TRUE(owning.clearCache());
  }
  const auto owningTrace =
      epub_production_test::storage.capturedPathOperations;
  epub_production_test::storage.reset();
  epub_production_test::storage.addFile(currentPath, {1});
  epub_production_test::storage.addFile(legacyPath, {2});
  epub_production_test::storage.boundedPathCapacity = 64U;

  EXPECT_FALSE(Epub::clearCacheForFilePathNoPathAlloc(exactPath, "/cache"));

  EXPECT_EQ(mutationTrace(epub_production_test::storage.capturedPathOperations),
            mutationTrace(owningTrace));
  EXPECT_FALSE(epub_production_test::storage.files.contains(currentPath));
  EXPECT_TRUE(epub_production_test::storage.files.contains(legacyPath));
}

TEST_F(EpubLegacyHrefTest, NoPathAllocationCacheCleanupPreservesRenameFailureSemantics) {
  const std::string exactPath = "/Books/legacy.epub";
  const std::string legacyPath =
      "/cache/epub_" + std::to_string(std::hash<std::string>{}(exactPath));
  epub_production_test::storage.addFile(legacyPath, {2});
  epub_production_test::storage.failRename = true;
  epub_production_test::storage.boundedPathCapacity = 64U;

  {
    Epub owning(exactPath, "/cache");
    EXPECT_TRUE(owning.clearCache());
  }
  const auto owningTrace =
      epub_production_test::storage.capturedPathOperations;
  epub_production_test::storage.reset();
  epub_production_test::storage.addFile(legacyPath, {2});
  epub_production_test::storage.failRename = true;
  epub_production_test::storage.boundedPathCapacity = 64U;

  EXPECT_FALSE(Epub::clearCacheForFilePathNoPathAlloc(exactPath, "/cache"));

  EXPECT_EQ(mutationTrace(epub_production_test::storage.capturedPathOperations),
            mutationTrace(owningTrace));
  EXPECT_TRUE(epub_production_test::storage.files.contains(legacyPath));
}

TEST_F(EpubLegacyHrefTest, NoPathAllocationCacheCleanupPreservesNeitherExistsTrace) {
  const std::string exactPath = "/Books/legacy.epub";
  epub_production_test::storage.boundedPathCapacity = 64U;
  {
    Epub owning(exactPath, "/cache");
    ASSERT_TRUE(owning.clearCache());
  }
  const auto owningTrace =
      epub_production_test::storage.capturedPathOperations;
  epub_production_test::storage.reset();
  epub_production_test::storage.boundedPathCapacity = 64U;

  ASSERT_TRUE(Epub::clearCacheForFilePathNoPathAlloc(exactPath, "/cache"));

  EXPECT_EQ(mutationTrace(epub_production_test::storage.capturedPathOperations),
            mutationTrace(owningTrace));
}

TEST_F(EpubLegacyHrefTest, NoPathAllocationCacheCleanupReportsRemoveFailure) {
  const std::string exactPath = "/Books/legacy.epub";
  const std::string currentPath =
      "/cache/epub_" +
      std::to_string(ZipFile::fnvHash64(exactPath.data(), exactPath.size()));
  epub_production_test::storage.addFile(currentPath, {1});
  epub_production_test::storage.failRemoveDir = true;

  EXPECT_FALSE(Epub::clearCacheForFilePathNoPathAlloc(exactPath, "/cache"));
  EXPECT_TRUE(epub_production_test::storage.files.contains(currentPath));
}

TEST_F(EpubLegacyHrefTest, NoPathAllocationCacheCleanupChecks64ByteBoundary) {
  const std::string exactPath = "/Books/legacy.epub";
  const std::string suffix =
      "/epub_" +
      std::to_string(ZipFile::fnvHash64(exactPath.data(), exactPath.size()));
  ASSERT_LT(suffix.size(), 63U);
  const std::string exactFitCacheDir(63U - suffix.size(), 'c');
  const std::string exactFitPath = exactFitCacheDir + suffix;
  epub_production_test::storage.addFile(exactFitPath, {1});
  epub_production_test::storage.boundedPathCapacity = 64U;

  EXPECT_TRUE(Epub::clearCacheForFilePathNoPathAlloc(
      exactPath, exactFitCacheDir.c_str()));
  EXPECT_TRUE(epub_production_test::storage.boundedPathsWereNulTerminated);

  epub_production_test::storage.reset();
  const std::string overflowCacheDir(64U - suffix.size(), 'c');
  EXPECT_FALSE(Epub::clearCacheForFilePathNoPathAlloc(
      exactPath, overflowCacheDir.c_str()));
  EXPECT_EQ(epub_production_test::storage.pathOperations, 0U);
}

TEST_F(EpubLegacyHrefTest,
       ProductionCacheFormatterBoundsFullUint64DecimalAndTerminates) {
  constexpr uint64_t kMaximumHash = UINT64_MAX;
  constexpr char kMaximumHashText[] = "18446744073709551615";
  constexpr size_t kSuffixBytes = sizeof("/epub_") - 1U +
                                  sizeof(kMaximumHashText) - 1U;
  const std::string exactFitCacheDir(63U - kSuffixBytes, 'c');
  const std::string expected =
      exactFitCacheDir + "/epub_" + kMaximumHashText;
  ASSERT_EQ(expected.size(), 63U);

  struct GuardedPath {
    char path[64];
    uint8_t canary;
  } exact{};
  exact.canary = 0xA5U;
  EXPECT_TRUE(Epub::formatCachePathForTest(
      exact.path, sizeof(exact.path), exactFitCacheDir.c_str(), kMaximumHash));
  EXPECT_EQ(std::string_view(exact.path), expected);
  EXPECT_EQ(exact.path[63], '\0');
  EXPECT_EQ(exact.canary, 0xA5U);

  GuardedPath overflow{};
  std::fill(std::begin(overflow.path), std::end(overflow.path), '\x7f');
  overflow.canary = 0x5AU;
  const std::string overflowCacheDir(64U - kSuffixBytes, 'c');
  EXPECT_FALSE(Epub::formatCachePathForTest(
      overflow.path, sizeof(overflow.path), overflowCacheDir.c_str(),
      kMaximumHash));
  EXPECT_EQ(overflow.path[63], '\0');
  EXPECT_EQ(overflow.canary, 0x5AU);
}

}  // namespace
