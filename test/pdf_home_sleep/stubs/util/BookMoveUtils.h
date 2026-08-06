#pragma once

#include <cstdint>
#include <string>

namespace BookMoveUtils {

struct MigrationCacheHashStub {
  bool succeeds = true;
  bool useNormalHash = true;
  uint64_t resolvedHash = 0;
  bool readOnlyFallback = false;
  uint32_t calls = 0;

  void reset() {
    succeeds = true;
    useNormalHash = true;
    resolvedHash = 0;
    readOnlyFallback = false;
    calls = 0;
  }
};

inline MigrationCacheHashStub TEST_MIGRATION_CACHE_HASH;

inline bool migrationCacheHash(const std::string&, const uint64_t normalCacheHash, uint64_t* const resolvedCacheHash,
                               bool* const readOnlyFallback = nullptr) {
  ++TEST_MIGRATION_CACHE_HASH.calls;
  if (resolvedCacheHash != nullptr) {
    *resolvedCacheHash =
        TEST_MIGRATION_CACHE_HASH.useNormalHash ? normalCacheHash : TEST_MIGRATION_CACHE_HASH.resolvedHash;
  }
  if (readOnlyFallback != nullptr) {
    *readOnlyFallback = TEST_MIGRATION_CACHE_HASH.readOnlyFallback;
  }
  return TEST_MIGRATION_CACHE_HASH.succeeds;
}

}  // namespace BookMoveUtils
