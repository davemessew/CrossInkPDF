#pragma once

#include <cstdint>

class HalFrontlight {
 public:
  static HalFrontlight& getInstance() {
    static HalFrontlight instance;
    return instance;
  }

  void begin(uint8_t brightness, uint8_t warmth, bool on) {
    lastBrightness = brightness > 100 ? 100 : brightness;
    lastWarmth = warmth > 100 ? 100 : warmth;
    lit = on;
  }
  bool present() const { return false; }
  bool hasColorTemperature() const { return false; }
  void setBrightness(uint8_t percent) { lastBrightness = percent > 100 ? 100 : percent; }
  void setWarmth(uint8_t percent) { lastWarmth = percent > 100 ? 100 : percent; }
  void setOn(bool on) { lit = on; }
  uint8_t brightness() const { return lastBrightness; }
  uint8_t warmth() const { return lastWarmth; }
  bool isOn() const { return lit; }

 private:
  uint8_t lastBrightness = 60;
  uint8_t lastWarmth = 50;
  bool lit = false;
};

#define Frontlight HalFrontlight::getInstance()
