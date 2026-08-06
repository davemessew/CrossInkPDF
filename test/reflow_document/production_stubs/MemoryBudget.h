#pragma once

#include <cstdint>

namespace MemoryBudget {
struct Snapshot {
  uint32_t freeHeap = 256U * 1024U;
  uint32_t maxAllocHeap = 128U * 1024U;
};
inline Snapshot snapshot() { return {}; }
inline bool shouldReleaseSdFontCachesForEpubInlineImage(const Snapshot&) { return false; }
}  // namespace MemoryBudget
