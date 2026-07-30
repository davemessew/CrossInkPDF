#pragma once

#include <InflateReader.h>

#include <cstddef>
#include <cstdint>

#include "PdfLimits.h"
#include "PdfTypes.h"
#include "PdfWorkBudget.h"

struct PdfObjectArena;

enum class PdfStreamFilter : uint8_t {
  ASCIIHex,
  ASCII85,
  Flate,
  Lzw,
  Unsupported,
};

struct PdfStreamDecodeLimits {
  uint64_t maxExpandedBytes = PdfLimits::MaxExpandedRequiredStreamBytes;
  uint16_t maxExpansionRatio = PdfLimits::MaxExpansionRatio;
};

struct PdfStreamDecoderWorkspace {
  uint8_t* sourceBuffer = nullptr;
  size_t sourceBufferSize = 0;
  uint8_t* outputBuffer = nullptr;
  size_t outputBufferSize = 0;
  uint8_t* inflateDictionary = nullptr;
  size_t inflateDictionarySize = 0;
};

class PdfStreamDecoder {
 public:
  explicit PdfStreamDecoder(PdfStreamDecoderWorkspace workspace);

  PdfStatus begin(const PdfByteSource& source, const PdfByteSink& sink, const PdfStreamFilter* filters,
                  uint8_t filterCount, PdfStreamDecodeLimits limits = {}, bool required = true);
  PdfStepResult step(PdfWorkBudget& budget);

  uint64_t inputBytes() const { return inputBytes_; }
  uint64_t outputBytes() const { return outputBytes_; }
  bool omitted() const { return omitted_; }
  bool usesExternalDictionary() const { return inflateInitialized_ && inflateContext_.reader.usesExternalDictionary(); }

 private:
  enum class PullState : uint8_t {
    Byte,
    End,
    Yielded,
    Failed,
  };

  struct PullResult {
    PullState state = PullState::End;
    PdfStatus status{};
  };

  struct FilterState {
    uint8_t output[4]{};
    uint8_t outputPosition = 0;
    uint8_t outputLength = 0;
    uint8_t group[5]{};
    uint8_t groupLength = 0;
    uint8_t highNibble = 0;
    bool hasHighNibble = false;
    bool terminated = false;
    bool pendingTerminator = false;
  };

  struct InflateContext {
    InflateReader reader;
    PdfStreamDecoder* owner = nullptr;
  };

  enum class Phase : uint8_t {
    Idle,
    ZlibHeader,
    Decode,
    Flush,
    Done,
    Failed,
  };

  static int inflateReadCallback(uzlib_uncomp* uncomp);
  int refillInflateInput(uzlib_uncomp* uncomp);
  PullResult pull(uint8_t stage, uint8_t* byte);
  PullResult pullRaw(uint8_t* byte);
  PullResult pullAsciiHex(uint8_t stage, uint8_t* byte);
  PullResult pullAscii85(uint8_t stage, uint8_t* byte);
  PullResult flushPending(PdfWorkBudget& budget);
  PdfStatus validateGrowth(uint64_t additionalBytes) const;
  PdfStepResult fail(PdfStatus status);

  PdfStreamDecoderWorkspace workspace_{};
  PdfByteSource source_{};
  PdfByteSink sink_{};
  PdfStreamFilter filters_[PdfLimits::MaxFiltersPerStream]{};
  FilterState filterStates_[PdfLimits::MaxFiltersPerStream]{};
  PdfStreamDecodeLimits limits_{};
  InflateContext inflateContext_{};
  PdfWorkBudget* activeBudget_ = nullptr;
  PdfStatus callbackStatus_{};
  Phase phase_ = Phase::Idle;
  size_t sourceBufferPosition_ = 0;
  size_t sourceBufferLength_ = 0;
  size_t pendingOutputLength_ = 0;
  size_t pendingOutputWritten_ = 0;
  size_t inflateInputLength_ = 0;
  size_t inflateInputCapacity_ = 0;
  size_t finalOutputOffset_ = 0;
  size_t finalOutputCapacity_ = 0;
  uint64_t sourceOffset_ = 0;
  uint64_t inputBytes_ = 0;
  uint64_t outputBytes_ = 0;
  uint8_t filterCount_ = 0;
  uint8_t preFlateStages_ = 0;
  uint8_t zlibHeader_[2]{};
  uint8_t zlibHeaderLength_ = 0;
  bool hasFlate_ = false;
  bool inflateInitialized_ = false;
  bool inflateInputAtEnd_ = false;
  bool finishAfterFlush_ = false;
  bool omitted_ = false;
};

PdfStatus pdfStreamFiltersFromDictionary(const PdfObjectArena& arena, uint16_t dictionaryIndex,
                                         PdfStreamFilter* filters, uint8_t filterCapacity, uint8_t* filterCount);
