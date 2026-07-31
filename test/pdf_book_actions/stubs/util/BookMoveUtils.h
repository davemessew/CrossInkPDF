#pragma once

#include <cstdint>
#include <string>

#include "TestState.h"
#ifdef PDF_BOOK_ACTIONS_PRODUCTION_STATS
#include <HalStorage.h>

#include "activities/reader/BookReadingStats.h"
#endif

namespace BookMoveUtils {

enum class MoveResult : uint8_t {
  Complete,
  NoPendingMove,
  Abandoned,
  Unsupported,
  Invalid,
  Conflict,
  Pending,
};

inline std::string buildReadFolderDestination(const std::string& source) {
  const size_t slash = source.find_last_of('/');
  return std::string("/Read/") + source.substr(slash == std::string::npos ? 0 : slash + 1);
}

inline MoveResult moveBook(const std::string& oldPath, const std::string& newPath, const bool keepInRecents = true) {
  ++TEST_STATE.pdfMoveCalls;
  TEST_STATE.pdfMoveOldPath = oldPath;
  TEST_STATE.pdfMoveNewPath = newPath;
  TEST_STATE.pdfMoveKeepInRecents = keepInRecents;
#ifdef PDF_BOOK_ACTIONS_PRODUCTION_STATS
  TEST_STATE.statsDurableAtMove =
      !TEST_STATE.expectedStatsCachePath.empty() &&
      BookReadingStats::load(TEST_STATE.expectedStatsCachePath).isCompleted;
#endif
  return static_cast<MoveResult>(TEST_STATE.pdfMoveResult);
}

inline bool migrationCacheHash(const std::string&, const uint64_t normalHash, uint64_t* const resolvedHash,
                               bool* const readOnlyFallback = nullptr) {
  ++TEST_STATE.resolverCalls;
  if (resolvedHash != nullptr) {
    *resolvedHash = TEST_STATE.resolvedHash == 0 ? normalHash : TEST_STATE.resolvedHash;
  }
  if (readOnlyFallback != nullptr) *readOnlyFallback = TEST_STATE.readOnlyFallback;
  return TEST_STATE.resolverSucceeds;
}

inline bool migrateMovedEpubState(const std::string&, const std::string&, const std::string&, const std::string&,
                                  const std::string&, const bool keepInRecents) {
  ++TEST_STATE.epubStateMigrations;
  TEST_STATE.epubMigrationKeepInRecents = keepInRecents;
  return true;
}

}  // namespace BookMoveUtils
