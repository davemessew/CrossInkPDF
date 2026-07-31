#pragma once

#include <cstdint>
#include <string>

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

// Starts or resumes the crash-safe, PDF-only deletion flow. Complete means the
// hidden source and all path-owned reading state are durably gone.
Result deletePdfBook(const std::string& sourcePath);

// Called during boot after recents have loaded and before move/open routing.
Result recoverPendingPdfDelete();

// Read-only arbitration used by PDF moves. A valid pending journal is
// classified by path; an unreadable journal fails closed.
BookMutationFence mutationFenceForPath(const std::string& sourcePath);

#if defined(PDF_DELETE_TESTING)
void resetPresenceForTest();
#endif

}  // namespace PdfDeleteUtils
