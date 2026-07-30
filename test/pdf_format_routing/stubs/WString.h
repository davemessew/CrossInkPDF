#pragma once

#include <cstddef>

class String {
 public:
  const char* c_str() const;
  size_t length() const;
};
