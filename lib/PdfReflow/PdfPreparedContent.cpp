#include "PdfPreparedContent.h"

#include <algorithm>
#include <cstring>

#include "PdfCheckedMath.h"

PdfPreparedContentStreams::PdfPreparedContentStreams(const PdfStreamDecoderWorkspace workspace) : decoder_(workspace) {}

PdfStatus PdfPreparedContentStreams::begin(const PdfEncodedContentStream* const streams, const uint8_t count,
                                           const PdfByteStore decodedStore, const PdfStreamDecodeLimits limits) {
  streams_ = nullptr;
  streamCount_ = 0;
  decodedStore_ = {};
  limits_ = {};
  failure_ = PdfStatus::success();
  decodedBytes_ = 0;
  streamIndex_ = 0;
  finalizeIndex_ = 0;
  storeReady_ = false;
  phase_ = Phase::Idle;
  for (uint8_t index = 0; index < MaxSources; ++index) {
    ranges_[index] = {};
    sources_[index] = {};
    offsets_[index] = 0;
    lengths_[index] = 0;
  }

  if (streams == nullptr || count == 0 || !decodedStore.valid() || decodedStore.capacity == 0 ||
      limits.maxExpandedBytes == 0 || limits.maxExpansionRatio == 0) {
    return PdfStatus::failure(PdfError::InvalidArgument);
  }
  if (count > MaxSources) {
    return PdfStatus::failure(PdfError::LimitExceeded, count);
  }
  for (uint8_t index = 0; index < count; ++index) {
    if (!streams[index].source.valid()) {
      return PdfStatus::failure(PdfError::InvalidArgument, index);
    }
    if (streams[index].filterCount > PdfLimits::MaxFiltersPerStream) {
      return PdfStatus::failure(PdfError::LimitExceeded, index);
    }
  }

  streams_ = streams;
  streamCount_ = count;
  decodedStore_ = decodedStore;
  limits_ = limits;
  limits_.maxExpandedBytes = std::min<uint64_t>(
      limits_.maxExpandedBytes, PdfLimits::MaxExpandedRequiredStreamBytes);
  limits_.maxExpansionRatio = std::min<uint16_t>(
      limits_.maxExpansionRatio, PdfLimits::MaxExpansionRatio);
  phase_ = Phase::ResetStore;
  return PdfStatus::success();
}

PdfStepResult PdfPreparedContentStreams::step(PdfWorkBudget& budget) {
  if (phase_ == Phase::Done) {
    return PdfStepResult::completed();
  }
  if (phase_ == Phase::Failed) {
    return PdfStepResult::failure(failure_);
  }
  if (phase_ == Phase::Idle) {
    return PdfStepResult::failure(PdfStatus::failure(PdfError::InvalidArgument));
  }

  if (phase_ == Phase::CleanupStore) {
    if (budget.operationsRemaining == 0) {
      return PdfStepResult::paused();
    }
    // Cancellation is already latched in failure_. Cleanup must still make
    // bounded forward progress when the caller keeps the stop signal asserted.
    --budget.operationsRemaining;
    const PdfStatus cleanup = decodedStore_.reset(decodedStore_.context);
    decodedBytes_ = 0;
    storeReady_ = false;
    phase_ = Phase::Failed;
    if (!cleanup) {
      failure_ = cleanup;
    }
    return PdfStepResult::failure(failure_);
  }

  if (budget.cancelRequested()) {
    return fail(PdfStatus::failure(PdfError::Cancelled, streamIndex_));
  }
  if (budget.stopRequested()) {
    return PdfStepResult::paused();
  }

  if (phase_ == Phase::ResetStore) {
    if (!budget.consumeOperation()) {
      return PdfStepResult::paused();
    }
    const PdfStatus status = decodedStore_.reset(decodedStore_.context);
    if (!status) {
      failure_ = status;
      phase_ = Phase::Failed;
      return PdfStepResult::failure(status);
    }
    storeReady_ = true;
    phase_ = Phase::BeginStream;
    return PdfStepResult::paused();
  }

  if (phase_ == Phase::BeginStream) {
    if (streamIndex_ >= streamCount_) {
      finalizeIndex_ = 0;
      phase_ = Phase::FinalizeSources;
      return PdfStepResult::paused();
    }
    if (!budget.consumeOperation()) {
      return PdfStepResult::paused();
    }
    const uint64_t storedBytes = decodedStore_.size(decodedStore_.context);
    if (storedBytes != decodedBytes_ || storedBytes > decodedStore_.capacity) {
      return fail(PdfStatus::failure(PdfError::IoFailure, storedBytes));
    }
    if (decodedBytes_ >= limits_.maxExpandedBytes) {
      return fail(PdfStatus::failure(PdfError::ExpansionLimit, decodedBytes_));
    }
    if (decodedBytes_ >= decodedStore_.capacity) {
      return fail(PdfStatus::failure(PdfError::InsufficientStorage, decodedBytes_));
    }
    offsets_[streamIndex_] = decodedBytes_;
    PdfStreamDecodeLimits streamLimits = limits_;
    streamLimits.maxExpandedBytes =
        std::min(limits_.maxExpandedBytes - decodedBytes_, decodedStore_.capacity - decodedBytes_);
    const PdfStatus status = decoder_.begin(streams_[streamIndex_].source, pdfByteStoreSink(decodedStore_),
                                            streams_[streamIndex_].filters, streams_[streamIndex_].filterCount,
                                            streamLimits, true);
    if (!status) {
      return fail(status);
    }
    phase_ = Phase::DecodeStream;
    return PdfStepResult::paused();
  }

  if (phase_ == Phase::DecodeStream) {
    const PdfStepResult result = decoder_.step(budget);
    if (result.failed()) {
      return fail(result.status);
    }
    if (result.complete()) {
      phase_ = Phase::FinishStream;
    }
    return PdfStepResult::paused();
  }

  if (phase_ == Phase::FinishStream) {
    if (!budget.consumeOperation()) {
      return PdfStepResult::paused();
    }
    uint64_t expectedEnd = 0;
    if (!pdfCheckedAdd(offsets_[streamIndex_], decoder_.outputBytes(), &expectedEnd)) {
      return fail(PdfStatus::failure(PdfError::LimitExceeded, offsets_[streamIndex_]));
    }
    const uint64_t storedBytes = decodedStore_.size(decodedStore_.context);
    if (storedBytes != expectedEnd || storedBytes > decodedStore_.capacity) {
      return fail(PdfStatus::failure(PdfError::IoFailure, storedBytes));
    }
    lengths_[streamIndex_] = decoder_.outputBytes();
    decodedBytes_ = storedBytes;
    ++streamIndex_;
    phase_ = Phase::BeginStream;
    return PdfStepResult::paused();
  }

  if (phase_ == Phase::FinalizeSources) {
    if (finalizeIndex_ >= streamCount_) {
      phase_ = Phase::Done;
      return PdfStepResult::completed();
    }
    if (!budget.consumeOperation()) {
      return PdfStepResult::paused();
    }
    const PdfByteSource decodedSource = pdfByteStoreSource(decodedStore_);
    if (!decodedSource.valid() || decodedSource.size != decodedBytes_) {
      return fail(PdfStatus::failure(PdfError::IoFailure, decodedSource.size));
    }
    const PdfStatus status = pdfInitializeByteRange(decodedSource, offsets_[finalizeIndex_], lengths_[finalizeIndex_],
                                                    &ranges_[finalizeIndex_]);
    if (!status) {
      return fail(status);
    }
    sources_[finalizeIndex_] = pdfByteRangeSource(ranges_[finalizeIndex_]);
    ++finalizeIndex_;
    return PdfStepResult::paused();
  }

  return fail(PdfStatus::failure(PdfError::InvalidArgument));
}

PdfStepResult PdfPreparedContentStreams::fail(const PdfStatus status) {
  failure_ = status.ok() ? PdfStatus::failure(PdfError::Malformed) : status;
  if (storeReady_) {
    phase_ = Phase::CleanupStore;
    return PdfStepResult::paused();
  }
  phase_ = Phase::Failed;
  return PdfStepResult::failure(failure_);
}

PdfStatus PdfPreparedContentResources::reset() {
  if (fontCapacity_ > MaxFonts || xobjectCapacity_ > MaxXObjects) {
    ready_ = false;
    return PdfStatus::failure(PdfError::LimitExceeded);
  }
  if ((fontCapacity_ != 0 && fonts_ == nullptr) || (xobjectCapacity_ != 0 && xobjects_ == nullptr)) {
    ready_ = false;
    return PdfStatus::failure(PdfError::InvalidArgument);
  }
  fontCount_ = 0;
  xobjectCount_ = 0;
  ready_ = true;
  return PdfStatus::success();
}

PdfStatus PdfPreparedContentResources::addFont(const uint8_t* const name, const size_t length, PdfFontMap* const font) {
  if (!ready_ || name == nullptr || length == 0 || font == nullptr) {
    return PdfStatus::failure(PdfError::InvalidArgument);
  }
  if (length > MaxNameBytes || fontCount_ >= fontCapacity_) {
    return PdfStatus::failure(PdfError::LimitExceeded, length);
  }
  if (!font->fullyResident()) {
    // Interpretation reads the decoded page stream from the sole SD reader.
    // Font lookup therefore must be RAM-only after resource preparation.
    return PdfStatus::failure(PdfError::LimitExceeded, font->fontId());
  }
  uint32_t materializedGlyphCount = 0;
  bool fontAlreadyRegistered = false;
  for (uint8_t index = 0; index < fontCount_; ++index) {
    const PdfPreparedFontResource& existing = fonts_[index];
    if (existing.nameLength == length && std::memcmp(existing.name, name, length) == 0) {
      return PdfStatus::failure(PdfError::Malformed, index);
    }
    fontAlreadyRegistered = fontAlreadyRegistered || existing.font == font;
    bool firstRegistrationOfFont = true;
    for (uint8_t previous = 0; previous < index; ++previous) {
      if (fonts_[previous].font == existing.font) {
        firstRegistrationOfFont = false;
        break;
      }
    }
    if (firstRegistrationOfFont) {
      materializedGlyphCount += existing.font->materializedGlyphCount();
    }
  }
  if (!fontAlreadyRegistered) {
    materializedGlyphCount += font->materializedGlyphCount();
  }
  if (materializedGlyphCount > PdfLimits::MaxPageUniqueGlyphs) {
    return PdfStatus::failure(PdfError::LimitExceeded, materializedGlyphCount);
  }
  PdfPreparedFontResource& resource = fonts_[fontCount_++];
  std::memcpy(resource.name, name, length);
  resource.nameLength = static_cast<uint8_t>(length);
  resource.font = font;
  return PdfStatus::success();
}

PdfStatus PdfPreparedContentResources::addXObject(const uint8_t* const name, const size_t length,
                                                  const PdfContentXObject& object) {
  if (!ready_ || name == nullptr || length == 0) {
    return PdfStatus::failure(PdfError::InvalidArgument);
  }
  if (length > MaxNameBytes || xobjectCount_ >= xobjectCapacity_) {
    return PdfStatus::failure(PdfError::LimitExceeded, length);
  }
  if (object.kind == PdfContentXObjectKind::Form && !object.content.valid()) {
    return PdfStatus::failure(PdfError::InvalidArgument);
  }
  for (uint8_t index = 0; index < xobjectCount_; ++index) {
    if (xobjects_[index].nameLength == length && std::memcmp(xobjects_[index].name, name, length) == 0) {
      return PdfStatus::failure(PdfError::Malformed, index);
    }
  }
  PdfPreparedXObjectResource& resource = xobjects_[xobjectCount_++];
  std::memcpy(resource.name, name, length);
  resource.nameLength = static_cast<uint8_t>(length);
  resource.object = object;
  return PdfStatus::success();
}

const PdfContentResources& PdfPreparedContentResources::descriptor() const { return descriptor_; }

PdfStatus PdfPreparedContentResources::resolveFont(void* const context, const uint8_t* const name, const size_t length,
                                                   PdfFontMap** const font) {
  if (context == nullptr || name == nullptr || length == 0 || font == nullptr) {
    return PdfStatus::failure(PdfError::InvalidArgument);
  }
  auto& resources = *static_cast<PdfPreparedContentResources*>(context);
  if (!resources.ready_) {
    return PdfStatus::failure(PdfError::InvalidArgument);
  }
  for (uint8_t index = 0; index < resources.fontCount_; ++index) {
    const PdfPreparedFontResource& resource = resources.fonts_[index];
    if (resource.nameLength == length && std::memcmp(resource.name, name, length) == 0) {
      *font = resource.font;
      return PdfStatus::success();
    }
  }
  *font = nullptr;
  if (resources.parent_ != nullptr && resources.parent_->resolveFont != nullptr) {
    return resources.parent_->resolveFont(resources.parent_->context, name, length, font);
  }
  return PdfStatus::failure(PdfError::UnsupportedEncoding);
}

PdfStatus PdfPreparedContentResources::resolveXObject(void* const context, const uint8_t* const name,
                                                       const size_t length, PdfContentXObject* const object) {
  if (context == nullptr || name == nullptr || length == 0 || object == nullptr) {
    return PdfStatus::failure(PdfError::InvalidArgument);
  }
  auto& resources = *static_cast<PdfPreparedContentResources*>(context);
  if (!resources.ready_) {
    return PdfStatus::failure(PdfError::InvalidArgument);
  }
  for (uint8_t index = 0; index < resources.xobjectCount_; ++index) {
    const PdfPreparedXObjectResource& resource = resources.xobjects_[index];
    if (resource.nameLength == length && std::memcmp(resource.name, name, length) == 0) {
      *object = resource.object;
      return PdfStatus::success();
    }
  }
  if (resources.parent_ != nullptr && resources.parent_->resolveXObject != nullptr) {
    return resources.parent_->resolveXObject(resources.parent_->context, name, length, object);
  }
  return PdfStatus::failure(PdfError::Unsupported);
}

PdfStatus PdfPreparedContentResources::forwardInlineImageToken(void* const context, const PdfToken& token) {
  if (context == nullptr) {
    return PdfStatus::failure(PdfError::InvalidArgument);
  }
  auto& resources = *static_cast<PdfPreparedContentResources*>(context);
  const PdfPreparedContentInlineImageHooks& hooks = resources.inlineImageHooks_;
  if (!resources.ready_ || hooks.consumeInlineImageToken == nullptr) {
    return PdfStatus::failure(PdfError::InvalidArgument);
  }
  return hooks.consumeInlineImageToken(hooks.context, token);
}

PdfStepResult PdfPreparedContentResources::forwardInlineImage(void* const context, const PdfByteSource& source,
                                                              const uint64_t idEndOffset, PdfWorkBudget& budget,
                                                              uint64_t* const resumeOffset,
                                                              PdfContentXObject* const image) {
  if (context == nullptr || resumeOffset == nullptr || image == nullptr) {
    return PdfStepResult::failure(PdfStatus::failure(PdfError::InvalidArgument, idEndOffset));
  }
  auto& resources = *static_cast<PdfPreparedContentResources*>(context);
  const PdfPreparedContentInlineImageHooks& hooks = resources.inlineImageHooks_;
  if (!resources.ready_ || hooks.finishInlineImage == nullptr) {
    return PdfStepResult::failure(PdfStatus::failure(PdfError::InvalidArgument, idEndOffset));
  }
  return hooks.finishInlineImage(hooks.context, source, idEndOffset, budget, resumeOffset, image);
}
