#include "HalPowerManager.h"

#include "QemuHalControl.h"

HalPowerManager powerManager;

namespace {
bool qemuPowerSavingEnabled = false;
}

void HalPowerManager::begin() {
  isLowPower = false;
  qemuPowerSavingEnabled = false;
}

void HalPowerManager::setPowerSaving(bool enabled) {
  isLowPower = enabled;
  qemuPowerSavingEnabled = enabled;
}

void HalPowerManager::startDeepSleep(HalGPIO& gpio) const {
  (void)gpio;
  qemuPowerSavingEnabled = true;
}

uint16_t HalPowerManager::getBatteryPercentage() const { return 100; }

HalPowerManager::Lock::Lock() : valid(true) { powerManager.setPowerSaving(false); }

HalPowerManager::Lock::~Lock() {
  if (valid) {
    powerManager.setPowerSaving(true);
  }
}

bool QemuHalControl::powerSavingEnabled() { return qemuPowerSavingEnabled; }
