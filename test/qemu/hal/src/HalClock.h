#pragma once

#include <Arduino.h>

#include "HalGPIO.h"

class HalClock;
extern HalClock halClock;

class HalClock {
  bool _available = false;

 public:
  enum DateFormat : uint8_t {
    MONTH_DAY_YEAR_LONG = 0,
    DAY_MONTH_YEAR_LONG,
    MONTH_DAY_YEAR_NUMERIC,
    DAY_MONTH_YEAR_NUMERIC,
    YEAR_MONTH_DAY_NUMERIC,
    MONTH_DAY_NUMERIC,
    DAY_MONTH_NUMERIC,
    MONTH_DAY_LONG,
    DAY_MONTH_LONG,
    DATE_FORMAT_COUNT
  };

  void begin();
  bool isAvailable() const { return _available; }
  bool getTime(uint8_t& hour, uint8_t& minute) const;
  bool formatTime(char* buf, size_t bufSize, uint8_t utcOffsetQuarterHoursBiased = 48, bool use12Hour = false) const;
  bool getDateTime(uint16_t& year, uint8_t& month, uint8_t& day, uint8_t& hour, uint8_t& minute) const {
    return getDate(year, month, day, hour, minute);
  }
  bool formatDate(char* buf, size_t bufSize, uint8_t utcOffsetQuarterHoursBiased = 48,
                  DateFormat dateFormat = MONTH_DAY_YEAR_LONG, char numericSeparator = '/') const;
  bool syncFromNTP();

 private:
  bool getDate(uint16_t& year, uint8_t& month, uint8_t& day, uint8_t& hour, uint8_t& minute) const;
  bool writeDateTimeToRTC(uint16_t year, uint8_t month, uint8_t day, uint8_t weekday, uint8_t hour, uint8_t minute,
                          uint8_t second);
};
