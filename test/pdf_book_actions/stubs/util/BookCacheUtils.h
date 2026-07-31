#pragma once

#include <string>

#include "TestState.h"

inline bool clearBookCachePreservingUserState(const std::string& path) {
  TEST_STATE.cacheClears.push_back(path);
  return TEST_STATE.cacheClearResult;
}
