#include "HalSpiBus.h"

HalSpiBus::HalSpiBus() = default;

HalSpiBus& HalSpiBus::getInstance() {
  static HalSpiBus instance;
  return instance;
}

HalSpiBus::Lock::Lock() : acquired(true) {}

HalSpiBus::Lock::~Lock() { acquired = false; }
