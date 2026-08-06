#include "HalTiltSensor.h"

HalTiltSensor halTiltSensor;

bool HalTiltSensor::writeReg(uint8_t reg, uint8_t val) const {
  (void)reg;
  (void)val;
  return false;
}

bool HalTiltSensor::readReg(uint8_t reg, uint8_t* val) const {
  (void)reg;
  (void)val;
  return false;
}

bool HalTiltSensor::readGyro(float& gx, float& gy, float& gz) const {
  (void)gx;
  (void)gy;
  (void)gz;
  return false;
}

void HalTiltSensor::begin() { _available = false; }

bool HalTiltSensor::wake() { return false; }

bool HalTiltSensor::deepSleep() { return true; }

void HalTiltSensor::update(const uint8_t enabled, const uint8_t direction, const uint8_t orientation,
                           const bool inReader) {
  (void)enabled;
  (void)direction;
  (void)orientation;
  (void)inReader;
}

bool HalTiltSensor::wasTiltedForward() { return false; }

bool HalTiltSensor::wasTiltedBack() { return false; }

bool HalTiltSensor::hadActivity() { return false; }

void HalTiltSensor::clearPendingEvents() {}
