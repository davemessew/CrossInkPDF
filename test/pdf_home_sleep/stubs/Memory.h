#pragma once

#include <cstddef>
#include <limits>
#include <memory>
#include <new>
#include <type_traits>
#include <utility>

namespace MemoryTestHooks {

inline size_t failNextUint8ArrayBytes = std::numeric_limits<size_t>::max();
inline uint32_t failedUint8ArrayAllocations = 0;

inline void failNextUint8Array(const size_t bytes) {
  failNextUint8ArrayBytes = bytes;
  failedUint8ArrayAllocations = 0;
}

inline void reset() {
  failNextUint8ArrayBytes = std::numeric_limits<size_t>::max();
  failedUint8ArrayAllocations = 0;
}

}  // namespace MemoryTestHooks

template <typename T, typename... Args>
  requires(!std::is_array_v<T>)
std::unique_ptr<T> makeUniqueNoThrow(Args&&... args) {
  return std::unique_ptr<T>(new (std::nothrow) T(std::forward<Args>(args)...));
}

template <typename T>
  requires std::is_unbounded_array_v<T>
std::unique_ptr<T> makeUniqueNoThrow(const size_t count) {
  using Element = std::remove_extent_t<T>;
  if constexpr (std::is_same_v<Element, uint8_t>) {
    if (MemoryTestHooks::failNextUint8ArrayBytes == count) {
      MemoryTestHooks::failNextUint8ArrayBytes = std::numeric_limits<size_t>::max();
      ++MemoryTestHooks::failedUint8ArrayAllocations;
      return nullptr;
    }
  }
  return std::unique_ptr<T>(new (std::nothrow) Element[count]());
}

template <typename F>
struct [[nodiscard]] ScopedCleanup final {
  const F fn;
  explicit ScopedCleanup(F function) : fn{std::move(function)} {}
  ScopedCleanup(const ScopedCleanup&) = delete;
  ScopedCleanup& operator=(const ScopedCleanup&) = delete;
  ScopedCleanup(ScopedCleanup&&) = delete;
  ScopedCleanup& operator=(ScopedCleanup&&) = delete;
  ~ScopedCleanup() { fn(); }
};

template <typename F>
ScopedCleanup(F) -> ScopedCleanup<F>;
