#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace PdfDirectoryDeleteScan {

constexpr size_t kPathCapacity = 1024;
constexpr size_t kEntryNameCapacity = 256;
constexpr char kSpoolTempPath[] =
    "/.crosspoint/pdf-directory-delete.spool.tmp";
constexpr char kSpoolSealedPath[] =
    "/.crosspoint/pdf-directory-delete.spool";

enum class Status : uint8_t {
  Complete,
  CommittedWithCleanupWarning,
  InvalidRoot,
  AllocationFailure,
  OpenFailure,
  IterationFailure,
  CloseFailure,
  PathLimit,
  ReservedTombstone,
  SpoolCleanupFailure,
  SpoolOpenFailure,
  SpoolWriteFailure,
  SpoolSyncFailure,
  SpoolReadFailure,
  SpoolCorrupt,
  PdfRecoveryFailure,
  PdfDeleteFailure,
  DirectoryDeleteFailure,
  MetadataCleanupFailure,
};

struct DeleteCallbacks {
  void* context = nullptr;
  bool (*deletePdf)(void* context, const char* path) = nullptr;
  bool (*clearLegacyMetadata)(void* context, std::string_view path) = nullptr;
  bool (*preparePdfDelete)(void* context) = nullptr;
};

// Read-only, spool-free classification used to keep PDF-free directories on
// the original FileBrowser deletion path.
Status containsPdfNoThrow(const std::string& rootPath, bool* containsPdf);
Status deleteLegacyDirectoryNoThrow(const std::string& rootPath,
                                    const DeleteCallbacks& callbacks);
Status deletePdfDirectoryNoThrow(const std::string& rootPath,
                                 const DeleteCallbacks& callbacks);
Status deleteDirectoryNoThrow(const std::string& rootPath,
                              const DeleteCallbacks& callbacks);

}  // namespace PdfDirectoryDeleteScan
