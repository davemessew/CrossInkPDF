#include "HalSystem.h"

void HalSystem::begin() {}

void HalSystem::checkPanic() {}

void HalSystem::clearPanic() {}

std::string HalSystem::getPanicInfo(bool full) {
  (void)full;
  return {};
}

bool HalSystem::isRebootFromPanic() { return false; }
