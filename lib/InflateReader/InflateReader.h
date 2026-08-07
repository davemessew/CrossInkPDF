#pragma once

#include <uzlib.h>

#include <cstddef>
#include <cstdint>
#include <memory>

enum class InflateStatus {
  Ok,
  Done,
  Error,
};

// Small deflate decompressor wrapping uzlib.
//
// FontDecompressor uses one-shot mode. PDF stream decoding supplies its own
// 32KB dictionary from the preparation workspace so it does not allocate or
// borrow the framebuffer.
class InflateReader {
 public:
  static constexpr size_t STREAMING_DICT_SIZE = 32768;

  InflateReader() = default;
  ~InflateReader();
  InflateReader(const InflateReader&) = delete;
  InflateReader& operator=(const InflateReader&) = delete;

  // Initialize one-shot mode, or allocate a dictionary for legacy streaming
  // callers. PDF decoding uses initWithExternalDictionary() instead.
  bool init(bool streaming = false);
  bool initWithExternalDictionary(uint8_t* dictionary, size_t dictionarySize);
  void deinit();

  // Set the entire compressed input as a contiguous memory buffer.
  // Used before the single read() call.
  void setSource(const uint8_t* src, size_t len);
  void setReadCallback(int (*cb)(uzlib_uncomp*));
  void skipZlibHeader();

  // Decompress exactly len bytes into dest.
  // Returns false if the stream ends before producing len bytes, or on error.
  bool read(uint8_t* dest, size_t len);

  // Decompress up to maxLen bytes into dest.
  // Sets *produced to the number of bytes written.
  // Returns Done when the stream ends cleanly, Ok when there is more to read,
  // and Error on failure.
  InflateStatus readAtMost(uint8_t* dest, size_t maxLen, size_t* produced);

  // Returns a pointer to the underlying TINF_DATA.
  // Useful for advanced streaming setups where the callback needs access to the
  // uzlib struct directly (e.g. updating source/source_limit).
  uzlib_uncomp* raw() { return &decomp; }
  bool usesExternalDictionary() const { return ringBuffer != nullptr && !ownedRingBuffer; }

 private:
  uzlib_uncomp decomp = {};
  std::unique_ptr<uint8_t[]> ownedRingBuffer;
  uint8_t* ringBuffer = nullptr;
};
