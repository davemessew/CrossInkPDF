#pragma once

#include <cstdint>

class CrossPointSettings {
 public:
  uint8_t showHiddenFiles = 0;

  static CrossPointSettings& getInstance() {
    static CrossPointSettings instance;
    return instance;
  }
};

#define SETTINGS CrossPointSettings::getInstance()
