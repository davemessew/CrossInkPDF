#pragma once

#include <cstddef>
#include <cstdint>

#include "PdfTypes.h"

struct PdfRunStoreWorkspace {
  PdfTextRun* memoryRuns = nullptr;
  uint16_t memoryRunCapacity = 0;
  uint8_t* memoryText = nullptr;
  size_t memoryTextCapacity = 0;
  PdfFixedRecordStore spillRuns{};
  PdfByteStore spillText{};
};

struct PdfRunStoreLifecycle {
  using TransitionFn = PdfStatus (*)(void* context, uint64_t continuationOffset);

  void* context = nullptr;
  TransitionFn closeSourceForSpillRead = nullptr;
  TransitionFn closeSpillAndReopenSource = nullptr;

  constexpr bool valid() const {
    return (closeSourceForSpillRead == nullptr) == (closeSpillAndReopenSource == nullptr);
  }
};

class PdfRunStore {
 public:
  PdfRunStore(PdfRunStoreWorkspace workspace, PdfRunStoreLifecycle lifecycle = {})
      : workspace_(workspace), lifecycle_(lifecycle) {}

  PdfStatus reset();
  PdfStatus append(const PdfTextRun& run, const uint8_t* text, size_t textLength);

  PdfStatus beginReduction(uint64_t continuationOffset);
  PdfStatus endReduction();
  PdfStatus readRun(uint32_t ordinal, PdfTextRun* run) const;
  PdfStatus readText(uint32_t ordinal, uint32_t offset, uint8_t* destination, size_t requested,
                     size_t* bytesRead) const;
  PdfStatus readTextExact(uint32_t ordinal, uint32_t offset, uint8_t* destination, size_t length) const;

  uint32_t count() const { return runCount_; }
  uint64_t textLength() const { return totalTextLength_; }
  bool spilled() const { return metadataSpillCount_ != 0 || textSpilling_; }
  bool reductionActive() const { return reductionActive_; }

 private:
  static constexpr uint16_t TextInSpillFlag = 1U << 15;

  PdfStatus readStoredRun(uint32_t ordinal, PdfTextRun* run) const;

  PdfRunStoreWorkspace workspace_{};
  PdfRunStoreLifecycle lifecycle_{};
  uint32_t runCount_ = 0;
  uint32_t metadataSpillCount_ = 0;
  size_t memoryTextLength_ = 0;
  uint64_t totalTextLength_ = 0;
  bool textSpilling_ = false;
  bool reductionActive_ = false;
  bool initialized_ = false;
  uint64_t continuationOffset_ = 0;
};
