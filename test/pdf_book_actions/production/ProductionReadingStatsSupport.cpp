#include "activities/reader/ReadingStatsUtils.h"

#include "TestState.h"

bool ReadingStatsDate::isValid() const {
  return year >= 2000 && year <= 2099 && month >= 1 && month <= 12 && day >= 1 && day <= 31;
}

void ReadingStatsDate::clear() {
  year = 0;
  month = 0;
  day = 0;
}

bool getCurrentLocalReadingStatsDateTime(ReadingStatsDateTime& value) {
  if (!TEST_STATE.dateAvailable) return false;
  value = {};
  value.date = {2026, 7, 31};
  return true;
}

void recordReadingSpanIntoBuckets(std::array<uint32_t, READING_TIME_BUCKET_COUNT>&,
                                  std::array<uint32_t, READING_DAY_OF_WEEK_COUNT>&,
                                  const ReadingStatsDateTime&, uint32_t) {}
