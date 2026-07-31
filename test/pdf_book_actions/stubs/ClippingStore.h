#pragma once

#include <string>

#include "TestState.h"

struct ClippingStore {
  static bool deleteForFilePath(const std::string& path, const std::string& type) {
    TEST_STATE.metadataDeletes.push_back("clipping:" + type + ":" + path);
    return true;
  }
};
