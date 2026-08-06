#pragma once

#include <atomic>

struct FakeCrossPointState {
  std::atomic<bool> hasPendingAlert{false};
  std::atomic<bool> pendingAlertGoHomeOnBack{false};
  char pendingAlertTitle[64]{};
  char pendingAlertBody[256]{};
};

extern FakeCrossPointState TEST_APP_STATE;
#define APP_STATE TEST_APP_STATE
