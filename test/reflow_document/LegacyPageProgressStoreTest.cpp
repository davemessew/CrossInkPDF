#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "Reflow/LegacyPageProgressStore.h"

namespace {

class MemoryProgressIo {
 public:
  LegacyPageProgressIo callbacks() {
    return {
        .context = this,
        .exists = exists,
        .read = read,
        .writeSynced = writeSynced,
        .remove = remove,
        .rename = rename,
    };
  }

  std::unordered_map<std::string, std::vector<uint8_t>> files;
  std::vector<std::string> operations;
  std::string failedRenameSource;
  std::string failedRenameDestination;

 private:
  static MemoryProgressIo& self(void* context) { return *static_cast<MemoryProgressIo*>(context); }

  static bool exists(void* context, const char* path) {
    const auto& io = self(context);
    return io.files.find(path) != io.files.end();
  }

  static bool read(void* context, const char* path, uint8_t* data, const size_t capacity, size_t* fileSize) {
    auto& io = self(context);
    const auto file = io.files.find(path);
    if (file == io.files.end() || fileSize == nullptr) {
      return false;
    }
    *fileSize = file->second.size();
    const size_t copied = std::min(capacity, file->second.size());
    std::copy_n(file->second.begin(), copied, data);
    io.operations.emplace_back(std::string("read:") + path);
    return true;
  }

  static bool writeSynced(void* context, const char* path, const uint8_t* data, const size_t size) {
    auto& io = self(context);
    io.files[path] = std::vector<uint8_t>(data, data + size);
    io.operations.emplace_back(std::string("write:") + path);
    return true;
  }

  static bool remove(void* context, const char* path) {
    auto& io = self(context);
    io.operations.emplace_back(std::string("remove:") + path);
    return io.files.erase(path) == 1;
  }

  static bool rename(void* context, const char* oldPath, const char* newPath) {
    auto& io = self(context);
    io.operations.emplace_back(std::string("rename:") + oldPath + "->" + newPath);
    if (io.failedRenameSource == oldPath && io.failedRenameDestination == newPath) {
      return false;
    }
    const auto file = io.files.find(oldPath);
    if (file == io.files.end()) {
      return false;
    }
    io.files[newPath] = std::move(file->second);
    io.files.erase(file);
    return true;
  }
};

constexpr char kCachePath[] = "/cache";
constexpr char kProgressPath[] = "/cache/progress.bin";
constexpr char kBackupPath[] = "/cache/progress.bin.bak";
constexpr char kTempPath[] = "/cache/progress.bin.tmp";

TEST(LegacyPageProgressStore, LoadsLegacyFourByteTupleAndPageSentinel) {
  MemoryProgressIo io;
  io.files[kProgressPath] = {0x34, 0x12, 0xFF, 0xFF};
  const LegacyPageProgressStore store(kCachePath, io.callbacks());

  ReflowReadingPosition position = {
      .sectionIndex = 99,
      .pageNumber = 99,
      .pageCount = 99,
      .hasPageCount = true,
      .hasSemanticPosition = true,
      .globalWordOrdinal = 99,
      .blockWordOffset = 99,
      .blockAnchor = "stale",
  };
  ASSERT_TRUE(store.load(position));
  EXPECT_EQ(position.sectionIndex, 0x1234);
  EXPECT_EQ(position.pageNumber, 0);
  EXPECT_EQ(position.pageCount, 0);
  EXPECT_FALSE(position.hasPageCount);
  EXPECT_FALSE(position.hasSemanticPosition);
  EXPECT_EQ(position.globalWordOrdinal, 0u);
  EXPECT_EQ(position.blockWordOffset, 0u);
  EXPECT_TRUE(position.blockAnchor.empty());
}

TEST(LegacyPageProgressStore, LoadsCurrentExactSixByteTuple) {
  MemoryProgressIo io;
  io.files[kProgressPath] = {0x34, 0x12, 0x78, 0x56, 0xBC, 0x9A};
  const LegacyPageProgressStore store(kCachePath, io.callbacks());

  ReflowReadingPosition position;
  ASSERT_TRUE(store.load(position));
  EXPECT_EQ(position.sectionIndex, 0x1234);
  EXPECT_EQ(position.pageNumber, 0x5678);
  EXPECT_EQ(position.pageCount, 0x9ABC);
  EXPECT_TRUE(position.hasPageCount);
}

TEST(LegacyPageProgressStore, RejectsInvalidLengthsAndRecoversBackup) {
  for (const size_t invalidSize : {0u, 1u, 3u, 5u, 7u}) {
    MemoryProgressIo io;
    io.files[kProgressPath] = std::vector<uint8_t>(invalidSize, 0xA5);
    io.files[kBackupPath] = {0x02, 0x00, 0x03, 0x00, 0x04, 0x00};
    const LegacyPageProgressStore store(kCachePath, io.callbacks());

    ReflowReadingPosition position;
    ASSERT_TRUE(store.load(position)) << "invalid primary size " << invalidSize;
    EXPECT_EQ(position.sectionIndex, 2);
    EXPECT_EQ(position.pageNumber, 3);
    EXPECT_EQ(position.pageCount, 4);
    EXPECT_TRUE(position.hasPageCount);
  }

  MemoryProgressIo invalidOnly;
  invalidOnly.files[kProgressPath] = {0x00, 0x01, 0x02, 0x03, 0x04};
  invalidOnly.files[kBackupPath] = {0x00, 0x01, 0x02};
  const LegacyPageProgressStore invalidStore(kCachePath, invalidOnly.callbacks());
  ReflowReadingPosition position;
  EXPECT_FALSE(invalidStore.load(position));
}

TEST(LegacyPageProgressStore, WritesExactSixBytesAndRotatesBackup) {
  MemoryProgressIo io;
  io.files[kProgressPath] = {0xAA, 0xBB, 0xCC, 0xDD};
  io.files[kBackupPath] = {0x10, 0x20, 0x30, 0x40};
  io.files[kTempPath] = {0xDE, 0xAD};
  const LegacyPageProgressStore store(kCachePath, io.callbacks());
  const ReflowReadingPosition position = {
      .sectionIndex = 0x1234,
      .pageNumber = 0x5678,
      .pageCount = 0x9ABC,
      .hasPageCount = true,
      .blockAnchor = {},
  };

  ASSERT_TRUE(store.save(position));
  EXPECT_EQ(io.files.at(kProgressPath), (std::vector<uint8_t>{0x34, 0x12, 0x78, 0x56, 0xBC, 0x9A}));
  EXPECT_EQ(io.files.at(kBackupPath), (std::vector<uint8_t>{0xAA, 0xBB, 0xCC, 0xDD}));
  EXPECT_EQ(io.files.count(kTempPath), 0u);
  EXPECT_EQ(io.operations, (std::vector<std::string>{
                               "remove:/cache/progress.bin.tmp",
                               "write:/cache/progress.bin.tmp",
                               "remove:/cache/progress.bin.bak",
                               "rename:/cache/progress.bin->/cache/progress.bin.bak",
                               "rename:/cache/progress.bin.tmp->/cache/progress.bin",
                           }));
}

TEST(LegacyPageProgressStore, RestoresPrimaryWhenFinalRenameFails) {
  MemoryProgressIo io;
  io.files[kProgressPath] = {0x01, 0x00, 0x02, 0x00};
  io.failedRenameSource = kTempPath;
  io.failedRenameDestination = kProgressPath;
  const LegacyPageProgressStore store(kCachePath, io.callbacks());
  const ReflowReadingPosition position = {
      .sectionIndex = 7,
      .pageNumber = 8,
      .pageCount = 9,
      .hasPageCount = true,
      .blockAnchor = {},
  };

  EXPECT_FALSE(store.save(position));
  EXPECT_EQ(io.files.at(kProgressPath), (std::vector<uint8_t>{0x01, 0x00, 0x02, 0x00}));
  EXPECT_EQ(io.files.count(kBackupPath), 0u);
  EXPECT_EQ(io.files.count(kTempPath), 0u);
}

TEST(LegacyPageProgressStore, RejectsValuesOutsideUnsignedSixteenBitRange) {
  const std::vector<ReflowReadingPosition> invalidPositions = {
      {.sectionIndex = -1, .pageNumber = 0, .pageCount = 0, .blockAnchor = {}},
      {.sectionIndex = 0x10000, .pageNumber = 0, .pageCount = 0, .blockAnchor = {}},
      {.sectionIndex = 0, .pageNumber = -1, .pageCount = 0, .blockAnchor = {}},
      {.sectionIndex = 0, .pageNumber = 0x10000, .pageCount = 0, .blockAnchor = {}},
      {.sectionIndex = 0, .pageNumber = 0, .pageCount = -1, .blockAnchor = {}},
      {.sectionIndex = 0, .pageNumber = 0, .pageCount = 0x10000, .blockAnchor = {}},
  };

  for (const auto& invalid : invalidPositions) {
    MemoryProgressIo io;
    const LegacyPageProgressStore store(kCachePath, io.callbacks());
    EXPECT_FALSE(store.save(invalid));
    EXPECT_TRUE(io.files.empty());
    EXPECT_TRUE(io.operations.empty());
  }
}

}  // namespace
