#pragma once

#include <cstddef>
#include <cstdint>

class QemuStorageCapacityCache {
 public:
  constexpr void refresh(const uint64_t capacityBytes, const uint64_t usedBytes) {
    capacityBytes_ = capacityBytes;
    accountedBytes_ = usedBytes;
  }

  constexpr bool canWrite(const size_t count) const {
    return accountedBytes_ <= capacityBytes_ &&
           static_cast<uint64_t>(count) <= capacityBytes_ - accountedBytes_;
  }

  constexpr void charge(const size_t written) {
    const uint64_t remainingBytes = remaining();
    const uint64_t chargedBytes = static_cast<uint64_t>(written);
    accountedBytes_ += chargedBytes <= remainingBytes ? chargedBytes : remainingBytes;
  }

  constexpr uint64_t remaining() const {
    return accountedBytes_ <= capacityBytes_ ? capacityBytes_ - accountedBytes_ : 0;
  }

 private:
  uint64_t capacityBytes_ = 0;
  uint64_t accountedBytes_ = 0;
};

static_assert(sizeof(QemuStorageCapacityCache) == 2 * sizeof(uint64_t));
