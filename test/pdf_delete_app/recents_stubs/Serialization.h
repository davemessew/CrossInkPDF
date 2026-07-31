#pragma once

#include <string>

namespace serialization {
template <typename TFile, typename TValue>
bool readPod(TFile&, TValue& value) {
  value = TValue{};
  return false;
}

template <typename TFile>
bool readString(TFile&, std::string&) {
  return false;
}
}  // namespace serialization
