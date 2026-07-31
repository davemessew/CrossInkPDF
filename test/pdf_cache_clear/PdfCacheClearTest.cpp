#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

#include "BookCacheUtils.h"
#include "Memory.h"
#include "PdfSourceIdentity.h"
#include "TestStorage.h"

namespace {

class PdfCacheClearTest : public testing::Test {
 protected:
  void SetUp() override {
    root_ = std::filesystem::temp_directory_path() / "crossink-pdf-cache-clear-test";
    Storage.reset(root_);
    TestMemory::failNextAllocation = false;
    TestMemory::successfulAllocations = 0;
  }

  void TearDown() override {
    EXPECT_EQ(Storage.openHandleCount(), 0U);
    EXPECT_EQ(Storage.childWrapperCount(), 0U);
    std::error_code error;
    std::filesystem::remove_all(root_, error);
  }

  std::string cacheRootFor(const char* const sourcePath) {
    char cacheRoot[PDF_CACHE_PATH_CAPACITY]{};
    EXPECT_TRUE(pdfFormatCacheRoot("/.crosspoint", sourcePath, cacheRoot, sizeof(cacheRoot)));
    return cacheRoot;
  }

  void write(const std::string& virtualPath, const char* const contents = "x") {
    const auto physical = Storage.physicalPath(virtualPath.c_str());
    std::filesystem::create_directories(physical.parent_path());
    std::ofstream stream(physical, std::ios::binary);
    stream << contents;
  }

  void directory(const std::string& virtualPath) {
    std::filesystem::create_directories(Storage.physicalPath(virtualPath.c_str()));
  }

  std::filesystem::path root_;
};

TEST_F(PdfCacheClearTest, PdfPathUsesProductionIdentityAndClearsDerivedStateOnly) {
  constexpr char sourcePath[] = "/Books/Identity.PDF";
  const std::string cacheRoot = cacheRootFor(sourcePath);
  write(cacheRoot + "/manifest.a");
  write(cacheRoot + "/progress.a", "progress");

  ASSERT_TRUE(clearBookCachePreservingUserState(sourcePath));
  EXPECT_FALSE(Storage.exists((cacheRoot + "/manifest.a").c_str()));
  EXPECT_TRUE(Storage.exists((cacheRoot + "/progress.a").c_str()));
}

TEST_F(PdfCacheClearTest, ClearsExactDerivedAllowlistAndPreservesEveryUserStateName) {
  constexpr char sourcePath[] = "/Books/Allowlist.pdf";
  const std::string cacheRoot = cacheRootFor(sourcePath);
  const char* const derivedFiles[] = {
      "manifest.a",     "manifest.b",     "build.a",     "build.b",
      "manifest.a.tmp", "manifest.b.tmp", "build.a.tmp", "build.b.tmp",
  };
  for (const char* const name : derivedFiles) {
    write(cacheRoot + "/" + name);
  }
  directory(cacheRoot + "/gen_1/sections");
  write(cacheRoot + "/gen_1/sections/000001.xhtml");
  directory(cacheRoot + "/gen_4294967295/images");
  write(cacheRoot + "/gen_4294967295/images/000001.pxc");
  directory(cacheRoot + "/sections");
  write(cacheRoot + "/sections/orphan.xhtml");

  const char* const userFiles[] = {
      "progress.a",    "progress.b",     "reader_settings.bin", "stats.bin",
      "stats_v5.bin",  "saved_items.a",  "saved_items.b",       "unknown-user-state.bin",
      "manifest.user", "manifest.a.bak", "build.notes",         "progress.a.tmp",
  };
  for (const char* const name : userFiles) {
    write(cacheRoot + "/" + name, name);
  }
  const char* const userDirectories[] = {
      "gen_", "gen_0", "gen_01", "gen_-1", "gen_4294967296", "gen_notes", "reader-state",
  };
  for (const char* const name : userDirectories) {
    directory(cacheRoot + "/" + name);
    write(cacheRoot + "/" + name + "/keep.bin", name);
  }

  ASSERT_TRUE(clearBookCachePreservingUserState(sourcePath));

  for (const char* const name : derivedFiles) {
    EXPECT_FALSE(Storage.exists((cacheRoot + "/" + name).c_str())) << name;
  }
  EXPECT_FALSE(Storage.exists((cacheRoot + "/gen_1").c_str()));
  EXPECT_FALSE(Storage.exists((cacheRoot + "/gen_4294967295").c_str()));
  EXPECT_FALSE(Storage.exists((cacheRoot + "/sections").c_str()));
  for (const char* const name : userFiles) {
    EXPECT_TRUE(Storage.exists((cacheRoot + "/" + name).c_str())) << name;
  }
  for (const char* const name : userDirectories) {
    EXPECT_TRUE(Storage.exists((cacheRoot + "/" + name + "/keep.bin").c_str())) << name;
  }
  EXPECT_EQ(Storage.openCallCount(), 1U);
  EXPECT_EQ(Storage.removeCallCount(), std::size(derivedFiles));
  EXPECT_EQ(Storage.removeDirCallCount(), 3U);
  EXPECT_EQ(Storage.maxOpenHandleCount(), 2U);
  EXPECT_EQ(Storage.childWrapperAllocationCount(), 1U);
  EXPECT_EQ(Storage.maxChildWrapperCount(), 1U);
  EXPECT_EQ(TestMemory::successfulAllocations, 1U);
}

TEST_F(PdfCacheClearTest, RecognizesOnlyPdfPrefixedCacheDirectoryNames) {
  EXPECT_TRUE(isBookCacheDirectoryName("pdf_0"));
  EXPECT_TRUE(isBookCacheDirectoryName("pdf_18446744073709551615"));
  EXPECT_FALSE(isBookCacheDirectoryName("pdf"));
  EXPECT_FALSE(isBookCacheDirectoryName("pd_1"));
}

TEST_F(PdfCacheClearTest, FailsBeforeDeletionWhenDirectoryCannotBeOpened) {
  constexpr char sourcePath[] = "/Books/OpenFailure.pdf";
  const std::string cacheRoot = cacheRootFor(sourcePath);
  write(cacheRoot + "/manifest.a");
  write(cacheRoot + "/progress.a");
  directory(cacheRoot + "/gen_1");
  write(cacheRoot + "/gen_1/metadata.bin");
  Storage.failOpenOf(cacheRoot);

  EXPECT_FALSE(clearBookCachePreservingUserState(sourcePath));
  EXPECT_TRUE(Storage.exists((cacheRoot + "/manifest.a").c_str()));
  EXPECT_TRUE(Storage.exists((cacheRoot + "/gen_1/metadata.bin").c_str()));
  EXPECT_TRUE(Storage.exists((cacheRoot + "/progress.a").c_str()));
}

TEST_F(PdfCacheClearTest, ReturnsFalseOnAnyDeletionFailureWithoutTouchingUserState) {
  constexpr char sourcePath[] = "/Books/DeleteFailure.pdf";
  const std::string cacheRoot = cacheRootFor(sourcePath);
  write(cacheRoot + "/manifest.a");
  write(cacheRoot + "/manifest.b");
  write(cacheRoot + "/progress.a");
  Storage.failRemovalOf(cacheRoot + "/manifest.a");

  EXPECT_FALSE(clearBookCachePreservingUserState(sourcePath));
  EXPECT_TRUE(Storage.exists((cacheRoot + "/manifest.a").c_str()));
  EXPECT_TRUE(Storage.exists((cacheRoot + "/manifest.b").c_str()));
  EXPECT_TRUE(Storage.exists((cacheRoot + "/progress.a").c_str()));
}

TEST_F(PdfCacheClearTest, AllocationFailureFailsSafeBeforeDeletion) {
  constexpr char sourcePath[] = "/Books/AllocationFailure.pdf";
  const std::string cacheRoot = cacheRootFor(sourcePath);
  write(cacheRoot + "/manifest.a");
  write(cacheRoot + "/progress.a");
  TestMemory::failNextAllocation = true;

  EXPECT_FALSE(clearBookCachePreservingUserState(sourcePath));
  EXPECT_TRUE(Storage.exists((cacheRoot + "/manifest.a").c_str()));
  EXPECT_TRUE(Storage.exists((cacheRoot + "/progress.a").c_str()));
}

TEST_F(PdfCacheClearTest, ChildWrapperAllocationFailureFailsBeforeScanOrDeletion) {
  constexpr char sourcePath[] = "/Books/ChildAllocationFailure.pdf";
  const std::string cacheRoot = cacheRootFor(sourcePath);
  write(cacheRoot + "/manifest.a");
  directory(cacheRoot + "/gen_1");
  write(cacheRoot + "/progress.a");
  Storage.failNextChildWrapperAllocation();

  EXPECT_FALSE(clearBookCachePreservingUserState(sourcePath));
  EXPECT_TRUE(Storage.exists((cacheRoot + "/manifest.a").c_str()));
  EXPECT_TRUE(Storage.exists((cacheRoot + "/gen_1").c_str()));
  EXPECT_TRUE(Storage.exists((cacheRoot + "/progress.a").c_str()));
  EXPECT_EQ(Storage.removeCallCount(), 0U);
  EXPECT_EQ(Storage.removeDirCallCount(), 0U);
  EXPECT_EQ(Storage.childWrapperAllocationCount(), 0U);
}

TEST_F(PdfCacheClearTest, MidScanDirectoryReadFailurePerformsNoDeletion) {
  constexpr char sourcePath[] = "/Books/DirectoryReadFailure.pdf";
  const std::string cacheRoot = cacheRootFor(sourcePath);
  write(cacheRoot + "/manifest.a");
  directory(cacheRoot + "/gen_1");
  write(cacheRoot + "/progress.a");
  Storage.failDirectoryReadAfter(1);

  EXPECT_FALSE(clearBookCachePreservingUserState(sourcePath));
  EXPECT_TRUE(Storage.exists((cacheRoot + "/manifest.a").c_str()));
  EXPECT_TRUE(Storage.exists((cacheRoot + "/gen_1").c_str()));
  EXPECT_TRUE(Storage.exists((cacheRoot + "/progress.a").c_str()));
  EXPECT_EQ(Storage.removeCallCount(), 0U);
  EXPECT_EQ(Storage.removeDirCallCount(), 0U);
  EXPECT_EQ(Storage.childWrapperAllocationCount(), 1U);
  EXPECT_EQ(Storage.maxChildWrapperCount(), 1U);
}

TEST_F(PdfCacheClearTest, GenerationOverflowFailsBeforeAnyDeletion) {
  constexpr char sourcePath[] = "/Books/GenerationOverflow.pdf";
  const std::string cacheRoot = cacheRootFor(sourcePath);
  write(cacheRoot + "/manifest.a");
  write(cacheRoot + "/progress.a");
  for (uint32_t generation = 1; generation <= 33; ++generation) {
    directory(cacheRoot + "/gen_" + std::to_string(generation));
  }

  EXPECT_FALSE(clearBookCachePreservingUserState(sourcePath));
  EXPECT_TRUE(Storage.exists((cacheRoot + "/manifest.a").c_str()));
  EXPECT_TRUE(Storage.exists((cacheRoot + "/gen_1").c_str()));
  EXPECT_TRUE(Storage.exists((cacheRoot + "/gen_33").c_str()));
  EXPECT_TRUE(Storage.exists((cacheRoot + "/progress.a").c_str()));
}

TEST_F(PdfCacheClearTest, ReturnsFalseWhenGenerationRemovalFails) {
  constexpr char sourcePath[] = "/Books/GenerationDeleteFailure.pdf";
  const std::string cacheRoot = cacheRootFor(sourcePath);
  directory(cacheRoot + "/gen_7");
  write(cacheRoot + "/gen_7/sections.bin");
  write(cacheRoot + "/progress.a");
  Storage.failRemovalOf(cacheRoot + "/gen_7");

  EXPECT_FALSE(clearBookCachePreservingUserState(sourcePath));
  EXPECT_TRUE(Storage.exists((cacheRoot + "/gen_7/sections.bin").c_str()));
  EXPECT_TRUE(Storage.exists((cacheRoot + "/progress.a").c_str()));
}

TEST_F(PdfCacheClearTest, MissingPdfCacheIsAlreadyClear) {
  TestMemory::failNextAllocation = true;
  EXPECT_TRUE(clearBookCachePreservingUserState("/Books/NotPrepared.pdf"));
  EXPECT_TRUE(TestMemory::failNextAllocation);
  TestMemory::failNextAllocation = false;
  EXPECT_EQ(TestMemory::successfulAllocations, 0U);
  EXPECT_EQ(Storage.maxOpenHandleCount(), 0U);
  EXPECT_EQ(Storage.childWrapperAllocationCount(), 0U);
}

TEST_F(PdfCacheClearTest, DirectoryBasedClearUsesTheSamePdfSelectivePolicy) {
  const std::string cacheRoot = cacheRootFor("/Books/SettingsClear.pdf");
  write(cacheRoot + "/manifest.a");
  directory(cacheRoot + "/gen_4/sections");
  write(cacheRoot + "/gen_4/sections/000001.xhtml");
  write(cacheRoot + "/progress.a", "progress");
  write(cacheRoot + "/reader_settings.bin", "settings");
  write(cacheRoot + "/stats_v5.bin", "stats");
  write(cacheRoot + "/future-user-state.bin", "future");

  ASSERT_TRUE(clearBookCacheDirectoryPreservingStats(cacheRoot + "/"));
  EXPECT_FALSE(Storage.exists((cacheRoot + "/manifest.a").c_str()));
  EXPECT_FALSE(Storage.exists((cacheRoot + "/gen_4").c_str()));
  EXPECT_TRUE(Storage.exists((cacheRoot + "/progress.a").c_str()));
  EXPECT_TRUE(Storage.exists((cacheRoot + "/reader_settings.bin").c_str()));
  EXPECT_TRUE(Storage.exists((cacheRoot + "/stats_v5.bin").c_str()));
  EXPECT_TRUE(Storage.exists((cacheRoot + "/future-user-state.bin").c_str()));
}

TEST_F(PdfCacheClearTest, LegacyBookCacheRoutesKeepTheirExistingPreservationRules) {
  const std::string epubPath = "/Books/Legacy.epub";
  const std::string epubCache = "/.crosspoint/epub_stub_" + epubPath;
  write(epubCache + "/book.bin");
  write(epubCache + "/progress.bin", "epub-progress");
  write(epubCache + "/reader_settings.bin", "epub-settings");
  write(epubCache + "/stats_v5.bin", "epub-stats");
  ASSERT_TRUE(clearBookCachePreservingUserState(epubPath));
  EXPECT_FALSE(Storage.exists((epubCache + "/book.bin").c_str()));
  EXPECT_TRUE(Storage.exists((epubCache + "/progress.bin").c_str()));
  EXPECT_TRUE(Storage.exists((epubCache + "/reader_settings.bin").c_str()));
  EXPECT_TRUE(Storage.exists((epubCache + "/stats_v5.bin").c_str()));

  const std::string xtcPath = "/Books/Legacy.xtc";
  const std::string xtcCache = "/.crosspoint/xtc_stub_" + xtcPath;
  write(xtcCache + "/index.bin");
  write(xtcCache + "/progress.bin", "xtc-progress");
  write(xtcCache + "/stats_v5.bin", "xtc-stats");
  ASSERT_TRUE(clearBookCachePreservingUserState(xtcPath));
  EXPECT_FALSE(Storage.exists((xtcCache + "/index.bin").c_str()));
  EXPECT_TRUE(Storage.exists((xtcCache + "/progress.bin").c_str()));
  EXPECT_TRUE(Storage.exists((xtcCache + "/stats_v5.bin").c_str()));

  const std::string txtPath = "/Books/Legacy.txt";
  const std::string txtCache = "/.crosspoint/txt_stub_" + txtPath;
  write(txtCache + "/index.bin");
  write(txtCache + "/progress.bin", "txt-progress");
  ASSERT_TRUE(clearBookCachePreservingUserState(txtPath));
  EXPECT_FALSE(Storage.exists((txtCache + "/index.bin").c_str()));
  EXPECT_TRUE(Storage.exists((txtCache + "/progress.bin").c_str()));

  const std::string directoryCache = "/.crosspoint/epub_directory_clear";
  write(directoryCache + "/book.bin");
  write(directoryCache + "/progress.bin");
  write(directoryCache + "/stats_v5.bin");
  ASSERT_TRUE(clearBookCacheDirectoryPreservingStats(directoryCache));
  EXPECT_FALSE(Storage.exists((directoryCache + "/book.bin").c_str()));
  EXPECT_FALSE(Storage.exists((directoryCache + "/progress.bin").c_str()));
  EXPECT_TRUE(Storage.exists((directoryCache + "/stats_v5.bin").c_str()));

  EXPECT_TRUE(isBookCacheDirectoryName("epub_1"));
  EXPECT_TRUE(isBookCacheDirectoryName("xtc_1"));
  EXPECT_TRUE(isBookCacheDirectoryName("txt_1"));
}

}  // namespace
