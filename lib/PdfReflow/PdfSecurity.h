#pragma once

#include <cstddef>
#include <cstdint>

#include "PdfObjectParser.h"
#include "PdfTypes.h"

struct PdfSecurityTrailer {
  static constexpr size_t FileIdentifierCapacity = sizeof(PdfToken::bytes);
  PdfObjectReference encryptionReference{};
  uint8_t fileIdentifier[FileIdentifierCapacity]{};
  uint8_t fileIdentifierLength = 0;
  bool encrypted = false;
};

// Narrow Standard Security handler for freely-openable V4/R4 RC4-128 PDFs.
// It deliberately has no password UI and rejects AES or non-empty-password
// documents as PdfError::Encrypted.
class PdfSecurity {
 public:
  void reset();
  PdfStatus initializeEmptyPassword(const PdfObjectArena& arena, uint16_t dictionaryIndex,
                                    const PdfSecurityTrailer& trailer);

  bool active() const { return enabled_; }
  PdfObjectReference encryptionReference() const { return encryptionReference_; }

  // PDF strings are encrypted independently with the containing indirect
  // object's key. Object-stream members must not call this: their bytes were
  // already decrypted with the ObjStm key before parsing.
  PdfStatus decryptStrings(PdfObjectArena& arena, PdfObjectReference reference);

  // Returns a source that decrypts one indirect object's stream before PDF
  // filters run. Only one encrypted source may be active at a time, matching
  // the firmware's single-reader SD contract.
  PdfStatus openStream(const PdfByteSource& encoded, PdfObjectReference reference, PdfByteSource* decrypted);

 private:
  struct Rc4Stream {
    PdfByteSource source{};
    PdfObjectReference reference{};
    uint64_t position = 0;
    uint8_t permutation[256]{};
    uint8_t objectKey[16]{};
    uint8_t keyLength = 0;
    uint8_t x = 0;
    uint8_t y = 0;
  };

  static PdfStatus readStream(void* context, uint64_t offset, uint8_t* destination, size_t requested,
                              size_t* bytesRead);
  PdfStatus makeObjectKey(PdfObjectReference reference, uint8_t* key, uint8_t* keyLength) const;
  void initializeRc4(const uint8_t* key, uint8_t keyLength);
  uint8_t nextRc4Byte();
  PdfStatus crypt(const uint8_t* key, uint8_t keyLength, uint8_t* bytes, size_t length);
  PdfStatus seekRc4(uint64_t offset);

  Rc4Stream stream_{};
  PdfObjectReference encryptionReference_{};
  uint8_t fileKey_[16]{};
  uint8_t keyLength_ = 0;
  bool enabled_ = false;
};

static_assert(sizeof(PdfSecurity) <= 384, "PDF security state must stay below 384 bytes");
