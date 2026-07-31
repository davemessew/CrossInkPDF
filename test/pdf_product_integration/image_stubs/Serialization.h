#pragma once

#include <cstdint>
#include <string>

#include "HalStorage.h"

namespace serialization {

template <typename T>
bool tryWritePod(FsFile& file, const T& value) {
  return file.write(&value, sizeof(value)) == sizeof(value);
}

template <typename T>
bool tryReadPod(FsFile& file, T& value) {
  return file.read(&value, sizeof(value)) == static_cast<int>(sizeof(value));
}

inline bool tryWriteString(FsFile& file, const std::string& value) {
  const uint32_t length = static_cast<uint32_t>(value.size());
  return tryWritePod(file, length) && file.write(value.data(), value.size()) == value.size();
}

inline bool tryReadString(FsFile& file, std::string& value) {
  uint32_t length = 0;
  if (!tryReadPod(file, length) || length > 4096) {
    return false;
  }
  value.resize(length);
  return length == 0 || file.read(value.data(), length) == static_cast<int>(length);
}

}  // namespace serialization
