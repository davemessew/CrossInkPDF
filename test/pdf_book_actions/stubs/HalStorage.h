#pragma once

#include <string>

#include "TestState.h"

struct FsFile {
  bool open = false;
  bool directory = false;

  explicit operator bool() const { return open; }
  bool isDirectory() const { return open && directory; }
  bool close() {
    open = false;
    return true;
  }
};

struct FakeStorage {
  bool exists(const char*) const { return TEST_STATE.cacheRootExists; }

  bool mkdir(const char*, const bool = true) {
    ++TEST_STATE.cacheRootMkdirCalls;
    if (!TEST_STATE.cacheRootMkdirResult) return false;
    TEST_STATE.cacheRootExists = true;
    return true;
  }

  FsFile open(const char*) {
    ++TEST_STATE.cacheRootOpenCalls;
    return {TEST_STATE.cacheRootExists, TEST_STATE.cacheRootIsDirectory};
  }

  bool rename(const char* oldPath, const char* newPath) {
    ++TEST_STATE.storageRenames;
    TEST_STATE.storageRenameOld = oldPath == nullptr ? "" : oldPath;
    TEST_STATE.storageRenameNew = newPath == nullptr ? "" : newPath;
    return TEST_STATE.storageRenameResult;
  }
};

extern FakeStorage Storage;
