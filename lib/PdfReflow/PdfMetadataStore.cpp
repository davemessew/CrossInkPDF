#include "PdfMetadataStore.h"

#include <algorithm>
#include <cstring>
#include <limits>

#include "PdfCacheFormat.h"
#include "PdfIo.h"
#include "PdfUnicode.h"

namespace {

constexpr uint8_t kMagic[] = {'X', 'P', 'M', 'D'};
constexpr size_t kHeaderBytes = 24;
constexpr size_t kSectionBytes = 24;
constexpr size_t kCrcBytes = 4;

void putU16(uint8_t* output, const uint16_t value) {
  output[0] = static_cast<uint8_t>(value);
  output[1] = static_cast<uint8_t>(value >> 8U);
}

void putU32(uint8_t* output, const uint32_t value) {
  output[0] = static_cast<uint8_t>(value);
  output[1] = static_cast<uint8_t>(value >> 8U);
  output[2] = static_cast<uint8_t>(value >> 16U);
  output[3] = static_cast<uint8_t>(value >> 24U);
}

uint16_t getU16(const uint8_t* input) {
  return static_cast<uint16_t>(input[0]) | static_cast<uint16_t>(static_cast<uint16_t>(input[1]) << 8U);
}

uint32_t getU32(const uint8_t* input) {
  return static_cast<uint32_t>(input[0]) | (static_cast<uint32_t>(input[1]) << 8U) |
         (static_cast<uint32_t>(input[2]) << 16U) | (static_cast<uint32_t>(input[3]) << 24U);
}

class Encoder {
 public:
  explicit Encoder(const PdfByteSink destination) : destination_(destination) {}

  PdfStatus write(const uint8_t* bytes, size_t length) {
    while (length != 0) {
      const size_t count = std::min(length, PdfMetadataLimits::IoChunkBytes);
      const PdfStatus status = pdfWriteExact(destination_, bytes, count);
      if (!status) {
        return status;
      }
      crc_ = pdfCacheCrc32(bytes, count, crc_);
      bytes += count;
      length -= count;
    }
    return PdfStatus::success();
  }

  PdfStatus finish() {
    uint8_t encoded[kCrcBytes]{};
    putU32(encoded, crc_);
    return pdfWriteExact(destination_, encoded, sizeof(encoded));
  }

 private:
  PdfByteSink destination_{};
  uint32_t crc_ = 0;
};

class Decoder {
 public:
  explicit Decoder(const PdfByteSource source) : source_(source) {}

  PdfStatus read(uint8_t* bytes, size_t length) {
    while (length != 0) {
      const size_t count = std::min(length, PdfMetadataLimits::IoChunkBytes);
      const PdfStatus status = pdfReadExact(source_, offset_, bytes, count);
      if (!status) {
        return status;
      }
      crc_ = pdfCacheCrc32(bytes, count, crc_);
      offset_ += count;
      bytes += count;
      length -= count;
    }
    return PdfStatus::success();
  }

  PdfStatus finish() {
    uint8_t encoded[kCrcBytes]{};
    PdfStatus status = pdfReadExact(source_, offset_, encoded, sizeof(encoded));
    if (!status) {
      return status;
    }
    offset_ += sizeof(encoded);
    if (getU32(encoded) != crc_ || offset_ != source_.size) {
      return PdfStatus::failure(PdfError::Malformed, offset_);
    }
    return PdfStatus::success();
  }

 private:
  PdfByteSource source_{};
  uint64_t offset_ = 0;
  uint32_t crc_ = 0;
};

PdfStatus boundedUtf8Length(const uint8_t* value, const size_t length, const size_t maximum, size_t* outputStart,
                            size_t* outputLength) {
  if ((value == nullptr && length != 0) || outputStart == nullptr || outputLength == nullptr || maximum == 0) {
    return PdfStatus::failure(PdfError::InvalidArgument);
  }

  size_t start = 0;
  while (start < length &&
         (value[start] == ' ' || value[start] == '\t' || value[start] == '\r' || value[start] == '\n')) {
    ++start;
  }
  size_t end = length;
  while (end > start &&
         (value[end - 1] == ' ' || value[end - 1] == '\t' || value[end - 1] == '\r' || value[end - 1] == '\n')) {
    --end;
  }

  size_t offset = start;
  size_t acceptedEnd = start;
  while (offset < end) {
    uint32_t scalar = 0;
    const PdfStatus status = pdfDecodeUtf8Scalar(value, end, &offset, &scalar);
    if (!status) {
      return status;
    }
    (void)scalar;
    if (offset - start <= maximum) {
      acceptedEnd = offset;
    }
  }
  *outputStart = start;
  *outputLength = acceptedEnd - start;
  return PdfStatus::success();
}

bool validMetadata(const PdfMetadata& metadata, const PdfMetadataSectionSource& sections) {
  return sections.valid() && metadata.titleLength != 0 && metadata.titleLength < PdfMetadataLimits::TitleBytes &&
         metadata.authorLength < PdfMetadataLimits::AuthorBytes &&
         metadata.languageLength < PdfMetadataLimits::LanguageBytes && metadata.sectionCount == sections.count &&
         metadata.sectionCount != 0 && metadata.sectionCount <= PdfMetadataLimits::MaxSections &&
         metadata.outlineCount != 0 && metadata.outlineCount <= PdfMetadataLimits::MaxOutlineEntries &&
         metadata.title[metadata.titleLength] == '\0' && metadata.author[metadata.authorLength] == '\0' &&
         metadata.language[metadata.languageLength] == '\0';
}

void encodeSection(const PdfMetadataSection& section, uint8_t output[kSectionBytes]) {
  std::memset(output, 0, kSectionBytes);
  putU32(output, section.byteSize);
  putU32(output + 4, section.cumulativeSize);
  putU32(output + 8, section.firstWordOrdinal);
  putU32(output + 12, section.wordCount);
  putU32(output + 16, section.firstAnchorOrdinal);
  putU16(output + 20, static_cast<uint16_t>(section.tocIndex));
  putU16(output + 22, section.reserved);
}

PdfMetadataSection decodeSection(const uint8_t input[kSectionBytes]) {
  PdfMetadataSection section{};
  section.byteSize = getU32(input);
  section.cumulativeSize = getU32(input + 4);
  section.firstWordOrdinal = getU32(input + 8);
  section.wordCount = getU32(input + 12);
  section.firstAnchorOrdinal = getU32(input + 16);
  section.tocIndex = static_cast<int16_t>(getU16(input + 20));
  section.reserved = getU16(input + 22);
  return section;
}

PdfStatus validateSection(const PdfMetadata& metadata, const PdfMetadataSection& section, const uint16_t index,
                          const uint32_t priorBytes, const uint32_t priorWords) {
  if (section.byteSize == 0 || section.byteSize > std::numeric_limits<uint32_t>::max() - priorBytes ||
      section.cumulativeSize != priorBytes + section.byteSize || section.firstWordOrdinal != priorWords ||
      section.wordCount > std::numeric_limits<uint32_t>::max() - priorWords || section.reserved != 0 ||
      section.tocIndex < -1 || section.tocIndex >= static_cast<int16_t>(metadata.outlineCount)) {
    return PdfStatus::failure(PdfError::Malformed, index);
  }
  return PdfStatus::success();
}

}  // namespace

uint8_t PdfMetadataBuilder::priorityFor(const PdfMetadataOrigin origin, const bool language) {
  if (language) {
    if (origin == PdfMetadataOrigin::Catalog) {
      return 3;
    }
    return origin == PdfMetadataOrigin::Xmp ? 2 : 0;
  }
  if (origin == PdfMetadataOrigin::Xmp) {
    return 3;
  }
  if (origin == PdfMetadataOrigin::Info) {
    return 2;
  }
  return origin == PdfMetadataOrigin::Filename ? 1 : 0;
}

PdfStatus PdfMetadataBuilder::setValue(const PdfMetadataOrigin origin, const uint8_t* const value, const size_t length,
                                       char* const destination, const size_t capacity, uint16_t* const storedLength,
                                       uint8_t* const priority, const bool language) {
  if (!initialized_ || destination == nullptr || storedLength == nullptr || priority == nullptr) {
    return PdfStatus::failure(PdfError::InvalidArgument);
  }
  const uint8_t candidatePriority = priorityFor(origin, language);
  if (candidatePriority == 0 || candidatePriority < *priority) {
    return PdfStatus::success();
  }

  size_t start = 0;
  size_t accepted = 0;
  const PdfStatus status = boundedUtf8Length(value, length, capacity - 1, &start, &accepted);
  if (!status) {
    return status;
  }
  if (accepted == 0) {
    return PdfStatus::success();
  }
  std::memcpy(destination, value + start, accepted);
  destination[accepted] = '\0';
  *storedLength = static_cast<uint16_t>(accepted);
  *priority = candidatePriority;
  return PdfStatus::success();
}

PdfStatus PdfMetadataBuilder::begin(const uint8_t* const filename, const size_t length) {
  metadata_ = {};
  titlePriority_ = 0;
  authorPriority_ = 0;
  languagePriority_ = 0;
  initialized_ = true;
  PdfStatus status = setTitle(PdfMetadataOrigin::Filename, filename, length);
  if (!status) {
    initialized_ = false;
    return status;
  }
  if (metadata_.titleLength == 0) {
    static constexpr uint8_t fallback[] = {'P', 'D', 'F'};
    status = setTitle(PdfMetadataOrigin::Filename, fallback, sizeof(fallback));
  }
  return status;
}

PdfStatus PdfMetadataBuilder::setTitle(const PdfMetadataOrigin origin, const uint8_t* const value,
                                       const size_t length) {
  return setValue(origin, value, length, metadata_.title, sizeof(metadata_.title), &metadata_.titleLength,
                  &titlePriority_, false);
}

PdfStatus PdfMetadataBuilder::setAuthor(const PdfMetadataOrigin origin, const uint8_t* const value,
                                        const size_t length) {
  return setValue(origin, value, length, metadata_.author, sizeof(metadata_.author), &metadata_.authorLength,
                  &authorPriority_, false);
}

PdfStatus PdfMetadataBuilder::setLanguage(const PdfMetadataOrigin origin, const uint8_t* const value,
                                          const size_t length) {
  uint16_t stored = metadata_.languageLength;
  const PdfStatus status = setValue(origin, value, length, metadata_.language, sizeof(metadata_.language), &stored,
                                    &languagePriority_, true);
  if (status) {
    metadata_.languageLength = static_cast<uint8_t>(stored);
  }
  return status;
}

PdfStatus pdfEncodeMetadata(const PdfMetadata& metadata, const PdfMetadataSectionSource& sections,
                            const PdfByteSink& destination) {
  if (!destination.valid() || !validMetadata(metadata, sections)) {
    return PdfStatus::failure(PdfError::InvalidArgument);
  }

  uint8_t header[kHeaderBytes]{};
  std::memcpy(header, kMagic, sizeof(kMagic));
  putU16(header + 4, PdfMetadataLimits::CodecVersion);
  putU16(header + 6, static_cast<uint16_t>(kHeaderBytes));
  putU16(header + 8, metadata.sectionCount);
  putU16(header + 10, metadata.outlineCount);
  putU32(header + 12, metadata.totalWords);
  putU16(header + 16, metadata.titleLength);
  putU16(header + 18, metadata.authorLength);
  putU16(header + 20, metadata.languageLength);
  putU16(header + 22, 0);

  Encoder encoder(destination);
  PdfStatus status = encoder.write(header, sizeof(header));
  if (status) {
    status = encoder.write(reinterpret_cast<const uint8_t*>(metadata.title), metadata.titleLength);
  }
  if (status) {
    status = encoder.write(reinterpret_cast<const uint8_t*>(metadata.author), metadata.authorLength);
  }
  if (status) {
    status = encoder.write(reinterpret_cast<const uint8_t*>(metadata.language), metadata.languageLength);
  }

  uint32_t cumulativeBytes = 0;
  uint32_t cumulativeWords = 0;
  for (uint16_t index = 0; status && index < sections.count; ++index) {
    PdfMetadataSection section{};
    status = sections.read(sections.context, index, &section);
    if (status) {
      status = validateSection(metadata, section, index, cumulativeBytes, cumulativeWords);
    }
    if (!status) {
      break;
    }
    uint8_t encoded[kSectionBytes]{};
    encodeSection(section, encoded);
    status = encoder.write(encoded, sizeof(encoded));
    cumulativeBytes = section.cumulativeSize;
    cumulativeWords += section.wordCount;
  }
  if (status && cumulativeWords != metadata.totalWords) {
    status = PdfStatus::failure(PdfError::Malformed, cumulativeWords);
  }
  return status ? encoder.finish() : status;
}

PdfStepResult pdfStepEncodeMetadata(const PdfMetadata& metadata, const PdfMetadataSectionSource& sections,
                                    const PdfByteSink& destination, PdfMetadataEncodeRuntime& runtime,
                                    PdfWorkBudget& budget) {
  if (!destination.valid() || !validMetadata(metadata, sections)) {
    return PdfStepResult::failure(PdfStatus::failure(PdfError::InvalidArgument));
  }
  if (runtime.stage == PdfMetadataEncodeStage::Complete) {
    return PdfStepResult::completed();
  }
  if (budget.stopRequested() || budget.operationsRemaining == 0 || budget.bytesRemaining == 0) {
    return PdfStepResult::paused();
  }

  const auto writeChunk = [&](const uint8_t* bytes, const size_t length, const bool includeInCrc) -> PdfStepResult {
    const size_t chunk = std::min<size_t>(length, PdfMetadataLimits::IoChunkBytes);
    if (chunk == 0) {
      return PdfStepResult::completed();
    }
    if (budget.operationsRemaining == 0 || budget.bytesRemaining < chunk || budget.stopRequested()) {
      return PdfStepResult::paused();
    }
    (void)budget.consumeOperation();
    (void)budget.takeBytes(chunk);
    const PdfStatus status = pdfWriteExact(destination, bytes, chunk);
    if (!status) {
      return PdfStepResult::failure(status);
    }
    if (includeInCrc) {
      runtime.crc32 = pdfCacheCrc32(bytes, chunk, runtime.crc32);
    }
    return PdfStepResult::completed();
  };

  if (runtime.stage == PdfMetadataEncodeStage::Header) {
    uint8_t header[kHeaderBytes]{};
    std::memcpy(header, kMagic, sizeof(kMagic));
    putU16(header + 4, PdfMetadataLimits::CodecVersion);
    putU16(header + 6, static_cast<uint16_t>(kHeaderBytes));
    putU16(header + 8, metadata.sectionCount);
    putU16(header + 10, metadata.outlineCount);
    putU32(header + 12, metadata.totalWords);
    putU16(header + 16, metadata.titleLength);
    putU16(header + 18, metadata.authorLength);
    putU16(header + 20, metadata.languageLength);
    const PdfStepResult written = writeChunk(header, sizeof(header), true);
    if (written.complete()) {
      runtime.stage = PdfMetadataEncodeStage::Title;
    }
    return written.complete() ? PdfStepResult::paused() : written;
  }

  const auto writeField = [&](const uint8_t* bytes, const uint16_t length,
                              const PdfMetadataEncodeStage next) -> PdfStepResult {
    if (runtime.fieldOffset >= length) {
      runtime.fieldOffset = 0;
      runtime.stage = next;
      return PdfStepResult::paused();
    }
    const PdfStepResult written = writeChunk(bytes + runtime.fieldOffset, length - runtime.fieldOffset, true);
    if (written.complete()) {
      runtime.fieldOffset = static_cast<uint16_t>(
          runtime.fieldOffset + std::min<size_t>(length - runtime.fieldOffset, PdfMetadataLimits::IoChunkBytes));
      if (runtime.fieldOffset == length) {
        runtime.fieldOffset = 0;
        runtime.stage = next;
      }
    }
    return written.complete() ? PdfStepResult::paused() : written;
  };

  if (runtime.stage == PdfMetadataEncodeStage::Title) {
    return writeField(reinterpret_cast<const uint8_t*>(metadata.title), metadata.titleLength,
                      PdfMetadataEncodeStage::Author);
  }
  if (runtime.stage == PdfMetadataEncodeStage::Author) {
    return writeField(reinterpret_cast<const uint8_t*>(metadata.author), metadata.authorLength,
                      PdfMetadataEncodeStage::Language);
  }
  if (runtime.stage == PdfMetadataEncodeStage::Language) {
    return writeField(reinterpret_cast<const uint8_t*>(metadata.language), metadata.languageLength,
                      PdfMetadataEncodeStage::Sections);
  }

  if (runtime.stage == PdfMetadataEncodeStage::Sections) {
    if (runtime.sectionIndex >= sections.count) {
      if (runtime.cumulativeWords != metadata.totalWords) {
        return PdfStepResult::failure(PdfStatus::failure(PdfError::Malformed, runtime.cumulativeWords));
      }
      runtime.stage = PdfMetadataEncodeStage::Crc;
      return PdfStepResult::paused();
    }
    if (budget.operationsRemaining < 4U || budget.bytesRemaining < kSectionBytes ||
        !budget.consumeOperation() || budget.takeBytes(kSectionBytes) != kSectionBytes) {
      return PdfStepResult::paused();
    }
    PdfMetadataSection section{};
    PdfStatus status = sections.read(sections.context, runtime.sectionIndex, &section);
    if (status) {
      status = validateSection(metadata, section, runtime.sectionIndex, runtime.cumulativeBytes,
                               runtime.cumulativeWords);
    }
    if (!status) {
      return PdfStepResult::failure(status);
    }
    uint8_t encoded[kSectionBytes]{};
    encodeSection(section, encoded);
    status = pdfWriteExact(destination, encoded, sizeof(encoded));
    if (!status) {
      return PdfStepResult::failure(status);
    }
    runtime.crc32 = pdfCacheCrc32(encoded, sizeof(encoded), runtime.crc32);
    runtime.cumulativeBytes = section.cumulativeSize;
    runtime.cumulativeWords += section.wordCount;
    ++runtime.sectionIndex;
    return PdfStepResult::paused();
  }

  if (runtime.stage == PdfMetadataEncodeStage::Crc) {
    uint8_t encoded[kCrcBytes]{};
    putU32(encoded, runtime.crc32);
    const PdfStepResult written = writeChunk(encoded, sizeof(encoded), false);
    if (written.complete()) {
      runtime.stage = PdfMetadataEncodeStage::Complete;
      return PdfStepResult::completed();
    }
    return written;
  }
  return PdfStepResult::failure(PdfStatus::failure(PdfError::InvalidArgument));
}

PdfStatus pdfInspectMetadata(const PdfByteSource& source, PdfMetadata* const metadata) {
  if (!source.valid() || metadata == nullptr || source.size < kHeaderBytes + kCrcBytes) {
    return PdfStatus::failure(PdfError::InvalidArgument);
  }
  uint8_t header[kHeaderBytes]{};
  const PdfStatus status = pdfReadExact(source, 0, header, sizeof(header));
  if (!status) {
    return status;
  }
  PdfMetadata inspected{};
  inspected.sectionCount = getU16(header + 8);
  inspected.outlineCount = getU16(header + 10);
  inspected.totalWords = getU32(header + 12);
  inspected.titleLength = getU16(header + 16);
  inspected.authorLength = getU16(header + 18);
  const uint16_t languageLength = getU16(header + 20);
  if (std::memcmp(header, kMagic, sizeof(kMagic)) != 0 || getU16(header + 4) != PdfMetadataLimits::CodecVersion ||
      getU16(header + 6) != kHeaderBytes || getU16(header + 22) != 0 || inspected.sectionCount == 0 ||
      inspected.sectionCount > PdfMetadataLimits::MaxSections || inspected.outlineCount == 0 ||
      inspected.outlineCount > PdfMetadataLimits::MaxOutlineEntries ||
      inspected.titleLength >= PdfMetadataLimits::TitleBytes ||
      inspected.authorLength >= PdfMetadataLimits::AuthorBytes || languageLength >= PdfMetadataLimits::LanguageBytes) {
    return PdfStatus::failure(PdfError::Malformed);
  }
  inspected.languageLength = static_cast<uint8_t>(languageLength);
  *metadata = inspected;
  return PdfStatus::success();
}

PdfStatus pdfDecodeMetadata(const PdfByteSource& source, PdfMetadata* const metadata,
                            const PdfMetadataSectionVisitor& sections) {
  if (!source.valid() || metadata == nullptr || !sections.valid() || source.size < kHeaderBytes + kCrcBytes) {
    return PdfStatus::failure(PdfError::InvalidArgument);
  }

  Decoder decoder(source);
  uint8_t header[kHeaderBytes]{};
  PdfStatus status = decoder.read(header, sizeof(header));
  if (!status) {
    return status;
  }
  PdfMetadata decoded{};
  decoded.sectionCount = getU16(header + 8);
  decoded.outlineCount = getU16(header + 10);
  decoded.totalWords = getU32(header + 12);
  decoded.titleLength = getU16(header + 16);
  decoded.authorLength = getU16(header + 18);
  const uint16_t languageLength = getU16(header + 20);
  if (std::memcmp(header, kMagic, sizeof(kMagic)) != 0 || getU16(header + 4) != PdfMetadataLimits::CodecVersion ||
      getU16(header + 6) != kHeaderBytes || getU16(header + 22) != 0 || decoded.sectionCount == 0 ||
      decoded.sectionCount > PdfMetadataLimits::MaxSections || decoded.outlineCount == 0 ||
      decoded.outlineCount > PdfMetadataLimits::MaxOutlineEntries ||
      decoded.titleLength >= PdfMetadataLimits::TitleBytes || decoded.authorLength >= PdfMetadataLimits::AuthorBytes ||
      languageLength >= PdfMetadataLimits::LanguageBytes) {
    return PdfStatus::failure(PdfError::Malformed);
  }
  decoded.languageLength = static_cast<uint8_t>(languageLength);

  status = decoder.read(reinterpret_cast<uint8_t*>(decoded.title), decoded.titleLength);
  if (status) {
    status = decoder.read(reinterpret_cast<uint8_t*>(decoded.author), decoded.authorLength);
  }
  if (status) {
    status = decoder.read(reinterpret_cast<uint8_t*>(decoded.language), decoded.languageLength);
  }
  decoded.title[decoded.titleLength] = '\0';
  decoded.author[decoded.authorLength] = '\0';
  decoded.language[decoded.languageLength] = '\0';
  if (!status || decoded.titleLength == 0) {
    return status ? PdfStatus::failure(PdfError::Malformed) : status;
  }

  size_t ignoredStart = 0;
  size_t ignoredLength = 0;
  status = boundedUtf8Length(reinterpret_cast<const uint8_t*>(decoded.title), decoded.titleLength,
                             PdfMetadataLimits::TitleBytes - 1, &ignoredStart, &ignoredLength);
  if (status && ignoredLength != decoded.titleLength) {
    status = PdfStatus::failure(PdfError::Malformed);
  }
  if (status && decoded.authorLength != 0) {
    status = boundedUtf8Length(reinterpret_cast<const uint8_t*>(decoded.author), decoded.authorLength,
                               PdfMetadataLimits::AuthorBytes - 1, &ignoredStart, &ignoredLength);
    if (status && ignoredLength != decoded.authorLength) {
      status = PdfStatus::failure(PdfError::Malformed);
    }
  }
  if (status && decoded.languageLength != 0) {
    status = boundedUtf8Length(reinterpret_cast<const uint8_t*>(decoded.language), decoded.languageLength,
                               PdfMetadataLimits::LanguageBytes - 1, &ignoredStart, &ignoredLength);
    if (status && ignoredLength != decoded.languageLength) {
      status = PdfStatus::failure(PdfError::Malformed);
    }
  }

  uint32_t cumulativeBytes = 0;
  uint32_t cumulativeWords = 0;
  for (uint16_t index = 0; status && index < decoded.sectionCount; ++index) {
    uint8_t encoded[kSectionBytes]{};
    status = decoder.read(encoded, sizeof(encoded));
    if (!status) {
      break;
    }
    const PdfMetadataSection section = decodeSection(encoded);
    status = validateSection(decoded, section, index, cumulativeBytes, cumulativeWords);
    if (status) {
      status = sections.accept(sections.context, index, section);
    }
    cumulativeBytes = section.cumulativeSize;
    cumulativeWords += section.wordCount;
  }
  if (status && cumulativeWords != decoded.totalWords) {
    status = PdfStatus::failure(PdfError::Malformed, cumulativeWords);
  }
  if (status) {
    status = decoder.finish();
  }
  if (status) {
    *metadata = decoded;
  }
  return status;
}
