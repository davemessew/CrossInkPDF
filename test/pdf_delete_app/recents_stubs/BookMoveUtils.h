#pragma once

#include <cstdint>
#include <string>

namespace BookMoveUtils {
bool migrationCacheHash(const std::string&, uint64_t normalHash, uint64_t* resolvedHash,
                        bool* readOnlyFallback);
}  // namespace BookMoveUtils
