#include "PdfJpegPreview.h"

#include <algorithm>
#include <cstring>
#include <new>

namespace {

constexpr size_t kBmpHeaderBytes = 62;
constexpr size_t kEntropyHeadroom = 512;

uint16_t readBe16(const uint8_t* const bytes) {
  return static_cast<uint16_t>((static_cast<uint16_t>(bytes[0]) << 8U) | bytes[1]);
}

void writeLe16(uint8_t* const bytes, const uint16_t value) {
  bytes[0] = static_cast<uint8_t>(value);
  bytes[1] = static_cast<uint8_t>(value >> 8U);
}

void writeLe32(uint8_t* const bytes, const uint32_t value) {
  bytes[0] = static_cast<uint8_t>(value);
  bytes[1] = static_cast<uint8_t>(value >> 8U);
  bytes[2] = static_cast<uint8_t>(value >> 16U);
  bytes[3] = static_cast<uint8_t>(value >> 24U);
}

int32_t extendJpegValue(const uint16_t value, const uint8_t bits) {
  if (bits == 0) {
    return 0;
  }
  const uint16_t threshold = static_cast<uint16_t>(1U << (bits - 1U));
  return value >= threshold ? static_cast<int32_t>(value)
                            : static_cast<int32_t>(value) - static_cast<int32_t>((1U << bits) - 1U);
}

template <typename T>
void resetInPlace(T& value) {
  value.~T();
  new (&value) T();
}

}  // namespace

PdfStatus PdfJpegPreview::beginHeader(const PdfCacheIo& io, const PdfCacheHandle source, const uint64_t sourceBytes,
                                      uint8_t* const ioBuffer, const size_t ioBufferBytes, uint8_t* const workspace,
                                      const size_t workspaceBytes) {
  reset();
  if (!io.valid() || !source.valid() || sourceBytes < 4U || ioBuffer == nullptr ||
      ioBufferBytes < kEntropyHeadroom || ioBufferBytes > PdfLimits::SourceBufferBytes || workspace == nullptr ||
      workspaceBytes < 64U) {
    return PdfStatus::failure(PdfError::InvalidArgument);
  }
  io_ = io;
  source_ = source;
  sourceBytes_ = sourceBytes;
  ioBuffer_ = ioBuffer;
  ioBufferBytes_ = ioBufferBytes;
  workspace_ = workspace;
  workspaceBytes_ = workspaceBytes;
  headerStage_ = HeaderStage::Soi;
  return PdfStatus::success();
}

bool PdfJpegPreview::headerReady() const { return headerStage_ == HeaderStage::Ready; }

void PdfJpegPreview::reset() { resetInPlace(*this); }

PdfStepResult PdfJpegPreview::readAt(const uint64_t offset, const size_t requested, PdfWorkBudget& budget) {
  if (!source_.valid() || ioBuffer_ == nullptr || requested == 0 || requested > ioBufferBytes_ ||
      offset > sourceBytes_ || requested > sourceBytes_ - offset) {
    return PdfStepResult::failure(PdfStatus::failure(PdfError::UnexpectedEof, offset));
  }
  if (budget.operationsRemaining == 0 || budget.bytesRemaining < requested || budget.stopRequested()) {
    return PdfStepResult::paused();
  }
  (void)budget.consumeOperation();
  (void)budget.takeBytes(requested);
  size_t bytesRead = 0;
  const PdfStatus status = io_.read(io_.context, source_, offset, ioBuffer_, requested, &bytesRead);
  if (!status || bytesRead != requested) {
    return PdfStepResult::failure(status ? PdfStatus::failure(PdfError::UnexpectedEof, offset + bytesRead) : status);
  }
  return PdfStepResult::completed();
}

PdfStepResult PdfJpegPreview::stepHeader(PdfWorkBudget& budget) {
  if (headerStage_ == HeaderStage::Idle) {
    return PdfStepResult::failure(PdfStatus::failure(PdfError::InvalidArgument));
  }
  if (headerStage_ == HeaderStage::Ready) {
    return PdfStepResult::completed();
  }
  if (headerStage_ == HeaderStage::Soi) {
    const PdfStepResult read = readAt(0, 2, budget);
    if (!read.complete()) {
      return read;
    }
    if (ioBuffer_[0] != 0xff || ioBuffer_[1] != 0xd8) {
      return PdfStepResult::failure(PdfStatus::failure(PdfError::Malformed));
    }
    headerOffset_ = 2;
    headerStage_ = HeaderStage::Marker;
    return PdfStepResult::paused();
  }
  if (headerStage_ == HeaderStage::Marker) {
    const PdfStepResult read = readAt(headerOffset_, 4, budget);
    if (!read.complete()) {
      return read;
    }
    if (ioBuffer_[0] != 0xff || ioBuffer_[1] == 0x00 || ioBuffer_[1] == 0xff) {
      return PdfStepResult::failure(PdfStatus::failure(PdfError::Malformed, headerOffset_));
    }
    marker_ = ioBuffer_[1];
    if (marker_ == 0xd9 || (marker_ >= 0xd0 && marker_ <= 0xd7) || marker_ == 0x01) {
      return PdfStepResult::failure(PdfStatus::failure(PdfError::Malformed, headerOffset_));
    }
    const uint16_t segmentBytes = readBe16(ioBuffer_ + 2);
    if (segmentBytes < 2U || headerOffset_ > sourceBytes_ ||
        static_cast<uint64_t>(segmentBytes) + 2U > sourceBytes_ - headerOffset_) {
      return PdfStepResult::failure(PdfStatus::failure(PdfError::Malformed, headerOffset_));
    }
    payloadLength_ = segmentBytes - 2U;
    segmentEnd_ = headerOffset_ + 2U + segmentBytes;
    const bool inspect = marker_ == 0xc0 || marker_ == 0xc1 || marker_ == 0xc2 || marker_ == 0xc3 ||
                         marker_ == 0xc4 || marker_ == 0xda || marker_ == 0xdb || marker_ == 0xdd;
    if (!inspect) {
      headerOffset_ = segmentEnd_;
      return PdfStepResult::paused();
    }
    if (payloadLength_ > ioBufferBytes_) {
      return PdfStepResult::failure(PdfStatus::failure(PdfError::LimitExceeded, payloadLength_));
    }
    headerStage_ = HeaderStage::Payload;
    return PdfStepResult::paused();
  }
  if (headerStage_ != HeaderStage::Payload) {
    return PdfStepResult::failure(PdfStatus::failure(PdfError::InvalidArgument));
  }
  const PdfStepResult read = readAt(headerOffset_ + 4U, payloadLength_, budget);
  if (!read.complete()) {
    return read;
  }
  const PdfStatus status = parseMarkerPayload();
  if (!status) {
    return PdfStepResult::failure(status);
  }
  headerOffset_ = segmentEnd_;
  if (marker_ == 0xda) {
    entropyStart_ = headerOffset_;
    headerStage_ = HeaderStage::Ready;
    return PdfStepResult::completed();
  }
  headerStage_ = HeaderStage::Marker;
  return PdfStepResult::paused();
}

PdfStatus PdfJpegPreview::parseMarkerPayload() {
  switch (marker_) {
    case 0xc0:
      return parseFrame(ioBuffer_, payloadLength_);
    case 0xc1:
    case 0xc2:
    case 0xc3:
      return PdfStatus::failure(PdfError::Unsupported, marker_);
    case 0xc4:
      return parseHuffmanTables(ioBuffer_, payloadLength_);
    case 0xda:
      return parseScan(ioBuffer_, payloadLength_);
    case 0xdb:
      return parseQuantizationTables(ioBuffer_, payloadLength_);
    case 0xdd:
      return parseRestartInterval(ioBuffer_, payloadLength_);
    default:
      return PdfStatus::failure(PdfError::Malformed, marker_);
  }
}

PdfStatus PdfJpegPreview::parseQuantizationTables(const uint8_t* const payload, const size_t length) {
  size_t offset = 0;
  while (offset < length) {
    const uint8_t information = payload[offset++];
    const uint8_t precision = information >> 4U;
    const uint8_t table = information & 0x0fU;
    const size_t tableBytes = precision == 0 ? 64U : precision == 1 ? 128U : 0U;
    if (table >= 4 || tableBytes == 0 || tableBytes > length - offset) {
      return PdfStatus::failure(PdfError::Malformed, headerOffset_ + 4U + offset);
    }
    quantizationDc_[table] = precision == 0 ? payload[offset] : readBe16(payload + offset);
    quantizationValid_[table] = quantizationDc_[table] != 0;
    offset += tableBytes;
  }
  return PdfStatus::success();
}

PdfStatus PdfJpegPreview::parseFrame(const uint8_t* const payload, const size_t length) {
  if (frameSeen_ || length < 6U || payload[0] != 8U) {
    return PdfStatus::failure(payload[0] == 8U ? PdfError::Malformed : PdfError::Unsupported, marker_);
  }
  height_ = readBe16(payload + 1);
  width_ = readBe16(payload + 3);
  componentCount_ = payload[5];
  if (width_ == 0 || height_ == 0 || componentCount_ == 0 || componentCount_ > 4 ||
      length != 6U + static_cast<size_t>(componentCount_) * 3U) {
    return PdfStatus::failure(PdfError::Malformed, headerOffset_);
  }
  maximumHorizontalSampling_ = 0;
  maximumVerticalSampling_ = 0;
  for (uint8_t index = 0; index < componentCount_; ++index) {
    const size_t componentOffset = 6U + static_cast<size_t>(index) * 3U;
    Component& component = components_[index];
    component.id = payload[componentOffset];
    component.horizontalSampling = payload[componentOffset + 1U] >> 4U;
    component.verticalSampling = payload[componentOffset + 1U] & 0x0fU;
    component.quantizationTable = payload[componentOffset + 2U];
    if (component.horizontalSampling == 0 || component.horizontalSampling > 4 || component.verticalSampling == 0 ||
        component.verticalSampling > 4 || component.quantizationTable >= 4) {
      return PdfStatus::failure(PdfError::Unsupported, componentOffset);
    }
    maximumHorizontalSampling_ = std::max(maximumHorizontalSampling_, component.horizontalSampling);
    maximumVerticalSampling_ = std::max(maximumVerticalSampling_, component.verticalSampling);
  }
  frameSeen_ = true;
  return PdfStatus::success();
}

PdfStatus PdfJpegPreview::parseHuffmanTables(const uint8_t* const payload, const size_t length) {
  size_t offset = 0;
  while (offset < length) {
    const uint8_t information = payload[offset++];
    const uint8_t tableClass = information >> 4U;
    const uint8_t tableIndex = information & 0x0fU;
    if (tableClass > 1 || tableIndex >= 4 || length - offset < 16U) {
      return PdfStatus::failure(PdfError::Malformed, headerOffset_ + 4U + offset);
    }
    HuffmanTable& table = tableClass == 0 ? dcTables_[tableIndex] : acTables_[tableIndex];
    std::memcpy(table.counts, payload + offset, sizeof(table.counts));
    offset += sizeof(table.counts);
    uint16_t symbolCount = 0;
    for (const uint8_t count : table.counts) {
      symbolCount = static_cast<uint16_t>(symbolCount + count);
    }
    if (symbolCount == 0 || symbolCount > sizeof(table.symbols) || symbolCount > length - offset) {
      return PdfStatus::failure(PdfError::Malformed, headerOffset_ + 4U + offset);
    }
    std::memcpy(table.symbols, payload + offset, symbolCount);
    offset += symbolCount;
    table.symbolCount = symbolCount;
    table.valid = true;
  }
  return PdfStatus::success();
}

PdfStatus PdfJpegPreview::parseRestartInterval(const uint8_t* const payload, const size_t length) {
  if (length != 2U) {
    return PdfStatus::failure(PdfError::Malformed, headerOffset_);
  }
  restartInterval_ = readBe16(payload);
  return PdfStatus::success();
}

PdfStatus PdfJpegPreview::parseScan(const uint8_t* const payload, const size_t length) {
  if (!frameSeen_ || length < 4U) {
    return PdfStatus::failure(PdfError::Malformed, headerOffset_);
  }
  scanComponentCount_ = payload[0];
  if (scanComponentCount_ != componentCount_ || length != 1U + static_cast<size_t>(scanComponentCount_) * 2U + 3U) {
    return PdfStatus::failure(PdfError::Unsupported, headerOffset_);
  }
  bool seen[4]{};
  for (uint8_t scanIndex = 0; scanIndex < scanComponentCount_; ++scanIndex) {
    const uint8_t id = payload[1U + static_cast<size_t>(scanIndex) * 2U];
    uint8_t componentIndex = UINT8_MAX;
    for (uint8_t index = 0; index < componentCount_; ++index) {
      if (components_[index].id == id) {
        componentIndex = index;
        break;
      }
    }
    const uint8_t tables = payload[2U + static_cast<size_t>(scanIndex) * 2U];
    if (componentIndex == UINT8_MAX || seen[componentIndex] || (tables >> 4U) >= 4 || (tables & 0x0fU) >= 4) {
      return PdfStatus::failure(PdfError::Malformed, headerOffset_);
    }
    seen[componentIndex] = true;
    scanComponents_[scanIndex] = componentIndex;
    components_[componentIndex].dcTable = tables >> 4U;
    components_[componentIndex].acTable = tables & 0x0fU;
  }
  const size_t spectralOffset = 1U + static_cast<size_t>(scanComponentCount_) * 2U;
  if (payload[spectralOffset] != 0 || payload[spectralOffset + 1U] != 63 || payload[spectralOffset + 2U] != 0) {
    return PdfStatus::failure(PdfError::Unsupported, headerOffset_);
  }
  luminanceComponent_ = 0;
  for (uint8_t index = 0; index < componentCount_; ++index) {
    if (components_[index].id == 1U) {
      luminanceComponent_ = index;
      break;
    }
  }
  const Component& luminance = components_[luminanceComponent_];
  if (luminance.horizontalSampling != maximumHorizontalSampling_ ||
      luminance.verticalSampling != maximumVerticalSampling_ || !quantizationValid_[luminance.quantizationTable]) {
    return PdfStatus::failure(PdfError::Unsupported, headerOffset_);
  }
  for (uint8_t index = 0; index < componentCount_; ++index) {
    const Component& component = components_[index];
    if (!quantizationValid_[component.quantizationTable] || !dcTables_[component.dcTable].valid ||
        !acTables_[component.acTable].valid) {
      return PdfStatus::failure(PdfError::Malformed, headerOffset_);
    }
  }
  blockColumns_ = static_cast<uint16_t>((static_cast<uint32_t>(width_) + 7U) / 8U);
  mcuColumns_ = static_cast<uint16_t>(
      (static_cast<uint32_t>(width_) + 8U * maximumHorizontalSampling_ - 1U) /
      (8U * maximumHorizontalSampling_));
  mcuRows_ = static_cast<uint16_t>((static_cast<uint32_t>(height_) + 8U * maximumVerticalSampling_ - 1U) /
                                   (8U * maximumVerticalSampling_));
  const uint32_t blockRowBytes = static_cast<uint32_t>(blockColumns_) * maximumVerticalSampling_;
  if (mcuColumns_ == 0 || mcuRows_ == 0 || blockRowBytes == 0 || blockRowBytes > workspaceBytes_) {
    return PdfStatus::failure(PdfError::LimitExceeded, blockRowBytes);
  }
  blockRowBytes_ = static_cast<uint16_t>(blockRowBytes);
  return PdfStatus::success();
}

PdfStatus PdfJpegPreview::beginAsset(PdfCacheTrackedWriter* const writer, const uint16_t targetWidth,
                                     const uint16_t targetHeight) {
  if (!headerReady() || writer == nullptr || !writer->open || targetWidth == 0 || targetHeight == 0) {
    return PdfStatus::failure(PdfError::InvalidArgument);
  }
  const uint32_t rowBytes = ((static_cast<uint32_t>(targetWidth) + 31U) / 32U) * 4U;
  if (rowBytes > UINT16_MAX || static_cast<size_t>(blockRowBytes_) + rowBytes > workspaceBytes_) {
    return PdfStatus::failure(PdfError::LimitExceeded, rowBytes);
  }
  writer_ = writer;
  targetWidth_ = targetWidth;
  targetHeight_ = targetHeight;
  outputRowBytes_ = static_cast<uint16_t>(rowBytes);
  const uint64_t widthLimitedHeight = static_cast<uint64_t>(height_) * targetWidth / width_;
  if (widthLimitedHeight <= targetHeight) {
    scaledWidth_ = targetWidth;
    scaledHeight_ = static_cast<uint16_t>(std::max<uint64_t>(1, widthLimitedHeight));
  } else {
    scaledHeight_ = targetHeight;
    scaledWidth_ =
        static_cast<uint16_t>(std::max<uint64_t>(1, static_cast<uint64_t>(width_) * targetHeight / height_));
  }
  offsetX_ = static_cast<uint16_t>((targetWidth - scaledWidth_) / 2U);
  offsetY_ = static_cast<uint16_t>((targetHeight - scaledHeight_) / 2U);
  outputRow_ = 0;
  currentMcuX_ = 0;
  currentMcuY_ = 0;
  readyMcuRow_ = 0;
  currentScanComponent_ = 0;
  currentComponentBlock_ = 0;
  entropyBufferOffset_ = entropyStart_;
  entropyBufferLength_ = 0;
  entropyBufferIndex_ = 0;
  bitBuffer_ = 0;
  bitCount_ = 0;
  mcusSinceRestart_ = 0;
  expectedRestartMarker_ = 0;
  decodedRowReady_ = false;
  restartPending_ = false;
  for (uint8_t index = 0; index < componentCount_; ++index) {
    components_[index].predictor = 0;
  }
  std::memset(workspace_, 0xff, static_cast<size_t>(blockRowBytes_) + outputRowBytes_);
  assetStage_ = AssetStage::Header;
  return PdfStatus::success();
}

uint64_t PdfJpegPreview::currentEntropyOffset() const {
  return entropyBufferOffset_ + static_cast<uint64_t>(entropyBufferIndex_);
}

PdfStepResult PdfJpegPreview::refillEntropy(PdfWorkBudget& budget) {
  const uint64_t offset = currentEntropyOffset();
  if (offset >= sourceBytes_) {
    return PdfStepResult::failure(PdfStatus::failure(PdfError::UnexpectedEof, offset));
  }
  const size_t requested =
      static_cast<size_t>(std::min<uint64_t>(ioBufferBytes_, sourceBytes_ - offset));
  const PdfStepResult read = readAt(offset, requested, budget);
  if (!read.complete()) {
    return read;
  }
  entropyBufferOffset_ = offset;
  entropyBufferLength_ = requested;
  entropyBufferIndex_ = 0;
  return PdfStepResult::completed();
}

bool PdfJpegPreview::entropyByte(uint8_t* const value) {
  if (value == nullptr || entropyBufferIndex_ >= entropyBufferLength_) {
    return false;
  }
  uint8_t byte = ioBuffer_[entropyBufferIndex_++];
  if (byte != 0xff) {
    *value = byte;
    return true;
  }
  if (entropyBufferIndex_ >= entropyBufferLength_) {
    return false;
  }
  const uint8_t escaped = ioBuffer_[entropyBufferIndex_++];
  if (escaped != 0x00) {
    return false;
  }
  *value = 0xff;
  return true;
}

bool PdfJpegPreview::entropyBits(const uint8_t count, uint16_t* const value) {
  if (value == nullptr || count > 16U) {
    return false;
  }
  while (bitCount_ < count) {
    uint8_t byte = 0;
    if (!entropyByte(&byte)) {
      return false;
    }
    bitBuffer_ = (bitBuffer_ << 8U) | byte;
    bitCount_ = static_cast<uint8_t>(bitCount_ + 8U);
  }
  if (count == 0) {
    *value = 0;
    return true;
  }
  const uint32_t mask = (1UL << count) - 1UL;
  *value = static_cast<uint16_t>((bitBuffer_ >> (bitCount_ - count)) & mask);
  bitCount_ = static_cast<uint8_t>(bitCount_ - count);
  bitBuffer_ &= bitCount_ == 0 ? 0U : (1UL << bitCount_) - 1UL;
  return true;
}

bool PdfJpegPreview::huffmanValue(const HuffmanTable& table, uint8_t* const value) {
  if (!table.valid || value == nullptr) {
    return false;
  }
  uint16_t code = 0;
  uint16_t firstCode = 0;
  uint16_t symbolOffset = 0;
  for (uint8_t length = 1; length <= 16; ++length) {
    uint16_t bit = 0;
    if (!entropyBits(1, &bit)) {
      return false;
    }
    code = static_cast<uint16_t>((code << 1U) | bit);
    const uint8_t count = table.counts[length - 1U];
    if (code >= firstCode && code - firstCode < count) {
      const uint16_t index = static_cast<uint16_t>(symbolOffset + code - firstCode);
      if (index >= table.symbolCount) {
        return false;
      }
      *value = table.symbols[index];
      return true;
    }
    firstCode = static_cast<uint16_t>((firstCode + count) << 1U);
    symbolOffset = static_cast<uint16_t>(symbolOffset + count);
  }
  return false;
}

PdfStatus PdfJpegPreview::decodeBlock(Component& component, const bool luminance, const uint8_t blockIndex) {
  uint8_t dcBits = 0;
  if (!huffmanValue(dcTables_[component.dcTable], &dcBits) || dcBits > 15U) {
    return PdfStatus::failure(PdfError::Malformed, currentEntropyOffset());
  }
  uint16_t dcValue = 0;
  if (!entropyBits(dcBits, &dcValue)) {
    return PdfStatus::failure(PdfError::Malformed, currentEntropyOffset());
  }
  component.predictor += extendJpegValue(dcValue, dcBits);
  uint8_t coefficient = 1;
  while (coefficient < 64U) {
    uint8_t symbol = 0;
    if (!huffmanValue(acTables_[component.acTable], &symbol)) {
      return PdfStatus::failure(PdfError::Malformed, currentEntropyOffset());
    }
    if (symbol == 0) {
      break;
    }
    const uint8_t run = symbol >> 4U;
    const uint8_t bits = symbol & 0x0fU;
    if (bits == 0) {
      if (run != 15U || coefficient > 47U) {
        return PdfStatus::failure(PdfError::Malformed, currentEntropyOffset());
      }
      coefficient = static_cast<uint8_t>(coefficient + 16U);
      continue;
    }
    if (coefficient + run >= 64U || bits > 15U) {
      return PdfStatus::failure(PdfError::Malformed, currentEntropyOffset());
    }
    coefficient = static_cast<uint8_t>(coefficient + run);
    uint16_t discarded = 0;
    if (!entropyBits(bits, &discarded)) {
      return PdfStatus::failure(PdfError::Malformed, currentEntropyOffset());
    }
    ++coefficient;
  }
  if (luminance) {
    const int32_t dc =
        component.predictor * static_cast<int32_t>(quantizationDc_[component.quantizationTable]);
    const int32_t mean = std::clamp<int32_t>(128 + dc / 8, 0, 255);
    const uint8_t localX = blockIndex % component.horizontalSampling;
    const uint8_t localY = blockIndex / component.horizontalSampling;
    const uint16_t blockX =
        static_cast<uint16_t>(currentMcuX_ * maximumHorizontalSampling_ + localX);
    if (blockX < blockColumns_ && localY < maximumVerticalSampling_) {
      workspace_[static_cast<size_t>(localY) * blockColumns_ + blockX] = static_cast<uint8_t>(mean);
    }
  }
  return PdfStatus::success();
}

void PdfJpegPreview::advanceDecodedBlock() {
  const uint8_t componentIndex = scanComponents_[currentScanComponent_];
  const Component& component = components_[componentIndex];
  ++currentComponentBlock_;
  if (currentComponentBlock_ < component.horizontalSampling * component.verticalSampling) {
    return;
  }
  currentComponentBlock_ = 0;
  ++currentScanComponent_;
  if (currentScanComponent_ < scanComponentCount_) {
    return;
  }
  currentScanComponent_ = 0;
  ++mcusSinceRestart_;
  ++currentMcuX_;
  if (currentMcuX_ < mcuColumns_) {
    if (restartInterval_ != 0 && mcusSinceRestart_ == restartInterval_) {
      restartPending_ = true;
    }
    return;
  }
  readyMcuRow_ = currentMcuY_;
  decodedRowReady_ = true;
  currentMcuX_ = 0;
  ++currentMcuY_;
  if (restartInterval_ != 0 && mcusSinceRestart_ == restartInterval_ &&
      currentMcuY_ < mcuRows_) {
    restartPending_ = true;
  }
}

PdfStatus PdfJpegPreview::consumeRestartMarker() {
  bitBuffer_ = 0;
  bitCount_ = 0;
  if (entropyBufferIndex_ + 2U > entropyBufferLength_ || ioBuffer_[entropyBufferIndex_] != 0xff ||
      ioBuffer_[entropyBufferIndex_ + 1U] != static_cast<uint8_t>(0xd0U + expectedRestartMarker_)) {
    return PdfStatus::failure(PdfError::Malformed, currentEntropyOffset());
  }
  entropyBufferIndex_ += 2U;
  expectedRestartMarker_ = static_cast<uint8_t>((expectedRestartMarker_ + 1U) & 7U);
  mcusSinceRestart_ = 0;
  restartPending_ = false;
  for (uint8_t index = 0; index < componentCount_; ++index) {
    components_[index].predictor = 0;
  }
  return PdfStatus::success();
}

PdfStepResult PdfJpegPreview::writeBmpHeader(PdfWorkBudget& budget) {
  if (budget.operationsRemaining == 0 || budget.bytesRemaining < kBmpHeaderBytes || budget.stopRequested()) {
    return PdfStepResult::paused();
  }
  uint8_t header[kBmpHeaderBytes]{};
  const uint32_t pixelBytes = static_cast<uint32_t>(outputRowBytes_) * targetHeight_;
  header[0] = 'B';
  header[1] = 'M';
  writeLe32(header + 2, static_cast<uint32_t>(kBmpHeaderBytes) + pixelBytes);
  writeLe32(header + 10, kBmpHeaderBytes);
  writeLe32(header + 14, 40);
  writeLe32(header + 18, targetWidth_);
  writeLe32(header + 22, static_cast<uint32_t>(-static_cast<int32_t>(targetHeight_)));
  writeLe16(header + 26, 1);
  writeLe16(header + 28, 1);
  writeLe32(header + 34, pixelBytes);
  writeLe32(header + 38, 2835);
  writeLe32(header + 42, 2835);
  writeLe32(header + 46, 2);
  header[58] = 0xff;
  header[59] = 0xff;
  header[60] = 0xff;
  (void)budget.consumeOperation();
  (void)budget.takeBytes(sizeof(header));
  const PdfStatus status = pdfWriteTrackedCacheFile(writer_, header, sizeof(header));
  return status ? PdfStepResult::completed() : PdfStepResult::failure(status);
}

PdfStepResult PdfJpegPreview::writeWhiteRow(PdfWorkBudget& budget) {
  if (budget.operationsRemaining == 0 || budget.bytesRemaining < outputRowBytes_ || budget.stopRequested()) {
    return PdfStepResult::paused();
  }
  uint8_t* const row = workspace_ + blockRowBytes_;
  std::memset(row, 0xff, outputRowBytes_);
  (void)budget.consumeOperation();
  (void)budget.takeBytes(outputRowBytes_);
  const PdfStatus status = pdfWriteTrackedCacheFile(writer_, row, outputRowBytes_);
  if (!status) {
    return PdfStepResult::failure(status);
  }
  ++outputRow_;
  return PdfStepResult::completed();
}

bool PdfJpegPreview::currentDecodedRowCanServeOutput() const {
  if (!decodedRowReady_ || outputRow_ < offsetY_ ||
      outputRow_ >= static_cast<uint16_t>(offsetY_ + scaledHeight_)) {
    return false;
  }
  const uint16_t sourceY = static_cast<uint16_t>(
      static_cast<uint32_t>(outputRow_ - offsetY_) * height_ / scaledHeight_);
  return sourceY / (8U * maximumVerticalSampling_) == readyMcuRow_;
}

PdfStepResult PdfJpegPreview::writeImageRow(PdfWorkBudget& budget) {
  if (!currentDecodedRowCanServeOutput()) {
    return PdfStepResult::failure(PdfStatus::failure(PdfError::InvalidArgument, outputRow_));
  }
  if (budget.operationsRemaining == 0 || budget.bytesRemaining < outputRowBytes_ || budget.stopRequested()) {
    return PdfStepResult::paused();
  }
  uint8_t* const row = workspace_ + blockRowBytes_;
  std::memset(row, 0xff, outputRowBytes_);
  const uint16_t sourceY = static_cast<uint16_t>(
      static_cast<uint32_t>(outputRow_ - offsetY_) * height_ / scaledHeight_);
  const uint8_t localBlockY = static_cast<uint8_t>((sourceY / 8U) % maximumVerticalSampling_);
  const uint16_t imageEndX = static_cast<uint16_t>(offsetX_ + scaledWidth_);
  for (uint16_t x = offsetX_; x < imageEndX; ++x) {
    const uint16_t sourceX =
        static_cast<uint16_t>(static_cast<uint32_t>(x - offsetX_) * width_ / scaledWidth_);
    const uint16_t blockX = static_cast<uint16_t>(std::min<uint32_t>(blockColumns_ - 1U, sourceX / 8U));
    const uint8_t luminance = workspace_[static_cast<size_t>(localBlockY) * blockColumns_ + blockX];
    if (luminance < 128U) {
      row[x / 8U] &= static_cast<uint8_t>(~(1U << (7U - x % 8U)));
    }
  }
  (void)budget.consumeOperation();
  (void)budget.takeBytes(outputRowBytes_);
  const PdfStatus status = pdfWriteTrackedCacheFile(writer_, row, outputRowBytes_);
  if (!status) {
    return PdfStepResult::failure(status);
  }
  ++outputRow_;
  return PdfStepResult::completed();
}

PdfStepResult PdfJpegPreview::stepAsset(PdfWorkBudget& budget) {
  if (!headerReady() || writer_ == nullptr || !writer_->open || assetStage_ == AssetStage::Idle) {
    return PdfStepResult::failure(PdfStatus::failure(PdfError::InvalidArgument));
  }
  if (assetStage_ == AssetStage::Complete) {
    return PdfStepResult::completed();
  }
  if (assetStage_ == AssetStage::Header) {
    const PdfStepResult written = writeBmpHeader(budget);
    if (!written.complete()) {
      return written;
    }
    assetStage_ = outputRow_ < offsetY_ ? AssetStage::TopRows : AssetStage::Decode;
    return PdfStepResult::paused();
  }
  if (assetStage_ == AssetStage::TopRows) {
    const PdfStepResult written = writeWhiteRow(budget);
    if (!written.complete()) {
      return written;
    }
    if (outputRow_ >= offsetY_) {
      assetStage_ = AssetStage::Decode;
    }
    return PdfStepResult::paused();
  }
  if (assetStage_ == AssetStage::Decode) {
    if (currentMcuY_ >= mcuRows_) {
      assetStage_ = AssetStage::BottomRows;
      return PdfStepResult::paused();
    }
    const size_t buffered = entropyBufferLength_ - entropyBufferIndex_;
    const bool unreadBytesBeyondBuffer = entropyBufferOffset_ + entropyBufferLength_ < sourceBytes_;
    if (buffered < kEntropyHeadroom && (entropyBufferLength_ == 0 || unreadBytesBeyondBuffer)) {
      const PdfStepResult refill = refillEntropy(budget);
      return refill.complete() ? PdfStepResult::paused() : refill;
    }
    if (restartPending_) {
      if (!budget.consumeOperation()) {
        return PdfStepResult::paused();
      }
      const PdfStatus status = consumeRestartMarker();
      return status ? PdfStepResult::paused() : PdfStepResult::failure(status);
    }
    if (!budget.consumeOperation()) {
      return PdfStepResult::paused();
    }
    const uint8_t componentIndex = scanComponents_[currentScanComponent_];
    Component& component = components_[componentIndex];
    const PdfStatus status =
        decodeBlock(component, componentIndex == luminanceComponent_, currentComponentBlock_);
    if (!status) {
      return PdfStepResult::failure(status);
    }
    advanceDecodedBlock();
    if (decodedRowReady_) {
      assetStage_ = AssetStage::ImageRows;
    }
    return PdfStepResult::paused();
  }
  if (assetStage_ == AssetStage::ImageRows) {
    const uint16_t imageEndY = static_cast<uint16_t>(offsetY_ + scaledHeight_);
    if (outputRow_ >= imageEndY) {
      decodedRowReady_ = false;
      assetStage_ = AssetStage::BottomRows;
      return PdfStepResult::paused();
    }
    if (currentDecodedRowCanServeOutput()) {
      const PdfStepResult written = writeImageRow(budget);
      return written.complete() ? PdfStepResult::paused() : written;
    }
    decodedRowReady_ = false;
    std::memset(workspace_, 0xff, blockRowBytes_);
    assetStage_ = currentMcuY_ < mcuRows_ ? AssetStage::Decode : AssetStage::BottomRows;
    return PdfStepResult::paused();
  }
  if (assetStage_ == AssetStage::BottomRows) {
    if (outputRow_ < targetHeight_) {
      const PdfStepResult written = writeWhiteRow(budget);
      return written.complete() ? PdfStepResult::paused() : written;
    }
    assetStage_ = AssetStage::Complete;
    return PdfStepResult::completed();
  }
  return PdfStepResult::failure(PdfStatus::failure(PdfError::InvalidArgument));
}
