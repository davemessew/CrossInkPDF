#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace serialization {
template <typename File, typename T>
bool tryReadPod(File& file, T& value) {
  return file.read(&value, sizeof(value)) == static_cast<int>(sizeof(value));
}
template <typename File, typename T>
bool tryWritePod(File& file, const T& value) {
  return file.write(reinterpret_cast<const uint8_t*>(&value), sizeof(value)) == sizeof(value);
}
template <typename File>
bool tryReadString(File&, std::string&) {
  return false;
}
template <typename File>
bool tryWriteString(File&, const std::string&) {
  return false;
}
}  // namespace serialization
