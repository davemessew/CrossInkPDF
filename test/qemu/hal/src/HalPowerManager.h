#pragma once

#include <Arduino.h>

#include "HalGPIO.h"

class HalPowerManager;
extern HalPowerManager powerManager;

class HalPowerManager {
  bool isLowPower = false;
  enum LockMode { None, NormalSpeed };
  LockMode currentLockMode = None;

 public:
  static constexpr int LOW_POWER_FREQ = 10;
  static constexpr unsigned long IDLE_POWER_SAVING_MS = 3000;
  static constexpr unsigned long BATTERY_POLL_MS = 1500;

  void begin();
  void setPowerSaving(bool enabled);
  void startDeepSleep(HalGPIO& gpio) const;
  uint16_t getBatteryPercentage() const;

  class Lock {
    friend class HalPowerManager;
    bool valid = false;

   public:
    explicit Lock();
    ~Lock();
    Lock(const Lock&) = delete;
    Lock& operator=(const Lock&) = delete;
    Lock(Lock&&) = delete;
    Lock& operator=(Lock&&) = delete;
  };
};
