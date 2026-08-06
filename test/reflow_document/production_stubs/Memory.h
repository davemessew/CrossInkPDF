#pragma once

#include <memory>
#include <new>
#include <type_traits>
#include <utility>

#include "EpubProductionTestState.h"

template <typename T, typename... Args, std::enable_if_t<!std::is_array_v<T>, int> = 0>
std::unique_ptr<T> makeUniqueNoThrow(Args&&... args) {
  return std::unique_ptr<T>(new (std::nothrow) T(std::forward<Args>(args)...));
}

template <typename T, std::enable_if_t<std::is_unbounded_array_v<T>, int> = 0>
std::unique_ptr<T> makeUniqueNoThrow(const size_t count) {
  using Element = std::remove_extent_t<T>;
  epub_production_test::arrayAllocationBytes.push_back(sizeof(Element) * count);
  return std::unique_ptr<T>(new (std::nothrow) Element[count]());
}
