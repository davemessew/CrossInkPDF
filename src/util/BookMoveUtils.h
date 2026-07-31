#pragma once

#include <cstdint>
#include <string>

#include "BookMutationFence.h"

namespace BookMoveUtils {

enum class MoveResult : uint8_t {
  Complete,
  NoPendingMove,
  Abandoned,
  Unsupported,
  Invalid,
  Conflict,
  Pending,
};

std::string buildReadFolderDestination(const std::string& srcPath);

// For PDF paths, durably prepares the migration journal before renaming the
// source and then performs a bounded recovery pass. Other formats retain their
// pre-PDF move paths and are rejected here.
MoveResult moveBook(const std::string& oldPath, const std::string& newPath, bool keepInRecents = true);

// Called once during boot after app state and recents have loaded. Only PDF
// records are accepted.
MoveResult recoverPendingBookMove();

// Read-only arbitration used by PDF deletion. A valid pending journal is
// classified by path; an unreadable or concurrently-starting journal fails
// closed.
BookMutationFence mutationFenceForPath(const std::string& bookPath);

// Resolves the cache key for a path while a move is in flight. Before durable
// activation this returns the old key; from Activated onward it returns new.
// A true readOnlyFallback forbids cache preparation, deletion, or other writes.
bool migrationCacheHash(const std::string& bookPath, uint64_t normalHash, uint64_t* resolvedHash,
                        bool* readOnlyFallback = nullptr);

// Legacy EPUB state migration. The caller performs the source rename first.
bool migrateMovedEpubState(const std::string& oldPath, const std::string& newPath, const std::string& oldCachePath,
                           const std::string& title, const std::string& author, bool keepInRecents);

}  // namespace BookMoveUtils
