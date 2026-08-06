#pragma once

#include <cstddef>

namespace ScratchWorkspace {
class Lease {
 public:
  explicit Lease(const bool valid = false) : valid_(valid) {}
  explicit operator bool() const { return valid_; }

 private:
  bool valid_ = false;
};
inline Lease acquire(size_t, const char*) { return Lease(false); }
}  // namespace ScratchWorkspace
