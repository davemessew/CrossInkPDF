#pragma once

#include <cstddef>
#include <cstdint>

#include "PdfTypes.h"

constexpr size_t PDF_CACHE_PATH_CAPACITY = 128;
constexpr size_t PDF_CACHE_ENTRY_NAME_CAPACITY = 96;

enum class PdfCacheOpenMode : uint8_t {
  Read,
  WriteTruncate,
};

struct PdfCacheHandle {
  uint8_t value = 0xff;

  constexpr bool valid() const { return value != 0xff; }
};

struct PdfOptionalU64 {
  bool known = false;
  uint64_t value = 0;
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
};

struct PdfCacheDirEntry {
  char name[PDF_CACHE_ENTRY_NAME_CAPACITY]{};
  uint8_t nameLength = 0;
  bool directory = false;
  bool symlinkLike = false;
};

using PdfCacheListVisitor = PdfStatus (*)(void* context, const PdfCacheDirEntry& entry);

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
