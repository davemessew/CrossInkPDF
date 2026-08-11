#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

constexpr uint32_t PDF_MIN_FREE_HEAP_BYTES = 64U * 1024U;
constexpr uint32_t PDF_MIN_LARGEST_BLOCK_BYTES = 48U * 1024U;
constexpr uint32_t PDF_MAX_OWNED_HEAP_BYTES = 96U * 1024U;
constexpr uint32_t PDF_MIN_STACK_MARGIN_BYTES = 1024U;
constexpr size_t PDF_RESOURCE_SLOT_COUNT = 6;

enum class PdfResourceKind : uint8_t {
  InflateDictionary,
  SourceWindow,
  DecoderOutput,
  PageText,
  RunRecords,
  OperandScratch,
};

enum class PdfResourceEventKind : uint8_t {
  StartAccepted,
  StartRejected,
  Acquired,
  Released,
  AccountingRejected,
  RuntimeRejected,
};

struct PdfResourceSnapshot {
  uint32_t freeHeap = 0;
  uint32_t largestBlock = 0;
  uint32_t stackMargin = 0;
};

struct PdfResourceEvent {
  PdfResourceEventKind event = PdfResourceEventKind::AccountingRejected;
  PdfResourceKind resource = PdfResourceKind::InflateDictionary;
  size_t bytes = 0;
  size_t currentBytes = 0;
  size_t peakBytes = 0;
  PdfResourceSnapshot snapshot{};
};

using PdfResourceMeasureFn = PdfResourceSnapshot (*)(void* context);
using PdfResourceEventFn = void (*)(void* context, const PdfResourceEvent& event);

struct PdfResourceHooks {
  void* context = nullptr;
  PdfResourceMeasureFn measure = nullptr;
  PdfResourceEventFn event = nullptr;
};

class PdfResourceTracker {
 public:
  explicit PdfResourceTracker(PdfResourceHooks hooks);

  bool canStart();
  bool acquire(PdfResourceKind kind, size_t bytes);
  bool release(PdfResourceKind kind);
  bool runtimeWithinLimits();

  size_t currentBytes() const { return currentBytes_; }
  size_t peakBytes() const { return peakBytes_; }
  size_t liveCount() const { return liveCount_; }

 private:
  struct LiveResource {
    PdfResourceKind kind = PdfResourceKind::InflateDictionary;
    size_t bytes = 0;
  };

  PdfResourceSnapshot measure() const;
  void emit(PdfResourceEventKind event, PdfResourceKind resource, size_t bytes,
            const PdfResourceSnapshot& snapshot = {});

  PdfResourceHooks hooks_{};
  std::array<LiveResource, PDF_RESOURCE_SLOT_COUNT> live_{};
  size_t liveCount_ = 0;
  size_t currentBytes_ = 0;
  size_t peakBytes_ = 0;
};
