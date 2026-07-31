#pragma once

#include <cstddef>
#include <cstdint>

#include "PdfCacheStore.h"
#include "PdfLimits.h"
#include "PdfWorkBudget.h"

class PdfJpegPreview {
 public:
  PdfStatus beginHeader(const PdfCacheIo& io, PdfCacheHandle source, uint64_t sourceBytes, uint8_t* ioBuffer,
                        size_t ioBufferBytes, uint8_t* workspace, size_t workspaceBytes);
  PdfStepResult stepHeader(PdfWorkBudget& budget);

  PdfStatus beginAsset(PdfCacheTrackedWriter* writer, uint16_t targetWidth, uint16_t targetHeight);
  PdfStepResult stepAsset(PdfWorkBudget& budget);

  uint16_t width() const { return width_; }
  uint16_t height() const { return height_; }
  bool headerReady() const;
  void reset();

 private:
  struct HuffmanTable {
    uint8_t counts[16]{};
    uint8_t symbols[256]{};
    uint16_t symbolCount = 0;
    bool valid = false;
  };

  struct Component {
    int32_t predictor = 0;
    uint8_t id = 0;
    uint8_t horizontalSampling = 0;
    uint8_t verticalSampling = 0;
    uint8_t quantizationTable = 0;
    uint8_t dcTable = 0;
    uint8_t acTable = 0;
  };

  enum class HeaderStage : uint8_t {
    Idle,
    Soi,
    Marker,
    Payload,
    Ready,
  };

  enum class AssetStage : uint8_t {
    Idle,
    Header,
    TopRows,
    Decode,
    ImageRows,
    BottomRows,
    Complete,
  };

  PdfStepResult readAt(uint64_t offset, size_t requested, PdfWorkBudget& budget);
  PdfStatus parseMarkerPayload();
  PdfStatus parseQuantizationTables(const uint8_t* payload, size_t length);
  PdfStatus parseFrame(const uint8_t* payload, size_t length);
  PdfStatus parseHuffmanTables(const uint8_t* payload, size_t length);
  PdfStatus parseRestartInterval(const uint8_t* payload, size_t length);
  PdfStatus parseScan(const uint8_t* payload, size_t length);

  PdfStepResult refillEntropy(PdfWorkBudget& budget);
  bool entropyByte(uint8_t* value);
  bool entropyBits(uint8_t count, uint16_t* value);
  bool huffmanValue(const HuffmanTable& table, uint8_t* value);
  PdfStatus decodeBlock(Component& component, bool luminance, uint8_t blockIndex);
  PdfStatus consumeRestartMarker();
  void advanceDecodedBlock();

  PdfStepResult writeBmpHeader(PdfWorkBudget& budget);
  PdfStepResult writeWhiteRow(PdfWorkBudget& budget);
  PdfStepResult writeImageRow(PdfWorkBudget& budget);
  bool currentDecodedRowCanServeOutput() const;
  uint64_t currentEntropyOffset() const;

  PdfCacheIo io_{};
  PdfCacheHandle source_{};
  PdfCacheTrackedWriter* writer_ = nullptr;
  uint8_t* ioBuffer_ = nullptr;
  uint8_t* workspace_ = nullptr;
  size_t ioBufferBytes_ = 0;
  size_t workspaceBytes_ = 0;
  uint64_t sourceBytes_ = 0;
  uint64_t headerOffset_ = 0;
  uint64_t segmentEnd_ = 0;
  uint64_t entropyStart_ = 0;
  uint64_t entropyBufferOffset_ = 0;
  size_t entropyBufferLength_ = 0;
  size_t entropyBufferIndex_ = 0;
  uint32_t bitBuffer_ = 0;
  uint16_t restartInterval_ = 0;
  uint16_t mcusSinceRestart_ = 0;
  uint16_t width_ = 0;
  uint16_t height_ = 0;
  uint16_t blockColumns_ = 0;
  uint16_t mcuColumns_ = 0;
  uint16_t mcuRows_ = 0;
  uint16_t currentMcuX_ = 0;
  uint16_t currentMcuY_ = 0;
  uint16_t readyMcuRow_ = 0;
  uint16_t targetWidth_ = 0;
  uint16_t targetHeight_ = 0;
  uint16_t scaledWidth_ = 0;
  uint16_t scaledHeight_ = 0;
  uint16_t offsetX_ = 0;
  uint16_t offsetY_ = 0;
  uint16_t outputRow_ = 0;
  uint16_t outputRowBytes_ = 0;
  uint16_t blockRowBytes_ = 0;
  size_t payloadLength_ = 0;
  uint8_t marker_ = 0;
  uint8_t componentCount_ = 0;
  uint8_t scanComponentCount_ = 0;
  uint8_t luminanceComponent_ = 0;
  uint8_t maximumHorizontalSampling_ = 0;
  uint8_t maximumVerticalSampling_ = 0;
  uint8_t scanComponents_[4]{};
  uint8_t currentScanComponent_ = 0;
  uint8_t currentComponentBlock_ = 0;
  uint8_t bitCount_ = 0;
  uint8_t expectedRestartMarker_ = 0;
  uint16_t quantizationDc_[4]{};
  bool quantizationValid_[4]{};
  HuffmanTable dcTables_[4]{};
  HuffmanTable acTables_[4]{};
  Component components_[4]{};
  HeaderStage headerStage_ = HeaderStage::Idle;
  AssetStage assetStage_ = AssetStage::Idle;
  bool frameSeen_ = false;
  bool decodedRowReady_ = false;
  bool restartPending_ = false;
};

static_assert(sizeof(PdfJpegPreview) <= 3072, "JPEG preview state must stay fixed and far below an image buffer");
