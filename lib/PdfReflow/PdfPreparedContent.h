#pragma once

#include <cstddef>
#include <cstdint>

#include "PdfContentInterpreter.h"
#include "PdfIo.h"
#include "PdfSecurity.h"
#include "PdfStreamDecoder.h"

struct PdfEncodedContentStream {
  PdfByteSource source{};
  PdfObjectReference reference{};
  PdfSecurity* security = nullptr;
  PdfStreamFilter filters[PdfLimits::MaxFiltersPerStream]{};
  uint8_t filterCount = 0;
};

class PdfPreparedContentStreams {
 public:
  static constexpr uint8_t MaxSources = PdfLimits::MaxContentStreamsPerPage;

  explicit PdfPreparedContentStreams(PdfStreamDecoderWorkspace workspace);

  PdfStatus begin(const PdfEncodedContentStream* streams, uint8_t count, PdfByteStore decodedStore,
                  PdfStreamDecodeLimits limits = {});
  PdfStatus beginAppend(const PdfEncodedContentStream* streams, uint8_t count, PdfByteStore decodedStore,
                        uint64_t existingBytes, PdfStreamDecodeLimits limits = {});
  PdfStatus beginSequence(const PdfEncodedContentStream* streams, uint8_t count, PdfByteStore decodedStore,
                          PdfStreamDecodeLimits limits = {});
  PdfStatus beginSequenceAppend(const PdfEncodedContentStream* streams, uint8_t count,
                                PdfByteStore decodedStore, uint64_t existingBytes,
                                PdfStreamDecodeLimits limits = {});
  PdfStepResult step(PdfWorkBudget& budget);

  const PdfByteSource* sources() const { return sources_; }
  uint8_t count() const { return phase_ == Phase::Done ? streamCount_ : 0; }
  uint64_t decodedBytes() const { return decodedBytes_; }
  PdfByteSource sequenceSource() const {
    return phase_ == Phase::Done && sequenceMode_ ? sequenceSource_ : PdfByteSource{};
  }

 private:
  enum class Phase : uint8_t {
    Idle,
    ResetStore,
    BeginStream,
    WriteSeparator,
    DecodeStream,
    FinishStream,
    FinalizeSources,
    CleanupStore,
    Done,
    Failed,
  };

  PdfStepResult fail(PdfStatus status);
  PdfStatus beginInternal(const PdfEncodedContentStream* streams, uint8_t count, PdfByteStore decodedStore,
                          uint64_t existingBytes, bool resetStore, bool sequenceMode,
                          bool prefixSeparator, PdfStreamDecodeLimits limits);

  PdfStreamDecoder decoder_;
  const PdfEncodedContentStream* streams_ = nullptr;
  PdfByteStore decodedStore_{};
  PdfStreamDecodeLimits limits_{};
  PdfByteRange ranges_[MaxSources]{};
  PdfByteSource sources_[MaxSources]{};
  uint64_t offsets_[MaxSources]{};
  uint64_t lengths_[MaxSources]{};
  PdfByteRange sequenceRange_{};
  PdfByteSource sequenceSource_{};
  PdfStatus failure_{};
  uint64_t decodedBytes_ = 0;
  uint8_t streamCount_ = 0;
  uint8_t streamIndex_ = 0;
  uint8_t finalizeIndex_ = 0;
  Phase phase_ = Phase::Idle;
  bool storeReady_ = false;
  bool sequenceMode_ = false;
  bool prefixSeparator_ = false;
  bool separatorWrittenForCurrent_ = false;
};

static_assert(sizeof(PdfPreparedContentStreams) <= PdfLimits::PageRunBytes,
              "prepared content stream state must fit the fixed page-run phase overlay");

struct PdfPreparedFontResource {
  uint32_t nameHash = 0;
  PdfFontMap* font = nullptr;
  uint16_t nameTag = 0;
  uint8_t nameLength = 0;
};

struct PdfPreparedXObjectResource {
  uint8_t name[32]{};
  PdfContentXObject object{};
  uint8_t nameLength = 0;
};

struct PdfPreparedContentInlineImageHooks {
  void* context = nullptr;
  PdfContentResources::ConsumeInlineImageTokenFn consumeInlineImageToken = nullptr;
  PdfContentResources::FinishInlineImageFn finishInlineImage = nullptr;
};

struct PdfPreparedContentResourceWorkspace {
  PdfPreparedFontResource* fonts = nullptr;
  size_t fontCapacity = 0;
  PdfPreparedXObjectResource* xobjects = nullptr;
  size_t xobjectCapacity = 0;
  // The resource table copies these function pointers. Only their context
  // remains caller-owned and must outlive interpretation.
  const PdfPreparedContentInlineImageHooks* inlineImageHooks = nullptr;
  const PdfContentResources* parent = nullptr;
};

class PdfPreparedContentResources {
 public:
  static constexpr uint8_t MaxFonts = 40;
  static constexpr uint8_t MaxXObjects = 96;
  static constexpr uint8_t MaxNameBytes = 32;

  explicit PdfPreparedContentResources(PdfPreparedContentResourceWorkspace workspace)
      : fonts_(workspace.fonts),
        fontCapacity_(workspace.fontCapacity),
        xobjects_(workspace.xobjects),
        xobjectCapacity_(workspace.xobjectCapacity),
        inlineImageHooks_(workspace.inlineImageHooks != nullptr
                              ? *workspace.inlineImageHooks
                              : PdfPreparedContentInlineImageHooks{}),
        parent_(workspace.parent),
        descriptor_{this, resolveFont, resolveXObject,
                    inlineImageHooks_.consumeInlineImageToken != nullptr
                        ? forwardInlineImageToken
                        : nullptr,
                    inlineImageHooks_.finishInlineImage != nullptr ? forwardInlineImage : nullptr} {}

  PdfPreparedContentResources(const PdfPreparedContentResources&) = delete;
  PdfPreparedContentResources& operator=(const PdfPreparedContentResources&) = delete;
  PdfPreparedContentResources(PdfPreparedContentResources&&) = delete;
  PdfPreparedContentResources& operator=(PdfPreparedContentResources&&) = delete;

  PdfStatus reset();
  PdfStatus addFont(const uint8_t* name, size_t length, PdfFontMap* font);
  PdfStatus addXObject(const uint8_t* name, size_t length, const PdfContentXObject& object);
  const PdfContentResources& descriptor() const;

  uint8_t fontCount() const { return fontCount_; }
  uint8_t xobjectCount() const { return xobjectCount_; }

 private:
  static PdfStatus resolveFont(void* context, const uint8_t* name, size_t length, PdfFontMap** font);
  static PdfStatus resolveXObject(void* context, const uint8_t* name, size_t length, PdfContentXObject* object);
  static PdfStatus forwardInlineImageToken(void* context, const PdfToken& token);
  static PdfStepResult forwardInlineImage(void* context, const PdfByteSource& source, uint64_t idEndOffset,
                                          PdfWorkBudget& budget, uint64_t* resumeOffset,
                                          PdfContentXObject* image);

  PdfPreparedFontResource* fonts_ = nullptr;
  size_t fontCapacity_ = 0;
  PdfPreparedXObjectResource* xobjects_ = nullptr;
  size_t xobjectCapacity_ = 0;
  PdfPreparedContentInlineImageHooks inlineImageHooks_{};
  const PdfContentResources* parent_ = nullptr;
  PdfContentResources descriptor_{};
  uint8_t fontCount_ = 0;
  uint8_t xobjectCount_ = 0;
  bool ready_ = false;
};
