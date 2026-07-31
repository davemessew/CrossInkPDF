#pragma once

#include <string>

#include "TestState.h"

struct BookmarkStore {
  static bool deleteForFilePath(const std::string& path, const std::string& type) {
    TEST_STATE.metadataDeletes.push_back("bookmark:" + type + ":" + path);
    return true;
  }
};
