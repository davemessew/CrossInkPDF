#include <HalStorage.h>
#include <gtest/gtest.h>
#include <uzlib.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <new>
#include <string>
#include <vector>

#include "BookmarkStore.h"
#include "ClippingStore.h"
#include "PdfSavedItemsSession.h"
#include "PdfTestCacheIo.h"

namespace {
thread_local bool allocationWatchActive = false;
thread_local size_t watchedAllocationCount = 0;
thread_local size_t forcedNothrowAllocationFailures = 0;
}  // namespace

void* operator new(const std::size_t size) {
  if (allocationWatchActive) ++watchedAllocationCount;
  if (void* const memory = std::malloc(size)) return memory;
  throw std::bad_alloc();
}

void* operator new[](const std::size_t size) {
  if (allocationWatchActive) ++watchedAllocationCount;
  if (void* const memory = std::malloc(size)) return memory;
  throw std::bad_alloc();
}

void* operator new(const std::size_t size, const std::nothrow_t&) noexcept {
  if (forcedNothrowAllocationFailures != 0) {
    --forcedNothrowAllocationFailures;
    return nullptr;
  }
  if (allocationWatchActive) ++watchedAllocationCount;
  return std::malloc(size);
}

void* operator new[](const std::size_t size, const std::nothrow_t&) noexcept {
  if (forcedNothrowAllocationFailures != 0) {
    --forcedNothrowAllocationFailures;
    return nullptr;
  }
  if (allocationWatchActive) ++watchedAllocationCount;
  return std::malloc(size);
}

void operator delete(void* const memory) noexcept { std::free(memory); }
void operator delete[](void* const memory) noexcept { std::free(memory); }
void operator delete(void* const memory, std::size_t) noexcept { std::free(memory); }
void operator delete[](void* const memory, std::size_t) noexcept { std::free(memory); }
void operator delete(void* const memory, const std::nothrow_t&) noexcept { std::free(memory); }
void operator delete[](void* const memory, const std::nothrow_t&) noexcept { std::free(memory); }

namespace {

template <typename T>
void appendPod(std::vector<uint8_t>* const output, const T& value) {
  const auto* const bytes = reinterpret_cast<const uint8_t*>(&value);
  output->insert(output->end(), bytes, bytes + sizeof(value));
}

void appendString(std::vector<uint8_t>* const output, const std::string& value) {
  appendPod(output, static_cast<uint32_t>(value.size()));
  output->insert(output->end(), value.begin(), value.end());
}

template <size_t Capacity>
void appendFixedString(std::vector<uint8_t>* const output, const char* const value) {
  std::array<uint8_t, Capacity> bytes{};
  std::snprintf(reinterpret_cast<char*>(bytes.data()), bytes.size(), "%s", value);
  output->insert(output->end(), bytes.begin(), bytes.end());
}

const std::vector<uint8_t>& onlyFileIn(const char* const directory) {
  const std::string prefix = std::string(directory) + "/";
  const auto& files = Storage.files();
  const auto found =
      std::find_if(files.begin(), files.end(), [&](const auto& entry) { return entry.first.rfind(prefix, 0) == 0; });
  EXPECT_NE(found, files.end());
  return found->second;
}

std::string canonicalFilePathIn(const char* const directory) {
  const std::string prefix = std::string(directory) + "/";
  const auto& files = Storage.files();
  const auto found = std::find_if(files.begin(), files.end(), [&](const auto& entry) {
    return entry.first.rfind(prefix, 0) == 0 && !entry.first.ends_with(".tmp") && !entry.first.ends_with(".bak");
  });
  EXPECT_NE(found, files.end());
  return found == files.end() ? std::string{} : found->first;
}

class LegacyStoreTest : public ::testing::Test {
 protected:
  void SetUp() override {
    forcedNothrowAllocationFailures = 0;
    Storage.reset();
  }

  void TearDown() override { forcedNothrowAllocationFailures = 0; }
};

std::string pdfCompatibilityPath(const char* const directory, const char* const bookPath) {
  const uint32_t crc = uzlib_crc32(bookPath, static_cast<unsigned int>(std::strlen(bookPath)), 0);
  return std::string(directory) + "/pdf_" + std::to_string(crc) + ".bin";
}

std::string pdfBookmarkLegacyCompatibilityPath(const char* const bookPath) {
  return std::string("/.crosspoint/bookmarks/pdf_") + std::to_string(std::hash<std::string>{}(bookPath)) + ".bin";
}

void seedPdfBookmarkArtifacts(BookmarkStore* const store, std::string* const canonicalPath,
                              const char* const bookPath = "/books/book.pdf") {
  ASSERT_NE(store, nullptr);
  ASSERT_NE(canonicalPath, nullptr);
  ASSERT_TRUE(store->loadForBook(bookPath, "Title", "Author", "pdf"));
  ASSERT_EQ(store->addPdfBookmark(2, 0.5F, "Chapter", 11, "old"), BookmarkStore::AddResult::Added);
  *canonicalPath = pdfCompatibilityPath("/.crosspoint/bookmarks", bookPath);
  ASSERT_TRUE(Storage.exists(canonicalPath->c_str()));
  Storage.fail(TestStorageFault::Remove);
  ASSERT_EQ(store->addPdfBookmark(3, 0.75F, "Chapter", 12, "new"), BookmarkStore::AddResult::Added);
  Storage.clearFault();
  ASSERT_TRUE(Storage.exists((*canonicalPath + ".bak").c_str()));
  Storage.addFile(*canonicalPath + ".tmp", {0xaa});
}

void seedPdfBookmarkLegacyOnly(const char* const bookPath, std::string* const currentPath,
                               std::string* const legacyPath, std::vector<uint8_t>* const legacyBytes) {
  ASSERT_NE(currentPath, nullptr);
  ASSERT_NE(legacyPath, nullptr);
  ASSERT_NE(legacyBytes, nullptr);
  BookmarkStore source;
  ASSERT_TRUE(source.loadForBook(bookPath, "Title", "Author", "pdf"));
  ASSERT_EQ(source.addPdfBookmark(2, 0.5F, "Chapter", 11, "legacy"), BookmarkStore::AddResult::Added);
  *currentPath = pdfCompatibilityPath("/.crosspoint/bookmarks", bookPath);
  *legacyPath = pdfBookmarkLegacyCompatibilityPath(bookPath);
  ASSERT_NE(*currentPath, *legacyPath);
  ASSERT_TRUE(Storage.rename(currentPath->c_str(), legacyPath->c_str()));
  *legacyBytes = Storage.files().at(*legacyPath);
}

void seedPdfBookmarkCurrentAndLegacyArtifacts(const char* const bookPath, std::string* const currentPath,
                                              std::string* const legacyPath) {
  ASSERT_NE(currentPath, nullptr);
  ASSERT_NE(legacyPath, nullptr);
  BookmarkStore legacySource;
  ASSERT_TRUE(legacySource.loadForBook(bookPath, "Title", "Author", "pdf"));
  ASSERT_EQ(legacySource.addPdfBookmark(3, 0.75F, "Chapter", 12, "legacy"), BookmarkStore::AddResult::Added);
  const std::string generatedPath = pdfCompatibilityPath("/.crosspoint/bookmarks", bookPath);
  const std::vector<uint8_t> legacyBytes = Storage.files().at(generatedPath);

  Storage.reset();
  BookmarkStore currentSource;
  ASSERT_TRUE(currentSource.loadForBook(bookPath, "Title", "Author", "pdf"));
  ASSERT_EQ(currentSource.addPdfBookmark(2, 0.5F, "Chapter", 11, "current"), BookmarkStore::AddResult::Added);
  *currentPath = generatedPath;
  *legacyPath = pdfBookmarkLegacyCompatibilityPath(bookPath);
  ASSERT_NE(*currentPath, *legacyPath);
  Storage.addFile(*legacyPath, legacyBytes);
  Storage.addFile(*currentPath + ".tmp", {0xaa});
  Storage.addFile(*currentPath + ".bak", Storage.files().at(*currentPath));
  Storage.addFile(*legacyPath + ".tmp", {0xbb});
  Storage.addFile(*legacyPath + ".bak", legacyBytes);
}

void seedPdfClippingArtifacts(ClippingStore* const store, std::string* const canonicalPath,
                              const char* const bookPath = "/books/book.pdf") {
  ASSERT_NE(store, nullptr);
  ASSERT_NE(canonicalPath, nullptr);
  ASSERT_TRUE(store->loadForBook(bookPath, "Title", "Author", "pdf"));
  ASSERT_EQ(store->addPdfClipping(1, 0, 0, 1, 0, 1, 2, "Chapter", 21, "old"), ClippingStore::AddResult::Added);
  *canonicalPath = pdfCompatibilityPath("/.crosspoint/clippings", bookPath);
  ASSERT_TRUE(Storage.exists(canonicalPath->c_str()));
  Storage.fail(TestStorageFault::Remove);
  ASSERT_EQ(store->addPdfClipping(2, 1, 1, 2, 2, 3, 4, "Chapter", 22, "new"), ClippingStore::AddResult::Added);
  Storage.clearFault();
  ASSERT_TRUE(Storage.exists((*canonicalPath + ".bak").c_str()));
  Storage.addFile(*canonicalPath + ".tmp", {0xaa});
}

TEST_F(LegacyStoreTest, EpubBookmarkV5WriterBytesRemainUnchanged) {
  BookmarkStore store;
  ASSERT_TRUE(store.loadForBook("/books/book.epub", "Title", "Author", "epub"));
  ASSERT_EQ(store.addBookmark(3, 0.25F, 4, "Chapter", 7, "Snippet"), BookmarkStore::AddResult::Added);

  std::vector<uint8_t> expected;
  appendPod(&expected, static_cast<uint8_t>(5));
  appendPod(&expected, static_cast<uint16_t>(1));
  appendString(&expected, "Title");
  appendString(&expected, "Author");
  appendString(&expected, "/books/book.epub");
  appendPod(&expected, static_cast<uint16_t>(3));
  appendPod(&expected, 0.25F);
  appendPod(&expected, static_cast<uint32_t>(0));
  appendFixedString<48>(&expected, "Chapter");
  appendPod(&expected, static_cast<uint16_t>(7));
  appendFixedString<64>(&expected, "Snippet");

  EXPECT_EQ(onlyFileIn("/.crosspoint/bookmarks"), expected);
}

TEST_F(LegacyStoreTest, EpubClippingV1WriterBytesRemainUnchanged) {
  ClippingStore store;
  ASSERT_TRUE(store.loadForBook("/books/book.epub", "Title", "Author", "epub"));
  ASSERT_EQ(store.addClipping(3, 1, 2, 4, 5, 8, 12, "Chapter", 7, "selected text"), ClippingStore::AddResult::Added);

  std::vector<uint8_t> expected;
  appendPod(&expected, static_cast<uint8_t>(1));
  appendPod(&expected, static_cast<uint16_t>(1));
  appendString(&expected, "Title");
  appendString(&expected, "Author");
  appendString(&expected, "/books/book.epub");
  appendPod(&expected, static_cast<uint16_t>(3));
  appendPod(&expected, static_cast<uint16_t>(1));
  appendPod(&expected, static_cast<uint16_t>(2));
  appendPod(&expected, static_cast<uint16_t>(4));
  appendPod(&expected, static_cast<uint16_t>(5));
  appendPod(&expected, static_cast<uint16_t>(8));
  appendPod(&expected, static_cast<uint16_t>(12));
  appendPod(&expected, static_cast<uint16_t>(7));
  appendPod(&expected, static_cast<uint32_t>(123));
  appendFixedString<48>(&expected, "Chapter");
  appendString(&expected, "selected text");

  EXPECT_EQ(onlyFileIn("/.crosspoint/clippings"), expected);
}

TEST_F(LegacyStoreTest, PdfSpecificMutationsCannotModifyLoadedEpubStores) {
  BookmarkStore bookmarks;
  ASSERT_TRUE(bookmarks.loadForBook("/books/book.epub", "Title", "Author", "epub"));
  ASSERT_EQ(bookmarks.addBookmark(3, 0.25F, 4, "Chapter", 7, "Snippet"), BookmarkStore::AddResult::Added);
  const std::vector<uint8_t> bookmarkBytes = onlyFileIn("/.crosspoint/bookmarks");

  ClippingStore clippings;
  ASSERT_TRUE(clippings.loadForBook("/books/book.epub", "Title", "Author", "epub"));
  ASSERT_EQ(clippings.addClipping(3, 1, 2, 4, 5, 8, 12, "Chapter", 7, "selected text"),
            ClippingStore::AddResult::Added);
  const std::vector<uint8_t> clippingBytes = onlyFileIn("/.crosspoint/clippings");

  EXPECT_EQ(bookmarks.addPdfBookmark(4, 0.5F, "PDF", 8, "PDF"), BookmarkStore::AddResult::SaveFailed);
  EXPECT_FALSE(bookmarks.removePdfBookmark(7));
  EXPECT_FALSE(bookmarks.clearPdfBookmarks());
  EXPECT_FALSE(bookmarks.reloadPdfFromDisk());
  EXPECT_EQ(bookmarks.getBookmarks().size(), 1U);
  EXPECT_EQ(onlyFileIn("/.crosspoint/bookmarks"), bookmarkBytes);

  EXPECT_EQ(clippings.addPdfClipping(4, 2, 2, 4, 9, 10, 12, "PDF", 8, "PDF"), ClippingStore::AddResult::SaveFailed);
  EXPECT_FALSE(clippings.removePdfClipping(7));
  EXPECT_FALSE(clippings.clearPdfClippings());
  EXPECT_FALSE(clippings.reloadPdfFromDisk());
  EXPECT_EQ(clippings.getClippings().size(), 1U);
  EXPECT_EQ(onlyFileIn("/.crosspoint/clippings"), clippingBytes);
}

TEST_F(LegacyStoreTest, PdfPathsAreAcceptedByBothStores) {
  BookmarkStore bookmarks;
  ClippingStore clippings;
  EXPECT_TRUE(bookmarks.loadForBook("/books/book.pdf", "Title", "Author", "pdf"));
  EXPECT_TRUE(clippings.loadForBook("/books/book.pdf", "Title", "Author", "pdf"));
}

TEST_F(LegacyStoreTest, PdfBookmarkStableIdsDoNotEraseAnotherBookmarkOnTheSamePage) {
  BookmarkStore store;
  ASSERT_TRUE(store.loadForBook("/books/book.pdf", "Title", "Author", "pdf"));
  ASSERT_EQ(store.addPdfBookmark(2, 0.5F, "Chapter", 11, "first"), BookmarkStore::AddResult::Added);
  ASSERT_EQ(store.addPdfBookmark(2, 0.5F, "Chapter", 12, "second"), BookmarkStore::AddResult::Added);

  ASSERT_EQ(store.getBookmarks().size(), 2U);
  EXPECT_EQ(store.getBookmarks()[0].paragraphIndex, 11);
  EXPECT_EQ(store.getBookmarks()[1].paragraphIndex, 12);
}

TEST_F(LegacyStoreTest, PdfBookmarkPathMigrationRetainsDistinctSamePageIds) {
  {
    BookmarkStore source;
    ASSERT_TRUE(source.loadForBook("/books/old.pdf", "Title", "Author", "pdf"));
    ASSERT_EQ(source.addPdfBookmark(2, 0.5F, "Chapter", 11, "first"), BookmarkStore::AddResult::Added);
    ASSERT_EQ(source.addPdfBookmark(2, 0.5F, "Chapter", 12, "second"), BookmarkStore::AddResult::Added);
  }

  ASSERT_TRUE(BookmarkStore::migrateForFilePath("/books/old.pdf", "/books/new.pdf", "Title", "Author", "pdf"));
  BookmarkStore moved;
  ASSERT_TRUE(moved.loadForBook("/books/new.pdf", "Title", "Author", "pdf"));
  ASSERT_EQ(moved.getBookmarks().size(), 2U);
  EXPECT_EQ(moved.getBookmarks()[0].paragraphIndex, 11);
  EXPECT_EQ(moved.getBookmarks()[1].paragraphIndex, 12);
  EXPECT_LE(Storage.maximumOpenHandles(), 1U);
}

TEST_F(LegacyStoreTest, PdfBookmarkLegacyCompatibilityMigrationUsesCheckedTransaction) {
  constexpr char bookPath[] = "/books/legacy-only.pdf";
  BookmarkStore source;
  ASSERT_TRUE(source.loadForBook(bookPath, "Title", "Author", "pdf"));
  ASSERT_EQ(source.addPdfBookmark(2, 0.5F, "Chapter", 11, "legacy"), BookmarkStore::AddResult::Added);

  const std::string currentPath = pdfCompatibilityPath("/.crosspoint/bookmarks", bookPath);
  const std::string legacyPath = pdfBookmarkLegacyCompatibilityPath(bookPath);
  ASSERT_NE(currentPath, legacyPath);
  ASSERT_TRUE(Storage.rename(currentPath.c_str(), legacyPath.c_str()));
  const std::vector<uint8_t> legacyBytes = Storage.files().at(legacyPath);

  Storage.fail(TestStorageFault::Flush);
  BookmarkStore interrupted;
  EXPECT_FALSE(interrupted.loadForBook(bookPath, "Title", "Author", "pdf"));
  Storage.clearFault();

  EXPECT_TRUE(Storage.exists(legacyPath.c_str()));
  EXPECT_EQ(Storage.files().at(legacyPath), legacyBytes);
  EXPECT_FALSE(Storage.exists(currentPath.c_str()));

  BookmarkStore rebooted;
  ASSERT_TRUE(rebooted.loadForBook(bookPath, "Title", "Author", "pdf"));
  ASSERT_EQ(rebooted.getBookmarks().size(), 1U);
  EXPECT_EQ(rebooted.getBookmarks()[0].paragraphIndex, 11);
  EXPECT_TRUE(Storage.exists(currentPath.c_str()));
  EXPECT_FALSE(Storage.exists(legacyPath.c_str()));
  EXPECT_LE(Storage.maximumOpenHandles(), 1U);
}

TEST_F(LegacyStoreTest, PdfBookmarkLegacyCompatibilityMigrationFaultsNeverLoseOnlySource) {
  struct FaultCase {
    TestStorageFault fault;
    uint16_t occurrence;
  };
  constexpr std::array<FaultCase, 10> faults{{
      {TestStorageFault::Read, 1},
      {TestStorageFault::Read, 15},
      {TestStorageFault::Write, 1},
      {TestStorageFault::Flush, 1},
      {TestStorageFault::Sync, 1},
      {TestStorageFault::Close, 1},
      {TestStorageFault::Close, 2},
      {TestStorageFault::Close, 3},
      {TestStorageFault::Rename, 1},
      {TestStorageFault::Remove, 1},
  }};

  for (const FaultCase fault : faults) {
    SCOPED_TRACE(static_cast<int>(fault.fault));
    SCOPED_TRACE(fault.occurrence);
    Storage.reset();
    constexpr char bookPath[] = "/books/legacy-fault.pdf";
    std::string currentPath;
    std::string legacyPath;
    std::vector<uint8_t> legacyBytes;
    seedPdfBookmarkLegacyOnly(bookPath, &currentPath, &legacyPath, &legacyBytes);

    Storage.fail(fault.fault, fault.occurrence);
    BookmarkStore interrupted;
    EXPECT_FALSE(interrupted.loadForBook(bookPath, "Title", "Author", "pdf"));
    Storage.clearFault();
    ASSERT_TRUE(Storage.exists(legacyPath.c_str()));
    EXPECT_EQ(Storage.files().at(legacyPath), legacyBytes);
    EXPECT_TRUE(interrupted.getBookmarks().empty());

    BookmarkStore rebooted;
    ASSERT_TRUE(rebooted.loadForBook(bookPath, "Title", "Author", "pdf"));
    ASSERT_EQ(rebooted.getBookmarks().size(), 1U);
    EXPECT_EQ(rebooted.getBookmarks()[0].paragraphIndex, 11);
    EXPECT_TRUE(Storage.exists(currentPath.c_str()));
    EXPECT_FALSE(Storage.exists(legacyPath.c_str()));
    EXPECT_FALSE(Storage.exists((currentPath + ".tmp").c_str()));
    EXPECT_FALSE(Storage.exists((currentPath + ".bak").c_str()));
    EXPECT_LE(Storage.maximumOpenHandles(), 1U);
  }
}

TEST_F(LegacyStoreTest, PdfBookmarkLoadRecoversCurrentAndLegacyArtifactsBeforeMerge) {
  for (uint16_t occurrence = 1; occurrence <= 4; ++occurrence) {
    SCOPED_TRACE(occurrence);
    Storage.reset();
    constexpr char bookPath[] = "/books/legacy-artifacts.pdf";
    std::string currentPath;
    std::string legacyPath;
    seedPdfBookmarkCurrentAndLegacyArtifacts(bookPath, &currentPath, &legacyPath);

    Storage.fail(TestStorageFault::Remove, occurrence);
    BookmarkStore interrupted;
    EXPECT_FALSE(interrupted.loadForBook(bookPath, "Title", "Author", "pdf"));
    Storage.clearFault();
    EXPECT_TRUE(interrupted.getBookmarks().empty());
    EXPECT_TRUE(Storage.exists(currentPath.c_str()));
    EXPECT_TRUE(Storage.exists(legacyPath.c_str()));

    BookmarkStore rebooted;
    ASSERT_TRUE(rebooted.loadForBook(bookPath, "Title", "Author", "pdf"));
    ASSERT_EQ(rebooted.getBookmarks().size(), 2U);
    EXPECT_EQ(rebooted.getBookmarks()[0].paragraphIndex, 11);
    EXPECT_EQ(rebooted.getBookmarks()[1].paragraphIndex, 12);
    EXPECT_FALSE(Storage.exists(legacyPath.c_str()));
    EXPECT_FALSE(Storage.exists((currentPath + ".tmp").c_str()));
    EXPECT_FALSE(Storage.exists((currentPath + ".bak").c_str()));
    EXPECT_FALSE(Storage.exists((legacyPath + ".tmp").c_str()));
    EXPECT_FALSE(Storage.exists((legacyPath + ".bak").c_str()));
    EXPECT_LE(Storage.maximumOpenHandles(), 1U);
  }
}

TEST_F(LegacyStoreTest, PdfBookmarkCurrentAndLegacyMergeFaultsRetainARecoverableGoodCopy) {
  struct FaultCase {
    TestStorageFault fault;
    uint16_t occurrence;
    bool committed;
  };
  constexpr std::array<FaultCase, 4> faults{{
      {TestStorageFault::Rename, 1, false},
      {TestStorageFault::Rename, 2, false},
      {TestStorageFault::Remove, 1, true},
      {TestStorageFault::Remove, 2, false},
  }};

  for (const FaultCase fault : faults) {
    SCOPED_TRACE(static_cast<int>(fault.fault));
    SCOPED_TRACE(fault.occurrence);
    Storage.reset();
    constexpr char bookPath[] = "/books/current-and-legacy-fault.pdf";
    std::string currentPath;
    std::string legacyPath;
    seedPdfBookmarkCurrentAndLegacyArtifacts(bookPath, &currentPath, &legacyPath);
    ASSERT_TRUE(Storage.remove((currentPath + ".tmp").c_str()));
    ASSERT_TRUE(Storage.remove((currentPath + ".bak").c_str()));
    ASSERT_TRUE(Storage.remove((legacyPath + ".tmp").c_str()));
    ASSERT_TRUE(Storage.remove((legacyPath + ".bak").c_str()));

    Storage.fail(fault.fault, fault.occurrence);
    BookmarkStore interrupted;
    EXPECT_EQ(interrupted.loadForBook(bookPath, "Title", "Author", "pdf"), fault.committed);
    Storage.clearFault();
    if (fault.committed) {
      ASSERT_EQ(interrupted.getBookmarks().size(), 2U);
    } else {
      EXPECT_TRUE(interrupted.getBookmarks().empty());
      EXPECT_TRUE(Storage.exists(legacyPath.c_str()));
      EXPECT_TRUE(Storage.exists(currentPath.c_str()) || Storage.exists((currentPath + ".bak").c_str()));
    }

    BookmarkStore rebooted;
    ASSERT_TRUE(rebooted.loadForBook(bookPath, "Title", "Author", "pdf"));
    ASSERT_EQ(rebooted.getBookmarks().size(), 2U);
    EXPECT_EQ(rebooted.getBookmarks()[0].paragraphIndex, 11);
    EXPECT_EQ(rebooted.getBookmarks()[1].paragraphIndex, 12);
    EXPECT_FALSE(Storage.exists(legacyPath.c_str()));
    EXPECT_FALSE(Storage.exists((currentPath + ".tmp").c_str()));
    EXPECT_FALSE(Storage.exists((currentPath + ".bak").c_str()));
    EXPECT_LE(Storage.maximumOpenHandles(), 1U);
  }
}

TEST_F(LegacyStoreTest, PdfBookmarkMigrationMergesDestinationByStableIdRatherThanPageSlice) {
  BookmarkStore source;
  ASSERT_TRUE(source.loadForBook("/books/old.pdf", "Title", "Author", "pdf"));
  ASSERT_EQ(source.addPdfBookmark(2, 0.5F, "Chapter", 11, "source"), BookmarkStore::AddResult::Added);

  BookmarkStore destination;
  ASSERT_TRUE(destination.loadForBook("/books/new.pdf", "Title", "Author", "pdf"));
  ASSERT_EQ(destination.addPdfBookmark(2, 0.5F, "Chapter", 12, "destination"), BookmarkStore::AddResult::Added);

  ASSERT_TRUE(BookmarkStore::migrateForFilePath("/books/old.pdf", "/books/new.pdf", "Title", "Author", "pdf"));
  BookmarkStore moved;
  ASSERT_TRUE(moved.loadForBook("/books/new.pdf", "Title", "Author", "pdf"));
  ASSERT_EQ(moved.getBookmarks().size(), 2U);
  EXPECT_EQ(moved.getBookmarks()[0].paragraphIndex, 12);
  EXPECT_EQ(moved.getBookmarks()[1].paragraphIndex, 11);
}

TEST_F(LegacyStoreTest, PdfBookmarkLoadRejectsARealCrcCollisionWithoutRewriting) {
  constexpr char firstBook[] = "/books/URYGSiQsIopJ.pdf";
  constexpr char collidingBook[] = "/books/t5SWIj04yLzY.pdf";
  constexpr uint32_t expectedCrc = 3724412502U;
  ASSERT_EQ(uzlib_crc32(firstBook, sizeof(firstBook) - 1U, 0), expectedCrc);
  ASSERT_EQ(uzlib_crc32(collidingBook, sizeof(collidingBook) - 1U, 0), expectedCrc);

  BookmarkStore first;
  ASSERT_TRUE(first.loadForBook(firstBook, "First", "Author", "pdf"));
  ASSERT_EQ(first.addPdfBookmark(2, 0.5F, "Chapter", 11, "first"), BookmarkStore::AddResult::Added);
  const std::string sharedPath = pdfCompatibilityPath("/.crosspoint/bookmarks", firstBook);
  const std::vector<uint8_t> originalBytes = Storage.files().at(sharedPath);

  BookmarkStore collision;
  EXPECT_FALSE(collision.loadForBook(collidingBook, "Second", "Author", "pdf"));
  EXPECT_TRUE(collision.getBookmarks().empty());
  EXPECT_EQ(Storage.files().at(sharedPath), originalBytes);

  BookmarkStore rebootedOriginal;
  ASSERT_TRUE(rebootedOriginal.loadForBook(firstBook, "First", "Author", "pdf"));
  ASSERT_EQ(rebootedOriginal.getBookmarks().size(), 1U);
  EXPECT_EQ(rebootedOriginal.getBookmarks()[0].paragraphIndex, 11);
  EXPECT_LE(Storage.maximumOpenHandles(), 1U);
}

TEST_F(LegacyStoreTest, PdfBookmarkMigrationRetagsOneSharedCrcCanonicalWithoutDeletingIt) {
  constexpr char oldPath[] = "/books/URYGSiQsIopJ.pdf";
  constexpr char newPath[] = "/books/t5SWIj04yLzY.pdf";
  BookmarkStore source;
  ASSERT_TRUE(source.loadForBook(oldPath, "First", "Author", "pdf"));
  ASSERT_EQ(source.addPdfBookmark(2, 0.5F, "Chapter", 11, "first"), BookmarkStore::AddResult::Added);
  const std::string sharedPath = pdfCompatibilityPath("/.crosspoint/bookmarks", oldPath);
  ASSERT_EQ(sharedPath, pdfCompatibilityPath("/.crosspoint/bookmarks", newPath));

  ASSERT_TRUE(BookmarkStore::migrateForFilePath(oldPath, newPath, "Second", "Author", "pdf"));
  EXPECT_TRUE(Storage.exists(sharedPath.c_str()));
  EXPECT_FALSE(Storage.exists((sharedPath + ".tmp").c_str()));
  EXPECT_FALSE(Storage.exists((sharedPath + ".bak").c_str()));

  BookmarkStore oldCollision;
  EXPECT_FALSE(oldCollision.loadForBook(oldPath, "First", "Author", "pdf"));
  BookmarkStore moved;
  ASSERT_TRUE(moved.loadForBook(newPath, "Second", "Author", "pdf"));
  ASSERT_EQ(moved.getBookmarks().size(), 1U);
  EXPECT_EQ(moved.getBookmarks()[0].paragraphIndex, 11);
  EXPECT_LE(Storage.maximumOpenHandles(), 1U);
}

TEST_F(LegacyStoreTest, PdfBookmarkCollisionRetagFaultsAreLosslessAcrossReboot) {
  struct FaultCase {
    TestStorageFault fault;
    uint16_t occurrence;
    bool committed;
  };
  constexpr std::array<FaultCase, 11> faults{{
      {TestStorageFault::Read, 1, false},
      {TestStorageFault::Read, 15, false},
      {TestStorageFault::Write, 1, false},
      {TestStorageFault::Flush, 1, false},
      {TestStorageFault::Sync, 1, false},
      {TestStorageFault::Close, 1, false},
      {TestStorageFault::Close, 2, false},
      {TestStorageFault::Close, 3, false},
      {TestStorageFault::Rename, 1, false},
      {TestStorageFault::Rename, 2, false},
      {TestStorageFault::Remove, 1, true},
  }};
  constexpr char oldPath[] = "/books/URYGSiQsIopJ.pdf";
  constexpr char newPath[] = "/books/t5SWIj04yLzY.pdf";
  constexpr uint32_t expectedCrc = 3724412502U;

  for (const FaultCase fault : faults) {
    SCOPED_TRACE(static_cast<int>(fault.fault));
    SCOPED_TRACE(fault.occurrence);
    Storage.reset();
    ASSERT_EQ(uzlib_crc32(oldPath, sizeof(oldPath) - 1U, 0), expectedCrc);
    ASSERT_EQ(uzlib_crc32(newPath, sizeof(newPath) - 1U, 0), expectedCrc);
    BookmarkStore source;
    ASSERT_TRUE(source.loadForBook(oldPath, "First", "Author", "pdf"));
    ASSERT_EQ(source.addPdfBookmark(2, 0.5F, "Chapter", 11, "first"), BookmarkStore::AddResult::Added);
    const std::string sharedPath = pdfCompatibilityPath("/.crosspoint/bookmarks", oldPath);
    ASSERT_EQ(sharedPath, pdfCompatibilityPath("/.crosspoint/bookmarks", newPath));

    Storage.fail(fault.fault, fault.occurrence);
    EXPECT_EQ(BookmarkStore::migrateForFilePath(oldPath, newPath, "Second", "Author", "pdf"), fault.committed);
    Storage.clearFault();
    EXPECT_TRUE(Storage.exists(sharedPath.c_str()) || Storage.exists((sharedPath + ".bak").c_str()));

    if (!fault.committed) {
      BookmarkStore oldReboot;
      ASSERT_TRUE(oldReboot.loadForBook(oldPath, "First", "Author", "pdf"));
      ASSERT_EQ(oldReboot.getBookmarks().size(), 1U);
      EXPECT_EQ(oldReboot.getBookmarks()[0].paragraphIndex, 11);
      BookmarkStore beforeRetryNew;
      EXPECT_FALSE(beforeRetryNew.loadForBook(newPath, "Second", "Author", "pdf"));
      ASSERT_TRUE(BookmarkStore::migrateForFilePath(oldPath, newPath, "Second", "Author", "pdf"));
    }

    BookmarkStore moved;
    ASSERT_TRUE(moved.loadForBook(newPath, "Second", "Author", "pdf"));
    ASSERT_EQ(moved.getBookmarks().size(), 1U);
    EXPECT_EQ(moved.getBookmarks()[0].paragraphIndex, 11);
    BookmarkStore oldCollision;
    EXPECT_FALSE(oldCollision.loadForBook(oldPath, "First", "Author", "pdf"));
    EXPECT_TRUE(Storage.exists(sharedPath.c_str()));
    EXPECT_FALSE(Storage.exists((sharedPath + ".tmp").c_str()));
    EXPECT_FALSE(Storage.exists((sharedPath + ".bak").c_str()));
    EXPECT_LE(Storage.maximumOpenHandles(), 1U);
  }
}

TEST_F(LegacyStoreTest, PdfLoadNeverRunsTheEpubReadFolderAutoMigration) {
  BookmarkStore source;
  ASSERT_TRUE(source.loadForBook("/books/book.pdf", "Title", "Author", "pdf"));
  ASSERT_EQ(source.addPdfBookmark(2, 0.5F, "Chapter", 11, "first"), BookmarkStore::AddResult::Added);

  BookmarkStore readFolderBook;
  ASSERT_TRUE(readFolderBook.loadForBook("/Read/book.pdf", "Title", "Author", "pdf"));
  EXPECT_TRUE(readFolderBook.getBookmarks().empty());
  EXPECT_EQ(Storage.listFiles("/.crosspoint/bookmarks").size(), 1U);
}

TEST_F(LegacyStoreTest, PdfClippingPathMigrationIsAccepted) {
  {
    ClippingStore source;
    ASSERT_TRUE(source.loadForBook("/books/old.pdf", "Title", "Author", "pdf"));
    ASSERT_EQ(source.addClipping(1, 0, 0, 1, 0, 1, 2, "Chapter", 23, "text"), ClippingStore::AddResult::Added);
  }

  ASSERT_TRUE(ClippingStore::migrateForFilePath("/books/old.pdf", "/books/new.pdf", "Title", "Author", "pdf"));
  ClippingStore moved;
  ASSERT_TRUE(moved.loadForBook("/books/new.pdf", "Title", "Author", "pdf"));
  ASSERT_EQ(moved.getClippings().size(), 1U);
  EXPECT_EQ(moved.getClippings()[0].paragraphIndex, 23);
  EXPECT_LE(Storage.maximumOpenHandles(), 1U);
}

TEST_F(LegacyStoreTest, PdfClippingLoadRejectsARealCrcCollisionWithoutRewriting) {
  constexpr char firstBook[] = "/books/URYGSiQsIopJ.pdf";
  constexpr char collidingBook[] = "/books/t5SWIj04yLzY.pdf";
  constexpr uint32_t expectedCrc = 3724412502U;
  ASSERT_EQ(uzlib_crc32(firstBook, sizeof(firstBook) - 1U, 0), expectedCrc);
  ASSERT_EQ(uzlib_crc32(collidingBook, sizeof(collidingBook) - 1U, 0), expectedCrc);

  ClippingStore first;
  ASSERT_TRUE(first.loadForBook(firstBook, "First", "Author", "pdf"));
  ASSERT_EQ(first.addPdfClipping(1, 0, 0, 1, 0, 1, 2, "Chapter", 21, "first"), ClippingStore::AddResult::Added);
  const std::string sharedPath = pdfCompatibilityPath("/.crosspoint/clippings", firstBook);
  const std::vector<uint8_t> originalBytes = Storage.files().at(sharedPath);

  ClippingStore collision;
  EXPECT_FALSE(collision.loadForBook(collidingBook, "Second", "Author", "pdf"));
  EXPECT_TRUE(collision.getClippings().empty());
  EXPECT_EQ(Storage.files().at(sharedPath), originalBytes);

  ClippingStore rebootedOriginal;
  ASSERT_TRUE(rebootedOriginal.loadForBook(firstBook, "First", "Author", "pdf"));
  ASSERT_EQ(rebootedOriginal.getClippings().size(), 1U);
  EXPECT_EQ(rebootedOriginal.getClippings()[0].paragraphIndex, 21);
  EXPECT_LE(Storage.maximumOpenHandles(), 1U);
}

TEST_F(LegacyStoreTest, PdfClippingMigrationRetagsOneSharedCrcCanonicalWithoutDeletingIt) {
  constexpr char oldPath[] = "/books/URYGSiQsIopJ.pdf";
  constexpr char newPath[] = "/books/t5SWIj04yLzY.pdf";
  ClippingStore source;
  ASSERT_TRUE(source.loadForBook(oldPath, "First", "Author", "pdf"));
  ASSERT_EQ(source.addPdfClipping(1, 0, 0, 1, 0, 1, 2, "Chapter", 21, "first"), ClippingStore::AddResult::Added);
  const std::string sharedPath = pdfCompatibilityPath("/.crosspoint/clippings", oldPath);
  ASSERT_EQ(sharedPath, pdfCompatibilityPath("/.crosspoint/clippings", newPath));

  ASSERT_TRUE(ClippingStore::migrateForFilePath(oldPath, newPath, "Second", "Author", "pdf"));
  EXPECT_TRUE(Storage.exists(sharedPath.c_str()));
  EXPECT_FALSE(Storage.exists((sharedPath + ".tmp").c_str()));
  EXPECT_FALSE(Storage.exists((sharedPath + ".bak").c_str()));

  ClippingStore oldCollision;
  EXPECT_FALSE(oldCollision.loadForBook(oldPath, "First", "Author", "pdf"));
  ClippingStore moved;
  ASSERT_TRUE(moved.loadForBook(newPath, "Second", "Author", "pdf"));
  ASSERT_EQ(moved.getClippings().size(), 1U);
  EXPECT_EQ(moved.getClippings()[0].paragraphIndex, 21);
  EXPECT_LE(Storage.maximumOpenHandles(), 1U);
}

TEST_F(LegacyStoreTest, PdfClippingCollisionRetagFaultsAreLosslessAcrossReboot) {
  struct FaultCase {
    TestStorageFault fault;
    uint16_t occurrence;
    bool committed;
  };
  constexpr std::array<FaultCase, 11> faults{{
      {TestStorageFault::Read, 1, false},
      {TestStorageFault::Read, 21, false},
      {TestStorageFault::Write, 1, false},
      {TestStorageFault::Flush, 1, false},
      {TestStorageFault::Sync, 1, false},
      {TestStorageFault::Close, 1, false},
      {TestStorageFault::Close, 2, false},
      {TestStorageFault::Close, 3, false},
      {TestStorageFault::Rename, 1, false},
      {TestStorageFault::Rename, 2, false},
      {TestStorageFault::Remove, 1, true},
  }};
  constexpr char oldPath[] = "/books/URYGSiQsIopJ.pdf";
  constexpr char newPath[] = "/books/t5SWIj04yLzY.pdf";
  constexpr uint32_t expectedCrc = 3724412502U;

  for (const FaultCase fault : faults) {
    SCOPED_TRACE(static_cast<int>(fault.fault));
    SCOPED_TRACE(fault.occurrence);
    Storage.reset();
    ASSERT_EQ(uzlib_crc32(oldPath, sizeof(oldPath) - 1U, 0), expectedCrc);
    ASSERT_EQ(uzlib_crc32(newPath, sizeof(newPath) - 1U, 0), expectedCrc);
    ClippingStore source;
    ASSERT_TRUE(source.loadForBook(oldPath, "First", "Author", "pdf"));
    ASSERT_EQ(source.addPdfClipping(1, 0, 0, 1, 0, 1, 2, "Chapter", 21, "first"), ClippingStore::AddResult::Added);
    const std::string sharedPath = pdfCompatibilityPath("/.crosspoint/clippings", oldPath);
    ASSERT_EQ(sharedPath, pdfCompatibilityPath("/.crosspoint/clippings", newPath));

    Storage.fail(fault.fault, fault.occurrence);
    EXPECT_EQ(ClippingStore::migrateForFilePath(oldPath, newPath, "Second", "Author", "pdf"), fault.committed);
    Storage.clearFault();
    EXPECT_TRUE(Storage.exists(sharedPath.c_str()) || Storage.exists((sharedPath + ".bak").c_str()));

    if (!fault.committed) {
      ClippingStore oldReboot;
      ASSERT_TRUE(oldReboot.loadForBook(oldPath, "First", "Author", "pdf"));
      ASSERT_EQ(oldReboot.getClippings().size(), 1U);
      EXPECT_EQ(oldReboot.getClippings()[0].paragraphIndex, 21);
      ClippingStore beforeRetryNew;
      EXPECT_FALSE(beforeRetryNew.loadForBook(newPath, "Second", "Author", "pdf"));
      ASSERT_TRUE(ClippingStore::migrateForFilePath(oldPath, newPath, "Second", "Author", "pdf"));
    }

    ClippingStore moved;
    ASSERT_TRUE(moved.loadForBook(newPath, "Second", "Author", "pdf"));
    ASSERT_EQ(moved.getClippings().size(), 1U);
    EXPECT_EQ(moved.getClippings()[0].paragraphIndex, 21);
    ClippingStore oldCollision;
    EXPECT_FALSE(oldCollision.loadForBook(oldPath, "First", "Author", "pdf"));
    EXPECT_TRUE(Storage.exists(sharedPath.c_str()));
    EXPECT_FALSE(Storage.exists((sharedPath + ".tmp").c_str()));
    EXPECT_FALSE(Storage.exists((sharedPath + ".bak").c_str()));
    EXPECT_LE(Storage.maximumOpenHandles(), 1U);
  }
}

TEST_F(LegacyStoreTest, PdfScratchOomPropagatesWithoutMutatingMemoryOrDisk) {
  constexpr char oldPath[] = "/books/URYGSiQsIopJ.pdf";
  constexpr char newPath[] = "/books/t5SWIj04yLzY.pdf";
  constexpr uint32_t expectedCrc = 3724412502U;
  ASSERT_EQ(uzlib_crc32(oldPath, sizeof(oldPath) - 1U, 0), expectedCrc);
  ASSERT_EQ(uzlib_crc32(newPath, sizeof(newPath) - 1U, 0), expectedCrc);

  BookmarkStore resident;
  ASSERT_TRUE(resident.loadForBook("/books/resident.pdf", "Resident", "Author", "pdf"));
  ASSERT_EQ(resident.addPdfBookmark(4, 0.25F, "Resident", 99, "resident"), BookmarkStore::AddResult::Added);
  BookmarkStore source;
  ASSERT_TRUE(source.loadForBook(oldPath, "First", "Author", "pdf"));
  ASSERT_EQ(source.addPdfBookmark(2, 0.5F, "Chapter", 11, "first"), BookmarkStore::AddResult::Added);
  const std::string bookmarkPath = pdfCompatibilityPath("/.crosspoint/bookmarks", oldPath);
  const std::vector<uint8_t> bookmarkBytes = Storage.files().at(bookmarkPath);

  forcedNothrowAllocationFailures = 1;
  EXPECT_FALSE(resident.loadForBook(oldPath, "First", "Author", "pdf"));
  ASSERT_EQ(resident.getBookmarks().size(), 1U);
  EXPECT_EQ(resident.getBookmarks()[0].paragraphIndex, 99);
  EXPECT_EQ(Storage.files().at(bookmarkPath), bookmarkBytes);

  forcedNothrowAllocationFailures = 1;
  EXPECT_FALSE(BookmarkStore::migrateForFilePath(oldPath, newPath, "Second", "Author", "pdf"));
  EXPECT_EQ(Storage.files().at(bookmarkPath), bookmarkBytes);
  BookmarkStore bookmarkReboot;
  ASSERT_TRUE(bookmarkReboot.loadForBook(oldPath, "First", "Author", "pdf"));
  ASSERT_EQ(bookmarkReboot.getBookmarks().size(), 1U);
  EXPECT_EQ(bookmarkReboot.getBookmarks()[0].paragraphIndex, 11);

  Storage.reset();
  ClippingStore clippingSource;
  ASSERT_TRUE(clippingSource.loadForBook(oldPath, "First", "Author", "pdf"));
  ASSERT_EQ(clippingSource.addPdfClipping(1, 0, 0, 1, 0, 1, 2, "Chapter", 21, "first"),
            ClippingStore::AddResult::Added);
  const std::string clippingPath = pdfCompatibilityPath("/.crosspoint/clippings", oldPath);
  const std::vector<uint8_t> clippingBytes = Storage.files().at(clippingPath);

  forcedNothrowAllocationFailures = 1;
  EXPECT_FALSE(ClippingStore::migrateForFilePath(oldPath, newPath, "Second", "Author", "pdf"));
  EXPECT_EQ(Storage.files().at(clippingPath), clippingBytes);
  ClippingStore clippingReboot;
  ASSERT_TRUE(clippingReboot.loadForBook(oldPath, "First", "Author", "pdf"));
  ASSERT_EQ(clippingReboot.getClippings().size(), 1U);
  EXPECT_EQ(clippingReboot.getClippings()[0].paragraphIndex, 21);
  EXPECT_LE(Storage.maximumOpenHandles(), 1U);
}

TEST_F(LegacyStoreTest, PdfBookmarkAtomicAddRejectsEveryPreCommitIoFailureAndRebootsToOldState) {
  struct FaultCase {
    TestStorageFault fault;
    uint16_t occurrence;
  };
  constexpr std::array<FaultCase, 8> faults{{
      {TestStorageFault::Write, 1},
      {TestStorageFault::Flush, 1},
      {TestStorageFault::Sync, 1},
      {TestStorageFault::Close, 1},
      {TestStorageFault::Read, 1},
      {TestStorageFault::Close, 2},
      {TestStorageFault::Rename, 1},
      {TestStorageFault::Rename, 2},
  }};

  for (const FaultCase fault : faults) {
    SCOPED_TRACE(static_cast<int>(fault.fault));
    SCOPED_TRACE(fault.occurrence);
    Storage.reset();
    BookmarkStore store;
    ASSERT_TRUE(store.loadForBook("/books/book.pdf", "Title", "Author", "pdf"));
    ASSERT_EQ(store.addPdfBookmark(2, 0.5F, "Chapter", 11, "old"), BookmarkStore::AddResult::Added);
    const std::string canonicalPath = canonicalFilePathIn("/.crosspoint/bookmarks");
    const std::vector<uint8_t> oldBytes = Storage.files().at(canonicalPath);

    Storage.fail(fault.fault, fault.occurrence);
    EXPECT_EQ(store.addPdfBookmark(3, 0.75F, "Chapter", 12, "new"), BookmarkStore::AddResult::SaveFailed);
    Storage.clearFault();
    ASSERT_EQ(store.getBookmarks().size(), 1U);
    EXPECT_EQ(store.getBookmarks()[0].paragraphIndex, 11);
    EXPECT_EQ(Storage.files().at(canonicalPath), oldBytes);

    BookmarkStore rebooted;
    ASSERT_TRUE(rebooted.loadForBook("/books/book.pdf", "Title", "Author", "pdf"));
    ASSERT_EQ(rebooted.getBookmarks().size(), 1U);
    EXPECT_EQ(rebooted.getBookmarks()[0].paragraphIndex, 11);
    EXPECT_LE(Storage.maximumOpenHandles(), 1U);
  }
}

TEST_F(LegacyStoreTest, PdfBookmarkAtomicAddHandlesRemoveFaultsWithoutAmbiguousMemoryState) {
  BookmarkStore store;
  ASSERT_TRUE(store.loadForBook("/books/book.pdf", "Title", "Author", "pdf"));
  ASSERT_EQ(store.addPdfBookmark(2, 0.5F, "Chapter", 11, "old"), BookmarkStore::AddResult::Added);
  const std::string canonicalPath = canonicalFilePathIn("/.crosspoint/bookmarks");
  const std::vector<uint8_t> oldBytes = Storage.files().at(canonicalPath);

  Storage.addFile(canonicalPath + ".tmp", {0xaa});
  Storage.fail(TestStorageFault::Remove);
  EXPECT_EQ(store.addPdfBookmark(3, 0.75F, "Chapter", 12, "new"), BookmarkStore::AddResult::SaveFailed);
  Storage.clearFault();
  EXPECT_EQ(Storage.files().at(canonicalPath), oldBytes);
  ASSERT_EQ(store.getBookmarks().size(), 1U);

  BookmarkStore failedRemoveReboot;
  ASSERT_TRUE(failedRemoveReboot.loadForBook("/books/book.pdf", "Title", "Author", "pdf"));
  ASSERT_EQ(failedRemoveReboot.getBookmarks().size(), 1U);
  EXPECT_EQ(failedRemoveReboot.getBookmarks()[0].paragraphIndex, 11);
  EXPECT_FALSE(Storage.exists((canonicalPath + ".tmp").c_str()));

  Storage.fail(TestStorageFault::Remove);
  EXPECT_EQ(store.addPdfBookmark(3, 0.75F, "Chapter", 12, "new"), BookmarkStore::AddResult::Added);
  Storage.clearFault();
  ASSERT_EQ(store.getBookmarks().size(), 2U);

  BookmarkStore rebooted;
  ASSERT_TRUE(rebooted.loadForBook("/books/book.pdf", "Title", "Author", "pdf"));
  ASSERT_EQ(rebooted.getBookmarks().size(), 2U);
  EXPECT_EQ(rebooted.getBookmarks()[0].paragraphIndex, 11);
  EXPECT_EQ(rebooted.getBookmarks()[1].paragraphIndex, 12);
  EXPECT_LE(Storage.maximumOpenHandles(), 1U);
}

TEST_F(LegacyStoreTest, PdfBookmarkRemoveAndClearChangeMemoryOnlyAfterDurableCommit) {
  BookmarkStore store;
  ASSERT_TRUE(store.loadForBook("/books/book.pdf", "Title", "Author", "pdf"));
  ASSERT_EQ(store.addPdfBookmark(2, 0.5F, "Chapter", 11, "first"), BookmarkStore::AddResult::Added);
  ASSERT_EQ(store.addPdfBookmark(3, 0.75F, "Chapter", 12, "second"), BookmarkStore::AddResult::Added);

  Storage.fail(TestStorageFault::Sync);
  EXPECT_FALSE(store.removePdfBookmark(12));
  Storage.clearFault();
  ASSERT_EQ(store.getBookmarks().size(), 2U);
  BookmarkStore failedRemoveReboot;
  ASSERT_TRUE(failedRemoveReboot.loadForBook("/books/book.pdf", "Title", "Author", "pdf"));
  EXPECT_EQ(failedRemoveReboot.getBookmarks().size(), 2U);

  ASSERT_TRUE(store.removePdfBookmark(12));
  ASSERT_EQ(store.getBookmarks().size(), 1U);
  EXPECT_EQ(store.getBookmarks()[0].paragraphIndex, 11);

  Storage.fail(TestStorageFault::Remove);
  EXPECT_FALSE(store.clearPdfBookmarks());
  Storage.clearFault();
  EXPECT_EQ(store.getBookmarks().size(), 1U);
  BookmarkStore failedClearReboot;
  ASSERT_TRUE(failedClearReboot.loadForBook("/books/book.pdf", "Title", "Author", "pdf"));
  ASSERT_EQ(failedClearReboot.getBookmarks().size(), 1U);
  EXPECT_EQ(failedClearReboot.getBookmarks()[0].paragraphIndex, 11);

  ASSERT_TRUE(store.clearPdfBookmarks());
  EXPECT_TRUE(store.getBookmarks().empty());
  EXPECT_FALSE(BookmarkStore::hasAnyBookmarks());
  BookmarkStore clearedReboot;
  ASSERT_TRUE(clearedReboot.loadForBook("/books/book.pdf", "Title", "Author", "pdf"));
  EXPECT_TRUE(clearedReboot.getBookmarks().empty());
  EXPECT_LE(Storage.maximumOpenHandles(), 1U);
}

TEST_F(LegacyStoreTest, PdfClippingAtomicMutationsRollbackOnFailureAndSurviveReboot) {
  ClippingStore store;
  ASSERT_TRUE(store.loadForBook("/books/book.pdf", "Title", "Author", "pdf"));
  ASSERT_EQ(store.addPdfClipping(1, 0, 0, 1, 0, 1, 2, "Chapter", 21, "old"), ClippingStore::AddResult::Added);

  for (const TestStorageFault fault : {TestStorageFault::Write, TestStorageFault::Flush, TestStorageFault::Sync,
                                       TestStorageFault::Close, TestStorageFault::Read, TestStorageFault::Rename}) {
    SCOPED_TRACE(static_cast<int>(fault));
    Storage.fail(fault, fault == TestStorageFault::Rename ? 2 : 1);
    EXPECT_EQ(store.addPdfClipping(2, 1, 1, 2, 2, 3, 4, "Chapter", 22, "new"), ClippingStore::AddResult::SaveFailed);
    Storage.clearFault();
    ASSERT_EQ(store.getClippings().size(), 1U);
    EXPECT_EQ(store.getClippings()[0].paragraphIndex, 21);

    ClippingStore rebooted;
    ASSERT_TRUE(rebooted.loadForBook("/books/book.pdf", "Title", "Author", "pdf"));
    ASSERT_EQ(rebooted.getClippings().size(), 1U);
    EXPECT_EQ(rebooted.getClippings()[0].paragraphIndex, 21);
  }

  ASSERT_EQ(store.addPdfClipping(2, 1, 1, 2, 2, 3, 4, "Chapter", 22, "new"), ClippingStore::AddResult::Added);
  Storage.fail(TestStorageFault::Sync);
  EXPECT_FALSE(store.removePdfClipping(22));
  Storage.clearFault();
  EXPECT_EQ(store.getClippings().size(), 2U);
  ASSERT_TRUE(store.removePdfClipping(22));
  EXPECT_EQ(store.getClippings().size(), 1U);

  Storage.fail(TestStorageFault::Remove);
  EXPECT_FALSE(store.clearPdfClippings());
  Storage.clearFault();
  EXPECT_EQ(store.getClippings().size(), 1U);
  ASSERT_TRUE(store.clearPdfClippings());
  EXPECT_TRUE(store.getClippings().empty());
  EXPECT_FALSE(ClippingStore::hasAnyClippings());

  ClippingStore rebooted;
  ASSERT_TRUE(rebooted.loadForBook("/books/book.pdf", "Title", "Author", "pdf"));
  EXPECT_TRUE(rebooted.getClippings().empty());
  EXPECT_LE(Storage.maximumOpenHandles(), 1U);
}

TEST_F(LegacyStoreTest, PdfDurableReloadReportsUntrustworthyReadForFailClosedReaderPath) {
  BookmarkStore bookmarks;
  ASSERT_TRUE(bookmarks.loadForBook("/books/reload-fault.pdf", "Title", "Author", "pdf"));
  ASSERT_EQ(bookmarks.addPdfBookmark(1, 0.5F, "Chapter", 7, "bookmark"), BookmarkStore::AddResult::Added);
  Storage.fail(TestStorageFault::Read);
  EXPECT_FALSE(bookmarks.reloadPdfFromDisk());
  Storage.clearFault();
  ASSERT_EQ(bookmarks.getBookmarks().size(), 1U);
  EXPECT_EQ(bookmarks.getBookmarks()[0].paragraphIndex, 7);

  ClippingStore clippings;
  ASSERT_TRUE(clippings.loadForBook("/books/reload-fault.pdf", "Title", "Author", "pdf"));
  ASSERT_EQ(clippings.addPdfClipping(1, 0, 0, 1, 0, 0, 1, "Chapter", 8, "clipping"),
            ClippingStore::AddResult::Added);
  Storage.fail(TestStorageFault::Read);
  EXPECT_FALSE(clippings.reloadPdfFromDisk());
  Storage.clearFault();
  EXPECT_TRUE(clippings.getClippings().empty());
  EXPECT_LE(Storage.maximumOpenHandles(), 1U);
}

TEST_F(LegacyStoreTest, GenericPdfListRemovalUsesDurableMutationAndRetainsStateOnFailure) {
  BookmarkStore bookmarks;
  ASSERT_TRUE(bookmarks.loadForBook("/books/book.pdf", "Title", "Author", "pdf"));
  ASSERT_EQ(bookmarks.addPdfBookmark(2, 0.5F, "Chapter", 11, "first"), BookmarkStore::AddResult::Added);
  ASSERT_EQ(bookmarks.addPdfBookmark(3, 0.75F, "Chapter", 12, "second"), BookmarkStore::AddResult::Added);

  Storage.fail(TestStorageFault::Sync);
  EXPECT_FALSE(bookmarks.removeBookmarkAt(1));
  Storage.clearFault();
  ASSERT_EQ(bookmarks.getBookmarks().size(), 2U);
  BookmarkStore bookmarkReboot;
  ASSERT_TRUE(bookmarkReboot.loadForBook("/books/book.pdf", "Title", "Author", "pdf"));
  EXPECT_EQ(bookmarkReboot.getBookmarks().size(), 2U);

  Storage.reset();
  ClippingStore clippings;
  ASSERT_TRUE(clippings.loadForBook("/books/book.pdf", "Title", "Author", "pdf"));
  ASSERT_EQ(clippings.addPdfClipping(1, 0, 0, 1, 0, 1, 2, "Chapter", 21, "first"), ClippingStore::AddResult::Added);
  ASSERT_EQ(clippings.addPdfClipping(2, 1, 1, 2, 2, 3, 4, "Chapter", 22, "second"), ClippingStore::AddResult::Added);

  Storage.fail(TestStorageFault::Sync);
  EXPECT_FALSE(clippings.removeClippingAt(1));
  Storage.clearFault();
  ASSERT_EQ(clippings.getClippings().size(), 2U);
  ClippingStore clippingReboot;
  ASSERT_TRUE(clippingReboot.loadForBook("/books/book.pdf", "Title", "Author", "pdf"));
  EXPECT_EQ(clippingReboot.getClippings().size(), 2U);
  EXPECT_LE(Storage.maximumOpenHandles(), 1U);
}

TEST_F(LegacyStoreTest, GenericPdfClearFailsClosedForEveryArtifactRemovalAndCannotResurrect) {
  for (uint16_t occurrence = 1; occurrence <= 3; ++occurrence) {
    SCOPED_TRACE(occurrence);
    Storage.reset();
    BookmarkStore store;
    std::string canonicalPath;
    seedPdfBookmarkArtifacts(&store, &canonicalPath);

    Storage.fail(TestStorageFault::Remove, occurrence);
    store.clearAll();
    Storage.clearFault();
    ASSERT_EQ(store.getBookmarks().size(), 2U);

    BookmarkStore rebooted;
    ASSERT_TRUE(rebooted.loadForBook("/books/book.pdf", "Title", "Author", "pdf"));
    ASSERT_EQ(rebooted.getBookmarks().size(), 2U);
    EXPECT_EQ(rebooted.getBookmarks()[0].paragraphIndex, 11);
    EXPECT_EQ(rebooted.getBookmarks()[1].paragraphIndex, 12);
  }

  Storage.reset();
  BookmarkStore store;
  std::string canonicalPath;
  seedPdfBookmarkArtifacts(&store, &canonicalPath);
  store.clearAll();
  EXPECT_TRUE(store.getBookmarks().empty());
  EXPECT_FALSE(Storage.exists(canonicalPath.c_str()));
  EXPECT_FALSE(Storage.exists((canonicalPath + ".tmp").c_str()));
  EXPECT_FALSE(Storage.exists((canonicalPath + ".bak").c_str()));
  BookmarkStore rebooted;
  ASSERT_TRUE(rebooted.loadForBook("/books/book.pdf", "Title", "Author", "pdf"));
  EXPECT_TRUE(rebooted.getBookmarks().empty());
}

TEST_F(LegacyStoreTest, GenericPdfClippingClearFailsClosedForEveryArtifactRemovalAndCannotResurrect) {
  for (uint16_t occurrence = 1; occurrence <= 3; ++occurrence) {
    SCOPED_TRACE(occurrence);
    Storage.reset();
    ClippingStore store;
    std::string canonicalPath;
    seedPdfClippingArtifacts(&store, &canonicalPath);

    Storage.fail(TestStorageFault::Remove, occurrence);
    store.clearAll();
    Storage.clearFault();
    ASSERT_EQ(store.getClippings().size(), 2U);

    ClippingStore rebooted;
    ASSERT_TRUE(rebooted.loadForBook("/books/book.pdf", "Title", "Author", "pdf"));
    ASSERT_EQ(rebooted.getClippings().size(), 2U);
    EXPECT_EQ(rebooted.getClippings()[0].paragraphIndex, 21);
    EXPECT_EQ(rebooted.getClippings()[1].paragraphIndex, 22);
  }

  Storage.reset();
  ClippingStore store;
  std::string canonicalPath;
  seedPdfClippingArtifacts(&store, &canonicalPath);
  store.clearAll();
  EXPECT_TRUE(store.getClippings().empty());
  EXPECT_FALSE(Storage.exists(canonicalPath.c_str()));
  EXPECT_FALSE(Storage.exists((canonicalPath + ".tmp").c_str()));
  EXPECT_FALSE(Storage.exists((canonicalPath + ".bak").c_str()));
  ClippingStore rebooted;
  ASSERT_TRUE(rebooted.loadForBook("/books/book.pdf", "Title", "Author", "pdf"));
  EXPECT_TRUE(rebooted.getClippings().empty());
}

TEST_F(LegacyStoreTest, StaticPdfDeleteFailsClosedForAllArtifactsAndSuccessCannotResurrect) {
  for (uint16_t occurrence = 1; occurrence <= 3; ++occurrence) {
    SCOPED_TRACE(occurrence);
    Storage.reset();
    BookmarkStore store;
    std::string canonicalPath;
    seedPdfBookmarkArtifacts(&store, &canonicalPath);

    Storage.fail(TestStorageFault::Remove, occurrence);
    EXPECT_FALSE(BookmarkStore::deleteForFilePath("/books/book.pdf", "pdf"));
    Storage.clearFault();
    BookmarkStore rebooted;
    ASSERT_TRUE(rebooted.loadForBook("/books/book.pdf", "Title", "Author", "pdf"));
    EXPECT_EQ(rebooted.getBookmarks().size(), 2U);
  }

  Storage.reset();
  BookmarkStore store;
  std::string canonicalPath;
  seedPdfBookmarkArtifacts(&store, &canonicalPath);
  EXPECT_TRUE(BookmarkStore::deleteForFilePath("/books/book.pdf", "pdf"));
  BookmarkStore rebooted;
  ASSERT_TRUE(rebooted.loadForBook("/books/book.pdf", "Title", "Author", "pdf"));
  EXPECT_TRUE(rebooted.getBookmarks().empty());
}

TEST_F(LegacyStoreTest, StaticPdfClippingDeleteFailsClosedForAllArtifactsAndSuccessCannotResurrect) {
  for (uint16_t occurrence = 1; occurrence <= 3; ++occurrence) {
    SCOPED_TRACE(occurrence);
    Storage.reset();
    ClippingStore store;
    std::string canonicalPath;
    seedPdfClippingArtifacts(&store, &canonicalPath);

    Storage.fail(TestStorageFault::Remove, occurrence);
    EXPECT_FALSE(ClippingStore::deleteForFilePath("/books/book.pdf", "pdf"));
    Storage.clearFault();
    ClippingStore rebooted;
    ASSERT_TRUE(rebooted.loadForBook("/books/book.pdf", "Title", "Author", "pdf"));
    EXPECT_EQ(rebooted.getClippings().size(), 2U);
  }

  Storage.reset();
  ClippingStore store;
  std::string canonicalPath;
  seedPdfClippingArtifacts(&store, &canonicalPath);
  EXPECT_TRUE(ClippingStore::deleteForFilePath("/books/book.pdf", "pdf"));
  ClippingStore rebooted;
  ASSERT_TRUE(rebooted.loadForBook("/books/book.pdf", "Title", "Author", "pdf"));
  EXPECT_TRUE(rebooted.getClippings().empty());
}

TEST_F(LegacyStoreTest, SavedItemScannersIgnorePdfTransactionArtifacts) {
  Storage.addFile("/books/book.pdf", {});
  BookmarkStore bookmarks;
  std::string bookmarkPath;
  seedPdfBookmarkArtifacts(&bookmarks, &bookmarkPath);
  std::vector<BookmarkedBookEntry> bookmarkedBooks;
  ASSERT_TRUE(BookmarkStore::getAllBookmarkedBooks(bookmarkedBooks));
  ASSERT_EQ(bookmarkedBooks.size(), 1U);
  EXPECT_EQ(bookmarkedBooks[0].count, 2U);
  ASSERT_TRUE(Storage.remove(bookmarkPath.c_str()));
  EXPECT_FALSE(BookmarkStore::hasAnyBookmarks());
  bookmarkedBooks.clear();
  ASSERT_TRUE(BookmarkStore::getAllBookmarkedBooks(bookmarkedBooks));
  EXPECT_TRUE(bookmarkedBooks.empty());

  ClippingStore clippings;
  std::string clippingPath;
  seedPdfClippingArtifacts(&clippings, &clippingPath);
  std::vector<ClippedBookEntry> clippedBooks;
  ASSERT_TRUE(ClippingStore::getAllClippedBooks(clippedBooks));
  ASSERT_EQ(clippedBooks.size(), 1U);
  EXPECT_EQ(clippedBooks[0].count, 2U);
  ASSERT_TRUE(Storage.remove(clippingPath.c_str()));
  EXPECT_FALSE(ClippingStore::hasAnyClippings());
  clippedBooks.clear();
  ASSERT_TRUE(ClippingStore::getAllClippedBooks(clippedBooks));
  EXPECT_TRUE(clippedBooks.empty());
}

TEST_F(LegacyStoreTest, PdfMigrationsRecoverSourceAndDestinationArtifactsBeforeMerging) {
  BookmarkStore sourceBookmarks;
  std::string sourceBookmarkPath;
  seedPdfBookmarkArtifacts(&sourceBookmarks, &sourceBookmarkPath, "/books/old.pdf");
  BookmarkStore destinationBookmarks;
  std::string destinationBookmarkPath;
  seedPdfBookmarkArtifacts(&destinationBookmarks, &destinationBookmarkPath, "/books/new.pdf");

  ASSERT_TRUE(BookmarkStore::migrateForFilePath("/books/old.pdf", "/books/new.pdf", "Title", "Author", "pdf"));
  EXPECT_FALSE(Storage.exists(sourceBookmarkPath.c_str()));
  EXPECT_FALSE(Storage.exists((sourceBookmarkPath + ".tmp").c_str()));
  EXPECT_FALSE(Storage.exists((sourceBookmarkPath + ".bak").c_str()));
  EXPECT_FALSE(Storage.exists((destinationBookmarkPath + ".tmp").c_str()));
  EXPECT_FALSE(Storage.exists((destinationBookmarkPath + ".bak").c_str()));
  BookmarkStore migratedBookmarks;
  ASSERT_TRUE(migratedBookmarks.loadForBook("/books/new.pdf", "Title", "Author", "pdf"));
  EXPECT_EQ(migratedBookmarks.getBookmarks().size(), 2U);

  Storage.reset();
  ClippingStore sourceClippings;
  std::string sourceClippingPath;
  seedPdfClippingArtifacts(&sourceClippings, &sourceClippingPath, "/books/old.pdf");
  ClippingStore destinationClippings;
  std::string destinationClippingPath;
  seedPdfClippingArtifacts(&destinationClippings, &destinationClippingPath, "/books/new.pdf");

  ASSERT_TRUE(ClippingStore::migrateForFilePath("/books/old.pdf", "/books/new.pdf", "Title", "Author", "pdf"));
  EXPECT_FALSE(Storage.exists(sourceClippingPath.c_str()));
  EXPECT_FALSE(Storage.exists((sourceClippingPath + ".tmp").c_str()));
  EXPECT_FALSE(Storage.exists((sourceClippingPath + ".bak").c_str()));
  EXPECT_FALSE(Storage.exists((destinationClippingPath + ".tmp").c_str()));
  EXPECT_FALSE(Storage.exists((destinationClippingPath + ".bak").c_str()));
  ClippingStore migratedClippings;
  ASSERT_TRUE(migratedClippings.loadForBook("/books/new.pdf", "Title", "Author", "pdf"));
  EXPECT_EQ(migratedClippings.getClippings().size(), 2U);
  EXPECT_LE(Storage.maximumOpenHandles(), 1U);
}

TEST_F(LegacyStoreTest, PdfBookmarkMigrationFaultsAreLosslessAndRetryIdempotently) {
  struct FaultCase {
    TestStorageFault fault;
    uint16_t occurrence;
  };
  constexpr std::array<FaultCase, 8> faults{{
      {TestStorageFault::Read, 1},
      {TestStorageFault::Write, 1},
      {TestStorageFault::Flush, 1},
      {TestStorageFault::Sync, 1},
      {TestStorageFault::Close, 1},
      {TestStorageFault::Rename, 1},
      {TestStorageFault::Rename, 2},
      {TestStorageFault::Remove, 2},
  }};

  for (const FaultCase fault : faults) {
    SCOPED_TRACE(static_cast<int>(fault.fault));
    SCOPED_TRACE(fault.occurrence);
    Storage.reset();
    BookmarkStore source;
    ASSERT_TRUE(source.loadForBook("/books/old.pdf", "Title", "Author", "pdf"));
    ASSERT_EQ(source.addPdfBookmark(2, 0.5F, "Chapter", 11, "source"), BookmarkStore::AddResult::Added);
    BookmarkStore destination;
    ASSERT_TRUE(destination.loadForBook("/books/new.pdf", "Title", "Author", "pdf"));
    ASSERT_EQ(destination.addPdfBookmark(3, 0.75F, "Chapter", 12, "destination"), BookmarkStore::AddResult::Added);

    Storage.fail(fault.fault, fault.occurrence);
    EXPECT_FALSE(BookmarkStore::migrateForFilePath("/books/old.pdf", "/books/new.pdf", "Title", "Author", "pdf"));
    Storage.clearFault();

    BookmarkStore sourceReboot;
    ASSERT_TRUE(sourceReboot.loadForBook("/books/old.pdf", "Title", "Author", "pdf"));
    ASSERT_EQ(sourceReboot.getBookmarks().size(), 1U);
    BookmarkStore destinationReboot;
    ASSERT_TRUE(destinationReboot.loadForBook("/books/new.pdf", "Title", "Author", "pdf"));
    ASSERT_GE(destinationReboot.getBookmarks().size(), 1U);

    ASSERT_TRUE(BookmarkStore::migrateForFilePath("/books/old.pdf", "/books/new.pdf", "Title", "Author", "pdf"));
    BookmarkStore moved;
    ASSERT_TRUE(moved.loadForBook("/books/new.pdf", "Title", "Author", "pdf"));
    ASSERT_EQ(moved.getBookmarks().size(), 2U);
    BookmarkStore old;
    ASSERT_TRUE(old.loadForBook("/books/old.pdf", "Title", "Author", "pdf"));
    EXPECT_TRUE(old.getBookmarks().empty());
    EXPECT_LE(Storage.maximumOpenHandles(), 1U);
  }
}

TEST_F(LegacyStoreTest, PdfClippingMigrationFaultsAreLosslessAndRetryIdempotently) {
  struct FaultCase {
    TestStorageFault fault;
    uint16_t occurrence;
  };
  constexpr std::array<FaultCase, 8> faults{{
      {TestStorageFault::Read, 1},
      {TestStorageFault::Write, 1},
      {TestStorageFault::Flush, 1},
      {TestStorageFault::Sync, 1},
      {TestStorageFault::Close, 1},
      {TestStorageFault::Rename, 1},
      {TestStorageFault::Rename, 2},
      {TestStorageFault::Remove, 2},
  }};

  for (const FaultCase fault : faults) {
    SCOPED_TRACE(static_cast<int>(fault.fault));
    SCOPED_TRACE(fault.occurrence);
    Storage.reset();
    ClippingStore source;
    ASSERT_TRUE(source.loadForBook("/books/old.pdf", "Title", "Author", "pdf"));
    ASSERT_EQ(source.addPdfClipping(1, 0, 0, 1, 0, 1, 2, "Chapter", 21, "source"), ClippingStore::AddResult::Added);
    ClippingStore destination;
    ASSERT_TRUE(destination.loadForBook("/books/new.pdf", "Title", "Author", "pdf"));
    ASSERT_EQ(destination.addPdfClipping(2, 1, 1, 2, 2, 3, 4, "Chapter", 22, "destination"),
              ClippingStore::AddResult::Added);

    Storage.fail(fault.fault, fault.occurrence);
    EXPECT_FALSE(ClippingStore::migrateForFilePath("/books/old.pdf", "/books/new.pdf", "Title", "Author", "pdf"));
    Storage.clearFault();

    ClippingStore sourceReboot;
    ASSERT_TRUE(sourceReboot.loadForBook("/books/old.pdf", "Title", "Author", "pdf"));
    ASSERT_EQ(sourceReboot.getClippings().size(), 1U);
    ClippingStore destinationReboot;
    ASSERT_TRUE(destinationReboot.loadForBook("/books/new.pdf", "Title", "Author", "pdf"));
    ASSERT_GE(destinationReboot.getClippings().size(), 1U);

    ASSERT_TRUE(ClippingStore::migrateForFilePath("/books/old.pdf", "/books/new.pdf", "Title", "Author", "pdf"));
    ClippingStore moved;
    ASSERT_TRUE(moved.loadForBook("/books/new.pdf", "Title", "Author", "pdf"));
    ASSERT_EQ(moved.getClippings().size(), 2U);
    ClippingStore old;
    ASSERT_TRUE(old.loadForBook("/books/old.pdf", "Title", "Author", "pdf"));
    EXPECT_TRUE(old.getClippings().empty());
    EXPECT_LE(Storage.maximumOpenHandles(), 1U);
  }
}

struct OperationTrace {
  std::array<char, 32> operations{};
  uint8_t count = 0;

  void record(const char operation) {
    if (count < operations.size()) operations[count++] = operation;
  }
};

struct FakePersistence {
  std::array<PdfSavedItem, PDF_SAVED_ITEMS_MAX_RECORDS> durable{};
  uint16_t durableCount = 0;
  PdfStatus loadStatus = PdfStatus::success();
  PdfStatus saveStatus = PdfStatus::success();
  uint16_t failSaveCall = 0;
  bool commitFailedSave = false;
  uint16_t loadCalls = 0;
  uint16_t saveCalls = 0;
  uint16_t validateCalls = 0;
  OperationTrace* trace = nullptr;

  static PdfStatus load(void* const context, PdfSavedItemsBuffer* const output) {
    auto& self = *static_cast<FakePersistence*>(context);
    ++self.loadCalls;
    output->count = 0;
    if (!self.loadStatus) return self.loadStatus;
    if (self.durableCount > output->capacity || (self.durableCount != 0 && output->items == nullptr)) {
      return PdfStatus::failure(PdfError::LimitExceeded);
    }
    for (uint16_t index = 0; index < self.durableCount; ++index) {
      output->items[index] = self.durable[index];
    }
    output->count = self.durableCount;
    return PdfStatus::success();
  }

  static PdfStatus save(void* const context, const PdfSavedItem* const items, const uint16_t count) {
    auto& self = *static_cast<FakePersistence*>(context);
    ++self.saveCalls;
    if (self.trace != nullptr) self.trace->record('S');
    const bool shouldFail = (self.failSaveCall != 0 && self.saveCalls == self.failSaveCall) || !self.saveStatus;
    if (count > self.durable.size() || (count != 0 && items == nullptr)) {
      return PdfStatus::failure(PdfError::LimitExceeded);
    }
    if (shouldFail && !self.commitFailedSave) return PdfStatus::failure(PdfError::IoFailure);
    for (uint16_t index = 0; index < count; ++index) {
      self.durable[index] = items[index];
    }
    self.durableCount = count;
    return shouldFail ? PdfStatus::failure(PdfError::IoFailure) : PdfStatus::success();
  }

  static PdfStatus validate(void* const context, const PdfSavedItem& item) {
    auto& self = *static_cast<FakePersistence*>(context);
    ++self.validateCalls;
    return pdfValidateSavedItem(item, 512);
  }

  PdfSavedItemsPersistence callbacks() {
    return {this, FakePersistence::load, FakePersistence::save, FakePersistence::validate};
  }
};

struct FakeLegacy {
  std::array<uint16_t, PDF_SAVED_ITEMS_MAX_CLIPPINGS> ids{};
  uint16_t size = 0;
  bool failCount = false;
  int failReadIndex = -1;
  bool failAdd = false;
  bool failRemove = false;
  bool failClear = false;
  bool ambiguousAdd = false;
  bool ambiguousRemove = false;
  bool ambiguousClear = false;
  bool applyAmbiguousMutation = false;
  uint16_t countCalls = 0;
  uint16_t readCalls = 0;
  uint16_t addCalls = 0;
  uint16_t removeCalls = 0;
  uint16_t clearCalls = 0;
  OperationTrace* trace = nullptr;

  bool contains(const uint16_t itemId) const {
    for (uint16_t index = 0; index < size; ++index) {
      if (ids[index] == itemId) return true;
    }
    return false;
  }

  static bool count(void* const context, uint16_t* const output) {
    auto& self = *static_cast<FakeLegacy*>(context);
    ++self.countCalls;
    if (self.failCount || output == nullptr) return false;
    *output = self.size;
    return true;
  }

  static bool read(void* const context, const uint16_t index, uint16_t* const output) {
    auto& self = *static_cast<FakeLegacy*>(context);
    ++self.readCalls;
    if (output == nullptr || index >= self.size || self.failReadIndex == index) return false;
    *output = self.ids[index];
    return true;
  }

  static PdfSavedItemsLegacyMutationResult add(void* const context, const uint16_t itemId) {
    auto& self = *static_cast<FakeLegacy*>(context);
    ++self.addCalls;
    if (self.trace != nullptr) self.trace->record('A');
    if (self.failAdd || self.size >= self.ids.size()) return PdfSavedItemsLegacyMutationResult::Rejected;
    if (self.ambiguousAdd && !self.applyAmbiguousMutation) {
      return PdfSavedItemsLegacyMutationResult::Ambiguous;
    }
    self.ids[self.size++] = itemId;
    return self.ambiguousAdd ? PdfSavedItemsLegacyMutationResult::Ambiguous
                             : PdfSavedItemsLegacyMutationResult::Applied;
  }

  static PdfSavedItemsLegacyMutationResult remove(void* const context, const uint16_t itemId) {
    auto& self = *static_cast<FakeLegacy*>(context);
    ++self.removeCalls;
    if (self.trace != nullptr) self.trace->record('R');
    if (self.failRemove) return PdfSavedItemsLegacyMutationResult::Rejected;
    if (self.ambiguousRemove && !self.applyAmbiguousMutation) {
      return PdfSavedItemsLegacyMutationResult::Ambiguous;
    }
    for (uint16_t index = 0; index < self.size; ++index) {
      if (self.ids[index] != itemId) continue;
      for (uint16_t tail = index + 1; tail < self.size; ++tail) {
        self.ids[tail - 1] = self.ids[tail];
      }
      --self.size;
      return self.ambiguousRemove ? PdfSavedItemsLegacyMutationResult::Ambiguous
                                  : PdfSavedItemsLegacyMutationResult::Applied;
    }
    return PdfSavedItemsLegacyMutationResult::Rejected;
  }

  static PdfSavedItemsLegacyMutationResult clear(void* const context) {
    auto& self = *static_cast<FakeLegacy*>(context);
    ++self.clearCalls;
    if (self.trace != nullptr) self.trace->record('C');
    if (self.failClear) return PdfSavedItemsLegacyMutationResult::Rejected;
    if (self.ambiguousClear && !self.applyAmbiguousMutation) {
      return PdfSavedItemsLegacyMutationResult::Ambiguous;
    }
    self.size = 0;
    return self.ambiguousClear ? PdfSavedItemsLegacyMutationResult::Ambiguous
                               : PdfSavedItemsLegacyMutationResult::Applied;
  }

  PdfSavedItemsLegacyAccess callbacks() {
    return {this, FakeLegacy::count, FakeLegacy::read, FakeLegacy::add, FakeLegacy::remove, FakeLegacy::clear};
  }
};

enum class AmbiguousLegacyOperation : uint8_t {
  Add,
  Remove,
  Clear,
};

struct StaleBookmarkLegacy {
  static constexpr const char* kPath = "/books/ambiguous-bookmark.pdf";
  static constexpr const char* kTitle = "Bookmark ambiguity";
  static constexpr const char* kAuthor = "Author";

  BookmarkStore resident;
  AmbiguousLegacyOperation operation = AmbiguousLegacyOperation::Add;
  bool durableMutationApplied = false;

  static bool count(void* const context, uint16_t* const output) {
    if (context == nullptr || output == nullptr) return false;
    const auto& entries = static_cast<StaleBookmarkLegacy*>(context)->resident.getBookmarks();
    if (entries.size() > UINT16_MAX) return false;
    *output = static_cast<uint16_t>(entries.size());
    return true;
  }

  static bool read(void* const context, const uint16_t index, uint16_t* const output) {
    if (context == nullptr || output == nullptr) return false;
    const auto& entries = static_cast<StaleBookmarkLegacy*>(context)->resident.getBookmarks();
    if (index >= entries.size()) return false;
    *output = entries[index].paragraphIndex;
    return true;
  }

  static PdfSavedItemsLegacyMutationResult add(void* const context, const uint16_t itemId) {
    auto& self = *static_cast<StaleBookmarkLegacy*>(context);
    if (self.operation != AmbiguousLegacyOperation::Add) return PdfSavedItemsLegacyMutationResult::Rejected;
    BookmarkStore durable;
    self.durableMutationApplied =
        durable.loadForBook(kPath, kTitle, kAuthor, "pdf") &&
        durable.addPdfBookmark(1, 0.5F, "Chapter", itemId, "durable") == BookmarkStore::AddResult::Added;
    return self.durableMutationApplied ? PdfSavedItemsLegacyMutationResult::Ambiguous
                                       : PdfSavedItemsLegacyMutationResult::Rejected;
  }

  static PdfSavedItemsLegacyMutationResult remove(void* const context, const uint16_t itemId) {
    auto& self = *static_cast<StaleBookmarkLegacy*>(context);
    if (self.operation != AmbiguousLegacyOperation::Remove) return PdfSavedItemsLegacyMutationResult::Rejected;
    BookmarkStore durable;
    self.durableMutationApplied =
        durable.loadForBook(kPath, kTitle, kAuthor, "pdf") && durable.removePdfBookmark(itemId);
    return self.durableMutationApplied ? PdfSavedItemsLegacyMutationResult::Ambiguous
                                       : PdfSavedItemsLegacyMutationResult::Rejected;
  }

  static PdfSavedItemsLegacyMutationResult clear(void* const context) {
    auto& self = *static_cast<StaleBookmarkLegacy*>(context);
    if (self.operation != AmbiguousLegacyOperation::Clear) return PdfSavedItemsLegacyMutationResult::Rejected;
    BookmarkStore durable;
    self.durableMutationApplied =
        durable.loadForBook(kPath, kTitle, kAuthor, "pdf") && durable.clearPdfBookmarks();
    return self.durableMutationApplied ? PdfSavedItemsLegacyMutationResult::Ambiguous
                                       : PdfSavedItemsLegacyMutationResult::Rejected;
  }

  PdfSavedItemsLegacyAccess callbacks() {
    return {this, StaleBookmarkLegacy::count, StaleBookmarkLegacy::read, StaleBookmarkLegacy::add,
            StaleBookmarkLegacy::remove, StaleBookmarkLegacy::clear};
  }
};

struct StaleClippingLegacy {
  static constexpr const char* kPath = "/books/ambiguous-clipping.pdf";
  static constexpr const char* kTitle = "Clipping ambiguity";
  static constexpr const char* kAuthor = "Author";

  ClippingStore resident;
  AmbiguousLegacyOperation operation = AmbiguousLegacyOperation::Add;
  bool durableMutationApplied = false;

  static bool count(void* const context, uint16_t* const output) {
    if (context == nullptr || output == nullptr) return false;
    const auto& entries = static_cast<StaleClippingLegacy*>(context)->resident.getClippings();
    if (entries.size() > UINT16_MAX) return false;
    *output = static_cast<uint16_t>(entries.size());
    return true;
  }

  static bool read(void* const context, const uint16_t index, uint16_t* const output) {
    if (context == nullptr || output == nullptr) return false;
    const auto& entries = static_cast<StaleClippingLegacy*>(context)->resident.getClippings();
    if (index >= entries.size()) return false;
    *output = entries[index].paragraphIndex;
    return true;
  }

  static PdfSavedItemsLegacyMutationResult add(void* const context, const uint16_t itemId) {
    auto& self = *static_cast<StaleClippingLegacy*>(context);
    if (self.operation != AmbiguousLegacyOperation::Add) return PdfSavedItemsLegacyMutationResult::Rejected;
    ClippingStore durable;
    self.durableMutationApplied =
        durable.loadForBook(kPath, kTitle, kAuthor, "pdf") &&
        durable.addPdfClipping(1, 0, 0, 1, 0, 0, 1, "Chapter", itemId, "durable") ==
            ClippingStore::AddResult::Added;
    return self.durableMutationApplied ? PdfSavedItemsLegacyMutationResult::Ambiguous
                                       : PdfSavedItemsLegacyMutationResult::Rejected;
  }

  static PdfSavedItemsLegacyMutationResult remove(void* const context, const uint16_t itemId) {
    auto& self = *static_cast<StaleClippingLegacy*>(context);
    if (self.operation != AmbiguousLegacyOperation::Remove) return PdfSavedItemsLegacyMutationResult::Rejected;
    ClippingStore durable;
    self.durableMutationApplied =
        durable.loadForBook(kPath, kTitle, kAuthor, "pdf") && durable.removePdfClipping(itemId);
    return self.durableMutationApplied ? PdfSavedItemsLegacyMutationResult::Ambiguous
                                       : PdfSavedItemsLegacyMutationResult::Rejected;
  }

  static PdfSavedItemsLegacyMutationResult clear(void* const context) {
    auto& self = *static_cast<StaleClippingLegacy*>(context);
    if (self.operation != AmbiguousLegacyOperation::Clear) return PdfSavedItemsLegacyMutationResult::Rejected;
    ClippingStore durable;
    self.durableMutationApplied =
        durable.loadForBook(kPath, kTitle, kAuthor, "pdf") && durable.clearPdfClippings();
    return self.durableMutationApplied ? PdfSavedItemsLegacyMutationResult::Ambiguous
                                       : PdfSavedItemsLegacyMutationResult::Rejected;
  }

  PdfSavedItemsLegacyAccess callbacks() {
    return {this, StaleClippingLegacy::count, StaleClippingLegacy::read, StaleClippingLegacy::add,
            StaleClippingLegacy::remove, StaleClippingLegacy::clear};
  }
};

PdfSavedItem savedItem(const PdfSavedItemKind kind, const uint16_t itemId, const uint32_t ordinal = 1) {
  PdfSavedItem item{};
  item.itemId = itemId;
  item.kind = kind;
  item.flags = PDF_SAVED_ITEM_HAS_START_SEMANTIC;
  item.startGlobalWordOrdinal = ordinal;
  item.sectionIndex = 1;
  std::snprintf(item.startBlockAnchor, sizeof(item.startBlockAnchor), "b%08u", ordinal);
  if (kind == PdfSavedItemKind::Clipping) {
    item.flags |= PDF_SAVED_ITEM_HAS_END_SEMANTIC;
    item.endGlobalWordOrdinal = ordinal;
    item.endBlockWordOffset = 1;
    std::snprintf(item.endBlockAnchor, sizeof(item.endBlockAnchor), "b%08u", ordinal);
  }
  return item;
}

struct SessionFixture {
  FakePersistence persistence;
  FakeLegacy bookmarks;
  FakeLegacy clippings;
  std::array<PdfSavedItem, PDF_SAVED_ITEMS_MAX_RECORDS> items{};
  PdfSavedItemsBuffer buffer{items.data(), static_cast<uint16_t>(items.size()), 0};
  PdfSavedItemsSession session;

  PdfStatus begin() {
    return session.begin(&buffer, persistence.callbacks(), bookmarks.callbacks(), clippings.callbacks());
  }
};

PdfSourceIdentity sessionIdentity() {
  PdfSourceIdentity source{};
  source.size = 4096;
  source.modificationTime = {true, 123456};
  source.headFingerprint = 0x0123456789abcdefULL;
  source.tailFingerprint = 0xfedcba9876543210ULL;
  return source;
}

struct RealStorePersistence {
  PdfSavedItemsStore* store = nullptr;

  static PdfStatus load(void* const context, PdfSavedItemsBuffer* const output) {
    return static_cast<RealStorePersistence*>(context)->store->load(output);
  }
  static PdfStatus save(void* const context, const PdfSavedItem* const items, const uint16_t count) {
    return static_cast<RealStorePersistence*>(context)->store->save(items, count);
  }
  static PdfStatus validate(void* const context, const PdfSavedItem& item) {
    return static_cast<RealStorePersistence*>(context)->store->validate(item);
  }
  PdfSavedItemsPersistence callbacks() {
    return {this, RealStorePersistence::load, RealStorePersistence::save, RealStorePersistence::validate};
  }
};

TEST(PdfSavedItemsSessionTest, EmptySuccessfulLoadEnablesBothKindsWithoutWriting) {
  SessionFixture fixture;
  EXPECT_EQ(PDF_SAVED_ITEMS_MAX_RECORDS * sizeof(PdfSavedItem), 7168U);
  EXPECT_LE(sizeof(PdfSavedItemsSession), 160U);
  ASSERT_TRUE(fixture.begin());
  EXPECT_EQ(fixture.buffer.count, 0);
  EXPECT_TRUE(fixture.session.supports(PdfSavedItemKind::Bookmark));
  EXPECT_TRUE(fixture.session.supports(PdfSavedItemKind::Clipping));
  EXPECT_TRUE(fixture.session.canAllocateItemIds());
  EXPECT_FALSE(fixture.session.dirty());
  EXPECT_EQ(fixture.persistence.saveCalls, 0);
}

TEST(PdfSavedItemsSessionTest, DurableBookmarkFailAfterReloadsStaleResidentBeforeReconciliation) {
  for (const AmbiguousLegacyOperation operation :
       {AmbiguousLegacyOperation::Add, AmbiguousLegacyOperation::Remove, AmbiguousLegacyOperation::Clear}) {
    SCOPED_TRACE(static_cast<unsigned>(operation));
    Storage.reset();
    StaleBookmarkLegacy legacy;
    legacy.operation = operation;
    ASSERT_TRUE(legacy.resident.loadForBook(StaleBookmarkLegacy::kPath, StaleBookmarkLegacy::kTitle,
                                            StaleBookmarkLegacy::kAuthor, "pdf"));

    FakePersistence persistence;
    if (operation != AmbiguousLegacyOperation::Add) {
      ASSERT_EQ(legacy.resident.addPdfBookmark(1, 0.5F, "Chapter", 7, "resident"),
                BookmarkStore::AddResult::Added);
      persistence.durable[0] = savedItem(PdfSavedItemKind::Bookmark, 7);
      persistence.durableCount = 1;
    }
    FakeLegacy clippings;
    std::array<PdfSavedItem, PDF_SAVED_ITEMS_MAX_RECORDS> items{};
    PdfSavedItemsBuffer buffer{items.data(), static_cast<uint16_t>(items.size()), 0};
    PdfSavedItemsSession session;
    ASSERT_TRUE(session.begin(&buffer, persistence.callbacks(), legacy.callbacks(), clippings.callbacks()));

    const PdfSavedItemsSessionResult mutation =
        operation == AmbiguousLegacyOperation::Add
            ? session.add(savedItem(PdfSavedItemKind::Bookmark, 0), nullptr)
            : (operation == AmbiguousLegacyOperation::Remove
                   ? session.remove(PdfSavedItemKind::Bookmark, 7)
                   : session.clear(PdfSavedItemKind::Bookmark));
    ASSERT_EQ(mutation, PdfSavedItemsSessionResult::ReloadRequired);
    ASSERT_TRUE(legacy.durableMutationApplied);
    EXPECT_FALSE(session.supports(PdfSavedItemKind::Bookmark));
    EXPECT_EQ(legacy.resident.getBookmarks().empty(), operation == AmbiguousLegacyOperation::Add);

    ASSERT_TRUE(legacy.resident.reloadPdfFromDisk());
    EXPECT_EQ(legacy.resident.getBookmarks().empty(), operation != AmbiguousLegacyOperation::Add);

    std::array<PdfSavedItem, PDF_SAVED_ITEMS_MAX_RECORDS> reloadedItems{};
    PdfSavedItemsBuffer reloadedBuffer{reloadedItems.data(), static_cast<uint16_t>(reloadedItems.size()), 0};
    PdfSavedItemsSession reloaded;
    ASSERT_TRUE(reloaded.begin(&reloadedBuffer, persistence.callbacks(), legacy.callbacks(), clippings.callbacks()));
    if (operation == AmbiguousLegacyOperation::Add) {
      EXPECT_NE(reloaded.find(PdfSavedItemKind::Bookmark, 1), nullptr);
      EXPECT_EQ(reloaded.queueJump(PdfSavedItemKind::Bookmark, 1), PdfSavedItemsSessionResult::Applied);
    } else {
      EXPECT_EQ(reloaded.find(PdfSavedItemKind::Bookmark, 7), nullptr);
      ASSERT_TRUE(reloaded.flush());
      EXPECT_EQ(persistence.durableCount, 0);
    }
    EXPECT_LE(Storage.maximumOpenHandles(), 1U);
  }
}

TEST(PdfSavedItemsSessionTest, DurableClippingFailAfterReloadsStaleResidentBeforeReconciliation) {
  for (const AmbiguousLegacyOperation operation :
       {AmbiguousLegacyOperation::Add, AmbiguousLegacyOperation::Remove, AmbiguousLegacyOperation::Clear}) {
    SCOPED_TRACE(static_cast<unsigned>(operation));
    Storage.reset();
    StaleClippingLegacy legacy;
    legacy.operation = operation;
    ASSERT_TRUE(legacy.resident.loadForBook(StaleClippingLegacy::kPath, StaleClippingLegacy::kTitle,
                                            StaleClippingLegacy::kAuthor, "pdf"));

    FakePersistence persistence;
    if (operation != AmbiguousLegacyOperation::Add) {
      ASSERT_EQ(legacy.resident.addPdfClipping(1, 0, 0, 1, 0, 0, 1, "Chapter", 7, "resident"),
                ClippingStore::AddResult::Added);
      persistence.durable[0] = savedItem(PdfSavedItemKind::Clipping, 7);
      persistence.durableCount = 1;
    }
    FakeLegacy bookmarks;
    std::array<PdfSavedItem, PDF_SAVED_ITEMS_MAX_RECORDS> items{};
    PdfSavedItemsBuffer buffer{items.data(), static_cast<uint16_t>(items.size()), 0};
    PdfSavedItemsSession session;
    ASSERT_TRUE(session.begin(&buffer, persistence.callbacks(), bookmarks.callbacks(), legacy.callbacks()));

    const PdfSavedItemsSessionResult mutation =
        operation == AmbiguousLegacyOperation::Add
            ? session.add(savedItem(PdfSavedItemKind::Clipping, 0), nullptr)
            : (operation == AmbiguousLegacyOperation::Remove
                   ? session.remove(PdfSavedItemKind::Clipping, 7)
                   : session.clear(PdfSavedItemKind::Clipping));
    ASSERT_EQ(mutation, PdfSavedItemsSessionResult::ReloadRequired);
    ASSERT_TRUE(legacy.durableMutationApplied);
    EXPECT_FALSE(session.supports(PdfSavedItemKind::Clipping));
    EXPECT_EQ(legacy.resident.getClippings().empty(), operation == AmbiguousLegacyOperation::Add);

    ASSERT_TRUE(legacy.resident.reloadPdfFromDisk());
    EXPECT_EQ(legacy.resident.getClippings().empty(), operation != AmbiguousLegacyOperation::Add);

    std::array<PdfSavedItem, PDF_SAVED_ITEMS_MAX_RECORDS> reloadedItems{};
    PdfSavedItemsBuffer reloadedBuffer{reloadedItems.data(), static_cast<uint16_t>(reloadedItems.size()), 0};
    PdfSavedItemsSession reloaded;
    ASSERT_TRUE(reloaded.begin(&reloadedBuffer, persistence.callbacks(), bookmarks.callbacks(), legacy.callbacks()));
    if (operation == AmbiguousLegacyOperation::Add) {
      EXPECT_NE(reloaded.find(PdfSavedItemKind::Clipping, 1), nullptr);
      EXPECT_EQ(reloaded.queueJump(PdfSavedItemKind::Clipping, 1), PdfSavedItemsSessionResult::Applied);
    } else {
      EXPECT_EQ(reloaded.find(PdfSavedItemKind::Clipping, 7), nullptr);
      ASSERT_TRUE(reloaded.flush());
      EXPECT_EQ(persistence.durableCount, 0);
    }
    EXPECT_LE(Storage.maximumOpenHandles(), 1U);
  }
}

TEST(PdfSavedItemsSessionTest, RealStoreInvalidOffsetMeansEmptyFirstBookAndEnablesTheSession) {
  PdfTestCacheIo files;
  files.addDirectory("/cache");
  PdfSavedItemsStore store;
  ASSERT_TRUE(store.initialize(files.io(), "/cache", sessionIdentity(), 512));
  std::array<PdfSavedItem, PDF_SAVED_ITEMS_MAX_RECORDS> items{};
  PdfSavedItemsBuffer buffer{items.data(), static_cast<uint16_t>(items.size()), 99};
  EXPECT_EQ(store.load(&buffer).error, PdfError::InvalidOffset);

  FakeLegacy bookmarks;
  FakeLegacy clippings;
  RealStorePersistence persistence{&store};
  PdfSavedItemsSession session;
  ASSERT_TRUE(session.begin(&buffer, persistence.callbacks(), bookmarks.callbacks(), clippings.callbacks()));
  EXPECT_EQ(buffer.count, 0);
  EXPECT_TRUE(session.supports(PdfSavedItemKind::Bookmark));
  EXPECT_TRUE(session.supports(PdfSavedItemKind::Clipping));
  EXPECT_TRUE(session.canAllocateItemIds());
}

TEST(PdfSavedItemsSessionTest, RealStoreCloseFailurePropagatesAndMasksAllSessionCapabilities) {
  PdfTestCacheIo files;
  files.addDirectory("/cache");
  PdfSavedItemsStore store;
  ASSERT_TRUE(store.initialize(files.io(), "/cache", sessionIdentity(), 512));
  const PdfSavedItem durable = savedItem(PdfSavedItemKind::Bookmark, 7);
  ASSERT_TRUE(store.save(&durable, 1));
  files.fail(PdfTestFaultPoint::Close);

  std::array<PdfSavedItem, PDF_SAVED_ITEMS_MAX_RECORDS> items{};
  PdfSavedItemsBuffer buffer{items.data(), static_cast<uint16_t>(items.size()), 99};
  FakeLegacy bookmarks;
  FakeLegacy clippings;
  RealStorePersistence persistence{&store};
  PdfSavedItemsSession session;
  EXPECT_EQ(session.begin(&buffer, persistence.callbacks(), bookmarks.callbacks(), clippings.callbacks()).error,
            PdfError::IoFailure);
  EXPECT_EQ(buffer.count, 0);
  EXPECT_EQ(session.capabilityMask(), 0);
  EXPECT_EQ(bookmarks.countCalls, 0);
  EXPECT_EQ(clippings.countCalls, 0);
}

TEST(PdfSavedItemsSessionTest, TransientPsitLoadFailureMasksAllCapabilitiesAndDoesNotInspectLegacy) {
  SessionFixture fixture;
  fixture.persistence.loadStatus = PdfStatus::failure(PdfError::IoFailure);
  EXPECT_EQ(fixture.begin().error, PdfError::IoFailure);
  EXPECT_EQ(fixture.buffer.count, 0);
  EXPECT_EQ(fixture.session.capabilityMask(), 0);
  EXPECT_EQ(fixture.bookmarks.countCalls, 0);
  EXPECT_EQ(fixture.clippings.countCalls, 0);
}

TEST(PdfSavedItemsSessionTest, PartialLegacyLoadMasksOnlyThatKindAndPreservesItsPsitBytes) {
  SessionFixture fixture;
  fixture.persistence.durable[0] = savedItem(PdfSavedItemKind::Bookmark, 10);
  fixture.persistence.durable[1] = savedItem(PdfSavedItemKind::Clipping, 20);
  fixture.persistence.durableCount = 2;
  fixture.bookmarks.ids[0] = 10;
  fixture.bookmarks.size = 1;
  fixture.bookmarks.failReadIndex = 0;
  fixture.clippings.ids[0] = 20;
  fixture.clippings.size = 1;

  ASSERT_TRUE(fixture.begin());
  EXPECT_FALSE(fixture.session.supports(PdfSavedItemKind::Bookmark));
  EXPECT_TRUE(fixture.session.supports(PdfSavedItemKind::Clipping));
  EXPECT_FALSE(fixture.session.canAllocateItemIds());
  ASSERT_EQ(fixture.buffer.count, 2);
  EXPECT_EQ(fixture.buffer.items[0].itemId, 10);
  EXPECT_EQ(fixture.buffer.items[1].itemId, 20);
  EXPECT_FALSE(fixture.session.dirty());
}

TEST(PdfSavedItemsSessionTest, ReconcileKeepsOnlyExactUniqueKindAndIdMatches) {
  SessionFixture fixture;
  fixture.persistence.durable[0] = savedItem(PdfSavedItemKind::Bookmark, 1);
  fixture.persistence.durable[1] = savedItem(PdfSavedItemKind::Bookmark, 2);
  fixture.persistence.durable[2] = savedItem(PdfSavedItemKind::Bookmark, 3);
  fixture.persistence.durable[3] = savedItem(PdfSavedItemKind::Bookmark, 4);
  fixture.persistence.durable[4] = savedItem(PdfSavedItemKind::Bookmark, 5);
  fixture.persistence.durable[5] = savedItem(PdfSavedItemKind::Bookmark, 5);
  fixture.persistence.durable[6] = savedItem(PdfSavedItemKind::Clipping, 6);
  fixture.persistence.durableCount = 7;
  fixture.bookmarks.ids = {1, 3, 3, 5};
  fixture.bookmarks.size = 4;
  fixture.clippings.ids = {4, 6};
  fixture.clippings.size = 2;

  ASSERT_TRUE(fixture.begin());
  ASSERT_EQ(fixture.buffer.count, 2);
  EXPECT_EQ(fixture.buffer.items[0].itemId, 1);
  EXPECT_EQ(fixture.buffer.items[0].kind, PdfSavedItemKind::Bookmark);
  EXPECT_EQ(fixture.buffer.items[1].itemId, 6);
  EXPECT_EQ(fixture.buffer.items[1].kind, PdfSavedItemKind::Clipping);
  EXPECT_TRUE(fixture.session.dirty());

  ASSERT_TRUE(fixture.session.flush());
  EXPECT_FALSE(fixture.session.dirty());
  ASSERT_EQ(fixture.persistence.durableCount, 2);
  EXPECT_EQ(fixture.persistence.durable[0].itemId, 1);
  EXPECT_EQ(fixture.persistence.durable[1].itemId, 6);
}

TEST(PdfSavedItemsSessionTest, IdAllocationWrapsAndSkipsReservedAndEveryUsedLegacyId) {
  SessionFixture fixture;
  fixture.persistence.durable[0] = savedItem(PdfSavedItemKind::Bookmark, UINT16_MAX);
  fixture.persistence.durable[1] = savedItem(PdfSavedItemKind::Bookmark, UINT16_MAX - 1U);
  fixture.persistence.durable[2] = savedItem(PdfSavedItemKind::Clipping, 1);
  fixture.persistence.durableCount = 3;
  fixture.bookmarks.ids = {UINT16_MAX, UINT16_MAX - 1U};
  fixture.bookmarks.size = 2;
  fixture.clippings.ids = {1};
  fixture.clippings.size = 1;

  ASSERT_TRUE(fixture.begin());
  ASSERT_EQ(fixture.buffer.count, 2);
  EXPECT_EQ(fixture.buffer.items[0].itemId, UINT16_MAX - 1U);
  EXPECT_EQ(fixture.buffer.items[1].itemId, 1);

  uint16_t assigned = 0;
  const PdfSavedItem item = savedItem(PdfSavedItemKind::Bookmark, 0, 7);
  EXPECT_EQ(fixture.session.add(item, &assigned), PdfSavedItemsSessionResult::Applied);
  EXPECT_EQ(assigned, 2);
  EXPECT_NE(assigned, 0);
  EXPECT_NE(assigned, UINT16_MAX);
}

TEST(PdfSavedItemsSessionTest, EnforcesSixtyFourItemsOfEachKindBeforeLegacyMutation) {
  SessionFixture fixture;
  for (uint16_t index = 0; index < PDF_SAVED_ITEMS_MAX_BOOKMARKS; ++index) {
    const uint16_t itemId = static_cast<uint16_t>(index + 1);
    fixture.persistence.durable[index] = savedItem(PdfSavedItemKind::Bookmark, itemId, index);
    fixture.bookmarks.ids[index] = itemId;
  }
  for (uint16_t index = 0; index < PDF_SAVED_ITEMS_MAX_CLIPPINGS; ++index) {
    const uint16_t itemId = static_cast<uint16_t>(100 + index);
    fixture.persistence.durable[PDF_SAVED_ITEMS_MAX_BOOKMARKS + index] =
        savedItem(PdfSavedItemKind::Clipping, itemId, 100 + index);
    fixture.clippings.ids[index] = itemId;
  }
  fixture.persistence.durableCount = PDF_SAVED_ITEMS_MAX_RECORDS;
  fixture.bookmarks.size = PDF_SAVED_ITEMS_MAX_BOOKMARKS;
  fixture.clippings.size = PDF_SAVED_ITEMS_MAX_CLIPPINGS;

  ASSERT_TRUE(fixture.begin());
  EXPECT_EQ(fixture.session.add(savedItem(PdfSavedItemKind::Bookmark, 0), nullptr),
            PdfSavedItemsSessionResult::LimitReached);
  EXPECT_EQ(fixture.session.add(savedItem(PdfSavedItemKind::Clipping, 0), nullptr),
            PdfSavedItemsSessionResult::LimitReached);
  EXPECT_EQ(fixture.bookmarks.addCalls, 0);
  EXPECT_EQ(fixture.clippings.addCalls, 0);
}

TEST(PdfSavedItemsSessionTest, AddDurablyPersistsSemanticIntentBeforeLegacyMutation) {
  SessionFixture fixture;
  OperationTrace trace;
  fixture.persistence.trace = &trace;
  fixture.bookmarks.trace = &trace;
  ASSERT_TRUE(fixture.begin());

  uint16_t assigned = 0;
  EXPECT_EQ(fixture.session.add(savedItem(PdfSavedItemKind::Bookmark, 0), &assigned),
            PdfSavedItemsSessionResult::Applied);
  ASSERT_EQ(trace.count, 2);
  EXPECT_EQ(trace.operations[0], 'S');
  EXPECT_EQ(trace.operations[1], 'A');
  EXPECT_TRUE(fixture.bookmarks.contains(assigned));
  EXPECT_EQ(fixture.bookmarks.addCalls, 1);
  EXPECT_FALSE(fixture.session.dirty());
  ASSERT_EQ(fixture.persistence.durableCount, 1);
  EXPECT_EQ(fixture.persistence.durable[0].itemId, assigned);
}

TEST(PdfSavedItemsSessionTest, PsitAddFailureNeverCreatesAVisibleUnqueueableLegacyId) {
  SessionFixture fixture;
  OperationTrace trace;
  fixture.persistence.trace = &trace;
  fixture.bookmarks.trace = &trace;
  ASSERT_TRUE(fixture.begin());
  fixture.persistence.saveStatus = PdfStatus::failure(PdfError::IoFailure);

  EXPECT_NE(fixture.session.add(savedItem(PdfSavedItemKind::Bookmark, 0), nullptr),
            PdfSavedItemsSessionResult::Applied);
  ASSERT_EQ(trace.count, 1);
  EXPECT_EQ(trace.operations[0], 'S');
  EXPECT_EQ(fixture.bookmarks.addCalls, 0);
  EXPECT_EQ(fixture.bookmarks.size, 0);
  EXPECT_FALSE(fixture.session.supports(PdfSavedItemKind::Bookmark));
}

TEST(PdfSavedItemsSessionTest, DefinitiveLegacyAddFailureDurablyRestoresPriorPsitSnapshot) {
  SessionFixture fixture;
  OperationTrace trace;
  fixture.persistence.trace = &trace;
  fixture.bookmarks.trace = &trace;
  fixture.bookmarks.failAdd = true;
  ASSERT_TRUE(fixture.begin());

  EXPECT_EQ(fixture.session.add(savedItem(PdfSavedItemKind::Bookmark, 0), nullptr),
            PdfSavedItemsSessionResult::LegacyFailure);
  ASSERT_EQ(trace.count, 3);
  EXPECT_EQ(trace.operations[0], 'S');
  EXPECT_EQ(trace.operations[1], 'A');
  EXPECT_EQ(trace.operations[2], 'S');
  EXPECT_EQ(fixture.buffer.count, 0);
  EXPECT_FALSE(fixture.session.dirty());
  EXPECT_EQ(fixture.persistence.saveCalls, 2);
  EXPECT_EQ(fixture.persistence.durableCount, 0);
}

TEST(PdfSavedItemsSessionTest, AmbiguousLegacyAddRetainsIntentButMasksItUntilRebootReconciliation) {
  for (const bool legacyApplied : {false, true}) {
    SCOPED_TRACE(legacyApplied);
    SessionFixture fixture;
    fixture.bookmarks.ambiguousAdd = true;
    fixture.bookmarks.applyAmbiguousMutation = legacyApplied;
    ASSERT_TRUE(fixture.begin());

    uint16_t assigned = UINT16_MAX;
    EXPECT_EQ(fixture.session.add(savedItem(PdfSavedItemKind::Bookmark, 0), &assigned),
              PdfSavedItemsSessionResult::ReloadRequired);
    EXPECT_EQ(assigned, UINT16_MAX);
    EXPECT_FALSE(fixture.session.supports(PdfSavedItemKind::Bookmark));
    ASSERT_EQ(fixture.persistence.durableCount, 1);
    EXPECT_EQ(fixture.persistence.durable[0].itemId, 1);

    std::array<PdfSavedItem, PDF_SAVED_ITEMS_MAX_RECORDS> rebootItems{};
    PdfSavedItemsBuffer rebootBuffer{rebootItems.data(), static_cast<uint16_t>(rebootItems.size()), 0};
    PdfSavedItemsSession rebooted;
    ASSERT_TRUE(rebooted.begin(&rebootBuffer, fixture.persistence.callbacks(), fixture.bookmarks.callbacks(),
                               fixture.clippings.callbacks()));
    if (legacyApplied) {
      ASSERT_NE(rebooted.find(PdfSavedItemKind::Bookmark, 1), nullptr);
      EXPECT_EQ(rebooted.queueJump(PdfSavedItemKind::Bookmark, 1), PdfSavedItemsSessionResult::Applied);
    } else {
      EXPECT_EQ(rebooted.find(PdfSavedItemKind::Bookmark, 1), nullptr);
      EXPECT_TRUE(rebooted.dirty());
      ASSERT_TRUE(rebooted.flush());
      EXPECT_EQ(fixture.persistence.durableCount, 0);
    }
  }
}

TEST(PdfSavedItemsSessionTest, FailedPsitRollbackAfterRejectedLegacyAddForcesRebootReconciliation) {
  SessionFixture fixture;
  fixture.bookmarks.failAdd = true;
  fixture.persistence.failSaveCall = 2;
  ASSERT_TRUE(fixture.begin());

  EXPECT_EQ(fixture.session.add(savedItem(PdfSavedItemKind::Bookmark, 0), nullptr),
            PdfSavedItemsSessionResult::ReloadRequired);
  EXPECT_FALSE(fixture.session.supports(PdfSavedItemKind::Bookmark));
  EXPECT_EQ(fixture.bookmarks.size, 0);
  ASSERT_EQ(fixture.persistence.durableCount, 1);

  fixture.persistence.failSaveCall = 0;
  std::array<PdfSavedItem, PDF_SAVED_ITEMS_MAX_RECORDS> rebootItems{};
  PdfSavedItemsBuffer rebootBuffer{rebootItems.data(), static_cast<uint16_t>(rebootItems.size()), 0};
  PdfSavedItemsSession rebooted;
  ASSERT_TRUE(rebooted.begin(&rebootBuffer, fixture.persistence.callbacks(), fixture.bookmarks.callbacks(),
                             fixture.clippings.callbacks()));
  EXPECT_EQ(rebooted.find(PdfSavedItemKind::Bookmark, 1), nullptr);
  ASSERT_TRUE(rebooted.flush());
  EXPECT_EQ(fixture.persistence.durableCount, 0);
}

TEST(PdfSavedItemsSessionTest, DefinitiveLegacyRemoveAndClearRejectionNeverMutatesPsit) {
  for (const bool clearOperation : {false, true}) {
    SCOPED_TRACE(clearOperation);
    SessionFixture fixture;
    OperationTrace trace;
    fixture.persistence.trace = &trace;
    fixture.bookmarks.trace = &trace;
    fixture.persistence.durable[0] = savedItem(PdfSavedItemKind::Bookmark, 7);
    fixture.persistence.durableCount = 1;
    fixture.bookmarks.ids[0] = 7;
    fixture.bookmarks.size = 1;
    fixture.bookmarks.failRemove = !clearOperation;
    fixture.bookmarks.failClear = clearOperation;
    ASSERT_TRUE(fixture.begin());

    const PdfSavedItemsSessionResult result =
        clearOperation ? fixture.session.clear(PdfSavedItemKind::Bookmark)
                       : fixture.session.remove(PdfSavedItemKind::Bookmark, 7);
    EXPECT_EQ(result, PdfSavedItemsSessionResult::LegacyFailure);
    ASSERT_EQ(trace.count, 1);
    EXPECT_EQ(trace.operations[0], clearOperation ? 'C' : 'R');
    EXPECT_EQ(fixture.persistence.saveCalls, 0);
    EXPECT_EQ(fixture.persistence.durableCount, 1);
    EXPECT_NE(fixture.session.find(PdfSavedItemKind::Bookmark, 7), nullptr);
    EXPECT_TRUE(fixture.bookmarks.contains(7));
    EXPECT_TRUE(fixture.session.supports(PdfSavedItemKind::Bookmark));
  }
}

TEST(PdfSavedItemsSessionTest, AmbiguousLegacyRemoveAndClearRetainPsitUntilRebootReconciliation) {
  for (const bool clearOperation : {false, true}) {
    for (const bool legacyApplied : {false, true}) {
      SCOPED_TRACE(clearOperation);
      SCOPED_TRACE(legacyApplied);
      SessionFixture fixture;
      fixture.persistence.durable[0] = savedItem(PdfSavedItemKind::Bookmark, 7);
      fixture.persistence.durableCount = 1;
      fixture.bookmarks.ids[0] = 7;
      fixture.bookmarks.size = 1;
      fixture.bookmarks.ambiguousRemove = !clearOperation;
      fixture.bookmarks.ambiguousClear = clearOperation;
      fixture.bookmarks.applyAmbiguousMutation = legacyApplied;
      ASSERT_TRUE(fixture.begin());

      const PdfSavedItemsSessionResult result =
          clearOperation ? fixture.session.clear(PdfSavedItemKind::Bookmark)
                         : fixture.session.remove(PdfSavedItemKind::Bookmark, 7);
      EXPECT_EQ(result, PdfSavedItemsSessionResult::ReloadRequired);
      EXPECT_FALSE(fixture.session.supports(PdfSavedItemKind::Bookmark));
      ASSERT_EQ(fixture.persistence.durableCount, 1);
      EXPECT_EQ(fixture.persistence.saveCalls, 0);

      fixture.bookmarks.ambiguousRemove = false;
      fixture.bookmarks.ambiguousClear = false;
      std::array<PdfSavedItem, PDF_SAVED_ITEMS_MAX_RECORDS> rebootItems{};
      PdfSavedItemsBuffer rebootBuffer{rebootItems.data(), static_cast<uint16_t>(rebootItems.size()), 0};
      PdfSavedItemsSession rebooted;
      ASSERT_TRUE(rebooted.begin(&rebootBuffer, fixture.persistence.callbacks(), fixture.bookmarks.callbacks(),
                                 fixture.clippings.callbacks()));
      if (legacyApplied) {
        EXPECT_EQ(rebooted.find(PdfSavedItemKind::Bookmark, 7), nullptr);
        EXPECT_TRUE(rebooted.dirty());
        ASSERT_TRUE(rebooted.flush());
        EXPECT_EQ(fixture.persistence.durableCount, 0);
      } else {
        EXPECT_NE(rebooted.find(PdfSavedItemKind::Bookmark, 7), nullptr);
        EXPECT_FALSE(rebooted.dirty());
        EXPECT_EQ(fixture.persistence.durableCount, 1);
      }
    }
  }
}

TEST(PdfSavedItemsSessionTest, EveryRealPsitAddSaveFaultAvoidsLegacyMutationAndReconcilesAfterReboot) {
  for (const PdfTestFaultPoint fault : {PdfTestFaultPoint::Write, PdfTestFaultPoint::Flush, PdfTestFaultPoint::Sync,
                                        PdfTestFaultPoint::Close}) {
    SCOPED_TRACE(static_cast<unsigned>(fault));
    PdfTestCacheIo files;
    files.addDirectory("/cache");
    PdfSavedItemsStore store;
    ASSERT_TRUE(store.initialize(files.io(), "/cache", sessionIdentity(), 512));
    ASSERT_TRUE(store.save(nullptr, 0));
    RealStorePersistence persistence{&store};
    FakeLegacy bookmarks;
    FakeLegacy clippings;
    std::array<PdfSavedItem, PDF_SAVED_ITEMS_MAX_RECORDS> items{};
    PdfSavedItemsBuffer buffer{items.data(), static_cast<uint16_t>(items.size()), 0};
    {
      PdfSavedItemsSession session;
      ASSERT_TRUE(session.begin(&buffer, persistence.callbacks(), bookmarks.callbacks(), clippings.callbacks()));
      files.fail(fault);
      EXPECT_EQ(session.add(savedItem(PdfSavedItemKind::Bookmark, 0), nullptr),
                PdfSavedItemsSessionResult::ReloadRequired);
      EXPECT_EQ(bookmarks.addCalls, 0);
      EXPECT_EQ(bookmarks.size, 0);
      EXPECT_FALSE(session.supports(PdfSavedItemKind::Bookmark));
      EXPECT_EQ(files.openHandleCount(), 0U);
    }

    files.clearFault();
    PdfSavedItemsStore rebootStore;
    ASSERT_TRUE(rebootStore.initialize(files.io(), "/cache", sessionIdentity(), 512));
    RealStorePersistence rebootPersistence{&rebootStore};
    std::array<PdfSavedItem, PDF_SAVED_ITEMS_MAX_RECORDS> rebootItems{};
    PdfSavedItemsBuffer rebootBuffer{rebootItems.data(), static_cast<uint16_t>(rebootItems.size()), 0};
    PdfSavedItemsSession rebooted;
    ASSERT_TRUE(
        rebooted.begin(&rebootBuffer, rebootPersistence.callbacks(), bookmarks.callbacks(), clippings.callbacks()));
    EXPECT_EQ(rebootBuffer.count, 0);
    if (rebooted.dirty()) {
      ASSERT_TRUE(rebooted.flush());
    }
    EXPECT_EQ(files.openHandleCount(), 0U);
  }
}

TEST(PdfSavedItemsSessionTest, EveryRealPsitRemoveSaveFaultReconcilesPsitOnlyIntentAfterReboot) {
  for (const PdfTestFaultPoint fault : {PdfTestFaultPoint::Write, PdfTestFaultPoint::Flush, PdfTestFaultPoint::Sync,
                                        PdfTestFaultPoint::Close}) {
    SCOPED_TRACE(static_cast<unsigned>(fault));
    PdfTestCacheIo files;
    files.addDirectory("/cache");
    PdfSavedItemsStore store;
    ASSERT_TRUE(store.initialize(files.io(), "/cache", sessionIdentity(), 512));
    const PdfSavedItem durable = savedItem(PdfSavedItemKind::Bookmark, 7);
    ASSERT_TRUE(store.save(&durable, 1));
    RealStorePersistence persistence{&store};
    FakeLegacy bookmarks;
    bookmarks.ids[0] = 7;
    bookmarks.size = 1;
    FakeLegacy clippings;
    std::array<PdfSavedItem, PDF_SAVED_ITEMS_MAX_RECORDS> items{};
    PdfSavedItemsBuffer buffer{items.data(), static_cast<uint16_t>(items.size()), 0};
    {
      PdfSavedItemsSession session;
      ASSERT_TRUE(session.begin(&buffer, persistence.callbacks(), bookmarks.callbacks(), clippings.callbacks()));
      files.fail(fault);
      EXPECT_EQ(session.remove(PdfSavedItemKind::Bookmark, 7), PdfSavedItemsSessionResult::ReloadRequired);
      EXPECT_EQ(bookmarks.removeCalls, 1);
      EXPECT_EQ(bookmarks.size, 0);
      EXPECT_FALSE(session.supports(PdfSavedItemKind::Bookmark));
      EXPECT_EQ(files.openHandleCount(), 0U);
    }

    files.clearFault();
    PdfSavedItemsStore rebootStore;
    ASSERT_TRUE(rebootStore.initialize(files.io(), "/cache", sessionIdentity(), 512));
    RealStorePersistence rebootPersistence{&rebootStore};
    std::array<PdfSavedItem, PDF_SAVED_ITEMS_MAX_RECORDS> rebootItems{};
    PdfSavedItemsBuffer rebootBuffer{rebootItems.data(), static_cast<uint16_t>(rebootItems.size()), 0};
    PdfSavedItemsSession rebooted;
    ASSERT_TRUE(
        rebooted.begin(&rebootBuffer, rebootPersistence.callbacks(), bookmarks.callbacks(), clippings.callbacks()));
    EXPECT_EQ(rebootBuffer.count, 0);
    if (rebooted.dirty()) {
      ASSERT_TRUE(rebooted.flush());
    }
    EXPECT_EQ(files.openHandleCount(), 0U);
  }
}

TEST(PdfSavedItemsSessionTest, EveryRealPsitClearSaveFaultReconcilesPsitOnlyIntentAfterReboot) {
  for (const PdfTestFaultPoint fault : {PdfTestFaultPoint::Write, PdfTestFaultPoint::Flush, PdfTestFaultPoint::Sync,
                                        PdfTestFaultPoint::Close}) {
    SCOPED_TRACE(static_cast<unsigned>(fault));
    PdfTestCacheIo files;
    files.addDirectory("/cache");
    PdfSavedItemsStore store;
    ASSERT_TRUE(store.initialize(files.io(), "/cache", sessionIdentity(), 512));
    const std::array<PdfSavedItem, 3> durable{
        savedItem(PdfSavedItemKind::Bookmark, 7),
        savedItem(PdfSavedItemKind::Bookmark, 8),
        savedItem(PdfSavedItemKind::Clipping, 9),
    };
    ASSERT_TRUE(store.save(durable.data(), static_cast<uint16_t>(durable.size())));
    RealStorePersistence persistence{&store};
    FakeLegacy bookmarks;
    bookmarks.ids = {7, 8};
    bookmarks.size = 2;
    FakeLegacy clippings;
    clippings.ids[0] = 9;
    clippings.size = 1;
    std::array<PdfSavedItem, PDF_SAVED_ITEMS_MAX_RECORDS> items{};
    PdfSavedItemsBuffer buffer{items.data(), static_cast<uint16_t>(items.size()), 0};
    {
      PdfSavedItemsSession session;
      ASSERT_TRUE(session.begin(&buffer, persistence.callbacks(), bookmarks.callbacks(), clippings.callbacks()));
      files.fail(fault);
      EXPECT_EQ(session.clear(PdfSavedItemKind::Bookmark), PdfSavedItemsSessionResult::ReloadRequired);
      EXPECT_EQ(bookmarks.clearCalls, 1);
      EXPECT_EQ(bookmarks.size, 0);
      EXPECT_FALSE(session.supports(PdfSavedItemKind::Bookmark));
      EXPECT_EQ(files.openHandleCount(), 0U);
    }

    files.clearFault();
    PdfSavedItemsStore rebootStore;
    ASSERT_TRUE(rebootStore.initialize(files.io(), "/cache", sessionIdentity(), 512));
    RealStorePersistence rebootPersistence{&rebootStore};
    std::array<PdfSavedItem, PDF_SAVED_ITEMS_MAX_RECORDS> rebootItems{};
    PdfSavedItemsBuffer rebootBuffer{rebootItems.data(), static_cast<uint16_t>(rebootItems.size()), 0};
    PdfSavedItemsSession rebooted;
    ASSERT_TRUE(
        rebooted.begin(&rebootBuffer, rebootPersistence.callbacks(), bookmarks.callbacks(), clippings.callbacks()));
    ASSERT_EQ(rebootBuffer.count, 1);
    EXPECT_EQ(rebootBuffer.items[0].itemId, 9);
    if (rebooted.dirty()) {
      ASSERT_TRUE(rebooted.flush());
    }
    EXPECT_EQ(files.openHandleCount(), 0U);
  }
}

TEST(PdfSavedItemsSessionTest, InvalidSemanticTemplateNeverMutatesLegacyOrPsitAndDoesNotBecomeDirty) {
  SessionFixture fixture;
  ASSERT_TRUE(fixture.begin());
  PdfSavedItem invalid = savedItem(PdfSavedItemKind::Bookmark, 0);
  invalid.flags = 0;

  EXPECT_EQ(fixture.session.add(invalid, nullptr), PdfSavedItemsSessionResult::InvalidItem);
  EXPECT_EQ(fixture.persistence.validateCalls, 1);
  EXPECT_EQ(fixture.bookmarks.addCalls, 0);
  EXPECT_EQ(fixture.persistence.saveCalls, 0);
  EXPECT_EQ(fixture.buffer.count, 0);
  EXPECT_FALSE(fixture.session.dirty());
}

TEST(PdfSavedItemsSessionTest, DeleteCannotResurrectAfterPsitSaveFailureAndRebootReconcile) {
  for (const bool psitDeleteCommitted : {false, true}) {
    SCOPED_TRACE(psitDeleteCommitted);
    SessionFixture fixture;
    OperationTrace trace;
    fixture.persistence.trace = &trace;
    fixture.bookmarks.trace = &trace;
    fixture.persistence.durable[0] = savedItem(PdfSavedItemKind::Bookmark, 7);
    fixture.persistence.durableCount = 1;
    fixture.bookmarks.ids[0] = 7;
    fixture.bookmarks.size = 1;
    ASSERT_TRUE(fixture.begin());
    fixture.persistence.saveStatus = PdfStatus::failure(PdfError::IoFailure);
    fixture.persistence.commitFailedSave = psitDeleteCommitted;

    EXPECT_EQ(fixture.session.remove(PdfSavedItemKind::Bookmark, 7),
              PdfSavedItemsSessionResult::ReloadRequired);
    ASSERT_EQ(trace.count, 2);
    EXPECT_EQ(trace.operations[0], 'R');
    EXPECT_EQ(trace.operations[1], 'S');
    EXPECT_FALSE(fixture.bookmarks.contains(7));
    EXPECT_EQ(fixture.bookmarks.removeCalls, 1);
    EXPECT_FALSE(fixture.session.supports(PdfSavedItemKind::Bookmark));
    EXPECT_EQ(fixture.persistence.durableCount, psitDeleteCommitted ? 0 : 1);

    fixture.persistence.saveStatus = PdfStatus::success();
    fixture.persistence.commitFailedSave = false;
    std::array<PdfSavedItem, PDF_SAVED_ITEMS_MAX_RECORDS> rebootItems{};
    PdfSavedItemsBuffer rebootBuffer{rebootItems.data(), static_cast<uint16_t>(rebootItems.size()), 0};
    PdfSavedItemsSession rebooted;
    ASSERT_TRUE(rebooted.begin(&rebootBuffer, fixture.persistence.callbacks(), fixture.bookmarks.callbacks(),
                               fixture.clippings.callbacks()));
    EXPECT_EQ(rebootBuffer.count, 0);
    EXPECT_EQ(rebooted.find(PdfSavedItemKind::Bookmark, 7), nullptr);
    EXPECT_FALSE(fixture.bookmarks.contains(7));
    EXPECT_EQ(rebooted.dirty(), !psitDeleteCommitted);
    ASSERT_TRUE(rebooted.flush());
    EXPECT_EQ(fixture.persistence.durableCount, 0);
  }
}

TEST(PdfSavedItemsSessionTest, ClearCannotResurrectItemsAfterPsitSaveFailure) {
  for (const bool psitClearCommitted : {false, true}) {
    SCOPED_TRACE(psitClearCommitted);
    SessionFixture fixture;
    OperationTrace trace;
    fixture.persistence.trace = &trace;
    fixture.bookmarks.trace = &trace;
    fixture.persistence.durable[0] = savedItem(PdfSavedItemKind::Bookmark, 7);
    fixture.persistence.durable[1] = savedItem(PdfSavedItemKind::Bookmark, 8);
    fixture.persistence.durable[2] = savedItem(PdfSavedItemKind::Clipping, 9);
    fixture.persistence.durableCount = 3;
    fixture.bookmarks.ids = {7, 8};
    fixture.bookmarks.size = 2;
    fixture.clippings.ids = {9};
    fixture.clippings.size = 1;
    ASSERT_TRUE(fixture.begin());
    fixture.persistence.saveStatus = PdfStatus::failure(PdfError::IoFailure);
    fixture.persistence.commitFailedSave = psitClearCommitted;

    EXPECT_EQ(fixture.session.clear(PdfSavedItemKind::Bookmark), PdfSavedItemsSessionResult::ReloadRequired);
    ASSERT_EQ(trace.count, 2);
    EXPECT_EQ(trace.operations[0], 'C');
    EXPECT_EQ(trace.operations[1], 'S');
    EXPECT_EQ(fixture.bookmarks.size, 0);
    EXPECT_EQ(fixture.bookmarks.clearCalls, 1);
    EXPECT_FALSE(fixture.session.supports(PdfSavedItemKind::Bookmark));
    EXPECT_EQ(fixture.persistence.durableCount, psitClearCommitted ? 1 : 3);

    fixture.persistence.saveStatus = PdfStatus::success();
    fixture.persistence.commitFailedSave = false;
    std::array<PdfSavedItem, PDF_SAVED_ITEMS_MAX_RECORDS> rebootItems{};
    PdfSavedItemsBuffer rebootBuffer{rebootItems.data(), static_cast<uint16_t>(rebootItems.size()), 0};
    PdfSavedItemsSession rebooted;
    ASSERT_TRUE(rebooted.begin(&rebootBuffer, fixture.persistence.callbacks(), fixture.bookmarks.callbacks(),
                               fixture.clippings.callbacks()));
    EXPECT_NE(rebooted.find(PdfSavedItemKind::Clipping, 9), nullptr);
    ASSERT_EQ(rebootBuffer.count, 1);
    EXPECT_EQ(fixture.bookmarks.size, 0);
    EXPECT_EQ(rebooted.dirty(), !psitClearCommitted);
    ASSERT_TRUE(rebooted.flush());
    ASSERT_EQ(fixture.persistence.durableCount, 1);
    EXPECT_EQ(fixture.persistence.durable[0].itemId, 9);
  }
}

TEST(PdfSavedItemsSessionTest, PendingJumpIsRetainedForRetryAndClearedOnlyAfterApplyOrExplicitCancel) {
  SessionFixture fixture;
  fixture.persistence.durable[0] = savedItem(PdfSavedItemKind::Bookmark, 7);
  fixture.persistence.durableCount = 1;
  fixture.bookmarks.ids[0] = 7;
  fixture.bookmarks.size = 1;
  ASSERT_TRUE(fixture.begin());
  ASSERT_EQ(fixture.session.queueJump(PdfSavedItemKind::Bookmark, 7), PdfSavedItemsSessionResult::Applied);

  PdfSavedItem pending{};
  ASSERT_TRUE(fixture.session.pendingJump(&pending));
  EXPECT_EQ(pending.itemId, 7);
  fixture.session.resolvePendingJump(false);
  EXPECT_TRUE(fixture.session.pendingJump(&pending));
  fixture.session.resolvePendingJump(true);
  EXPECT_FALSE(fixture.session.pendingJump(&pending));

  ASSERT_EQ(fixture.session.queueJump(PdfSavedItemKind::Bookmark, 7), PdfSavedItemsSessionResult::Applied);
  fixture.session.cancelPendingJump();
  EXPECT_FALSE(fixture.session.pendingJump(&pending));
}

TEST(PdfSavedItemsSessionTest, BookmarkContainmentFindsMidpageStableIdAfterRelayout) {
  SessionFixture fixture;
  fixture.persistence.durable[0] = savedItem(PdfSavedItemKind::Bookmark, 7, 105);
  fixture.persistence.durableCount = 1;
  fixture.bookmarks.ids[0] = 7;
  fixture.bookmarks.size = 1;
  ASSERT_TRUE(fixture.begin());

  const PdfSavedItem* const bookmark = fixture.session.findBookmarkInPage(1, 100, 110);
  ASSERT_NE(bookmark, nullptr);
  EXPECT_EQ(bookmark->itemId, 7);
  EXPECT_EQ(fixture.session.findBookmarkInPage(1, 106, 110), nullptr);
  EXPECT_EQ(fixture.session.findBookmarkInPage(2, 100, 110), nullptr);
  EXPECT_EQ(fixture.session.findBookmarkInPage(1, 111, 110), nullptr);
}

TEST(PdfSavedItemsSessionTest, CoordinatorAllocatesNothingAcrossLoadMutationAndJump) {
  SessionFixture fixture;
  PdfStatus beginStatus;
  PdfSavedItemsSessionResult addResult = PdfSavedItemsSessionResult::InvalidArgument;
  PdfStatus flushStatus;
  PdfSavedItemsSessionResult jumpResult = PdfSavedItemsSessionResult::InvalidArgument;
  PdfSavedItem pending{};
  bool pendingFound = false;
  watchedAllocationCount = 0;
  allocationWatchActive = true;
  try {
    beginStatus = fixture.begin();
    addResult = fixture.session.add(savedItem(PdfSavedItemKind::Bookmark, 0), nullptr);
    flushStatus = fixture.session.flush();
    jumpResult = fixture.session.queueJump(PdfSavedItemKind::Bookmark, fixture.buffer.items[0].itemId);
    pendingFound = fixture.session.pendingJump(&pending);
  } catch (const std::bad_alloc&) {
    allocationWatchActive = false;
    FAIL() << "coordinator allocated";
  }
  allocationWatchActive = false;

  EXPECT_TRUE(beginStatus);
  EXPECT_EQ(addResult, PdfSavedItemsSessionResult::Applied);
  EXPECT_TRUE(flushStatus);
  EXPECT_EQ(jumpResult, PdfSavedItemsSessionResult::Applied);
  EXPECT_TRUE(pendingFound);
  EXPECT_EQ(watchedAllocationCount, 0U);
}

}  // namespace
