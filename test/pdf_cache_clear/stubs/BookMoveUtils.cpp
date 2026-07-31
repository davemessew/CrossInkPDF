#include "BookMoveUtils.h"

namespace BookMoveUtils {

bool migrationCacheHash(const std::string&, const uint64_t normalCacheHash, uint64_t* const resolvedCacheHash,
                        bool* const readOnlyFallback) {
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
