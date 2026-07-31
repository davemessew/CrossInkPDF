#pragma once

#include <cstdint>

#include "TestState.h"

struct GlobalReadingStats {
  uint32_t completedBooks = 0;

  static GlobalReadingStats load() {
    ++TEST_STATE.globalLoads;
    return {TEST_STATE.globalCompleted};
  }

  void save() const {
    ++TEST_STATE.globalSaves;
    TEST_STATE.globalCompleted = completedBooks;
  }
};
