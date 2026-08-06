#pragma once

#include <cstdint>

class EspProductionTestStub {
 public:
  uint32_t getFreeHeap() const { return 256U * 1024U; }
  uint32_t getMaxAllocHeap() const { return 128U * 1024U; }
};

inline EspProductionTestStub ESP;
inline uint32_t millis() {
  static uint32_t value = 0;
  return ++value;
}
inline void delay(uint32_t) {}
