#pragma once

#include <SdFat.h>

#include <cstdint>

#include "QemuSemihost.h"

class QemuSdBlockDevice final : public FsBlockDeviceInterface {
 public:
  bool begin();
  void end() override;

  bool isBusy() override { return false; }
  bool readSector(Sector_t sector, uint8_t* dst) override;
  bool readSectors(Sector_t sector, uint8_t* dst, size_t count) override;
  Sector_t sectorCount() override { return sectorCount_; }
  bool syncDevice() override;
  bool writeSector(Sector_t sector, const uint8_t* src) override;
  bool writeSectors(Sector_t sector, const uint8_t* src, size_t count) override;

  bool formatAllowed() const;

 private:
  bool transfer(Sector_t sector, uint8_t* data, size_t count, bool write);

  int image_ = -1;
  Sector_t sectorCount_ = 0;
};
