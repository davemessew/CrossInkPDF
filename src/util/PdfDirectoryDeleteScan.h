#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace PdfDirectoryDeleteScan {

constexpr size_t kPathCapacity = 1024;
constexpr size_t kEntryNameCapacity = 256;
constexpr char kSpoolTempPath[] =
    "/.crosspoint/pdf-directory-delete.spool.tmp";
constexpr char kSpoolSealedPath[] =
    "/.crosspoint/pdf-directory-delete.spool";

enum class Status : uint8_t {
  Complete,
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
  PdfDeleteFailure,
  DirectoryDeleteFailure,
};

struct DeleteCallbacks {
  void* context = nullptr;
  bool (*deletePdf)(void* context, const char* path) = nullptr;
  void (*clearLegacyMetadata)(void* context, const std::string& path) = nullptr;
};

Status deleteDirectoryNoThrow(const std::string& rootPath,
                              const DeleteCallbacks& callbacks);

}  // namespace PdfDirectoryDeleteScan
