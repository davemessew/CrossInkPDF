#pragma once

namespace BookMoveDurableFile {

struct Payload {
  void* context = nullptr;
  // fileContext points to the platform FsFile used by the implementation.
  // Keeping it opaque lets narrow host tests supply their own FsFile facade.
  bool (*write)(void*, void* fileContext) = nullptr;
  bool (*verify)(void*, const char*) = nullptr;

  bool valid() const { return context != nullptr && write != nullptr && verify != nullptr; }
};

// Restores a readable canonical snapshot after a reset between canonical ->
// backup and temporary -> canonical. The backup is preferred so callers can
// load the pre-mutation state before replaying their owning journal.
bool restoreCanonicalForRead(const char* canonical, const char* temporary, const char* backup);

// Replaces canonical through a verified temporary file. At most one file
// handle is open at a time, and an existing canonical is retained as backup
// until the promoted bytes have been read back successfully.
bool replace(const char* canonical, const char* temporary, const char* backup, const Payload& payload);

}  // namespace BookMoveDurableFile
