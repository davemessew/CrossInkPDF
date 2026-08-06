#pragma once

#include <HalStorage.h>
#include <Print.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

class ZipFile {
 public:
  struct SizeTarget {
    uint64_t hash;
    uint16_t len;
    uint16_t index;
  };

  static uint64_t fnvHash64(const char* value, const size_t length) {
    uint64_t hash = 14695981039346656037ull;
    for (size_t index = 0; index < length; ++index) {
      hash ^= static_cast<uint8_t>(value[index]);
      hash *= 1099511628211ull;
    }
    return hash;
  }

  explicit ZipFile(const std::string&) {}
  bool getInflatedFileSize(const char*, size_t*) { return false; }
  uint8_t* readFileToMemory(const char*, size_t* = nullptr, bool = false) { return nullptr; }
  bool readFileToStream(const char*, Print&, size_t) { return false; }
  int fillUncompressedSizes(const SizeTarget*, size_t, uint32_t*, size_t) { return 0; }
  template <typename Callback>
  bool enumerateFilePaths(Callback&&) {
    return true;
  }
};
