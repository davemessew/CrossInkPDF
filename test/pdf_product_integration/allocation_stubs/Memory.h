#pragma once

#include <cstddef>
#include <memory>
#include <new>
#include <type_traits>
#include <utility>

namespace TestMemory {
inline bool observeArrays = false;
inline size_t failArrayBytes = 0;
inline size_t arraySizes[16]{};
inline size_t arrayCount = 0;

inline void resetArrayProbe() {
  observeArrays = false;
  failArrayBytes = 0;
  arrayCount = 0;
  for (size_t& size : arraySizes) {
    size = 0;
  }
}

inline bool sawArrayBytes(const size_t bytes) {
  for (size_t index = 0; index < arrayCount; ++index) {
    if (arraySizes[index] == bytes) {
      return true;
    }
  }
  return false;
}
}  // namespace TestMemory

template <typename T, typename... Args>
  requires(!std::is_array_v<T>)
std::unique_ptr<T> makeUniqueNoThrow(Args&&... args) {
  return std::unique_ptr<T>(new (std::nothrow) T(std::forward<Args>(args)...));
}

template <typename T>
  requires std::is_unbounded_array_v<T>
std::unique_ptr<T> makeUniqueNoThrow(const size_t count) {
  using Element = std::remove_extent_t<T>;
  const size_t bytes = count * sizeof(Element);
  if (TestMemory::observeArrays &&
      TestMemory::arrayCount < sizeof(TestMemory::arraySizes) / sizeof(TestMemory::arraySizes[0])) {
    TestMemory::arraySizes[TestMemory::arrayCount++] = bytes;
  }
  if (TestMemory::observeArrays && TestMemory::failArrayBytes == bytes) {
    return {};
  }
  return std::unique_ptr<T>(new (std::nothrow) Element[count]());
}

template <typename F>
struct [[nodiscard]] ScopedCleanup final {
  const F fn;
  explicit ScopedCleanup(F f) : fn{std::move(f)} {}
  ScopedCleanup(const ScopedCleanup&) = delete;
  ScopedCleanup& operator=(const ScopedCleanup&) = delete;
  ScopedCleanup(ScopedCleanup&&) = delete;
  ScopedCleanup& operator=(ScopedCleanup&&) = delete;
  ~ScopedCleanup() { fn(); }
};

template <typename F>
ScopedCleanup(F) -> ScopedCleanup<F>;
