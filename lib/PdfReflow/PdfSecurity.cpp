#include "PdfSecurity.h"

#include <algorithm>
#include <cstring>
#include <limits>

namespace {

constexpr uint8_t kPasswordPadding[32] = {
    0x28, 0xbf, 0x4e, 0x5e, 0x4e, 0x75, 0x8a, 0x41, 0x64, 0x00, 0x4e,
    0x56, 0xff, 0xfa, 0x01, 0x08, 0x2e, 0x2e, 0x00, 0xb6, 0xd0, 0x68,
    0x3e, 0x80, 0x2f, 0x0c, 0xa9, 0xfe, 0x64, 0x53, 0x69, 0x7a,
};

constexpr uint32_t kMd5Constants[64] = {
    0xd76aa478U, 0xe8c7b756U, 0x242070dbU, 0xc1bdceeeU, 0xf57c0fafU, 0x4787c62aU, 0xa8304613U,
    0xfd469501U, 0x698098d8U, 0x8b44f7afU, 0xffff5bb1U, 0x895cd7beU, 0x6b901122U, 0xfd987193U,
    0xa679438eU, 0x49b40821U, 0xf61e2562U, 0xc040b340U, 0x265e5a51U, 0xe9b6c7aaU, 0xd62f105dU,
    0x02441453U, 0xd8a1e681U, 0xe7d3fbc8U, 0x21e1cde6U, 0xc33707d6U, 0xf4d50d87U, 0x455a14edU,
    0xa9e3e905U, 0xfcefa3f8U, 0x676f02d9U, 0x8d2a4c8aU, 0xfffa3942U, 0x8771f681U, 0x6d9d6122U,
    0xfde5380cU, 0xa4beea44U, 0x4bdecfa9U, 0xf6bb4b60U, 0xbebfbc70U, 0x289b7ec6U, 0xeaa127faU,
    0xd4ef3085U, 0x04881d05U, 0xd9d4d039U, 0xe6db99e5U, 0x1fa27cf8U, 0xc4ac5665U, 0xf4292244U,
    0x432aff97U, 0xab9423a7U, 0xfc93a039U, 0x655b59c3U, 0x8f0ccc92U, 0xffeff47dU, 0x85845dd1U,
    0x6fa87e4fU, 0xfe2ce6e0U, 0xa3014314U, 0x4e0811a1U, 0xf7537e82U, 0xbd3af235U, 0x2ad7d2bbU,
    0xeb86d391U,
};

constexpr uint8_t kMd5Shifts[64] = {
    7,  12, 17, 22, 7,  12, 17, 22, 7,  12, 17, 22, 7,  12, 17, 22,
    5,  9,  14, 20, 5,  9,  14, 20, 5,  9,  14, 20, 5,  9,  14, 20,
    4,  11, 16, 23, 4,  11, 16, 23, 4,  11, 16, 23, 4,  11, 16, 23,
    6,  10, 15, 21, 6,  10, 15, 21, 6,  10, 15, 21, 6,  10, 15, 21,
};

uint32_t rotateLeft(const uint32_t value, const uint8_t bits) {
  return (value << bits) | (value >> (32U - bits));
}

uint32_t readLe32(const uint8_t* bytes) {
  return static_cast<uint32_t>(bytes[0]) | static_cast<uint32_t>(bytes[1]) << 8U |
         static_cast<uint32_t>(bytes[2]) << 16U | static_cast<uint32_t>(bytes[3]) << 24U;
}

void writeLe32(uint8_t* bytes, const uint32_t value) {
  for (uint8_t index = 0; index < 4; ++index) {
    bytes[index] = static_cast<uint8_t>(value >> (index * 8U));
  }
}

struct Md5State {
  uint32_t hash[4]{0x67452301U, 0xefcdab89U, 0x98badcfeU, 0x10325476U};
  uint64_t byteCount = 0;
  uint8_t buffer[64]{};
  uint8_t buffered = 0;
};

void md5Transform(Md5State* state, const uint8_t* block) {
  uint32_t words[16]{};
  for (uint8_t index = 0; index < 16; ++index) {
    words[index] = readLe32(block + index * 4U);
  }
  uint32_t a = state->hash[0];
  uint32_t b = state->hash[1];
  uint32_t c = state->hash[2];
  uint32_t d = state->hash[3];
  for (uint8_t index = 0; index < 64; ++index) {
    uint32_t function = 0;
    uint8_t word = 0;
    if (index < 16) {
      function = (b & c) | (~b & d);
      word = index;
    } else if (index < 32) {
      function = (d & b) | (~d & c);
      word = static_cast<uint8_t>((5U * index + 1U) & 15U);
    } else if (index < 48) {
      function = b ^ c ^ d;
      word = static_cast<uint8_t>((3U * index + 5U) & 15U);
    } else {
      function = c ^ (b | ~d);
      word = static_cast<uint8_t>((7U * index) & 15U);
    }
    const uint32_t next = b + rotateLeft(a + function + kMd5Constants[index] + words[word], kMd5Shifts[index]);
    a = d;
    d = c;
    c = b;
    b = next;
  }
  state->hash[0] += a;
  state->hash[1] += b;
  state->hash[2] += c;
  state->hash[3] += d;
}

void md5Update(Md5State* state, const uint8_t* bytes, size_t length) {
  if (state == nullptr || (bytes == nullptr && length != 0)) {
    return;
  }
  state->byteCount += length;
  while (length != 0) {
    const size_t copied = std::min<size_t>(length, sizeof(state->buffer) - state->buffered);
    std::memcpy(state->buffer + state->buffered, bytes, copied);
    state->buffered = static_cast<uint8_t>(state->buffered + copied);
    bytes += copied;
    length -= copied;
    if (state->buffered == sizeof(state->buffer)) {
      md5Transform(state, state->buffer);
      state->buffered = 0;
    }
  }
}

void md5Finish(Md5State* state, uint8_t* digest) {
  const uint64_t bitCount = state->byteCount * 8U;
  const uint8_t marker = 0x80;
  md5Update(state, &marker, 1);
  const uint8_t zero = 0;
  while (state->buffered != 56) {
    md5Update(state, &zero, 1);
  }
  uint8_t length[8]{};
  for (uint8_t index = 0; index < 8; ++index) {
    length[index] = static_cast<uint8_t>(bitCount >> (index * 8U));
  }
  md5Update(state, length, sizeof(length));
  for (uint8_t index = 0; index < 4; ++index) {
    writeLe32(digest + index * 4U, state->hash[index]);
  }
}

void md5(const uint8_t* bytes, const size_t length, uint8_t* digest) {
  Md5State state{};
  md5Update(&state, bytes, length);
  md5Finish(&state, digest);
}

bool dictionaryKeyEquals(const PdfObjectArena& arena, const PdfDictionaryEntry& entry, const char* key) {
  const size_t length = std::strlen(key);
  return entry.keyLength == length && static_cast<uint32_t>(entry.keyOffset) + entry.keyLength <= arena.textLength &&
         std::memcmp(arena.text + entry.keyOffset, key, length) == 0;
}

PdfStatus findUnique(const PdfObjectArena& arena, const uint16_t dictionaryIndex, const char* key,
                     uint16_t* valueIndex, bool* found = nullptr) {
  if (key == nullptr || valueIndex == nullptr || dictionaryIndex >= arena.valueCount ||
      arena.values[dictionaryIndex].kind != PdfValueKind::Dictionary) {
    return PdfStatus::failure(PdfError::Encrypted, dictionaryIndex);
  }
  const PdfValue& dictionary = arena.values[dictionaryIndex];
  uint16_t entryIndex = dictionary.firstLink;
  uint8_t matches = 0;
  for (uint16_t ordinal = 0; ordinal < dictionary.count; ++ordinal) {
    if (entryIndex >= arena.dictionaryCount) {
      return PdfStatus::failure(PdfError::Encrypted, dictionaryIndex);
    }
    const PdfDictionaryEntry& entry = arena.dictionaryEntries[entryIndex];
    if (dictionaryKeyEquals(arena, entry, key)) {
      if (++matches != 1 || entry.valueIndex >= arena.valueCount) {
        return PdfStatus::failure(PdfError::Encrypted, dictionaryIndex);
      }
      *valueIndex = entry.valueIndex;
    }
    entryIndex = entry.next;
  }
  if (found != nullptr) {
    *found = matches == 1;
    return PdfStatus::success();
  }
  return matches == 1 ? PdfStatus::success() : PdfStatus::failure(PdfError::Encrypted, dictionaryIndex);
}

bool nameEquals(const PdfObjectArena& arena, const uint16_t valueIndex, const char* expected) {
  return valueIndex < arena.valueCount && arena.values[valueIndex].kind == PdfValueKind::Name &&
         pdfTextEquals(arena, arena.values[valueIndex], expected);
}

bool integerEquals(const PdfObjectArena& arena, const uint16_t valueIndex, const int64_t expected) {
  return valueIndex < arena.valueCount && arena.values[valueIndex].kind == PdfValueKind::Integer &&
         arena.values[valueIndex].integerValue == expected;
}

}  // namespace

void PdfSecurity::reset() {
  stream_ = {};
  encryptionReference_ = {};
  std::memset(fileKey_, 0, sizeof(fileKey_));
  keyLength_ = 0;
  enabled_ = false;
}

PdfStatus PdfSecurity::initializeEmptyPassword(const PdfObjectArena& arena, const uint16_t dictionaryIndex,
                                               const PdfSecurityTrailer& trailer) {
  reset();
  if (!trailer.encrypted || trailer.encryptionReference.objectNumber == 0 || dictionaryIndex >= arena.valueCount ||
      arena.values[dictionaryIndex].kind != PdfValueKind::Dictionary || trailer.fileIdentifierLength == 0 ||
      trailer.fileIdentifierLength > sizeof(trailer.fileIdentifier)) {
    return PdfStatus::failure(PdfError::Encrypted, trailer.encryptionReference.objectNumber);
  }

  uint16_t filter = PDF_INVALID_INDEX;
  uint16_t version = PDF_INVALID_INDEX;
  uint16_t revision = PDF_INVALID_INDEX;
  uint16_t length = PDF_INVALID_INDEX;
  uint16_t owner = PDF_INVALID_INDEX;
  uint16_t user = PDF_INVALID_INDEX;
  uint16_t permissions = PDF_INVALID_INDEX;
  uint16_t cryptFilters = PDF_INVALID_INDEX;
  uint16_t streamFilter = PDF_INVALID_INDEX;
  uint16_t stringFilter = PDF_INVALID_INDEX;
  PdfStatus status = findUnique(arena, dictionaryIndex, "Filter", &filter);
  if (status) status = findUnique(arena, dictionaryIndex, "V", &version);
  if (status) status = findUnique(arena, dictionaryIndex, "R", &revision);
  if (status) status = findUnique(arena, dictionaryIndex, "Length", &length);
  if (status) status = findUnique(arena, dictionaryIndex, "O", &owner);
  if (status) status = findUnique(arena, dictionaryIndex, "U", &user);
  if (status) status = findUnique(arena, dictionaryIndex, "P", &permissions);
  if (status) status = findUnique(arena, dictionaryIndex, "CF", &cryptFilters);
  if (status) status = findUnique(arena, dictionaryIndex, "StmF", &streamFilter);
  if (status) status = findUnique(arena, dictionaryIndex, "StrF", &stringFilter);
  if (!status || !nameEquals(arena, filter, "Standard") || !integerEquals(arena, version, 4) ||
      !integerEquals(arena, revision, 4) || !integerEquals(arena, length, 128) ||
      !nameEquals(arena, streamFilter, "StdCF") || !nameEquals(arena, stringFilter, "StdCF") ||
      cryptFilters >= arena.valueCount || arena.values[cryptFilters].kind != PdfValueKind::Dictionary ||
      owner >= arena.valueCount || user >= arena.valueCount || permissions >= arena.valueCount ||
      arena.values[owner].kind != PdfValueKind::String || arena.values[owner].textLength != 32 ||
      arena.values[user].kind != PdfValueKind::String || arena.values[user].textLength != 32 ||
      arena.values[permissions].kind != PdfValueKind::Integer ||
      arena.values[permissions].integerValue < std::numeric_limits<int32_t>::min() ||
      arena.values[permissions].integerValue > std::numeric_limits<int32_t>::max()) {
    return PdfStatus::failure(PdfError::Encrypted, trailer.encryptionReference.objectNumber);
  }

  uint16_t standardFilter = PDF_INVALID_INDEX;
  status = findUnique(arena, cryptFilters, "StdCF", &standardFilter);
  if (!status || standardFilter >= arena.valueCount ||
      arena.values[standardFilter].kind != PdfValueKind::Dictionary) {
    return PdfStatus::failure(PdfError::Encrypted, trailer.encryptionReference.objectNumber);
  }
  uint16_t method = PDF_INVALID_INDEX;
  uint16_t cryptLength = PDF_INVALID_INDEX;
  status = findUnique(arena, standardFilter, "CFM", &method);
  if (status) status = findUnique(arena, standardFilter, "Length", &cryptLength);
  if (!status || !nameEquals(arena, method, "V2") || !integerEquals(arena, cryptLength, 16)) {
    return PdfStatus::failure(PdfError::Encrypted, trailer.encryptionReference.objectNumber);
  }
  uint16_t optional = PDF_INVALID_INDEX;
  bool found = false;
  status = findUnique(arena, standardFilter, "AuthEvent", &optional, &found);
  if (!status || (found && !nameEquals(arena, optional, "DocOpen"))) {
    return PdfStatus::failure(PdfError::Encrypted, trailer.encryptionReference.objectNumber);
  }
  status = findUnique(arena, dictionaryIndex, "EncryptMetadata", &optional, &found);
  if (!status || (found && (arena.values[optional].kind != PdfValueKind::Boolean ||
                            !arena.values[optional].booleanValue))) {
    return PdfStatus::failure(PdfError::Encrypted, trailer.encryptionReference.objectNumber);
  }
  status = findUnique(arena, dictionaryIndex, "SubFilter", &optional, &found);
  if (!status || found) {
    return PdfStatus::failure(PdfError::Encrypted, trailer.encryptionReference.objectNumber);
  }
  status = findUnique(arena, dictionaryIndex, "EFF", &optional, &found);
  if (!status || found) {
    return PdfStatus::failure(PdfError::Encrypted, trailer.encryptionReference.objectNumber);
  }

  const PdfValue& ownerValue = arena.values[owner];
  const PdfValue& userValue = arena.values[user];
  if (static_cast<uint32_t>(ownerValue.textOffset) + ownerValue.textLength > arena.textLength ||
      static_cast<uint32_t>(userValue.textOffset) + userValue.textLength > arena.textLength) {
    return PdfStatus::failure(PdfError::Encrypted, trailer.encryptionReference.objectNumber);
  }
  const uint8_t* const ownerBytes = arena.text + ownerValue.textOffset;
  const uint8_t* const userBytes = arena.text + userValue.textOffset;
  const int32_t permissionValue = static_cast<int32_t>(arena.values[permissions].integerValue);
  uint8_t permissionBytes[4]{};
  writeLe32(permissionBytes, static_cast<uint32_t>(permissionValue));

  Md5State keyHash{};
  md5Update(&keyHash, kPasswordPadding, sizeof(kPasswordPadding));
  md5Update(&keyHash, ownerBytes, 32);
  md5Update(&keyHash, permissionBytes, sizeof(permissionBytes));
  md5Update(&keyHash, trailer.fileIdentifier, trailer.fileIdentifierLength);
  uint8_t digest[16]{};
  md5Finish(&keyHash, digest);
  for (uint8_t iteration = 0; iteration < 50; ++iteration) {
    keyHash = {};
    md5Update(&keyHash, digest, sizeof(digest));
    md5Finish(&keyHash, digest);
  }

  keyHash = {};
  md5Update(&keyHash, kPasswordPadding, sizeof(kPasswordPadding));
  md5Update(&keyHash, trailer.fileIdentifier, trailer.fileIdentifierLength);
  uint8_t validation[16]{};
  md5Finish(&keyHash, validation);
  status = crypt(digest, sizeof(digest), validation, sizeof(validation));
  uint8_t roundKey[16]{};
  for (uint8_t iteration = 1; status && iteration <= 19; ++iteration) {
    for (uint8_t index = 0; index < sizeof(roundKey); ++index) {
      roundKey[index] = static_cast<uint8_t>(digest[index] ^ iteration);
    }
    status = crypt(roundKey, sizeof(roundKey), validation, sizeof(validation));
  }
  if (!status || std::memcmp(validation, userBytes, sizeof(validation)) != 0) {
    reset();
    return PdfStatus::failure(PdfError::Encrypted, trailer.encryptionReference.objectNumber);
  }

  std::memcpy(fileKey_, digest, sizeof(fileKey_));
  keyLength_ = sizeof(fileKey_);
  encryptionReference_ = trailer.encryptionReference;
  enabled_ = true;
  stream_ = {};
  return PdfStatus::success();
}

PdfStatus PdfSecurity::makeObjectKey(const PdfObjectReference reference, uint8_t* const key,
                                     uint8_t* const keyLength) const {
  if (!enabled_ || key == nullptr || keyLength == nullptr || reference.objectNumber == 0 || keyLength_ != 16) {
    return PdfStatus::failure(PdfError::Encrypted, reference.objectNumber);
  }
  uint8_t input[21]{};
  std::memcpy(input, fileKey_, keyLength_);
  input[16] = static_cast<uint8_t>(reference.objectNumber);
  input[17] = static_cast<uint8_t>(reference.objectNumber >> 8U);
  input[18] = static_cast<uint8_t>(reference.objectNumber >> 16U);
  input[19] = static_cast<uint8_t>(reference.generation);
  input[20] = static_cast<uint8_t>(reference.generation >> 8U);
  uint8_t digest[16]{};
  md5(input, sizeof(input), digest);
  *keyLength = static_cast<uint8_t>(std::min<uint8_t>(keyLength_ + 5U, sizeof(digest)));
  std::memcpy(key, digest, *keyLength);
  return PdfStatus::success();
}

void PdfSecurity::initializeRc4(const uint8_t* const key, const uint8_t keyLength) {
  stream_.x = 0;
  stream_.y = 0;
  stream_.position = 0;
  for (uint16_t index = 0; index < 256; ++index) {
    stream_.permutation[index] = static_cast<uint8_t>(index);
  }
  uint8_t y = 0;
  for (uint16_t index = 0; index < 256; ++index) {
    y = static_cast<uint8_t>(y + stream_.permutation[index] + key[index % keyLength]);
    std::swap(stream_.permutation[index], stream_.permutation[y]);
  }
}

uint8_t PdfSecurity::nextRc4Byte() {
  stream_.x = static_cast<uint8_t>(stream_.x + 1U);
  stream_.y = static_cast<uint8_t>(stream_.y + stream_.permutation[stream_.x]);
  std::swap(stream_.permutation[stream_.x], stream_.permutation[stream_.y]);
  ++stream_.position;
  return stream_.permutation[static_cast<uint8_t>(stream_.permutation[stream_.x] + stream_.permutation[stream_.y])];
}

PdfStatus PdfSecurity::crypt(const uint8_t* const key, const uint8_t keyLength, uint8_t* const bytes,
                             const size_t length) {
  if (key == nullptr || keyLength == 0 || bytes == nullptr) {
    return PdfStatus::failure(PdfError::InvalidArgument);
  }
  initializeRc4(key, keyLength);
  for (size_t index = 0; index < length; ++index) {
    bytes[index] ^= nextRc4Byte();
  }
  return PdfStatus::success();
}

PdfStatus PdfSecurity::decryptStrings(PdfObjectArena& arena, const PdfObjectReference reference) {
  if (!enabled_) {
    return PdfStatus::success();
  }
  if (reference == encryptionReference_) {
    return PdfStatus::success();
  }
  uint8_t objectKey[16]{};
  uint8_t objectKeyLength = 0;
  PdfStatus status = makeObjectKey(reference, objectKey, &objectKeyLength);
  for (uint16_t index = 0; status && index < arena.valueCount; ++index) {
    const PdfValue& value = arena.values[index];
    if (value.kind != PdfValueKind::String) {
      continue;
    }
    if (static_cast<uint32_t>(value.textOffset) + value.textLength > arena.textLength) {
      return PdfStatus::failure(PdfError::Malformed, reference.objectNumber);
    }
    status = crypt(objectKey, objectKeyLength, arena.text + value.textOffset, value.textLength);
  }
  stream_ = {};
  return status;
}

PdfStatus PdfSecurity::openStream(const PdfByteSource& encoded, const PdfObjectReference reference,
                                  PdfByteSource* const decrypted) {
  if (!enabled_ || !encoded.valid() || decrypted == nullptr || reference == encryptionReference_) {
    return PdfStatus::failure(PdfError::Encrypted, reference.objectNumber);
  }
  stream_ = {};
  stream_.source = encoded;
  stream_.reference = reference;
  PdfStatus status = makeObjectKey(reference, stream_.objectKey, &stream_.keyLength);
  if (!status) {
    stream_ = {};
    return status;
  }
  initializeRc4(stream_.objectKey, stream_.keyLength);
  *decrypted = {this, encoded.size, readStream};
  return PdfStatus::success();
}

PdfStatus PdfSecurity::seekRc4(const uint64_t offset) {
  if (offset < stream_.position) {
    initializeRc4(stream_.objectKey, stream_.keyLength);
  }
  while (stream_.position < offset) {
    (void)nextRc4Byte();
  }
  return PdfStatus::success();
}

PdfStatus PdfSecurity::readStream(void* const context, const uint64_t offset, uint8_t* const destination,
                                  const size_t requested, size_t* const bytesRead) {
  if (context == nullptr || destination == nullptr || bytesRead == nullptr) {
    return PdfStatus::failure(PdfError::InvalidArgument, offset);
  }
  auto& security = *static_cast<PdfSecurity*>(context);
  if (!security.enabled_ || !security.stream_.source.valid() || offset > security.stream_.source.size ||
      requested > security.stream_.source.size - offset) {
    return PdfStatus::failure(PdfError::InvalidOffset, offset);
  }
  size_t read = 0;
  const PdfStatus status = security.stream_.source.readAt(security.stream_.source.context, offset, destination,
                                                          requested, &read);
  if (!status || read > requested) {
    return status ? PdfStatus::failure(PdfError::IoFailure, offset) : status;
  }
  security.seekRc4(offset);
  for (size_t index = 0; index < read; ++index) {
    destination[index] ^= security.nextRc4Byte();
  }
  *bytesRead = read;
  return PdfStatus::success();
}
