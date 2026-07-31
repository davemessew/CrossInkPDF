#pragma once

#include <cstdint>

// Move and delete each use a single crash-recovery journal and both rewrite
// the global recent-books store. The path distinction is retained for
// diagnostics, while callers serialize either pending kind before starting a
// second mutation so two durable snapshots cannot overwrite each other.
enum class BookMutationFence : uint8_t {
  Clear,
  MatchingPending,
  UnrelatedPending,
  Indeterminate,
};
