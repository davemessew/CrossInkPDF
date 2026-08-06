#pragma once

#include <cstdint>
#include <string>

#include "TestState.h"

struct ReadingStatsDate {
  uint16_t year = 0;
  uint8_t month = 0;
  uint8_t day = 0;
};

struct ReadingStatsDateTime {
  ReadingStatsDate date{};
};

inline bool getCurrentLocalReadingStatsDateTime(ReadingStatsDateTime& value) {
  if (!TEST_STATE.dateAvailable) return false;
  value.date = {2026, 7, 31};
  return true;
}

struct BookReadingStats {
  bool isCompleted = false;
  bool finishedDateManual = false;
  ReadingStatsDate finishedDate{};

  static BookReadingStats load(const std::string& cachePath) {
    TEST_STATE.statsLoads.push_back(cachePath);
    BookReadingStats value;
    const auto found = TEST_STATE.completedByCache.find(cachePath);
    value.isCompleted = found != TEST_STATE.completedByCache.end() && found->second;
    return value;
  }

  void save(const std::string& cachePath) const {
    TEST_STATE.statsSaves.push_back(cachePath);
    TEST_STATE.completedByCache[cachePath] = isCompleted;
  }

  static bool remove(const std::string& cachePath) {
    TEST_STATE.statsRemoves.push_back(cachePath);
    return TEST_STATE.statsRemoveResult;
  }
};
