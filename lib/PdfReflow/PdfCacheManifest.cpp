#include "PdfCacheManifest.h"

#include <algorithm>
#include <cstring>

#include "PdfCheckedMath.h"

namespace {

constexpr uint8_t kManifestMagic[] = {'P', 'R', 'M', 'F'};
constexpr size_t kCodecBufferBytes = 64;
constexpr uint64_t kFixedManifestBytes = 92;
constexpr uint64_t kRecordHeaderBytes = 16;

class Encoder {
 public:
  explicit Encoder(const PdfByteSink& sink) : sink_(sink) {}

  PdfStatus put(const void* data, size_t length, const bool includeInCrc = true) {
    if (data == nullptr && length != 0) {
      return PdfStatus::failure(PdfError::InvalidArgument);
    }
    const auto* source = static_cast<const uint8_t*>(data);
    if (includeInCrc) {
      crc_ = pdfCacheCrc32(source, length, crc_);
    }
    while (length != 0) {
      const size_t count = std::min(length, sizeof(buffer_) - buffered_);
      std::memcpy(buffer_ + buffered_, source, count);
      buffered_ += count;
      source += count;
      length -= count;
      if (buffered_ == sizeof(buffer_)) {
        const PdfStatus status = flush();
        if (!status) {
          return status;
        }
      }
    }
    return PdfStatus::success();
  }

  PdfStatus u8(const uint8_t value, const bool includeInCrc = true) { return put(&value, sizeof(value), includeInCrc); }

  PdfStatus u16(const uint16_t value, const bool includeInCrc = true) {
    const uint8_t bytes[2] = {static_cast<uint8_t>(value), static_cast<uint8_t>(value >> 8U)};
    return put(bytes, sizeof(bytes), includeInCrc);
  }

  PdfStatus u32(const uint32_t value, const bool includeInCrc = true) {
    const uint8_t bytes[4] = {
        static_cast<uint8_t>(value),
        static_cast<uint8_t>(value >> 8U),
        static_cast<uint8_t>(value >> 16U),
        static_cast<uint8_t>(value >> 24U),
    };
    return put(bytes, sizeof(bytes), includeInCrc);
  }

  PdfStatus u64(const uint64_t value, const bool includeInCrc = true) {
    uint8_t bytes[8];
    for (uint8_t index = 0; index < sizeof(bytes); ++index) {
      bytes[index] = static_cast<uint8_t>(value >> (index * 8U));
    }
    return put(bytes, sizeof(bytes), includeInCrc);
  }

  PdfStatus flush() {
    if (buffered_ == 0) {
      return PdfStatus::success();
    }
    const PdfStatus status = pdfWriteExact(sink_, buffer_, buffered_);
    if (status) {
      buffered_ = 0;
    }
    return status;
  }

  uint32_t crc() const { return crc_; }

 private:
  PdfByteSink sink_{};
  uint8_t buffer_[kCodecBufferBytes]{};
  size_t buffered_ = 0;
  uint32_t crc_ = 0;
};

class Decoder {
 public:
  explicit Decoder(const PdfByteSource& source) : source_(source) {}

  PdfStatus get(void* destination, size_t length, const bool includeInCrc = true) {
    if (destination == nullptr && length != 0) {
      return PdfStatus::failure(PdfError::InvalidArgument, consumed_);
    }
    auto* output = static_cast<uint8_t*>(destination);
    while (length != 0) {
      if (available_ == 0) {
        const PdfStatus status = fill();
        if (!status) {
          return status;
        }
      }
      const size_t count = std::min(length, available_);
      std::memcpy(output, buffer_ + cursor_, count);
      if (includeInCrc) {
        crc_ = pdfCacheCrc32(buffer_ + cursor_, count, crc_);
      }
      cursor_ += count;
      available_ -= count;
      consumed_ += count;
      output += count;
      length -= count;
    }
    return PdfStatus::success();
  }

  PdfStatus u8(uint8_t* value, const bool includeInCrc = true) { return get(value, sizeof(*value), includeInCrc); }

  PdfStatus u16(uint16_t* value, const bool includeInCrc = true) {
    uint8_t bytes[2];
    const PdfStatus status = get(bytes, sizeof(bytes), includeInCrc);
    if (status) {
      *value = static_cast<uint16_t>(bytes[0]) | static_cast<uint16_t>(bytes[1]) << 8U;
    }
    return status;
  }

  PdfStatus u32(uint32_t* value, const bool includeInCrc = true) {
    uint8_t bytes[4];
    const PdfStatus status = get(bytes, sizeof(bytes), includeInCrc);
    if (status) {
      *value = static_cast<uint32_t>(bytes[0]) | static_cast<uint32_t>(bytes[1]) << 8U |
               static_cast<uint32_t>(bytes[2]) << 16U | static_cast<uint32_t>(bytes[3]) << 24U;
    }
    return status;
  }

  PdfStatus u64(uint64_t* value, const bool includeInCrc = true) {
    uint8_t bytes[8];
    const PdfStatus status = get(bytes, sizeof(bytes), includeInCrc);
    if (status) {
      *value = 0;
      for (uint8_t index = 0; index < sizeof(bytes); ++index) {
        *value |= static_cast<uint64_t>(bytes[index]) << (index * 8U);
      }
    }
    return status;
  }

  uint32_t crc() const { return crc_; }
  uint64_t consumed() const { return consumed_; }

 private:
  PdfStatus fill() {
    if (sourceOffset_ >= source_.size) {
      return PdfStatus::failure(PdfError::UnexpectedEof, sourceOffset_);
    }
    const size_t requested = static_cast<size_t>(std::min<uint64_t>(sizeof(buffer_), source_.size - sourceOffset_));
    size_t bytesRead = 0;
    const PdfStatus status = source_.readAt(source_.context, sourceOffset_, buffer_, requested, &bytesRead);
    if (!status) {
      return status;
    }
    if (bytesRead == 0 || bytesRead > requested) {
      return PdfStatus::failure(bytesRead > requested ? PdfError::Malformed : PdfError::UnexpectedEof, sourceOffset_);
    }
    sourceOffset_ += bytesRead;
    cursor_ = 0;
    available_ = bytesRead;
    return PdfStatus::success();
  }

  PdfByteSource source_{};
  uint8_t buffer_[kCodecBufferBytes]{};
  size_t cursor_ = 0;
  size_t available_ = 0;
  uint64_t sourceOffset_ = 0;
  uint64_t consumed_ = 0;
  uint32_t crc_ = 0;
};

#define PDF_CACHE_RETURN_IF_ERROR(expression) \
  do {                                        \
    const PdfStatus status = (expression);    \
    if (!status) return status;               \
  } while (false)

PdfStatus validateRecord(const PdfRequiredFileRecord& record) {
  if (record.pathLength == 0 || record.pathLength >= sizeof(record.path) || record.path[record.pathLength] != '\0' ||
      !pdfValidateCacheRelativePath(record.path, record.pathLength)) {
    return PdfStatus::failure(PdfError::Malformed);
  }
  return PdfStatus::success();
}

}  // namespace

bool pdfValidateCacheRelativePath(const char* const path, const size_t length) {
  if (path == nullptr || length == 0 || length >= PDF_CACHE_REQUIRED_PATH_CAPACITY || path[0] == '/' ||
      path[0] == '\\' || path[length - 1] == '/' || path[length - 1] == '\\') {
    return false;
  }
  size_t componentStart = 0;
  for (size_t index = 0; index <= length; ++index) {
    if (index != length && path[index] == '\\') {
      return false;
    }
    const bool boundary = index == length || path[index] == '/';
    if (!boundary) {
      if (path[index] == '\0' || path[index] == ':') {
        return false;
      }
      continue;
    }
    const size_t componentLength = index - componentStart;
    if (componentLength == 0 || (componentLength == 1 && path[componentStart] == '.') ||
        (componentLength == 2 && path[componentStart] == '.' && path[componentStart + 1] == '.')) {
      return false;
    }
    componentStart = index + 1;
  }
  return true;
}

uint64_t pdfUpdateRequiredFileLedger(uint64_t ledger, const PdfRequiredFileRecord& record) {
  ledger = pdfCacheFnv64(&record.pathLength, sizeof(record.pathLength), ledger);
  ledger = pdfCacheFnv64(record.path, record.pathLength, ledger);
  uint8_t sizeBytes[8];
  for (uint8_t index = 0; index < sizeof(sizeBytes); ++index) {
    sizeBytes[index] = static_cast<uint8_t>(record.size >> (index * 8U));
  }
  ledger = pdfCacheFnv64(sizeBytes, sizeof(sizeBytes), ledger);
  const uint8_t crcBytes[4] = {
      static_cast<uint8_t>(record.crc32),
      static_cast<uint8_t>(record.crc32 >> 8U),
      static_cast<uint8_t>(record.crc32 >> 16U),
      static_cast<uint8_t>(record.crc32 >> 24U),
  };
  return pdfCacheFnv64(crcBytes, sizeof(crcBytes), ledger);
}

PdfStatus pdfEncodeCacheManifest(const PdfCacheManifest& manifest, const PdfRequiredFileTableSource& files,
                                 const PdfByteSink& destination) {
  if (!destination.valid() || !files.valid() || files.count != manifest.requiredFileCount ||
      files.count > PDF_CACHE_MAX_REQUIRED_FILES || manifest.formatVersion != PDF_CACHE_FORMAT_VERSION ||
      manifest.capabilityVersion != PDF_CACHE_CAPABILITY_VERSION) {
    return PdfStatus::failure(PdfError::InvalidArgument);
  }

  uint64_t totalLength = kFixedManifestBytes;
  uint64_t totalFileBytes = 0;
  uint64_t ledger = PDF_CACHE_FNV64_OFFSET;
  for (uint32_t index = 0; index < files.count; ++index) {
    PdfRequiredFileRecord record{};
    PDF_CACHE_RETURN_IF_ERROR(files.read(files.context, index, &record));
    PDF_CACHE_RETURN_IF_ERROR(validateRecord(record));
    if (!pdfCheckedAdd(totalLength, kRecordHeaderBytes + record.pathLength, &totalLength) ||
        !pdfCheckedAdd(totalFileBytes, record.size, &totalFileBytes) || totalLength > PDF_CACHE_MAX_SLOT_BYTES) {
      return PdfStatus::failure(PdfError::LimitExceeded);
    }
    ledger = pdfUpdateRequiredFileLedger(ledger, record);
  }
  if (totalFileBytes != manifest.requiredFileBytes || ledger != manifest.requiredFileLedger ||
      totalLength > UINT32_MAX) {
    return PdfStatus::failure(PdfError::Malformed);
  }

  Encoder encoder(destination);
  PDF_CACHE_RETURN_IF_ERROR(encoder.put(kManifestMagic, sizeof(kManifestMagic)));
  PDF_CACHE_RETURN_IF_ERROR(encoder.u16(PDF_CACHE_CODEC_VERSION));
  PDF_CACHE_RETURN_IF_ERROR(encoder.u16(manifest.formatVersion));
  PDF_CACHE_RETURN_IF_ERROR(encoder.u16(manifest.capabilityVersion));
  PDF_CACHE_RETURN_IF_ERROR(encoder.u16(0));
  PDF_CACHE_RETURN_IF_ERROR(encoder.u32(manifest.sequence));
  PDF_CACHE_RETURN_IF_ERROR(encoder.u8(manifest.completed ? 1 : 0));
  PDF_CACHE_RETURN_IF_ERROR(encoder.u8(manifest.source.modificationTime.known ? 1 : 0));
  PDF_CACHE_RETURN_IF_ERROR(encoder.u16(0));
  PDF_CACHE_RETURN_IF_ERROR(encoder.u32(manifest.warningFlags));
  PDF_CACHE_RETURN_IF_ERROR(encoder.u64(manifest.source.size));
  PDF_CACHE_RETURN_IF_ERROR(encoder.u64(manifest.source.modificationTime.value));
  PDF_CACHE_RETURN_IF_ERROR(encoder.u64(manifest.source.headFingerprint));
  PDF_CACHE_RETURN_IF_ERROR(encoder.u64(manifest.source.tailFingerprint));
  PDF_CACHE_RETURN_IF_ERROR(encoder.u32(manifest.generation));
  PDF_CACHE_RETURN_IF_ERROR(encoder.u32(manifest.totalWords));
  PDF_CACHE_RETURN_IF_ERROR(encoder.u32(manifest.requiredFileCount));
  PDF_CACHE_RETURN_IF_ERROR(encoder.u64(manifest.requiredFileBytes));
  PDF_CACHE_RETURN_IF_ERROR(encoder.u64(manifest.requiredFileLedger));
  for (uint32_t index = 0; index < files.count; ++index) {
    PdfRequiredFileRecord record{};
    PDF_CACHE_RETURN_IF_ERROR(files.read(files.context, index, &record));
    PDF_CACHE_RETURN_IF_ERROR(encoder.u8(record.pathLength));
    PDF_CACHE_RETURN_IF_ERROR(encoder.u8(0));
    PDF_CACHE_RETURN_IF_ERROR(encoder.u16(0));
    PDF_CACHE_RETURN_IF_ERROR(encoder.u64(record.size));
    PDF_CACHE_RETURN_IF_ERROR(encoder.u32(record.crc32));
    PDF_CACHE_RETURN_IF_ERROR(encoder.put(record.path, record.pathLength));
  }
  PDF_CACHE_RETURN_IF_ERROR(encoder.u32(static_cast<uint32_t>(totalLength)));
  const uint32_t crc = encoder.crc();
  PDF_CACHE_RETURN_IF_ERROR(encoder.u32(crc, false));
  return encoder.flush();
}

PdfStatus pdfDecodeCacheManifest(const PdfByteSource& source, PdfCacheManifest* const manifest,
                                 const PdfRequiredFileTableVisitor& visitor) {
  if (!source.valid() || manifest == nullptr || source.size < kFixedManifestBytes ||
      source.size > PDF_CACHE_MAX_SLOT_BYTES) {
    return PdfStatus::failure(PdfError::InvalidArgument);
  }
  *manifest = {};
  Decoder decoder(source);
  uint8_t magic[sizeof(kManifestMagic)];
  uint16_t codecVersion = 0;
  uint16_t reserved16 = 0;
  uint8_t completed = 0;
  uint8_t modificationTimeKnown = 0;
  PDF_CACHE_RETURN_IF_ERROR(decoder.get(magic, sizeof(magic)));
  PDF_CACHE_RETURN_IF_ERROR(decoder.u16(&codecVersion));
  PDF_CACHE_RETURN_IF_ERROR(decoder.u16(&manifest->formatVersion));
  PDF_CACHE_RETURN_IF_ERROR(decoder.u16(&manifest->capabilityVersion));
  PDF_CACHE_RETURN_IF_ERROR(decoder.u16(&reserved16));
  PDF_CACHE_RETURN_IF_ERROR(decoder.u32(&manifest->sequence));
  PDF_CACHE_RETURN_IF_ERROR(decoder.u8(&completed));
  PDF_CACHE_RETURN_IF_ERROR(decoder.u8(&modificationTimeKnown));
  uint16_t reservedFlags = 0;
  PDF_CACHE_RETURN_IF_ERROR(decoder.u16(&reservedFlags));
  PDF_CACHE_RETURN_IF_ERROR(decoder.u32(&manifest->warningFlags));
  PDF_CACHE_RETURN_IF_ERROR(decoder.u64(&manifest->source.size));
  PDF_CACHE_RETURN_IF_ERROR(decoder.u64(&manifest->source.modificationTime.value));
  PDF_CACHE_RETURN_IF_ERROR(decoder.u64(&manifest->source.headFingerprint));
  PDF_CACHE_RETURN_IF_ERROR(decoder.u64(&manifest->source.tailFingerprint));
  PDF_CACHE_RETURN_IF_ERROR(decoder.u32(&manifest->generation));
  PDF_CACHE_RETURN_IF_ERROR(decoder.u32(&manifest->totalWords));
  PDF_CACHE_RETURN_IF_ERROR(decoder.u32(&manifest->requiredFileCount));
  PDF_CACHE_RETURN_IF_ERROR(decoder.u64(&manifest->requiredFileBytes));
  PDF_CACHE_RETURN_IF_ERROR(decoder.u64(&manifest->requiredFileLedger));
  if (std::memcmp(magic, kManifestMagic, sizeof(magic)) != 0 || codecVersion != PDF_CACHE_CODEC_VERSION ||
      manifest->formatVersion != PDF_CACHE_FORMAT_VERSION ||
      manifest->capabilityVersion != PDF_CACHE_CAPABILITY_VERSION || reserved16 != 0 || reservedFlags != 0 ||
      completed > 1 || modificationTimeKnown > 1 || manifest->requiredFileCount > PDF_CACHE_MAX_REQUIRED_FILES) {
    return PdfStatus::failure(PdfError::Malformed);
  }
  manifest->completed = completed != 0;
  manifest->source.modificationTime.known = modificationTimeKnown != 0;

  uint64_t decodedFileBytes = 0;
  uint64_t decodedLedger = PDF_CACHE_FNV64_OFFSET;
  for (uint32_t index = 0; index < manifest->requiredFileCount; ++index) {
    PdfRequiredFileRecord record{};
    uint8_t reserved8 = 0;
    uint16_t recordReserved16 = 0;
    PDF_CACHE_RETURN_IF_ERROR(decoder.u8(&record.pathLength));
    PDF_CACHE_RETURN_IF_ERROR(decoder.u8(&reserved8));
    PDF_CACHE_RETURN_IF_ERROR(decoder.u16(&recordReserved16));
    PDF_CACHE_RETURN_IF_ERROR(decoder.u64(&record.size));
    PDF_CACHE_RETURN_IF_ERROR(decoder.u32(&record.crc32));
    if (record.pathLength == 0 || record.pathLength >= sizeof(record.path) || reserved8 != 0 || recordReserved16 != 0) {
      return PdfStatus::failure(PdfError::Malformed);
    }
    PDF_CACHE_RETURN_IF_ERROR(decoder.get(record.path, record.pathLength));
    record.path[record.pathLength] = '\0';
    PDF_CACHE_RETURN_IF_ERROR(validateRecord(record));
    if (!pdfCheckedAdd(decodedFileBytes, record.size, &decodedFileBytes)) {
      return PdfStatus::failure(PdfError::LimitExceeded);
    }
    decodedLedger = pdfUpdateRequiredFileLedger(decodedLedger, record);
    if (visitor.valid()) {
      PDF_CACHE_RETURN_IF_ERROR(visitor.accept(visitor.context, record));
    }
  }

  uint32_t storedLength = 0;
  PDF_CACHE_RETURN_IF_ERROR(decoder.u32(&storedLength));
  const uint32_t calculatedCrc = decoder.crc();
  uint32_t storedCrc = 0;
  PDF_CACHE_RETURN_IF_ERROR(decoder.u32(&storedCrc, false));
  if (storedLength != source.size || decoder.consumed() != source.size || storedCrc != calculatedCrc ||
      decodedFileBytes != manifest->requiredFileBytes || decodedLedger != manifest->requiredFileLedger) {
    return PdfStatus::failure(PdfError::Malformed);
  }
  return PdfStatus::success();
}
