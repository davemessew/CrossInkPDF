#include "HalClock.h"

HalClock halClock;

void HalClock::begin() { _available = false; }

bool HalClock::getTime(uint8_t& hour, uint8_t& minute) const {
  (void)hour;
  (void)minute;
  return false;
}

bool HalClock::formatTime(char* buf, size_t bufSize, uint8_t utcOffsetQuarterHoursBiased, bool use12Hour) const {
  (void)utcOffsetQuarterHoursBiased;
  (void)use12Hour;
  if (buf != nullptr && bufSize > 0) {
    buf[0] = '\0';
  }
  return false;
}

bool HalClock::getDate(uint16_t& year, uint8_t& month, uint8_t& day, uint8_t& hour, uint8_t& minute) const {
  (void)year;
  (void)month;
  (void)day;
  (void)hour;
  (void)minute;
  return false;
}

bool HalClock::formatDate(char* buf, size_t bufSize, uint8_t utcOffsetQuarterHoursBiased) const {
  (void)utcOffsetQuarterHoursBiased;
  if (buf != nullptr && bufSize > 0) {
    buf[0] = '\0';
  }
  return false;
}

bool HalClock::writeDateTimeToRTC(uint16_t year, uint8_t month, uint8_t day, uint8_t weekday, uint8_t hour,
                                  uint8_t minute, uint8_t second) {
  (void)year;
  (void)month;
  (void)day;
  (void)weekday;
  (void)hour;
  (void)minute;
  (void)second;
  return false;
}

bool HalClock::syncFromNTP() { return false; }
