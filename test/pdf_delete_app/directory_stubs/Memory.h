#pragma once

#include <limits>
#include <memory>
#include <new>
#include <type_traits>
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
  requires(!std::is_array_v<T>)
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
  return std::unique_ptr<T>(
      new (std::nothrow) T(std::forward<Args>(args)...));
}

template <typename T>
  requires std::is_unbounded_array_v<T>
std::unique_ptr<T> makeUniqueNoThrow(const size_t count) {
  using Element = std::remove_extent_t<T>;
  ++TestMemory::allocations;
  const size_t bytes = count * sizeof(Element);
  TestMemory::allocationSizes.push_back(bytes);
  if (TestMemory::failNextAllocation ||
      (TestMemory::failAllocationCall != 0 &&
       TestMemory::allocations == TestMemory::failAllocationCall) ||
      bytes >= TestMemory::failAtOrAboveBytes) {
    TestMemory::failNextAllocation = false;
    return {};
  }
  return std::unique_ptr<T>(new (std::nothrow) Element[count]());
}
