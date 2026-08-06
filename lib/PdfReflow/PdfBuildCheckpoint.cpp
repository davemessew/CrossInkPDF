#include "PdfBuildCheckpoint.h"

#include <cstring>

namespace {

constexpr uint8_t kCheckpointMagic[] = {'P', 'R', 'C', 'P'};
constexpr size_t kCheckpointBytes = 96;
constexpr size_t kCheckpointCrcOffset = kCheckpointBytes - sizeof(uint32_t);

class FixedEncoder {
 public:
  PdfStatus bytes(const void* source, const size_t length) {
    if (source == nullptr || length > sizeof(buffer_) - offset_) {
      return PdfStatus::failure(PdfError::LimitExceeded, offset_);
    }
    std::memcpy(buffer_ + offset_, source, length);
    offset_ += length;
    return PdfStatus::success();
  }

  PdfStatus u8(const uint8_t value) { return bytes(&value, sizeof(value)); }

  PdfStatus u16(const uint16_t value) {
    const uint8_t encoded[2] = {static_cast<uint8_t>(value), static_cast<uint8_t>(value >> 8U)};
    return bytes(encoded, sizeof(encoded));
  }

  PdfStatus u24(const uint32_t value) {
    if (value > 0x00ffffffU) {
      return PdfStatus::failure(PdfError::LimitExceeded, value);
    }
    const uint8_t encoded[3] = {
        static_cast<uint8_t>(value),
        static_cast<uint8_t>(value >> 8U),
        static_cast<uint8_t>(value >> 16U),
    };
    return bytes(encoded, sizeof(encoded));
  }

  PdfStatus u32(const uint32_t value) {
    const uint8_t encoded[4] = {
        static_cast<uint8_t>(value),
        static_cast<uint8_t>(value >> 8U),
        static_cast<uint8_t>(value >> 16U),
        static_cast<uint8_t>(value >> 24U),
    };
    return bytes(encoded, sizeof(encoded));
  }

  PdfStatus u64(const uint64_t value) {
    uint8_t encoded[8];
    for (uint8_t index = 0; index < sizeof(encoded); ++index) {
      encoded[index] = static_cast<uint8_t>(value >> (index * 8U));
    }
    return bytes(encoded, sizeof(encoded));
  }

  const uint8_t* data() const { return buffer_; }
  size_t size() const { return offset_; }

 private:
  uint8_t buffer_[kCheckpointBytes]{};
  size_t offset_ = 0;
};

class FixedDecoder {
 public:
  explicit FixedDecoder(const uint8_t* data) : data_(data) {}

  PdfStatus bytes(void* destination, const size_t length) {
    if (destination == nullptr || length > kCheckpointBytes - offset_) {
      return PdfStatus::failure(PdfError::UnexpectedEof, offset_);
    }
    std::memcpy(destination, data_ + offset_, length);
    offset_ += length;
    return PdfStatus::success();
  }

  PdfStatus u8(uint8_t* value) { return bytes(value, sizeof(*value)); }

  PdfStatus u16(uint16_t* value) {
    uint8_t encoded[2];
    const PdfStatus status = bytes(encoded, sizeof(encoded));
    if (status) {
      *value = static_cast<uint16_t>(encoded[0]) | static_cast<uint16_t>(encoded[1]) << 8U;
    }
    return status;
  }

  PdfStatus u32(uint32_t* value) {
    uint8_t encoded[4];
    const PdfStatus status = bytes(encoded, sizeof(encoded));
    if (status) {
      *value = static_cast<uint32_t>(encoded[0]) | static_cast<uint32_t>(encoded[1]) << 8U |
               static_cast<uint32_t>(encoded[2]) << 16U | static_cast<uint32_t>(encoded[3]) << 24U;
    }
    return status;
  }

  PdfStatus u24(uint32_t* value) {
    uint8_t encoded[3];
    const PdfStatus status = bytes(encoded, sizeof(encoded));
    if (status) {
      *value = static_cast<uint32_t>(encoded[0]) | static_cast<uint32_t>(encoded[1]) << 8U |
               static_cast<uint32_t>(encoded[2]) << 16U;
    }
    return status;
  }

  PdfStatus u64(uint64_t* value) {
    uint8_t encoded[8];
    const PdfStatus status = bytes(encoded, sizeof(encoded));
    if (status) {
      *value = 0;
      for (uint8_t index = 0; index < sizeof(encoded); ++index) {
        *value |= static_cast<uint64_t>(encoded[index]) << (index * 8U);
      }
    }
    return status;
  }

 private:
  const uint8_t* data_ = nullptr;
  size_t offset_ = 0;
};

#define PDF_CACHE_RETURN_IF_ERROR(expression) \
  do {                                        \
    const PdfStatus status = (expression);    \
    if (!status) return status;               \
  } while (false)

bool validResumePhase(const PdfBuildCheckpoint& checkpoint) {
  if (checkpoint.resumePhase == PdfBuildResumePhase::None) {
    return checkpoint.journalBytes == 0;
  }
  if (checkpoint.generation == 0 || checkpoint.lastVerifiedPage == 0 || checkpoint.lastVerifiedObject == 0 ||
      checkpoint.emittedSections == 0 || checkpoint.outputBytes == 0 || checkpoint.journalBytes > 0x00ffffffU) {
    return false;
  }
  switch (checkpoint.resumePhase) {
    case PdfBuildResumePhase::CommitManifest:
    case PdfBuildResumePhase::AfterEmitSections:
      return checkpoint.phase == PdfBuildPhase::Cancelled;
    case PdfBuildResumePhase::AfterPage:
      return checkpoint.journalBytes != 0 &&
             (checkpoint.phase == PdfBuildPhase::ParsePages || checkpoint.phase == PdfBuildPhase::Cancelled);
    case PdfBuildResumePhase::AfterImage:
      return checkpoint.journalBytes != 0 &&
             (checkpoint.phase == PdfBuildPhase::EmitImages || checkpoint.phase == PdfBuildPhase::Cancelled);
    case PdfBuildResumePhase::AfterImageRepair:
      return checkpoint.journalBytes != 0 &&
             (checkpoint.phase == PdfBuildPhase::Finalize || checkpoint.phase == PdfBuildPhase::Cancelled);
    case PdfBuildResumePhase::None:
    default:
      return false;
  }
}

}  // namespace

PdfStatus pdfEncodeBuildCheckpoint(const PdfBuildCheckpoint& checkpoint, const PdfByteSink& destination) {
  if (!destination.valid() || checkpoint.phase > PdfBuildPhase::Cancelled ||
      checkpoint.resumePhase > PdfBuildResumePhase::AfterImageRepair || !validResumePhase(checkpoint)) {
    return PdfStatus::failure(PdfError::InvalidArgument);
  }
  FixedEncoder encoder;
  PDF_CACHE_RETURN_IF_ERROR(encoder.bytes(kCheckpointMagic, sizeof(kCheckpointMagic)));
  PDF_CACHE_RETURN_IF_ERROR(encoder.u16(PDF_BUILD_CHECKPOINT_CODEC_VERSION));
  PDF_CACHE_RETURN_IF_ERROR(encoder.u16(0));
  PDF_CACHE_RETURN_IF_ERROR(encoder.u32(checkpoint.sequence));
  PDF_CACHE_RETURN_IF_ERROR(encoder.u64(checkpoint.source.size));
  PDF_CACHE_RETURN_IF_ERROR(encoder.u8(checkpoint.source.modificationTime.known ? 1 : 0));
  PDF_CACHE_RETURN_IF_ERROR(encoder.u24(checkpoint.journalBytes));
  PDF_CACHE_RETURN_IF_ERROR(encoder.u64(checkpoint.source.modificationTime.value));
  PDF_CACHE_RETURN_IF_ERROR(encoder.u64(checkpoint.source.headFingerprint));
  PDF_CACHE_RETURN_IF_ERROR(encoder.u64(checkpoint.source.tailFingerprint));
  PDF_CACHE_RETURN_IF_ERROR(encoder.u32(checkpoint.generation));
  PDF_CACHE_RETURN_IF_ERROR(encoder.u8(static_cast<uint8_t>(checkpoint.phase)));
  PDF_CACHE_RETURN_IF_ERROR(encoder.u8(static_cast<uint8_t>(checkpoint.resumePhase)));
  PDF_CACHE_RETURN_IF_ERROR(encoder.u16(0));
  PDF_CACHE_RETURN_IF_ERROR(encoder.u32(checkpoint.lastVerifiedPage));
  PDF_CACHE_RETURN_IF_ERROR(encoder.u32(checkpoint.lastVerifiedObject));
  PDF_CACHE_RETURN_IF_ERROR(encoder.u32(checkpoint.emittedSections));
  PDF_CACHE_RETURN_IF_ERROR(encoder.u32(checkpoint.emittedImages));
  PDF_CACHE_RETURN_IF_ERROR(encoder.u32(checkpoint.cumulativeWords));
  PDF_CACHE_RETURN_IF_ERROR(encoder.u64(checkpoint.outputBytes));
  PDF_CACHE_RETURN_IF_ERROR(encoder.u32(checkpoint.warningFlags));
  PDF_CACHE_RETURN_IF_ERROR(encoder.u32(kCheckpointBytes));
  if (encoder.size() != kCheckpointCrcOffset) {
    return PdfStatus::failure(PdfError::Malformed);
  }
  PDF_CACHE_RETURN_IF_ERROR(encoder.u32(pdfCacheCrc32(encoder.data(), encoder.size())));
  if (encoder.size() != kCheckpointBytes) {
    return PdfStatus::failure(PdfError::Malformed);
  }
  return pdfWriteExact(destination, encoder.data(), encoder.size());
}

PdfStatus pdfDecodeBuildCheckpoint(const PdfByteSource& source, PdfBuildCheckpoint* const checkpoint) {
  if (!source.valid() || checkpoint == nullptr || source.size != kCheckpointBytes) {
    return PdfStatus::failure(PdfError::InvalidArgument);
  }
  uint8_t buffer[kCheckpointBytes];
  const PdfStatus readStatus = pdfReadExact(source, 0, buffer, sizeof(buffer));
  if (!readStatus) {
    return readStatus;
  }
  const uint32_t calculatedCrc = pdfCacheCrc32(buffer, kCheckpointCrcOffset);
  FixedDecoder decoder(buffer);
  uint8_t magic[sizeof(kCheckpointMagic)];
  uint16_t codecVersion = 0;
  uint16_t reserved16 = 0;
  uint8_t modificationTimeKnown = 0;
  uint8_t phase = 0;
  uint8_t resumePhase = 0;
  uint16_t phaseReserved16 = 0;
  uint32_t storedLength = 0;
  uint32_t storedCrc = 0;
  *checkpoint = {};
  PDF_CACHE_RETURN_IF_ERROR(decoder.bytes(magic, sizeof(magic)));
  PDF_CACHE_RETURN_IF_ERROR(decoder.u16(&codecVersion));
  PDF_CACHE_RETURN_IF_ERROR(decoder.u16(&reserved16));
  PDF_CACHE_RETURN_IF_ERROR(decoder.u32(&checkpoint->sequence));
  PDF_CACHE_RETURN_IF_ERROR(decoder.u64(&checkpoint->source.size));
  PDF_CACHE_RETURN_IF_ERROR(decoder.u8(&modificationTimeKnown));
  PDF_CACHE_RETURN_IF_ERROR(decoder.u24(&checkpoint->journalBytes));
  PDF_CACHE_RETURN_IF_ERROR(decoder.u64(&checkpoint->source.modificationTime.value));
  PDF_CACHE_RETURN_IF_ERROR(decoder.u64(&checkpoint->source.headFingerprint));
  PDF_CACHE_RETURN_IF_ERROR(decoder.u64(&checkpoint->source.tailFingerprint));
  PDF_CACHE_RETURN_IF_ERROR(decoder.u32(&checkpoint->generation));
  PDF_CACHE_RETURN_IF_ERROR(decoder.u8(&phase));
  PDF_CACHE_RETURN_IF_ERROR(decoder.u8(&resumePhase));
  PDF_CACHE_RETURN_IF_ERROR(decoder.u16(&phaseReserved16));
  PDF_CACHE_RETURN_IF_ERROR(decoder.u32(&checkpoint->lastVerifiedPage));
  PDF_CACHE_RETURN_IF_ERROR(decoder.u32(&checkpoint->lastVerifiedObject));
  PDF_CACHE_RETURN_IF_ERROR(decoder.u32(&checkpoint->emittedSections));
  PDF_CACHE_RETURN_IF_ERROR(decoder.u32(&checkpoint->emittedImages));
  PDF_CACHE_RETURN_IF_ERROR(decoder.u32(&checkpoint->cumulativeWords));
  PDF_CACHE_RETURN_IF_ERROR(decoder.u64(&checkpoint->outputBytes));
  PDF_CACHE_RETURN_IF_ERROR(decoder.u32(&checkpoint->warningFlags));
  PDF_CACHE_RETURN_IF_ERROR(decoder.u32(&storedLength));
  PDF_CACHE_RETURN_IF_ERROR(decoder.u32(&storedCrc));
  if (std::memcmp(magic, kCheckpointMagic, sizeof(magic)) != 0 || codecVersion != PDF_BUILD_CHECKPOINT_CODEC_VERSION ||
      reserved16 != 0 || modificationTimeKnown > 1 || phase > static_cast<uint8_t>(PdfBuildPhase::Cancelled) ||
      resumePhase > static_cast<uint8_t>(PdfBuildResumePhase::AfterImageRepair) || phaseReserved16 != 0 ||
      storedLength != kCheckpointBytes || storedCrc != calculatedCrc) {
    return PdfStatus::failure(PdfError::Malformed);
  }
  checkpoint->source.modificationTime.known = modificationTimeKnown != 0;
  checkpoint->phase = static_cast<PdfBuildPhase>(phase);
  checkpoint->resumePhase = static_cast<PdfBuildResumePhase>(resumePhase);
  if (!validResumePhase(*checkpoint)) {
    return PdfStatus::failure(PdfError::Malformed);
  }
  return PdfStatus::success();
}

bool pdfCheckpointDue(const PdfCheckpointGate& gate, const uint32_t completedPages, const uint64_t outputBytes,
                      const uint32_t nowMs, const bool forced) {
  if (forced) {
    return true;
  }
  const uint32_t elapsedMs = nowMs - gate.committedAtMs;
  if (elapsedMs < PDF_CACHE_CHECKPOINT_TIME_INTERVAL_MS || completedPages < gate.completedPages ||
      outputBytes < gate.outputBytes) {
    return false;
  }
  return completedPages - gate.completedPages >= PDF_CACHE_CHECKPOINT_PAGE_INTERVAL ||
         outputBytes - gate.outputBytes >= PDF_CACHE_CHECKPOINT_BYTE_INTERVAL;
}

void pdfCheckpointCommitted(PdfCheckpointGate* const gate, const uint32_t completedPages, const uint64_t outputBytes,
                            const uint32_t nowMs) {
  if (gate != nullptr) {
    *gate = {completedPages, outputBytes, nowMs};
  }
}
