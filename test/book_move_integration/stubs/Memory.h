#pragma once

#include <memory>
#include <new>
#include <limits>
#include <utility>
#include <vector>

namespace TestMemory {
inline size_t allocations = 0;
inline bool failNextAllocation = false;
inline size_t failAllocationCall = 0;
inline size_t failAtOrAboveBytes = std::numeric_limits<size_t>::max();
inline std::vector<size_t> allocationSizes;
inline void reset() {
  allocations = 0;
  failNextAllocation = false;
  failAllocationCall = 0;
  failAtOrAboveBytes = std::numeric_limits<size_t>::max();
  allocationSizes.clear();
}
}  // namespace TestMemory

template <typename T, typename... Args>
std::unique_ptr<T> makeUniqueNoThrow(Args&&... args) {
  ++TestMemory::allocations;
  TestMemory::allocationSizes.push_back(sizeof(T));
  if (TestMemory::failNextAllocation ||
      (TestMemory::failAllocationCall != 0 &&
       TestMemory::allocations == TestMemory::failAllocationCall) ||
      sizeof(T) >= TestMemory::failAtOrAboveBytes) {
    TestMemory::failNextAllocation = false;
    return {};
  }
  return std::unique_ptr<T>(new (std::nothrow) T(std::forward<Args>(args)...));
}
