#pragma once

#include <string>

#include "TestState.h"

struct EpubReaderActivity {
  static bool resetBookReaderSettings(const std::string&) {
    ++TEST_STATE.resetReaderSettingsCalls;
    return true;
  }
};
