#pragma once

#include <cstddef>
#include <cstdint>

#include "PdfTypes.h"

constexpr size_t PDF_CACHE_PATH_CAPACITY = 128;
constexpr size_t PDF_CACHE_ENTRY_NAME_CAPACITY = 96;

enum class PdfCacheOpenMode : uint8_t {
  Read,
  WriteTruncate,
  Write,
  ReadWrite,
};

struct PdfCacheHandle {
  uint8_t value = 0xff;

  constexpr bool valid() const { return value != 0xff; }
};

struct PdfOptionalU64 {
  bool known = false;
  uint64_t value = 0;
};

enum class PdfCacheMetadataOperation : uint8_t {
  Read,
  Seek,
  Truncate,
};

struct PdfCacheCapacity {
  PdfOptionalU64 total{};
  PdfOptionalU64 free{};
};

struct PdfCacheFileMetadata {
  uint64_t size = 0;
  PdfOptionalU64 modificationTime{};
  bool directory = false;
  bool symlinkLike = false;
  // Read is the normal metadata query. Seek and Truncate reuse `size` as the
  // requested position/length so cold resume operations do not grow every
  // PdfCacheIo instance by more function pointers.
  PdfCacheMetadataOperation operation = PdfCacheMetadataOperation::Read;
};

struct PdfCacheDirEntry {
  char name[PDF_CACHE_ENTRY_NAME_CAPACITY]{};
  uint8_t nameLength = 0;
  bool directory = false;
  bool symlinkLike = false;
};

using PdfCacheListVisitor = PdfStatus (*)(void* context, const PdfCacheDirEntry& entry);
using PdfCacheRenameFn = PdfStatus (*)(void* context, const char* sourcePath, const char* destinationPath);

struct PdfCacheIo {
  using OpenFn = PdfStatus (*)(void* context, const char* path, PdfCacheOpenMode mode, PdfCacheHandle* handle);
  using ReadFn = PdfStatus (*)(void* context, PdfCacheHandle handle, uint64_t offset, uint8_t* destination,
                               size_t requested, size_t* bytesRead);
  using WriteFn = PdfStatus (*)(void* context, PdfCacheHandle handle, const uint8_t* source, size_t requested,
                                size_t* bytesWritten);
  using HandleFn = PdfStatus (*)(void* context, PdfCacheHandle handle);
  using CloseFn = PdfStatus (*)(void* context, PdfCacheHandle* handle);
  using RemoveFn = PdfStatus (*)(void* context, const char* path, bool recursive);
  using MkdirFn = PdfStatus (*)(void* context, const char* path);
  using ListFn = PdfStatus (*)(void* context, const char* path, PdfCacheListVisitor visitor, void* visitorContext);
  using CapacityFn = PdfStatus (*)(void* context, PdfCacheCapacity* capacity);
  using MetadataFn = PdfStatus (*)(void* context, PdfCacheHandle handle, PdfCacheFileMetadata* metadata);
  void* context = nullptr;
  OpenFn open = nullptr;
  ReadFn read = nullptr;
  WriteFn write = nullptr;
  HandleFn flush = nullptr;
  HandleFn sync = nullptr;
  CloseFn close = nullptr;
  RemoveFn remove = nullptr;
  MkdirFn mkdir = nullptr;
  ListFn list = nullptr;
  CapacityFn capacity = nullptr;
  MetadataFn metadata = nullptr;

  constexpr bool valid() const {
    return open != nullptr && read != nullptr && write != nullptr && flush != nullptr && sync != nullptr &&
           close != nullptr && remove != nullptr && mkdir != nullptr && list != nullptr && capacity != nullptr &&
           metadata != nullptr;
  }
};

#if UINTPTR_MAX == UINT32_MAX
static_assert(sizeof(PdfCacheIo) == 48U, "PdfCacheIo must remain a 12-pointer callback table on RV32");
static_assert(offsetof(PdfCacheIo, metadata) == 44U, "PdfCacheIo metadata callback moved on RV32");
#endif

inline PdfStatus pdfCacheSeek(const PdfCacheIo& io, const PdfCacheHandle handle, const uint64_t offset) {
  if (io.metadata == nullptr || !handle.valid()) {
    return PdfStatus::failure(PdfError::InvalidArgument, offset);
  }
  PdfCacheFileMetadata request{};
  request.size = offset;
  request.operation = PdfCacheMetadataOperation::Seek;
  return io.metadata(io.context, handle, &request);
}

inline PdfStatus pdfCacheTruncate(const PdfCacheIo& io, const PdfCacheHandle handle, const uint64_t length) {
  if (io.metadata == nullptr || !handle.valid()) {
    return PdfStatus::failure(PdfError::InvalidArgument, length);
  }
  PdfCacheFileMetadata request{};
  request.size = length;
  request.operation = PdfCacheMetadataOperation::Truncate;
  return io.metadata(io.context, handle, &request);
}
