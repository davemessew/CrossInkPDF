#pragma once

#include <string>
#include <string_view>

#include "TestState.h"

struct BookmarkStore {
  static bool deleteForFilePath(const std::string& path, const std::string& type) {
    ++TEST_STATE.owningMetadataPathCalls;
    TEST_STATE.metadataDeletes.push_back("bookmark:" + type + ":" + path);
    return true;
  }

  static bool deleteLegacyForFilePathNoPathAlloc(const std::string_view path, const std::string_view type) {
    TEST_STATE.metadataDeletes.emplace_back("bookmark:" + std::string(type) + ":" + std::string(path));
    return TEST_STATE.bookmarkNoPathAllocResult;
  }
};
