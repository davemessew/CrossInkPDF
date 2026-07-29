#pragma once

#include <cstdint>

namespace QemuHalControl {
uint32_t frameCrc32();
uint32_t storageOpenCount();
uint32_t storageCloseCount();
void setStorageQuota(uint64_t bytes);
uint64_t storageQuota();
uint64_t storageCapacity();
uint64_t storageFree();
bool powerSavingEnabled();
}  // namespace QemuHalControl
