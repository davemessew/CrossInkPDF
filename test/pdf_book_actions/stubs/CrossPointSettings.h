#pragma once

#include <cstdint>

struct FakeCrossPointSettings {
  uint8_t removeReadBooksFromRecents = 0;
  uint8_t moveFinishedToReadFolder = 0;
};

extern FakeCrossPointSettings TEST_SETTINGS;
#define SETTINGS TEST_SETTINGS
