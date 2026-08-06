#pragma once

#include <memory>
#include <new>
#include <utility>

#include "TestState.h"

template <typename T, typename... Args>
std::unique_ptr<T> makeUniqueNoThrow(Args&&... args) {
  ++TEST_STATE.productStateAllocations;
  if (TEST_STATE.failProductStateAllocation) return {};
  return std::unique_ptr<T>(new (std::nothrow) T(std::forward<Args>(args)...));
}
