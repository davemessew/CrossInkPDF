#include "BookMoveUtils.h"

namespace BookMoveUtils {

bool migrationCacheHash(const std::string&, const uint64_t normalHash, uint64_t* const resolvedHash,
                        bool* const readOnlyFallback) {
  if (resolvedHash != nullptr) *resolvedHash = normalHash;
  if (readOnlyFallback != nullptr) *readOnlyFallback = false;
  return true;
}

}  // namespace BookMoveUtils
