#include "InflateReader.h"

#include <Memory.h>

#include <cstring>
#include <type_traits>

static_assert(std::is_standard_layout<InflateReader>::value,
              "InflateReader must remain standard-layout for uzlib callbacks");

InflateReader::~InflateReader() { deinit(); }

bool InflateReader::init(const bool streaming) {
  deinit();
  if (streaming) {
    ownedRingBuffer = makeUniqueNoThrow<uint8_t[]>(STREAMING_DICT_SIZE);
    if (!ownedRingBuffer) return false;
    ringBuffer = ownedRingBuffer.get();
  }
  uzlib_uncompress_init(&decomp, ringBuffer, ringBuffer ? STREAMING_DICT_SIZE : 0);
  return true;
}

bool InflateReader::initWithExternalDictionary(uint8_t* dictionary, const size_t dictionarySize) {
  deinit();
  if (!dictionary || dictionarySize < STREAMING_DICT_SIZE) return false;

  ringBuffer = dictionary;
  memset(ringBuffer, 0, STREAMING_DICT_SIZE);
  uzlib_uncompress_init(&decomp, ringBuffer, STREAMING_DICT_SIZE);
  return true;
}

void InflateReader::deinit() {
  ringBuffer = nullptr;
  ownedRingBuffer.reset();
  memset(&decomp, 0, sizeof(decomp));
}

void InflateReader::setSource(const uint8_t* src, size_t len) {
  decomp.source = src;
  decomp.source_limit = src + len;
}

void InflateReader::setReadCallback(int (*cb)(uzlib_uncomp*)) { decomp.source_read_cb = cb; }

void InflateReader::skipZlibHeader() {
  uzlib_get_byte(&decomp);
  uzlib_get_byte(&decomp);
}

bool InflateReader::read(uint8_t* dest, size_t len) {
  if (!ringBuffer) decomp.dest_start = dest;
  decomp.dest = dest;
  decomp.dest_limit = dest + len;

  const int res = uzlib_uncompress(&decomp);
  if (res < 0) return false;
  return decomp.dest == decomp.dest_limit;
}

InflateStatus InflateReader::readAtMost(uint8_t* dest, size_t maxLen, size_t* produced) {
  if (!ringBuffer) decomp.dest_start = dest;
  decomp.dest = dest;
  decomp.dest_limit = dest + maxLen;

  const int res = uzlib_uncompress(&decomp);
  *produced = static_cast<size_t>(decomp.dest - dest);
  if (res == TINF_DONE) return InflateStatus::Done;
  if (res < 0) return InflateStatus::Error;
  return InflateStatus::Ok;
}
