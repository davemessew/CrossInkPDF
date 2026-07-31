#pragma once

#include <cstddef>
#include <memory>
#include <new>
#include <utility>

namespace TestMemory {
inline bool failNextAllocation = false;
inline size_t successfulAllocations = 0;
}

template <typename T, typename... Args>
std::unique_ptr<T> makeUniqueNoThrow(Args&&... args) {
  if (TestMemory::failNextAllocation) {
    TestMemory::failNextAllocation = false;
    return nullptr;
  }
  ++TestMemory::successfulAllocations;
  return std::unique_ptr<T>(new (std::nothrow) T(std::forward<Args>(args)...));
}
