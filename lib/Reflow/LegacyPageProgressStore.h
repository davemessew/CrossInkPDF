#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "ReflowDocument.h"

struct LegacyPageProgressIo {
  using ExistsFn = bool (*)(void* context, const char* path);
  using ReadFn = bool (*)(void* context, const char* path, uint8_t* data, size_t capacity, size_t* fileSize);
  using WriteSyncedFn = bool (*)(void* context, const char* path, const uint8_t* data, size_t size);
  using RemoveFn = bool (*)(void* context, const char* path);
  using RenameFn = bool (*)(void* context, const char* oldPath, const char* newPath);

  void* context = nullptr;
  ExistsFn exists = nullptr;
  ReadFn read = nullptr;
  WriteSyncedFn writeSynced = nullptr;
  RemoveFn remove = nullptr;
  RenameFn rename = nullptr;

  bool isValid() const {
    return exists != nullptr && read != nullptr && writeSynced != nullptr && remove != nullptr && rename != nullptr;
  }
};

class LegacyPageProgressStore {
 public:
  LegacyPageProgressStore(const std::string_view cachePath, const LegacyPageProgressIo io)
      : cachePath_(cachePath), io_(io) {}

  bool load(ReflowReadingPosition& position) const;
  bool save(const ReflowReadingPosition& position) const;

 private:
  bool readPosition(const std::string& path, ReflowReadingPosition& position) const;

  std::string_view cachePath_;
  LegacyPageProgressIo io_;
};
