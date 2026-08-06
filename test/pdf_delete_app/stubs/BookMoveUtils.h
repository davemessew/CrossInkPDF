#pragma once

#include <string>
#include <string_view>

#include "BookMutationFence.h"

namespace BookMoveUtils {

inline BookMutationFence testFence = BookMutationFence::Clear;
inline size_t fenceQueries = 0;

inline BookMutationFence mutationFenceForPath(const std::string&) {
  ++fenceQueries;
  return testFence;
}

inline BookMutationFence mutationFenceForPathNoPathAlloc(std::string_view) {
  ++fenceQueries;
  return testFence;
}

}  // namespace BookMoveUtils
