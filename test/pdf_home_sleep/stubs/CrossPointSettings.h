#pragma once

struct CrossPointSettings {
  enum class SLEEP_SCREEN_MODE {
    COVER,
    COVER_CUSTOM,
    MINIMAL_SLEEP,
    MINIMAL_STATS_SLEEP,
    DASHBOARD_SLEEP,
    OTHER,
  };

  SLEEP_SCREEN_MODE sleepScreen = SLEEP_SCREEN_MODE::OTHER;
  int getReaderFontId() const { return 0; }
};

inline CrossPointSettings SETTINGS;
