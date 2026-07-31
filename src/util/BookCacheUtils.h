#pragma once

#include <string>

// Clears the reading cache for a book file if its extension is recognised
// (EPUB, PDF, XTC, or TXT). Does nothing for other file types.
void clearBookCache(const std::string& path);

// Clears derived reading cache files while preserving user-owned state such as
// progress and per-book stats. PDF clearing is an exact derived-file allowlist,
// so unknown root state is preserved. Returns false if clearing fails.
bool clearBookCachePreservingUserState(const std::string& path);

// Clears a known book cache directory while preserving per-book stats. PDF
// directories use the same selective user-state-preserving policy as path-based
// clearing.
bool clearBookCacheDirectoryPreservingStats(const std::string& cachePath);

// Returns true if the directory name matches a book cache entry.
bool isBookCacheDirectoryName(const char* name);
