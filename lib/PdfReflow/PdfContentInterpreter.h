#pragma once

#include <cstddef>
#include <cstdint>

#include "PdfFontMap.h"
#include "PdfPageModel.h"

enum class PdfContentOperandKind : uint8_t {
  Number,
  Name,
  String,
  Array,
  Dictionary,
  ActualText,
};

struct PdfContentOperand {
  int32_t number = 0;
  uint16_t textOffset = 0;
  uint16_t textLength = 0;
  uint16_t firstItem = 0;
  uint16_t itemCount = 0;
  PdfContentOperandKind kind = PdfContentOperandKind::Number;
  uint8_t reserved = 0;
};

struct PdfContentArrayItem {
  int32_t number = 0;
  uint16_t textOffset = 0;
  uint16_t textLength = 0;
  PdfContentOperandKind kind = PdfContentOperandKind::Number;
  uint8_t reserved[3]{};
};

struct PdfContentResources;

enum class PdfContentXObjectKind : uint8_t {
  Image,
  Form,
};

struct PdfContentXObject {
  PdfContentXObjectKind kind = PdfContentXObjectKind::Image;
  PdfObjectReference reference{};
  PdfByteSource content{};
  const PdfContentResources* resources = nullptr;
  PdfMatrix matrix{};
  uint32_t pixelWidth = 0;
  uint32_t pixelHeight = 0;
};

struct PdfContentResources {
  using ResolveFontFn = PdfStatus (*)(void* context, const uint8_t* name, size_t length, PdfFontMap** font);
  using ResolveXObjectFn = PdfStatus (*)(void* context, const uint8_t* name, size_t length, PdfContentXObject* xobject);

  void* context = nullptr;
  ResolveFontFn resolveFont = nullptr;
  ResolveXObjectFn resolveXObject = nullptr;
};

struct PdfContentInterpreterWorkspace {
  uint8_t* sourceBuffer = nullptr;
  size_t sourceBufferSize = 0;
  PdfContentOperand* operands = nullptr;
  uint8_t operandCapacity = 0;
  PdfContentArrayItem* arrayItems = nullptr;
  uint8_t arrayItemCapacity = 0;
  uint8_t* scratchText = nullptr;
  uint16_t scratchTextCapacity = 0;
  uint8_t* markedText = nullptr;
  uint16_t markedTextCapacity = 0;
  uint32_t* documentOperatorCount = nullptr;
};

class PdfContentInterpreter {
 public:
  explicit PdfContentInterpreter(PdfContentInterpreterWorkspace workspace);

  PdfStatus begin(const PdfByteSource* contentSources, uint8_t contentSourceCount, const PdfContentResources& resources,
                  PdfPageModel& pageModel);
  PdfStepResult step(PdfWorkBudget& budget);

  uint32_t operatorCount() const { return operatorCount_; }
  uint8_t maximumFormDepth() const { return maximumFormDepth_; }

 private:
  struct GraphicsState {
    PdfMatrix ctm{};
    uint8_t nonstrokingLuminance = 0;
  };

  struct TextState {
    PdfMatrix matrix{};
    PdfMatrix lineMatrix{};
    PdfFixed16 fontSize{};
    PdfFixed16 characterSpacing{};
    PdfFixed16 wordSpacing{};
    PdfFixed16 horizontalScale = PdfFixed16::fromInteger(1);
    PdfFixed16 leading{};
    PdfFixed16 rise{};
    PdfFontMap* font = nullptr;
    uint8_t renderMode = 0;
    bool active = false;
  };

  struct MarkedContentFrame {
    uint16_t textOffset = 0;
    uint16_t textLength = 0;
    uint16_t runIndex = UINT16_MAX;
    bool hasActualText = false;
    bool emitted = false;
  };

  struct FormFrame {
    PdfByteSource source{};
    uint64_t resumeOffset = 0;
    const PdfContentResources* resources = nullptr;
    GraphicsState graphics{};
    TextState text{};
    PdfObjectReference formReference{};
    uint16_t markedTextLength = 0;
    uint8_t graphicsDepth = 0;
    uint8_t markedDepth = 0;
    uint8_t topLevelSourceIndex = 0;
  };

  enum class Phase : uint8_t {
    Tokens,
    InlineImageData,
    Done,
    Failed,
  };

  enum class InlineKey : uint8_t {
    None,
    Width,
    Height,
  };

  PdfStatus handleToken(const PdfToken& token);
  PdfStatus handleArrayToken(const PdfToken& token);
  PdfStatus handleDictionaryToken(const PdfToken& token);
  PdfStatus handleInlineDictionaryToken(const PdfToken& token);
  PdfStatus processOperator(const PdfToken& token);
  PdfStatus processTextOperator(const PdfToken& token);
  PdfStatus processGraphicsOperator(const PdfToken& token);
  PdfStatus processMarkedContentOperator(const PdfToken& token);
  PdfStatus processXObjectOperator(const PdfToken& token);
  PdfStatus enterForm(const PdfContentXObject& form);
  PdfStatus leaveFormOrAdvanceSource(bool* complete);
  PdfStatus showString(const uint8_t* source, size_t length);
  PdfStatus showArray(const PdfContentOperand& array);
  PdfStatus emitActualText(MarkedContentFrame& frame);
  PdfStatus emitDecodedText(const uint8_t* source, size_t length, bool actualText);
  PdfStatus advanceVisualText(const uint8_t* source, size_t length);
  PdfStatus appendInlineImage();
  PdfStatus appendImage(const PdfContentXObject& image, bool inlineImage);
  PdfStatus pushOperand(const PdfContentOperand& operand);
  PdfStatus pushTextOperand(PdfContentOperandKind kind, const uint8_t* text, size_t length);
  PdfStatus pushNumberOperand(const PdfToken& token);
  PdfStatus pushMarkedContent(const PdfContentOperand* actualText);
  PdfStatus translateText(PdfFixed16 x, PdfFixed16 y, bool lineMatrix);
  PdfStatus adjustText(PdfFixed16 amount);
  PdfStatus currentTextPoint(PdfFixed16 textX, PdfFixed16 textY, PdfFixed16* x, PdfFixed16* y) const;
  PdfStatus currentTextOrigin(PdfFixed16* x, PdfFixed16* y) const;
  PdfStatus currentTextAscent(PdfFixed16* x, PdfFixed16* y) const;
  PdfStatus makeRun(PdfTextRun* run, uint16_t flags) const;
  PdfStatus expandRunGeometry(uint16_t runIndex);
  PdfStatus advanceGlyph(const PdfDecodedGlyph& glyph);
  PdfStatus advanceFallback(size_t byteCount);
  PdfStatus copyScratch(const uint8_t* source, size_t length, uint16_t* offset);
  PdfStatus countOperator();
  PdfStatus failStatus(PdfError error, uint64_t offset = 0) const;
  PdfStepResult fail(PdfStatus status);
  void clearOperands();

  static PdfStatus pageTextWrite(void* context, const uint8_t* source, size_t requested, size_t* bytesWritten);

  PdfContentInterpreterWorkspace workspace_{};
  PdfLexer lexer_;
  const PdfByteSource* topLevelSources_ = nullptr;
  const PdfContentResources* resources_ = nullptr;
  PdfPageModel* pageModel_ = nullptr;
  PdfByteSource currentSource_{};
  GraphicsState graphics_{};
  TextState text_{};
  GraphicsState graphicsStack_[16]{};
  TextState textStack_[16]{};
  MarkedContentFrame markedStack_[16]{};
  FormFrame formStack_[PdfLimits::MaxFormDepth]{};
  PdfStatus failure_{};
  Phase phase_ = Phase::Done;
  uint32_t operatorCount_ = 0;
  uint32_t sourceOrder_ = 0;
  uint16_t scratchTextLength_ = 0;
  uint16_t markedTextLength_ = 0;
  uint16_t dictionaryActualTextOffset_ = 0;
  uint16_t dictionaryActualTextLength_ = 0;
  uint32_t inlineWidth_ = 0;
  uint32_t inlineHeight_ = 0;
  uint8_t operandCount_ = 0;
  uint8_t arrayItemCount_ = 0;
  uint8_t arrayStart_ = 0;
  uint8_t graphicsDepth_ = 0;
  uint8_t markedDepth_ = 0;
  uint8_t formDepth_ = 0;
  uint8_t maximumFormDepth_ = 0;
  uint8_t topLevelSourceIndex_ = 0;
  uint8_t topLevelSourceCount_ = 0;
  uint8_t dictionaryDepth_ = 0;
  InlineKey inlineKey_ = InlineKey::None;
  bool arrayOpen_ = false;
  bool dictionaryCapturingActualText_ = false;
  bool dictionaryHasActualText_ = false;
  bool inlineDictionary_ = false;
};
