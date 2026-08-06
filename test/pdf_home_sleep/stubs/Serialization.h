#pragma once

#include <cstddef>

namespace serialization {

template <typename File, typename T>
bool tryReadPod(File& file, T& value) {
  return file.read(&value, sizeof(value)) == static_cast<int>(sizeof(value));
}

}  // namespace serialization
