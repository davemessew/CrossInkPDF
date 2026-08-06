#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace epub_production_test {

inline constexpr size_t kUseDataSize = std::numeric_limits<size_t>::max();

struct FileFixture {
  std::vector<uint8_t> data;
  size_t reportedSize = kUseDataSize;
  size_t readableSize = kUseDataSize;
  bool closeSucceeds = true;
};

struct StorageBoundarySnapshot {
  std::string operation;
  std::unordered_map<std::string, std::vector<uint8_t>> files;
};

struct StorageReadObservation {
  std::string path;
  size_t offset = 0;
  size_t requested = 0;
  size_t returned = 0;
};

struct StorageState {
  std::unordered_map<std::string, FileFixture> files;
  std::vector<size_t> requestedReadCapacities;
  std::vector<StorageReadObservation> readObservations;
  std::vector<std::string> openedReadPaths;
  std::vector<std::string> openedWritePaths;
  int closeCount = 0;
  bool failWrites = false;
  bool failRename = false;
  std::string failRenameOldPath;
  std::string failRemovePath;
  std::string failSyncPath;
  size_t failSyncOccurrence = 0;
  std::unordered_map<std::string, size_t> syncCounts;
  std::string failPatchWritePath;
  size_t failPatchWriteOffset = 0;
  bool shortPatchWrite = false;
  size_t removeFaultsReached = 0;
  size_t renameFaultsReached = 0;
  size_t syncFaultsReached = 0;
  size_t shortPatchFaultsReached = 0;
  size_t headerPatchFaultsReached = 0;
  size_t footerPatchFaultsReached = 0;
  std::string corruptRenameDestinationPath;
  size_t corruptRenameOffset = 0;
  uint8_t corruptRenameXor = 0;
  size_t renameCorruptionsReached = 0;
  bool failRemoveDir = false;
  size_t boundedPathCapacity = 0;
  bool boundedPathsWereNulTerminated = true;
  size_t pathOperations = 0;
  std::vector<std::string> capturedPathOperations;
  std::vector<StorageBoundarySnapshot> boundarySnapshots;
  std::string installPriorFilesOnSyncPath;
  std::unordered_map<std::string, std::vector<uint8_t>> priorFiles;

  void reset() { *this = {}; }

  void recordPath(const char* operation, const char* path) {
    if (boundedPathCapacity == 0) return;
    ++pathOperations;
    if (boundedPathCapacity != 0 &&
        (path == nullptr ||
         std::memchr(path, '\0', boundedPathCapacity) == nullptr)) {
      boundedPathsWereNulTerminated = false;
      return;
    }
    capturedPathOperations.emplace_back(std::string(operation) + path);
  }

  void addFile(std::string path, std::vector<uint8_t> data, const size_t reportedSize = kUseDataSize,
               const size_t readableSize = kUseDataSize, const bool closeSucceeds = true) {
    files[std::move(path)] = {
        .data = std::move(data),
        .reportedSize = reportedSize,
        .readableSize = readableSize,
        .closeSucceeds = closeSucceeds,
    };
  }

  void recordBoundary(std::string operation, const std::string& path = {}) {
    if (!installPriorFilesOnSyncPath.empty() && path == installPriorFilesOnSyncPath) {
      for (const auto& [priorPath, data] : priorFiles) addFile(priorPath, data);
      installPriorFilesOnSyncPath.clear();
    }
    StorageBoundarySnapshot snapshot;
    snapshot.operation = std::move(operation);
    for (const auto& [filePath, fixture] : files) snapshot.files[filePath] = fixture.data;
    boundarySnapshots.push_back(std::move(snapshot));
  }
};

inline StorageState storage;
inline std::vector<size_t> arrayAllocationBytes;
inline std::vector<std::string> errorLogs;

enum class ParserMode : uint8_t {
  Reject,
  CaptureOnly,
  BorrowedPixelCachePage,
};

struct ParserState {
  ParserMode mode = ParserMode::Reject;
  std::string relativeImageHref = "../images/0123456789abcdef-89abcdef.pxc";
  std::string parsePath;
  std::string contentBase;
  std::string resolvedImageHref;
  bool semanticPaginationHooksPresent = false;
  bool preserveImagePathRoot = false;
  std::string borrowedImagePath;
  uint16_t borrowedImageWidth = 0;
  uint16_t borrowedImageHeight = 0;
  bool borrowedPixelCache = false;
  bool pageImageFound = false;
  std::string pageImagePath;
  int16_t pageImageWidth = 0;
  int16_t pageImageHeight = 0;
  uint16_t paragraphIndex = 0;
  uint16_t listItemIndex = 0;
  int serializedPages = 0;
  uint16_t pageCount = 1;
  uint8_t serializedPageByte = 0x5a;

  void reset() { *this = {}; }
};

inline ParserState parser;

inline void recordError(const char*, const char* format) { errorLogs.emplace_back(format); }

template <typename... Args>
  requires(sizeof...(Args) > 0)
void recordError(const char*, const char* format, Args... args) {
  char buffer[512] = {};
  std::snprintf(buffer, sizeof(buffer), format, args...);
  errorLogs.emplace_back(buffer);
}

struct SpineFixture {
  std::string href;
  uint32_t cumulativeSize = 0;
  int16_t tocIndex = -1;
};

struct TocFixture {
  std::string title;
  std::string href;
  std::string anchor;
  uint8_t level = 0;
  int16_t spineIndex = -1;
};

struct MetadataState {
  std::vector<SpineFixture> spines = {
      {.href = "text/part.xhtml", .cumulativeSize = 100, .tocIndex = -1},
      {.href = "text/chapter.xhtml", .cumulativeSize = 250, .tocIndex = 0},
  };
  std::vector<TocFixture> toc = {
      {.title = "Expensive chapter title",
       .href = "text/chapter.xhtml",
       .anchor = {},
       .level = 0,
       .spineIndex = 1},
  };
  int spineEntryQueries = 0;
  int cumulativeSizeQueries = 0;
  int tocEntryQueries = 0;
  bool loadSucceeds = true;

  void reset() { *this = {}; }
  void resetQueryCounts() {
    spineEntryQueries = 0;
    cumulativeSizeQueries = 0;
    tocEntryQueries = 0;
  }
};

inline MetadataState metadata;

inline void resetAll() {
  storage.reset();
  metadata.reset();
  parser.reset();
  arrayAllocationBytes.clear();
  errorLogs.clear();
}

}  // namespace epub_production_test
