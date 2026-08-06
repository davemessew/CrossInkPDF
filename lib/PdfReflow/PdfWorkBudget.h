#pragma once

#include <cstddef>
#include <cstdint>

struct PdfWorkBudget {
  using StopRequestedFn = bool (*)(void* context);

  uint32_t operationsRemaining = 0;
  size_t bytesRemaining = 0;
  void* stopContext = nullptr;
  StopRequestedFn stopRequestedFn = nullptr;
  void* yieldContext = nullptr;
  StopRequestedFn yieldRequestedFn = nullptr;

  constexpr bool cancelRequested() const {
    return stopRequestedFn != nullptr && stopRequestedFn(stopContext);
  }

  constexpr bool stopRequested() const {
    return cancelRequested() ||
           (yieldRequestedFn != nullptr && yieldRequestedFn(yieldContext));
  }

  constexpr bool consumeOperation() {
    if (operationsRemaining == 0 || stopRequested()) {
      return false;
    }
    --operationsRemaining;
    return true;
  }

  constexpr size_t takeBytes(const size_t requested) {
    if (stopRequested()) {
      return 0;
    }
    const size_t granted = requested < bytesRemaining ? requested : bytesRemaining;
    bytesRemaining -= granted;
    return granted;
  }
};
