#pragma once

#include <cstddef>
#include <cstdint>

namespace TestSha256 {

struct Context {
  uint32_t state[8]{};
  uint64_t bitLength = 0;
  uint8_t block[64]{};
  size_t blockLength = 0;
};

bool start(void* context);
bool update(void* context, const uint8_t* data, size_t length);
bool finish(void* context, uint8_t digest[32]);
void abort(void* context);

}  // namespace TestSha256
