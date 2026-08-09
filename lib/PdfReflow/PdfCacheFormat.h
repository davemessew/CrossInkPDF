#pragma once

#include <cstddef>
#include <cstdint>

constexpr uint16_t PDF_CACHE_CODEC_VERSION = 1;
constexpr uint16_t PDF_CACHE_FORMAT_VERSION = 1;
constexpr uint16_t PDF_CACHE_CAPABILITY_VERSION = 3;
constexpr uint64_t PDF_CACHE_FNV64_OFFSET = 14695981039346656037ULL;
constexpr uint64_t PDF_CACHE_FNV64_PRIME = 1099511628211ULL;
constexpr uint64_t PDF_CACHE_MIN_BYTES = 4ULL * 1024ULL * 1024ULL;
constexpr uint64_t PDF_CACHE_MAX_BYTES = 64ULL * 1024ULL * 1024ULL;
constexpr uint64_t PDF_CACHE_SOURCE_OVERHEAD_BYTES = 1ULL * 1024ULL * 1024ULL;
constexpr uint64_t PDF_CACHE_MIN_FREE_RESERVE_BYTES = 16ULL * 1024ULL * 1024ULL;
constexpr uint32_t PDF_CACHE_CHECKPOINT_PAGE_INTERVAL = 8;
constexpr uint64_t PDF_CACHE_CHECKPOINT_BYTE_INTERVAL = 512ULL * 1024ULL;
constexpr uint32_t PDF_CACHE_CHECKPOINT_TIME_INTERVAL_MS = 5000;
constexpr uint32_t PDF_CACHE_MAX_REQUIRED_FILES = 4096;
constexpr uint64_t PDF_CACHE_MAX_SLOT_BYTES = 512ULL * 1024ULL;

inline uint64_t pdfCacheFnv64(const void* const data, const size_t length, uint64_t hash = PDF_CACHE_FNV64_OFFSET) {
  const auto* bytes = static_cast<const uint8_t*>(data);
  for (size_t index = 0; index < length; ++index) {
    hash ^= bytes[index];
    hash *= PDF_CACHE_FNV64_PRIME;
  }
  return hash;
}

inline uint32_t pdfCacheCrc32(const void* const data, const size_t length, uint32_t crc = 0) {
  static constexpr uint32_t table[16] = {
      0x00000000, 0x1db71064, 0x3b6e20c8, 0x26d930ac, 0x76dc4190, 0x6b6b51f4, 0x4db26158, 0x5005713c,
      0xedb88320, 0xf00f9344, 0xd6d6a3e8, 0xcb61b38c, 0x9b64c2b0, 0x86d3d2d4, 0xa00ae278, 0xbdbdf21c,
  };
  const auto* bytes = static_cast<const uint8_t*>(data);
  crc = ~crc;
  for (size_t index = 0; index < length; ++index) {
    crc = (crc >> 4U) ^ table[(crc ^ bytes[index]) & 0x0fU];
    crc = (crc >> 4U) ^ table[(crc ^ (bytes[index] >> 4U)) & 0x0fU];
  }
  return ~crc;
}

inline bool pdfCacheSequenceNewer(const uint32_t candidate, const uint32_t reference) {
  return candidate != reference && static_cast<uint32_t>(candidate - reference) < 0x80000000U;
}
