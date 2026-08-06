#pragma once

#include <cstddef>
#include <memory>
#include <new>
#include <utility>

namespace PdfReaderAllocationProbe {
inline size_t calls = 0;
inline bool fail = false;

inline void reset() {
  calls = 0;
  fail = false;
}
}  // namespace PdfReaderAllocationProbe

template <typename T, typename... Args>
std::unique_ptr<T> makeUniqueNoThrow(Args&&... args) {
  ++PdfReaderAllocationProbe::calls;
  if (PdfReaderAllocationProbe::fail) {
    return {};
  }
  return std::unique_ptr<T>(new (std::nothrow) T(std::forward<Args>(args)...));
}
