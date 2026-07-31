#include <cstdio>
#include <filesystem>
#include <iostream>
#include <string>

#include "BookActions.h"
#include "CrossPointSettings.h"
#include <HalStorage.h>
#include "PdfSourceIdentity.h"
#include "TestState.h"
#include "activities/reader/BookReadingStats.h"
#include "util/BookMoveUtils.h"

namespace {

constexpr char kPdfPath[] = "/Books/durable.pdf";
constexpr char kPdfDisplayName[] = "durable.pdf";

int failures = 0;

void expect(const bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

std::string pdfCachePath() {
  char path[PDF_CACHE_PATH_CAPACITY]{};
  const uint64_t hash = pdfPathHash64(kPdfPath, sizeof(kPdfPath) - 1U);
  const int length =
      std::snprintf(path, sizeof(path), "/.crosspoint/pdf_%016llx", static_cast<unsigned long long>(hash));
  return length > 0 ? std::string(path, static_cast<size_t>(length)) : std::string{};
}

std::string statsPath(const std::string& cachePath) { return cachePath + "/stats_v5.bin"; }

void resetHarness(const char* const caseName) {
  resetBookActionTestState();
  Storage.reset(std::filesystem::temp_directory_path() / "crossink_pdf_book_actions" / caseName);
}

void expectNoDownstreamMutation(const bool completed, const std::string& context) {
  expect(!completed, context + " must leave caller completion output unchanged");
  expect(TEST_STATE.globalLoads == 0 && TEST_STATE.globalSaves == 0,
         context + " must not mutate global stats");
  expect(TEST_STATE.recentAdds.empty() && TEST_STATE.recentRemovals.empty(),
         context + " must not mutate recents");
  expect(TEST_STATE.pdfMoveCalls == 0, context + " must not start a move");
  expect(TEST_STATE.productLoads == 0 && TEST_STATE.sourceIdentityPasses == 0,
         context + " must not load cosmetic product state");
}

void testMissingRootIsCreatedAndCompletionIsDurableBeforeMove() {
  resetHarness("missing_root");
  const std::string cachePath = pdfCachePath();
  TEST_SETTINGS.moveFinishedToReadFolder = 1;
  TEST_STATE.expectedStatsCachePath = cachePath;
  TEST_STATE.pdfMoveResult = static_cast<uint8_t>(BookMoveUtils::MoveResult::Complete);

  expect(!Storage.exists(cachePath.c_str()), "precondition: PDF cache root must be missing");
  bool completed = false;
  expect(BookActions::toggleBookCompleted(kPdfPath, kPdfDisplayName, completed),
         "missing PDF cache root must be created before completion");
  expect(completed, "successful missing-root completion must update caller state");
  FsFile root = Storage.open(cachePath.c_str());
  expect(root && root.isDirectory(), "resolved PDF cache root must exist as a directory");
  root.close();
  expect(TEST_STATE.statsDurableAtMove,
         "production BookReadingStats completion must be readable before moveBook starts");
  expect(BookReadingStats::load(cachePath).isCompleted,
         "production BookReadingStats completion must survive a fresh read");
  expect(TEST_STATE.pdfMoveCalls == 1, "durable completion must then start the configured move");
}

void testMkdirFailureFailsClosedBeforeAnyMutation() {
  resetHarness("mkdir_failure");
  const std::string cachePath = pdfCachePath();
  Storage.failMkdirOf(cachePath);
  TEST_SETTINGS.removeReadBooksFromRecents = 1;
  TEST_SETTINGS.moveFinishedToReadFolder = 1;

  bool completed = false;
  expect(!BookActions::toggleBookCompleted(kPdfPath, kPdfDisplayName, completed),
         "PDF completion must fail when its resolved cache root cannot be created");
  expect(Storage.mkdirCallCount() == 1, "missing PDF root must make one bounded mkdir attempt");
  expect(Storage.writeAttemptCount() == 0, "mkdir failure must occur before per-book stats save");
  expectNoDownstreamMutation(completed, "mkdir failure");
}

void testRootVerificationFailureFailsClosedBeforeAnyMutation() {
  resetHarness("root_verification_failure");
  const std::string cachePath = pdfCachePath();
  Storage.failDirectoryOpenOf(cachePath);
  TEST_SETTINGS.removeReadBooksFromRecents = 1;
  TEST_SETTINGS.moveFinishedToReadFolder = 1;

  bool completed = false;
  expect(!BookActions::toggleBookCompleted(kPdfPath, kPdfDisplayName, completed),
         "PDF completion must fail when the created cache root cannot be verified");
  expect(Storage.mkdirCallCount() == 1, "root verification case must first create the missing root");
  expect(Storage.writeAttemptCount() == 0, "root verification failure must occur before stats save");
  expectNoDownstreamMutation(completed, "root verification failure");
}

void testStatsSaveFailureFailsClosedBeforeGlobalRecentsOrMove() {
  resetHarness("stats_save_failure");
  const std::string cachePath = pdfCachePath();
  expect(Storage.mkdir(cachePath.c_str()), "precondition: writable PDF cache root must exist");
  Storage.failWriteOf(statsPath(cachePath));
  TEST_SETTINGS.removeReadBooksFromRecents = 1;
  TEST_SETTINGS.moveFinishedToReadFolder = 1;

  bool completed = false;
  expect(!BookActions::toggleBookCompleted(kPdfPath, kPdfDisplayName, completed),
         "PDF completion must fail when production BookReadingStats cannot persist");
  expect(Storage.writeAttemptCount() == 1, "stats save failure must make one bounded write attempt");
  expect(!BookReadingStats::load(cachePath).isCompleted,
         "failed production stats save must not appear durable");
  expectNoDownstreamMutation(completed, "stats save failure");
}

void testStatsReadbackFailureFailsClosedBeforeGlobalRecentsOrMove() {
  resetHarness("stats_readback_failure");
  const std::string cachePath = pdfCachePath();
  expect(Storage.mkdir(cachePath.c_str()), "precondition: writable PDF cache root must exist");
  Storage.failExistingReadOf(statsPath(cachePath));
  TEST_SETTINGS.removeReadBooksFromRecents = 1;
  TEST_SETTINGS.moveFinishedToReadFolder = 1;

  bool completed = false;
  expect(!BookActions::toggleBookCompleted(kPdfPath, kPdfDisplayName, completed),
         "PDF completion must fail when production stats readback cannot verify persistence");
  expect(Storage.writeAttemptCount() == 1,
         "readback verification failure must follow exactly one production stats write");
  expectNoDownstreamMutation(completed, "stats readback failure");
}

}  // namespace

int main() {
  testMissingRootIsCreatedAndCompletionIsDurableBeforeMove();
  testMkdirFailureFailsClosedBeforeAnyMutation();
  testRootVerificationFailureFailsClosedBeforeAnyMutation();
  testStatsSaveFailureFailsClosedBeforeGlobalRecentsOrMove();
  testStatsReadbackFailureFailsClosedBeforeGlobalRecentsOrMove();
  if (failures != 0) return 1;
  std::cout << "PDF_BOOK_ACTIONS_DURABILITY_PASS\n";
  return 0;
}
