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

bool migrationCacheHash(const std::string&, uint64_t normalCacheHash, uint64_t* resolvedCacheHash,
                        bool* readOnlyFallback = nullptr);

}  // namespace BookMoveUtils
