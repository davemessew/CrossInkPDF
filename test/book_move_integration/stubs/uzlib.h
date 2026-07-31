#pragma once

#include <cstddef>
#include <cstdint>

inline uint32_t uzlib_crc32(const void* data, unsigned int length, uint32_t crc) {
  const auto* bytes = static_cast<const uint8_t*>(data);
  crc = ~crc;
  for (unsigned int index = 0; index < length; ++index) {
    crc ^= bytes[index];
    for (uint8_t bit = 0; bit < 8; ++bit) {
      crc = (crc >> 1U) ^ (0xedb88320U & (0U - (crc & 1U)));
    }
  }
  return ~crc;
}
