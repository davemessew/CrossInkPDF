#pragma once

#include <Arduino.h>

#include "HalGPIO.h"

namespace CrossPointOrientation {
enum Value : uint8_t { PORTRAIT = 0, LANDSCAPE_CW = 1, INVERTED = 2, LANDSCAPE_CCW = 3 };
}

namespace CrossPointTiltPageTurn {
enum Value : uint8_t { TILT_OFF = 0, TILT_ON = 1 };
}

namespace CrossPointTiltPageTurnDirection {
enum Value : uint8_t {
  TILT_LEFT_RIGHT = 0,
  TILT_LEFT_RIGHT_INVERTED = 1,
  TILT_FORWARD_BACK = 2,
  TILT_FORWARD_BACK_INVERTED = 3
};
}

class HalTiltSensor;
extern HalTiltSensor halTiltSensor;

class HalTiltSensor {
  bool _available = false;

  bool writeReg(uint8_t reg, uint8_t val) const;
  bool readReg(uint8_t reg, uint8_t* val) const;
  bool readGyro(float& gx, float& gy, float& gz) const;

 public:
  void begin();
  bool wake();
  bool deepSleep();
  bool isAvailable() const { return _available; }
  void update(const uint8_t enabled, const uint8_t direction, const uint8_t orientation, const bool inReader);
  bool wasTiltedForward();
  bool wasTiltedBack();
  bool hadActivity();
  void clearPendingEvents();
};
