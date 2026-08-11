#pragma once

#include <cstddef>
#include <limits>
#include <memory>
#include <new>
#include <type_traits>
#include <utility>

namespace TestMemory {
inline bool observeArrays = false;
inline size_t failArrayBytes = 0;
inline size_t arraySizes[16]{};
inline size_t arrayCount = 0;
inline size_t firmwareBudgetBytes = std::numeric_limits<size_t>::max();
inline size_t firmwareReservedBytes = 0;
inline size_t firmwareAllocatedBytes = 0;
inline size_t firmwarePeakBytes = 0;
inline bool firmwareBudgetExceeded = false;

inline void beginFirmwareBudget(const size_t limitBytes, const size_t reservedBytes) {
  firmwareBudgetBytes = limitBytes;
  firmwareReservedBytes = reservedBytes;
  firmwareAllocatedBytes = 0;
  firmwarePeakBytes = reservedBytes;
  firmwareBudgetExceeded = reservedBytes > limitBytes;
}

inline void disableFirmwareBudget() {
  firmwareBudgetBytes = std::numeric_limits<size_t>::max();
  firmwareReservedBytes = 0;
  firmwareAllocatedBytes = 0;
  firmwarePeakBytes = 0;
  firmwareBudgetExceeded = false;
}

inline bool reserveFirmwareBytes(const size_t bytes) {
  if (firmwareBudgetExceeded || bytes > firmwareBudgetBytes ||
      firmwareReservedBytes > firmwareBudgetBytes - bytes ||
      firmwareAllocatedBytes > firmwareBudgetBytes - firmwareReservedBytes - bytes) {
    firmwareBudgetExceeded = true;
    return false;
  }
  firmwareAllocatedBytes += bytes;
  const size_t current = firmwareReservedBytes + firmwareAllocatedBytes;
  if (current > firmwarePeakBytes) {
    firmwarePeakBytes = current;
  }
  return true;
}

inline void undoFirmwareReservation(const size_t bytes) {
  if (bytes <= firmwareAllocatedBytes) {
    firmwareAllocatedBytes -= bytes;
  }
}

inline size_t firmwareCurrentBytes() { return firmwareReservedBytes + firmwareAllocatedBytes; }

inline size_t firmwareRemainingBytes() {
  const size_t current = firmwareCurrentBytes();
  return current >= firmwareBudgetBytes ? 0 : firmwareBudgetBytes - current;
}

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
  if (!TestMemory::reserveFirmwareBytes(sizeof(T))) {
    return {};
  }
  auto* const allocation = new (std::nothrow) T(std::forward<Args>(args)...);
  if (allocation == nullptr) {
    TestMemory::undoFirmwareReservation(sizeof(T));
  }
  return std::unique_ptr<T>(allocation);
}

template <typename T>
  requires std::is_unbounded_array_v<T>
std::unique_ptr<T> makeUniqueNoThrow(const size_t count) {
  using Element = std::remove_extent_t<T>;
  if (count > std::numeric_limits<size_t>::max() / sizeof(Element)) {
    TestMemory::firmwareBudgetExceeded = true;
    return {};
  }
  const size_t bytes = count * sizeof(Element);
  if (TestMemory::observeArrays &&
      TestMemory::arrayCount < sizeof(TestMemory::arraySizes) / sizeof(TestMemory::arraySizes[0])) {
    TestMemory::arraySizes[TestMemory::arrayCount++] = bytes;
  }
  if (TestMemory::observeArrays && TestMemory::failArrayBytes == bytes) {
    return {};
  }
  if (!TestMemory::reserveFirmwareBytes(bytes)) {
    return {};
  }
  auto* const allocation = new (std::nothrow) Element[count]();
  if (allocation == nullptr) {
    TestMemory::undoFirmwareReservation(bytes);
  }
  return std::unique_ptr<T>(allocation);
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
