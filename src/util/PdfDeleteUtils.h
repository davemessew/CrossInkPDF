#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

#include "BookMutationFence.h"

namespace PdfDeleteUtils {

enum class Result : uint8_t {
  Complete,
  NoPendingDelete,
  Unsupported,
  Invalid,
  Conflict,
  Pending,
};

class DirectoryDeleteSession;

struct DirectoryDeleteSessionDeleter {
  void operator()(DirectoryDeleteSession* session) const;
};

using DirectoryDeleteSessionPtr =
    std::unique_ptr<DirectoryDeleteSession, DirectoryDeleteSessionDeleter>;

// Starts or resumes the crash-safe, PDF-only deletion flow. Complete means the
// hidden source and all path-owned reading state are durably gone.
Result deletePdfBook(const std::string& sourcePath);

// Directory replay prepares one checked workspace and reuses it for every PDF.
// The source view is copied only into the session's bounded fixed buffer.
DirectoryDeleteSessionPtr makeDirectoryDeleteSessionNoThrow();
Result deletePdfBookNoPathAlloc(DirectoryDeleteSession& session,
                                std::string_view sourcePath);

// Called during boot after recents have loaded and before move/open routing.
Result recoverPendingPdfDelete();

// Read-only arbitration used by PDF moves. A valid pending journal is
// classified by path; an unreadable journal fails closed.
BookMutationFence mutationFenceForPath(const std::string& sourcePath);

#if defined(PDF_DELETE_TESTING)
void resetPresenceForTest();
void markDeleteStartingForTest();
#endif

}  // namespace PdfDeleteUtils
