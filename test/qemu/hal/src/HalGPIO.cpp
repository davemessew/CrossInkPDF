#include "HalGPIO.h"

HalGPIO gpio;

void HalGPIO::begin() {
  lastUsbConnected = false;
  usbStateChanged = false;
}

void HalGPIO::update() { usbStateChanged = false; }

bool HalGPIO::isPressed(uint8_t buttonIndex) const {
  (void)buttonIndex;
  return false;
}

bool HalGPIO::wasPressed(uint8_t buttonIndex) const {
  (void)buttonIndex;
  return false;
}

bool HalGPIO::wasAnyPressed() const { return false; }

bool HalGPIO::wasReleased(uint8_t buttonIndex) const {
  (void)buttonIndex;
  return false;
}

bool HalGPIO::wasAnyReleased() const { return false; }

unsigned long HalGPIO::getHeldTime() const { return 0; }

unsigned long HalGPIO::getPowerButtonHeldTime() const { return 0; }

void HalGPIO::startDeepSleep() {}

bool HalGPIO::verifyPowerButtonWakeup(uint16_t requiredDurationMs, bool shortPressAllowed) {
  (void)requiredDurationMs;
  (void)shortPressAllowed;
  return true;
}

void HalGPIO::setSharedConfirmPowerShortPressEmitsPower(bool enabled) { (void)enabled; }

bool HalGPIO::isUsbConnected() const { return false; }

bool HalGPIO::wasUsbStateChanged() const { return usbStateChanged; }

HalGPIO::WakeupReason HalGPIO::getWakeupReason() const { return WakeupReason::Other; }
