#include "TestSha256.h"

#include <cstring>
#include <new>

namespace TestSha256 {
namespace {

constexpr uint32_t kRoundConstants[64] = {
    0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U, 0x3956c25bU, 0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U,
    0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U, 0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U, 0xc19bf174U,
    0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU, 0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU,
    0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U, 0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U,
    0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU, 0x53380d13U, 0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U,
    0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U, 0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U,
    0x19a4c116U, 0x1e376c08U, 0x2748774cU, 0x34b0bcb5U, 0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
    0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U, 0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U,
};

uint32_t rotateRight(const uint32_t value, const uint8_t count) { return (value >> count) | (value << (32U - count)); }

uint32_t loadBigEndian(const uint8_t* data) {
  return (static_cast<uint32_t>(data[0]) << 24U) | (static_cast<uint32_t>(data[1]) << 16U) |
         (static_cast<uint32_t>(data[2]) << 8U) | static_cast<uint32_t>(data[3]);
}

void transform(Context& context) {
  uint32_t words[64]{};
  for (size_t index = 0; index < 16; ++index) {
    words[index] = loadBigEndian(context.block + index * 4);
  }
  for (size_t index = 16; index < 64; ++index) {
    const uint32_t s0 =
        rotateRight(words[index - 15], 7) ^ rotateRight(words[index - 15], 18) ^ (words[index - 15] >> 3U);
    const uint32_t s1 =
        rotateRight(words[index - 2], 17) ^ rotateRight(words[index - 2], 19) ^ (words[index - 2] >> 10U);
    words[index] = words[index - 16] + s0 + words[index - 7] + s1;
  }

  uint32_t a = context.state[0];
  uint32_t b = context.state[1];
  uint32_t c = context.state[2];
  uint32_t d = context.state[3];
  uint32_t e = context.state[4];
  uint32_t f = context.state[5];
  uint32_t g = context.state[6];
  uint32_t h = context.state[7];
  for (size_t index = 0; index < 64; ++index) {
    const uint32_t choose = (e & f) ^ (~e & g);
    const uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
    const uint32_t sum0 = rotateRight(a, 2) ^ rotateRight(a, 13) ^ rotateRight(a, 22);
    const uint32_t sum1 = rotateRight(e, 6) ^ rotateRight(e, 11) ^ rotateRight(e, 25);
    const uint32_t temporary1 = h + sum1 + choose + kRoundConstants[index] + words[index];
    const uint32_t temporary2 = sum0 + majority;
    h = g;
    g = f;
    f = e;
    e = d + temporary1;
    d = c;
    c = b;
    b = a;
    a = temporary1 + temporary2;
  }

  context.state[0] += a;
  context.state[1] += b;
  context.state[2] += c;
  context.state[3] += d;
  context.state[4] += e;
  context.state[5] += f;
  context.state[6] += g;
  context.state[7] += h;
}

}  // namespace

bool start(void* const opaque) {
  if (opaque == nullptr) return false;
  auto& context = *new (opaque) Context{};
  context.state[0] = 0x6a09e667U;
  context.state[1] = 0xbb67ae85U;
  context.state[2] = 0x3c6ef372U;
  context.state[3] = 0xa54ff53aU;
  context.state[4] = 0x510e527fU;
  context.state[5] = 0x9b05688cU;
  context.state[6] = 0x1f83d9abU;
  context.state[7] = 0x5be0cd19U;
  return true;
}

bool update(void* const opaque, const uint8_t* data, const size_t length) {
  if (opaque == nullptr || (data == nullptr && length != 0)) return false;
  auto& context = *static_cast<Context*>(opaque);
  for (size_t index = 0; index < length; ++index) {
    context.block[context.blockLength++] = data[index];
    if (context.blockLength == sizeof(context.block)) {
      transform(context);
      context.bitLength += 512;
      context.blockLength = 0;
    }
  }
  return true;
}

bool finish(void* const opaque, uint8_t digest[32]) {
  if (opaque == nullptr || digest == nullptr) return false;
  auto& context = *static_cast<Context*>(opaque);
  context.bitLength += static_cast<uint64_t>(context.blockLength) * 8U;
  context.block[context.blockLength++] = 0x80U;
  if (context.blockLength > 56) {
    while (context.blockLength < sizeof(context.block)) context.block[context.blockLength++] = 0;
    transform(context);
    context.blockLength = 0;
  }
  while (context.blockLength < 56) context.block[context.blockLength++] = 0;
  for (size_t index = 0; index < 8; ++index) {
    context.block[63 - index] = static_cast<uint8_t>(context.bitLength >> (index * 8U));
  }
  transform(context);

  for (size_t index = 0; index < 8; ++index) {
    digest[index * 4] = static_cast<uint8_t>(context.state[index] >> 24U);
    digest[index * 4 + 1] = static_cast<uint8_t>(context.state[index] >> 16U);
    digest[index * 4 + 2] = static_cast<uint8_t>(context.state[index] >> 8U);
    digest[index * 4 + 3] = static_cast<uint8_t>(context.state[index]);
  }
  context = Context{};
  return true;
}

void abort(void* const opaque) {
  if (opaque != nullptr) *static_cast<Context*>(opaque) = Context{};
}

}  // namespace TestSha256
