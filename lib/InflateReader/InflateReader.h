#pragma once

#include <uzlib.h>

#include <cstddef>
#include <cstdint>

// Small one-shot deflate decompressor wrapping uzlib.
//
// Retained only for FontDecompressor's tiny flash-resident group
// decompressions, where uzlib's ~1KB state beats tinfl's ~11KB on the
// OOM-sensitive render path. All throughput paths (zip entries, PNG IDAT) use
// InflateStream (lib/miniz), which decodes several times faster.
class InflateReader {
 public:
  InflateReader() = default;
  InflateReader(const InflateReader&) = delete;
  InflateReader& operator=(const InflateReader&) = delete;

  // Initialize one-shot mode. The destination buffer holds the entire output,
  // so back-references resolve inside it without a separate 32KB dictionary.
  void init();

  // Set the entire compressed input as a contiguous memory buffer.
  // Used before the single read() call.
  void setSource(const uint8_t* src, size_t len);

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
  bool usesExternalDictionary() const { return ringBuffer != nullptr && !ownsRingBuffer; }

 private:
  uzlib_uncomp decomp = {};
};
