#include "PdfPreparation.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <new>

#include "Memory.h"
#include "PdfImageCache.h"
#include "PdfImageObject.h"
#include "PdfImagePreparation.h"
#include "PixelCache.h"

namespace {

// Leave two milliseconds of headroom for sink callbacks that may complete a
// bounded row write before control returns to the budget check.
constexpr uint32_t kSliceMilliseconds = 6;
constexpr uint32_t kSliceOperations = 32;
// Decoder output can expand into row-oriented cache writes. Keeping the input
// allowance below the public 4 KiB ceiling prevents one decoded slice from
// emitting more than one 4 KiB aggregate output chunk.
constexpr size_t kSliceBytes = 3U * 1024U;
constexpr uint64_t kSectionByteLimit = 1024ULL * 1024ULL;
constexpr uint16_t kArenaEntryCount = 128;
constexpr uint16_t kArenaTextBytes = 1536;
constexpr uint32_t kXrefRecordCount = 256;
constexpr uint32_t kTraversalRecordCount = 64;
constexpr uint16_t kPreparationPageLimit = 32;
constexpr uint16_t kPreparationOutlineLimit = 32;
constexpr uint8_t kPreparationNamedDestinationLimit = 16;
constexpr uint8_t kPreparationPageLabelLimit = 16;
constexpr uint16_t kPreparationLinkLimit = 32;
constexpr size_t kPreparationImageNameBytes = 32;
constexpr uint8_t kPreparationPageImageLimit = 8;
constexpr uint8_t kPreparationImageRepetitionLimit = 16;
constexpr uint8_t kPreparationPaletteSlots = 2;
constexpr size_t kPreparationPaletteBytes = PDF_IMAGE_BUILD_PALETTE_BYTES;
constexpr size_t kPreparationBlockWorkspaceBytes =
    PdfLimits::DecoderOutputBytes - kPreparationPaletteSlots * kPreparationPaletteBytes;
constexpr uint32_t kWarningOptionalImageOmitted = 1U << 0U;
constexpr size_t kRasterDecoderWorkspaceOffset = 4096;
constexpr size_t kMaskSpoolWorkspaceOffset = 8192;

struct ArenaWorkspace {
  PdfValue values[kArenaEntryCount]{};
  PdfDictionaryEntry dictionaries[kArenaEntryCount]{};
  PdfArrayItem arrays[kArenaEntryCount]{};
  uint8_t text[kArenaTextBytes]{};
};

struct RecordWorkspace {
  PdfXrefEntry xref[kXrefRecordCount]{};
  PdfPageTreeRecord traversal[kTraversalRecordCount]{};
};

static_assert(sizeof(ArenaWorkspace) <= PdfLimits::PageTextBytes);
static_assert(sizeof(RecordWorkspace) <= PdfLimits::PageRunBytes);
static_assert(kPreparationBlockWorkspaceBytes >= 2048);

struct PreparedOutlinePending {
  PdfObjectReference reference{};
  int16_t parentIndex = -1;
};

struct PreparedLink {
  PdfRawDestination destination{};
  uint16_t sourcePageIndex = 0;
};

struct PreparedImageCandidate {
  PdfObjectReference reference{};
  uint64_t streamOffset = 0;
  uint64_t streamLength = 0;
  uint64_t contentHash = PDF_CACHE_FNV64_OFFSET;
  uint32_t sourceCrc32 = 0;
  char name[kPreparationImageNameBytes]{};
  char href[PDF_CACHE_REQUIRED_PATH_CAPACITY]{};
  uint32_t width = 0;
  uint32_t height = 0;
  PdfImageParameters parameters{};
  PdfStreamFilter filters[PdfLimits::MaxFiltersPerStream]{};
  PdfObjectReference auxiliaryReference{};
  uint64_t auxiliaryStreamOffset = 0;
  uint64_t auxiliaryStreamLength = 0;
  PdfImageParameters auxiliaryParameters{};
  PdfStreamFilter auxiliaryFilters[PdfLimits::MaxFiltersPerStream]{};
  uint8_t nameLength = 0;
  uint8_t filterCount = 0;
  uint8_t auxiliaryFilterCount = 0;
  PdfImageAuxiliaryKind auxiliaryKind = PdfImageAuxiliaryKind::SoftMask;
  PdfImagePlacement placement{};
  uint16_t semanticBlockIndex = UINT16_MAX;
  uint8_t placementCount = 0;
  uint8_t documentRepetitionCount = 1;
  uint8_t rasterIdentityIndex = UINT8_MAX;
  bool hasAuxiliary = false;
  bool jpeg = false;
  bool raster = false;
  bool jpegCaptured = false;
  bool jpegCaptureFailed = false;
  bool placed = false;
  bool retained = false;
  bool coverCandidate = false;
};

struct FixedMatrix {
  int32_t a = 1 << 16;
  int32_t b = 0;
  int32_t c = 0;
  int32_t d = 1 << 16;
  int32_t e = 0;
  int32_t f = 0;
};

enum class RasterFinalizeStage : uint8_t {
  Decode,
  FinishExtractor,
  FlushWriter,
  SyncWriter,
  CloseWriter,
  Reserve,
  AppendRecord,
};

struct RasterRuntime {
  explicit RasterRuntime(const PdfStreamDecoderWorkspace& workspace) : decoder(workspace) {}

  PdfStreamDecoder decoder;
  PdfImageExtractor extractor;
  PdfCacheTrackedWriter writer{};
  PdfRequiredFileRecord record{};
  PdfByteRange encodedRange{};
  PdfImageWorkspace imageWorkspace{};
  PdfImageParameters parameters{};
  char relativePath[PDF_CACHE_REQUIRED_PATH_CAPACITY]{};
  char fullPath[PDF_CACHE_PATH_CAPACITY]{};
  RasterFinalizeStage finalizeStage = RasterFinalizeStage::Decode;
};

static_assert(sizeof(RasterRuntime) <= 4096);

struct MaskDecodeRuntime {
  explicit MaskDecodeRuntime(const PdfStreamDecoderWorkspace& workspace) : decoder(workspace) {}

  PdfStreamDecoder decoder;
  PdfMaskPlaneWriter plane;
  PdfByteRange encodedRange{};
  PdfMaskPlaneConfig config{};
};

struct MaskCompositeRuntime {
  PdfCacheTrackedWriter writer{};
  PdfPixelCacheWriter pixelWriter{};
  PdfRequiredFileRecord record{};
  PdfMaskSpoolRecord spoolRecord{};
  pixel_cache::Layout layout{};
  char relativePath[PDF_CACHE_REQUIRED_PATH_CAPACITY]{};
  char fullPath[PDF_CACHE_PATH_CAPACITY]{};
  uint16_t nextRow = 0;
};

struct FailedImageTagRange {
  uint32_t offset = 0;
  uint16_t sectionIndex = 0;
  uint16_t length = 0;
};

static_assert(sizeof(MaskDecodeRuntime) <= kRasterDecoderWorkspaceOffset);
static_assert(sizeof(MaskCompositeRuntime) <= kRasterDecoderWorkspaceOffset);
static_assert(kMaskSpoolWorkspaceOffset + sizeof(PdfMaskSpool) <= PdfLimits::PageRunBytes);

PdfStatus writeRasterCache(void* context, const uint8_t* source, const size_t requested, size_t* bytesWritten) {
  if (context == nullptr) {
    return PdfStatus::failure(PdfError::InvalidArgument);
  }
  PdfStatus status = pdfWriteTrackedCacheFile(static_cast<PdfCacheTrackedWriter*>(context), source, requested);
  *bytesWritten = status ? requested : 0;
  return status;
}

using RasterBatchCandidate = PdfDeferredImageRecord;

void calculateRasterOutputDimensions(const RasterBatchCandidate& candidate, const uint16_t maximumWidth,
                                     const uint16_t maximumHeight, uint16_t* const width, uint16_t* const height) {
  uint64_t outputWidth = candidate.width;
  uint64_t outputHeight = candidate.height;
  if (candidate.width > maximumWidth || candidate.height > maximumHeight) {
    if (static_cast<uint64_t>(candidate.width) * maximumHeight >
        static_cast<uint64_t>(candidate.height) * maximumWidth) {
      outputWidth = maximumWidth;
      outputHeight = static_cast<uint64_t>(candidate.height) * outputWidth / candidate.width;
    } else {
      outputHeight = maximumHeight;
      outputWidth = static_cast<uint64_t>(candidate.width) * outputHeight / candidate.height;
    }
    outputWidth = std::max<uint64_t>(1, outputWidth);
    outputHeight = std::max<uint64_t>(1, outputHeight);
  }
  *width = static_cast<uint16_t>(outputWidth);
  *height = static_cast<uint16_t>(outputHeight);
}

PdfStatus calculateRasterSourceBytes(const uint64_t streamLength, const bool hasAuxiliary,
                                     const uint64_t auxiliaryStreamLength, uint64_t* const sourceBytes) {
  if (sourceBytes == nullptr || streamLength == 0 ||
      (hasAuxiliary && auxiliaryStreamLength > UINT64_MAX - streamLength)) {
    return PdfStatus::failure(PdfError::Malformed, streamLength);
  }
  *sourceBytes = streamLength + (hasAuxiliary ? auxiliaryStreamLength : 0);
  return PdfStatus::success();
}

void hashPreparedImagePart(PreparedImageCandidate& candidate, const void* bytes, const size_t length) {
  candidate.contentHash = pdfCacheFnv64(bytes, length, candidate.contentHash);
  candidate.sourceCrc32 = pdfCacheCrc32(bytes, length, candidate.sourceCrc32);
}

PdfStatus beginPreparedRasterFingerprint(PreparedImageCandidate& candidate) {
  candidate.contentHash = PDF_CACHE_FNV64_OFFSET;
  candidate.sourceCrc32 = 0;
  hashPreparedImagePart(candidate, &candidate.parameters.width, sizeof(candidate.parameters.width));
  hashPreparedImagePart(candidate, &candidate.parameters.height, sizeof(candidate.parameters.height));
  hashPreparedImagePart(candidate, &candidate.parameters.bitsPerComponent,
                        sizeof(candidate.parameters.bitsPerComponent));
  hashPreparedImagePart(candidate, &candidate.parameters.predictor, sizeof(candidate.parameters.predictor));
  hashPreparedImagePart(candidate, &candidate.parameters.colorSpace, sizeof(candidate.parameters.colorSpace));
  hashPreparedImagePart(candidate, &candidate.parameters.decode, sizeof(candidate.parameters.decode));
  hashPreparedImagePart(candidate, &candidate.parameters.imageMaskPaintLuminance,
                        sizeof(candidate.parameters.imageMaskPaintLuminance));
  hashPreparedImagePart(candidate, &candidate.filterCount, sizeof(candidate.filterCount));
  hashPreparedImagePart(candidate, candidate.filters, candidate.filterCount * sizeof(candidate.filters[0]));
  hashPreparedImagePart(candidate, &candidate.parameters.paletteEntries, sizeof(candidate.parameters.paletteEntries));
  hashPreparedImagePart(candidate, &candidate.parameters.paletteBytes, sizeof(candidate.parameters.paletteBytes));
  if (candidate.parameters.paletteBytes == 0) {
    return PdfStatus::success();
  }
  if (candidate.parameters.palette == nullptr || candidate.parameters.paletteBytes > PDF_IMAGE_BUILD_PALETTE_BYTES) {
    return PdfStatus::failure(PdfError::Malformed, candidate.parameters.paletteBytes);
  }
  hashPreparedImagePart(candidate, candidate.parameters.palette, candidate.parameters.paletteBytes);
  return PdfStatus::success();
}

void hashPreparedRasterAuxiliaryContract(PreparedImageCandidate& candidate) {
  hashPreparedImagePart(candidate, &candidate.auxiliaryKind, sizeof(candidate.auxiliaryKind));
  hashPreparedImagePart(candidate, &candidate.auxiliaryStreamLength, sizeof(candidate.auxiliaryStreamLength));
  hashPreparedImagePart(candidate, &candidate.auxiliaryParameters.width, sizeof(candidate.auxiliaryParameters.width));
  hashPreparedImagePart(candidate, &candidate.auxiliaryParameters.height, sizeof(candidate.auxiliaryParameters.height));
  hashPreparedImagePart(candidate, &candidate.auxiliaryParameters.bitsPerComponent,
                        sizeof(candidate.auxiliaryParameters.bitsPerComponent));
  hashPreparedImagePart(candidate, &candidate.auxiliaryParameters.predictor,
                        sizeof(candidate.auxiliaryParameters.predictor));
  hashPreparedImagePart(candidate, &candidate.auxiliaryParameters.colorSpace,
                        sizeof(candidate.auxiliaryParameters.colorSpace));
  hashPreparedImagePart(candidate, &candidate.auxiliaryParameters.decode, sizeof(candidate.auxiliaryParameters.decode));
  hashPreparedImagePart(candidate, &candidate.auxiliaryFilterCount, sizeof(candidate.auxiliaryFilterCount));
  hashPreparedImagePart(candidate, candidate.auxiliaryFilters,
                        candidate.auxiliaryFilterCount * sizeof(candidate.auxiliaryFilters[0]));
}

bool copyPath(const char* source, char* destination, const size_t capacity) {
  if (source == nullptr || destination == nullptr || capacity == 0) {
    return false;
  }
  const size_t length = std::strlen(source);
  if (length == 0 || length >= capacity) {
    destination[0] = '\0';
    return false;
  }
  std::memcpy(destination, source, length + 1);
  return true;
}

bool asciiEqualInsensitive(const char* const value, const char* const expected, const size_t length) {
  for (size_t index = 0; index < length; ++index) {
    char left = value[index];
    char right = expected[index];
    if (left >= 'A' && left <= 'Z') {
      left = static_cast<char>(left - 'A' + 'a');
    }
    if (right >= 'A' && right <= 'Z') {
      right = static_cast<char>(right - 'A' + 'a');
    }
    if (left != right) {
      return false;
    }
  }
  return true;
}

bool tokenEquals(const PdfToken& token, const char* const value) {
  const size_t length = std::strlen(value);
  return token.length == length && std::memcmp(token.bytes, value, length) == 0;
}

bool keyEquals(const char* const key, const uint8_t keyLength, const char* const shortName,
               const char* const longName) {
  const size_t shortLength = std::strlen(shortName);
  const size_t longLength = std::strlen(longName);
  return (keyLength == shortLength && std::memcmp(key, shortName, shortLength) == 0) ||
         (keyLength == longLength && std::memcmp(key, longName, longLength) == 0);
}

bool pdfWhitespace(const uint8_t byte) {
  return byte == 0 || byte == '\t' || byte == '\n' || byte == '\f' || byte == '\r' || byte == ' ';
}

void writeLe16Bmp(uint8_t* const output, const uint16_t value) {
  output[0] = static_cast<uint8_t>(value);
  output[1] = static_cast<uint8_t>(value >> 8U);
}

void writeLe32Bmp(uint8_t* const output, const uint32_t value) {
  for (uint8_t index = 0; index < 4; ++index) {
    output[index] = static_cast<uint8_t>(value >> (index * 8U));
  }
}

void writeLe64(uint8_t* const output, const uint64_t value) {
  for (uint8_t index = 0; index < 8; ++index) {
    output[index] = static_cast<uint8_t>(value >> (index * 8U));
  }
}

constexpr size_t kManifestHeaderBytes = 84;
constexpr size_t kManifestTrailerBytes = 8;
constexpr size_t kManifestRecordHeaderBytes = 16;
constexpr size_t kCheckpointBytes = 96;

template <typename T>
void resetInPlace(T& value);

uint16_t readLe16Prep(const uint8_t* const input) {
  return static_cast<uint16_t>(input[0]) | static_cast<uint16_t>(input[1]) << 8U;
}

uint32_t readLe32Prep(const uint8_t* const input) {
  uint32_t value = 0;
  for (uint8_t index = 0; index < 4; ++index) {
    value |= static_cast<uint32_t>(input[index]) << (index * 8U);
  }
  return value;
}

uint64_t readLe64Prep(const uint8_t* const input) {
  uint64_t value = 0;
  for (uint8_t index = 0; index < 8; ++index) {
    value |= static_cast<uint64_t>(input[index]) << (index * 8U);
  }
  return value;
}

PdfStatus decodeManifestHeader(const uint8_t* const input, PdfCacheManifest* const manifest) {
  if (input == nullptr || manifest == nullptr) {
    return PdfStatus::failure(PdfError::InvalidArgument);
  }
  const uint8_t magic[] = {'P', 'R', 'M', 'F'};
  if (std::memcmp(input, magic, sizeof(magic)) != 0 || readLe16Prep(input + 4) != PDF_CACHE_CODEC_VERSION ||
      readLe16Prep(input + 6) != PDF_CACHE_FORMAT_VERSION || readLe16Prep(input + 8) != PDF_CACHE_CAPABILITY_VERSION ||
      readLe16Prep(input + 10) != 0 || input[16] > 1 || input[17] > 1 || readLe16Prep(input + 18) != 0) {
    return PdfStatus::failure(PdfError::Malformed);
  }
  resetInPlace(*manifest);
  manifest->sequence = readLe32Prep(input + 12);
  manifest->completed = input[16] != 0;
  manifest->source.modificationTime.known = input[17] != 0;
  manifest->warningFlags = readLe32Prep(input + 20);
  manifest->source.size = readLe64Prep(input + 24);
  manifest->source.modificationTime.value = readLe64Prep(input + 32);
  manifest->source.headFingerprint = readLe64Prep(input + 40);
  manifest->source.tailFingerprint = readLe64Prep(input + 48);
  manifest->generation = readLe32Prep(input + 56);
  manifest->totalWords = readLe32Prep(input + 60);
  manifest->requiredFileCount = readLe32Prep(input + 64);
  manifest->requiredFileBytes = readLe64Prep(input + 68);
  manifest->requiredFileLedger = readLe64Prep(input + 76);
  if (manifest->requiredFileCount > PDF_CACHE_MAX_REQUIRED_FILES) {
    return PdfStatus::failure(PdfError::Malformed);
  }
  return PdfStatus::success();
}

PdfStatus decodeManifestRecordHeader(const uint8_t* const input, PdfRequiredFileRecord* const record) {
  if (input == nullptr || record == nullptr || input[0] == 0 || input[0] >= sizeof(record->path) || input[1] != 0 ||
      readLe16Prep(input + 2) != 0) {
    return PdfStatus::failure(PdfError::Malformed);
  }
  resetInPlace(*record);
  record->pathLength = input[0];
  record->size = readLe64Prep(input + 4);
  record->crc32 = readLe32Prep(input + 12);
  return PdfStatus::success();
}

bool recoverableCacheSlotError(const PdfError error) {
  return error == PdfError::InvalidArgument || error == PdfError::UnexpectedEof || error == PdfError::Malformed ||
         error == PdfError::LimitExceeded;
}

struct FixedMemorySource {
  const uint8_t* bytes = nullptr;
  size_t size = 0;

  static PdfStatus read(void* const context, const uint64_t offset, uint8_t* const destination, const size_t requested,
                        size_t* const bytesRead) {
    if (context == nullptr || destination == nullptr || bytesRead == nullptr) {
      return PdfStatus::failure(PdfError::InvalidArgument, offset);
    }
    const auto& source = *static_cast<FixedMemorySource*>(context);
    if (offset > source.size) {
      return PdfStatus::failure(PdfError::InvalidOffset, offset);
    }
    *bytesRead = std::min(requested, source.size - static_cast<size_t>(offset));
    if (*bytesRead != 0) {
      std::memcpy(destination, source.bytes + offset, *bytesRead);
    }
    return PdfStatus::success();
  }

  PdfByteSource source() { return {this, size, read}; }
};

struct FixedMemorySink {
  uint8_t* data = nullptr;
  size_t capacity = 0;
  size_t size = 0;

  static PdfStatus write(void* const context, const uint8_t* const source, const size_t requested,
                         size_t* const bytesWritten) {
    if (context == nullptr || source == nullptr || bytesWritten == nullptr) {
      return PdfStatus::failure(PdfError::InvalidArgument);
    }
    auto& sink = *static_cast<FixedMemorySink*>(context);
    *bytesWritten = 0;
    if (requested > sink.capacity - sink.size) {
      return PdfStatus::failure(PdfError::LimitExceeded, sink.size + requested);
    }
    std::memcpy(sink.data + sink.size, source, requested);
    sink.size += requested;
    *bytesWritten = requested;
    return PdfStatus::success();
  }

  PdfByteSink sink() { return {this, write}; }
};

bool checkpointEqualPrep(const PdfBuildCheckpoint& left, const PdfBuildCheckpoint& right) {
  return left.sequence == right.sequence && pdfSourceIdentityEqual(left.source, right.source) &&
         left.generation == right.generation && left.phase == right.phase &&
         left.lastVerifiedPage == right.lastVerifiedPage && left.lastVerifiedObject == right.lastVerifiedObject &&
         left.emittedSections == right.emittedSections && left.emittedImages == right.emittedImages &&
         left.cumulativeWords == right.cumulativeWords && left.outputBytes == right.outputBytes &&
         left.warningFlags == right.warningFlags;
}

void encodeManifestHeader(const PdfCacheManifest& manifest, uint8_t* const output) {
  size_t offset = 0;
  const uint8_t magic[] = {'P', 'R', 'M', 'F'};
  std::memcpy(output + offset, magic, sizeof(magic));
  offset += sizeof(magic);
  writeLe16Bmp(output + offset, PDF_CACHE_CODEC_VERSION);
  offset += 2;
  writeLe16Bmp(output + offset, manifest.formatVersion);
  offset += 2;
  writeLe16Bmp(output + offset, manifest.capabilityVersion);
  offset += 2;
  writeLe16Bmp(output + offset, 0);
  offset += 2;
  writeLe32Bmp(output + offset, manifest.sequence);
  offset += 4;
  output[offset++] = manifest.completed ? 1 : 0;
  output[offset++] = manifest.source.modificationTime.known ? 1 : 0;
  writeLe16Bmp(output + offset, 0);
  offset += 2;
  writeLe32Bmp(output + offset, manifest.warningFlags);
  offset += 4;
  writeLe64(output + offset, manifest.source.size);
  offset += 8;
  writeLe64(output + offset, manifest.source.modificationTime.value);
  offset += 8;
  writeLe64(output + offset, manifest.source.headFingerprint);
  offset += 8;
  writeLe64(output + offset, manifest.source.tailFingerprint);
  offset += 8;
  writeLe32Bmp(output + offset, manifest.generation);
  offset += 4;
  writeLe32Bmp(output + offset, manifest.totalWords);
  offset += 4;
  writeLe32Bmp(output + offset, manifest.requiredFileCount);
  offset += 4;
  writeLe64(output + offset, manifest.requiredFileBytes);
  offset += 8;
  writeLe64(output + offset, manifest.requiredFileLedger);
}

size_t encodeManifestRecord(const PdfRequiredFileRecord& record, uint8_t* const output) {
  output[0] = record.pathLength;
  output[1] = 0;
  writeLe16Bmp(output + 2, 0);
  writeLe64(output + 4, record.size);
  writeLe32Bmp(output + 12, record.crc32);
  std::memcpy(output + kManifestRecordHeaderBytes, record.path, record.pathLength);
  return kManifestRecordHeaderBytes + record.pathLength;
}

uint8_t coverGlyphRow(char character, const uint8_t row) {
  static constexpr uint8_t glyphs[37][7] = {
      {14, 17, 17, 31, 17, 17, 17}, {30, 17, 17, 30, 17, 17, 30}, {14, 17, 16, 16, 16, 17, 14},
      {30, 17, 17, 17, 17, 17, 30}, {31, 16, 16, 30, 16, 16, 31}, {31, 16, 16, 30, 16, 16, 16},
      {14, 17, 16, 23, 17, 17, 15}, {17, 17, 17, 31, 17, 17, 17}, {14, 4, 4, 4, 4, 4, 14},
      {7, 2, 2, 2, 18, 18, 12},     {17, 18, 20, 24, 20, 18, 17}, {16, 16, 16, 16, 16, 16, 31},
      {17, 27, 21, 21, 17, 17, 17}, {17, 25, 21, 19, 17, 17, 17}, {14, 17, 17, 17, 17, 17, 14},
      {30, 17, 17, 30, 16, 16, 16}, {14, 17, 17, 17, 21, 18, 13}, {30, 17, 17, 30, 20, 18, 17},
      {15, 16, 16, 14, 1, 1, 30},   {31, 4, 4, 4, 4, 4, 4},       {17, 17, 17, 17, 17, 17, 14},
      {17, 17, 17, 17, 17, 10, 4},  {17, 17, 17, 21, 21, 21, 10}, {17, 17, 10, 4, 10, 17, 17},
      {17, 17, 10, 4, 4, 4, 4},     {31, 1, 2, 4, 8, 16, 31},     {14, 17, 19, 21, 25, 17, 14},
      {4, 12, 4, 4, 4, 4, 14},      {14, 17, 1, 2, 4, 8, 31},     {30, 1, 1, 14, 1, 1, 30},
      {2, 6, 10, 18, 31, 2, 2},     {31, 16, 16, 30, 1, 1, 30},   {14, 16, 16, 30, 17, 17, 14},
      {31, 1, 2, 4, 8, 8, 8},       {14, 17, 17, 14, 17, 17, 14}, {14, 17, 17, 15, 1, 1, 14},
      {14, 17, 1, 2, 4, 0, 4},
  };
  if (row >= 7 || character == ' ') {
    return 0;
  }
  if (character >= 'a' && character <= 'z') {
    character = static_cast<char>(character - 'a' + 'A');
  }
  if (character >= 'A' && character <= 'Z') {
    return glyphs[character - 'A'][row];
  }
  if (character >= '0' && character <= '9') {
    return glyphs[26 + character - '0'][row];
  }
  if (character == '-') {
    return row == 3 ? 14 : 0;
  }
  if (character == '.') {
    return row == 6 ? 4 : 0;
  }
  return glyphs[36][row];
}

void renderCoverTextRow(uint8_t* const output, const uint16_t width, const uint16_t row, const char* const text,
                        const uint16_t textLength, const uint16_t top, const uint8_t scale,
                        const uint8_t maximumLines) {
  if (output == nullptr || text == nullptr || textLength == 0 || scale == 0 || row < top) {
    return;
  }
  const uint16_t lineHeight = static_cast<uint16_t>(8U * scale);
  const uint16_t line = static_cast<uint16_t>((row - top) / lineHeight);
  const uint16_t rowInLine = static_cast<uint16_t>((row - top) % lineHeight);
  if (line >= maximumLines || rowInLine >= 7U * scale) {
    return;
  }
  const uint16_t charactersPerLine = std::max<uint16_t>(1, static_cast<uint16_t>((width - 16U) / (6U * scale)));
  const uint16_t start = static_cast<uint16_t>(line * charactersPerLine);
  if (start >= textLength) {
    return;
  }
  const uint16_t count = std::min<uint16_t>(charactersPerLine, textLength - start);
  const uint16_t textWidth = static_cast<uint16_t>(count * 6U * scale - scale);
  const uint16_t origin = textWidth < width ? static_cast<uint16_t>((width - textWidth) / 2U) : 0;
  const uint8_t glyphRow = static_cast<uint8_t>(rowInLine / scale);
  for (uint16_t character = 0; character < count; ++character) {
    const uint8_t bits = coverGlyphRow(text[start + character], glyphRow);
    for (uint8_t column = 0; column < 5; ++column) {
      if ((bits & (1U << (4U - column))) == 0) {
        continue;
      }
      const uint16_t x = static_cast<uint16_t>(origin + character * 6U * scale + column * scale);
      for (uint8_t repeat = 0; repeat < scale && x + repeat < width; ++repeat) {
        output[(x + repeat) / 8U] &= static_cast<uint8_t>(~(0x80U >> ((x + repeat) % 8U)));
      }
    }
  }
}

void sourceFallbackTitle(const char* const path, const uint8_t** const title, size_t* const length) {
  const char* start = path;
  for (const char* cursor = path; *cursor != '\0'; ++cursor) {
    if (*cursor == '/' || *cursor == '\\') {
      start = cursor + 1;
    }
  }
  size_t titleLength = std::strlen(start);
  if (titleLength >= 4 && asciiEqualInsensitive(start + titleLength - 4, ".pdf", 4)) {
    titleLength -= 4;
  }
  *title = reinterpret_cast<const uint8_t*>(start);
  *length = titleLength;
}

uint32_t deterministicGeneration(const PdfSourceIdentity& identity) {
  uint32_t generation = static_cast<uint32_t>(identity.headFingerprint ^ (identity.headFingerprint >> 32U) ^
                                              identity.tailFingerprint ^ (identity.tailFingerprint >> 32U));
  if (generation == 0) {
    generation = 1;
  }
  return generation;
}

bool parseTokenInt16(const PdfToken& token, int16_t* const value) {
  if (value == nullptr || (token.kind != PdfTokenKind::Integer && token.kind != PdfTokenKind::Real) ||
      token.length == 0) {
    return false;
  }
  size_t index = 0;
  bool negative = false;
  if (token.bytes[index] == '-' || token.bytes[index] == '+') {
    negative = token.bytes[index] == '-';
    if (++index == token.length) {
      return false;
    }
  }
  int32_t parsed = 0;
  bool digit = false;
  while (index < token.length && token.bytes[index] != '.') {
    if (token.bytes[index] < '0' || token.bytes[index] > '9') {
      return false;
    }
    digit = true;
    parsed = parsed * 10 + token.bytes[index] - '0';
    if (parsed > INT16_MAX + (negative ? 1 : 0)) {
      return false;
    }
    ++index;
  }
  if (!digit) {
    return false;
  }
  *value = static_cast<int16_t>(negative ? -parsed : parsed);
  return true;
}

bool parseTokenFixed16(const PdfToken& token, int32_t* const value) {
  if (value == nullptr || (token.kind != PdfTokenKind::Integer && token.kind != PdfTokenKind::Real) ||
      token.length == 0) {
    return false;
  }
  size_t index = 0;
  bool negative = false;
  if (token.bytes[index] == '-' || token.bytes[index] == '+') {
    negative = token.bytes[index] == '-';
    if (++index == token.length) {
      return false;
    }
  }
  uint64_t whole = 0;
  bool digit = false;
  while (index < token.length && token.bytes[index] != '.') {
    if (token.bytes[index] < '0' || token.bytes[index] > '9') {
      return false;
    }
    digit = true;
    whole = whole * 10U + static_cast<uint8_t>(token.bytes[index] - '0');
    if (whole > 32768U) {
      return false;
    }
    ++index;
  }
  uint64_t fraction = 0;
  uint64_t scale = 1;
  if (index < token.length && token.bytes[index] == '.') {
    ++index;
    while (index < token.length) {
      if (token.bytes[index] < '0' || token.bytes[index] > '9') {
        return false;
      }
      digit = true;
      if (scale < 1000000U) {
        fraction = fraction * 10U + static_cast<uint8_t>(token.bytes[index] - '0');
        scale *= 10U;
      }
      ++index;
    }
  }
  if (!digit) {
    return false;
  }
  int64_t fixed = static_cast<int64_t>(whole * 65536U + fraction * 65536U / scale);
  if (negative) {
    fixed = -fixed;
  }
  if (fixed < INT32_MIN || fixed > INT32_MAX) {
    return false;
  }
  *value = static_cast<int32_t>(fixed);
  return true;
}

uint8_t fixedColorByte(const int64_t raw) {
  constexpr int64_t one = INT64_C(1) << 16U;
  const int64_t bounded = std::clamp<int64_t>(raw, 0, one);
  return static_cast<uint8_t>((bounded * 255 + one / 2) / one);
}

uint8_t rgbLuminance(const uint8_t red, const uint8_t green, const uint8_t blue) {
  return static_cast<uint8_t>((77U * red + 150U * green + 29U * blue + 128U) >> 8U);
}

bool fixedColorLuminance(const int32_t* const operands, const uint8_t count, uint8_t* const luminance) {
  if (operands == nullptr || luminance == nullptr || (count != 1U && count != 3U && count != 4U)) {
    return false;
  }
  if (count == 1U) {
    *luminance = fixedColorByte(operands[0]);
    return true;
  }
  if (count == 3U) {
    *luminance = rgbLuminance(fixedColorByte(operands[0]), fixedColorByte(operands[1]), fixedColorByte(operands[2]));
    return true;
  }
  constexpr int64_t one = INT64_C(1) << 16U;
  const int64_t cyan = std::clamp<int64_t>(operands[0], 0, one);
  const int64_t magenta = std::clamp<int64_t>(operands[1], 0, one);
  const int64_t yellow = std::clamp<int64_t>(operands[2], 0, one);
  const int64_t black = std::clamp<int64_t>(operands[3], 0, one);
  *luminance = rgbLuminance(fixedColorByte(one - std::min(one, cyan + black)),
                            fixedColorByte(one - std::min(one, magenta + black)),
                            fixedColorByte(one - std::min(one, yellow + black)));
  return true;
}

int32_t clampFixed(const int64_t value) {
  return static_cast<int32_t>(std::clamp<int64_t>(value, INT32_MIN, INT32_MAX));
}

FixedMatrix concatenateMatrix(const FixedMatrix& current, const int32_t* const operand) {
  FixedMatrix result{};
  result.a =
      clampFixed((static_cast<int64_t>(current.a) * operand[0] + static_cast<int64_t>(current.c) * operand[1]) >> 16U);
  result.b =
      clampFixed((static_cast<int64_t>(current.b) * operand[0] + static_cast<int64_t>(current.d) * operand[1]) >> 16U);
  result.c =
      clampFixed((static_cast<int64_t>(current.a) * operand[2] + static_cast<int64_t>(current.c) * operand[3]) >> 16U);
  result.d =
      clampFixed((static_cast<int64_t>(current.b) * operand[2] + static_cast<int64_t>(current.d) * operand[3]) >> 16U);
  result.e = clampFixed(
      ((static_cast<int64_t>(current.a) * operand[4] + static_cast<int64_t>(current.c) * operand[5]) >> 16U) +
      current.e);
  result.f = clampFixed(
      ((static_cast<int64_t>(current.b) * operand[4] + static_cast<int64_t>(current.d) * operand[5]) >> 16U) +
      current.f);
  return result;
}

PdfImagePlacement matrixPlacement(const FixedMatrix& matrix, const uint32_t width, const uint32_t height,
                                  const bool inlineImage) {
  const int32_t xs[4] = {
      matrix.e,
      clampFixed(static_cast<int64_t>(matrix.a) + matrix.e),
      clampFixed(static_cast<int64_t>(matrix.c) + matrix.e),
      clampFixed(static_cast<int64_t>(matrix.a) + matrix.c + matrix.e),
  };
  const int32_t ys[4] = {
      matrix.f,
      clampFixed(static_cast<int64_t>(matrix.b) + matrix.f),
      clampFixed(static_cast<int64_t>(matrix.d) + matrix.f),
      clampFixed(static_cast<int64_t>(matrix.b) + matrix.d + matrix.f),
  };
  PdfImagePlacement placement{};
  placement.pixelWidth = width;
  placement.pixelHeight = height;
  placement.xMin = *std::min_element(std::begin(xs), std::end(xs));
  placement.yMin = *std::min_element(std::begin(ys), std::end(ys));
  placement.xMax = *std::max_element(std::begin(xs), std::end(xs));
  placement.yMax = *std::max_element(std::begin(ys), std::end(ys));
  placement.flags = inlineImage ? PdfImageInline : 0;
  return placement;
}

PdfImagePlacement orientPlacementToPage(const PdfImagePlacement& placement, const PdfPageInfo& page) {
  PdfImagePlacement oriented = placement;
  const bool quarterTurn = page.rotation == 90 || page.rotation == 270;
  const int64_t boxWidth = static_cast<int64_t>(quarterTurn ? page.pageHeight : page.pageWidth) << 16U;
  const int64_t boxHeight = static_cast<int64_t>(quarterTurn ? page.pageWidth : page.pageHeight) << 16U;
  const int32_t xs[4] = {
      placement.xMin,
      placement.xMax,
      placement.xMin,
      placement.xMax,
  };
  const int32_t ys[4] = {
      placement.yMin,
      placement.yMin,
      placement.yMax,
      placement.yMax,
  };
  int64_t minimumX = INT64_MAX;
  int64_t minimumY = INT64_MAX;
  int64_t maximumX = INT64_MIN;
  int64_t maximumY = INT64_MIN;
  for (uint8_t index = 0; index < std::size(xs); ++index) {
    const int64_t localX = static_cast<int64_t>(xs[index]) - page.viewXMin;
    const int64_t localY = static_cast<int64_t>(ys[index]) - page.viewYMin;
    int64_t transformedX = localX;
    int64_t transformedY = localY;
    switch (page.rotation) {
      case 90:
        transformedX = localY;
        transformedY = boxWidth - localX;
        break;
      case 180:
        transformedX = boxWidth - localX;
        transformedY = boxHeight - localY;
        break;
      case 270:
        transformedX = boxHeight - localY;
        transformedY = localX;
        break;
      default:
        break;
    }
    minimumX = std::min(minimumX, transformedX);
    minimumY = std::min(minimumY, transformedY);
    maximumX = std::max(maximumX, transformedX);
    maximumY = std::max(maximumY, transformedY);
  }
  oriented.xMin = clampFixed(minimumX);
  oriented.yMin = clampFixed(minimumY);
  oriented.xMax = clampFixed(maximumX);
  oriented.yMax = clampFixed(maximumY);
  return oriented;
}

bool repeatedPageBandDecoration(const PdfImagePlacement& placement, const uint16_t pageWidth,
                                const uint16_t pageHeight) {
  if (pageWidth == 0 || pageHeight == 0 || placement.xMax <= placement.xMin || placement.yMax <= placement.yMin) {
    return false;
  }
  const int64_t width = (static_cast<int64_t>(placement.xMax) - placement.xMin + 65535) >> 16U;
  const int64_t height = (static_cast<int64_t>(placement.yMax) - placement.yMin + 65535) >> 16U;
  const int64_t lowerBand = static_cast<int64_t>(pageHeight) << 13U;
  const int64_t upperBand = static_cast<int64_t>(pageHeight) * 7 << 13U;
  const bool inBand = placement.yMin <= lowerBand || placement.yMax >= upperBand;
  return inBand && width <= pageWidth / 2U && height <= pageHeight / 8U;
}

PdfStatus closeDurableWriter(const PdfCacheIo& io, PdfCacheHandle* const handle) {
  if (handle == nullptr || !handle->valid()) {
    return PdfStatus::failure(PdfError::InvalidArgument);
  }
  PdfStatus status = io.flush(io.context, *handle);
  if (status) {
    status = io.sync(io.context, *handle);
  }
  const PdfStatus closeStatus = io.close(io.context, handle);
  return status ? closeStatus : status;
}

template <typename T>
void resetInPlace(T& value) {
  value.~T();
  new (&value) T();
}

}  // namespace

struct PdfPreparation::RasterBatchWorkspace {
  PdfMaskSpoolCloseRuntime maskClose{};
  PdfMaskSpoolReadRuntime maskRead{};
  PdfImageSpoolReadRuntime imageBuildRead{};
  uint8_t maskCompositeIndex = 0;
};

struct PdfPreparation::PlacementWorkspace {
  FixedMatrix current{};
  FixedMatrix stack[PdfLimits::MaxFormDepth]{};
  int32_t operands[6]{};
  uint8_t nonstrokingLuminance = 0;
  uint8_t luminanceStack[PdfLimits::MaxFormDepth]{};
  uint8_t operandCount = 0;
  uint8_t depth = 0;
};

struct PdfPreparation::ExtractedBlockRecord {
  uint16_t textOffset = 0;
  uint16_t textLength = 0;
  int16_t sourceFontSize = 0;
  uint16_t reserved = 0;
};

struct PdfPreparation::NavigationWorkspace {
  PdfPageInfo pages[kPreparationPageLimit]{};
  PdfPageInfo pageScratch{};
  PdfRawOutlineNode outlineNodeScratch{};
  PdfRawLinkAnnotation linkAnnotationScratch{};
  PdfCacheManifest manifestScratch{};
  PdfToken contentTokenScratch{};
  PdfOutlineEntry outlineEntries[kPreparationOutlineLimit]{};
  PdfNamedDestinationRecord namedDestinations[kPreparationNamedDestinationLimit]{};
  PdfPageLabelRange pageLabels[kPreparationPageLabelLimit]{};
  PreparedLink links[kPreparationLinkLimit]{};
  PreparedOutlinePending outlinePending[kPreparationOutlineLimit]{};
  PdfObjectReference outlineSeen[kPreparationOutlineLimit]{};
  PdfMetadataSection sections[kPreparationPageLimit]{};
  PdfRequiredFileRecord sectionFiles[kPreparationPageLimit]{};
  PdfImageCacheEntry imageCacheEntries[PDF_IMAGE_CACHE_MAX_ENTRIES]{};
  PreparedImageCandidate imageCandidates[kPreparationPageImageLimit]{};
  PdfImageCache imageCache{};
  PdfImageCacheRuntime imageCacheRuntime{};
  PdfImageSpoolReadRuntime imageFileRead{};
  PdfCachedImage cachedImageScratch{};
  PdfImageObjectDescriptor imageDescriptorScratch{};
  PdfImageObjectDescriptor baseDescriptorScratch{};
  uint32_t pageFirstAnchors[kPreparationPageLimit]{};
  uint16_t pageFirstSections[kPreparationPageLimit]{};
  uint16_t pageWidths[kPreparationPageLimit]{};
  uint16_t pageHeights[kPreparationPageLimit]{};
  uint16_t linkCount = 0;
};

struct PdfPreparation::SectionRepairRuntime {
  FailedImageTagRange tags[PDF_IMAGE_BUILD_SPOOL_MAX_RECORDS]{};
  PdfCacheTrackedWriter finalWriter{};
  PdfRequiredFileRecord originalRecord{};
  PdfRequiredFileRecord finalRecord{};
  PdfCacheHandle reader{};
  PdfCacheHandle temporaryWriter{};
  char originalPath[PDF_CACHE_PATH_CAPACITY]{};
  char temporaryPath[PDF_CACHE_PATH_CAPACITY]{};
  uint64_t offset = 0;
  uint8_t failedTagCount = 0;
  uint8_t nextTag = 0;
  uint8_t nextCandidate = 0;
  uint8_t sectionTagEnd = 0;
  uint16_t sectionIndex = 0;
  SectionRepairStage stage = SectionRepairStage::Idle;
};

bool PdfPreparationPaintGate::shouldPaint(const uint8_t progressPercent, const uint32_t nowMs) {
  if (intermediatePaintCount_ >= 10 || progressPercent < lastPaintPercent_ ||
      static_cast<uint8_t>(progressPercent - lastPaintPercent_) < 10 || nowMs - lastPaintMs_ < 15000U) {
    return false;
  }
  lastPaintPercent_ = progressPercent;
  lastPaintMs_ = nowMs;
  ++intermediatePaintCount_;
  return true;
}

PdfPreparation::~PdfPreparation() {
  imageBuildSpool_.abort();
  imageFileSpool_.abort();
  destroyParsers();
  if (sectionWriter_.open) {
    pdfAbortTrackedCacheFile(&sectionWriter_);
  }
  if (metadataWriter_.open) {
    pdfAbortTrackedCacheFile(&metadataWriter_);
  }
  if (outlineWriter_.open) {
    pdfAbortTrackedCacheFile(&outlineWriter_);
  }
  (void)closeSource();
  releaseWorkspaces();
}

PdfStatus PdfPreparation::begin(const PdfPreparationConfig& config) {
  imageBuildSpool_.abort();
  imageFileSpool_.abort();
  destroyParsers();
  if (sectionWriter_.open) {
    pdfAbortTrackedCacheFile(&sectionWriter_);
  }
  if (metadataWriter_.open) {
    pdfAbortTrackedCacheFile(&metadataWriter_);
  }
  if (outlineWriter_.open) {
    pdfAbortTrackedCacheFile(&outlineWriter_);
  }
  (void)closeSource();
  releaseWorkspaces();
  resources_.reset();

  if (!config.io.valid() || config.sourcePath == nullptr || config.cacheDirectory == nullptr ||
      config.maximumRasterOutputWidth == 0 || config.maximumRasterOutputWidth > PdfLimits::DecoderOutputBytes ||
      config.maximumRasterOutputHeight == 0 || config.resourceHooks.measure == nullptr ||
      !copyPath(config.sourcePath, sourcePath_, sizeof(sourcePath_))) {
    phase_ = PdfPreparationPhase::Failed;
    status_ = PdfStatus::failure(PdfError::InvalidArgument);
    return status_;
  }

  config_ = config;
  status_ = pdfFormatCacheRoot(config.cacheDirectory, sourcePath_, cacheRoot_, sizeof(cacheRoot_));
  if (!status_) {
    phase_ = PdfPreparationPhase::Failed;
    return status_;
  }
  const uint8_t* fallbackTitle = nullptr;
  size_t fallbackTitleLength = 0;
  sourceFallbackTitle(sourcePath_, &fallbackTitle, &fallbackTitleLength);
  status_ = metadataBuilder_.begin(fallbackTitle, fallbackTitleLength);
  if (!status_) {
    phase_ = PdfPreparationPhase::Failed;
    return status_;
  }

  resources_.emplace(config.resourceHooks);
  sourceHandle_ = {};
  sourceMetadata_ = {};
  sourceIdentity_ = {};
  sourceContext_ = {};
  arena_ = {};
  xrefRecords_ = {};
  traversalRecords_ = {};
  navigation_ = nullptr;
  namedDestinations_.reset();
  pageLabels_.reset();
  outlineBuilder_.reset();
  catalogNavigation_ = {};
  infoReference_ = {};
  activeNavigationReference_ = {};
  pageCount_ = 0;
  currentPageIndex_ = 0;
  currentContentIndex_ = 0;
  extractedBlockCount_ = 0;
  currentBlockIndex_ = 0;
  sectionEmitEndBlock_ = 0;
  sectionCount_ = 0;
  explicitOutlineCount_ = 0;
  outlinePendingCount_ = 0;
  outlineSeenCount_ = 0;
  currentOutlineParent_ = -1;
  currentAnnotationPage_ = 0;
  currentAnnotationIndex_ = 0;
  navigationStage_ = 0;
  navigationTask_ = NavigationTask::None;
  imageResolveTask_ = ImageResolveTask::None;
  imageCandidateCount_ = 0;
  currentPageImageStart_ = 0;
  currentPageImageEnd_ = 0;
  sectionEmitImageIndex_ = 0;
  imageResolveIndex_ = 0;
  imagePaletteCount_ = 0;
  rasterDecodeIndex_ = 0;
  currentPageImageCandidate_ = -1;
  imageCacheStage_ = ImageCacheStage::Idle;
  imageCacheRange_ = {};
  inlineCapturedJpeg_ = {};
  imageCacheOffset_ = 0;
  rasterIdentityScanIndex_ = 0;
  retainedImageFileCount_ = 0;
  lastContentNameLength_ = 0;
  std::memset(lastContentName_, 0, sizeof(lastContentName_));
  resetInlineImageDictionaryState();
  inlineNavigationSpoolPath_[0] = '\0';
  inlineNavigationSpoolHandle_ = {};
  inlineNavigationSpoolOffset_ = 0;
  inlineNavigationSpoolCrc32_ = 0;
  inlineNavigationSpoolReadCrc32_ = 0;
  inlineNavigationSpillStage_ = InlineNavigationSpillStage::None;
  placement_ = nullptr;
  rasterBatch_ = nullptr;
  maskSpool_ = nullptr;
  navigationSpoolPath_[0] = '\0';
  maskSpoolPath_[0] = '\0';
  imageBuildSpoolPath_[0] = '\0';
  imageFileSpoolPath_[0] = '\0';
  navigationSpoolHandle_ = {};
  navigationSpoolOffset_ = 0;
  navigationSpoolCrc32_ = 0;
  navigationSpoolReadCrc32_ = 0;
  navigationSpoolBytes_ = 0;
  navigationSpoolWriteCount_ = 0;
  navigationSpoolReadCount_ = 0;
  maskSpoolWriteCount_ = 0;
  maskSpoolReadCount_ = 0;
  navigationSpoolStage_ = NavigationSpoolStage::None;
  std::fill(std::begin(imageRepetitionReferences_), std::end(imageRepetitionReferences_), PdfObjectReference{});
  std::fill(std::begin(imageRepetitionCounts_), std::end(imageRepetitionCounts_), 0);
  imageRepetitionEntryCount_ = 0;
  continueAfterImageDecode_ = false;
  sectionEmitStage_ = SectionEmitStage::Idle;
  rasterRuntimeActive_ = false;
  maskDecodeRuntimeActive_ = false;
  maskCompositeRuntimeActive_ = false;
  sectionRepairRuntimeActive_ = false;
  rasterDecodeStage_ = RasterDecodeStage::Idle;
  failedRasterImages_ = 0;
  hasInfoReference_ = false;
  contentRange_ = {};
  transcriptLength_ = 0;
  currentFontSize_ = 0;
  lastNumericValue_ = 0;
  nextAnchorOrdinal_ = 0;
  currentSectionFirstWord_ = 0;
  currentSectionFirstAnchor_ = 0;
  cumulativeSectionBytes_ = 0;
  metadataEncodeBytes_ = 0;
  xmpStreamOffset_ = 0;
  xmpStreamLength_ = 0;
  coverCandidateSourceCount_ = 0;
  for (auto& source : coverCandidateSources_) {
    source = {};
  }
  resetInPlace(cacheStore_);
  cacheCapacity_ = {};
  cacheBudget_ = {};
  resetInPlace(manifestSelection_);
  resetInPlace(checkpointSelection_);
  resetInPlace(sectionWriter_);
  resetInPlace(metadataWriter_);
  resetInPlace(outlineWriter_);
  outlineEncodeRuntime_ = {};
  cacheSetupHandle_ = {};
  cacheSetupFileSize_ = 0;
  cacheSetupOffset_ = 0;
  cacheSetupDecodedFileBytes_ = 0;
  cacheSetupDecodedLedger_ = PDF_CACHE_FNV64_OFFSET;
  cacheSetupCrc32_ = 0;
  cacheSetupRecordIndex_ = 0;
  cacheSetupSlot_ = 0;
  cacheSetupStage_ = CacheSetupStage::Idle;
  checkpointCommitHandle_ = {};
  checkpointCommitStage_ = CheckpointCommitStage::Idle;
  cleanupIndex_ = 0;
  cleanupStage_ = CleanupStage::Idle;
  manifestHandle_ = {};
  manifestPath_[0] = '\0';
  manifestOffset_ = 0;
  manifestEncodedBytes_ = 0;
  manifestRecordIndex_ = 0;
  manifestCrc32_ = 0;
  manifestReadCrc32_ = 0;
  manifestTargetSlot_ = 0;
  manifestCommitStage_ = ManifestCommitStage::Idle;
  sectionRecord_ = {};
  metadataRecord_ = {};
  outlineRecord_ = {};
  coverImageSourceRecord_ = {};
  for (auto& record : coverRecords_) {
    record = {};
  }
  coverFileCount_ = 0;
  typographyAssetIndex_ = 0;
  typographyRow_ = 0;
  typographySourceHandle_ = {};
  coverImageContentHash_ = 0;
  coverImageSourceCrc32_ = 0;
  typographySourceWidth_ = 0;
  typographySourceHeight_ = 0;
  typographySourceRowBytes_ = 0;
  typographySourceLoadedRow_ = UINT16_MAX;
  typographyScaledWidth_ = 0;
  typographyScaledHeight_ = 0;
  typographyOffsetX_ = 0;
  typographyOffsetY_ = 0;
  jpegPreview_.reset();
  meaningfulEarlyImageSeen_ = false;
  coverImageFingerprintSelected_ = false;
  coverImageRecordAvailable_ = false;
  coverImageSourceJpeg_ = false;
  typographyAssetStage_ = TypographyAssetStage::Idle;
  navigationRecordsPrepared_ = false;
  resetInPlace(semanticWriter_);
  resetInPlace(metadata_);
  generation_ = 0;
  sequence_ = 0;
  totalWords_ = 0;
  warningFlags_ = 0;
  resetInPlace(activeRasterCandidate_);
  std::fill(std::begin(rasterCanonicalRecordIndices_), std::end(rasterCanonicalRecordIndices_), UINT8_MAX);
  progressPercent_ = 0;
  allocationIndex_ = 0;
  cancelRequested_ = false;
  resumedFromCheckpoint_ = false;
  phase_ = PdfPreparationPhase::ResourceGate;
  status_ = PdfStatus::success();
  return status_;
}

size_t PdfPreparation::resourceCurrentBytes() const { return resources_.has_value() ? resources_->currentBytes() : 0; }

size_t PdfPreparation::resourcePeakBytes() const { return resources_.has_value() ? resources_->peakBytes() : 0; }

uint32_t PdfPreparation::nowMs() const { return config_.nowMs == nullptr ? 0 : config_.nowMs(config_.clockContext); }

void PdfPreparation::setPhase(const PdfPreparationPhase phase, const uint8_t progressPercent) {
  phase_ = phase;
  progressPercent_ = progressPercent;
}

PdfStepResult PdfPreparation::pause() { return PdfStepResult::paused(); }

PdfStepResult PdfPreparation::fail(const PdfStatus status) {
  status_ = status.ok() ? PdfStatus::failure(PdfError::Malformed) : status;
  destroyParsers();
  if (sectionWriter_.open) {
    pdfAbortTrackedCacheFile(&sectionWriter_);
  }
  if (metadataWriter_.open) {
    pdfAbortTrackedCacheFile(&metadataWriter_);
  }
  if (outlineWriter_.open) {
    pdfAbortTrackedCacheFile(&outlineWriter_);
  }
  (void)closeSource();
  imageBuildSpool_.abort();
  imageFileSpool_.abort();
  if (generation_ != 0 && sourceIdentity_.size != 0) {
    (void)commitCheckpoint(PdfBuildPhase::Failed);
  }
  releaseWorkspaces();
  phase_ = PdfPreparationPhase::Failed;
  return PdfStepResult::failure(status_);
}

PdfStepResult PdfPreparation::cancel() {
  destroyParsers();
  if (sectionWriter_.open) {
    pdfAbortTrackedCacheFile(&sectionWriter_);
  }
  if (metadataWriter_.open) {
    pdfAbortTrackedCacheFile(&metadataWriter_);
  }
  if (outlineWriter_.open) {
    pdfAbortTrackedCacheFile(&outlineWriter_);
  }
  (void)closeSource();
  imageBuildSpool_.abort();
  imageFileSpool_.abort();
  releaseWorkspaces();
  PdfStatus cleanupStatus = PdfStatus::success();
  bool generationProtected = false;
  for (const PdfCacheManifestSlotState& slot : manifestSelection_.slots) {
    generationProtected = generationProtected || (slot.valid && slot.manifest.generation == generation_);
  }
  if (generation_ != 0 && !generationProtected && config_.io.valid()) {
    const int pathLength = std::snprintf(sectionPath_, sizeof(sectionPath_), "%s/gen_%lu", cacheRoot_,
                                         static_cast<unsigned long>(generation_));
    if (pathLength <= 0 || static_cast<size_t>(pathLength) >= sizeof(sectionPath_)) {
      cleanupStatus = PdfStatus::failure(PdfError::LimitExceeded);
    } else {
      cleanupStatus = config_.io.remove(config_.io.context, sectionPath_, true);
      if (!cleanupStatus && cleanupStatus.error == PdfError::InvalidOffset) {
        cleanupStatus = PdfStatus::success();
      }
    }
  }
  if (!cleanupStatus) {
    status_ = cleanupStatus;
    phase_ = PdfPreparationPhase::Failed;
    return PdfStepResult::failure(status_);
  }
  status_ = PdfStatus::failure(PdfError::Cancelled);
  phase_ = PdfPreparationPhase::Cancelled;
  return PdfStepResult::failure(status_);
}

bool PdfPreparation::cancelRequested(void* context) { return static_cast<PdfPreparation*>(context)->cancelRequested_; }

bool PdfPreparation::sliceExpired(void* context) {
  auto& self = *static_cast<PdfPreparation*>(context);
  return self.config_.nowMs != nullptr && self.nowMs() - self.sliceStartedAtMs_ >= kSliceMilliseconds;
}

PdfStatus PdfPreparation::readSource(void* context, const uint64_t offset, uint8_t* destination, const size_t requested,
                                     size_t* bytesRead) {
  if (context == nullptr) {
    return PdfStatus::failure(PdfError::InvalidArgument, offset);
  }
  auto& source = *static_cast<SourceContext*>(context);
  if (source.io == nullptr || source.handle == nullptr || !source.handle->valid() || offset > source.size ||
      requested > PdfLimits::SourceBufferBytes) {
    return PdfStatus::failure(PdfError::InvalidArgument, offset);
  }
  return source.io->read(source.io->context, *source.handle, offset, destination, requested, bytesRead);
}

PdfByteSource PdfPreparation::source() { return {&sourceContext_, sourceMetadata_.size, readSource}; }

PdfStatus PdfPreparation::readMemoryRecord(void* context, const uint32_t ordinal, void* record,
                                           const size_t recordSize) {
  if (context == nullptr || record == nullptr) {
    return PdfStatus::failure(PdfError::InvalidArgument, ordinal);
  }
  auto& records = *static_cast<MemoryRecordContext*>(context);
  if (recordSize != records.recordSize || ordinal >= records.capacity) {
    return PdfStatus::failure(PdfError::InvalidOffset, ordinal);
  }
  std::memcpy(record, records.bytes + static_cast<size_t>(ordinal) * recordSize, recordSize);
  return PdfStatus::success();
}

PdfStatus PdfPreparation::writeMemoryRecord(void* context, const uint32_t ordinal, const void* record,
                                            const size_t recordSize) {
  if (context == nullptr || record == nullptr) {
    return PdfStatus::failure(PdfError::InvalidArgument, ordinal);
  }
  auto& records = *static_cast<MemoryRecordContext*>(context);
  if (recordSize != records.recordSize || ordinal >= records.capacity) {
    return PdfStatus::failure(PdfError::InvalidOffset, ordinal);
  }
  std::memcpy(records.bytes + static_cast<size_t>(ordinal) * recordSize, record, recordSize);
  return PdfStatus::success();
}

PdfFixedRecordStore PdfPreparation::recordStore(MemoryRecordContext& context) {
  return {&context, context.capacity, context.recordSize, readMemoryRecord, writeMemoryRecord};
}

PdfStatus PdfPreparation::capturePage(void* context, const PdfPageInfo& page) {
  if (context == nullptr) {
    return PdfStatus::failure(PdfError::InvalidArgument);
  }
  auto& self = *static_cast<PdfPreparation*>(context);
  if (self.navigation_ == nullptr || self.pageCount_ >= kPreparationPageLimit) {
    return PdfStatus::failure(PdfError::LimitExceeded, page.pageIndex);
  }
  if (page.pageWidth == 0 || page.pageHeight == 0) {
    return PdfStatus::failure(PdfError::Malformed, page.pageIndex);
  }
  self.navigation_->pages[self.pageCount_] = page;
  self.navigation_->pageWidths[self.pageCount_] = page.pageWidth;
  self.navigation_->pageHeights[self.pageCount_] = page.pageHeight;
  if (page.pageIndex < PdfLimits::MaxCoverScanPages && page.hasResources &&
      self.coverCandidateSourceCount_ < PdfLimits::MaxCoverCandidateSources) {
    const PdfObjectReference reference = page.resourcesIndirect ? page.resourceReference : page.resourceOwner;
    if (reference.objectNumber != 0) {
      bool alreadyRecorded = false;
      for (uint8_t index = 0; index < self.coverCandidateSourceCount_; ++index) {
        if (self.coverCandidateSources_[index].reference == reference) {
          alreadyRecorded = true;
          break;
        }
      }
      if (!alreadyRecorded) {
        self.coverCandidateSources_[self.coverCandidateSourceCount_++] = {
            reference,
            static_cast<uint16_t>(page.pageIndex),
            page.resourcesIndirect,
        };
      }
    }
  }
  ++self.pageCount_;
  return PdfStatus::success();
}

bool PdfPreparation::coverCandidateSource(const uint8_t index, PdfCoverCandidateSource* const output) const {
  if (output == nullptr || index >= coverCandidateSourceCount_) {
    return false;
  }
  *output = coverCandidateSources_[index];
  return true;
}

PdfStatus PdfPreparation::writeSection(void* context, const uint8_t* source, const size_t requested,
                                       size_t* bytesWritten) {
  if (context == nullptr || bytesWritten == nullptr || requested > PdfLimits::SourceBufferBytes) {
    return PdfStatus::failure(PdfError::InvalidArgument);
  }
  auto& self = *static_cast<PdfPreparation*>(context);
  const PdfStatus status = pdfWriteTrackedCacheFile(&self.sectionWriter_, source, requested);
  *bytesWritten = status ? requested : 0;
  return status;
}

PdfStatus PdfPreparation::writeMetadata(void* context, const uint8_t* source, const size_t requested,
                                        size_t* bytesWritten) {
  if (context == nullptr || bytesWritten == nullptr || (source == nullptr && requested != 0)) {
    return PdfStatus::failure(PdfError::InvalidArgument);
  }
  auto& self = *static_cast<PdfPreparation*>(context);
  if (!self.sourceWindow_ || requested > PdfLimits::SourceBufferBytes - self.metadataEncodeBytes_) {
    *bytesWritten = 0;
    return PdfStatus::failure(PdfError::LimitExceeded, self.metadataEncodeBytes_);
  }
  std::memcpy(self.sourceWindow_.get() + self.metadataEncodeBytes_, source, requested);
  self.metadataEncodeBytes_ += requested;
  *bytesWritten = requested;
  return PdfStatus::success();
}

PdfStatus PdfPreparation::writeOutline(void* context, const uint8_t* source, const size_t requested,
                                       size_t* bytesWritten) {
  if (context == nullptr || bytesWritten == nullptr) {
    return PdfStatus::failure(PdfError::InvalidArgument);
  }
  auto& self = *static_cast<PdfPreparation*>(context);
  const uint64_t before = self.outlineWriter_.record.size;
  const PdfStatus status = pdfWriteTrackedCacheFile(&self.outlineWriter_, source, requested);
  *bytesWritten = static_cast<size_t>(self.outlineWriter_.record.size - before);
  return status;
}

PdfStatus PdfPreparation::discardInlineImageDecoded(void*, const uint8_t* const source, const size_t requested,
                                                    size_t* const bytesWritten) {
  if ((source == nullptr && requested != 0) || bytesWritten == nullptr) {
    return PdfStatus::failure(PdfError::InvalidArgument);
  }
  *bytesWritten = requested;
  return PdfStatus::success();
}

PdfStatus PdfPreparation::emitBlock(void*, const PdfSemanticBlockRecord&) { return PdfStatus::success(); }

PdfStatus PdfPreparation::readMetadataSection(void* context, const uint16_t index, PdfMetadataSection* record) {
  if (context == nullptr || record == nullptr) {
    return PdfStatus::failure(PdfError::InvalidArgument, index);
  }
  auto& self = *static_cast<PdfPreparation*>(context);
  if (self.navigation_ == nullptr || index >= self.sectionCount_) {
    return PdfStatus::failure(PdfError::InvalidOffset, index);
  }
  *record = self.navigation_->sections[index];
  return PdfStatus::success();
}

PdfStatus PdfPreparation::readOutlineEntry(void* context, const uint16_t index, PdfOutlineEntry* record) {
  if (context == nullptr || record == nullptr) {
    return PdfStatus::failure(PdfError::InvalidArgument, index);
  }
  auto& self = *static_cast<PdfPreparation*>(context);
  if (self.navigation_ == nullptr || !self.outlineBuilder_.has_value() || index >= self.outlineBuilder_->count()) {
    return PdfStatus::failure(PdfError::InvalidOffset, index);
  }
  *record = self.navigation_->outlineEntries[index];
  return PdfStatus::success();
}

PdfStatus PdfPreparation::readRequiredFile(void* context, const uint32_t index, PdfRequiredFileRecord* record) {
  static_assert(sizeof(PdfRequiredFileRecord) * PDF_IMAGE_BUILD_SPOOL_MAX_RECORDS <= PdfLimits::PageTextBytes);
  if (context == nullptr || record == nullptr) {
    return PdfStatus::failure(PdfError::InvalidArgument, index);
  }
  auto& self = *static_cast<PdfPreparation*>(context);
  const uint32_t imageEnd = static_cast<uint32_t>(self.sectionCount_) + self.retainedImageFileCount_;
  const uint32_t coverEnd = imageEnd + self.coverFileCount_;
  if (self.navigation_ == nullptr || index >= coverEnd + 2U) {
    return PdfStatus::failure(PdfError::InvalidOffset, index);
  }
  if (index < self.sectionCount_) {
    *record = self.navigation_->sectionFiles[index];
  } else if (index < imageEnd) {
    if (!self.pageText_) {
      return PdfStatus::failure(PdfError::InsufficientMemory);
    }
    const auto* const imageRecords = reinterpret_cast<const PdfRequiredFileRecord*>(self.pageText_.get());
    *record = imageRecords[index - self.sectionCount_];
  } else if (index < coverEnd) {
    *record = self.coverRecords_[index - imageEnd];
  } else if (index == coverEnd) {
    *record = self.metadataRecord_;
  } else {
    *record = self.outlineRecord_;
  }
  return PdfStatus::success();
}

void PdfPreparation::abortInlineNavigationSpill() {
  if (inlineNavigationSpoolHandle_.valid() && config_.io.valid()) {
    (void)config_.io.close(config_.io.context, &inlineNavigationSpoolHandle_);
  }
  inlineNavigationSpoolHandle_ = {};
  if (inlineNavigationSpoolPath_[0] != '\0' && config_.io.valid()) {
    (void)config_.io.remove(config_.io.context, inlineNavigationSpoolPath_, false);
  }
  inlineNavigationSpoolPath_[0] = '\0';
  inlineImageEncodedLength_ = 0;
  inlineNavigationSpoolOffset_ = 0;
  inlineNavigationSpoolCrc32_ = 0;
  inlineNavigationSpoolReadCrc32_ = 0;
  inlineNavigationSpillStage_ = InlineNavigationSpillStage::None;
}

void PdfPreparation::abortNavigationSpool() {
  if (navigationSpoolHandle_.valid() && config_.io.valid()) {
    (void)config_.io.close(config_.io.context, &navigationSpoolHandle_);
  }
  navigationSpoolHandle_ = {};
  if (navigationSpoolPath_[0] != '\0' && config_.io.valid()) {
    (void)config_.io.remove(config_.io.context, navigationSpoolPath_, false);
  }
  navigationSpoolPath_[0] = '\0';
  navigationSpoolOffset_ = 0;
  navigationSpoolCrc32_ = 0;
  navigationSpoolReadCrc32_ = 0;
  navigationSpoolStage_ = NavigationSpoolStage::None;
}

void PdfPreparation::abortManifestCommit() {
  if (manifestHandle_.valid() && config_.io.valid()) {
    (void)config_.io.close(config_.io.context, &manifestHandle_);
  }
  manifestHandle_ = {};
  if (manifestPath_[0] != '\0' && config_.io.valid() && manifestCommitStage_ != ManifestCommitStage::Complete) {
    (void)config_.io.remove(config_.io.context, manifestPath_, false);
  }
  manifestPath_[0] = '\0';
  manifestOffset_ = 0;
  manifestEncodedBytes_ = 0;
  manifestRecordIndex_ = 0;
  manifestCrc32_ = 0;
  manifestReadCrc32_ = 0;
  manifestCommitStage_ = ManifestCommitStage::Idle;
}

void PdfPreparation::destroyParsers() {
  abortInlineNavigationSpill();
  inlineImageDecoder_.reset();
  contentLexer_.reset();
  pageWalker_.reset();
  resolver_.reset();
  xrefParser_.reset();
  xref_.reset();
  outlineBuilder_.reset();
  pageLabels_.reset();
  namedDestinations_.reset();
}

PdfStatus PdfPreparation::closeSource() {
  if (!sourceHandle_.valid() || !config_.io.valid()) {
    sourceHandle_ = {};
    sourceContext_ = {};
    return PdfStatus::success();
  }
  const PdfStatus status = config_.io.close(config_.io.context, &sourceHandle_);
  sourceContext_ = {};
  return status;
}

PdfStatus PdfPreparation::reopenSource() {
  if (sourceHandle_.valid() || !config_.io.valid() || sourceIdentity_.size == 0) {
    return PdfStatus::failure(PdfError::InvalidArgument);
  }
  PdfStatus status = config_.io.open(config_.io.context, sourcePath_, PdfCacheOpenMode::Read, &sourceHandle_);
  PdfCacheFileMetadata metadata{};
  if (status) {
    status = config_.io.metadata(config_.io.context, sourceHandle_, &metadata);
  }
  if (!status || metadata.directory || metadata.symlinkLike || metadata.size != sourceIdentity_.size) {
    if (sourceHandle_.valid()) {
      (void)config_.io.close(config_.io.context, &sourceHandle_);
    }
    return status ? PdfStatus::failure(PdfError::InvalidArgument, metadata.size) : status;
  }
  sourceMetadata_ = metadata;
  sourceContext_ = {&config_.io, &sourceHandle_, sourceMetadata_.size};
  return PdfStatus::success();
}

void PdfPreparation::abortActiveImageRuntime() {
  if (rasterRuntimeActive_ && runRecords_) {
    auto* runtime = reinterpret_cast<RasterRuntime*>(runRecords_.get());
    if (runtime->writer.open) {
      pdfAbortTrackedCacheFile(&runtime->writer);
    }
    runtime->~RasterRuntime();
    rasterRuntimeActive_ = false;
  }
  if (maskDecodeRuntimeActive_ && runRecords_) {
    auto* runtime = reinterpret_cast<MaskDecodeRuntime*>(runRecords_.get());
    runtime->~MaskDecodeRuntime();
    maskDecodeRuntimeActive_ = false;
  }
  if (maskCompositeRuntimeActive_ && runRecords_) {
    auto* runtime = reinterpret_cast<MaskCompositeRuntime*>(runRecords_.get());
    if (runtime->writer.open || runtime->writer.fullPath[0] != '\0') {
      pdfAbortTrackedCacheFile(&runtime->writer);
    }
    runtime->~MaskCompositeRuntime();
    maskCompositeRuntimeActive_ = false;
  }
}

PdfStepResult PdfPreparation::omitActiveRasterImage(const PdfStatus status) {
  if (rasterDecodeIndex_ >= PDF_IMAGE_BUILD_SPOOL_MAX_RECORDS || status.ok()) {
    return PdfStepResult::failure(status.ok() ? PdfStatus::failure(PdfError::InvalidArgument, rasterDecodeIndex_)
                                              : status);
  }
  failedRasterImages_ |= UINT64_C(1) << rasterDecodeIndex_;
  warningFlags_ |= kWarningOptionalImageOmitted;
  cacheBudget_.optionalOmitted = true;
  return PdfStepResult::completed();
}

void PdfPreparation::abortSectionRepairRuntime() {
  if (!sectionRepairRuntimeActive_ || !runRecords_) {
    return;
  }
  auto* runtime = reinterpret_cast<SectionRepairRuntime*>(runRecords_.get());
  if (runtime->finalWriter.open) {
    pdfAbortTrackedCacheFile(&runtime->finalWriter);
  } else if (runtime->finalWriter.failed && runtime->finalWriter.fullPath[0] != '\0') {
    (void)config_.io.remove(config_.io.context, runtime->finalWriter.fullPath, false);
  }
  if (runtime->temporaryWriter.valid()) {
    (void)config_.io.close(config_.io.context, &runtime->temporaryWriter);
  }
  if (runtime->reader.valid()) {
    (void)config_.io.close(config_.io.context, &runtime->reader);
  }
  if (runtime->temporaryPath[0] != '\0') {
    (void)config_.io.remove(config_.io.context, runtime->temporaryPath, false);
  }
  runtime->~SectionRepairRuntime();
  sectionRepairRuntimeActive_ = false;
}

void PdfPreparation::releaseWorkspaces() {
  abortSectionRepairRuntime();
  abortManifestCommit();
  jpegPreview_.reset();
  if (typographySourceHandle_.valid() && config_.io.valid()) {
    (void)config_.io.close(config_.io.context, &typographySourceHandle_);
  }
  typographySourceHandle_ = {};
  if (cacheSetupHandle_.valid() && config_.io.valid()) {
    (void)config_.io.close(config_.io.context, &cacheSetupHandle_);
  }
  cacheSetupHandle_ = {};
  cacheSetupFileSize_ = 0;
  cacheSetupOffset_ = 0;
  cacheSetupDecodedFileBytes_ = 0;
  cacheSetupDecodedLedger_ = PDF_CACHE_FNV64_OFFSET;
  cacheSetupCrc32_ = 0;
  cacheSetupRecordIndex_ = 0;
  cacheSetupSlot_ = 0;
  cacheSetupStage_ = CacheSetupStage::Idle;
  if (checkpointCommitHandle_.valid() && config_.io.valid()) {
    (void)config_.io.close(config_.io.context, &checkpointCommitHandle_);
  }
  checkpointCommitHandle_ = {};
  checkpointCommitStage_ = CheckpointCommitStage::Idle;
  cleanupIndex_ = 0;
  cleanupStage_ = CleanupStage::Idle;
  if (navigation_ != nullptr) {
    navigation_->imageCache.abortJpeg(navigation_->imageCacheRuntime);
  }
  navigation_ = nullptr;
  placement_ = nullptr;
  abortActiveImageRuntime();
  if (maskSpool_ != nullptr) {
    maskSpool_->abort();
    maskSpool_->~PdfMaskSpool();
    maskSpool_ = nullptr;
  }
  imageBuildSpool_.abort();
  imageFileSpool_.abort();
  abortNavigationSpool();
  if (!resources_.has_value()) {
    dictionary_.reset();
    sourceWindow_.reset();
    decoderOutput_.reset();
    pageText_.reset();
    runRecords_.reset();
    operandScratch_.reset();
    return;
  }

  if (operandScratch_) {
    operandScratch_.reset();
    (void)resources_->release(PdfResourceKind::OperandScratch);
  }
  if (runRecords_) {
    runRecords_.reset();
    (void)resources_->release(PdfResourceKind::RunRecords);
  }
  if (pageText_) {
    pageText_.reset();
    (void)resources_->release(PdfResourceKind::PageText);
  }
  if (decoderOutput_) {
    decoderOutput_.reset();
    (void)resources_->release(PdfResourceKind::DecoderOutput);
  }
  if (sourceWindow_) {
    sourceWindow_.reset();
    (void)resources_->release(PdfResourceKind::SourceWindow);
  }
  if (dictionary_) {
    dictionary_.reset();
    (void)resources_->release(PdfResourceKind::InflateDictionary);
  }
}

bool PdfPreparation::allocateNextWorkspace() {
  if (!resources_.has_value() || allocationIndex_ >= PDF_RESOURCE_SLOT_COUNT) {
    return false;
  }

  PdfResourceKind kind = PdfResourceKind::InflateDictionary;
  size_t bytes = 0;
  bool allocated = false;
  switch (allocationIndex_) {
    case 0:
      kind = PdfResourceKind::InflateDictionary;
      bytes = PdfLimits::UzlibDictionaryBytes;
      dictionary_ = makeUniqueNoThrow<uint8_t[]>(bytes);
      allocated = dictionary_ != nullptr;
      break;
    case 1:
      kind = PdfResourceKind::SourceWindow;
      bytes = PdfLimits::SourceBufferBytes;
      sourceWindow_ = makeUniqueNoThrow<uint8_t[]>(bytes);
      allocated = sourceWindow_ != nullptr;
      break;
    case 2:
      kind = PdfResourceKind::DecoderOutput;
      bytes = PdfLimits::DecoderOutputBytes;
      decoderOutput_ = makeUniqueNoThrow<uint8_t[]>(bytes);
      allocated = decoderOutput_ != nullptr;
      break;
    case 3:
      kind = PdfResourceKind::PageText;
      bytes = PdfLimits::PageTextBytes;
      pageText_ = makeUniqueNoThrow<uint8_t[]>(bytes);
      allocated = pageText_ != nullptr;
      break;
    case 4:
      kind = PdfResourceKind::RunRecords;
      bytes = PdfLimits::PageRunBytes;
      runRecords_ = makeUniqueNoThrow<uint8_t[]>(bytes);
      allocated = runRecords_ != nullptr;
      break;
    case 5:
      kind = PdfResourceKind::OperandScratch;
      bytes = PdfLimits::OperandOrderHistogramBytes;
      operandScratch_ = makeUniqueNoThrow<uint8_t[]>(bytes);
      allocated = operandScratch_ != nullptr;
      break;
    default:
      return false;
  }

  if (!allocated || !resources_->acquire(kind, bytes)) {
    switch (allocationIndex_) {
      case 0:
        dictionary_.reset();
        break;
      case 1:
        sourceWindow_.reset();
        break;
      case 2:
        decoderOutput_.reset();
        break;
      case 3:
        pageText_.reset();
        break;
      case 4:
        runRecords_.reset();
        break;
      case 5:
        operandScratch_.reset();
        break;
      default:
        break;
    }
    return false;
  }
  ++allocationIndex_;
  return true;
}

PdfStatus PdfPreparation::initializeParserStorage() {
  static_assert(sizeof(NavigationWorkspace) <= PdfLimits::UzlibDictionaryBytes);
  static_assert(sizeof(ExtractedBlockRecord) <= 8);
  if (!dictionary_ || !pageText_ || !runRecords_ || !decoderOutput_) {
    return PdfStatus::failure(PdfError::InsufficientMemory);
  }
  navigation_ = new (dictionary_.get()) NavigationWorkspace{};
  for (uint32_t index = 0; index < kPreparationPageLimit; ++index) {
    navigation_->pageFirstAnchors[index] = UINT32_MAX;
    navigation_->pageFirstSections[index] = UINT16_MAX;
  }
  auto* arenaWorkspace = new (pageText_.get()) ArenaWorkspace{};
  auto* recordWorkspace = new (runRecords_.get()) RecordWorkspace{};
  arena_ = {
      arenaWorkspace->values,       static_cast<uint16_t>(std::size(arenaWorkspace->values)),
      arenaWorkspace->dictionaries, static_cast<uint16_t>(std::size(arenaWorkspace->dictionaries)),
      arenaWorkspace->arrays,       static_cast<uint16_t>(std::size(arenaWorkspace->arrays)),
      arenaWorkspace->text,         static_cast<uint16_t>(std::size(arenaWorkspace->text)),
  };
  xrefRecords_ = {
      reinterpret_cast<uint8_t*>(recordWorkspace->xref),
      sizeof(PdfXrefEntry),
      static_cast<uint32_t>(std::size(recordWorkspace->xref)),
  };
  traversalRecords_ = {
      reinterpret_cast<uint8_t*>(recordWorkspace->traversal),
      sizeof(PdfPageTreeRecord),
      static_cast<uint32_t>(std::size(recordWorkspace->traversal)),
  };
  xref_.emplace(recordStore(xrefRecords_));
  return PdfStatus::success();
}

PdfStepResult PdfPreparation::stepSetupCache(PdfWorkBudget& budget) {
  static constexpr const char* kCheckpointNames[] = {"build.a", "build.b"};
  static constexpr const char* kManifestNames[] = {"manifest.a", "manifest.b"};

  const auto pauseForBudget = [&]() { return PdfStepResult::paused(); };
  const auto readBytes = [&](const uint64_t offset, uint8_t* const destination, const size_t length) -> PdfStatus {
    if (budget.bytesRemaining < length || !budget.consumeOperation()) {
      return PdfStatus::failure(PdfError::BudgetExhausted);
    }
    (void)budget.takeBytes(length);
    size_t bytesRead = 0;
    const PdfStatus status =
        config_.io.read(config_.io.context, cacheSetupHandle_, offset, destination, length, &bytesRead);
    if (!status) {
      return status;
    }
    return bytesRead == length ? PdfStatus::success() : PdfStatus::failure(PdfError::UnexpectedEof, offset + bytesRead);
  };

  if (cacheSetupStage_ == CacheSetupStage::Idle) {
    if (navigation_ == nullptr || !sourceWindow_ || cacheSetupHandle_.valid()) {
      return PdfStepResult::failure(PdfStatus::failure(PdfError::InsufficientMemory));
    }
    resetInPlace(checkpointSelection_);
    resetInPlace(manifestSelection_);
    cacheSetupSlot_ = 0;
    cacheSetupStage_ = CacheSetupStage::CloseSource;
    return PdfStepResult::paused();
  }

  if (cacheSetupStage_ == CacheSetupStage::CloseSource) {
    if (sourceHandle_.valid() && !budget.consumeOperation()) {
      return pauseForBudget();
    }
    const PdfStatus status = closeSource();
    if (!status) {
      return PdfStepResult::failure(status);
    }
    cacheSetupStage_ = CacheSetupStage::CreateCacheDirectory;
    return PdfStepResult::paused();
  }

  if (cacheSetupStage_ == CacheSetupStage::CreateCacheDirectory) {
    if (!budget.consumeOperation()) {
      return pauseForBudget();
    }
    const PdfStatus status = config_.io.mkdir(config_.io.context, config_.cacheDirectory);
    if (!status) {
      return PdfStepResult::failure(status);
    }
    cacheSetupStage_ = CacheSetupStage::InitializeStore;
    return PdfStepResult::paused();
  }

  if (cacheSetupStage_ == CacheSetupStage::InitializeStore) {
    const PdfStatus status = cacheStore_.initialize(config_.io, cacheRoot_);
    if (!status) {
      return PdfStepResult::failure(status);
    }
    cacheSetupSlot_ = 0;
    cacheSetupStage_ = CacheSetupStage::OpenCheckpointSlot;
    return PdfStepResult::paused();
  }

  if (cacheSetupStage_ == CacheSetupStage::OpenCheckpointSlot) {
    const int pathLength =
        std::snprintf(sectionPath_, sizeof(sectionPath_), "%s/%s", cacheRoot_, kCheckpointNames[cacheSetupSlot_]);
    if (pathLength <= 0 || static_cast<size_t>(pathLength) >= sizeof(sectionPath_)) {
      return PdfStepResult::failure(PdfStatus::failure(PdfError::LimitExceeded));
    }
    if (!budget.consumeOperation()) {
      return pauseForBudget();
    }
    PdfStatus status = config_.io.open(config_.io.context, sectionPath_, PdfCacheOpenMode::Read, &cacheSetupHandle_);
    if (!status && status.error == PdfError::InvalidOffset) {
      resetInPlace(checkpointSelection_.slots[cacheSetupSlot_]);
      if (++cacheSetupSlot_ < 2U) {
        return PdfStepResult::paused();
      }
      cacheSetupSlot_ = 0;
      cacheSetupStage_ = CacheSetupStage::OpenManifestSlot;
      return PdfStepResult::paused();
    }
    if (!status) {
      return PdfStepResult::failure(status);
    }
    cacheSetupStage_ = CacheSetupStage::ReadCheckpointMetadata;
    return PdfStepResult::paused();
  }

  if (cacheSetupStage_ == CacheSetupStage::ReadCheckpointMetadata) {
    if (!budget.consumeOperation()) {
      return pauseForBudget();
    }
    PdfCacheFileMetadata metadata{};
    const PdfStatus status = config_.io.metadata(config_.io.context, cacheSetupHandle_, &metadata);
    if (!status) {
      return PdfStepResult::failure(status);
    }
    cacheSetupFileSize_ = metadata.size;
    if (metadata.directory || metadata.symlinkLike || metadata.size != kCheckpointBytes) {
      resetInPlace(checkpointSelection_.slots[cacheSetupSlot_]);
      cacheSetupStage_ = CacheSetupStage::CloseCheckpointSlot;
      return PdfStepResult::paused();
    }
    cacheSetupStage_ = CacheSetupStage::ReadCheckpoint;
    return PdfStepResult::paused();
  }

  if (cacheSetupStage_ == CacheSetupStage::ReadCheckpoint) {
    PdfStatus status = readBytes(0, sourceWindow_.get(), kCheckpointBytes);
    if (!status && status.error == PdfError::BudgetExhausted) {
      return pauseForBudget();
    }
    PdfBuildCheckpointSlotState& state = checkpointSelection_.slots[cacheSetupSlot_];
    if (status) {
      FixedMemorySource memory{sourceWindow_.get(), kCheckpointBytes};
      status = pdfDecodeBuildCheckpoint(memory.source(), &state.checkpoint);
    }
    if (!status) {
      if (!recoverableCacheSlotError(status.error)) {
        return PdfStepResult::failure(status);
      }
      resetInPlace(state);
    } else {
      state.valid = true;
      state.sourceMatches = pdfSourceIdentityEqual(state.checkpoint.source, sourceIdentity_);
    }
    cacheSetupStage_ = CacheSetupStage::CloseCheckpointSlot;
    return PdfStepResult::paused();
  }

  if (cacheSetupStage_ == CacheSetupStage::CloseCheckpointSlot) {
    if (!budget.consumeOperation()) {
      return pauseForBudget();
    }
    const PdfStatus status = config_.io.close(config_.io.context, &cacheSetupHandle_);
    if (!status) {
      return PdfStepResult::failure(status);
    }
    const PdfBuildCheckpointSlotState& state = checkpointSelection_.slots[cacheSetupSlot_];
    if (state.valid && state.sourceMatches &&
        (!checkpointSelection_.selected ||
         pdfCacheSequenceNewer(state.checkpoint.sequence, checkpointSelection_.checkpoint.sequence))) {
      checkpointSelection_.selected = true;
      checkpointSelection_.selectedSlot = cacheSetupSlot_ == 0 ? PdfCacheSlot::A : PdfCacheSlot::B;
      checkpointSelection_.checkpoint = state.checkpoint;
    }
    if (++cacheSetupSlot_ < 2U) {
      cacheSetupStage_ = CacheSetupStage::OpenCheckpointSlot;
    } else {
      cacheSetupSlot_ = 0;
      cacheSetupStage_ = CacheSetupStage::OpenManifestSlot;
    }
    return PdfStepResult::paused();
  }

  if (cacheSetupStage_ == CacheSetupStage::OpenManifestSlot) {
    const int pathLength =
        std::snprintf(sectionPath_, sizeof(sectionPath_), "%s/%s", cacheRoot_, kManifestNames[cacheSetupSlot_]);
    if (pathLength <= 0 || static_cast<size_t>(pathLength) >= sizeof(sectionPath_)) {
      return PdfStepResult::failure(PdfStatus::failure(PdfError::LimitExceeded));
    }
    if (!budget.consumeOperation()) {
      return pauseForBudget();
    }
    PdfStatus status = config_.io.open(config_.io.context, sectionPath_, PdfCacheOpenMode::Read, &cacheSetupHandle_);
    if (!status && status.error == PdfError::InvalidOffset) {
      resetInPlace(manifestSelection_.slots[cacheSetupSlot_]);
      if (++cacheSetupSlot_ < 2U) {
        return PdfStepResult::paused();
      }
      cacheSetupSlot_ = 0;
      cacheSetupStage_ = CacheSetupStage::SelectGeneration;
      return PdfStepResult::paused();
    }
    if (!status) {
      return PdfStepResult::failure(status);
    }
    cacheSetupStage_ = CacheSetupStage::ReadManifestMetadata;
    return PdfStepResult::paused();
  }

  if (cacheSetupStage_ == CacheSetupStage::ReadManifestMetadata) {
    if (!budget.consumeOperation()) {
      return pauseForBudget();
    }
    PdfCacheFileMetadata metadata{};
    const PdfStatus status = config_.io.metadata(config_.io.context, cacheSetupHandle_, &metadata);
    if (!status) {
      return PdfStepResult::failure(status);
    }
    cacheSetupFileSize_ = metadata.size;
    if (metadata.directory || metadata.symlinkLike || metadata.size < kManifestHeaderBytes + kManifestTrailerBytes ||
        metadata.size > PDF_CACHE_MAX_SLOT_BYTES) {
      resetInPlace(manifestSelection_.slots[cacheSetupSlot_]);
      cacheSetupStage_ = CacheSetupStage::CloseManifestSlot;
      return PdfStepResult::paused();
    }
    cacheSetupOffset_ = 0;
    cacheSetupCrc32_ = 0;
    cacheSetupRecordIndex_ = 0;
    cacheSetupDecodedFileBytes_ = 0;
    cacheSetupDecodedLedger_ = PDF_CACHE_FNV64_OFFSET;
    cacheSetupStage_ = CacheSetupStage::ReadManifestHeader;
    return PdfStepResult::paused();
  }

  if (cacheSetupStage_ == CacheSetupStage::ReadManifestHeader) {
    PdfStatus status = readBytes(0, sourceWindow_.get(), kManifestHeaderBytes);
    if (!status && status.error == PdfError::BudgetExhausted) {
      return pauseForBudget();
    }
    PdfCacheManifestSlotState& state = manifestSelection_.slots[cacheSetupSlot_];
    if (status) {
      status = decodeManifestHeader(sourceWindow_.get(), &state.manifest);
    }
    if (!status) {
      if (!recoverableCacheSlotError(status.error)) {
        return PdfStepResult::failure(status);
      }
      resetInPlace(state);
      cacheSetupStage_ = CacheSetupStage::CloseManifestSlot;
      return PdfStepResult::paused();
    }
    cacheSetupCrc32_ = pdfCacheCrc32(sourceWindow_.get(), kManifestHeaderBytes);
    cacheSetupOffset_ = kManifestHeaderBytes;
    cacheSetupRecordIndex_ = 0;
    cacheSetupStage_ = state.manifest.requiredFileCount == 0 ? CacheSetupStage::ReadManifestTrailer
                                                             : CacheSetupStage::ReadManifestRecordHeader;
    return PdfStepResult::paused();
  }

  if (cacheSetupStage_ == CacheSetupStage::ReadManifestRecordHeader) {
    if (cacheSetupOffset_ > cacheSetupFileSize_ ||
        kManifestRecordHeaderBytes + kManifestTrailerBytes > cacheSetupFileSize_ - cacheSetupOffset_) {
      resetInPlace(manifestSelection_.slots[cacheSetupSlot_]);
      cacheSetupStage_ = CacheSetupStage::CloseManifestSlot;
      return PdfStepResult::paused();
    }
    PdfStatus status = readBytes(cacheSetupOffset_, sourceWindow_.get(), kManifestRecordHeaderBytes);
    if (!status && status.error == PdfError::BudgetExhausted) {
      return pauseForBudget();
    }
    if (status) {
      status = decodeManifestRecordHeader(sourceWindow_.get(), &sectionRecord_);
    }
    if (!status) {
      if (!recoverableCacheSlotError(status.error)) {
        return PdfStepResult::failure(status);
      }
      resetInPlace(manifestSelection_.slots[cacheSetupSlot_]);
      cacheSetupStage_ = CacheSetupStage::CloseManifestSlot;
      return PdfStepResult::paused();
    }
    cacheSetupCrc32_ = pdfCacheCrc32(sourceWindow_.get(), kManifestRecordHeaderBytes, cacheSetupCrc32_);
    cacheSetupOffset_ += kManifestRecordHeaderBytes;
    cacheSetupStage_ = CacheSetupStage::ReadManifestRecordPath;
    return PdfStepResult::paused();
  }

  if (cacheSetupStage_ == CacheSetupStage::ReadManifestRecordPath) {
    if (cacheSetupOffset_ > cacheSetupFileSize_ ||
        static_cast<uint64_t>(sectionRecord_.pathLength) + kManifestTrailerBytes >
            cacheSetupFileSize_ - cacheSetupOffset_) {
      resetInPlace(manifestSelection_.slots[cacheSetupSlot_]);
      cacheSetupStage_ = CacheSetupStage::CloseManifestSlot;
      return PdfStepResult::paused();
    }
    PdfStatus status = readBytes(cacheSetupOffset_, sourceWindow_.get(), sectionRecord_.pathLength);
    if (!status && status.error == PdfError::BudgetExhausted) {
      return pauseForBudget();
    }
    if (status) {
      std::memcpy(sectionRecord_.path, sourceWindow_.get(), sectionRecord_.pathLength);
      sectionRecord_.path[sectionRecord_.pathLength] = '\0';
      if (!pdfValidateCacheRelativePath(sectionRecord_.path, sectionRecord_.pathLength) ||
          cacheSetupDecodedFileBytes_ > UINT64_MAX - sectionRecord_.size) {
        status = PdfStatus::failure(PdfError::Malformed);
      }
    }
    if (!status) {
      if (!recoverableCacheSlotError(status.error)) {
        return PdfStepResult::failure(status);
      }
      resetInPlace(manifestSelection_.slots[cacheSetupSlot_]);
      cacheSetupStage_ = CacheSetupStage::CloseManifestSlot;
      return PdfStepResult::paused();
    }
    cacheSetupCrc32_ = pdfCacheCrc32(sourceWindow_.get(), sectionRecord_.pathLength, cacheSetupCrc32_);
    cacheSetupDecodedFileBytes_ += sectionRecord_.size;
    cacheSetupDecodedLedger_ = pdfUpdateRequiredFileLedger(cacheSetupDecodedLedger_, sectionRecord_);
    cacheSetupOffset_ += sectionRecord_.pathLength;
    ++cacheSetupRecordIndex_;
    const PdfCacheManifest& manifest = manifestSelection_.slots[cacheSetupSlot_].manifest;
    cacheSetupStage_ = cacheSetupRecordIndex_ < manifest.requiredFileCount ? CacheSetupStage::ReadManifestRecordHeader
                                                                           : CacheSetupStage::ReadManifestTrailer;
    return PdfStepResult::paused();
  }

  if (cacheSetupStage_ == CacheSetupStage::ReadManifestTrailer) {
    if (cacheSetupOffset_ + kManifestTrailerBytes != cacheSetupFileSize_) {
      resetInPlace(manifestSelection_.slots[cacheSetupSlot_]);
      cacheSetupStage_ = CacheSetupStage::CloseManifestSlot;
      return PdfStepResult::paused();
    }
    PdfStatus status = readBytes(cacheSetupOffset_, sourceWindow_.get(), kManifestTrailerBytes);
    if (!status && status.error == PdfError::BudgetExhausted) {
      return pauseForBudget();
    }
    PdfCacheManifestSlotState& state = manifestSelection_.slots[cacheSetupSlot_];
    if (status) {
      const uint32_t storedLength = readLe32Prep(sourceWindow_.get());
      const uint32_t calculatedCrc = pdfCacheCrc32(sourceWindow_.get(), sizeof(uint32_t), cacheSetupCrc32_);
      const uint32_t storedCrc = readLe32Prep(sourceWindow_.get() + sizeof(uint32_t));
      if (storedLength != cacheSetupFileSize_ || storedCrc != calculatedCrc ||
          cacheSetupDecodedFileBytes_ != state.manifest.requiredFileBytes ||
          cacheSetupDecodedLedger_ != state.manifest.requiredFileLedger || !state.manifest.completed) {
        status = PdfStatus::failure(PdfError::Malformed);
      }
    }
    if (!status) {
      if (!recoverableCacheSlotError(status.error)) {
        return PdfStepResult::failure(status);
      }
      resetInPlace(state);
    } else {
      state.valid = true;
      state.sourceMatches = pdfSourceIdentityEqual(state.manifest.source, sourceIdentity_);
    }
    cacheSetupStage_ = CacheSetupStage::CloseManifestSlot;
    return PdfStepResult::paused();
  }

  if (cacheSetupStage_ == CacheSetupStage::CloseManifestSlot) {
    if (!budget.consumeOperation()) {
      return pauseForBudget();
    }
    const PdfStatus status = config_.io.close(config_.io.context, &cacheSetupHandle_);
    if (!status) {
      return PdfStepResult::failure(status);
    }
    const PdfCacheManifestSlotState& state = manifestSelection_.slots[cacheSetupSlot_];
    if (state.valid && state.sourceMatches &&
        (!manifestSelection_.selected ||
         pdfCacheSequenceNewer(state.manifest.sequence, manifestSelection_.manifest.sequence))) {
      manifestSelection_.selected = true;
      manifestSelection_.selectedSlot = cacheSetupSlot_ == 0 ? PdfCacheSlot::A : PdfCacheSlot::B;
      manifestSelection_.manifest = state.manifest;
    }
    if (++cacheSetupSlot_ < 2U) {
      cacheSetupStage_ = CacheSetupStage::OpenManifestSlot;
    } else {
      cacheSetupSlot_ = 0;
      cacheSetupStage_ = CacheSetupStage::SelectGeneration;
    }
    return PdfStepResult::paused();
  }

  if (cacheSetupStage_ == CacheSetupStage::SelectGeneration) {
    sequence_ = manifestSelection_.selected ? manifestSelection_.manifest.sequence + 1U : 1U;
    if (checkpointSelection_.selected &&
        (!manifestSelection_.selected ||
         pdfCacheSequenceNewer(checkpointSelection_.checkpoint.sequence, manifestSelection_.manifest.sequence))) {
      sequence_ = checkpointSelection_.checkpoint.sequence + 1U;
    }
    const PdfBuildPhase checkpointPhase =
        checkpointSelection_.selected ? checkpointSelection_.checkpoint.phase : PdfBuildPhase::None;
    const bool checkpointResumable =
        checkpointPhase == PdfBuildPhase::Discover || checkpointPhase == PdfBuildPhase::ParsePages ||
        checkpointPhase == PdfBuildPhase::EmitSections || checkpointPhase == PdfBuildPhase::EmitImages ||
        checkpointPhase == PdfBuildPhase::Finalize;
    bool checkpointGenerationProtected = false;
    if (checkpointSelection_.selected) {
      for (const PdfCacheManifestSlotState& slot : manifestSelection_.slots) {
        checkpointGenerationProtected =
            checkpointGenerationProtected ||
            (slot.valid && slot.manifest.generation == checkpointSelection_.checkpoint.generation);
      }
    }
    if (checkpointSelection_.selected && checkpointResumable && !checkpointGenerationProtected) {
      resumedFromCheckpoint_ = true;
      generation_ = checkpointSelection_.checkpoint.generation;
    } else {
      generation_ = deterministicGeneration(sourceIdentity_);
      bool conflicts = true;
      while (conflicts) {
        conflicts = false;
        for (const PdfCacheManifestSlotState& slot : manifestSelection_.slots) {
          conflicts = conflicts || (slot.valid && slot.manifest.generation == generation_);
        }
        if (conflicts) {
          ++generation_;
          if (generation_ == 0) {
            generation_ = 1;
          }
        }
      }
    }
    if (generation_ == 0) {
      generation_ = 1;
    }
    const int pathLength = std::snprintf(sectionPath_, sizeof(sectionPath_), "%s/gen_%lu", cacheRoot_,
                                         static_cast<unsigned long>(generation_));
    if (pathLength <= 0 || static_cast<size_t>(pathLength) >= sizeof(sectionPath_)) {
      return PdfStepResult::failure(PdfStatus::failure(PdfError::LimitExceeded));
    }
    cacheSetupStage_ = CacheSetupStage::CreateCacheRoot;
    return PdfStepResult::paused();
  }

  if (cacheSetupStage_ == CacheSetupStage::CreateCacheRoot) {
    if (!budget.consumeOperation()) {
      return pauseForBudget();
    }
    const PdfStatus status = config_.io.mkdir(config_.io.context, cacheRoot_);
    if (!status) {
      return PdfStepResult::failure(status);
    }
    cacheSetupStage_ = CacheSetupStage::CreateGeneration;
    return PdfStepResult::paused();
  }

  if (cacheSetupStage_ == CacheSetupStage::CreateGeneration) {
    if (!budget.consumeOperation()) {
      return pauseForBudget();
    }
    PdfStatus status = config_.io.mkdir(config_.io.context, sectionPath_);
    if (!status) {
      return PdfStepResult::failure(status);
    }
    const int pathLength = std::snprintf(sectionPath_, sizeof(sectionPath_), "%s/gen_%lu/sections", cacheRoot_,
                                         static_cast<unsigned long>(generation_));
    if (pathLength <= 0 || static_cast<size_t>(pathLength) >= sizeof(sectionPath_)) {
      return PdfStepResult::failure(PdfStatus::failure(PdfError::LimitExceeded));
    }
    cacheSetupStage_ = CacheSetupStage::CreateSectionDirectory;
    return PdfStepResult::paused();
  }

  if (cacheSetupStage_ == CacheSetupStage::CreateSectionDirectory) {
    if (!budget.consumeOperation()) {
      return pauseForBudget();
    }
    const PdfStatus status = config_.io.mkdir(config_.io.context, sectionPath_);
    if (!status) {
      return PdfStepResult::failure(status);
    }
    cacheSetupStage_ = CacheSetupStage::ReadCapacity;
    return PdfStepResult::paused();
  }

  if (cacheSetupStage_ == CacheSetupStage::ReadCapacity) {
    if (!budget.consumeOperation()) {
      return pauseForBudget();
    }
    const PdfStatus status = config_.io.capacity(config_.io.context, &cacheCapacity_);
    if (!status) {
      return PdfStepResult::failure(status);
    }
    cacheSetupStage_ = CacheSetupStage::InitializeBudget;
    return PdfStepResult::paused();
  }

  if (cacheSetupStage_ == CacheSetupStage::InitializeBudget) {
    const PdfStatus status = pdfInitializeCacheBudget(sourceIdentity_.size, cacheCapacity_, 0, &cacheBudget_);
    if (!status || cacheBudget_.limit == 0) {
      return PdfStepResult::failure(status ? PdfStatus::failure(PdfError::InsufficientStorage) : status);
    }
    cacheSetupStage_ = CacheSetupStage::InitializeImageCache;
    return PdfStepResult::paused();
  }

  if (cacheSetupStage_ == CacheSetupStage::InitializeImageCache) {
    if (!budget.consumeOperation()) {
      return pauseForBudget();
    }
    const PdfStatus status = navigation_->imageCache.begin(
        {config_.io, cacheRoot_, generation_, &cacheBudget_, sourceWindow_.get(), PdfLimits::SourceBufferBytes,
         navigation_->imageCacheEntries, static_cast<uint8_t>(std::size(navigation_->imageCacheEntries)),
         config_.rename});
    if (!status) {
      return PdfStepResult::failure(status);
    }
    cacheSetupStage_ = CacheSetupStage::FormatOutputPaths;
    return PdfStepResult::paused();
  }

  if (cacheSetupStage_ == CacheSetupStage::FormatOutputPaths) {
    const int relativeLength = std::snprintf(sectionRelativePath_, sizeof(sectionRelativePath_),
                                             "gen_%lu/sections/000000.xhtml", static_cast<unsigned long>(generation_));
    const int fullLength = std::snprintf(sectionPath_, sizeof(sectionPath_), "%s/%s", cacheRoot_, sectionRelativePath_);
    const int metadataRelativeLength = std::snprintf(metadataRelativePath_, sizeof(metadataRelativePath_),
                                                     "gen_%lu/metadata.bin", static_cast<unsigned long>(generation_));
    const int metadataFullLength =
        std::snprintf(metadataPath_, sizeof(metadataPath_), "%s/%s", cacheRoot_, metadataRelativePath_);
    const int outlineRelativeLength = std::snprintf(outlineRelativePath_, sizeof(outlineRelativePath_),
                                                    "gen_%lu/outline.bin", static_cast<unsigned long>(generation_));
    const int outlineFullLength =
        std::snprintf(outlinePath_, sizeof(outlinePath_), "%s/%s", cacheRoot_, outlineRelativePath_);
    if (relativeLength < 0 || static_cast<size_t>(relativeLength) >= sizeof(sectionRelativePath_) || fullLength < 0 ||
        static_cast<size_t>(fullLength) >= sizeof(sectionPath_) || metadataRelativeLength < 0 ||
        static_cast<size_t>(metadataRelativeLength) >= sizeof(metadataRelativePath_) || metadataFullLength < 0 ||
        static_cast<size_t>(metadataFullLength) >= sizeof(metadataPath_) || outlineRelativeLength < 0 ||
        static_cast<size_t>(outlineRelativeLength) >= sizeof(outlineRelativePath_) || outlineFullLength < 0 ||
        static_cast<size_t>(outlineFullLength) >= sizeof(outlinePath_)) {
      return PdfStepResult::failure(PdfStatus::failure(PdfError::LimitExceeded));
    }
    cacheSetupStage_ = CacheSetupStage::ReopenSource;
    return PdfStepResult::paused();
  }

  if (cacheSetupStage_ == CacheSetupStage::ReopenSource) {
    if (budget.operationsRemaining < 2U || !budget.consumeOperation() || !budget.consumeOperation()) {
      return pauseForBudget();
    }
    const PdfStatus status = reopenSource();
    if (!status) {
      return PdfStepResult::failure(status);
    }
    cacheSetupStage_ = CacheSetupStage::Complete;
    return PdfStepResult::completed();
  }

  return cacheSetupStage_ == CacheSetupStage::Complete
             ? PdfStepResult::completed()
             : PdfStepResult::failure(PdfStatus::failure(PdfError::InvalidArgument));
}

PdfStatus PdfPreparation::startXref() {
  if (!xref_.has_value() || !sourceWindow_) {
    return PdfStatus::failure(PdfError::InvalidArgument);
  }
  xrefParser_.emplace(source(), sourceWindow_.get(), PdfLimits::SourceBufferBytes, arena_, *xref_);
  xrefParser_->begin();
  return PdfStatus::success();
}

PdfStatus PdfPreparation::finishXref() {
  PdfObjectReference root{};
  if (!xref_.has_value() || !xref_->root(&root)) {
    return PdfStatus::failure(PdfError::Malformed);
  }
  xrefParser_.reset();
  resolver_.emplace(source(), *xref_, sourceWindow_.get(), PdfLimits::SourceBufferBytes, arena_);
  return resolver_->begin(root);
}

PdfStatus PdfPreparation::finishCatalog() {
  if (!resolver_.has_value() || navigation_ == nullptr) {
    return PdfStatus::failure(PdfError::InvalidArgument);
  }
  PdfStatus status = pdfReadCatalogNavigation(arena_, resolver_->result().rootIndex, &catalogNavigation_);
  if (!status) {
    return status;
  }
  status = pdfApplyCatalogMetadata(catalogNavigation_, &metadataBuilder_);
  if (!status) {
    return status;
  }
  hasInfoReference_ = xref_.has_value() && xref_->info(&infoReference_);
  pageCount_ = 0;
  pageWalker_.emplace(*resolver_, arena_, recordStore(traversalRecords_), capturePage, this, &navigation_->pageScratch);
  return pageWalker_->begin(catalogNavigation_.pages);
}

PdfStatus PdfPreparation::finishPageTree() {
  pageWalker_.reset();
  if (!resolver_.has_value() || navigation_ == nullptr || pageCount_ == 0) {
    return PdfStatus::failure(PdfError::NoReadableText);
  }
  return beginNavigationDiscovery();
}

PdfStatus PdfPreparation::beginNavigationDiscovery() {
  if (navigation_ == nullptr || !resolver_.has_value()) {
    return PdfStatus::failure(PdfError::InvalidArgument);
  }
  namedDestinations_.emplace(PdfNamedDestinationWorkspace{
      navigation_->namedDestinations,
      static_cast<uint8_t>(std::size(navigation_->namedDestinations)),
  });
  pageLabels_.emplace(PdfPageLabelWorkspace{
      navigation_->pageLabels,
      static_cast<uint8_t>(std::size(navigation_->pageLabels)),
  });
  outlineBuilder_.emplace(
      PdfOutlineWorkspace{navigation_->outlineEntries, static_cast<uint16_t>(std::size(navigation_->outlineEntries))});
  PdfStatus status = namedDestinations_->begin();
  if (status) {
    status = pageLabels_->begin();
  }
  if (status) {
    status = outlineBuilder_->begin();
  }
  if (!status) {
    return status;
  }
  navigationStage_ = 0;
  navigationTask_ = NavigationTask::None;
  outlinePendingCount_ = 0;
  outlineSeenCount_ = 0;
  explicitOutlineCount_ = 0;
  currentAnnotationPage_ = 0;
  currentAnnotationIndex_ = 0;
  navigation_->linkCount = 0;
  return startNextNavigationObject();
}

PdfStatus PdfPreparation::startNextNavigationObject() {
  if (!resolver_.has_value() || navigation_ == nullptr) {
    return PdfStatus::failure(PdfError::InvalidArgument);
  }
  while (true) {
    switch (navigationStage_) {
      case 0:
        ++navigationStage_;
        if (hasInfoReference_) {
          navigationTask_ = NavigationTask::Info;
          activeNavigationReference_ = infoReference_;
          return resolver_->begin(activeNavigationReference_);
        }
        break;
      case 1:
        ++navigationStage_;
        if (catalogNavigation_.hasMetadata) {
          navigationTask_ = NavigationTask::Xmp;
          activeNavigationReference_ = catalogNavigation_.metadata;
          return resolver_->begin(activeNavigationReference_);
        }
        break;
      case 2:
        ++navigationStage_;
        if (catalogNavigation_.hasNamedDestinations) {
          navigationTask_ = NavigationTask::NamedDestinations;
          activeNavigationReference_ = catalogNavigation_.namedDestinations;
          return resolver_->begin(activeNavigationReference_);
        }
        break;
      case 3:
        ++navigationStage_;
        if (catalogNavigation_.hasPageLabels) {
          navigationTask_ = NavigationTask::PageLabels;
          activeNavigationReference_ = catalogNavigation_.pageLabels;
          return resolver_->begin(activeNavigationReference_);
        }
        break;
      case 4:
        ++navigationStage_;
        if (catalogNavigation_.hasOutlines) {
          navigationTask_ = NavigationTask::OutlineRoot;
          activeNavigationReference_ = catalogNavigation_.outlines;
          return resolver_->begin(activeNavigationReference_);
        }
        break;
      case 5:
        if (outlinePendingCount_ != 0) {
          const PreparedOutlinePending pending = navigation_->outlinePending[--outlinePendingCount_];
          navigationTask_ = NavigationTask::OutlineNode;
          activeNavigationReference_ = pending.reference;
          currentOutlineParent_ = pending.parentIndex;
          return resolver_->begin(activeNavigationReference_);
        }
        ++navigationStage_;
        break;
      case 6:
        while (currentAnnotationPage_ < pageCount_) {
          const PdfPageInfo& page = navigation_->pages[currentAnnotationPage_];
          if (currentAnnotationIndex_ < page.annotationCount) {
            navigationTask_ = NavigationTask::Annotation;
            activeNavigationReference_ = page.annotations[currentAnnotationIndex_++];
            return resolver_->begin(activeNavigationReference_);
          }
          ++currentAnnotationPage_;
          currentAnnotationIndex_ = 0;
        }
        ++navigationStage_;
        break;
      default:
        navigationTask_ = NavigationTask::Complete;
        return PdfStatus::success();
    }
  }
}

PdfStatus PdfPreparation::resolveDestination(const PdfRawDestination& raw,
                                             PdfResolvedDestination* const destination) const {
  if (destination == nullptr || navigation_ == nullptr) {
    return PdfStatus::failure(PdfError::InvalidArgument);
  }
  *destination = {};
  PdfRawDestination explicitDestination = raw;
  if (raw.kind == PdfRawDestinationKind::Named) {
    if (!namedDestinations_.has_value()) {
      return PdfStatus::failure(PdfError::InvalidOffset);
    }
    const PdfStatus status =
        namedDestinations_->resolve(reinterpret_cast<const uint8_t*>(raw.name), raw.nameLength, &explicitDestination);
    if (!status) {
      return status;
    }
  }
  if (explicitDestination.kind != PdfRawDestinationKind::Explicit) {
    return PdfStatus::failure(PdfError::InvalidOffset);
  }
  for (uint16_t page = 0; page < pageCount_; ++page) {
    if (navigation_->pages[page].pageReference == explicitDestination.pageReference) {
      destination->sectionIndex =
          navigation_->pageFirstSections[page] == UINT16_MAX ? page : navigation_->pageFirstSections[page];
      destination->sourcePageIndex = page;
      destination->anchorOrdinal =
          navigation_->pageFirstAnchors[page] == UINT32_MAX ? 0 : navigation_->pageFirstAnchors[page];
      destination->resolved = true;
      return PdfStatus::success();
    }
  }
  return PdfStatus::failure(PdfError::InvalidOffset, explicitDestination.pageReference.objectNumber);
}

PdfStatus PdfPreparation::finishNavigationObject() {
  if (!resolver_.has_value() || navigation_ == nullptr || !outlineBuilder_.has_value()) {
    return PdfStatus::failure(PdfError::InvalidArgument);
  }
  const PdfResolvedObject& resolved = resolver_->result();
  PdfStatus status = PdfStatus::success();
  switch (navigationTask_) {
    case NavigationTask::Info:
      status = pdfApplyInfoMetadata(arena_, resolved.rootIndex, &metadataBuilder_);
      break;
    case NavigationTask::Xmp:
      if (!resolved.hasStream || resolved.streamLength > PdfLimits::DecoderOutputBytes) {
        return PdfStatus::failure(PdfError::LimitExceeded, resolved.streamLength);
      }
      xmpStreamOffset_ = resolved.streamOffset;
      xmpStreamLength_ = resolved.streamLength;
      return PdfStatus::success();
    case NavigationTask::NamedDestinations:
      status = pdfReadNamedDestinations(arena_, resolved.rootIndex, &*namedDestinations_);
      break;
    case NavigationTask::PageLabels:
      status = pdfReadPageLabels(arena_, resolved.rootIndex, &*pageLabels_);
      break;
    case NavigationTask::OutlineRoot: {
      PdfObjectReference first{};
      status = pdfReadOutlineRoot(arena_, resolved.rootIndex, &first);
      if (status) {
        navigation_->outlinePending[outlinePendingCount_++] = {first, -1};
      }
      break;
    }
    case NavigationTask::OutlineNode: {
      for (uint16_t index = 0; index < outlineSeenCount_; ++index) {
        if (navigation_->outlineSeen[index] == activeNavigationReference_) {
          return PdfStatus::failure(PdfError::Malformed, activeNavigationReference_.objectNumber);
        }
      }
      if (outlineSeenCount_ >= kPreparationOutlineLimit) {
        return PdfStatus::failure(PdfError::LimitExceeded, activeNavigationReference_.objectNumber);
      }
      navigation_->outlineSeen[outlineSeenCount_++] = activeNavigationReference_;
      PdfRawOutlineNode& node = navigation_->outlineNodeScratch;
      resetInPlace(node);
      status = pdfReadOutlineNode(arena_, resolved.rootIndex, &node);
      if (!status) {
        break;
      }
      PdfResolvedDestination destination{};
      const PdfStatus destinationStatus = resolveDestination(node.destination, &destination);
      int16_t childParent = currentOutlineParent_;
      if (destinationStatus) {
        const PdfOutlineCandidate candidate{
            activeNavigationReference_,
            currentOutlineParent_,
            destination,
            reinterpret_cast<const uint8_t*>(node.title),
            node.titleLength,
        };
        status = outlineBuilder_->append(candidate);
        if (!status) {
          break;
        }
        childParent = static_cast<int16_t>(outlineBuilder_->count() - 1);
        explicitOutlineCount_ = outlineBuilder_->count();
      }
      const uint16_t needed = static_cast<uint16_t>((node.hasNext ? 1 : 0) + (node.hasFirstChild ? 1 : 0));
      if (outlinePendingCount_ > kPreparationOutlineLimit - needed) {
        return PdfStatus::failure(PdfError::LimitExceeded, activeNavigationReference_.objectNumber);
      }
      if (node.hasNext) {
        navigation_->outlinePending[outlinePendingCount_++] = {node.next, currentOutlineParent_};
      }
      if (node.hasFirstChild) {
        navigation_->outlinePending[outlinePendingCount_++] = {node.firstChild, childParent};
      }
      break;
    }
    case NavigationTask::Annotation: {
      PdfRawLinkAnnotation& annotation = navigation_->linkAnnotationScratch;
      resetInPlace(annotation);
      status = pdfReadLinkAnnotation(arena_, resolved.rootIndex, &annotation);
      if (!status && status.error == PdfError::Unsupported) {
        status = PdfStatus::success();
      } else if (status && annotation.action == PdfActionKind::GoTo &&
                 annotation.destination.kind != PdfRawDestinationKind::None) {
        if (navigation_->linkCount >= kPreparationLinkLimit) {
          return PdfStatus::failure(PdfError::LimitExceeded, activeNavigationReference_.objectNumber);
        }
        navigation_->links[navigation_->linkCount++] = {annotation.destination, currentAnnotationPage_};
      }
      break;
    }
    case NavigationTask::None:
    case NavigationTask::Complete:
    default:
      return PdfStatus::failure(PdfError::InvalidArgument);
  }
  if (!status) {
    return status;
  }
  navigationTask_ = NavigationTask::None;
  return startNextNavigationObject();
}

PdfStatus PdfPreparation::readXmpMetadata() {
  if (!decoderOutput_ || xmpStreamLength_ > PdfLimits::DecoderOutputBytes) {
    return PdfStatus::failure(PdfError::LimitExceeded, xmpStreamLength_);
  }
  size_t bytesRead = 0;
  PdfStatus status = source().readAt(source().context, xmpStreamOffset_, decoderOutput_.get(),
                                     static_cast<size_t>(xmpStreamLength_), &bytesRead);
  if (!status) {
    return status;
  }
  if (bytesRead != xmpStreamLength_) {
    return PdfStatus::failure(PdfError::UnexpectedEof, xmpStreamOffset_ + bytesRead);
  }
  status = pdfApplyXmpMetadata(decoderOutput_.get(), bytesRead, &metadataBuilder_);
  if (!status) {
    return status;
  }
  navigationTask_ = NavigationTask::None;
  return startNextNavigationObject();
}

PdfStatus PdfPreparation::beginCurrentPageImages() {
  if (!resolver_.has_value() || navigation_ == nullptr || currentPageIndex_ >= pageCount_) {
    return PdfStatus::failure(PdfError::InvalidArgument, currentPageIndex_);
  }
  imageResolveTask_ = ImageResolveTask::None;
  imageCandidateCount_ = 0;
  for (auto& candidate : navigation_->imageCandidates) {
    resetInPlace(candidate);
  }
  currentPageImageStart_ = 0;
  currentPageImageEnd_ = 0;
  imageResolveIndex_ = 0;
  imagePaletteCount_ = 0;
  currentPageImageCandidate_ = -1;
  lastContentNameLength_ = 0;
  std::memset(lastContentName_, 0, sizeof(lastContentName_));

  const PdfPageInfo& page = navigation_->pages[currentPageIndex_];
  if (!page.hasResources) {
    return beginCurrentPageContent();
  }
  const PdfObjectReference reference = page.resourcesIndirect ? page.resourceReference : page.resourceOwner;
  if (reference.objectNumber == 0) {
    return PdfStatus::failure(PdfError::Malformed, currentPageIndex_);
  }
  imageResolveTask_ = ImageResolveTask::ResourceOwner;
  return resolver_->begin(reference);
}

PdfStatus PdfPreparation::collectImageCandidates(const uint16_t dictionaryIndex) {
  if (navigation_ == nullptr || dictionaryIndex >= arena_.valueCount ||
      arena_.values[dictionaryIndex].kind != PdfValueKind::Dictionary) {
    return PdfStatus::failure(PdfError::Malformed, dictionaryIndex);
  }
  const PdfValue& dictionary = arena_.values[dictionaryIndex];
  uint16_t entryIndex = dictionary.firstLink;
  for (uint16_t visited = 0; visited < dictionary.count; ++visited) {
    if (entryIndex >= arena_.dictionaryCount) {
      return PdfStatus::failure(PdfError::Malformed, entryIndex);
    }
    const PdfDictionaryEntry& entry = arena_.dictionaryEntries[entryIndex];
    if (entry.valueIndex >= arena_.valueCount ||
        static_cast<uint32_t>(entry.keyOffset) + entry.keyLength > arena_.textLength) {
      return PdfStatus::failure(PdfError::Malformed, entryIndex);
    }
    const PdfValue& value = arena_.values[entry.valueIndex];
    if (value.kind == PdfValueKind::Reference && entry.keyLength != 0 && entry.keyLength < kPreparationImageNameBytes &&
        imageCandidateCount_ < kPreparationPageImageLimit) {
      PreparedImageCandidate& candidate = navigation_->imageCandidates[imageCandidateCount_++];
      candidate.reference = {value.objectNumber, value.generation};
      bool repetitionRecorded = false;
      for (uint8_t index = 0; index < imageRepetitionEntryCount_; ++index) {
        if (imageRepetitionReferences_[index] == candidate.reference) {
          if (imageRepetitionCounts_[index] != UINT8_MAX) {
            ++imageRepetitionCounts_[index];
          }
          candidate.documentRepetitionCount = imageRepetitionCounts_[index];
          repetitionRecorded = true;
          break;
        }
      }
      if (!repetitionRecorded && imageRepetitionEntryCount_ < kPreparationImageRepetitionLimit) {
        imageRepetitionReferences_[imageRepetitionEntryCount_] = candidate.reference;
        imageRepetitionCounts_[imageRepetitionEntryCount_] = 1;
        candidate.documentRepetitionCount = 1;
        ++imageRepetitionEntryCount_;
      }
      candidate.nameLength = static_cast<uint8_t>(entry.keyLength);
      std::memcpy(candidate.name, arena_.text + entry.keyOffset, entry.keyLength);
      candidate.name[entry.keyLength] = '\0';
    } else if (value.kind == PdfValueKind::Reference && entry.keyLength != 0 &&
               entry.keyLength < kPreparationImageNameBytes) {
      warningFlags_ |= kWarningOptionalImageOmitted;
    }
    entryIndex = entry.next;
  }
  imageResolveIndex_ = 0;
  currentPageImageEnd_ = imageCandidateCount_;
  imageResolveIndex_ = currentPageImageStart_;
  return PdfStatus::success();
}

PdfStatus PdfPreparation::beginNextImageObject() {
  if (!resolver_.has_value() || navigation_ == nullptr) {
    return PdfStatus::failure(PdfError::InvalidArgument);
  }
  if (imageResolveIndex_ >= currentPageImageEnd_) {
    imageResolveTask_ = ImageResolveTask::None;
    return beginCurrentPageContent();
  }
  imageResolveTask_ = ImageResolveTask::ImageObject;
  return resolver_->begin(navigation_->imageCandidates[imageResolveIndex_].reference);
}

PdfStatus PdfPreparation::finishImageResolution() {
  if (!resolver_.has_value() || navigation_ == nullptr) {
    return PdfStatus::failure(PdfError::InvalidArgument);
  }
  const PdfResolvedObject& resolved = resolver_->result();
  switch (imageResolveTask_) {
    case ImageResolveTask::ResourceOwner: {
      const PdfPageInfo& page = navigation_->pages[currentPageIndex_];
      uint16_t resourcesIndex = resolved.rootIndex;
      if (!page.resourcesIndirect && !pdfDictionaryFind(arena_, resolved.rootIndex, "Resources", &resourcesIndex)) {
        return PdfStatus::failure(PdfError::Malformed, resolved.reference.objectNumber);
      }
      uint16_t xObjectIndex = PDF_INVALID_INDEX;
      if (!pdfDictionaryFind(arena_, resourcesIndex, "XObject", &xObjectIndex)) {
        imageResolveTask_ = ImageResolveTask::None;
        return beginCurrentPageContent();
      }
      if (xObjectIndex >= arena_.valueCount) {
        return PdfStatus::failure(PdfError::Malformed, xObjectIndex);
      }
      const PdfValue& xObject = arena_.values[xObjectIndex];
      if (xObject.kind == PdfValueKind::Reference) {
        imageResolveTask_ = ImageResolveTask::XObjectDictionary;
        return resolver_->begin({xObject.objectNumber, xObject.generation});
      }
      PdfStatus status = collectImageCandidates(xObjectIndex);
      return status ? beginNextImageObject() : status;
    }
    case ImageResolveTask::XObjectDictionary: {
      PdfStatus status = collectImageCandidates(resolved.rootIndex);
      return status ? beginNextImageObject() : status;
    }
    case ImageResolveTask::ImageObject: {
      if (imageResolveIndex_ >= imageCandidateCount_) {
        return PdfStatus::failure(PdfError::Malformed, imageResolveIndex_);
      }
      uint16_t subtypeIndex = PDF_INVALID_INDEX;
      if (resolved.rootIndex >= arena_.valueCount ||
          arena_.values[resolved.rootIndex].kind != PdfValueKind::Dictionary ||
          !pdfDictionaryFind(arena_, resolved.rootIndex, "Subtype", &subtypeIndex) ||
          subtypeIndex >= arena_.valueCount || arena_.values[subtypeIndex].kind != PdfValueKind::Name) {
        warningFlags_ |= kWarningOptionalImageOmitted;
        ++imageResolveIndex_;
        return beginNextImageObject();
      }
      const PdfValue& subtype = arena_.values[subtypeIndex];
      if (pdfTextEquals(arena_, subtype, "Form")) {
        ++imageResolveIndex_;
        return beginNextImageObject();
      }
      if (!pdfTextEquals(arena_, subtype, "Image")) {
        warningFlags_ |= kWarningOptionalImageOmitted;
        ++imageResolveIndex_;
        return beginNextImageObject();
      }
      PdfImageObjectDescriptor& descriptor = navigation_->imageDescriptorScratch;
      descriptor = {};
      const PdfImageObjectParseInput input{
          resolved.rootIndex,
          {resolved.streamOffset, resolved.streamLength, sourceMetadata_.size},
          nullptr,
          0,
      };
      PdfStatus status = pdfParseImageObject(arena_, input, &descriptor);
      if (!status && status.error == PdfError::InsufficientMemory && status.offset <= kPreparationPaletteBytes) {
        uint8_t* palette = nullptr;
        status = allocateImagePalette(&palette);
        if (status) {
          descriptor = {};
          status = pdfParseImageObject(arena_,
                                       {resolved.rootIndex,
                                        {resolved.streamOffset, resolved.streamLength, sourceMetadata_.size},
                                        palette,
                                        kPreparationPaletteBytes},
                                       &descriptor);
        }
      }
      if (!status) {
        if (status.error == PdfError::LimitExceeded) {
          warningFlags_ |= kWarningOptionalImageOmitted;
          ++imageResolveIndex_;
          return beginNextImageObject();
        }
        return status;
      }
      return continueImageDescriptorResolution();
    }
    case ImageResolveTask::ColorSpace:
      return finishResolvedImageColorSpace(false);
    case ImageResolveTask::IndexedBaseColorSpace:
      return finishResolvedImageColorSpace(true);
    case ImageResolveTask::IndexedPalette:
      return finishResolvedImagePalette();
    case ImageResolveTask::AuxiliaryImageObject:
      return finishAuxiliaryImageResolution();
    case ImageResolveTask::None:
    default:
      return PdfStatus::failure(PdfError::InvalidArgument);
  }
}

PdfStatus PdfPreparation::allocateImagePalette(uint8_t** const palette) {
  if (palette == nullptr || !decoderOutput_ || imagePaletteCount_ >= kPreparationPaletteSlots) {
    return PdfStatus::failure(PdfError::LimitExceeded, imagePaletteCount_);
  }
  *palette = decoderOutput_.get() + kPreparationBlockWorkspaceBytes +
             static_cast<size_t>(imagePaletteCount_) * kPreparationPaletteBytes;
  std::memset(*palette, 0, kPreparationPaletteBytes);
  ++imagePaletteCount_;
  return PdfStatus::success();
}

PdfStatus PdfPreparation::continueImageDescriptorResolution() {
  if (!resolver_.has_value() || navigation_ == nullptr || imageResolveIndex_ >= currentPageImageEnd_) {
    return PdfStatus::failure(PdfError::InvalidArgument, imageResolveIndex_);
  }
  PdfImageObjectDescriptor& descriptor = navigation_->imageDescriptorScratch;
  PreparedImageCandidate& candidate = navigation_->imageCandidates[imageResolveIndex_];
  if (descriptor.disposition == PdfImageDisposition::OmitUnsupported) {
    warningFlags_ |= kWarningOptionalImageOmitted;
    ++imageResolveIndex_;
    return beginNextImageObject();
  }
  if (descriptor.disposition == PdfImageDisposition::NeedsResolution) {
    if (pdfImageHasUnresolved(descriptor.unresolved, PdfImageUnresolved::ColorSpace)) {
      imageResolveTask_ = ImageResolveTask::ColorSpace;
      return resolver_->begin(descriptor.colorSpaceReference);
    }
    if (pdfImageHasUnresolved(descriptor.unresolved, PdfImageUnresolved::IndexedBaseColorSpace)) {
      imageResolveTask_ = ImageResolveTask::IndexedBaseColorSpace;
      return resolver_->begin(descriptor.indexedBaseColorSpaceReference);
    }
    if (pdfImageHasUnresolved(descriptor.unresolved, PdfImageUnresolved::IndexedPalette)) {
      if (descriptor.parameters.palette == nullptr) {
        uint8_t* palette = nullptr;
        const PdfStatus status = allocateImagePalette(&palette);
        if (!status) {
          warningFlags_ |= kWarningOptionalImageOmitted;
          ++imageResolveIndex_;
          return beginNextImageObject();
        }
        descriptor.parameters.palette = palette;
      }
      imageResolveTask_ = ImageResolveTask::IndexedPalette;
      return resolver_->begin(descriptor.paletteReference);
    }
  }

  candidate.streamOffset = descriptor.stream.offset;
  candidate.streamLength = descriptor.stream.length;
  candidate.width = descriptor.parameters.width;
  candidate.height = descriptor.parameters.height;
  candidate.parameters = descriptor.parameters;
  candidate.filterCount = descriptor.stream.decoderFilterCount;
  std::copy_n(descriptor.stream.decoderFilters, candidate.filterCount, candidate.filters);
  if (descriptor.disposition == PdfImageDisposition::NeedsResolution) {
    const bool explicitOnly = descriptor.unresolved == PdfImageUnresolved::ExplicitMask && descriptor.hasExplicitMask;
    const bool softOnly = descriptor.unresolved == PdfImageUnresolved::SoftMask && descriptor.hasSoftMaskReference;
    if (explicitOnly || softOnly) {
      candidate.auxiliaryReference = explicitOnly ? descriptor.explicitMaskReference : descriptor.softMaskReference;
      candidate.auxiliaryKind = explicitOnly ? PdfImageAuxiliaryKind::ExplicitMask : PdfImageAuxiliaryKind::SoftMask;
      if (candidate.auxiliaryReference == candidate.reference) {
        warningFlags_ |= kWarningOptionalImageOmitted;
        ++imageResolveIndex_;
        return beginNextImageObject();
      }
      imageResolveTask_ = ImageResolveTask::AuxiliaryImageObject;
      return resolver_->begin(candidate.auxiliaryReference);
    }
    warningFlags_ |= kWarningOptionalImageOmitted;
  } else if (descriptor.stream.terminalCodec == PdfImageTerminalCodec::DctJpeg &&
             descriptor.stream.target == PdfImageStreamTarget::JpegBytes && descriptor.parameters.width != 0 &&
             descriptor.parameters.height != 0 && descriptor.parameters.width <= UINT16_MAX &&
             descriptor.parameters.height <= UINT16_MAX) {
    candidate.jpeg = true;
  } else if (descriptor.stream.terminalCodec == PdfImageTerminalCodec::Raster &&
             descriptor.stream.target == PdfImageStreamTarget::ExtractorDecoded && descriptor.parameters.width != 0 &&
             descriptor.parameters.height != 0) {
    candidate.raster = true;
  }
  ++imageResolveIndex_;
  return beginNextImageObject();
}

PdfStatus PdfPreparation::finishResolvedImageColorSpace(const bool indexedBase) {
  if (!resolver_.has_value() || navigation_ == nullptr || imageResolveIndex_ >= currentPageImageEnd_) {
    return PdfStatus::failure(PdfError::InvalidArgument, imageResolveIndex_);
  }
  PdfImageObjectDescriptor& descriptor = navigation_->imageDescriptorScratch;
  const PdfResolvedObject& resolved = resolver_->result();
  if (resolved.rootIndex >= arena_.valueCount) {
    return PdfStatus::failure(PdfError::Malformed, resolved.rootIndex);
  }
  const PdfValue& root = arena_.values[resolved.rootIndex];
  auto deviceColorSpace = [&]() -> PdfImageColorSpace {
    if (root.kind == PdfValueKind::Name && pdfTextEquals(arena_, root, "DeviceGray")) {
      return PdfImageColorSpace::Gray;
    }
    if (root.kind == PdfValueKind::Name && pdfTextEquals(arena_, root, "DeviceRGB")) {
      return PdfImageColorSpace::RGB;
    }
    return PdfImageColorSpace::ImageMask;
  };
  if (indexedBase || root.kind == PdfValueKind::Name) {
    const PdfImageColorSpace colorSpace = deviceColorSpace();
    if (colorSpace == PdfImageColorSpace::ImageMask) {
      return PdfStatus::failure(PdfError::UnsupportedEncoding, resolved.rootIndex);
    }
    const PdfStatus status = pdfApplyResolvedImageColorSpace(&descriptor, colorSpace);
    return status ? continueImageDescriptorResolution() : status;
  }
  if (root.kind != PdfValueKind::Array || root.count != 4) {
    return PdfStatus::failure(PdfError::UnsupportedEncoding, resolved.rootIndex);
  }
  uint16_t familyIndex = PDF_INVALID_INDEX;
  uint16_t baseIndex = PDF_INVALID_INDEX;
  uint16_t highIndex = PDF_INVALID_INDEX;
  uint16_t paletteIndex = PDF_INVALID_INDEX;
  if (!pdfArrayAt(arena_, resolved.rootIndex, 0, &familyIndex) ||
      !pdfArrayAt(arena_, resolved.rootIndex, 1, &baseIndex) ||
      !pdfArrayAt(arena_, resolved.rootIndex, 2, &highIndex) ||
      !pdfArrayAt(arena_, resolved.rootIndex, 3, &paletteIndex) || familyIndex >= arena_.valueCount ||
      baseIndex >= arena_.valueCount || highIndex >= arena_.valueCount || paletteIndex >= arena_.valueCount ||
      arena_.values[familyIndex].kind != PdfValueKind::Name ||
      !pdfTextEquals(arena_, arena_.values[familyIndex], "Indexed")) {
    return PdfStatus::failure(PdfError::Malformed, resolved.rootIndex);
  }
  const PdfValue& high = arena_.values[highIndex];
  if (high.kind != PdfValueKind::Integer || high.integerValue < 0 || high.integerValue > 255) {
    return PdfStatus::failure(PdfError::Malformed, highIndex);
  }
  descriptor.parameters.paletteEntries = static_cast<uint16_t>(high.integerValue + 1);
  uint8_t components = 0;
  const PdfValue& base = arena_.values[baseIndex];
  if (base.kind == PdfValueKind::Reference) {
    descriptor.indexedBaseColorSpaceReference = {base.objectNumber, base.generation};
    descriptor.unresolved = descriptor.unresolved | PdfImageUnresolved::IndexedBaseColorSpace;
  } else if (base.kind == PdfValueKind::Name && pdfTextEquals(arena_, base, "DeviceGray")) {
    descriptor.parameters.colorSpace = PdfImageColorSpace::IndexedGray;
    components = 1;
  } else if (base.kind == PdfValueKind::Name && pdfTextEquals(arena_, base, "DeviceRGB")) {
    descriptor.parameters.colorSpace = PdfImageColorSpace::IndexedRGB;
    components = 3;
  } else {
    return PdfStatus::failure(PdfError::UnsupportedEncoding, baseIndex);
  }
  if (components != 0) {
    descriptor.paletteBytesRequired = static_cast<uint16_t>(descriptor.parameters.paletteEntries * components);
  }
  const PdfValue& paletteValue = arena_.values[paletteIndex];
  if (paletteValue.kind == PdfValueKind::Reference) {
    descriptor.paletteReference = {paletteValue.objectNumber, paletteValue.generation};
    descriptor.unresolved = descriptor.unresolved | PdfImageUnresolved::IndexedPalette;
  } else if (paletteValue.kind == PdfValueKind::String) {
    const size_t required = components == 0 ? paletteValue.textLength : descriptor.paletteBytesRequired;
    const size_t minimum = descriptor.parameters.paletteEntries;
    const size_t maximum = static_cast<size_t>(descriptor.parameters.paletteEntries) * 3U;
    if (required < minimum || required > maximum || paletteValue.textLength < required ||
        static_cast<uint32_t>(paletteValue.textOffset) + required > arena_.textLength) {
      return PdfStatus::failure(PdfError::Malformed, paletteIndex);
    }
    uint8_t* palette = nullptr;
    PdfStatus status = allocateImagePalette(&palette);
    if (!status) {
      return status;
    }
    std::memcpy(palette, arena_.text + paletteValue.textOffset, required);
    descriptor.parameters.palette = palette;
    descriptor.parameters.paletteBytes = static_cast<uint16_t>(required);
  } else {
    return PdfStatus::failure(PdfError::Malformed, paletteIndex);
  }
  descriptor.unresolved = static_cast<PdfImageUnresolved>(static_cast<uint8_t>(descriptor.unresolved) &
                                                          ~static_cast<uint8_t>(PdfImageUnresolved::ColorSpace));
  descriptor.colorSpaceReference = {};
  descriptor.disposition = descriptor.unresolved == PdfImageUnresolved::None ? PdfImageDisposition::Ready
                                                                             : PdfImageDisposition::NeedsResolution;
  return continueImageDescriptorResolution();
}

PdfStatus PdfPreparation::finishResolvedImagePalette() {
  if (!resolver_.has_value() || navigation_ == nullptr || imageResolveIndex_ >= currentPageImageEnd_) {
    return PdfStatus::failure(PdfError::InvalidArgument, imageResolveIndex_);
  }
  PdfImageObjectDescriptor& descriptor = navigation_->imageDescriptorScratch;
  const PdfResolvedObject& resolved = resolver_->result();
  const size_t required = descriptor.paletteBytesRequired;
  if (required == 0 || required > kPreparationPaletteBytes || descriptor.parameters.palette == nullptr ||
      resolved.rootIndex >= arena_.valueCount) {
    return PdfStatus::failure(PdfError::Malformed, required);
  }
  const PdfValue& root = arena_.values[resolved.rootIndex];
  PdfStatus status = PdfStatus::success();
  if (root.kind == PdfValueKind::String) {
    if (root.textLength < required || static_cast<uint32_t>(root.textOffset) + required > arena_.textLength) {
      return PdfStatus::failure(PdfError::Malformed, resolved.rootIndex);
    }
    std::memcpy(const_cast<uint8_t*>(descriptor.parameters.palette), arena_.text + root.textOffset, required);
  } else if (resolved.hasStream && root.kind == PdfValueKind::Dictionary) {
    uint16_t filterIndex = PDF_INVALID_INDEX;
    if (pdfDictionaryFind(arena_, resolved.rootIndex, "Filter", &filterIndex) || resolved.streamLength < required) {
      return PdfStatus::failure(PdfError::UnsupportedEncoding, resolved.rootIndex);
    }
    size_t bytesRead = 0;
    status = source().readAt(source().context, resolved.streamOffset,
                             const_cast<uint8_t*>(descriptor.parameters.palette), required, &bytesRead);
    if (status && bytesRead != required) {
      status = PdfStatus::failure(PdfError::UnexpectedEof, resolved.streamOffset + bytesRead);
    }
  } else {
    return PdfStatus::failure(PdfError::Malformed, resolved.rootIndex);
  }
  if (!status) {
    return status;
  }
  descriptor.parameters.paletteBytes = static_cast<uint16_t>(required);
  descriptor.unresolved = static_cast<PdfImageUnresolved>(static_cast<uint8_t>(descriptor.unresolved) &
                                                          ~static_cast<uint8_t>(PdfImageUnresolved::IndexedPalette));
  descriptor.paletteReference = {};
  descriptor.disposition = descriptor.unresolved == PdfImageUnresolved::None ? PdfImageDisposition::Ready
                                                                             : PdfImageDisposition::NeedsResolution;
  return continueImageDescriptorResolution();
}

PdfStatus PdfPreparation::finishAuxiliaryImageResolution() {
  if (!resolver_.has_value() || navigation_ == nullptr || imageResolveIndex_ >= currentPageImageEnd_) {
    return PdfStatus::failure(PdfError::Malformed, imageResolveIndex_);
  }
  const PdfResolvedObject& resolved = resolver_->result();
  PreparedImageCandidate& candidate = navigation_->imageCandidates[imageResolveIndex_];
  PdfImageObjectDescriptor& auxiliary = navigation_->imageDescriptorScratch;
  auxiliary = {};
  PdfStatus status = pdfParseImageObject(
      arena_, {resolved.rootIndex, {resolved.streamOffset, resolved.streamLength, sourceMetadata_.size}, nullptr, 0},
      &auxiliary);
  if (!status) {
    return status;
  }
  PdfImageObjectDescriptor& base = navigation_->baseDescriptorScratch;
  base = {};
  base.parameters = candidate.parameters;
  base.stream.offset = candidate.streamOffset;
  base.stream.length = candidate.streamLength;
  base.stream.terminalCodec = PdfImageTerminalCodec::Raster;
  base.stream.target = PdfImageStreamTarget::ExtractorDecoded;
  base.disposition = PdfImageDisposition::NeedsResolution;
  if (candidate.auxiliaryKind == PdfImageAuxiliaryKind::ExplicitMask) {
    base.unresolved = PdfImageUnresolved::ExplicitMask;
    base.hasExplicitMask = true;
  } else {
    base.unresolved = PdfImageUnresolved::SoftMask;
    base.hasSoftMaskReference = true;
  }
  status = pdfApplyResolvedImageAuxiliary(&base, auxiliary, candidate.auxiliaryKind);
  if (!status) {
    warningFlags_ |= kWarningOptionalImageOmitted;
    ++imageResolveIndex_;
    return beginNextImageObject();
  }
  if (base.disposition == PdfImageDisposition::Ready && auxiliary.disposition == PdfImageDisposition::Ready &&
      auxiliary.stream.terminalCodec == PdfImageTerminalCodec::Raster) {
    candidate.parameters = base.parameters;
    candidate.parameters.hasSoftMask = true;
    candidate.auxiliaryStreamOffset = auxiliary.stream.offset;
    candidate.auxiliaryStreamLength = auxiliary.stream.length;
    candidate.auxiliaryParameters = auxiliary.parameters;
    candidate.auxiliaryFilterCount = auxiliary.stream.decoderFilterCount;
    std::copy_n(auxiliary.stream.decoderFilters, candidate.auxiliaryFilterCount, candidate.auxiliaryFilters);
    candidate.hasAuxiliary = true;
    candidate.raster = true;
  } else {
    warningFlags_ |= kWarningOptionalImageOmitted;
  }
  ++imageResolveIndex_;
  return beginNextImageObject();
}

PdfStatus PdfPreparation::appendImageFileRecord(const PdfRequiredFileRecord& record) {
  if (!imageFileSpool_.writing()) {
    const int pathLength =
        std::snprintf(imageFileSpoolPath_, sizeof(imageFileSpoolPath_), "%s/gen_%lu/build.image-files", cacheRoot_,
                      static_cast<unsigned long>(generation_));
    if (pathLength <= 0 || static_cast<size_t>(pathLength) >= sizeof(imageFileSpoolPath_)) {
      return PdfStatus::failure(PdfError::LimitExceeded);
    }
    PdfStatus status = imageFileSpool_.beginWrite(config_.io, imageFileSpoolPath_);
    if (!status) {
      return status;
    }
  }
  const PdfStatus status = imageFileSpool_.append(record);
  if (status) {
    retainedImageFileCount_ = imageFileSpool_.recordCount();
  }
  return status;
}

PdfStepResult PdfPreparation::cacheCurrentPageImage(PdfWorkBudget& budget) {
  if (navigation_ == nullptr || currentPageImageCandidate_ < 0 ||
      static_cast<uint8_t>(currentPageImageCandidate_) >= imageCandidateCount_) {
    return PdfStepResult::completed();
  }
  PreparedImageCandidate& candidate = navigation_->imageCandidates[currentPageImageCandidate_];
  if (candidate.jpegCaptured &&
      ((!candidate.jpeg && !candidate.raster) || !candidate.placed || candidate.streamLength == 0)) {
    if (!budget.consumeOperation()) {
      return PdfStepResult::paused();
    }
    const PdfStatus discardStatus =
        navigation_->imageCache.discardCapturedJpeg(static_cast<uint8_t>(currentPageImageCandidate_));
    if (!discardStatus) {
      return PdfStepResult::failure(discardStatus);
    }
    candidate.jpegCaptured = false;
  }
  if ((!candidate.jpeg && !candidate.raster) || !candidate.placed || candidate.streamLength == 0) {
    return PdfStepResult::completed();
  }
  if (candidate.jpegCaptureFailed) {
    return PdfStepResult::completed();
  }
  if (candidate.raster) {
    if (imageCacheStage_ == ImageCacheStage::Idle) {
      PdfStatus status = beginPreparedRasterFingerprint(candidate);
      if (status) {
        status = pdfInitializeByteRange(source(), candidate.streamOffset, candidate.streamLength, &imageCacheRange_);
      }
      if (!status) {
        return PdfStepResult::failure(status);
      }
      imageCacheOffset_ = 0;
      rasterIdentityScanIndex_ = 0;
      candidate.rasterIdentityIndex = UINT8_MAX;
      imageCacheStage_ = ImageCacheStage::RasterPrimary;
    }

    if (imageCacheOffset_ < imageCacheRange_.length) {
      if (!budget.consumeOperation()) {
        return PdfStepResult::paused();
      }
      const size_t requested = budget.takeBytes(static_cast<size_t>(
          std::min<uint64_t>(PdfLimits::SourceBufferBytes, imageCacheRange_.length - imageCacheOffset_)));
      if (requested == 0) {
        return PdfStepResult::paused();
      }
      size_t bytesRead = 0;
      PdfByteSource encoded = pdfByteRangeSource(imageCacheRange_);
      const PdfStatus status =
          encoded.readAt(encoded.context, imageCacheOffset_, sourceWindow_.get(), requested, &bytesRead);
      if (!status || bytesRead == 0 || bytesRead > requested) {
        return PdfStepResult::failure(status ? PdfStatus::failure(PdfError::UnexpectedEof, imageCacheOffset_) : status);
      }
      hashPreparedImagePart(candidate, sourceWindow_.get(), bytesRead);
      imageCacheOffset_ += bytesRead;
      return PdfStepResult::paused();
    }

    if (imageCacheStage_ == ImageCacheStage::RasterPrimary && candidate.hasAuxiliary) {
      hashPreparedRasterAuxiliaryContract(candidate);
      const PdfStatus status = pdfInitializeByteRange(source(), candidate.auxiliaryStreamOffset,
                                                      candidate.auxiliaryStreamLength, &imageCacheRange_);
      if (!status) {
        return PdfStepResult::failure(status);
      }
      imageCacheOffset_ = 0;
      imageCacheStage_ = ImageCacheStage::RasterAuxiliary;
      return PdfStepResult::paused();
    }

    if (imageCacheStage_ == ImageCacheStage::RasterPrimary || imageCacheStage_ == ImageCacheStage::RasterAuxiliary) {
      imageCacheRange_ = {};
      imageCacheOffset_ = 0;
      rasterIdentityScanIndex_ = 0;
      imageCacheStage_ = ImageCacheStage::RasterIdentity;
    }
    if (imageCacheStage_ != ImageCacheStage::RasterIdentity) {
      return PdfStepResult::failure(PdfStatus::failure(PdfError::InvalidArgument));
    }
    if (imageBuildSpool_.recordCount() >= PDF_IMAGE_BUILD_SPOOL_MAX_RECORDS) {
      warningFlags_ |= kWarningOptionalImageOmitted;
      cacheBudget_.optionalOmitted = true;
      candidate.retained = false;
      imageCacheStage_ = ImageCacheStage::Idle;
      return PdfStepResult::completed();
    }
    uint64_t sourceBytes = 0;
    PdfStatus status = calculateRasterSourceBytes(candidate.streamLength, candidate.hasAuxiliary,
                                                  candidate.auxiliaryStreamLength, &sourceBytes);
    if (!status) {
      return PdfStepResult::failure(status);
    }
    const PdfStepResult identityResult =
        navigation_->imageCache.stepRasterIdentity(candidate.contentHash, candidate.sourceCrc32, sourceBytes,
                                                   &rasterIdentityScanIndex_, &candidate.rasterIdentityIndex, budget);
    if (identityResult.yielded()) {
      return identityResult;
    }
    imageCacheStage_ = ImageCacheStage::Idle;
    rasterIdentityScanIndex_ = 0;
    if (identityResult.failed()) {
      if (identityResult.status.error != PdfError::LimitExceeded) {
        return identityResult;
      }
      warningFlags_ |= kWarningOptionalImageOmitted;
      cacheBudget_.optionalOmitted = true;
      candidate.retained = false;
      return PdfStepResult::completed();
    }
    const int hrefLength = std::snprintf(candidate.href, sizeof(candidate.href), "../images/%016llx-%08lx.pxc",
                                         static_cast<unsigned long long>(candidate.contentHash),
                                         static_cast<unsigned long>(candidate.sourceCrc32));
    if (hrefLength <= 0 || static_cast<size_t>(hrefLength) >= sizeof(candidate.href)) {
      return PdfStepResult::failure(PdfStatus::failure(PdfError::LimitExceeded));
    }
    if (candidate.coverCandidate && !coverImageFingerprintSelected_) {
      coverImageContentHash_ = candidate.contentHash;
      coverImageSourceCrc32_ = candidate.sourceCrc32;
      coverImageFingerprintSelected_ = true;
      coverImageSourceJpeg_ = false;
    }
    candidate.retained = true;
    return PdfStepResult::completed();
  }
  PdfCachedImage& cached = navigation_->cachedImageScratch;
  if (imageCacheStage_ == ImageCacheStage::Idle) {
    cached = {};
    PdfStatus status = PdfStatus::success();
    if (candidate.jpegCaptured) {
      const PdfCapturedJpeg captured{candidate.contentHash, candidate.streamLength, candidate.sourceCrc32,
                                    static_cast<uint8_t>(currentPageImageCandidate_)};
      status = navigation_->imageCache.beginCapturedJpeg(
          captured, static_cast<uint16_t>(candidate.width), static_cast<uint16_t>(candidate.height), &cached,
          &navigation_->imageCacheRuntime);
    } else {
      status = pdfInitializeByteRange(source(), candidate.streamOffset, candidate.streamLength, &imageCacheRange_);
      if (status) {
        status = navigation_->imageCache.beginJpeg(
            pdfByteRangeSource(imageCacheRange_), static_cast<uint16_t>(candidate.width),
            static_cast<uint16_t>(candidate.height), &cached, &navigation_->imageCacheRuntime);
      }
    }
    if (!status) {
      cacheBudget_.optionalOmitted = true;
      imageCacheRange_ = {};
      if (candidate.jpegCaptured) {
        (void)navigation_->imageCache.discardCapturedJpeg(static_cast<uint8_t>(currentPageImageCandidate_));
        candidate.jpegCaptured = false;
      }
      return PdfStepResult::completed();
    }
    imageCacheStage_ = ImageCacheStage::Jpeg;
    return PdfStepResult::paused();
  }
  if (imageCacheStage_ != ImageCacheStage::Jpeg) {
    return PdfStepResult::failure(PdfStatus::failure(PdfError::InvalidArgument));
  }
  const PdfStepResult cacheResult = navigation_->imageCache.stepJpeg(navigation_->imageCacheRuntime, budget);
  if (cacheResult.yielded()) {
    return cacheResult;
  }
  imageCacheStage_ = ImageCacheStage::Idle;
  imageCacheRange_ = {};
  candidate.jpegCaptured = false;
  if (cacheResult.failed()) {
    cacheBudget_.optionalOmitted = true;
    return PdfStepResult::completed();
  }
  PdfStatus status = PdfStatus::success();
  const char* const imageLeaf = std::strstr(cached.record.path, "/images/");
  if (imageLeaf == nullptr) {
    return PdfStepResult::failure(PdfStatus::failure(PdfError::Malformed));
  }
  const int hrefLength =
      std::snprintf(candidate.href, sizeof(candidate.href), "../images/%s", imageLeaf + std::strlen("/images/"));
  if (hrefLength <= 0 || static_cast<size_t>(hrefLength) >= sizeof(candidate.href)) {
    return PdfStepResult::failure(PdfStatus::failure(PdfError::LimitExceeded));
  }
  candidate.retained = true;
  if (!cached.reused) {
    status = appendImageFileRecord(cached.record);
    if (!status) {
      (void)config_.io.remove(config_.io.context, cached.fullPath, false);
      warningFlags_ |= kWarningOptionalImageOmitted;
      cacheBudget_.optionalOmitted = true;
      candidate.retained = false;
      return status.error == PdfError::LimitExceeded ? PdfStepResult::completed() : PdfStepResult::failure(status);
    }
  }
  if (candidate.coverCandidate && !coverImageFingerprintSelected_) {
    coverImageContentHash_ = cached.contentHash;
    coverImageSourceCrc32_ = cached.record.crc32;
    coverImageSourceRecord_ = cached.record;
    coverImageFingerprintSelected_ = true;
    coverImageRecordAvailable_ = true;
    coverImageSourceJpeg_ = true;
  }
  return PdfStepResult::completed();
}

PdfStatus PdfPreparation::appendDeferredImageRecord(const uint8_t candidateIndex, const uint32_t tagOffset,
                                                    const uint16_t tagLength) {
  if (navigation_ == nullptr || candidateIndex >= imageCandidateCount_ || !imageBuildSpool_.writing() ||
      tagLength == 0) {
    return PdfStatus::failure(PdfError::InvalidArgument, candidateIndex);
  }
  const PreparedImageCandidate& candidate = navigation_->imageCandidates[candidateIndex];
  if (!candidate.raster || !candidate.retained || candidate.streamLength == 0 ||
      candidate.rasterIdentityIndex == UINT8_MAX ||
      candidate.parameters.paletteBytes > sizeof(activeRasterCandidate_.palette) ||
      (candidate.parameters.paletteBytes != 0 && candidate.parameters.palette == nullptr)) {
    return PdfStatus::failure(PdfError::Malformed, candidateIndex);
  }
  resetInPlace(activeRasterCandidate_);
  activeRasterCandidate_.streamOffset = candidate.streamOffset;
  activeRasterCandidate_.streamLength = candidate.streamLength;
  activeRasterCandidate_.contentHash = candidate.contentHash;
  activeRasterCandidate_.sourceCrc32 = candidate.sourceCrc32;
  activeRasterCandidate_.width = candidate.parameters.width;
  activeRasterCandidate_.height = candidate.parameters.height;
  activeRasterCandidate_.bitsPerComponent = candidate.parameters.bitsPerComponent;
  activeRasterCandidate_.predictor = candidate.parameters.predictor;
  activeRasterCandidate_.colorSpace = candidate.parameters.colorSpace;
  activeRasterCandidate_.decode = candidate.parameters.decode;
  activeRasterCandidate_.imageMaskPaintLuminance = candidate.parameters.imageMaskPaintLuminance;
  activeRasterCandidate_.paletteBytes = candidate.parameters.paletteBytes;
  activeRasterCandidate_.paletteEntries = candidate.parameters.paletteEntries;
  if (candidate.parameters.paletteBytes != 0) {
    std::memcpy(activeRasterCandidate_.palette, candidate.parameters.palette, candidate.parameters.paletteBytes);
  }
  activeRasterCandidate_.filterCount = candidate.filterCount;
  std::copy_n(candidate.filters, candidate.filterCount, activeRasterCandidate_.filters);
  activeRasterCandidate_.auxiliaryStreamOffset = candidate.auxiliaryStreamOffset;
  activeRasterCandidate_.auxiliaryStreamLength = candidate.auxiliaryStreamLength;
  activeRasterCandidate_.auxiliaryWidth = candidate.auxiliaryParameters.width;
  activeRasterCandidate_.auxiliaryHeight = candidate.auxiliaryParameters.height;
  activeRasterCandidate_.auxiliaryBitsPerComponent = candidate.auxiliaryParameters.bitsPerComponent;
  activeRasterCandidate_.auxiliaryPredictor = candidate.auxiliaryParameters.predictor;
  activeRasterCandidate_.auxiliaryColorSpace = candidate.auxiliaryParameters.colorSpace;
  activeRasterCandidate_.auxiliaryDecode = candidate.auxiliaryParameters.decode;
  activeRasterCandidate_.auxiliaryFilterCount = candidate.auxiliaryFilterCount;
  std::copy_n(candidate.auxiliaryFilters, candidate.auxiliaryFilterCount, activeRasterCandidate_.auxiliaryFilters);
  activeRasterCandidate_.auxiliaryKind = candidate.auxiliaryKind;
  activeRasterCandidate_.hasAuxiliary = candidate.hasAuxiliary;
  activeRasterCandidate_.sectionIndex = sectionCount_;
  activeRasterCandidate_.tagOffset = tagOffset;
  activeRasterCandidate_.tagLength = tagLength;
  const uint8_t recordIndex = imageBuildSpool_.recordCount();
  if (recordIndex >= PDF_IMAGE_BUILD_SPOOL_MAX_RECORDS) {
    return PdfStatus::failure(PdfError::LimitExceeded, recordIndex);
  }
  uint8_t canonicalRecordIndex = UINT8_MAX;
  PdfStatus status =
      navigation_->imageCache.bindRasterRecord(candidate.rasterIdentityIndex, recordIndex, &canonicalRecordIndex);
  if (!status || canonicalRecordIndex > recordIndex) {
    return status ? PdfStatus::failure(PdfError::Malformed, recordIndex) : status;
  }
  rasterCanonicalRecordIndices_[recordIndex] = canonicalRecordIndex;
  status = imageBuildSpool_.append(activeRasterCandidate_);
  if (!status) {
    rasterCanonicalRecordIndices_[recordIndex] = UINT8_MAX;
  }
  return status;
}

PdfStepResult PdfPreparation::spoolNavigation(PdfWorkBudget& budget) {
  static_assert(sizeof(RasterBatchWorkspace) <= PdfLimits::OperandOrderHistogramBytes);
  if (navigationSpoolStage_ == NavigationSpoolStage::None) {
    if (navigation_ == nullptr || !operandScratch_ || !runRecords_ || imageBuildSpool_.recordCount() == 0 ||
        imageBuildSpool_.writing() || navigationSpoolHandle_.valid() || navigationSpoolPath_[0] != '\0') {
      return PdfStepResult::failure(PdfStatus::failure(PdfError::InvalidArgument));
    }
    rasterBatch_ = new (operandScratch_.get()) RasterBatchWorkspace{};
    const int pathLength = std::snprintf(navigationSpoolPath_, sizeof(navigationSpoolPath_), "%s/gen_%lu/build.nav",
                                         cacheRoot_, static_cast<unsigned long>(generation_));
    if (pathLength <= 0 || static_cast<size_t>(pathLength) >= sizeof(navigationSpoolPath_)) {
      navigationSpoolPath_[0] = '\0';
      return PdfStepResult::failure(PdfStatus::failure(PdfError::LimitExceeded));
    }
    const PdfStatus status = config_.io.open(config_.io.context, navigationSpoolPath_, PdfCacheOpenMode::WriteTruncate,
                                             &navigationSpoolHandle_);
    if (!status) {
      abortNavigationSpool();
      return PdfStepResult::failure(status);
    }
    navigationSpoolOffset_ = 0;
    navigationSpoolCrc32_ = 0;
    navigationSpoolStage_ = NavigationSpoolStage::Writing;
    return PdfStepResult::paused();
  }

  if (navigation_ == nullptr || !navigationSpoolHandle_.valid() ||
      navigationSpoolOffset_ > sizeof(NavigationWorkspace)) {
    return PdfStepResult::failure(PdfStatus::failure(PdfError::InvalidArgument, navigationSpoolOffset_));
  }
  if (navigationSpoolStage_ == NavigationSpoolStage::Writing && navigationSpoolOffset_ < sizeof(NavigationWorkspace)) {
    if (!budget.consumeOperation()) {
      return PdfStepResult::paused();
    }
    const size_t remaining = sizeof(NavigationWorkspace) - static_cast<size_t>(navigationSpoolOffset_);
    const size_t requested = budget.takeBytes(std::min<size_t>(PdfLimits::SourceBufferBytes, remaining));
    if (requested == 0) {
      return PdfStepResult::paused();
    }
    const auto* const bytes = reinterpret_cast<const uint8_t*>(navigation_) + navigationSpoolOffset_;
    size_t written = 0;
    PdfStatus status = config_.io.write(config_.io.context, navigationSpoolHandle_, bytes, requested, &written);
    if (!status || written != requested) {
      if (status) {
        status = PdfStatus::failure(PdfError::IoFailure, navigationSpoolOffset_ + written);
      }
      abortNavigationSpool();
      return PdfStepResult::failure(status);
    }
    navigationSpoolCrc32_ = pdfCacheCrc32(bytes, written, navigationSpoolCrc32_);
    navigationSpoolOffset_ += written;
    return PdfStepResult::paused();
  }

  if (navigationSpoolStage_ == NavigationSpoolStage::Writing) {
    navigationSpoolStage_ = NavigationSpoolStage::Flush;
    return PdfStepResult::paused();
  }
  if (!budget.consumeOperation()) {
    return PdfStepResult::paused();
  }
  PdfStatus status = PdfStatus::failure(PdfError::InvalidArgument);
  if (navigationSpoolStage_ == NavigationSpoolStage::Flush) {
    status = config_.io.flush(config_.io.context, navigationSpoolHandle_);
    if (status) {
      navigationSpoolStage_ = NavigationSpoolStage::Sync;
    }
    if (!status) {
      abortNavigationSpool();
      return PdfStepResult::failure(status);
    }
    return PdfStepResult::paused();
  } else if (navigationSpoolStage_ == NavigationSpoolStage::Sync) {
    status = config_.io.sync(config_.io.context, navigationSpoolHandle_);
    if (status) {
      navigationSpoolStage_ = NavigationSpoolStage::Close;
    }
    if (!status) {
      abortNavigationSpool();
      return PdfStepResult::failure(status);
    }
    return PdfStepResult::paused();
  } else if (navigationSpoolStage_ == NavigationSpoolStage::Close) {
    status = config_.io.close(config_.io.context, &navigationSpoolHandle_);
  }
  if (!status) {
    abortNavigationSpool();
    return PdfStepResult::failure(status);
  }
  navigationSpoolHandle_ = {};
  navigationSpoolOffset_ = 0;
  navigationSpoolBytes_ = static_cast<uint32_t>(sizeof(NavigationWorkspace));
  ++navigationSpoolWriteCount_;
  contentLexer_.reset();
  pageWalker_.reset();
  resolver_.reset();
  xrefParser_.reset();
  xref_.reset();
  navigation_ = nullptr;
  std::memset(dictionary_.get(), 0, PdfLimits::UzlibDictionaryBytes);
  navigationSpoolStage_ = NavigationSpoolStage::ReadyToRead;
  return PdfStepResult::completed();
}

PdfStepResult PdfPreparation::decodeRasterBatch(PdfWorkBudget& budget) {
  if (navigation_ != nullptr || rasterBatch_ == nullptr || imageBuildSpool_.recordCount() == 0 || !dictionary_ ||
      !sourceWindow_ || !decoderOutput_ || !runRecords_) {
    return PdfStepResult::failure(PdfStatus::failure(PdfError::InvalidArgument));
  }
  if (rasterDecodeStage_ == RasterDecodeStage::Idle) {
    if (rasterBatch_->imageBuildRead.stage == PdfImageSpoolReadStage::Idle) {
      PdfStatus status = closeSource();
      if (status) {
        status = imageBuildSpool_.beginRead(config_.io, imageBuildSpoolPath_, sourceWindow_.get(),
                                            PdfLimits::SourceBufferBytes, &rasterBatch_->imageBuildRead);
      }
      if (!status) {
        return PdfStepResult::failure(status);
      }
      return PdfStepResult::paused();
    }
    const PdfStepResult opened = imageBuildSpool_.stepReadOpen(rasterBatch_->imageBuildRead, budget);
    if (!opened.complete()) {
      return opened;
    }
    const PdfStatus status = imageBuildSpool_.closeRead();
    if (!status) {
      return PdfStepResult::failure(status);
    }
    rasterBatch_->imageBuildRead = {};
    rasterDecodeIndex_ = 0;
    rasterDecodeStage_ = RasterDecodeStage::LoadCandidate;
    return PdfStepResult::paused();
  }

  if (rasterDecodeStage_ == RasterDecodeStage::LoadCandidate) {
    if (rasterDecodeIndex_ >= imageBuildSpool_.recordCount()) {
      rasterDecodeStage_ = maskSpool_ == nullptr ? RasterDecodeStage::Finalize : RasterDecodeStage::CloseMaskSpool;
      return PdfStepResult::paused();
    }
    const uint8_t canonicalRecordIndex = rasterCanonicalRecordIndices_[rasterDecodeIndex_];
    if (canonicalRecordIndex == UINT8_MAX || canonicalRecordIndex > rasterDecodeIndex_) {
      return PdfStepResult::failure(PdfStatus::failure(PdfError::Malformed, rasterDecodeIndex_));
    }
    if (!budget.consumeOperation()) {
      return PdfStepResult::paused();
    }
    if (canonicalRecordIndex < rasterDecodeIndex_) {
      if ((failedRasterImages_ & (UINT64_C(1) << canonicalRecordIndex)) != 0) {
        failedRasterImages_ |= UINT64_C(1) << rasterDecodeIndex_;
      }
      ++rasterDecodeIndex_;
      return PdfStepResult::paused();
    }
    PdfStatus status = closeSource();
    if (status) {
      status = imageBuildSpool_.readRecordDetached(rasterDecodeIndex_, &activeRasterCandidate_);
    }
    if (status) {
      status = reopenSource();
    }
    if (!status) {
      return PdfStepResult::failure(status);
    }
    const uint64_t used = cacheBudget_.requiredBytes + cacheBudget_.optionalBytes;
    const uint64_t byteLimit = used < cacheBudget_.limit ? cacheBudget_.limit - used : 0;
    if (activeRasterCandidate_.hasAuxiliary && maskSpool_ == nullptr) {
      const int pathLength = std::snprintf(maskSpoolPath_, sizeof(maskSpoolPath_), "%s/gen_%lu/build.mask", cacheRoot_,
                                           static_cast<unsigned long>(generation_));
      if (pathLength <= 0 || static_cast<size_t>(pathLength) >= sizeof(maskSpoolPath_)) {
        return PdfStepResult::failure(PdfStatus::failure(PdfError::LimitExceeded));
      }
      maskSpool_ = new (runRecords_.get() + kMaskSpoolWorkspaceOffset) PdfMaskSpool{};
      status = maskSpool_->beginWrite(config_.io, maskSpoolPath_);
      if (!status) {
        maskSpool_->~PdfMaskSpool();
        maskSpool_ = nullptr;
        return PdfStepResult::failure(status);
      }
    }
    status = activeRasterCandidate_.hasAuxiliary ? beginMaskedRaster(activeRasterCandidate_, byteLimit)
                                                 : beginUnmaskedRaster(activeRasterCandidate_, byteLimit);
    if (!status) {
      if (!activeRasterCandidate_.hasAuxiliary) {
        const PdfStepResult omitted = omitActiveRasterImage(status);
        if (omitted.failed()) {
          return omitted;
        }
        ++rasterDecodeIndex_;
        rasterDecodeStage_ = RasterDecodeStage::LoadCandidate;
        return PdfStepResult::paused();
      }
      return PdfStepResult::failure(status);
    }
    rasterDecodeStage_ =
        activeRasterCandidate_.hasAuxiliary ? RasterDecodeStage::DecodeMaskedBase : RasterDecodeStage::DecodeUnmasked;
    return PdfStepResult::paused();
  }

  if (rasterDecodeStage_ == RasterDecodeStage::DecodeUnmasked) {
    const PdfStepResult result = stepActiveRaster(budget, false);
    if (result.complete()) {
      ++rasterDecodeIndex_;
      rasterDecodeStage_ = RasterDecodeStage::LoadCandidate;
      return PdfStepResult::paused();
    }
    return result;
  }

  if (rasterDecodeStage_ == RasterDecodeStage::DecodeMaskedBase) {
    const PdfStepResult result = stepActiveRaster(budget, true);
    if (!result.complete()) {
      return result;
    }
    const PdfStatus status = beginActiveMask(activeRasterCandidate_);
    if (!status) {
      return PdfStepResult::failure(status);
    }
    rasterDecodeStage_ = RasterDecodeStage::DecodeMaskedAlpha;
    return PdfStepResult::paused();
  }

  if (rasterDecodeStage_ == RasterDecodeStage::DecodeMaskedAlpha) {
    const PdfStepResult result = stepActiveMask(budget);
    if (result.complete()) {
      ++rasterDecodeIndex_;
      rasterDecodeStage_ = RasterDecodeStage::LoadCandidate;
      return PdfStepResult::paused();
    }
    return result;
  }

  if (rasterDecodeStage_ == RasterDecodeStage::CloseMaskSpool) {
    if (maskSpool_ == nullptr) {
      return PdfStepResult::failure(PdfStatus::failure(PdfError::InvalidArgument));
    }
    if (rasterBatch_->maskClose.stage == PdfMaskSpoolCloseStage::Idle) {
      const PdfStatus status = maskSpool_->beginCloseWrite(&rasterBatch_->maskClose);
      if (!status) {
        return PdfStepResult::failure(status);
      }
      return PdfStepResult::paused();
    }
    const PdfStepResult result = maskSpool_->stepCloseWrite(rasterBatch_->maskClose, budget);
    if (!result.complete()) {
      return result;
    }
    rasterBatch_->maskClose = {};
    ++maskSpoolWriteCount_;
    rasterDecodeStage_ = RasterDecodeStage::OpenMaskSpool;
    return PdfStepResult::paused();
  }

  if (rasterDecodeStage_ == RasterDecodeStage::OpenMaskSpool) {
    const PdfStatus status = beginMaskCompositeSpool();
    if (!status) {
      return PdfStepResult::failure(status);
    }
    rasterDecodeStage_ = RasterDecodeStage::CompositeMasks;
    return PdfStepResult::paused();
  }

  if (rasterDecodeStage_ == RasterDecodeStage::CompositeMasks) {
    const PdfStepResult result = stepMaskCompositeSpool(budget);
    if (!result.complete()) {
      return result;
    }
    rasterDecodeStage_ = RasterDecodeStage::Finalize;
    return PdfStepResult::paused();
  }

  if (rasterDecodeStage_ == RasterDecodeStage::Finalize) {
    PdfStatus status = PdfStatus::success();
    if (imageFileSpool_.writing()) {
      status = imageFileSpool_.closeWrite();
    }
    if (!status) {
      return PdfStepResult::failure(status);
    }
    rasterDecodeStage_ = RasterDecodeStage::Idle;
    return PdfStepResult::completed();
  }

  return PdfStepResult::failure(PdfStatus::failure(PdfError::InvalidArgument));
}

PdfStatus PdfPreparation::beginUnmaskedRaster(const PdfDeferredImageRecord& candidate, const uint64_t byteLimit) {
  if (rasterBatch_ == nullptr) {
    return PdfStatus::failure(PdfError::InvalidArgument);
  }
  auto* runtime = new (runRecords_.get()) RasterRuntime(
      {sourceWindow_.get(), PdfLimits::SourceBufferBytes, runRecords_.get() + kRasterDecoderWorkspaceOffset,
       PdfLimits::DecoderOutputBytes, dictionary_.get(), PdfLimits::UzlibDictionaryBytes});
  const int relativeLength =
      std::snprintf(runtime->relativePath, sizeof(runtime->relativePath), "gen_%lu/images/%016llx-%08lx.pxc",
                    static_cast<unsigned long>(generation_), static_cast<unsigned long long>(candidate.contentHash),
                    static_cast<unsigned long>(candidate.sourceCrc32));
  const int fullLength =
      std::snprintf(runtime->fullPath, sizeof(runtime->fullPath), "%s/%s", cacheRoot_, runtime->relativePath);
  if (relativeLength <= 0 || static_cast<size_t>(relativeLength) >= sizeof(runtime->relativePath) || fullLength <= 0 ||
      static_cast<size_t>(fullLength) >= sizeof(runtime->fullPath)) {
    runtime->~RasterRuntime();
    return PdfStatus::failure(PdfError::LimitExceeded);
  }
  PdfStatus status = pdfOpenTrackedCacheWriter(config_.io, runtime->fullPath, runtime->relativePath,
                                               PdfCacheFileKind::Optional, byteLimit, &runtime->writer);
  runtime->parameters.width = candidate.width;
  runtime->parameters.height = candidate.height;
  runtime->parameters.bitsPerComponent = candidate.bitsPerComponent;
  runtime->parameters.predictor = candidate.predictor;
  runtime->parameters.colorSpace = candidate.colorSpace;
  runtime->parameters.decode = candidate.decode;
  runtime->parameters.imageMaskPaintLuminance = candidate.imageMaskPaintLuminance;
  runtime->parameters.palette = candidate.paletteBytes == 0 ? nullptr : candidate.palette;
  runtime->parameters.paletteBytes = candidate.paletteBytes;
  runtime->parameters.paletteEntries = candidate.paletteEntries;
  runtime->parameters.maximumOutputWidth = config_.maximumRasterOutputWidth;
  runtime->parameters.maximumOutputHeight = config_.maximumRasterOutputHeight;
  runtime->parameters.maximumOutputBytes = byteLimit > UINT32_MAX ? UINT32_MAX : static_cast<uint32_t>(byteLimit);
  if (status) {
    status = pdfMakePreparationImageWorkspace(pageText_.get(), PdfLimits::PageTextBytes, decoderOutput_.get(),
                                              PdfLimits::DecoderOutputBytes, &runtime->imageWorkspace);
  }
  if (status) {
    status =
        runtime->extractor.begin(runtime->parameters, {&runtime->writer, writeRasterCache}, runtime->imageWorkspace);
  }
  if (status) {
    status = pdfInitializeByteRange(source(), candidate.streamOffset, candidate.streamLength, &runtime->encodedRange);
  }
  if (status) {
    status = runtime->decoder.begin(pdfByteRangeSource(runtime->encodedRange), runtime->extractor.decodedSink(),
                                    candidate.filters, candidate.filterCount, {}, false);
  }
  if (!status) {
    if (runtime->writer.open) {
      pdfAbortTrackedCacheFile(&runtime->writer);
    } else {
      (void)config_.io.remove(config_.io.context, runtime->fullPath, false);
    }
    runtime->~RasterRuntime();
    return status;
  }
  rasterRuntimeActive_ = true;
  return PdfStatus::success();
}

PdfStatus PdfPreparation::beginMaskedRaster(const PdfDeferredImageRecord& candidate, const uint64_t byteLimit) {
  if (maskSpool_ == nullptr || rasterBatch_ == nullptr) {
    return PdfStatus::failure(PdfError::InvalidArgument);
  }
  uint16_t outputWidth = 0;
  uint16_t outputHeight = 0;
  calculateRasterOutputDimensions(candidate, config_.maximumRasterOutputWidth, config_.maximumRasterOutputHeight,
                                  &outputWidth, &outputHeight);
  pixel_cache::Layout layout{};
  if (pixel_cache::calculateLayout(outputWidth, outputHeight, layout) != pixel_cache::Status::Ok ||
      layout.fileBytes > byteLimit) {
    return PdfStatus::failure(PdfError::LimitExceeded, layout.fileBytes);
  }
  PdfByteSink baseSink{};
  PdfStatus status =
      maskSpool_->beginRecord(candidate.contentHash, candidate.sourceCrc32, outputWidth, outputHeight, &baseSink);
  auto* runtime = new (runRecords_.get()) RasterRuntime(
      {sourceWindow_.get(), PdfLimits::SourceBufferBytes, runRecords_.get() + kRasterDecoderWorkspaceOffset,
       PdfLimits::DecoderOutputBytes, dictionary_.get(), PdfLimits::UzlibDictionaryBytes});
  runtime->parameters.width = candidate.width;
  runtime->parameters.height = candidate.height;
  runtime->parameters.bitsPerComponent = candidate.bitsPerComponent;
  runtime->parameters.predictor = candidate.predictor;
  runtime->parameters.colorSpace = candidate.colorSpace;
  runtime->parameters.decode = candidate.decode;
  runtime->parameters.imageMaskPaintLuminance = candidate.imageMaskPaintLuminance;
  runtime->parameters.palette = candidate.paletteBytes == 0 ? nullptr : candidate.palette;
  runtime->parameters.paletteBytes = candidate.paletteBytes;
  runtime->parameters.paletteEntries = candidate.paletteEntries;
  runtime->parameters.maximumOutputWidth = config_.maximumRasterOutputWidth;
  runtime->parameters.maximumOutputHeight = config_.maximumRasterOutputHeight;
  runtime->parameters.maximumOutputBytes = static_cast<uint32_t>(std::min<uint64_t>(byteLimit, UINT32_MAX));
  if (status) {
    status = pdfMakePreparationImageWorkspace(pageText_.get(), PdfLimits::PageTextBytes, decoderOutput_.get(),
                                              PdfLimits::DecoderOutputBytes, &runtime->imageWorkspace);
  }
  if (status) {
    status = runtime->extractor.begin(runtime->parameters, baseSink, runtime->imageWorkspace);
  }
  if (status) {
    status = pdfInitializeByteRange(source(), candidate.streamOffset, candidate.streamLength, &runtime->encodedRange);
  }
  if (status) {
    status = runtime->decoder.begin(pdfByteRangeSource(runtime->encodedRange), runtime->extractor.decodedSink(),
                                    candidate.filters, candidate.filterCount, {}, false);
  }
  if (!status) {
    runtime->~RasterRuntime();
    return status;
  }
  rasterRuntimeActive_ = true;
  return PdfStatus::success();
}

PdfStepResult PdfPreparation::stepActiveRaster(PdfWorkBudget& budget, const bool masked) {
  if (!rasterRuntimeActive_ || !runRecords_) {
    return PdfStepResult::failure(PdfStatus::failure(PdfError::InvalidArgument));
  }
  auto* runtime = reinterpret_cast<RasterRuntime*>(runRecords_.get());
  if (runtime->finalizeStage == RasterFinalizeStage::Decode) {
    const PdfStepResult decoded = runtime->decoder.step(budget);
    if (decoded.yielded()) {
      return decoded;
    }
    if (decoded.failed()) {
      if (runtime->writer.open) {
        pdfAbortTrackedCacheFile(&runtime->writer);
      }
      runtime->~RasterRuntime();
      rasterRuntimeActive_ = false;
      if (!masked) {
        return omitActiveRasterImage(decoded.status);
      }
      return decoded;
    }
    runtime->finalizeStage = RasterFinalizeStage::FinishExtractor;
    return PdfStepResult::paused();
  }

  PdfStatus status = PdfStatus::success();
  if (runtime->finalizeStage == RasterFinalizeStage::FinishExtractor) {
    if (!budget.consumeOperation()) {
      return PdfStepResult::paused();
    }
    status = runtime->extractor.finish();
    if (status && masked) {
      runtime->~RasterRuntime();
      rasterRuntimeActive_ = false;
      return PdfStepResult::completed();
    }
    if (status) {
      runtime->finalizeStage = RasterFinalizeStage::FlushWriter;
      return PdfStepResult::paused();
    }
  } else if (runtime->finalizeStage == RasterFinalizeStage::FlushWriter) {
    if (!runtime->writer.open || !budget.consumeOperation()) {
      return runtime->writer.open ? PdfStepResult::paused()
                                  : PdfStepResult::failure(PdfStatus::failure(PdfError::InvalidArgument));
    }
    status = runtime->writer.io.flush(runtime->writer.io.context, runtime->writer.handle);
    if (status) {
      runtime->finalizeStage = RasterFinalizeStage::SyncWriter;
      return PdfStepResult::paused();
    }
  } else if (runtime->finalizeStage == RasterFinalizeStage::SyncWriter) {
    if (!runtime->writer.open || !budget.consumeOperation()) {
      return runtime->writer.open ? PdfStepResult::paused()
                                  : PdfStepResult::failure(PdfStatus::failure(PdfError::InvalidArgument));
    }
    status = runtime->writer.io.sync(runtime->writer.io.context, runtime->writer.handle);
    if (status) {
      runtime->finalizeStage = RasterFinalizeStage::CloseWriter;
      return PdfStepResult::paused();
    }
  } else if (runtime->finalizeStage == RasterFinalizeStage::CloseWriter) {
    if (!runtime->writer.open || !budget.consumeOperation()) {
      return runtime->writer.open ? PdfStepResult::paused()
                                  : PdfStepResult::failure(PdfStatus::failure(PdfError::InvalidArgument));
    }
    status = runtime->writer.io.close(runtime->writer.io.context, &runtime->writer.handle);
    runtime->writer.open = false;
    if (status) {
      runtime->record = runtime->writer.record;
      runtime->finalizeStage = RasterFinalizeStage::Reserve;
      return PdfStepResult::paused();
    }
    runtime->writer.failed = true;
  } else if (runtime->finalizeStage == RasterFinalizeStage::Reserve) {
    status = pdfReserveCacheBytes(&cacheBudget_, runtime->record.size, PdfCacheFileKind::Optional);
    if (status) {
      runtime->finalizeStage = RasterFinalizeStage::AppendRecord;
      return PdfStepResult::paused();
    }
  } else if (runtime->finalizeStage == RasterFinalizeStage::AppendRecord) {
    if (!budget.consumeOperation()) {
      return PdfStepResult::paused();
    }
    status = appendImageFileRecord(runtime->record);
    if (status && coverImageFingerprintSelected_ && !coverImageRecordAvailable_ &&
        activeRasterCandidate_.contentHash == coverImageContentHash_ &&
        activeRasterCandidate_.sourceCrc32 == coverImageSourceCrc32_) {
      coverImageSourceRecord_ = runtime->record;
      coverImageRecordAvailable_ = true;
      coverImageSourceJpeg_ = false;
    }
  } else {
    status = PdfStatus::failure(PdfError::InvalidArgument);
  }
  if (!status) {
    if (runtime->writer.open) {
      pdfAbortTrackedCacheFile(&runtime->writer);
    } else if (runtime->fullPath[0] != '\0') {
      (void)config_.io.remove(config_.io.context, runtime->fullPath, false);
    }
  }
  runtime->~RasterRuntime();
  rasterRuntimeActive_ = false;
  if (!status && !masked) {
    return omitActiveRasterImage(status);
  }
  return status ? PdfStepResult::completed() : PdfStepResult::failure(status);
}

PdfStatus PdfPreparation::beginActiveMask(const PdfDeferredImageRecord& candidate) {
  if (maskSpool_ == nullptr || maskDecodeRuntimeActive_ || rasterRuntimeActive_) {
    return PdfStatus::failure(PdfError::InvalidArgument);
  }
  uint16_t outputWidth = 0;
  uint16_t outputHeight = 0;
  calculateRasterOutputDimensions(candidate, config_.maximumRasterOutputWidth, config_.maximumRasterOutputHeight,
                                  &outputWidth, &outputHeight);
  auto* maskRuntime = new (runRecords_.get()) MaskDecodeRuntime(
      {sourceWindow_.get(), PdfLimits::SourceBufferBytes, runRecords_.get() + kRasterDecoderWorkspaceOffset,
       PdfLimits::DecoderOutputBytes, dictionary_.get(), PdfLimits::UzlibDictionaryBytes});
  maskRuntime->config.sourceWidth = candidate.auxiliaryWidth;
  maskRuntime->config.sourceHeight = candidate.auxiliaryHeight;
  maskRuntime->config.outputWidth = outputWidth;
  maskRuntime->config.outputHeight = outputHeight;
  maskRuntime->config.bitsPerComponent = candidate.auxiliaryBitsPerComponent;
  maskRuntime->config.predictor = candidate.auxiliaryPredictor;
  maskRuntime->config.decode = candidate.auxiliaryDecode;
  maskRuntime->config.explicitMask = candidate.auxiliaryKind == PdfImageAuxiliaryKind::ExplicitMask;
  maskRuntime->config.rowWorkspace = pageText_.get();
  maskRuntime->config.rowWorkspaceBytes = PdfLimits::PageTextBytes;
  maskRuntime->config.outputWorkspace = decoderOutput_.get();
  maskRuntime->config.outputWorkspaceBytes = PdfLimits::DecoderOutputBytes;
  PdfStatus status = maskSpool_->beginAlpha(maskRuntime->config, &maskRuntime->plane);
  if (status) {
    status = pdfInitializeByteRange(source(), candidate.auxiliaryStreamOffset, candidate.auxiliaryStreamLength,
                                    &maskRuntime->encodedRange);
  }
  if (status) {
    status = maskRuntime->decoder.begin(pdfByteRangeSource(maskRuntime->encodedRange), maskRuntime->plane.decodedSink(),
                                        candidate.auxiliaryFilters, candidate.auxiliaryFilterCount, {}, false);
  }
  if (!status) {
    maskRuntime->~MaskDecodeRuntime();
    return status;
  }
  maskDecodeRuntimeActive_ = true;
  return status;
}

PdfStepResult PdfPreparation::stepActiveMask(PdfWorkBudget& budget) {
  if (!maskDecodeRuntimeActive_ || !runRecords_ || maskSpool_ == nullptr) {
    return PdfStepResult::failure(PdfStatus::failure(PdfError::InvalidArgument));
  }
  auto* runtime = reinterpret_cast<MaskDecodeRuntime*>(runRecords_.get());
  const PdfStepResult decoded = runtime->decoder.step(budget);
  if (decoded.yielded()) {
    return decoded;
  }
  if (decoded.failed()) {
    runtime->~MaskDecodeRuntime();
    maskDecodeRuntimeActive_ = false;
    return decoded;
  }
  PdfStatus status = runtime->plane.finish();
  if (status) {
    status = maskSpool_->finishRecord();
  }
  runtime->~MaskDecodeRuntime();
  maskDecodeRuntimeActive_ = false;
  return status ? PdfStepResult::completed() : PdfStepResult::failure(status);
}

PdfStatus PdfPreparation::beginMaskCompositeSpool() {
  if (maskSpool_ == nullptr || rasterBatch_ == nullptr || maskSpoolPath_[0] == '\0') {
    return PdfStatus::failure(PdfError::InvalidArgument);
  }
  PdfStatus status = closeSource();
  if (status) {
    status = maskSpool_->beginRead(config_.io, maskSpoolPath_, sourceWindow_.get(), PdfLimits::SourceBufferBytes,
                                   &rasterBatch_->maskRead);
  }
  if (status) {
    ++maskSpoolReadCount_;
    rasterBatch_->maskCompositeIndex = 0;
  }
  return status;
}

PdfStepResult PdfPreparation::stepMaskCompositeSpool(PdfWorkBudget& budget) {
  if (maskSpool_ == nullptr || rasterBatch_ == nullptr) {
    return PdfStepResult::failure(PdfStatus::failure(PdfError::InvalidArgument));
  }
  if (rasterBatch_->maskRead.stage != PdfMaskSpoolReadStage::Complete) {
    const PdfStepResult opened = maskSpool_->stepReadOpen(rasterBatch_->maskRead, budget);
    if (opened.complete()) {
      return PdfStepResult::paused();
    }
    return opened;
  }
  if (maskCompositeRuntimeActive_) {
    const PdfStepResult result = stepMaskCompositeRecord(budget);
    if (result.complete()) {
      ++rasterBatch_->maskCompositeIndex;
      return PdfStepResult::paused();
    }
    return result;
  }
  if (rasterBatch_->maskCompositeIndex < maskSpool_->recordCount()) {
    const PdfStatus status = beginMaskCompositeRecord(maskSpool_->record(rasterBatch_->maskCompositeIndex));
    return status ? PdfStepResult::paused() : PdfStepResult::failure(status);
  }

  PdfStatus status = maskSpool_->closeRead();
  const PdfStatus removeStatus = config_.io.remove(config_.io.context, maskSpoolPath_, false);
  if (status && !removeStatus) {
    status = removeStatus;
  }
  maskSpoolPath_[0] = '\0';
  maskSpool_->~PdfMaskSpool();
  maskSpool_ = nullptr;
  rasterBatch_->maskRead = {};
  rasterBatch_->maskCompositeIndex = 0;
  return status ? PdfStepResult::completed() : PdfStepResult::failure(status);
}

PdfStatus PdfPreparation::beginMaskCompositeRecord(const PdfMaskSpoolRecord& record) {
  if (maskSpool_ == nullptr || maskCompositeRuntimeActive_) {
    return PdfStatus::failure(PdfError::InvalidArgument);
  }
  auto* runtime = new (runRecords_.get()) MaskCompositeRuntime{};
  maskCompositeRuntimeActive_ = true;
  runtime->spoolRecord = record;
  const int relativeLength =
      std::snprintf(runtime->relativePath, sizeof(runtime->relativePath), "gen_%lu/images/%016llx-%08lx.pxc",
                    static_cast<unsigned long>(generation_), static_cast<unsigned long long>(record.contentHash),
                    static_cast<unsigned long>(record.sourceCrc32));
  const int fullLength =
      std::snprintf(runtime->fullPath, sizeof(runtime->fullPath), "%s/%s", cacheRoot_, runtime->relativePath);
  PdfStatus status = PdfStatus::success();
  if (relativeLength <= 0 || static_cast<size_t>(relativeLength) >= sizeof(runtime->relativePath) || fullLength <= 0 ||
      static_cast<size_t>(fullLength) >= sizeof(runtime->fullPath)) {
    status = PdfStatus::failure(PdfError::LimitExceeded);
  }
  size_t bytesRead = 0;
  if (status) {
    status = maskSpool_->read(record.baseOffset, sourceWindow_.get(), pixel_cache::kHeaderSize, &bytesRead);
    if (status && bytesRead != pixel_cache::kHeaderSize) {
      status = PdfStatus::failure(PdfError::UnexpectedEof, record.baseOffset + bytesRead);
    }
  }
  if (status &&
      (pixel_cache::decodeHeader(sourceWindow_.get(), pixel_cache::kHeaderSize, runtime->layout) !=
           pixel_cache::Status::Ok ||
       runtime->layout.width != record.width || runtime->layout.height != record.height ||
       runtime->layout.fileBytes != record.baseBytes || runtime->layout.bytesPerRow > PdfLimits::SourceBufferBytes ||
       runtime->layout.width > PdfLimits::DecoderOutputBytes ||
       runtime->layout.bytesPerRow + runtime->layout.width > PdfLimits::SourceBufferBytes)) {
    status = PdfStatus::failure(PdfError::Malformed, record.baseOffset);
  }
  const uint64_t used = cacheBudget_.requiredBytes + cacheBudget_.optionalBytes;
  const uint64_t byteLimit = used < cacheBudget_.limit ? cacheBudget_.limit - used : 0;
  if (status) {
    status = pdfOpenTrackedCacheWriter(config_.io, runtime->fullPath, runtime->relativePath, PdfCacheFileKind::Optional,
                                       byteLimit, &runtime->writer);
  }
  if (status) {
    status =
        runtime->pixelWriter.begin({&runtime->writer, writeRasterCache}, runtime->layout.width, runtime->layout.height);
  }
  if (!status) {
    if (runtime->writer.open) {
      pdfAbortTrackedCacheFile(&runtime->writer);
    } else if (runtime->fullPath[0] != '\0') {
      (void)config_.io.remove(config_.io.context, runtime->fullPath, false);
    }
    runtime->~MaskCompositeRuntime();
    maskCompositeRuntimeActive_ = false;
  }
  return status;
}

PdfStepResult PdfPreparation::stepMaskCompositeRecord(PdfWorkBudget& budget) {
  if (!maskCompositeRuntimeActive_ || maskSpool_ == nullptr || !runRecords_) {
    return PdfStepResult::failure(PdfStatus::failure(PdfError::InvalidArgument));
  }
  auto* runtime = reinterpret_cast<MaskCompositeRuntime*>(runRecords_.get());
  const PdfMaskSpoolRecord& record = runtime->spoolRecord;
  const pixel_cache::Layout& layout = runtime->layout;
  if (runtime->nextRow < layout.height) {
    constexpr uint8_t kRowOperations = 4;
    const size_t inputBytes = static_cast<size_t>(layout.bytesPerRow) + layout.width;
    if (budget.operationsRemaining < kRowOperations || budget.bytesRemaining < inputBytes || budget.stopRequested()) {
      return PdfStepResult::paused();
    }
    for (uint8_t operation = 0; operation < kRowOperations; ++operation) {
      (void)budget.consumeOperation();
    }
    (void)budget.takeBytes(inputBytes);
    size_t bytesRead = 0;
    PdfStatus status = maskSpool_->read(
        record.baseOffset + pixel_cache::kHeaderSize + static_cast<uint64_t>(runtime->nextRow) * layout.bytesPerRow,
        sourceWindow_.get(), layout.bytesPerRow, &bytesRead);
    if (status && bytesRead != layout.bytesPerRow) {
      status = PdfStatus::failure(PdfError::UnexpectedEof,
                                  record.baseOffset + pixel_cache::kHeaderSize +
                                      static_cast<uint64_t>(runtime->nextRow) * layout.bytesPerRow + bytesRead);
    }
    if (status) {
      status = maskSpool_->read(record.alphaOffset + static_cast<uint64_t>(runtime->nextRow) * layout.width,
                                pageText_.get(), layout.width, &bytesRead);
      if (status && bytesRead != layout.width) {
        status =
            PdfStatus::failure(PdfError::UnexpectedEof,
                               record.alphaOffset + static_cast<uint64_t>(runtime->nextRow) * layout.width + bytesRead);
      }
    }
    if (status) {
      for (uint16_t x = 0; x < layout.width; ++x) {
        const uint8_t base = static_cast<uint8_t>((sourceWindow_[x / 4U] >> (6U - (x % 4U) * 2U)) & 0x03U);
        const uint16_t alpha = pageText_[x];
        const uint16_t luminance = static_cast<uint16_t>(base) * 85U;
        const uint16_t flattened = static_cast<uint16_t>((luminance * alpha + 255U * (255U - alpha) + 127U) / 255U);
        decoderOutput_[x] = static_cast<uint8_t>((flattened * 3U + 127U) / 255U);
      }
      status = runtime->pixelWriter.writeRow(runtime->nextRow, decoderOutput_.get(), layout.width);
    }
    if (!status) {
      return PdfStepResult::failure(status);
    }
    ++runtime->nextRow;
    return PdfStepResult::paused();
  }

  PdfStatus status = runtime->pixelWriter.finish();
  if (status) {
    status = pdfCloseTrackedCacheFile(&runtime->writer, &runtime->record);
  }
  if (status) {
    status = pdfReserveCacheBytes(&cacheBudget_, runtime->record.size, PdfCacheFileKind::Optional);
  }
  if (status) {
    status = appendImageFileRecord(runtime->record);
    if (status && coverImageFingerprintSelected_ && !coverImageRecordAvailable_ &&
        record.contentHash == coverImageContentHash_ && record.sourceCrc32 == coverImageSourceCrc32_) {
      coverImageSourceRecord_ = runtime->record;
      coverImageRecordAvailable_ = true;
      coverImageSourceJpeg_ = false;
    }
  }
  if (!status) {
    if (runtime->writer.open || runtime->writer.fullPath[0] != '\0') {
      pdfAbortTrackedCacheFile(&runtime->writer);
    }
  }
  runtime->~MaskCompositeRuntime();
  maskCompositeRuntimeActive_ = false;
  return status ? PdfStepResult::completed() : PdfStepResult::failure(status);
}

PdfStepResult PdfPreparation::restoreNavigation(PdfWorkBudget& budget) {
  if (navigationSpoolStage_ == NavigationSpoolStage::ReadyToRead) {
    if (navigation_ != nullptr || rasterBatch_ == nullptr || navigationSpoolPath_[0] == '\0' ||
        navigationSpoolHandle_.valid()) {
      return PdfStepResult::failure(PdfStatus::failure(PdfError::InvalidArgument));
    }
    PdfStatus status = closeSource();
    if (status) {
      status =
          config_.io.open(config_.io.context, navigationSpoolPath_, PdfCacheOpenMode::Read, &navigationSpoolHandle_);
    }
    if (!status) {
      abortNavigationSpool();
      return PdfStepResult::failure(status);
    }
    navigationSpoolOffset_ = 0;
    navigationSpoolReadCrc32_ = 0;
    navigationSpoolStage_ = NavigationSpoolStage::Reading;
    return PdfStepResult::paused();
  }

  if (navigationSpoolStage_ != NavigationSpoolStage::Reading || navigation_ != nullptr || rasterBatch_ == nullptr ||
      !navigationSpoolHandle_.valid() || !dictionary_ || navigationSpoolOffset_ > sizeof(NavigationWorkspace)) {
    return PdfStepResult::failure(PdfStatus::failure(PdfError::InvalidArgument, navigationSpoolOffset_));
  }
  if (navigationSpoolOffset_ < sizeof(NavigationWorkspace)) {
    if (!budget.consumeOperation()) {
      return PdfStepResult::paused();
    }
    const size_t remaining = sizeof(NavigationWorkspace) - static_cast<size_t>(navigationSpoolOffset_);
    const size_t requested = budget.takeBytes(std::min<size_t>(PdfLimits::SourceBufferBytes, remaining));
    if (requested == 0) {
      return PdfStepResult::paused();
    }
    size_t bytesRead = 0;
    PdfStatus status = config_.io.read(config_.io.context, navigationSpoolHandle_, navigationSpoolOffset_,
                                       dictionary_.get() + navigationSpoolOffset_, requested, &bytesRead);
    if (!status || bytesRead != requested) {
      if (status) {
        status = PdfStatus::failure(PdfError::UnexpectedEof, navigationSpoolOffset_ + bytesRead);
      }
      abortNavigationSpool();
      return PdfStepResult::failure(status);
    }
    navigationSpoolReadCrc32_ =
        pdfCacheCrc32(dictionary_.get() + navigationSpoolOffset_, bytesRead, navigationSpoolReadCrc32_);
    navigationSpoolOffset_ += bytesRead;
    return PdfStepResult::paused();
  }

  PdfStatus status = config_.io.close(config_.io.context, &navigationSpoolHandle_);
  navigationSpoolHandle_ = {};
  if (status && navigationSpoolReadCrc32_ != navigationSpoolCrc32_) {
    status = PdfStatus::failure(PdfError::Malformed);
  }
  if (!status) {
    abortNavigationSpool();
    return PdfStepResult::failure(status);
  }
  ++navigationSpoolReadCount_;
  navigation_ = reinterpret_cast<NavigationWorkspace*>(dictionary_.get());
  (void)config_.io.remove(config_.io.context, navigationSpoolPath_, false);
  navigationSpoolPath_[0] = '\0';
  navigationSpoolOffset_ = 0;
  navigationSpoolCrc32_ = 0;
  navigationSpoolReadCrc32_ = 0;
  navigationSpoolStage_ = NavigationSpoolStage::None;
  return PdfStepResult::completed();
}

PdfStepResult PdfPreparation::repairFailedImageSections(PdfWorkBudget& budget) {
  static_assert(sizeof(SectionRepairRuntime) <= kRasterDecoderWorkspaceOffset);
  if (navigation_ == nullptr || !runRecords_ || !sourceWindow_ || rasterBatch_ == nullptr ||
      (!sectionRepairRuntimeActive_ && imageBuildSpoolPath_[0] == '\0')) {
    return PdfStepResult::failure(PdfStatus::failure(PdfError::InvalidArgument));
  }

  if (!sectionRepairRuntimeActive_) {
    if (failedRasterImages_ == 0) {
      imageBuildSpool_.remove();
      imageBuildSpoolPath_[0] = '\0';
      return PdfStepResult::completed();
    }
    if (rasterBatch_->imageBuildRead.stage == PdfImageSpoolReadStage::Idle) {
      PdfStatus status = closeSource();
      if (status) {
        status = imageBuildSpool_.beginRead(config_.io, imageBuildSpoolPath_, sourceWindow_.get(),
                                            PdfLimits::SourceBufferBytes, &rasterBatch_->imageBuildRead);
      }
      return status ? PdfStepResult::paused() : PdfStepResult::failure(status);
    }
    if (rasterBatch_->imageBuildRead.stage != PdfImageSpoolReadStage::Complete) {
      const PdfStepResult opened = imageBuildSpool_.stepReadOpen(rasterBatch_->imageBuildRead, budget);
      return opened.complete() ? PdfStepResult::paused() : opened;
    }
    auto* runtime = new (runRecords_.get()) SectionRepairRuntime{};
    sectionRepairRuntimeActive_ = true;
    runtime->stage = SectionRepairStage::CollectTags;
    return PdfStepResult::paused();
  }

  auto* runtime = reinterpret_cast<SectionRepairRuntime*>(runRecords_.get());
  if (runtime->stage == SectionRepairStage::CollectTags) {
    if (runtime->nextCandidate < imageBuildSpool_.recordCount()) {
      const uint8_t index = runtime->nextCandidate++;
      if ((failedRasterImages_ & (UINT64_C(1) << index)) == 0) {
        return PdfStepResult::paused();
      }
      PdfStatus status = imageBuildSpool_.readRecord(index, &activeRasterCandidate_);
      if (!status) {
        return PdfStepResult::failure(status);
      }
      if (runtime->failedTagCount == PDF_IMAGE_BUILD_SPOOL_MAX_RECORDS ||
          activeRasterCandidate_.sectionIndex >= sectionCount_) {
        return PdfStepResult::failure(PdfStatus::failure(PdfError::Malformed, index));
      }
      const PdfRequiredFileRecord& section = navigation_->sectionFiles[activeRasterCandidate_.sectionIndex];
      const uint64_t tagEnd =
          static_cast<uint64_t>(activeRasterCandidate_.tagOffset) + activeRasterCandidate_.tagLength;
      if (activeRasterCandidate_.tagLength == 0 || tagEnd > section.size) {
        return PdfStepResult::failure(PdfStatus::failure(PdfError::Malformed, index));
      }
      runtime->tags[runtime->failedTagCount++] = {
          activeRasterCandidate_.tagOffset,
          activeRasterCandidate_.sectionIndex,
          activeRasterCandidate_.tagLength,
      };
      return PdfStepResult::paused();
    }
    PdfStatus status = imageBuildSpool_.closeRead();
    rasterBatch_->imageBuildRead = {};
    if (!status || runtime->failedTagCount == 0) {
      return PdfStepResult::failure(status ? PdfStatus::failure(PdfError::Malformed) : status);
    }
    imageBuildSpool_.remove();
    imageBuildSpoolPath_[0] = '\0';
    std::sort(runtime->tags, runtime->tags + runtime->failedTagCount,
              [](const FailedImageTagRange& left, const FailedImageTagRange& right) {
                return left.sectionIndex < right.sectionIndex ||
                       (left.sectionIndex == right.sectionIndex && left.offset < right.offset);
              });
    for (uint8_t index = 1; index < runtime->failedTagCount; ++index) {
      const FailedImageTagRange& previous = runtime->tags[index - 1U];
      const FailedImageTagRange& current = runtime->tags[index];
      if (previous.sectionIndex == current.sectionIndex &&
          static_cast<uint64_t>(previous.offset) + previous.length > current.offset) {
        return PdfStepResult::failure(PdfStatus::failure(PdfError::Malformed, index));
      }
    }
    runtime->stage = SectionRepairStage::OpenOriginal;
    return PdfStepResult::paused();
  }

  if (runtime->stage == SectionRepairStage::OpenOriginal) {
    if (runtime->nextTag >= runtime->failedTagCount) {
      runtime->stage = SectionRepairStage::Done;
      return PdfStepResult::paused();
    }
    runtime->sectionIndex = runtime->tags[runtime->nextTag].sectionIndex;
    runtime->sectionTagEnd = runtime->nextTag;
    while (runtime->sectionTagEnd < runtime->failedTagCount &&
           runtime->tags[runtime->sectionTagEnd].sectionIndex == runtime->sectionIndex) {
      ++runtime->sectionTagEnd;
    }
    runtime->originalRecord = navigation_->sectionFiles[runtime->sectionIndex];
    const int originalLength = std::snprintf(runtime->originalPath, sizeof(runtime->originalPath), "%s/%s", cacheRoot_,
                                             runtime->originalRecord.path);
    const int temporaryLength =
        std::snprintf(runtime->temporaryPath, sizeof(runtime->temporaryPath), "%s/gen_%lu/build.section-repair",
                      cacheRoot_, static_cast<unsigned long>(generation_));
    if (originalLength <= 0 || static_cast<size_t>(originalLength) >= sizeof(runtime->originalPath) ||
        temporaryLength <= 0 || static_cast<size_t>(temporaryLength) >= sizeof(runtime->temporaryPath)) {
      return PdfStepResult::failure(PdfStatus::failure(PdfError::LimitExceeded));
    }
    (void)config_.io.remove(config_.io.context, runtime->temporaryPath, false);
    PdfStatus status =
        config_.io.open(config_.io.context, runtime->originalPath, PdfCacheOpenMode::Read, &runtime->reader);
    if (status) {
      status = config_.io.open(config_.io.context, runtime->temporaryPath, PdfCacheOpenMode::WriteTruncate,
                               &runtime->temporaryWriter);
    }
    if (!status) {
      return PdfStepResult::failure(status);
    }
    runtime->offset = 0;
    runtime->stage = SectionRepairStage::PatchToTemporary;
    return PdfStepResult::paused();
  }

  if (runtime->stage == SectionRepairStage::PatchToTemporary) {
    if (runtime->offset == runtime->originalRecord.size) {
      runtime->stage = SectionRepairStage::CloseTemporary;
      return PdfStepResult::paused();
    }
    const size_t requested = static_cast<size_t>(
        std::min<uint64_t>(PdfLimits::SourceBufferBytes, runtime->originalRecord.size - runtime->offset));
    size_t bytesRead = 0;
    PdfStatus status = config_.io.read(config_.io.context, runtime->reader, runtime->offset, sourceWindow_.get(),
                                       requested, &bytesRead);
    if (status && bytesRead != requested) {
      status = PdfStatus::failure(PdfError::UnexpectedEof, runtime->offset + bytesRead);
    }
    const uint64_t chunkEnd = runtime->offset + requested;
    for (uint8_t index = runtime->nextTag; status && index < runtime->sectionTagEnd; ++index) {
      const uint64_t tagStart = runtime->tags[index].offset;
      const uint64_t tagEnd = tagStart + runtime->tags[index].length;
      const uint64_t overlapStart = std::max(runtime->offset, tagStart);
      const uint64_t overlapEnd = std::min(chunkEnd, tagEnd);
      if (overlapStart < overlapEnd) {
        std::memset(sourceWindow_.get() + static_cast<size_t>(overlapStart - runtime->offset), ' ',
                    static_cast<size_t>(overlapEnd - overlapStart));
      }
    }
    size_t bytesWritten = 0;
    if (status) {
      status =
          config_.io.write(config_.io.context, runtime->temporaryWriter, sourceWindow_.get(), requested, &bytesWritten);
      if (status && bytesWritten != requested) {
        status = PdfStatus::failure(PdfError::IoFailure, runtime->offset + bytesWritten);
      }
    }
    if (!status) {
      return PdfStepResult::failure(status);
    }
    runtime->offset += requested;
    return PdfStepResult::paused();
  }

  if (runtime->stage == SectionRepairStage::CloseTemporary) {
    PdfStatus status = closeDurableWriter(config_.io, &runtime->temporaryWriter);
    const PdfStatus closeStatus =
        runtime->reader.valid() ? config_.io.close(config_.io.context, &runtime->reader) : PdfStatus::success();
    if (status && !closeStatus) {
      status = closeStatus;
    }
    if (!status) {
      return PdfStepResult::failure(status);
    }
    runtime->stage = SectionRepairStage::OpenTemporary;
    return PdfStepResult::paused();
  }

  if (runtime->stage == SectionRepairStage::OpenTemporary) {
    PdfStatus status =
        config_.io.open(config_.io.context, runtime->temporaryPath, PdfCacheOpenMode::Read, &runtime->reader);
    if (status) {
      status =
          pdfOpenTrackedCacheWriter(config_.io, runtime->originalPath, runtime->originalRecord.path,
                                    PdfCacheFileKind::Required, runtime->originalRecord.size, &runtime->finalWriter);
    }
    if (!status) {
      return PdfStepResult::failure(status);
    }
    runtime->offset = 0;
    runtime->stage = SectionRepairStage::CopyToFinal;
    return PdfStepResult::paused();
  }

  if (runtime->stage == SectionRepairStage::CopyToFinal) {
    if (runtime->offset == runtime->originalRecord.size) {
      runtime->stage = SectionRepairStage::CloseFinal;
      return PdfStepResult::paused();
    }
    const size_t requested = static_cast<size_t>(
        std::min<uint64_t>(PdfLimits::SourceBufferBytes, runtime->originalRecord.size - runtime->offset));
    size_t bytesRead = 0;
    PdfStatus status = config_.io.read(config_.io.context, runtime->reader, runtime->offset, sourceWindow_.get(),
                                       requested, &bytesRead);
    if (status && bytesRead != requested) {
      status = PdfStatus::failure(PdfError::UnexpectedEof, runtime->offset + bytesRead);
    }
    if (status) {
      status = pdfWriteTrackedCacheFile(&runtime->finalWriter, sourceWindow_.get(), requested);
    }
    if (!status) {
      return PdfStepResult::failure(status);
    }
    runtime->offset += requested;
    return PdfStepResult::paused();
  }

  if (runtime->stage == SectionRepairStage::CloseFinal) {
    PdfStatus status = pdfCloseTrackedCacheFile(&runtime->finalWriter, &runtime->finalRecord);
    const PdfStatus closeStatus =
        runtime->reader.valid() ? config_.io.close(config_.io.context, &runtime->reader) : PdfStatus::success();
    if (status && !closeStatus) {
      status = closeStatus;
    }
    (void)config_.io.remove(config_.io.context, runtime->temporaryPath, false);
    runtime->temporaryPath[0] = '\0';
    if (!status) {
      if (runtime->finalWriter.failed) {
        (void)config_.io.remove(config_.io.context, runtime->originalPath, false);
      }
      return PdfStepResult::failure(status);
    }
    if (runtime->finalRecord.size != runtime->originalRecord.size ||
        runtime->finalRecord.pathLength != runtime->originalRecord.pathLength ||
        std::memcmp(runtime->finalRecord.path, runtime->originalRecord.path, runtime->originalRecord.pathLength) != 0) {
      return PdfStepResult::failure(PdfStatus::failure(PdfError::Malformed, runtime->sectionIndex));
    }
    navigation_->sectionFiles[runtime->sectionIndex] = runtime->finalRecord;
    runtime->nextTag = runtime->sectionTagEnd;
    runtime->stage =
        runtime->nextTag < runtime->failedTagCount ? SectionRepairStage::OpenOriginal : SectionRepairStage::Done;
    return PdfStepResult::paused();
  }

  if (runtime->stage == SectionRepairStage::Done) {
    runtime->~SectionRepairRuntime();
    sectionRepairRuntimeActive_ = false;
    return PdfStepResult::completed();
  }

  return PdfStepResult::failure(PdfStatus::failure(PdfError::InvalidArgument));
}

PdfStatus PdfPreparation::beginCurrentPageContent() {
  static_assert(sizeof(PlacementWorkspace) <= PdfLimits::OperandOrderHistogramBytes);
  if (!resolver_.has_value() || navigation_ == nullptr || currentPageIndex_ >= pageCount_ || !operandScratch_) {
    return PdfStatus::failure(PdfError::InvalidArgument, currentPageIndex_);
  }
  abortInlineNavigationSpill();
  const PdfPageInfo& page = navigation_->pages[currentPageIndex_];
  if (page.contentCount != 1) {
    return PdfStatus::failure(page.contentCount == 0 ? PdfError::NoReadableText : PdfError::Unsupported,
                              currentPageIndex_);
  }
  currentContentIndex_ = 0;
  transcriptLength_ = 0;
  extractedBlockCount_ = 0;
  currentBlockIndex_ = 0;
  currentFontSize_ = 0;
  lastNumericValue_ = 0;
  lastContentNameLength_ = 0;
  resetInlineImageDictionaryState();
  placement_ = new (operandScratch_.get()) PlacementWorkspace{};
  return resolver_->begin(page.contents[0]);
}

PdfStatus PdfPreparation::finishContentObject() {
  if (!resolver_.has_value() || !resolver_->result().hasStream) {
    return PdfStatus::failure(PdfError::Malformed);
  }
  const PdfResolvedObject content = resolver_->result();
  PdfStatus status = pdfInitializeByteRange(source(), content.streamOffset, content.streamLength, &contentRange_);
  if (!status) {
    return status;
  }
  contentLexer_.emplace(pdfByteRangeSource(contentRange_), sourceWindow_.get(), PdfLimits::SourceBufferBytes);
  return PdfStatus::success();
}

PdfStatus PdfPreparation::finishExtractedPage() {
  if (inlineImageDictionaryActive_ || inlineImageAwaitingData_) {
    return PdfStatus::failure(PdfError::Malformed, currentPageIndex_);
  }
  if (transcriptLength_ == 0 || extractedBlockCount_ == 0) {
    return PdfStatus::failure(PdfError::NoReadableText, currentPageIndex_);
  }
  for (uint8_t index = currentPageImageStart_; index < currentPageImageEnd_; ++index) {
    PreparedImageCandidate& candidate = navigation_->imageCandidates[index];
    if (candidate.placementCount == 0 || candidate.semanticBlockIndex >= extractedBlockCount_) {
      continue;
    }
    PdfImageMeaningDecision decision{};
    uint8_t repetitionCount = std::max(candidate.placementCount, candidate.documentRepetitionCount);
    if (repeatedPageBandDecoration(candidate.placement, navigation_->pageWidths[currentPageIndex_],
                                   navigation_->pageHeights[currentPageIndex_])) {
      repetitionCount = std::max<uint8_t>(repetitionCount, 3);
    }
    const PdfStatus status = pdfClassifyMeaningfulImage(
        {candidate.placement, navigation_->pageWidths[currentPageIndex_], navigation_->pageHeights[currentPageIndex_],
         static_cast<uint16_t>(currentPageIndex_), nextAnchorOrdinal_, std::max<uint8_t>(1, repetitionCount), true,
         !meaningfulEarlyImageSeen_},
        &decision);
    if (!status) {
      return status;
    }
    candidate.placed = decision.retain;
    candidate.coverCandidate = decision.coverCandidate;
    if (decision.retain && currentPageIndex_ < PdfLimits::MaxCoverScanPages) {
      meaningfulEarlyImageSeen_ = true;
    }
    if (!decision.retain) {
      warningFlags_ |= kWarningOptionalImageOmitted;
    }
  }
  currentPageImageCandidate_ =
      currentPageImageStart_ < currentPageImageEnd_ ? static_cast<int8_t>(currentPageImageStart_) : -1;
  contentLexer_.reset();
  placement_ = nullptr;
  return PdfStatus::success();
}

void PdfPreparation::resetInlineImageDictionaryState() {
  inlineImageParameters_ = {};
  inlineImageParameters_.predictor = 1;
  inlineImageParameters_.decode = PdfImageDecode::Normal;
  inlineImageParameters_.colorSpace = PdfImageColorSpace::Gray;
  std::fill(std::begin(inlineImageFilters_), std::end(inlineImageFilters_), PdfStreamFilter::Unsupported);
  std::memset(inlineImageKey_, 0, sizeof(inlineImageKey_));
  std::fill(std::begin(inlineImageDecodeValues_), std::end(inlineImageDecodeValues_), 0);
  inlineImageRange_ = {};
  inlineImageDecoder_.reset();
  inlineCapturedJpeg_ = {};
  inlineImageIdEnd_ = 0;
  inlineImageDataOffset_ = 0;
  inlineImageScanOffset_ = 0;
  inlineImageEncodedLength_ = 0;
  inlineImageScanPendingBytes_ = 0;
  inlineImageScanPendingBufferOffset_ = 0;
  inlineImageFilterCount_ = 0;
  inlineImageKeyLength_ = 0;
  inlineImageDecodeValueCount_ = 0;
  inlineImagePredictorColumns_ = 1;
  inlineImagePredictorColors_ = 1;
  inlineImagePredictorBitsPerComponent_ = 8;
  inlineImageDictionaryActive_ = false;
  inlineImageAwaitingData_ = false;
  inlineImageJpeg_ = false;
  inlineImageScanSawJpegMarker_ = false;
  inlineImageCaptureStarted_ = false;
  inlineImageCaptureFailed_ = false;
  inlineImageSupported_ = true;
  inlineImageContainer_ = InlineImageContainer::None;
  inlineIndexedStage_ = InlineIndexedStage::Family;
}

void PdfPreparation::finalizeInlineImageDictionary() {
  if (inlineImageContainer_ != InlineImageContainer::None || inlineImageKeyLength_ != 0 ||
      inlineImageParameters_.width == 0 || inlineImageParameters_.height == 0 ||
      inlineImageParameters_.width > PdfLimits::MaxImageDimension ||
      inlineImageParameters_.height > PdfLimits::MaxImageDimension) {
    inlineImageSupported_ = false;
  }

  const uint8_t bits = inlineImageParameters_.bitsPerComponent;
  switch (inlineImageParameters_.colorSpace) {
    case PdfImageColorSpace::RGB:
      inlineImageSupported_ &= bits == 8U;
      break;
    case PdfImageColorSpace::Gray:
    case PdfImageColorSpace::IndexedGray:
    case PdfImageColorSpace::IndexedRGB:
      inlineImageSupported_ &= bits == 1U || bits == 2U || bits == 4U || bits == 8U;
      break;
    case PdfImageColorSpace::ImageMask:
      inlineImageSupported_ &= bits == 1U;
      break;
  }

  const bool indexed = inlineImageParameters_.colorSpace == PdfImageColorSpace::IndexedGray ||
                       inlineImageParameters_.colorSpace == PdfImageColorSpace::IndexedRGB;
  if (indexed) {
    const size_t components = inlineImageParameters_.colorSpace == PdfImageColorSpace::IndexedRGB ? 3U : 1U;
    const size_t required = static_cast<size_t>(inlineImageParameters_.paletteEntries) * components;
    inlineImageSupported_ &= inlineIndexedStage_ == InlineIndexedStage::Complete &&
                             inlineImageParameters_.palette != nullptr && inlineImageParameters_.paletteEntries != 0 &&
                             inlineImageParameters_.paletteEntries <= 256U &&
                             inlineImageParameters_.paletteBytes == required;
  }

  if (inlineImageDecodeValueCount_ != 0) {
    const uint8_t expectedCount = inlineImageParameters_.colorSpace == PdfImageColorSpace::RGB ? 6U : 2U;
    inlineImageSupported_ &= inlineImageDecodeValueCount_ == expectedCount;
    const int16_t upper = bits == 0 || bits > 8U ? 0 : static_cast<int16_t>((1U << bits) - 1U);
    bool orientationSet = false;
    bool inverted = false;
    for (uint8_t index = 0; index + 1U < inlineImageDecodeValueCount_; index += 2U) {
      const int16_t first = inlineImageDecodeValues_[index];
      const int16_t second = inlineImageDecodeValues_[index + 1U];
      const bool normalPair = first == 0 && second == upper;
      const bool invertedPair = first == upper && second == 0;
      if (!normalPair && !invertedPair) {
        inlineImageSupported_ = false;
        break;
      }
      if (!orientationSet) {
        inverted = invertedPair;
        orientationSet = true;
      } else if (inverted != invertedPair) {
        inlineImageSupported_ = false;
        break;
      }
    }
    inlineImageParameters_.decode = inverted ? PdfImageDecode::Inverted : PdfImageDecode::Normal;
  }

  const uint8_t predictor = inlineImageParameters_.predictor;
  const bool supportedPredictor = predictor == 1U || predictor == 2U || (predictor >= 10U && predictor <= 15U);
  inlineImageSupported_ &= supportedPredictor;
  if (predictor != 1U) {
    const bool flate = inlineImageFilterCount_ == 1U && inlineImageFilters_[0] == PdfStreamFilter::Flate;
    const uint8_t components = inlineImageParameters_.colorSpace == PdfImageColorSpace::RGB ? 3U : 1U;
    inlineImageSupported_ &= flate && inlineImagePredictorColors_ == components &&
                             inlineImagePredictorBitsPerComponent_ == bits &&
                             inlineImagePredictorColumns_ == inlineImageParameters_.width;
  }
}

PdfStatus PdfPreparation::initializeInlineImageDataOffset(const PdfByteSource& contentSource) {
  if (inlineImageDataOffset_ != 0) {
    return PdfStatus::success();
  }
  uint8_t separator = 0;
  size_t bufferedOffset = 0;
  size_t bufferedBytes = 0;
  const bool firstBuffered =
      contentLexer_.has_value() &&
      contentLexer_->bufferedRange(inlineImageIdEnd_, &bufferedOffset, &bufferedBytes) && bufferedBytes != 0;
  PdfStatus status = PdfStatus::success();
  if (firstBuffered) {
    separator = sourceWindow_[bufferedOffset];
  } else {
    status = pdfReadExact(contentSource, inlineImageIdEnd_, &separator, 1);
  }
  if (!status || !pdfWhitespace(separator)) {
    return status ? PdfStatus::failure(PdfError::Malformed, inlineImageIdEnd_) : status;
  }
  inlineImageDataOffset_ = inlineImageIdEnd_ + 1U;
  if (separator == '\r') {
    uint8_t possibleLineFeed = 0;
    size_t lineFeedOffset = 0;
    size_t lineFeedBytes = 0;
    const bool lineFeedBuffered =
        contentLexer_.has_value() &&
        contentLexer_->bufferedRange(inlineImageIdEnd_ + 1U, &lineFeedOffset, &lineFeedBytes) &&
        lineFeedBytes != 0;
    if (lineFeedBuffered) {
      possibleLineFeed = sourceWindow_[lineFeedOffset];
    } else {
      status = pdfReadExact(contentSource, inlineImageIdEnd_ + 1U, &possibleLineFeed, 1);
      if (!status) {
        return status;
      }
    }
    if (possibleLineFeed == '\n') {
      ++inlineImageDataOffset_;
    }
  }

  if (!inlineImageSupported_ || !inlineImageJpeg_ || !contentLexer_.has_value() ||
      !contentLexer_->bufferedRange(inlineImageDataOffset_, &bufferedOffset, &bufferedBytes) ||
      bufferedBytes == 0) {
    return PdfStatus::success();
  }
  size_t capturedBytes = bufferedBytes;
  for (size_t index = 0; index < bufferedBytes; ++index) {
    const uint8_t byte = sourceWindow_[bufferedOffset + index];
    if (inlineImageScanSawJpegMarker_ && byte == 0xD9) {
      capturedBytes = index + 1U;
      inlineImageEncodedLength_ = capturedBytes;
      break;
    }
    inlineImageScanSawJpegMarker_ = byte == 0xFF;
  }
  inlineImageScanPendingBufferOffset_ = bufferedOffset;
  inlineImageScanPendingBytes_ = capturedBytes;
  return PdfStatus::success();
}

PdfStatus PdfPreparation::consumeInlineImageToken(const PdfToken& token) {
  if (!inlineImageDictionaryActive_) {
    return PdfStatus::failure(PdfError::InvalidArgument);
  }

  if (token.kind == PdfTokenKind::ArrayBegin) {
    if (inlineImageContainer_ != InlineImageContainer::None) {
      inlineImageSupported_ = false;
    } else if (keyEquals(inlineImageKey_, inlineImageKeyLength_, "F", "Filter")) {
      inlineImageContainer_ = InlineImageContainer::FilterArray;
    } else if (keyEquals(inlineImageKey_, inlineImageKeyLength_, "D", "Decode")) {
      inlineImageContainer_ = InlineImageContainer::DecodeArray;
      inlineImageDecodeValueCount_ = 0;
    } else if (keyEquals(inlineImageKey_, inlineImageKeyLength_, "CS", "ColorSpace")) {
      inlineImageContainer_ = InlineImageContainer::ColorSpaceArray;
      inlineIndexedStage_ = InlineIndexedStage::Family;
    } else {
      inlineImageSupported_ = false;
    }
    inlineImageKeyLength_ = 0;
    return PdfStatus::success();
  }

  if (token.kind == PdfTokenKind::ArrayEnd) {
    if (inlineImageContainer_ == InlineImageContainer::ColorSpaceArray &&
        inlineIndexedStage_ != InlineIndexedStage::Complete) {
      inlineImageSupported_ = false;
    } else if (inlineImageContainer_ != InlineImageContainer::FilterArray &&
               inlineImageContainer_ != InlineImageContainer::DecodeArray &&
               inlineImageContainer_ != InlineImageContainer::ColorSpaceArray) {
      inlineImageSupported_ = false;
    }
    inlineImageContainer_ = InlineImageContainer::None;
    inlineImageKeyLength_ = 0;
    return PdfStatus::success();
  }

  if (token.kind == PdfTokenKind::DictionaryBegin) {
    if (inlineImageContainer_ == InlineImageContainer::None &&
        keyEquals(inlineImageKey_, inlineImageKeyLength_, "DP", "DecodeParms")) {
      inlineImageContainer_ = InlineImageContainer::DecodeParametersDictionary;
    } else {
      inlineImageSupported_ = false;
    }
    inlineImageKeyLength_ = 0;
    return PdfStatus::success();
  }

  if (token.kind == PdfTokenKind::DictionaryEnd) {
    if (inlineImageContainer_ != InlineImageContainer::DecodeParametersDictionary) {
      inlineImageSupported_ = false;
    }
    inlineImageContainer_ = InlineImageContainer::None;
    inlineImageKeyLength_ = 0;
    return PdfStatus::success();
  }

  if (token.kind == PdfTokenKind::Name) {
    if (inlineImageContainer_ == InlineImageContainer::ColorSpaceArray) {
      if (inlineIndexedStage_ == InlineIndexedStage::Family &&
          (tokenEquals(token, "I") || tokenEquals(token, "Indexed"))) {
        inlineIndexedStage_ = InlineIndexedStage::Base;
      } else if (inlineIndexedStage_ == InlineIndexedStage::Base &&
                 (tokenEquals(token, "G") || tokenEquals(token, "DeviceGray"))) {
        inlineImageParameters_.colorSpace = PdfImageColorSpace::IndexedGray;
        inlineIndexedStage_ = InlineIndexedStage::High;
      } else if (inlineIndexedStage_ == InlineIndexedStage::Base &&
                 (tokenEquals(token, "RGB") || tokenEquals(token, "DeviceRGB"))) {
        inlineImageParameters_.colorSpace = PdfImageColorSpace::IndexedRGB;
        inlineIndexedStage_ = InlineIndexedStage::High;
      } else {
        inlineImageSupported_ = false;
      }
      return PdfStatus::success();
    }

    const bool filterValue = inlineImageContainer_ == InlineImageContainer::FilterArray ||
                             (inlineImageContainer_ == InlineImageContainer::None &&
                              keyEquals(inlineImageKey_, inlineImageKeyLength_, "F", "Filter"));
    if (filterValue) {
      PdfStreamFilter filter = PdfStreamFilter::Unsupported;
      if (tokenEquals(token, "Fl") || tokenEquals(token, "FlateDecode")) {
        filter = PdfStreamFilter::Flate;
      } else if (tokenEquals(token, "AHx") || tokenEquals(token, "ASCIIHexDecode")) {
        filter = PdfStreamFilter::ASCIIHex;
      } else if (tokenEquals(token, "A85") || tokenEquals(token, "ASCII85Decode")) {
        filter = PdfStreamFilter::ASCII85;
      } else if (tokenEquals(token, "DCT") || tokenEquals(token, "DCTDecode")) {
        if (inlineImageFilterCount_ == 0 && !inlineImageJpeg_) {
          inlineImageJpeg_ = true;
        } else {
          inlineImageSupported_ = false;
        }
      } else {
        inlineImageSupported_ = false;
      }
      if (filter != PdfStreamFilter::Unsupported) {
        if (inlineImageJpeg_ || inlineImageFilterCount_ >= PdfLimits::MaxFiltersPerStream) {
          inlineImageSupported_ = false;
        } else {
          inlineImageFilters_[inlineImageFilterCount_++] = filter;
        }
      }
      if (inlineImageContainer_ != InlineImageContainer::FilterArray) {
        inlineImageKeyLength_ = 0;
      }
      return PdfStatus::success();
    }

    if (inlineImageKeyLength_ == 0 && (inlineImageContainer_ == InlineImageContainer::None ||
                                       inlineImageContainer_ == InlineImageContainer::DecodeParametersDictionary)) {
      if (token.length == 0 || token.length >= sizeof(inlineImageKey_)) {
        inlineImageSupported_ = false;
      } else {
        std::memcpy(inlineImageKey_, token.bytes, token.length);
        inlineImageKeyLength_ = static_cast<uint8_t>(token.length);
      }
      return PdfStatus::success();
    }

    if (inlineImageContainer_ == InlineImageContainer::None &&
        keyEquals(inlineImageKey_, inlineImageKeyLength_, "CS", "ColorSpace")) {
      if (tokenEquals(token, "G") || tokenEquals(token, "DeviceGray")) {
        inlineImageParameters_.colorSpace = PdfImageColorSpace::Gray;
      } else if (tokenEquals(token, "RGB") || tokenEquals(token, "DeviceRGB")) {
        inlineImageParameters_.colorSpace = PdfImageColorSpace::RGB;
      } else {
        inlineImageSupported_ = false;
      }
      inlineImageKeyLength_ = 0;
      return PdfStatus::success();
    }

    inlineImageSupported_ = false;
    inlineImageKeyLength_ = 0;
    return PdfStatus::success();
  }

  if (token.kind == PdfTokenKind::Integer || token.kind == PdfTokenKind::Real) {
    int16_t value = 0;
    if (!parseTokenInt16(token, &value) || value < 0) {
      inlineImageSupported_ = false;
      inlineImageKeyLength_ = 0;
      return PdfStatus::success();
    }

    if (inlineImageContainer_ == InlineImageContainer::DecodeArray) {
      if (inlineImageDecodeValueCount_ >= std::size(inlineImageDecodeValues_)) {
        inlineImageSupported_ = false;
      } else {
        inlineImageDecodeValues_[inlineImageDecodeValueCount_++] = value;
      }
      return PdfStatus::success();
    }

    if (inlineImageContainer_ == InlineImageContainer::ColorSpaceArray) {
      if (inlineIndexedStage_ != InlineIndexedStage::High || value > 255) {
        inlineImageSupported_ = false;
      } else {
        inlineImageParameters_.paletteEntries = static_cast<uint16_t>(value + 1);
        inlineIndexedStage_ = InlineIndexedStage::Palette;
      }
      return PdfStatus::success();
    }

    if (inlineImageContainer_ == InlineImageContainer::DecodeParametersDictionary) {
      if (keyEquals(inlineImageKey_, inlineImageKeyLength_, "Predictor", "Predictor")) {
        inlineImageParameters_.predictor = static_cast<uint8_t>(value);
      } else if (keyEquals(inlineImageKey_, inlineImageKeyLength_, "Colors", "Colors")) {
        inlineImagePredictorColors_ = static_cast<uint8_t>(value);
      } else if (keyEquals(inlineImageKey_, inlineImageKeyLength_, "BPC", "BitsPerComponent")) {
        inlineImagePredictorBitsPerComponent_ = static_cast<uint8_t>(value);
      } else if (keyEquals(inlineImageKey_, inlineImageKeyLength_, "Columns", "Columns")) {
        inlineImagePredictorColumns_ = static_cast<uint16_t>(value);
      }
      inlineImageKeyLength_ = 0;
      return PdfStatus::success();
    }

    if (inlineImageContainer_ != InlineImageContainer::None) {
      inlineImageSupported_ = false;
    } else if (keyEquals(inlineImageKey_, inlineImageKeyLength_, "W", "Width")) {
      inlineImageParameters_.width = static_cast<uint16_t>(value);
    } else if (keyEquals(inlineImageKey_, inlineImageKeyLength_, "H", "Height")) {
      inlineImageParameters_.height = static_cast<uint16_t>(value);
    } else if (keyEquals(inlineImageKey_, inlineImageKeyLength_, "BPC", "BitsPerComponent")) {
      inlineImageParameters_.bitsPerComponent = static_cast<uint8_t>(value);
    }
    inlineImageKeyLength_ = 0;
    return PdfStatus::success();
  }

  if ((token.kind == PdfTokenKind::String || token.kind == PdfTokenKind::HexString) &&
      inlineImageContainer_ == InlineImageContainer::ColorSpaceArray &&
      inlineIndexedStage_ == InlineIndexedStage::Palette) {
    const size_t components = inlineImageParameters_.colorSpace == PdfImageColorSpace::IndexedRGB ? 3U : 1U;
    const size_t required = static_cast<size_t>(inlineImageParameters_.paletteEntries) * components;
    uint8_t* palette = nullptr;
    if (required == 0 || required > kPreparationPaletteBytes || token.length < required ||
        !allocateImagePalette(&palette)) {
      inlineImageSupported_ = false;
    } else {
      std::memcpy(palette, token.bytes, required);
      inlineImageParameters_.palette = palette;
      inlineImageParameters_.paletteBytes = required;
      inlineIndexedStage_ = InlineIndexedStage::Complete;
    }
    return PdfStatus::success();
  }

  if (token.kind == PdfTokenKind::Keyword && inlineImageKeyLength_ != 0) {
    if (inlineImageContainer_ == InlineImageContainer::None &&
        keyEquals(inlineImageKey_, inlineImageKeyLength_, "IM", "ImageMask") && tokenEquals(token, "true")) {
      inlineImageParameters_.colorSpace = PdfImageColorSpace::ImageMask;
      inlineImageParameters_.bitsPerComponent = 1;
    } else {
      inlineImageSupported_ = false;
    }
    inlineImageKeyLength_ = 0;
    return PdfStatus::success();
  }

  inlineImageSupported_ = false;
  inlineImageKeyLength_ = 0;
  return PdfStatus::success();
}

PdfStatus PdfPreparation::retainInlineImage(const uint64_t dataOffset, const uint64_t dataLength) {
  if (!inlineImageSupported_ || navigation_ == nullptr || imageCandidateCount_ >= kPreparationPageImageLimit ||
      dataLength == 0) {
    warningFlags_ |= kWarningOptionalImageOmitted;
    return PdfStatus::success();
  }
  PreparedImageCandidate& candidate = navigation_->imageCandidates[imageCandidateCount_];
  candidate.streamOffset = contentRange_.offset + dataOffset;
  candidate.streamLength = dataLength;
  candidate.width = inlineImageParameters_.width;
  candidate.height = inlineImageParameters_.height;
  candidate.parameters = inlineImageParameters_;
  if (candidate.parameters.colorSpace == PdfImageColorSpace::ImageMask && placement_ != nullptr) {
    candidate.parameters.imageMaskPaintLuminance = placement_->nonstrokingLuminance;
  }
  candidate.filterCount = inlineImageFilterCount_;
  std::copy_n(inlineImageFilters_, inlineImageFilterCount_, candidate.filters);
  candidate.nameLength = 6;
  std::memcpy(candidate.name, "inline", 7);
  candidate.jpeg = inlineImageJpeg_;
  candidate.raster = !inlineImageJpeg_;
  candidate.placement =
      orientPlacementToPage(matrixPlacement(placement_->current, candidate.width, candidate.height, true),
                            navigation_->pages[currentPageIndex_]);
  candidate.placement.imageMaskPaintLuminance = candidate.parameters.imageMaskPaintLuminance;
  candidate.placementCount = 1;
  candidate.semanticBlockIndex = extractedBlockCount_;
  currentPageImageCandidate_ = static_cast<int8_t>(imageCandidateCount_);
  ++imageCandidateCount_;
  currentPageImageEnd_ = imageCandidateCount_;
  return PdfStatus::success();
}

PdfStepResult PdfPreparation::finishInlineImageData(PdfWorkBudget& budget) {
  const bool jpegScanActive = inlineImageSupported_ && inlineImageJpeg_ && inlineImageDataOffset_ != 0;
  const bool navigationSpillActive = inlineNavigationSpillStage_ != InlineNavigationSpillStage::None;
  if (!inlineImageAwaitingData_ || (!contentLexer_.has_value() && !inlineImageDecoder_.has_value() && !jpegScanActive &&
                                    !navigationSpillActive && inlineImageEncodedLength_ == 0)) {
    return PdfStepResult::failure(PdfStatus::failure(PdfError::InvalidArgument));
  }
  PdfByteSource contentSource = pdfByteRangeSource(contentRange_);
  PdfStatus& status = operationStatus_;
  status = PdfStatus::success();
  status = initializeInlineImageDataOffset(contentSource);
  if (!status) {
    return PdfStepResult::failure(status);
  }
  const uint64_t dataOffset = inlineImageDataOffset_;
  uint64_t dataLength = 0;
  uint64_t resumeOffset = 0;
  bool jpegCaptureCompleted = false;
  if (inlineImageFilterCount_ == 0 && !inlineImageJpeg_ && inlineImageSupported_) {
    const uint8_t components = inlineImageParameters_.colorSpace == PdfImageColorSpace::RGB ? 3U : 1U;
    const uint64_t rowBits =
        static_cast<uint64_t>(inlineImageParameters_.width) * components * inlineImageParameters_.bitsPerComponent;
    const uint64_t rowBytes = (rowBits + 7U) / 8U;
    if (inlineImageParameters_.width == 0 || inlineImageParameters_.height == 0 || rowBytes == 0 ||
        rowBytes > PdfLimits::MaxDecodedImageRowBytes ||
        inlineImageParameters_.height > PdfLimits::MaxExpandedRequiredStreamBytes / rowBytes) {
      inlineImageSupported_ = false;
    } else {
      dataLength = rowBytes * inlineImageParameters_.height;
      uint8_t terminator[4]{};
      status = pdfReadExact(contentSource, dataOffset + dataLength, terminator, sizeof(terminator));
      if (!status || !pdfWhitespace(terminator[0]) || terminator[1] != 'E' || terminator[2] != 'I' ||
          !pdfWhitespace(terminator[3])) {
        return PdfStepResult::failure(status ? PdfStatus::failure(PdfError::Malformed, dataOffset + dataLength)
                                             : status);
      }
      resumeOffset = dataOffset + dataLength + sizeof(terminator);
      contentLexer_->setSource(contentSource, resumeOffset);
    }
  }

  if (resumeOffset == 0 && inlineImageSupported_ && inlineImageJpeg_) {
    if (contentLexer_.has_value()) {
      contentLexer_.reset();
    }
    if (dataOffset > contentSource.size || inlineImageScanOffset_ > contentSource.size - dataOffset) {
      return PdfStepResult::failure(PdfStatus::failure(PdfError::Malformed, dataOffset));
    }
    if (!inlineImageCaptureStarted_ && !inlineImageCaptureFailed_) {
      if (navigation_ == nullptr || imageCandidateCount_ >= kPreparationPageImageLimit) {
        inlineImageCaptureFailed_ = true;
        warningFlags_ |= kWarningOptionalImageOmitted;
      } else {
        status = navigation_->imageCache.beginJpegCapture(
            imageCandidateCount_, contentSource.size - dataOffset, &navigation_->imageCacheRuntime);
        if (!status) {
          inlineImageCaptureFailed_ = true;
          warningFlags_ |= kWarningOptionalImageOmitted;
        } else {
          inlineImageCaptureStarted_ = true;
        }
        return PdfStepResult::paused();
      }
    }
    if (inlineImageScanPendingBytes_ != 0) {
      if (!inlineImageCaptureStarted_) {
        if (!inlineImageCaptureFailed_) {
          return PdfStepResult::failure(PdfStatus::failure(PdfError::InvalidArgument, dataOffset));
        }
        inlineImageScanOffset_ += inlineImageScanPendingBytes_;
        inlineImageScanPendingBytes_ = 0;
        inlineImageScanPendingBufferOffset_ = 0;
        return PdfStepResult::paused();
      }
      if (budget.bytesRemaining < inlineImageScanPendingBytes_ || !budget.consumeOperation()) {
        return PdfStepResult::paused();
      }
      (void)budget.takeBytes(inlineImageScanPendingBytes_);
      status = navigation_->imageCache.appendJpegCapture(
          sourceWindow_.get() + inlineImageScanPendingBufferOffset_, inlineImageScanPendingBytes_,
          navigation_->imageCacheRuntime);
      inlineImageScanOffset_ += inlineImageScanPendingBytes_;
      inlineImageScanPendingBytes_ = 0;
      inlineImageScanPendingBufferOffset_ = 0;
      if (!status) {
        inlineImageCaptureStarted_ = false;
        inlineImageCaptureFailed_ = true;
        warningFlags_ |= kWarningOptionalImageOmitted;
      }
      return PdfStepResult::paused();
    }
    if (inlineImageEncodedLength_ != 0) {
      dataLength = inlineImageEncodedLength_;
    } else {
      if (!budget.consumeOperation()) {
        return PdfStepResult::paused();
      }
      const uint64_t remaining = contentSource.size - dataOffset - inlineImageScanOffset_;
      const size_t requested =
          budget.takeBytes(static_cast<size_t>(std::min<uint64_t>(remaining, PdfLimits::SourceBufferBytes)));
      if (requested == 0) {
        return PdfStepResult::failure(PdfStatus::failure(PdfError::UnexpectedEof, dataOffset + inlineImageScanOffset_));
      }
      size_t bytesRead = 0;
      status = contentSource.readAt(contentSource.context, dataOffset + inlineImageScanOffset_, sourceWindow_.get(),
                                    requested, &bytesRead);
      if (!status || bytesRead == 0 || bytesRead > requested) {
        return PdfStepResult::failure(
            status ? PdfStatus::failure(PdfError::UnexpectedEof, dataOffset + inlineImageScanOffset_) : status);
      }
      size_t capturedBytes = bytesRead;
      for (size_t index = 0; index < bytesRead; ++index) {
        const uint8_t byte = sourceWindow_[index];
        if (inlineImageScanSawJpegMarker_ && byte == 0xD9) {
          capturedBytes = index + 1U;
          inlineImageEncodedLength_ = inlineImageScanOffset_ + capturedBytes;
          break;
        }
        inlineImageScanSawJpegMarker_ = byte == 0xFF;
      }
      if (inlineImageCaptureStarted_) {
        inlineImageScanPendingBufferOffset_ = 0;
        inlineImageScanPendingBytes_ = capturedBytes;
      } else {
        inlineImageScanOffset_ += capturedBytes;
      }
      return PdfStepResult::paused();
    }
  }

  if (resumeOffset == 0 && inlineImageSupported_ && !inlineImageJpeg_ && inlineImageFilterCount_ != 0) {
    constexpr size_t decoderSourceOffset = (sizeof(PlacementWorkspace) + 7U) & ~size_t{7U};
    constexpr size_t decoderSourceBytes = 512;
    constexpr size_t decoderOutputOffset = decoderSourceOffset + decoderSourceBytes;
    constexpr size_t decoderOutputBytes = PdfLimits::OperandOrderHistogramBytes - decoderOutputOffset;
    static_assert(decoderOutputBytes >= 512);
    const bool requiresNavigationSpill =
        std::find(inlineImageFilters_, inlineImageFilters_ + inlineImageFilterCount_, PdfStreamFilter::Flate) !=
        inlineImageFilters_ + inlineImageFilterCount_;

    if (!requiresNavigationSpill && inlineNavigationSpillStage_ != InlineNavigationSpillStage::None) {
      return PdfStepResult::failure(PdfStatus::failure(PdfError::InvalidArgument, dataOffset));
    }

    if (requiresNavigationSpill && inlineNavigationSpillStage_ == InlineNavigationSpillStage::None &&
        inlineImageEncodedLength_ == 0 && !inlineImageDecoder_.has_value()) {
      if (navigation_ == nullptr || !dictionary_ || inlineNavigationSpoolPath_[0] != '\0' ||
          inlineNavigationSpoolHandle_.valid()) {
        return PdfStepResult::failure(PdfStatus::failure(PdfError::InvalidArgument, dataOffset));
      }
      contentLexer_.reset();
      const int pathLength =
          std::snprintf(inlineNavigationSpoolPath_, sizeof(inlineNavigationSpoolPath_), "%s/gen_%lu/build.inline-nav",
                        cacheRoot_, static_cast<unsigned long>(generation_));
      if (pathLength <= 0 || static_cast<size_t>(pathLength) >= sizeof(inlineNavigationSpoolPath_)) {
        inlineNavigationSpoolPath_[0] = '\0';
        return PdfStepResult::failure(PdfStatus::failure(PdfError::LimitExceeded));
      }
      status = config_.io.open(config_.io.context, inlineNavigationSpoolPath_, PdfCacheOpenMode::WriteTruncate,
                               &inlineNavigationSpoolHandle_);
      if (!status) {
        abortInlineNavigationSpill();
        return PdfStepResult::failure(status);
      }
      inlineNavigationSpoolOffset_ = 0;
      inlineNavigationSpoolCrc32_ = 0;
      inlineNavigationSpoolReadCrc32_ = 0;
      inlineNavigationSpillStage_ = InlineNavigationSpillStage::Writing;
      return PdfStepResult::paused();
    }

    if (inlineNavigationSpillStage_ == InlineNavigationSpillStage::Writing) {
      if (navigation_ == nullptr || !inlineNavigationSpoolHandle_.valid() ||
          inlineNavigationSpoolOffset_ > sizeof(NavigationWorkspace)) {
        return PdfStepResult::failure(PdfStatus::failure(PdfError::InvalidArgument, inlineNavigationSpoolOffset_));
      }
      if (inlineNavigationSpoolOffset_ < sizeof(NavigationWorkspace)) {
        if (!budget.consumeOperation()) {
          return PdfStepResult::paused();
        }
        const size_t remaining = sizeof(NavigationWorkspace) - static_cast<size_t>(inlineNavigationSpoolOffset_);
        const size_t requested = budget.takeBytes(std::min<size_t>(PdfLimits::SourceBufferBytes, remaining));
        if (requested == 0) {
          return PdfStepResult::paused();
        }
        size_t bytesWritten = 0;
        status = config_.io.write(config_.io.context, inlineNavigationSpoolHandle_,
                                  reinterpret_cast<const uint8_t*>(navigation_) + inlineNavigationSpoolOffset_,
                                  requested, &bytesWritten);
        if (!status || bytesWritten != requested) {
          if (status) {
            status = PdfStatus::failure(PdfError::IoFailure, inlineNavigationSpoolOffset_ + bytesWritten);
          }
          abortInlineNavigationSpill();
          return PdfStepResult::failure(status);
        }
        inlineNavigationSpoolCrc32_ =
            pdfCacheCrc32(reinterpret_cast<const uint8_t*>(navigation_) + inlineNavigationSpoolOffset_, bytesWritten,
                          inlineNavigationSpoolCrc32_);
        inlineNavigationSpoolOffset_ += bytesWritten;
        if (inlineNavigationSpoolOffset_ < sizeof(NavigationWorkspace)) {
          return PdfStepResult::paused();
        }
      }
      status = closeDurableWriter(config_.io, &inlineNavigationSpoolHandle_);
      if (!status) {
        abortInlineNavigationSpill();
        return PdfStepResult::failure(status);
      }
      inlineNavigationSpoolHandle_ = {};
      inlineNavigationSpoolOffset_ = 0;
      navigation_ = nullptr;
      inlineNavigationSpillStage_ = InlineNavigationSpillStage::Spilled;
      return PdfStepResult::paused();
    }

    if (inlineNavigationSpillStage_ == InlineNavigationSpillStage::Spilled && inlineImageEncodedLength_ != 0) {
      status = closeSource();
      if (status) {
        status = config_.io.open(config_.io.context, inlineNavigationSpoolPath_, PdfCacheOpenMode::Read,
                                 &inlineNavigationSpoolHandle_);
      }
      if (!status) {
        abortInlineNavigationSpill();
        return PdfStepResult::failure(status);
      }
      inlineNavigationSpoolOffset_ = 0;
      inlineNavigationSpoolReadCrc32_ = 0;
      inlineNavigationSpillStage_ = InlineNavigationSpillStage::Reading;
      return PdfStepResult::paused();
    }

    if (inlineNavigationSpillStage_ == InlineNavigationSpillStage::Reading) {
      if (navigation_ != nullptr || sourceHandle_.valid() || !inlineNavigationSpoolHandle_.valid() || !dictionary_ ||
          inlineNavigationSpoolOffset_ > sizeof(NavigationWorkspace)) {
        return PdfStepResult::failure(PdfStatus::failure(PdfError::InvalidArgument, inlineNavigationSpoolOffset_));
      }
      if (inlineNavigationSpoolOffset_ < sizeof(NavigationWorkspace)) {
        if (!budget.consumeOperation()) {
          return PdfStepResult::paused();
        }
        const size_t remaining = sizeof(NavigationWorkspace) - static_cast<size_t>(inlineNavigationSpoolOffset_);
        const size_t requested = budget.takeBytes(std::min<size_t>(PdfLimits::SourceBufferBytes, remaining));
        if (requested == 0) {
          return PdfStepResult::paused();
        }
        size_t bytesRead = 0;
        status = config_.io.read(config_.io.context, inlineNavigationSpoolHandle_, inlineNavigationSpoolOffset_,
                                 dictionary_.get() + inlineNavigationSpoolOffset_, requested, &bytesRead);
        if (!status || bytesRead != requested) {
          if (status) {
            status = PdfStatus::failure(PdfError::UnexpectedEof, inlineNavigationSpoolOffset_ + bytesRead);
          }
          abortInlineNavigationSpill();
          return PdfStepResult::failure(status);
        }
        inlineNavigationSpoolReadCrc32_ =
            pdfCacheCrc32(dictionary_.get() + inlineNavigationSpoolOffset_, bytesRead, inlineNavigationSpoolReadCrc32_);
        inlineNavigationSpoolOffset_ += bytesRead;
        if (inlineNavigationSpoolOffset_ < sizeof(NavigationWorkspace)) {
          return PdfStepResult::paused();
        }
      }
      status = config_.io.close(config_.io.context, &inlineNavigationSpoolHandle_);
      inlineNavigationSpoolHandle_ = {};
      if (!status || inlineNavigationSpoolReadCrc32_ != inlineNavigationSpoolCrc32_) {
        if (status) {
          status = PdfStatus::failure(PdfError::Malformed);
        }
        abortInlineNavigationSpill();
        return PdfStepResult::failure(status);
      }
      navigation_ = reinterpret_cast<NavigationWorkspace*>(dictionary_.get());
      (void)config_.io.remove(config_.io.context, inlineNavigationSpoolPath_, false);
      inlineNavigationSpoolPath_[0] = '\0';
      inlineNavigationSpoolOffset_ = 0;
      inlineNavigationSpoolCrc32_ = 0;
      inlineNavigationSpoolReadCrc32_ = 0;
      inlineNavigationSpillStage_ = InlineNavigationSpillStage::None;
      status = reopenSource();
      return status ? PdfStepResult::paused() : PdfStepResult::failure(status);
    }

    if (inlineImageEncodedLength_ != 0) {
      dataLength = inlineImageEncodedLength_;
    } else {
      if (!inlineImageDecoder_.has_value()) {
        if (!operandScratch_ || !dictionary_ || dataOffset >= contentSource.size ||
            (requiresNavigationSpill &&
             (inlineNavigationSpillStage_ != InlineNavigationSpillStage::Spilled || navigation_ != nullptr))) {
          return PdfStepResult::failure(PdfStatus::failure(PdfError::InvalidArgument, dataOffset));
        }
        contentLexer_.reset();
        status = pdfInitializeByteRange(contentSource, dataOffset, contentSource.size - dataOffset, &inlineImageRange_);
        if (status) {
          inlineImageDecoder_.emplace(
              PdfStreamDecoderWorkspace{operandScratch_.get() + decoderSourceOffset, decoderSourceBytes,
                                        operandScratch_.get() + decoderOutputOffset, decoderOutputBytes,
                                        dictionary_.get(), PdfLimits::UzlibDictionaryBytes});
          status = inlineImageDecoder_->begin(pdfByteRangeSource(inlineImageRange_), {this, discardInlineImageDecoded},
                                              inlineImageFilters_, inlineImageFilterCount_, {}, false);
        }
        if (!status) {
          inlineImageDecoder_.reset();
          return PdfStepResult::failure(status);
        }
      }
      const PdfStepResult decoded = inlineImageDecoder_->step(budget);
      if (!decoded.complete()) {
        return decoded;
      }
      dataLength = inlineImageDecoder_->consumedInputBytes();
      if (dataLength == 0) {
        return PdfStepResult::failure(PdfStatus::failure(PdfError::Malformed, dataOffset));
      }
      if (requiresNavigationSpill) {
        inlineImageEncodedLength_ = dataLength;
        inlineImageDecoder_.reset();
        return PdfStepResult::paused();
      }
    }
  }

  if (resumeOffset == 0 && dataLength != 0 &&
      (inlineImageSupported_ && (inlineImageJpeg_ || inlineImageFilterCount_ != 0))) {
    uint8_t terminator[4]{};
    status = pdfReadExact(contentSource, dataOffset + dataLength, terminator, sizeof(terminator));
    if (!status || !pdfWhitespace(terminator[0]) || terminator[1] != 'E' || terminator[2] != 'I' ||
        !pdfWhitespace(terminator[3])) {
      return PdfStepResult::failure(status ? PdfStatus::failure(PdfError::Malformed, dataOffset + dataLength) : status);
    }
    resumeOffset = dataOffset + dataLength + sizeof(terminator);
    contentLexer_.emplace(contentSource, sourceWindow_.get(), PdfLimits::SourceBufferBytes);
    contentLexer_->setSource(contentSource, resumeOffset);
  }

  if (resumeOffset == 0) {
    if (!contentLexer_.has_value()) {
      return PdfStepResult::failure(PdfStatus::failure(PdfError::Malformed, dataOffset));
    }
    const PdfStepResult skipped = contentLexer_->skipInlineImageData(budget);
    if (!skipped.complete()) {
      return skipped;
    }
    resumeOffset = contentLexer_->position();
    if (resumeOffset < dataOffset + 4U) {
      return PdfStepResult::failure(PdfStatus::failure(PdfError::Malformed, resumeOffset));
    }
    dataLength = resumeOffset - dataOffset - 4U;
  }
  if (inlineImageSupported_ && inlineImageJpeg_ && inlineImageCaptureStarted_) {
    status = navigation_->imageCache.finishJpegCapture(navigation_->imageCacheRuntime, &inlineCapturedJpeg_);
    inlineImageCaptureStarted_ = false;
    if (!status) {
      inlineImageCaptureFailed_ = true;
      warningFlags_ |= kWarningOptionalImageOmitted;
    } else if (inlineCapturedJpeg_.sourceBytes != dataLength) {
      (void)navigation_->imageCache.discardCapturedJpeg(inlineCapturedJpeg_.temporaryOrdinal);
      return PdfStepResult::failure(PdfStatus::failure(PdfError::Malformed, dataOffset));
    } else {
      jpegCaptureCompleted = true;
    }
  }
  const bool jpegCaptureFailed = inlineImageCaptureFailed_;
  const uint8_t retainedCandidateIndex = imageCandidateCount_;
  inlineImageAwaitingData_ = false;
  inlineImageDecoder_.reset();
  inlineImageRange_ = {};
  inlineImageDataOffset_ = 0;
  inlineImageScanOffset_ = 0;
  inlineImageEncodedLength_ = 0;
  inlineImageScanPendingBytes_ = 0;
  inlineImageScanPendingBufferOffset_ = 0;
  inlineImageScanSawJpegMarker_ = false;
  inlineImageCaptureStarted_ = false;
  inlineImageCaptureFailed_ = false;
  status = retainInlineImage(dataOffset, dataLength);
  if (status && imageCandidateCount_ == static_cast<uint8_t>(retainedCandidateIndex + 1U)) {
    PreparedImageCandidate& candidate = navigation_->imageCandidates[retainedCandidateIndex];
    candidate.jpegCaptured = jpegCaptureCompleted;
    candidate.jpegCaptureFailed = jpegCaptureFailed;
    if (jpegCaptureCompleted) {
      candidate.contentHash = inlineCapturedJpeg_.contentHash;
      candidate.sourceCrc32 = inlineCapturedJpeg_.sourceCrc32;
    }
  } else if (jpegCaptureCompleted) {
    (void)navigation_->imageCache.discardCapturedJpeg(inlineCapturedJpeg_.temporaryOrdinal);
  }
  return status ? PdfStepResult::completed() : PdfStepResult::failure(status);
}

PdfStatus PdfPreparation::appendContentToken(const PdfToken& token) {
  if (inlineImageDictionaryActive_) {
    if (token.kind == PdfTokenKind::Keyword && tokenEquals(token, "ID")) {
      finalizeInlineImageDictionary();
      inlineImageDictionaryActive_ = false;
      inlineImageAwaitingData_ = true;
      inlineImageIdEnd_ = contentLexer_.has_value() ? contentLexer_->position() : 0;
      return PdfStatus::success();
    }
    return consumeInlineImageToken(token);
  }
  if (token.kind == PdfTokenKind::Name) {
    lastContentNameLength_ = 0;
    if (token.length != 0 && token.length < sizeof(lastContentName_)) {
      std::memcpy(lastContentName_, token.bytes, token.length);
      lastContentName_[token.length] = '\0';
      lastContentNameLength_ = static_cast<uint8_t>(token.length);
    }
    return PdfStatus::success();
  }
  if (token.kind == PdfTokenKind::Integer || token.kind == PdfTokenKind::Real) {
    int16_t value = 0;
    if (parseTokenInt16(token, &value)) {
      lastNumericValue_ = value;
    }
    int32_t fixed = 0;
    if (placement_ != nullptr && parseTokenFixed16(token, &fixed)) {
      if (placement_->operandCount < std::size(placement_->operands)) {
        placement_->operands[placement_->operandCount++] = fixed;
      } else {
        std::move(placement_->operands + 1, placement_->operands + std::size(placement_->operands),
                  placement_->operands);
        placement_->operands[std::size(placement_->operands) - 1U] = fixed;
      }
    }
    return PdfStatus::success();
  }
  if (token.kind == PdfTokenKind::Keyword) {
    if (tokenEquals(token, "BI")) {
      abortInlineNavigationSpill();
      resetInlineImageDictionaryState();
      inlineImageDictionaryActive_ = true;
    } else if (tokenEquals(token, "q") && placement_ != nullptr) {
      if (placement_->depth < std::size(placement_->stack)) {
        placement_->stack[placement_->depth] = placement_->current;
        placement_->luminanceStack[placement_->depth] = placement_->nonstrokingLuminance;
        ++placement_->depth;
      }
    } else if (tokenEquals(token, "Q") && placement_ != nullptr) {
      if (placement_->depth != 0) {
        --placement_->depth;
        placement_->current = placement_->stack[placement_->depth];
        placement_->nonstrokingLuminance = placement_->luminanceStack[placement_->depth];
      }
    } else if (tokenEquals(token, "cm") && placement_ != nullptr && placement_->operandCount == 6) {
      placement_->current = concatenateMatrix(placement_->current, placement_->operands);
    } else if (placement_ != nullptr &&
               (tokenEquals(token, "g") || tokenEquals(token, "rg") || tokenEquals(token, "k") ||
                tokenEquals(token, "sc") || tokenEquals(token, "scn"))) {
      const uint8_t expectedCount = tokenEquals(token, "g")    ? 1U
                                    : tokenEquals(token, "rg") ? 3U
                                    : tokenEquals(token, "k")  ? 4U
                                                               : placement_->operandCount;
      uint8_t luminance = 0;
      if (placement_->operandCount == expectedCount &&
          fixedColorLuminance(placement_->operands, expectedCount, &luminance)) {
        placement_->nonstrokingLuminance = luminance;
      }
    } else if (token.length == 2 && token.bytes[0] == 'T' && token.bytes[1] == 'f') {
      currentFontSize_ = lastNumericValue_;
    } else if (token.length == 2 && token.bytes[0] == 'D' && token.bytes[1] == 'o' && navigation_ != nullptr) {
      for (uint8_t index = currentPageImageStart_; index < currentPageImageEnd_; ++index) {
        PreparedImageCandidate& candidate = navigation_->imageCandidates[index];
        if ((candidate.jpeg || candidate.raster) && candidate.nameLength == lastContentNameLength_ &&
            std::memcmp(candidate.name, lastContentName_, lastContentNameLength_) == 0) {
          if (candidate.placementCount != UINT8_MAX) {
            ++candidate.placementCount;
          }
          if (candidate.placementCount == 1) {
            if (candidate.parameters.colorSpace == PdfImageColorSpace::ImageMask) {
              candidate.parameters.imageMaskPaintLuminance = placement_->nonstrokingLuminance;
            }
            candidate.placement =
                orientPlacementToPage(matrixPlacement(placement_->current, candidate.width, candidate.height, false),
                                      navigation_->pages[currentPageIndex_]);
            candidate.placement.reference = candidate.reference;
            candidate.placement.imageMaskPaintLuminance = candidate.parameters.imageMaskPaintLuminance;
            candidate.semanticBlockIndex = extractedBlockCount_;
          }
          currentPageImageCandidate_ = static_cast<int8_t>(index);
          break;
        }
      }
    }
    if (placement_ != nullptr) {
      placement_->operandCount = 0;
    }
    return PdfStatus::success();
  }
  if (token.kind != PdfTokenKind::String && token.kind != PdfTokenKind::HexString) {
    return PdfStatus::success();
  }
  const size_t blockCapacity = kPreparationBlockWorkspaceBytes / sizeof(ExtractedBlockRecord);
  if (!pageText_ || !decoderOutput_ || token.length == 0 || token.length > UINT16_MAX ||
      extractedBlockCount_ >= blockCapacity || token.length > PdfLimits::PageTextBytes ||
      transcriptLength_ > PdfLimits::PageTextBytes - token.length) {
    return PdfStatus::failure(PdfError::LimitExceeded);
  }
  auto* const blocks = reinterpret_cast<ExtractedBlockRecord*>(decoderOutput_.get());
  blocks[extractedBlockCount_++] = {
      static_cast<uint16_t>(transcriptLength_),
      static_cast<uint16_t>(token.length),
      currentFontSize_,
      0,
  };
  std::memcpy(pageText_.get() + transcriptLength_, token.bytes, token.length);
  transcriptLength_ += token.length;
  return PdfStatus::success();
}

PdfStatus PdfPreparation::formatCurrentSectionPath() {
  const int relativeLength =
      std::snprintf(sectionRelativePath_, sizeof(sectionRelativePath_), "gen_%lu/sections/%06u.xhtml",
                    static_cast<unsigned long>(generation_), sectionCount_);
  const int fullLength = std::snprintf(sectionPath_, sizeof(sectionPath_), "%s/%s", cacheRoot_, sectionRelativePath_);
  if (relativeLength < 0 || static_cast<size_t>(relativeLength) >= sizeof(sectionRelativePath_) || fullLength < 0 ||
      static_cast<size_t>(fullLength) >= sizeof(sectionPath_)) {
    return PdfStatus::failure(PdfError::LimitExceeded, sectionCount_);
  }
  return PdfStatus::success();
}

PdfStatus PdfPreparation::openSection() {
  if (transcriptLength_ == 0 || extractedBlockCount_ == 0 || navigation_ == nullptr ||
      sectionCount_ >= kPreparationPageLimit) {
    return PdfStatus::failure(PdfError::NoReadableText);
  }
  PdfStatus status = formatCurrentSectionPath();
  if (!status) {
    return status;
  }
  const uint64_t used = cacheBudget_.requiredBytes + cacheBudget_.optionalBytes;
  const uint64_t byteLimit =
      used >= cacheBudget_.limit ? 0 : std::min<uint64_t>(cacheBudget_.limit - used, kSectionByteLimit);
  if (byteLimit == 0) {
    return PdfStatus::failure(PdfError::InsufficientStorage);
  }
  status = pdfOpenTrackedCacheWriter(config_.io, sectionPath_, sectionRelativePath_, PdfCacheFileKind::Required,
                                     byteLimit, &sectionWriter_);
  if (!status) {
    return status;
  }
  status = semanticWriter_.begin({this, writeSection}, {this, emitBlock},
                                 {operandScratch_.get(), PdfLimits::OperandOrderHistogramBytes}, totalWords_);
  if (!status) {
    pdfAbortTrackedCacheFile(&sectionWriter_);
  } else {
    sectionEmitStage_ = SectionEmitStage::Idle;
    sectionEmitEndBlock_ = currentBlockIndex_;
    sectionEmitImageIndex_ = currentPageImageStart_;
    if (navigation_->pageFirstSections[currentPageIndex_] == UINT16_MAX) {
      navigation_->pageFirstSections[currentPageIndex_] = sectionCount_;
    }
    currentSectionFirstWord_ = totalWords_;
    currentSectionFirstAnchor_ = nextAnchorOrdinal_;
  }
  return status;
}

PdfStatus PdfPreparation::formatInternalLink(const uint16_t sourcePageIndex, const uint8_t* const text,
                                             const size_t textLength, char* const href, const size_t capacity,
                                             size_t* const hrefLength) const {
  if (navigation_ == nullptr || text == nullptr || textLength == 0 || href == nullptr || capacity == 0 ||
      hrefLength == nullptr) {
    return PdfStatus::failure(PdfError::InvalidArgument);
  }
  href[0] = '\0';
  *hrefLength = 0;
  for (uint16_t linkIndex = 0; linkIndex < navigation_->linkCount; ++linkIndex) {
    const PreparedLink& link = navigation_->links[linkIndex];
    if (link.sourcePageIndex != sourcePageIndex) {
      continue;
    }
    PdfResolvedDestination destination{};
    if (!resolveDestination(link.destination, &destination)) {
      continue;
    }
    bool titleMatches = false;
    for (uint16_t outlineIndex = 0; outlineIndex < explicitOutlineCount_; ++outlineIndex) {
      const PdfOutlineEntry& entry = navigation_->outlineEntries[outlineIndex];
      if (entry.sourcePageIndex == destination.sourcePageIndex && entry.titleLength == textLength &&
          std::memcmp(entry.title, text, textLength) == 0) {
        titleMatches = true;
        break;
      }
    }
    if (!titleMatches) {
      continue;
    }
    if (navigation_->pageFirstAnchors[destination.sourcePageIndex] == UINT32_MAX) {
      return PdfStatus::failure(PdfError::InvalidOffset, destination.sourcePageIndex);
    }
    destination.anchorOrdinal = navigation_->pageFirstAnchors[destination.sourcePageIndex];
    return pdfResolveInternalAction(PdfActionKind::GoTo, destination, href, capacity, hrefLength);
  }
  return PdfStatus::failure(PdfError::InvalidOffset);
}

PdfStepResult PdfPreparation::emitSection(PdfWorkBudget& budget) {
  if (navigation_ == nullptr || !pageLabels_.has_value() || currentPageIndex_ >= pageCount_) {
    return PdfStepResult::failure(PdfStatus::failure(PdfError::InvalidArgument, currentPageIndex_));
  }
  if (sectionEmitStage_ == SectionEmitStage::Complete) {
    return PdfStepResult::completed();
  }
  if (budget.stopRequested()) {
    return PdfStepResult::paused();
  }
  auto* const blocks = reinterpret_cast<const ExtractedBlockRecord*>(decoderOutput_.get());

  if (sectionEmitStage_ == SectionEmitStage::Idle) {
    char pageLabel[PdfSemanticWriterLimits::PublisherLabelBytes]{};
    size_t pageLabelLength = 0;
    PdfStatus status = pageLabels_->format(currentPageIndex_, pageLabel, sizeof(pageLabel), &pageLabelLength);
    if (!status) {
      const int length =
          std::snprintf(pageLabel, sizeof(pageLabel), "%lu", static_cast<unsigned long>(currentPageIndex_ + 1U));
      if (length <= 0 || static_cast<size_t>(length) >= sizeof(pageLabel)) {
        return PdfStepResult::failure(PdfStatus::failure(PdfError::LimitExceeded, currentPageIndex_));
      }
      pageLabelLength = static_cast<size_t>(length);
      status = PdfStatus::success();
    }
    if (currentBlockIndex_ == 0) {
      navigation_->pageFirstAnchors[currentPageIndex_] = nextAnchorOrdinal_;
      if (currentPageIndex_ + 1U < pageCount_ && navigation_->pageFirstAnchors[currentPageIndex_ + 1U] == UINT32_MAX) {
        navigation_->pageFirstAnchors[currentPageIndex_ + 1U] = nextAnchorOrdinal_ + extractedBlockCount_;
      }
      status = semanticWriter_.writePublisherPageBreak(currentPageIndex_, reinterpret_cast<const uint8_t*>(pageLabel),
                                                       pageLabelLength);
    }
    if (!status) {
      return PdfStepResult::failure(status);
    }
    sectionEmitEndBlock_ = extractedBlockCount_;
    if (explicitOutlineCount_ == 0) {
      for (uint16_t index = static_cast<uint16_t>(currentBlockIndex_ + 1U); index < extractedBlockCount_; ++index) {
        if (blocks[index].sourceFontSize >= 18) {
          sectionEmitEndBlock_ = index;
          break;
        }
      }
    }
    sectionEmitStage_ = SectionEmitStage::BeginBlock;
    return PdfStepResult::paused();
  }

  if (sectionEmitStage_ == SectionEmitStage::BeginBlock) {
    if (currentBlockIndex_ >= sectionEmitEndBlock_) {
      sectionEmitStage_ = SectionEmitStage::Finish;
      return PdfStepResult::paused();
    }
    const ExtractedBlockRecord& record = blocks[currentBlockIndex_];
    const uint8_t* const text = pageText_.get() + record.textOffset;
    const bool heading = record.sourceFontSize >= 18;
    PdfStatus status = PdfStatus::success();
    if (heading && explicitOutlineCount_ == 0) {
      status = outlineBuilder_->appendHeading(text, record.textLength, sectionCount_, nextAnchorOrdinal_, 1);
    }
    if (status) {
      status = semanticWriter_.beginBlock({heading ? PdfSemanticBlockKind::Heading : PdfSemanticBlockKind::Paragraph,
                                           nextAnchorOrdinal_, static_cast<uint8_t>(heading ? 1 : 0)});
    }
    char href[PdfOutlineLimits::HrefBytes]{};
    size_t hrefLength = 0;
    bool linked = false;
    if (status && formatInternalLink(static_cast<uint16_t>(currentPageIndex_), text, record.textLength, href,
                                     sizeof(href), &hrefLength)) {
      status = semanticWriter_.beginInternalLink(reinterpret_cast<const uint8_t*>(href), hrefLength);
      linked = status.ok();
    }
    if (status) {
      status = semanticWriter_.writeText(text, record.textLength);
    }
    if (status && linked) {
      status = semanticWriter_.endInternalLink();
    }
    if (!status) {
      return PdfStepResult::failure(status);
    }
    sectionEmitImageIndex_ = currentPageImageStart_;
    sectionEmitStage_ = SectionEmitStage::Images;
    return PdfStepResult::paused();
  }

  if (sectionEmitStage_ == SectionEmitStage::Images) {
    while (sectionEmitImageIndex_ < currentPageImageEnd_) {
      const uint8_t imageIndex = sectionEmitImageIndex_++;
      PreparedImageCandidate& image = navigation_->imageCandidates[imageIndex];
      if (!image.retained || image.semanticBlockIndex != currentBlockIndex_) {
        continue;
      }
      uint64_t tagOffset = 0;
      PdfStatus status = PdfStatus::success();
      if (image.raster) {
        if (imageBuildSpool_.recordCount() >= PDF_IMAGE_BUILD_SPOOL_MAX_RECORDS) {
          warningFlags_ |= kWarningOptionalImageOmitted;
          cacheBudget_.optionalOmitted = true;
          image.retained = false;
          return PdfStepResult::paused();
        }
        if (!imageBuildSpool_.writing()) {
          const int pathLength =
              std::snprintf(imageBuildSpoolPath_, sizeof(imageBuildSpoolPath_), "%s/gen_%lu/build.images", cacheRoot_,
                            static_cast<unsigned long>(generation_));
          if (pathLength <= 0 || static_cast<size_t>(pathLength) >= sizeof(imageBuildSpoolPath_)) {
            return PdfStepResult::failure(PdfStatus::failure(PdfError::LimitExceeded));
          }
          status = imageBuildSpool_.beginWrite(config_.io, imageBuildSpoolPath_, sourceWindow_.get(),
                                               PdfLimits::SourceBufferBytes);
        }
        if (status) {
          status = semanticWriter_.flush();
        }
        if (!status) {
          return PdfStepResult::failure(status);
        }
        tagOffset = sectionWriter_.record.size;
        if (tagOffset > UINT32_MAX) {
          return PdfStepResult::failure(PdfStatus::failure(PdfError::LimitExceeded, tagOffset));
        }
      }
      status =
          semanticWriter_.writeRetainedImage(reinterpret_cast<const uint8_t*>(image.href), std::strlen(image.href),
                                             static_cast<uint16_t>(image.width), static_cast<uint16_t>(image.height));
      if (status && image.raster) {
        status = semanticWriter_.flush();
        const uint64_t tagLength = sectionWriter_.record.size - tagOffset;
        if (status && (tagLength == 0 || tagLength > UINT16_MAX)) {
          status = PdfStatus::failure(PdfError::LimitExceeded, tagLength);
        }
        if (status) {
          status =
              appendDeferredImageRecord(imageIndex, static_cast<uint32_t>(tagOffset), static_cast<uint16_t>(tagLength));
        }
      }
      return status ? PdfStepResult::paused() : PdfStepResult::failure(status);
    }
    sectionEmitStage_ = SectionEmitStage::EndBlock;
    return PdfStepResult::paused();
  }

  if (sectionEmitStage_ == SectionEmitStage::EndBlock) {
    const PdfStatus status = semanticWriter_.endBlock();
    if (!status) {
      return PdfStepResult::failure(status);
    }
    ++nextAnchorOrdinal_;
    ++currentBlockIndex_;
    sectionEmitStage_ = SectionEmitStage::BeginBlock;
    return PdfStepResult::paused();
  }

  if (sectionEmitStage_ == SectionEmitStage::Finish) {
    const PdfStatus status = semanticWriter_.finish();
    if (!status) {
      return PdfStepResult::failure(status);
    }
    totalWords_ = semanticWriter_.totalWords();
    currentBlockIndex_ = sectionEmitEndBlock_;
    sectionEmitStage_ = SectionEmitStage::Complete;
    return PdfStepResult::completed();
  }

  return PdfStepResult::failure(PdfStatus::failure(PdfError::InvalidArgument));
}

PdfStatus PdfPreparation::closeSection() {
  PdfStatus status = pdfCloseTrackedCacheFile(&sectionWriter_, &sectionRecord_);
  if (status) {
    status = pdfReserveCacheBytes(&cacheBudget_, sectionRecord_.size, PdfCacheFileKind::Required);
  }
  if (status) {
    if (sectionRecord_.size == 0 || sectionRecord_.size > UINT32_MAX ||
        cumulativeSectionBytes_ > UINT32_MAX - sectionRecord_.size) {
      return PdfStatus::failure(PdfError::LimitExceeded, sectionCount_);
    }
    cumulativeSectionBytes_ += sectionRecord_.size;
    navigation_->sectionFiles[sectionCount_] = sectionRecord_;
    navigation_->sections[sectionCount_] = {
        static_cast<uint32_t>(sectionRecord_.size),
        static_cast<uint32_t>(cumulativeSectionBytes_),
        currentSectionFirstWord_,
        totalWords_ - currentSectionFirstWord_,
        currentSectionFirstAnchor_,
        -1,
        0,
    };
    ++sectionCount_;
  }
  return status;
}

PdfStatus PdfPreparation::prepareNavigationRecords() {
  if (navigation_ == nullptr || !outlineBuilder_.has_value() || sectionCount_ == 0) {
    return PdfStatus::failure(PdfError::InvalidArgument);
  }
  for (uint16_t index = 0; index < explicitOutlineCount_; ++index) {
    PdfOutlineEntry& entry = navigation_->outlineEntries[index];
    if (entry.sourcePageIndex >= pageCount_ || navigation_->pageFirstAnchors[entry.sourcePageIndex] == UINT32_MAX) {
      return PdfStatus::failure(PdfError::InvalidOffset, entry.sourcePageIndex);
    }
    entry.sectionIndex = navigation_->pageFirstSections[entry.sourcePageIndex];
    entry.anchorOrdinal = navigation_->pageFirstAnchors[entry.sourcePageIndex];
    PdfStatus status = pdfFormatSemanticAnchor(entry.anchorOrdinal, entry.anchor);
    if (!status) {
      return status;
    }
  }
  metadata_ = metadataBuilder_.metadata();
  PdfStatus status = outlineBuilder_->finish(reinterpret_cast<const uint8_t*>(metadata_.title), metadata_.titleLength);
  if (!status) {
    return status;
  }
  metadata_.sectionCount = sectionCount_;
  metadata_.outlineCount = outlineBuilder_->count();
  metadata_.totalWords = totalWords_;
  for (uint16_t outlineIndex = 0; outlineIndex < outlineBuilder_->count(); ++outlineIndex) {
    const PdfOutlineEntry& entry = navigation_->outlineEntries[outlineIndex];
    if (entry.sectionIndex < sectionCount_ && navigation_->sections[entry.sectionIndex].tocIndex < 0) {
      navigation_->sections[entry.sectionIndex].tocIndex = static_cast<int16_t>(outlineIndex);
    }
  }
  coverFileCount_ = 0;
  typographyAssetIndex_ = 0;
  typographyRow_ = 0;
  typographyAssetStage_ =
      coverImageRecordAvailable_ ? TypographyAssetStage::OpenSource : TypographyAssetStage::BeginAsset;
  return PdfStatus::success();
}

PdfStepResult PdfPreparation::stepTypographyAssets(PdfWorkBudget& budget) {
  static constexpr uint16_t kWidths[] = {240, 96};
  static constexpr uint16_t kHeights[] = {400, 160};
  static constexpr const char* kLeafNames[] = {"cover.bmp", "thumb.bmp"};
  constexpr uint32_t kHeaderBytes = 62;

  if (typographyAssetStage_ == TypographyAssetStage::Complete) {
    return PdfStepResult::completed();
  }
  if (pageText_ == nullptr || typographyAssetStage_ == TypographyAssetStage::Idle) {
    return PdfStepResult::failure(PdfStatus::failure(PdfError::InvalidArgument));
  }
  if (typographyAssetStage_ == TypographyAssetStage::CloseSource) {
    if (!typographySourceHandle_.valid() || !budget.consumeOperation()) {
      return typographySourceHandle_.valid() ? PdfStepResult::paused()
                                             : PdfStepResult::failure(PdfStatus::failure(PdfError::InvalidArgument));
    }
    const bool fallbackToTypography = !coverImageRecordAvailable_;
    const PdfStatus status = config_.io.close(config_.io.context, &typographySourceHandle_);
    if (!status) {
      return PdfStepResult::failure(status);
    }
    jpegPreview_.reset();
    typographyAssetStage_ =
        fallbackToTypography ? TypographyAssetStage::BeginAsset : TypographyAssetStage::Complete;
    return fallbackToTypography ? PdfStepResult::paused() : PdfStepResult::completed();
  }
  if (typographyAssetIndex_ >= std::size(kWidths)) {
    return PdfStepResult::failure(PdfStatus::failure(PdfError::InvalidArgument));
  }

  const uint16_t width = kWidths[typographyAssetIndex_];
  const uint16_t height = kHeights[typographyAssetIndex_];
  const uint32_t rowBytes = ((static_cast<uint32_t>(width) + 31U) / 32U) * 4U;

  if (typographyAssetStage_ == TypographyAssetStage::OpenSource) {
    if (!coverImageRecordAvailable_ || typographySourceHandle_.valid() || !budget.consumeOperation()) {
      return coverImageRecordAvailable_ && !typographySourceHandle_.valid()
                 ? PdfStepResult::paused()
                 : PdfStepResult::failure(PdfStatus::failure(PdfError::InvalidArgument));
    }
    const int fullLength = std::snprintf(sectionPath_, sizeof(sectionPath_), "%s/%.*s", cacheRoot_,
                                         static_cast<int>(coverImageSourceRecord_.pathLength),
                                         coverImageSourceRecord_.path);
    if (fullLength <= 0 || static_cast<size_t>(fullLength) >= sizeof(sectionPath_)) {
      return PdfStepResult::failure(PdfStatus::failure(PdfError::LimitExceeded));
    }
    const PdfStatus status =
        config_.io.open(config_.io.context, sectionPath_, PdfCacheOpenMode::Read, &typographySourceHandle_);
    if (!status) {
      return PdfStepResult::failure(status);
    }
    if (coverImageSourceJpeg_) {
      const PdfStatus previewStatus =
          jpegPreview_.beginHeader(config_.io, typographySourceHandle_, coverImageSourceRecord_.size,
                                   sourceWindow_.get(), PdfLimits::SourceBufferBytes, pageText_.get(),
                                   PdfLimits::PageTextBytes);
      if (!previewStatus) {
        return PdfStepResult::failure(previewStatus);
      }
    }
    typographyAssetStage_ = TypographyAssetStage::ReadSourceHeader;
    return PdfStepResult::paused();
  }

  if (typographyAssetStage_ == TypographyAssetStage::ReadSourceHeader) {
    if (coverImageSourceJpeg_) {
      const PdfStepResult preview = jpegPreview_.stepHeader(budget);
      if (!preview.complete()) {
        if (preview.failed() && preview.status.error == PdfError::Unsupported) {
          jpegPreview_.reset();
          coverImageRecordAvailable_ = false;
          coverImageSourceJpeg_ = false;
          typographyAssetIndex_ = 0;
          typographyRow_ = 0;
          typographyAssetStage_ = TypographyAssetStage::CloseSource;
          return PdfStepResult::paused();
        }
        return preview;
      }
      typographySourceWidth_ = jpegPreview_.width();
      typographySourceHeight_ = jpegPreview_.height();
      typographySourceRowBytes_ = 0;
      typographyAssetStage_ = TypographyAssetStage::BeginAsset;
      return PdfStepResult::paused();
    }
    if (!typographySourceHandle_.valid() || !budget.consumeOperation() ||
        budget.takeBytes(pixel_cache::kHeaderSize) != pixel_cache::kHeaderSize) {
      return typographySourceHandle_.valid() ? PdfStepResult::paused()
                                             : PdfStepResult::failure(PdfStatus::failure(PdfError::InvalidArgument));
    }
    uint8_t header[pixel_cache::kHeaderSize]{};
    size_t bytesRead = 0;
    PdfStatus status =
        config_.io.read(config_.io.context, typographySourceHandle_, 0, header, sizeof(header), &bytesRead);
    pixel_cache::Layout layout{};
    if (status && bytesRead != sizeof(header)) {
      status = PdfStatus::failure(PdfError::UnexpectedEof, bytesRead);
    }
    if (status && pixel_cache::decodeHeader(header, sizeof(header), layout) != pixel_cache::Status::Ok) {
      status = PdfStatus::failure(PdfError::Malformed);
    }
    if (status && (layout.fileBytes != coverImageSourceRecord_.size ||
                   layout.bytesPerRow > PdfLimits::SourceBufferBytes || layout.bytesPerRow > UINT16_MAX)) {
      status = PdfStatus::failure(PdfError::Malformed, layout.fileBytes);
    }
    if (!status) {
      return PdfStepResult::failure(status);
    }
    typographySourceWidth_ = layout.width;
    typographySourceHeight_ = layout.height;
    typographySourceRowBytes_ = static_cast<uint16_t>(layout.bytesPerRow);
    typographyAssetStage_ = TypographyAssetStage::BeginAsset;
    return PdfStepResult::paused();
  }

  if (typographyAssetStage_ == TypographyAssetStage::BeginAsset) {
    const int relativeLength =
        std::snprintf(sectionRelativePath_, sizeof(sectionRelativePath_), "gen_%lu/%s",
                      static_cast<unsigned long>(generation_), kLeafNames[typographyAssetIndex_]);
    const int fullLength = std::snprintf(sectionPath_, sizeof(sectionPath_), "%s/%s", cacheRoot_, sectionRelativePath_);
    if (relativeLength <= 0 || static_cast<size_t>(relativeLength) >= sizeof(sectionRelativePath_) || fullLength <= 0 ||
        static_cast<size_t>(fullLength) >= sizeof(sectionPath_)) {
      return PdfStepResult::failure(PdfStatus::failure(PdfError::LimitExceeded));
    }
    const uint64_t used = cacheBudget_.requiredBytes + cacheBudget_.optionalBytes;
    const uint64_t byteLimit = used < cacheBudget_.limit ? cacheBudget_.limit - used : 0;
    const PdfStatus status = pdfOpenTrackedCacheWriter(config_.io, sectionPath_, sectionRelativePath_,
                                                       PdfCacheFileKind::Required, byteLimit, &sectionWriter_);
    if (!status) {
      return PdfStepResult::failure(status);
    }
    if (coverImageSourceJpeg_) {
      const PdfStatus previewStatus = jpegPreview_.beginAsset(&sectionWriter_, width, height);
      if (!previewStatus) {
        pdfAbortTrackedCacheFile(&sectionWriter_);
        return PdfStepResult::failure(previewStatus);
      }
    }
    typographyRow_ = 0;
    typographySourceLoadedRow_ = UINT16_MAX;
    if (coverImageRecordAvailable_) {
      const uint64_t widthLimitedHeight =
          static_cast<uint64_t>(typographySourceHeight_) * width / typographySourceWidth_;
      if (widthLimitedHeight <= height) {
        typographyScaledWidth_ = width;
        typographyScaledHeight_ = static_cast<uint16_t>(std::max<uint64_t>(1, widthLimitedHeight));
      } else {
        typographyScaledHeight_ = height;
        typographyScaledWidth_ = static_cast<uint16_t>(
            std::max<uint64_t>(1, static_cast<uint64_t>(typographySourceWidth_) * height / typographySourceHeight_));
      }
      typographyOffsetX_ = static_cast<uint16_t>((width - typographyScaledWidth_) / 2U);
      typographyOffsetY_ = static_cast<uint16_t>((height - typographyScaledHeight_) / 2U);
    }
    typographyAssetStage_ = TypographyAssetStage::Header;
    return PdfStepResult::paused();
  }

  if (typographyAssetStage_ == TypographyAssetStage::Header) {
    if (coverImageSourceJpeg_) {
      const PdfStepResult preview = jpegPreview_.stepAsset(budget);
      if (preview.complete()) {
        typographyAssetStage_ = TypographyAssetStage::Close;
        return PdfStepResult::paused();
      }
      if (preview.failed()) {
        pdfAbortTrackedCacheFile(&sectionWriter_);
      }
      return preview;
    }
    if (!budget.consumeOperation() || budget.takeBytes(kHeaderBytes) != kHeaderBytes) {
      return PdfStepResult::paused();
    }
    const uint32_t pixelBytes = rowBytes * height;
    const uint32_t fileBytes = kHeaderBytes + pixelBytes;
    uint8_t header[kHeaderBytes]{};
    header[0] = 'B';
    header[1] = 'M';
    writeLe32Bmp(header + 2, fileBytes);
    writeLe32Bmp(header + 10, kHeaderBytes);
    writeLe32Bmp(header + 14, 40);
    writeLe32Bmp(header + 18, width);
    writeLe32Bmp(header + 22, static_cast<uint32_t>(-static_cast<int32_t>(height)));
    writeLe16Bmp(header + 26, 1);
    writeLe16Bmp(header + 28, 1);
    writeLe32Bmp(header + 34, pixelBytes);
    writeLe32Bmp(header + 38, 2835);
    writeLe32Bmp(header + 42, 2835);
    writeLe32Bmp(header + 46, 2);
    header[58] = 0xff;
    header[59] = 0xff;
    header[60] = 0xff;
    const PdfStatus status = pdfWriteTrackedCacheFile(&sectionWriter_, header, sizeof(header));
    if (!status) {
      pdfAbortTrackedCacheFile(&sectionWriter_);
      return PdfStepResult::failure(status);
    }
    typographyAssetStage_ = TypographyAssetStage::Rows;
    return PdfStepResult::paused();
  }

  if (typographyAssetStage_ == TypographyAssetStage::Rows) {
    if (typographyRow_ >= height) {
      typographyAssetStage_ = TypographyAssetStage::Close;
      return PdfStepResult::paused();
    }
    if (coverImageRecordAvailable_) {
      const bool imageRow = typographyRow_ >= typographyOffsetY_ &&
                            typographyRow_ < static_cast<uint16_t>(typographyOffsetY_ + typographyScaledHeight_);
      uint16_t sourceRow = UINT16_MAX;
      if (imageRow) {
        sourceRow = static_cast<uint16_t>(static_cast<uint32_t>(typographyRow_ - typographyOffsetY_) *
                                          typographySourceHeight_ / typographyScaledHeight_);
      }
      const bool readSource = imageRow && sourceRow != typographySourceLoadedRow_;
      const uint8_t requiredOperations = static_cast<uint8_t>(readSource ? 2U : 1U);
      const size_t requiredBytes = rowBytes + (readSource ? typographySourceRowBytes_ : 0U);
      if (budget.operationsRemaining < requiredOperations || budget.bytesRemaining < requiredBytes ||
          budget.stopRequested()) {
        return PdfStepResult::paused();
      }
      if (readSource) {
        (void)budget.consumeOperation();
        (void)budget.takeBytes(typographySourceRowBytes_);
        size_t bytesRead = 0;
        const uint64_t offset = pixel_cache::kHeaderSize + static_cast<uint64_t>(sourceRow) * typographySourceRowBytes_;
        const PdfStatus status = config_.io.read(config_.io.context, typographySourceHandle_, offset,
                                                 sourceWindow_.get(), typographySourceRowBytes_, &bytesRead);
        if (!status || bytesRead != typographySourceRowBytes_) {
          return PdfStepResult::failure(status ? PdfStatus::failure(PdfError::UnexpectedEof, offset + bytesRead)
                                               : status);
        }
        typographySourceLoadedRow_ = sourceRow;
      }
      std::memset(pageText_.get(), 0xff, rowBytes);
      if (imageRow) {
        const uint16_t imageEnd = static_cast<uint16_t>(typographyOffsetX_ + typographyScaledWidth_);
        for (uint16_t x = typographyOffsetX_; x < imageEnd; ++x) {
          const uint16_t sourceX = static_cast<uint16_t>(static_cast<uint32_t>(x - typographyOffsetX_) *
                                                         typographySourceWidth_ / typographyScaledWidth_);
          const uint8_t sample =
              static_cast<uint8_t>((sourceWindow_[sourceX / 4U] >> (6U - (sourceX % 4U) * 2U)) & 0x03U);
          if (sample < 2U) {
            pageText_[x / 8U] &= static_cast<uint8_t>(~(1U << (7U - x % 8U)));
          }
        }
      }
      (void)budget.consumeOperation();
      (void)budget.takeBytes(rowBytes);
      const PdfStatus status = pdfWriteTrackedCacheFile(&sectionWriter_, pageText_.get(), rowBytes);
      if (!status) {
        pdfAbortTrackedCacheFile(&sectionWriter_);
        return PdfStepResult::failure(status);
      }
      ++typographyRow_;
      return PdfStepResult::paused();
    }
    if (budget.operationsRemaining < 2U || budget.bytesRemaining < rowBytes || budget.stopRequested()) {
      return PdfStepResult::paused();
    }
    const size_t maximumBuffered = std::min<size_t>(PdfLimits::PageTextBytes, budget.bytesRemaining);
    size_t buffered = 0;
    while (typographyRow_ < height && buffered + rowBytes <= maximumBuffered && budget.operationsRemaining > 1U &&
           !budget.stopRequested()) {
      if (!budget.consumeOperation()) {
        break;
      }
      uint8_t* const outputRow = pageText_.get() + buffered;
      std::memset(outputRow, 0xff, rowBytes);
      const uint8_t titleScale = width >= 200 ? 3 : 1;
      const uint8_t authorScale = width >= 200 ? 2 : 1;
      renderCoverTextRow(outputRow, width, typographyRow_, metadata_.title, metadata_.titleLength,
                         static_cast<uint16_t>(height / 5U), titleScale, width >= 200 ? 4 : 5);
      renderCoverTextRow(outputRow, width, typographyRow_, metadata_.author, metadata_.authorLength,
                         static_cast<uint16_t>(height * 3U / 4U), authorScale, 2);
      buffered += rowBytes;
      ++typographyRow_;
    }
    if (buffered == 0 || !budget.consumeOperation() || budget.takeBytes(buffered) != buffered) {
      return PdfStepResult::paused();
    }
    const PdfStatus status = pdfWriteTrackedCacheFile(&sectionWriter_, pageText_.get(), buffered);
    if (!status) {
      pdfAbortTrackedCacheFile(&sectionWriter_);
      return PdfStepResult::failure(status);
    }
    return PdfStepResult::paused();
  }

  if (typographyAssetStage_ != TypographyAssetStage::Close) {
    return PdfStepResult::failure(PdfStatus::failure(PdfError::InvalidArgument));
  }
  PdfRequiredFileRecord& record = coverRecords_[typographyAssetIndex_];
  resetInPlace(record);
  PdfStatus status = pdfCloseTrackedCacheFile(&sectionWriter_, &record);
  if (status) {
    status = pdfReserveCacheBytes(&cacheBudget_, record.size, PdfCacheFileKind::Required);
  }
  if (!status) {
    if (sectionWriter_.open) {
      pdfAbortTrackedCacheFile(&sectionWriter_);
    } else {
      (void)config_.io.remove(config_.io.context, sectionPath_, false);
    }
    return PdfStepResult::failure(status);
  }
  ++coverFileCount_;
  ++typographyAssetIndex_;
  typographyRow_ = 0;
  typographyAssetStage_ = typographyAssetIndex_ < std::size(kWidths) ? TypographyAssetStage::BeginAsset
                          : coverImageRecordAvailable_               ? TypographyAssetStage::CloseSource
                                                                     : TypographyAssetStage::Complete;
  return PdfStepResult::paused();
}

PdfStatus PdfPreparation::openMetadata() {
  const uint64_t used = cacheBudget_.requiredBytes + cacheBudget_.optionalBytes;
  const uint64_t byteLimit =
      used >= cacheBudget_.limit ? 0 : std::min<uint64_t>(cacheBudget_.limit - used, 16ULL * 1024ULL);
  if (byteLimit == 0) {
    return PdfStatus::failure(PdfError::InsufficientStorage);
  }
  return pdfOpenTrackedCacheWriter(config_.io, metadataPath_, metadataRelativePath_, PdfCacheFileKind::Required,
                                   byteLimit, &metadataWriter_);
}

PdfStatus PdfPreparation::writeMetadata() {
  static_assert(24U + PdfMetadataLimits::TitleBytes + PdfMetadataLimits::AuthorBytes +
                    PdfMetadataLimits::LanguageBytes + kPreparationPageLimit * sizeof(PdfMetadataSection) + 4U <=
                PdfLimits::SourceBufferBytes);
  metadataEncodeBytes_ = 0;
  PdfStatus status = pdfEncodeMetadata(metadata_, {this, sectionCount_, readMetadataSection}, {this, writeMetadata});
  if (status) {
    status = pdfWriteTrackedCacheFile(&metadataWriter_, sourceWindow_.get(), metadataEncodeBytes_);
  }
  return status;
}

PdfStatus PdfPreparation::closeMetadata() {
  PdfStatus status = pdfCloseTrackedCacheFile(&metadataWriter_, &metadataRecord_);
  if (status) {
    status = pdfReserveCacheBytes(&cacheBudget_, metadataRecord_.size, PdfCacheFileKind::Required);
  }
  return status;
}

PdfStatus PdfPreparation::openOutline() {
  static_assert(sizeof(PdfOutlineEncodeWorkspace) <= PdfLimits::PageTextBytes);
  if (!pageText_) {
    return PdfStatus::failure(PdfError::InsufficientMemory);
  }
  const uint64_t used = cacheBudget_.requiredBytes + cacheBudget_.optionalBytes;
  const uint64_t byteLimit =
      used >= cacheBudget_.limit ? 0 : std::min<uint64_t>(cacheBudget_.limit - used, 64ULL * 1024ULL);
  if (byteLimit == 0) {
    return PdfStatus::failure(PdfError::InsufficientStorage);
  }
  PdfStatus status = pdfOpenTrackedCacheWriter(config_.io, outlinePath_, outlineRelativePath_,
                                               PdfCacheFileKind::Required, byteLimit, &outlineWriter_);
  if (status) {
    outlineEncodeRuntime_ = {};
    new (pageText_.get()) PdfOutlineEncodeWorkspace{};
  }
  return status;
}

PdfStepResult PdfPreparation::stepWriteOutline(PdfWorkBudget& budget) {
  if (!pageText_ || !outlineBuilder_.has_value()) {
    return PdfStepResult::failure(PdfStatus::failure(PdfError::InvalidArgument));
  }
  auto& workspace = *reinterpret_cast<PdfOutlineEncodeWorkspace*>(pageText_.get());
  return pdfStepEncodeOutline({this, outlineBuilder_->count(), readOutlineEntry}, {this, writeOutline},
                              outlineEncodeRuntime_, workspace, budget);
}

PdfStatus PdfPreparation::closeOutline() {
  PdfStatus status = pdfCloseTrackedCacheFile(&outlineWriter_, &outlineRecord_);
  if (status) {
    status = pdfReserveCacheBytes(&cacheBudget_, outlineRecord_.size, PdfCacheFileKind::Required);
  }
  return status;
}

PdfStepResult PdfPreparation::stepCommitManifest(PdfWorkBudget& budget) {
  static_assert(sizeof(PdfRequiredFileRecord) * PDF_IMAGE_BUILD_SPOOL_MAX_RECORDS <= PdfLimits::PageTextBytes);
  if (navigation_ == nullptr || !sourceWindow_ || !decoderOutput_ || !pageText_) {
    return PdfStepResult::failure(PdfStatus::failure(PdfError::InvalidArgument));
  }
  PdfCacheManifest& manifest = navigation_->manifestScratch;
  PdfRequiredFileRecord& record = navigation_->cachedImageScratch.record;
  auto* const materializedImageRecords = reinterpret_cast<PdfRequiredFileRecord*>(pageText_.get());

  const auto accountRecord = [&](const PdfRequiredFileRecord& value, const uint32_t index) -> PdfStatus {
    if (value.pathLength == 0 || value.pathLength >= sizeof(value.path) || value.path[value.pathLength] != '\0' ||
        !pdfValidateCacheRelativePath(value.path, value.pathLength)) {
      return PdfStatus::failure(PdfError::Malformed, index);
    }
    const uint32_t encoded = static_cast<uint32_t>(kManifestRecordHeaderBytes + value.pathLength);
    if (manifestEncodedBytes_ > PDF_CACHE_MAX_SLOT_BYTES - encoded ||
        manifest.requiredFileBytes > UINT64_MAX - value.size) {
      return PdfStatus::failure(PdfError::LimitExceeded, index);
    }
    manifestEncodedBytes_ += encoded;
    manifest.requiredFileBytes += value.size;
    manifest.requiredFileLedger = pdfUpdateRequiredFileLedger(manifest.requiredFileLedger, value);
    return PdfStatus::success();
  };

  if (manifestCommitStage_ == ManifestCommitStage::Idle) {
    if (manifestHandle_.valid() || manifestPath_[0] != '\0') {
      return PdfStepResult::failure(PdfStatus::failure(PdfError::InvalidArgument));
    }
    if (retainedImageFileCount_ != 0) {
      if (navigation_->imageFileRead.stage == PdfImageSpoolReadStage::Idle) {
        const PdfStatus status = imageFileSpool_.beginRead(config_.io, imageFileSpoolPath_, sourceWindow_.get(),
                                                           PdfLimits::SourceBufferBytes, &navigation_->imageFileRead);
        return status ? PdfStepResult::paused() : PdfStepResult::failure(status);
      }
      if (navigation_->imageFileRead.stage != PdfImageSpoolReadStage::Complete) {
        const PdfStepResult opened = imageFileSpool_.stepReadOpen(navigation_->imageFileRead, budget);
        return opened.complete() ? PdfStepResult::paused() : opened;
      }
      retainedImageFileCount_ = imageFileSpool_.recordCount();
    }
    resetInPlace(manifest);
    manifest.sequence = sequence_;
    manifest.completed = true;
    manifest.source = sourceIdentity_;
    manifest.generation = generation_;
    manifest.totalWords = totalWords_;
    manifest.warningFlags = warningFlags_;
    manifest.requiredFileCount = static_cast<uint32_t>(sectionCount_) + retainedImageFileCount_ + coverFileCount_ + 2U;
    manifest.requiredFileBytes = 0;
    manifest.requiredFileLedger = PDF_CACHE_FNV64_OFFSET;
    manifestEncodedBytes_ = static_cast<uint32_t>(kManifestHeaderBytes + kManifestTrailerBytes);
    manifestRecordIndex_ = 0;
    manifestOffset_ = 0;
    manifestCrc32_ = 0;
    manifestReadCrc32_ = 0;
    manifestCommitStage_ = ManifestCommitStage::LedgerSections;
    return PdfStepResult::paused();
  }

  if (manifestCommitStage_ == ManifestCommitStage::LedgerSections) {
    if (manifestRecordIndex_ < sectionCount_) {
      const PdfStatus status = accountRecord(navigation_->sectionFiles[manifestRecordIndex_], manifestRecordIndex_);
      if (!status) {
        return PdfStepResult::failure(status);
      }
      ++manifestRecordIndex_;
      return PdfStepResult::paused();
    }
    manifestRecordIndex_ = 0;
    manifestCommitStage_ = ManifestCommitStage::LedgerImages;
    return PdfStepResult::paused();
  }

  if (manifestCommitStage_ == ManifestCommitStage::LedgerImages) {
    if (manifestRecordIndex_ < retainedImageFileCount_) {
      if (budget.bytesRemaining < PDF_IMAGE_FILE_RECORD_BYTES || !budget.consumeOperation()) {
        return PdfStepResult::paused();
      }
      (void)budget.takeBytes(PDF_IMAGE_FILE_RECORD_BYTES);
      resetInPlace(record);
      PdfStatus status = imageFileSpool_.readRecord(static_cast<uint8_t>(manifestRecordIndex_), &record);
      if (status) {
        status = accountRecord(record, static_cast<uint32_t>(sectionCount_) + manifestRecordIndex_);
      }
      if (!status) {
        return PdfStepResult::failure(status);
      }
      materializedImageRecords[manifestRecordIndex_] = record;
      ++manifestRecordIndex_;
      return PdfStepResult::paused();
    }
    manifestRecordIndex_ = 0;
    manifestCommitStage_ = ManifestCommitStage::LedgerCovers;
    return PdfStepResult::paused();
  }

  if (manifestCommitStage_ == ManifestCommitStage::LedgerCovers) {
    if (manifestRecordIndex_ < coverFileCount_) {
      const PdfStatus status =
          accountRecord(coverRecords_[manifestRecordIndex_],
                        static_cast<uint32_t>(sectionCount_) + retainedImageFileCount_ + manifestRecordIndex_);
      if (!status) {
        return PdfStepResult::failure(status);
      }
      ++manifestRecordIndex_;
      return PdfStepResult::paused();
    }
    manifestRecordIndex_ = 0;
    manifestCommitStage_ = ManifestCommitStage::LedgerMetadata;
    return PdfStepResult::paused();
  }

  if (manifestCommitStage_ == ManifestCommitStage::LedgerMetadata) {
    const PdfRequiredFileRecord& value = manifestRecordIndex_ == 0 ? metadataRecord_ : outlineRecord_;
    const PdfStatus status = accountRecord(
        value, static_cast<uint32_t>(sectionCount_) + retainedImageFileCount_ + coverFileCount_ + manifestRecordIndex_);
    if (!status) {
      return PdfStepResult::failure(status);
    }
    if (++manifestRecordIndex_ < 2U) {
      return PdfStepResult::paused();
    }
    if (manifest.requiredFileCount !=
        static_cast<uint32_t>(sectionCount_) + retainedImageFileCount_ + coverFileCount_ + 2U) {
      return PdfStepResult::failure(PdfStatus::failure(PdfError::Malformed));
    }
    PdfCacheSlot newestValid = PdfCacheSlot::B;
    bool hasValid = false;
    for (uint8_t index = 0; index < 2; ++index) {
      if (!manifestSelection_.slots[index].valid) {
        continue;
      }
      const uint8_t newestIndex = newestValid == PdfCacheSlot::A ? 0 : 1;
      if (!hasValid || pdfCacheSequenceNewer(manifestSelection_.slots[index].manifest.sequence,
                                             manifestSelection_.slots[newestIndex].manifest.sequence)) {
        newestValid = index == 0 ? PdfCacheSlot::A : PdfCacheSlot::B;
        hasValid = true;
      }
    }
    manifestTargetSlot_ = !hasValid || newestValid == PdfCacheSlot::B ? 0 : 1;
    const char* const leaf = manifestTargetSlot_ == 0 ? "manifest.a" : "manifest.b";
    const int pathLength = std::snprintf(manifestPath_, sizeof(manifestPath_), "%s/%s", cacheRoot_, leaf);
    if (pathLength <= 0 || static_cast<size_t>(pathLength) >= sizeof(manifestPath_)) {
      manifestPath_[0] = '\0';
      return PdfStepResult::failure(PdfStatus::failure(PdfError::LimitExceeded));
    }
    manifestRecordIndex_ = 0;
    manifestCommitStage_ = ManifestCommitStage::OpenWriter;
    return PdfStepResult::paused();
  }

  if (manifestCommitStage_ == ManifestCommitStage::OpenWriter) {
    PdfStatus status = config_.io.mkdir(config_.io.context, cacheRoot_);
    if (status) {
      status = config_.io.open(config_.io.context, manifestPath_, PdfCacheOpenMode::WriteTruncate, &manifestHandle_);
    }
    if (!status) {
      return PdfStepResult::failure(status);
    }
    manifestOffset_ = 0;
    manifestCrc32_ = 0;
    manifestCommitStage_ = ManifestCommitStage::WriteHeader;
    return PdfStepResult::paused();
  }

  const auto writeManifestBytes = [&](const uint8_t* const bytes, const size_t length,
                                      const bool includeInCrc) -> PdfStepResult {
    if (!budget.consumeOperation() || budget.takeBytes(length) != length) {
      return PdfStepResult::paused();
    }
    size_t written = 0;
    PdfStatus status = config_.io.write(config_.io.context, manifestHandle_, bytes, length, &written);
    if (!status || written != length) {
      if (status) {
        status = PdfStatus::failure(PdfError::IoFailure, manifestOffset_ + written);
      }
      return PdfStepResult::failure(status);
    }
    if (includeInCrc) {
      manifestCrc32_ = pdfCacheCrc32(bytes, length, manifestCrc32_);
    }
    manifestOffset_ += written;
    return PdfStepResult::completed();
  };

  if (manifestCommitStage_ == ManifestCommitStage::WriteHeader) {
    encodeManifestHeader(manifest, sourceWindow_.get());
    const PdfStepResult written = writeManifestBytes(sourceWindow_.get(), kManifestHeaderBytes, true);
    if (!written.complete()) {
      return written;
    }
    manifestRecordIndex_ = 0;
    manifestCommitStage_ = ManifestCommitStage::WriteRecords;
    return PdfStepResult::paused();
  }

  if (manifestCommitStage_ == ManifestCommitStage::WriteRecords) {
    if (manifestRecordIndex_ >= manifest.requiredFileCount) {
      manifestCommitStage_ = ManifestCommitStage::WriteTrailer;
      return PdfStepResult::paused();
    }
    resetInPlace(record);
    PdfStatus status = readRequiredFile(this, manifestRecordIndex_, &record);
    if (!status) {
      return PdfStepResult::failure(status);
    }
    const size_t encoded = encodeManifestRecord(record, sourceWindow_.get());
    const PdfStepResult written = writeManifestBytes(sourceWindow_.get(), encoded, true);
    if (!written.complete()) {
      return written;
    }
    ++manifestRecordIndex_;
    return PdfStepResult::paused();
  }

  if (manifestCommitStage_ == ManifestCommitStage::WriteTrailer) {
    writeLe32Bmp(sourceWindow_.get(), manifestEncodedBytes_);
    manifestCrc32_ = pdfCacheCrc32(sourceWindow_.get(), sizeof(uint32_t), manifestCrc32_);
    writeLe32Bmp(sourceWindow_.get() + sizeof(uint32_t), manifestCrc32_);
    const PdfStepResult written = writeManifestBytes(sourceWindow_.get(), kManifestTrailerBytes, false);
    if (!written.complete()) {
      return written;
    }
    if (manifestOffset_ != manifestEncodedBytes_) {
      return PdfStepResult::failure(PdfStatus::failure(PdfError::Malformed, manifestOffset_));
    }
    manifestCommitStage_ = ManifestCommitStage::CloseWriter;
    return PdfStepResult::paused();
  }

  if (manifestCommitStage_ == ManifestCommitStage::CloseWriter) {
    const PdfStatus status = closeDurableWriter(config_.io, &manifestHandle_);
    if (!status) {
      return PdfStepResult::failure(status);
    }
    manifestHandle_ = {};
    manifestCommitStage_ = ManifestCommitStage::CloseImageSpool;
    return PdfStepResult::paused();
  }

  if (manifestCommitStage_ == ManifestCommitStage::CloseImageSpool) {
    if (imageFileSpool_.reading() && !budget.consumeOperation()) {
      return PdfStepResult::paused();
    }
    const PdfStatus status = imageFileSpool_.closeRead();
    if (!status) {
      return PdfStepResult::failure(status);
    }
    navigation_->imageFileRead = {};
    manifestCommitStage_ = ManifestCommitStage::OpenVerifier;
    return PdfStepResult::paused();
  }

  if (manifestCommitStage_ == ManifestCommitStage::OpenVerifier) {
    PdfStatus status = config_.io.open(config_.io.context, manifestPath_, PdfCacheOpenMode::Read, &manifestHandle_);
    PdfCacheFileMetadata metadata{};
    if (status) {
      status = config_.io.metadata(config_.io.context, manifestHandle_, &metadata);
    }
    if (!status || metadata.directory || metadata.symlinkLike || metadata.size != manifestEncodedBytes_) {
      return PdfStepResult::failure(status ? PdfStatus::failure(PdfError::Malformed, metadata.size) : status);
    }
    manifestOffset_ = 0;
    manifestReadCrc32_ = 0;
    manifestRecordIndex_ = 0;
    manifestCommitStage_ = ManifestCommitStage::VerifyHeader;
    return PdfStepResult::paused();
  }

  const auto verifyManifestBytes = [&](const uint8_t* const expected, const size_t length,
                                       const bool includeInCrc) -> PdfStepResult {
    if (!budget.consumeOperation() || budget.takeBytes(length) != length) {
      return PdfStepResult::paused();
    }
    size_t bytesRead = 0;
    PdfStatus status =
        config_.io.read(config_.io.context, manifestHandle_, manifestOffset_, decoderOutput_.get(), length, &bytesRead);
    if (!status || bytesRead != length) {
      if (status) {
        status = PdfStatus::failure(PdfError::UnexpectedEof, manifestOffset_ + bytesRead);
      }
      return PdfStepResult::failure(status);
    }
    if (std::memcmp(decoderOutput_.get(), expected, length) != 0) {
      return PdfStepResult::failure(PdfStatus::failure(PdfError::Malformed, manifestOffset_));
    }
    if (includeInCrc) {
      manifestReadCrc32_ = pdfCacheCrc32(decoderOutput_.get(), length, manifestReadCrc32_);
    }
    manifestOffset_ += bytesRead;
    return PdfStepResult::completed();
  };

  if (manifestCommitStage_ == ManifestCommitStage::VerifyHeader) {
    encodeManifestHeader(manifest, sourceWindow_.get());
    const PdfStepResult verified = verifyManifestBytes(sourceWindow_.get(), kManifestHeaderBytes, true);
    if (!verified.complete()) {
      return verified;
    }
    manifestCommitStage_ = ManifestCommitStage::VerifyRecords;
    return PdfStepResult::paused();
  }

  if (manifestCommitStage_ == ManifestCommitStage::VerifyRecords) {
    if (manifestRecordIndex_ >= manifest.requiredFileCount) {
      manifestCommitStage_ = ManifestCommitStage::VerifyTrailer;
      return PdfStepResult::paused();
    }
    resetInPlace(record);
    PdfStatus status = readRequiredFile(this, manifestRecordIndex_, &record);
    if (!status) {
      return PdfStepResult::failure(status);
    }
    const size_t encoded = encodeManifestRecord(record, sourceWindow_.get());
    const PdfStepResult verified = verifyManifestBytes(sourceWindow_.get(), encoded, true);
    if (!verified.complete()) {
      return verified;
    }
    ++manifestRecordIndex_;
    return PdfStepResult::paused();
  }

  if (manifestCommitStage_ == ManifestCommitStage::VerifyTrailer) {
    writeLe32Bmp(sourceWindow_.get(), manifestEncodedBytes_);
    const uint32_t expectedCrc = pdfCacheCrc32(sourceWindow_.get(), sizeof(uint32_t), manifestReadCrc32_);
    writeLe32Bmp(sourceWindow_.get() + sizeof(uint32_t), expectedCrc);
    const PdfStepResult verified = verifyManifestBytes(sourceWindow_.get(), kManifestTrailerBytes, false);
    if (!verified.complete()) {
      return verified;
    }
    if (expectedCrc != manifestCrc32_ || manifestOffset_ != manifestEncodedBytes_) {
      return PdfStepResult::failure(PdfStatus::failure(PdfError::Malformed, manifestOffset_));
    }
    manifestCommitStage_ = ManifestCommitStage::CloseVerifier;
    return PdfStepResult::paused();
  }

  if (manifestCommitStage_ == ManifestCommitStage::CloseVerifier) {
    const PdfStatus status = config_.io.close(config_.io.context, &manifestHandle_);
    manifestHandle_ = {};
    if (!status) {
      return PdfStepResult::failure(status);
    }
    PdfCacheManifestSlotState& target = manifestSelection_.slots[manifestTargetSlot_];
    target.valid = true;
    target.sourceMatches = true;
    target.manifest = manifest;
    manifestSelection_.selected = true;
    manifestSelection_.selectedSlot = manifestTargetSlot_ == 0 ? PdfCacheSlot::A : PdfCacheSlot::B;
    manifestSelection_.manifest = manifest;
    imageFileSpool_.remove();
    imageFileSpoolPath_[0] = '\0';
    manifestPath_[0] = '\0';
    manifestCommitStage_ = ManifestCommitStage::Complete;
    return PdfStepResult::completed();
  }

  if (manifestCommitStage_ == ManifestCommitStage::Complete) {
    return PdfStepResult::completed();
  }
  return PdfStepResult::failure(PdfStatus::failure(PdfError::InvalidArgument));
}

PdfStepResult PdfPreparation::stepCommitCheckpoint(PdfWorkBudget& budget, const PdfBuildPhase phase) {
  if (pageText_ == nullptr || !config_.io.valid()) {
    return PdfStepResult::failure(PdfStatus::failure(PdfError::InvalidArgument));
  }
  if (checkpointCommitStage_ == CheckpointCommitStage::Idle) {
    if (checkpointCommitHandle_.valid()) {
      return PdfStepResult::failure(PdfStatus::failure(PdfError::InvalidArgument));
    }
    PdfBuildCheckpoint& checkpoint = checkpointSelection_.checkpoint;
    resetInPlace(checkpoint);
    checkpoint.sequence = sequence_;
    checkpoint.source = sourceIdentity_;
    checkpoint.generation = generation_;
    checkpoint.phase = phase;
    checkpoint.lastVerifiedPage = pageCount_;
    checkpoint.emittedSections = sectionCount_;
    checkpoint.cumulativeWords = totalWords_;
    checkpoint.outputBytes = cumulativeSectionBytes_ + metadataRecord_.size + outlineRecord_.size;
    for (uint8_t index = 0; index < coverFileCount_; ++index) {
      checkpoint.outputBytes += coverRecords_[index].size;
    }
    FixedMemorySink sink{pageText_.get(), kCheckpointBytes, 0};
    PdfStatus status = pdfEncodeBuildCheckpoint(checkpoint, sink.sink());
    if (!status || sink.size != kCheckpointBytes) {
      return PdfStepResult::failure(status ? PdfStatus::failure(PdfError::Malformed, sink.size) : status);
    }
    const char* const leaf = (checkpoint.sequence & 1U) != 0 ? "build.a" : "build.b";
    const int pathLength = std::snprintf(sectionPath_, sizeof(sectionPath_), "%s/%s", cacheRoot_, leaf);
    if (pathLength <= 0 || static_cast<size_t>(pathLength) >= sizeof(sectionPath_)) {
      return PdfStepResult::failure(PdfStatus::failure(PdfError::LimitExceeded));
    }
    checkpointCommitStage_ = CheckpointCommitStage::CreateCacheRoot;
    return PdfStepResult::paused();
  }

  if (checkpointCommitStage_ == CheckpointCommitStage::CreateCacheRoot) {
    if (!budget.consumeOperation()) {
      return PdfStepResult::paused();
    }
    const PdfStatus status = config_.io.mkdir(config_.io.context, cacheRoot_);
    if (!status) {
      return PdfStepResult::failure(status);
    }
    checkpointCommitStage_ = CheckpointCommitStage::OpenWriter;
    return PdfStepResult::paused();
  }

  if (checkpointCommitStage_ == CheckpointCommitStage::OpenWriter) {
    if (!budget.consumeOperation()) {
      return PdfStepResult::paused();
    }
    const PdfStatus status =
        config_.io.open(config_.io.context, sectionPath_, PdfCacheOpenMode::WriteTruncate, &checkpointCommitHandle_);
    if (!status) {
      return PdfStepResult::failure(status);
    }
    checkpointCommitStage_ = CheckpointCommitStage::Write;
    return PdfStepResult::paused();
  }

  if (checkpointCommitStage_ == CheckpointCommitStage::Write) {
    if (budget.bytesRemaining < kCheckpointBytes || !budget.consumeOperation()) {
      return PdfStepResult::paused();
    }
    (void)budget.takeBytes(kCheckpointBytes);
    size_t bytesWritten = 0;
    PdfStatus status =
        config_.io.write(config_.io.context, checkpointCommitHandle_, pageText_.get(), kCheckpointBytes, &bytesWritten);
    if (!status || bytesWritten != kCheckpointBytes) {
      return PdfStepResult::failure(status ? PdfStatus::failure(PdfError::IoFailure, bytesWritten) : status);
    }
    checkpointCommitStage_ = CheckpointCommitStage::Flush;
    return PdfStepResult::paused();
  }

  if (checkpointCommitStage_ == CheckpointCommitStage::Flush || checkpointCommitStage_ == CheckpointCommitStage::Sync) {
    if (!budget.consumeOperation()) {
      return PdfStepResult::paused();
    }
    const PdfStatus status = checkpointCommitStage_ == CheckpointCommitStage::Flush
                                 ? config_.io.flush(config_.io.context, checkpointCommitHandle_)
                                 : config_.io.sync(config_.io.context, checkpointCommitHandle_);
    if (!status) {
      return PdfStepResult::failure(status);
    }
    checkpointCommitStage_ = checkpointCommitStage_ == CheckpointCommitStage::Flush
                                 ? CheckpointCommitStage::Sync
                                 : CheckpointCommitStage::CloseWriter;
    return PdfStepResult::paused();
  }

  if (checkpointCommitStage_ == CheckpointCommitStage::CloseWriter) {
    if (!budget.consumeOperation()) {
      return PdfStepResult::paused();
    }
    const PdfStatus status = config_.io.close(config_.io.context, &checkpointCommitHandle_);
    if (!status) {
      return PdfStepResult::failure(status);
    }
    checkpointCommitStage_ = CheckpointCommitStage::OpenVerifier;
    return PdfStepResult::paused();
  }

  if (checkpointCommitStage_ == CheckpointCommitStage::OpenVerifier) {
    if (!budget.consumeOperation()) {
      return PdfStepResult::paused();
    }
    const PdfStatus status =
        config_.io.open(config_.io.context, sectionPath_, PdfCacheOpenMode::Read, &checkpointCommitHandle_);
    if (!status) {
      return PdfStepResult::failure(status);
    }
    checkpointCommitStage_ = CheckpointCommitStage::ReadVerifierMetadata;
    return PdfStepResult::paused();
  }

  if (checkpointCommitStage_ == CheckpointCommitStage::ReadVerifierMetadata) {
    if (!budget.consumeOperation()) {
      return PdfStepResult::paused();
    }
    PdfCacheFileMetadata metadata{};
    const PdfStatus status = config_.io.metadata(config_.io.context, checkpointCommitHandle_, &metadata);
    if (!status) {
      return PdfStepResult::failure(status);
    }
    if (metadata.directory || metadata.symlinkLike || metadata.size != kCheckpointBytes) {
      return PdfStepResult::failure(PdfStatus::failure(PdfError::Malformed, metadata.size));
    }
    checkpointCommitStage_ = CheckpointCommitStage::ReadVerifier;
    return PdfStepResult::paused();
  }

  if (checkpointCommitStage_ == CheckpointCommitStage::ReadVerifier) {
    if (budget.bytesRemaining < kCheckpointBytes || !budget.consumeOperation()) {
      return PdfStepResult::paused();
    }
    (void)budget.takeBytes(kCheckpointBytes);
    size_t bytesRead = 0;
    PdfStatus status =
        config_.io.read(config_.io.context, checkpointCommitHandle_, 0, pageText_.get(), kCheckpointBytes, &bytesRead);
    if (!status || bytesRead != kCheckpointBytes) {
      return PdfStepResult::failure(status ? PdfStatus::failure(PdfError::UnexpectedEof, bytesRead) : status);
    }
    checkpointCommitStage_ = CheckpointCommitStage::CloseVerifier;
    return PdfStepResult::paused();
  }

  if (checkpointCommitStage_ == CheckpointCommitStage::CloseVerifier) {
    if (!budget.consumeOperation()) {
      return PdfStepResult::paused();
    }
    const PdfStatus status = config_.io.close(config_.io.context, &checkpointCommitHandle_);
    if (!status) {
      return PdfStepResult::failure(status);
    }
    checkpointCommitStage_ = CheckpointCommitStage::Verify;
    return PdfStepResult::paused();
  }

  if (checkpointCommitStage_ == CheckpointCommitStage::Verify) {
    FixedMemorySource source{pageText_.get(), kCheckpointBytes};
    PdfBuildCheckpoint verified{};
    const PdfStatus status = pdfDecodeBuildCheckpoint(source.source(), &verified);
    if (!status || !checkpointEqualPrep(verified, checkpointSelection_.checkpoint)) {
      return PdfStepResult::failure(status ? PdfStatus::failure(PdfError::Malformed) : status);
    }
    checkpointCommitStage_ = CheckpointCommitStage::Complete;
    return PdfStepResult::completed();
  }

  return checkpointCommitStage_ == CheckpointCommitStage::Complete
             ? PdfStepResult::completed()
             : PdfStepResult::failure(PdfStatus::failure(PdfError::InvalidArgument));
}

PdfStepResult PdfPreparation::stepCleanup(PdfWorkBudget& budget) {
  static_assert(sizeof(PdfCacheGenerationList) <= PdfLimits::SourceBufferBytes);
  if (!sourceWindow_) {
    return PdfStepResult::failure(PdfStatus::failure(PdfError::InsufficientMemory));
  }
  auto* const generations = reinterpret_cast<PdfCacheGenerationList*>(sourceWindow_.get());
  if (cleanupStage_ == CleanupStage::Idle) {
    new (generations) PdfCacheGenerationList{};
    cleanupIndex_ = 0;
    cleanupStage_ = CleanupStage::List;
    return PdfStepResult::paused();
  }
  if (cleanupStage_ == CleanupStage::List) {
    if (!budget.consumeOperation()) {
      return PdfStepResult::paused();
    }
    const PdfStatus status = cacheStore_.listGenerations(generations);
    if (!status) {
      return PdfStepResult::failure(status);
    }
    cleanupIndex_ = 0;
    cleanupStage_ = CleanupStage::Remove;
    return PdfStepResult::paused();
  }
  if (cleanupStage_ == CleanupStage::Remove) {
    if (cleanupIndex_ >= generations->count) {
      cleanupStage_ = CleanupStage::Complete;
      return PdfStepResult::completed();
    }
    const uint32_t candidate = generations->generations[cleanupIndex_];
    bool protectedGeneration = false;
    for (const PdfCacheManifestSlotState& slot : manifestSelection_.slots) {
      protectedGeneration = protectedGeneration || (slot.valid && slot.manifest.generation == candidate);
    }
    if (protectedGeneration) {
      ++cleanupIndex_;
      return PdfStepResult::paused();
    }
    if (!budget.consumeOperation()) {
      return PdfStepResult::paused();
    }
    const PdfStatus status = cacheStore_.removeGeneration(candidate);
    if (!status) {
      return PdfStepResult::failure(status);
    }
    ++cleanupIndex_;
    return PdfStepResult::paused();
  }
  return cleanupStage_ == CleanupStage::Complete
             ? PdfStepResult::completed()
             : PdfStepResult::failure(PdfStatus::failure(PdfError::InvalidArgument));
}

PdfStatus PdfPreparation::commitCheckpoint(const PdfBuildPhase phase) {
  PdfBuildCheckpoint checkpoint{};
  checkpoint.sequence = sequence_;
  checkpoint.source = sourceIdentity_;
  checkpoint.generation = generation_;
  checkpoint.phase = phase;
  checkpoint.lastVerifiedPage = pageCount_;
  checkpoint.emittedSections = sectionCount_;
  checkpoint.cumulativeWords = totalWords_;
  checkpoint.outputBytes = cumulativeSectionBytes_ + metadataRecord_.size + outlineRecord_.size;
  for (uint8_t index = 0; index < coverFileCount_; ++index) {
    checkpoint.outputBytes += coverRecords_[index].size;
  }
  return cacheStore_.commitCheckpoint(checkpoint);
}

PdfStepResult PdfPreparation::step() {
  if (phase_ == PdfPreparationPhase::Complete) {
    return PdfStepResult::completed();
  }
  if (phase_ == PdfPreparationPhase::Failed || phase_ == PdfPreparationPhase::Cancelled) {
    return PdfStepResult::failure(status_);
  }
  if (phase_ == PdfPreparationPhase::Idle) {
    return fail(PdfStatus::failure(PdfError::InvalidArgument));
  }
  if (cancelRequested_) {
    return cancel();
  }

  sliceStartedAtMs_ = nowMs();
  PdfWorkBudget budget{kSliceOperations, kSliceBytes, this, cancelRequested, this, sliceExpired};
  PdfStatus& operation = operationStatus_;
  operation = PdfStatus::success();

  switch (phase_) {
    case PdfPreparationPhase::ResourceGate:
      if (!resources_.has_value() || !resources_->canStart()) {
        return fail(PdfStatus::failure(PdfError::InsufficientMemory));
      }
      setPhase(PdfPreparationPhase::AllocateWorkspaces, 1);
      return pause();

    case PdfPreparationPhase::AllocateWorkspaces:
      if (!allocateNextWorkspace()) {
        return fail(PdfStatus::failure(PdfError::InsufficientMemory));
      }
      progressPercent_ = static_cast<uint8_t>(1 + allocationIndex_);
      if (allocationIndex_ == PDF_RESOURCE_SLOT_COUNT) {
        operation = initializeParserStorage();
        if (!operation) {
          return fail(operation);
        }
        setPhase(PdfPreparationPhase::OpenSource, 8);
      }
      return pause();

    case PdfPreparationPhase::OpenSource:
      operation = config_.io.open(config_.io.context, sourcePath_, PdfCacheOpenMode::Read, &sourceHandle_);
      if (operation) {
        operation = config_.io.metadata(config_.io.context, sourceHandle_, &sourceMetadata_);
      }
      if (!operation || sourceMetadata_.directory || sourceMetadata_.symlinkLike) {
        return fail(operation ? PdfStatus::failure(PdfError::InvalidArgument) : operation);
      }
      sourceContext_ = {&config_.io, &sourceHandle_, sourceMetadata_.size};
      sourceIdentity_.size = sourceMetadata_.size;
      sourceIdentity_.modificationTime = sourceMetadata_.modificationTime;
      setPhase(PdfPreparationPhase::FingerprintHead, 10);
      return pause();

    case PdfPreparationPhase::FingerprintHead: {
      const size_t length = static_cast<size_t>(std::min<uint64_t>(sourceMetadata_.size, PDF_SOURCE_FINGERPRINT_BYTES));
      size_t bytesRead = 0;
      if (length != 0) {
        operation = config_.io.read(config_.io.context, sourceHandle_, 0, sourceWindow_.get(), length, &bytesRead);
      }
      if (!operation || bytesRead != length) {
        return fail(operation ? PdfStatus::failure(PdfError::UnexpectedEof, bytesRead) : operation);
      }
      sourceIdentity_.headFingerprint = pdfFingerprintSourceWindow(
          PdfSourceFingerprintWindow::Head, sourceMetadata_.size, 0, sourceWindow_.get(), length);
      if (sourceMetadata_.size <= PDF_SOURCE_FINGERPRINT_BYTES) {
        sourceIdentity_.tailFingerprint = pdfFingerprintSourceWindow(
            PdfSourceFingerprintWindow::Tail, sourceMetadata_.size, 0, sourceWindow_.get(), length);
        setPhase(PdfPreparationPhase::PrepareCache, 14);
      } else {
        setPhase(PdfPreparationPhase::FingerprintTail, 12);
      }
      return pause();
    }

    case PdfPreparationPhase::FingerprintTail: {
      const uint64_t offset = sourceMetadata_.size - PDF_SOURCE_FINGERPRINT_BYTES;
      size_t bytesRead = 0;
      operation = config_.io.read(config_.io.context, sourceHandle_, offset, sourceWindow_.get(),
                                  PDF_SOURCE_FINGERPRINT_BYTES, &bytesRead);
      if (!operation || bytesRead != PDF_SOURCE_FINGERPRINT_BYTES) {
        return fail(operation ? PdfStatus::failure(PdfError::UnexpectedEof, offset + bytesRead) : operation);
      }
      sourceIdentity_.tailFingerprint =
          pdfFingerprintSourceWindow(PdfSourceFingerprintWindow::Tail, sourceMetadata_.size, offset,
                                     sourceWindow_.get(), PDF_SOURCE_FINGERPRINT_BYTES);
      setPhase(PdfPreparationPhase::PrepareCache, 14);
      return pause();
    }

    case PdfPreparationPhase::PrepareCache: {
      const PdfStepResult result = stepSetupCache(budget);
      if (result.failed()) {
        return fail(result.status);
      }
      if (result.yielded()) {
        return pause();
      }
    }
      operation = startXref();
      if (!operation) {
        return fail(operation);
      }
      setPhase(PdfPreparationPhase::ParseXref, 20);
      return pause();

    case PdfPreparationPhase::ParseXref: {
      const PdfStepResult result = xrefParser_->step(budget);
      if (cancelRequested_) {
        return cancel();
      }
      if (result.failed()) {
        return fail(result.status);
      }
      if (result.yielded()) {
        return pause();
      }
      operation = finishXref();
      if (!operation) {
        return fail(operation);
      }
      setPhase(PdfPreparationPhase::ResolveCatalog, 35);
      return pause();
    }

    case PdfPreparationPhase::ResolveCatalog: {
      const PdfStepResult result = resolver_->step(budget);
      if (cancelRequested_) {
        return cancel();
      }
      if (result.failed()) {
        return fail(result.status);
      }
      if (result.yielded()) {
        return pause();
      }
      operation = finishCatalog();
      if (!operation) {
        return fail(operation);
      }
      setPhase(PdfPreparationPhase::WalkPages, 45);
      return pause();
    }

    case PdfPreparationPhase::WalkPages: {
      const PdfStepResult result = pageWalker_->step(budget);
      if (cancelRequested_) {
        return cancel();
      }
      if (result.failed()) {
        return fail(result.status);
      }
      if (result.yielded()) {
        return pause();
      }
      operation = finishPageTree();
      if (!operation) {
        return fail(operation);
      }
      setPhase(PdfPreparationPhase::ResolveNavigation, 55);
      return pause();
    }

    case PdfPreparationPhase::ResolveNavigation: {
      if (navigationTask_ == NavigationTask::Complete) {
        currentPageIndex_ = 0;
        operation = beginCurrentPageImages();
        if (!operation) {
          return fail(operation);
        }
        setPhase(imageResolveTask_ == ImageResolveTask::None ? PdfPreparationPhase::ResolveContent
                                                             : PdfPreparationPhase::ResolveImageResources,
                 64);
        return pause();
      }
      const NavigationTask completedTask = navigationTask_;
      const PdfStepResult result = resolver_->step(budget);
      if (cancelRequested_) {
        return cancel();
      }
      if (result.failed()) {
        return fail(result.status);
      }
      if (result.yielded()) {
        return pause();
      }
      operation = finishNavigationObject();
      if (!operation) {
        return fail(operation);
      }
      if (completedTask == NavigationTask::Xmp) {
        setPhase(PdfPreparationPhase::ReadXmpMetadata, 60);
      }
      return pause();
    }

    case PdfPreparationPhase::ReadXmpMetadata:
      operation = readXmpMetadata();
      if (!operation) {
        return fail(operation);
      }
      setPhase(PdfPreparationPhase::ResolveNavigation, 62);
      return pause();

    case PdfPreparationPhase::ResolveImageResources: {
      const PdfStepResult result = resolver_->step(budget);
      if (cancelRequested_) {
        return cancel();
      }
      if (result.failed()) {
        return fail(result.status);
      }
      if (result.yielded()) {
        return pause();
      }
      operation = finishImageResolution();
      if (!operation) {
        return fail(operation);
      }
      if (imageResolveTask_ == ImageResolveTask::None) {
        setPhase(PdfPreparationPhase::ResolveContent, 68);
      }
      return pause();
    }

    case PdfPreparationPhase::ResolveContent: {
      const PdfStepResult result = resolver_->step(budget);
      if (cancelRequested_) {
        return cancel();
      }
      if (result.failed()) {
        return fail(result.status);
      }
      if (result.yielded()) {
        return pause();
      }
      operation = finishContentObject();
      if (!operation) {
        return fail(operation);
      }
      setPhase(PdfPreparationPhase::ExtractText, 70);
      return pause();
    }

    case PdfPreparationPhase::ExtractText:
      while (budget.operationsRemaining != 0 && budget.bytesRemaining != 0 && !budget.stopRequested()) {
        if (inlineImageAwaitingData_) {
          const PdfStepResult inlineResult = finishInlineImageData(budget);
          if (inlineResult.failed()) {
            return fail(inlineResult.status);
          }
          if (inlineResult.yielded()) {
            return pause();
          }
          continue;
        }
        if (navigation_ == nullptr) {
          return fail(PdfStatus::failure(PdfError::InvalidArgument));
        }
        PdfToken& token = navigation_->contentTokenScratch;
        resetInPlace(token);
        const PdfStepResult result = contentLexer_->next(token, budget);
        if (cancelRequested_) {
          return cancel();
        }
        if (result.failed()) {
          return fail(result.status);
        }
        if (result.yielded()) {
          return pause();
        }
        if (token.kind == PdfTokenKind::End) {
          operation = finishExtractedPage();
          if (!operation) {
            return fail(operation);
          }
          setPhase(currentPageImageCandidate_ >= 0 ? PdfPreparationPhase::CacheImage : PdfPreparationPhase::OpenSection,
                   78);
          return pause();
        }
        operation = appendContentToken(token);
        if (!operation) {
          return fail(operation);
        }
      }
      return pause();

    case PdfPreparationPhase::CacheImage: {
      const PdfStepResult result = cacheCurrentPageImage(budget);
      if (cancelRequested_) {
        return cancel();
      }
      if (result.failed()) {
        return fail(result.status);
      }
      if (result.yielded()) {
        return pause();
      }
    }
      if (currentPageImageCandidate_ >= 0 &&
          static_cast<uint8_t>(currentPageImageCandidate_ + 1) < currentPageImageEnd_) {
        ++currentPageImageCandidate_;
      } else {
        currentPageImageCandidate_ = -1;
        setPhase(PdfPreparationPhase::OpenSection, 80);
      }
      return pause();

    case PdfPreparationPhase::CloseSource:
      if (!navigationRecordsPrepared_) {
        operation = closeSource();
        if (!operation) {
          return fail(operation);
        }
        operation = prepareNavigationRecords();
        if (!operation) {
          return fail(operation);
        }
        navigationRecordsPrepared_ = true;
        return pause();
      }
      {
        const PdfStepResult result = stepTypographyAssets(budget);
        if (cancelRequested_) {
          return cancel();
        }
        if (result.failed()) {
          return fail(result.status);
        }
        if (result.yielded()) {
          return pause();
        }
      }
      setPhase(PdfPreparationPhase::OpenMetadata, 91);
      return pause();

    case PdfPreparationPhase::OpenSection:
      operation = openSection();
      if (!operation) {
        return fail(operation);
      }
      setPhase(PdfPreparationPhase::EmitSection, 86);
      return pause();

    case PdfPreparationPhase::EmitSection: {
      const PdfStepResult result = emitSection(budget);
      if (cancelRequested_) {
        return cancel();
      }
      if (result.failed()) {
        return fail(result.status);
      }
      if (result.yielded()) {
        return pause();
      }
    }
      setPhase(PdfPreparationPhase::CloseSection, 90);
      return pause();

    case PdfPreparationPhase::CloseSection:
      operation = closeSection();
      if (!operation) {
        return fail(operation);
      }
      if (currentBlockIndex_ < extractedBlockCount_) {
        setPhase(PdfPreparationPhase::OpenSection, 78);
      } else if (++currentPageIndex_ < pageCount_) {
        operation = beginCurrentPageImages();
        if (!operation) {
          return fail(operation);
        }
        setPhase(imageResolveTask_ == ImageResolveTask::None ? PdfPreparationPhase::ResolveContent
                                                             : PdfPreparationPhase::ResolveImageResources,
                 68);
      } else if (imageBuildSpool_.recordCount() != 0) {
        operation = imageBuildSpool_.closeWrite();
        if (!operation) {
          return fail(operation);
        }
        setPhase(PdfPreparationPhase::SpoolNavigation, 90);
      } else {
        if (imageFileSpool_.writing()) {
          operation = imageFileSpool_.closeWrite();
          if (!operation) {
            return fail(operation);
          }
        }
        setPhase(PdfPreparationPhase::CloseSource, 90);
      }
      return pause();

    case PdfPreparationPhase::SpoolNavigation: {
      const PdfStepResult result = spoolNavigation(budget);
      if (cancelRequested_) {
        return cancel();
      }
      if (result.failed()) {
        return fail(result.status);
      }
      if (result.yielded()) {
        return pause();
      }
    }
      setPhase(PdfPreparationPhase::DecodeImages, 90);
      return pause();

    case PdfPreparationPhase::DecodeImages: {
      const PdfStepResult result = decodeRasterBatch(budget);
      if (cancelRequested_) {
        return cancel();
      }
      if (result.failed()) {
        return fail(result.status);
      }
      if (result.yielded()) {
        return pause();
      }
    }
      setPhase(PdfPreparationPhase::RestoreNavigation, 90);
      return pause();

    case PdfPreparationPhase::RestoreNavigation: {
      const PdfStepResult result = restoreNavigation(budget);
      if (cancelRequested_) {
        return cancel();
      }
      if (result.failed()) {
        return fail(result.status);
      }
      if (result.yielded()) {
        return pause();
      }
    }
      setPhase(PdfPreparationPhase::RepairImageSections, 90);
      return pause();

    case PdfPreparationPhase::RepairImageSections: {
      const PdfStepResult result = repairFailedImageSections(budget);
      if (cancelRequested_) {
        return cancel();
      }
      if (result.failed()) {
        return fail(result.status);
      }
      if (result.yielded()) {
        return pause();
      }
    }
      setPhase(PdfPreparationPhase::CloseSource, 90);
      return pause();

    case PdfPreparationPhase::OpenMetadata:
      operation = openMetadata();
      if (!operation) {
        return fail(operation);
      }
      setPhase(PdfPreparationPhase::WriteMetadata, 92);
      return pause();

    case PdfPreparationPhase::WriteMetadata:
      operation = writeMetadata();
      if (!operation) {
        return fail(operation);
      }
      setPhase(PdfPreparationPhase::CloseMetadata, 93);
      return pause();

    case PdfPreparationPhase::CloseMetadata:
      operation = closeMetadata();
      if (!operation) {
        return fail(operation);
      }
      setPhase(PdfPreparationPhase::OpenOutline, 94);
      return pause();

    case PdfPreparationPhase::OpenOutline:
      operation = openOutline();
      if (!operation) {
        return fail(operation);
      }
      setPhase(PdfPreparationPhase::WriteOutline, 95);
      return pause();

    case PdfPreparationPhase::WriteOutline: {
      const PdfStepResult result = stepWriteOutline(budget);
      if (result.failed()) {
        return fail(result.status);
      }
      if (result.yielded()) {
        return pause();
      }
    }
      setPhase(PdfPreparationPhase::CloseOutline, 96);
      return pause();

    case PdfPreparationPhase::CloseOutline:
      operation = closeOutline();
      if (!operation) {
        return fail(operation);
      }
      setPhase(PdfPreparationPhase::CommitManifest, 97);
      return pause();

    case PdfPreparationPhase::CommitManifest: {
      const PdfStepResult result = stepCommitManifest(budget);
      if (cancelRequested_) {
        return cancel();
      }
      if (result.failed()) {
        return fail(result.status);
      }
      if (result.yielded()) {
        return pause();
      }
    }
      setPhase(PdfPreparationPhase::CommitCheckpoint, 98);
      return pause();

    case PdfPreparationPhase::CommitCheckpoint: {
      const PdfStepResult result = stepCommitCheckpoint(budget, PdfBuildPhase::Complete);
      if (result.failed()) {
        return fail(result.status);
      }
      if (result.yielded()) {
        return pause();
      }
    }
      setPhase(PdfPreparationPhase::Cleanup, 99);
      return pause();

    case PdfPreparationPhase::Cleanup: {
      const PdfStepResult result = stepCleanup(budget);
      if (result.failed()) {
        return fail(result.status);
      }
      if (result.yielded()) {
        return pause();
      }
    }
      destroyParsers();
      releaseWorkspaces();
      status_ = PdfStatus::success();
      setPhase(PdfPreparationPhase::Complete, 100);
      return PdfStepResult::completed();

    case PdfPreparationPhase::Complete:
      return PdfStepResult::completed();
    case PdfPreparationPhase::Failed:
    case PdfPreparationPhase::Cancelled:
      return PdfStepResult::failure(status_);
    case PdfPreparationPhase::Idle:
    default:
      return fail(PdfStatus::failure(PdfError::InvalidArgument));
  }
}
