#include "PdfContentInterpreter.h"

#include <algorithm>
#include <climits>
#include <cstring>
#include <limits>

namespace {

constexpr uint64_t OPERATOR_PAGE_LIMIT = std::numeric_limits<uint64_t>::max();
constexpr uint64_t OPERATOR_DOCUMENT_LIMIT = OPERATOR_PAGE_LIMIT - 1;

template <size_t Length>
bool tokenEquals(const PdfToken& token, const char (&expected)[Length]) {
  static_assert(Length != 0);
  return token.length == Length - 1U && std::memcmp(token.bytes, expected, Length - 1U) == 0;
}

bool parseWideFixed(const PdfToken& token, int64_t* value) {
  if (value == nullptr || (token.kind != PdfTokenKind::Integer && token.kind != PdfTokenKind::Real) ||
      token.length == 0) {
    return false;
  }
  size_t position = 0;
  bool negative = false;
  if (token.bytes[position] == '+' || token.bytes[position] == '-') {
    negative = token.bytes[position] == '-';
    ++position;
  }
  if (position == token.length) {
    return false;
  }
  uint64_t whole = 0;
  int64_t fraction = 0;
  int64_t scale = 1;
  bool decimal = false;
  bool digitSeen = false;
  for (; position < token.length; ++position) {
    const char byte = token.bytes[position];
    if (byte == '.' && !decimal) {
      decimal = true;
      continue;
    }
    if (byte < '0' || byte > '9') {
      return false;
    }
    digitSeen = true;
    const uint8_t digit = static_cast<uint8_t>(byte - '0');
    if (!decimal) {
      constexpr uint64_t MaxWhole = static_cast<uint64_t>(INT64_MAX) / 65536U;
      if (whole > (MaxWhole - digit) / 10U) {
        return false;
      }
      whole = whole * 10 + digit;
    } else if (scale < 1000000) {
      fraction = fraction * 10 + digit;
      scale *= 10;
    }
  }
  if (!digitSeen) {
    return false;
  }
  const int64_t fractionalRaw = (fraction * 65536 + scale / 2) / scale;
  const uint64_t wholeRaw = whole * 65536U;
  if (wholeRaw > static_cast<uint64_t>(INT64_MAX) - static_cast<uint64_t>(fractionalRaw)) {
    return false;
  }
  const int64_t raw = static_cast<int64_t>(wholeRaw + static_cast<uint64_t>(fractionalRaw));
  *value = negative ? -raw : raw;
  return true;
}

bool parseFixed(const PdfToken& token, int32_t* value) {
  int64_t raw = 0;
  if (value == nullptr || !parseWideFixed(token, &raw)) {
    return false;
  }
  *value = static_cast<int32_t>(std::clamp<int64_t>(raw, INT32_MIN, INT32_MAX));
  return true;
}

bool checkedAdd64(const int64_t left, const int64_t right, int64_t* const result) {
  if (result == nullptr || (right > 0 && left > INT64_MAX - right) ||
      (right < 0 && left < INT64_MIN - right)) {
    return false;
  }
  *result = left + right;
  return true;
}

bool checkedMultiply64(const int64_t left, const int64_t right, int64_t* const result) {
  if (result == nullptr) {
    return false;
  }
  if (left == 0 || right == 0) {
    *result = 0;
    return true;
  }
  if ((left == -1 && right == INT64_MIN) || (right == -1 && left == INT64_MIN)) {
    return false;
  }
  if ((left > 0 && right > 0 && left > INT64_MAX / right) ||
      (left > 0 && right < 0 && right < INT64_MIN / left) ||
      (left < 0 && right > 0 && left < INT64_MIN / right) ||
      (left < 0 && right < 0 && left < INT64_MAX / right)) {
    return false;
  }
  *result = left * right;
  return true;
}

bool multiplyFixedWide(const int32_t left, const int64_t right, int64_t* const result) {
  const int64_t whole = right / 65536;
  const int64_t remainder = right % 65536;
  int64_t wholeProduct = 0;
  if (!checkedMultiply64(left, whole, &wholeProduct)) {
    return false;
  }
  const int64_t fractionalProduct = static_cast<int64_t>(left) * remainder;
  const int64_t fractional = fractionalProduct >= 0
                                 ? (fractionalProduct + 32768) / 65536
                                 : -(((-fractionalProduct) + 32768) / 65536);
  return checkedAdd64(wholeProduct, fractional, result);
}

bool concatenateWide(const PdfMatrix& current, const PdfContentOperand* const next, PdfMatrix* const result) {
  if (next == nullptr || result == nullptr) {
    return false;
  }
  const auto component = [&next](const int32_t leftFirst, const uint8_t rightFirst,
                                 const int32_t leftSecond, const uint8_t rightSecond, const int32_t add,
                                 PdfFixed16* const output) {
    int64_t first = 0;
    int64_t second = 0;
    int64_t sum = 0;
    if (output == nullptr || !multiplyFixedWide(leftFirst, next[rightFirst].wideNumber(), &first) ||
        !multiplyFixedWide(leftSecond, next[rightSecond].wideNumber(), &second) ||
        !checkedAdd64(first, second, &sum) || !checkedAdd64(sum, add, &sum) || sum < INT32_MIN || sum > INT32_MAX) {
      return false;
    }
    output->raw = static_cast<int32_t>(sum);
    return true;
  };
  PdfMatrix value;
  return component(current.a.raw, 0, current.c.raw, 1, 0, &value.a) &&
         component(current.b.raw, 0, current.d.raw, 1, 0, &value.b) &&
         component(current.a.raw, 2, current.c.raw, 3, 0, &value.c) &&
         component(current.b.raw, 2, current.d.raw, 3, 0, &value.d) &&
         component(current.a.raw, 4, current.c.raw, 5, current.e.raw, &value.e) &&
         component(current.b.raw, 4, current.d.raw, 5, current.f.raw, &value.f) && (*result = value, true);
}

bool fixedNegate(const PdfFixed16 value, PdfFixed16* result) {
  if (result == nullptr || value.raw == INT32_MIN) {
    return false;
  }
  result->raw = -value.raw;
  return true;
}

bool fixedDivide(const PdfFixed16 value, const int32_t divisor, PdfFixed16* result) {
  if (result == nullptr || divisor == 0) {
    return false;
  }
  result->raw = value.raw / divisor;
  return true;
}

bool fixedFromProductRatio(const PdfFixed16 value, const int32_t multiplier, const int32_t divisor,
                           PdfFixed16* result) {
  if (result == nullptr || divisor == 0) {
    return false;
  }
  const int64_t raw = static_cast<int64_t>(value.raw) * multiplier / divisor;
  if (raw < INT32_MIN || raw > INT32_MAX) {
    return false;
  }
  result->raw = static_cast<int32_t>(raw);
  return true;
}

constexpr int32_t kPdfColorOne = 65536L;
constexpr uint8_t kMarkedContentEmitted = 1U << 0;
constexpr uint8_t kMarkedContentSuppressed = 1U << 1;
constexpr uint8_t kMarkedContentOverflow = 1U << 2;

int32_t clampColor(const int32_t raw) {
  if (raw <= 0) {
    return 0;
  }
  return raw >= kPdfColorOne ? kPdfColorOne : raw;
}

uint8_t colorByte(const int32_t raw) {
  const int32_t bounded = clampColor(raw);
  return static_cast<uint8_t>((static_cast<int64_t>(bounded) * 255 + kPdfColorOne / 2) / kPdfColorOne);
}

uint8_t rgbLuminance(const uint8_t red, const uint8_t green, const uint8_t blue) {
  return static_cast<uint8_t>((77U * red + 150U * green + 29U * blue + 128U) >> 8U);
}

bool colorLuminance(const PdfContentOperand* const operands, const uint8_t count, uint8_t* const luminance) {
  if (operands == nullptr || luminance == nullptr || (count != 1U && count != 3U && count != 4U)) {
    return false;
  }
  for (uint8_t index = 0; index < count; ++index) {
    if (operands[index].kind != PdfContentOperandKind::Number) {
      return false;
    }
  }
  if (count == 1U) {
    *luminance = colorByte(operands[0].number);
    return true;
  }
  if (count == 3U) {
    *luminance =
        rgbLuminance(colorByte(operands[0].number), colorByte(operands[1].number), colorByte(operands[2].number));
    return true;
  }

  const int32_t cyan = clampColor(operands[0].number);
  const int32_t magenta = clampColor(operands[1].number);
  const int32_t yellow = clampColor(operands[2].number);
  const int32_t black = clampColor(operands[3].number);
  const uint8_t red = colorByte(kPdfColorOne - clampColor(cyan + black));
  const uint8_t green = colorByte(kPdfColorOne - clampColor(magenta + black));
  const uint8_t blue = colorByte(kPdfColorOne - clampColor(yellow + black));
  *luminance = rgbLuminance(red, green, blue);
  return true;
}

bool concatenate(const PdfMatrix& current, const PdfMatrix& next, PdfMatrix* result) {
  if (result == nullptr) {
    return false;
  }
  PdfFixed16 aa;
  PdfFixed16 cb;
  PdfFixed16 ba;
  PdfFixed16 db;
  PdfFixed16 ac;
  PdfFixed16 cd;
  PdfFixed16 bc;
  PdfFixed16 dd;
  PdfFixed16 ae;
  PdfFixed16 cf;
  PdfFixed16 be;
  PdfFixed16 df;
  PdfMatrix value;
  return pdfFixedMultiply(current.a, next.a, &aa) && pdfFixedMultiply(current.c, next.b, &cb) &&
         pdfFixedAdd(aa, cb, &value.a) && pdfFixedMultiply(current.b, next.a, &ba) &&
         pdfFixedMultiply(current.d, next.b, &db) && pdfFixedAdd(ba, db, &value.b) &&
         pdfFixedMultiply(current.a, next.c, &ac) && pdfFixedMultiply(current.c, next.d, &cd) &&
         pdfFixedAdd(ac, cd, &value.c) && pdfFixedMultiply(current.b, next.c, &bc) &&
         pdfFixedMultiply(current.d, next.d, &dd) && pdfFixedAdd(bc, dd, &value.d) &&
         pdfFixedMultiply(current.a, next.e, &ae) && pdfFixedMultiply(current.c, next.f, &cf) &&
         pdfFixedAdd(ae, cf, &value.e) && pdfFixedAdd(value.e, current.e, &value.e) &&
         pdfFixedMultiply(current.b, next.e, &be) && pdfFixedMultiply(current.d, next.f, &df) &&
         pdfFixedAdd(be, df, &value.f) && pdfFixedAdd(value.f, current.f, &value.f) && (*result = value, true);
}

bool translate(PdfMatrix* matrix, const PdfFixed16 x, const PdfFixed16 y) {
  if (matrix == nullptr) {
    return false;
  }
  PdfFixed16 ax;
  PdfFixed16 cy;
  PdfFixed16 bx;
  PdfFixed16 dy;
  PdfFixed16 deltaX;
  PdfFixed16 deltaY;
  return pdfFixedMultiply(matrix->a, x, &ax) && pdfFixedMultiply(matrix->c, y, &cy) && pdfFixedAdd(ax, cy, &deltaX) &&
         pdfFixedAdd(matrix->e, deltaX, &matrix->e) && pdfFixedMultiply(matrix->b, x, &bx) &&
         pdfFixedMultiply(matrix->d, y, &dy) && pdfFixedAdd(bx, dy, &deltaY) &&
         pdfFixedAdd(matrix->f, deltaY, &matrix->f);
}

bool isVectorOperator(const PdfToken& token) {
  if (token.length == 1U) {
    switch (token.bytes[0]) {
      case 'm':
      case 'l':
      case 'c':
      case 'v':
      case 'y':
      case 'h':
      case 'S':
      case 's':
      case 'f':
      case 'F':
      case 'B':
      case 'b':
      case 'n':
      case 'W':
        return true;
      default:
        return false;
    }
  }
  return token.length == 2U &&
         ((token.bytes[0] == 'r' && token.bytes[1] == 'e') ||
          ((token.bytes[0] == 'f' || token.bytes[0] == 'B' || token.bytes[0] == 'b' ||
            token.bytes[0] == 'W') &&
           token.bytes[1] == '*'));
}

}  // namespace

PdfContentInterpreter::PdfContentInterpreter(const PdfContentInterpreterWorkspace workspace)
    : workspace_(workspace), lexer_({}, workspace.sourceBuffer, workspace.sourceBufferSize) {}

PdfStatus PdfContentInterpreter::begin(const PdfByteSource* const contentSources, const uint8_t contentSourceCount,
                                       const PdfContentResources& resources, PdfPageModel& pageModel) {
  if (contentSources == nullptr || contentSourceCount == 0 ||
      contentSourceCount > PdfLimits::MaxContentStreamsPerPage || workspace_.sourceBuffer == nullptr ||
      workspace_.sourceBufferSize == 0 ||
      workspace_.sourceBufferSize > PdfLimits::InterpreterSourceBufferBytes ||
      workspace_.operands == nullptr || workspace_.operandCapacity == 0 || workspace_.arrayItems == nullptr ||
      workspace_.arrayItemCapacity == 0 || workspace_.scratchText == nullptr || workspace_.scratchTextCapacity == 0 ||
      workspace_.markedText == nullptr || workspace_.markedTextCapacity == 0) {
    return PdfStatus::failure(PdfError::InvalidArgument);
  }
  for (uint8_t index = 0; index < contentSourceCount; ++index) {
    if (!contentSources[index].valid()) {
      return PdfStatus::failure(PdfError::InvalidArgument, index);
    }
  }
  const PdfStatus modelStatus = pageModel.reset();
  if (!modelStatus.ok()) {
    return modelStatus;
  }
  topLevelSources_ = contentSources;
  topLevelSourceCount_ = contentSourceCount;
  topLevelSourceIndex_ = 0;
  currentSource_ = contentSources[0];
  resources_ = &resources;
  pageModel_ = &pageModel;
  lexer_.setSource(currentSource_);
  graphics_ = {};
  if (workspace_.hasPageBounds) {
    graphics_.clip = workspace_.pageBounds;
    graphics_.hasClip = true;
  }
  text_ = {};
  failure_ = {};
  phase_ = Phase::Tokens;
  operatorCount_ = 0;
  sourceOrder_ = 0;
  scratchTextLength_ = 0;
  markedTextLength_ = 0;
  dictionaryActualTextOffset_ = 0;
  dictionaryActualTextLength_ = 0;
  inlineWidth_ = 0;
  inlineHeight_ = 0;
  operandCount_ = 0;
  arrayItemCount_ = 0;
  arrayStart_ = 0;
  graphicsDepth_ = 0;
  markedDepth_ = 0;
  formDepth_ = 0;
  maximumFormDepth_ = 0;
  dictionaryDepth_ = 0;
  inlineKey_ = InlineKey::None;
  arrayOpen_ = false;
  arrayHasString_ = false;
  arrayStreamed_ = false;
  arrayPendingAdjustment_ = INT32_MAX;
  dictionaryCapturingActualText_ = false;
  dictionaryHasActualText_ = false;
  inlineDictionary_ = false;
  textCapacityReached_ = false;
  resetCurrentPath();
  return PdfStatus::success();
}

PdfStatus PdfContentInterpreter::failStatus(const PdfError error, const uint64_t offset) const {
  return PdfStatus::failure(error, offset == 0 ? lexer_.tokenOffset() : offset);
}

PdfStepResult PdfContentInterpreter::fail(const PdfStatus status) {
  if (pageModel_ != nullptr) {
    pageModel_->abortTextRun();
  }
  failure_ = status;
  phase_ = Phase::Failed;
  return PdfStepResult::failure(status);
}

PdfStepResult PdfContentInterpreter::step(PdfWorkBudget& budget) {
  if (phase_ == Phase::Done) {
    return PdfStepResult::completed();
  }
  if (phase_ == Phase::Failed) {
    return PdfStepResult::failure(failure_);
  }
  while (budget.operationsRemaining != 0) {
    if (phase_ == Phase::InlineImageData) {
      if (budget.stopRequested()) {
        break;
      }
      if (resources_ == nullptr || resources_->finishInlineImage == nullptr) {
        return fail(PdfStatus::failure(PdfError::UnsupportedFilter, lexer_.position()));
      }
      uint64_t resumeOffset = 0;
      PdfContentXObject image;
      image.kind = PdfContentXObjectKind::Image;
      image.pixelWidth = inlineWidth_;
      image.pixelHeight = inlineHeight_;
      const PdfStepResult finishResult = resources_->finishInlineImage(
          resources_->context, currentSource_, lexer_.position(), budget, &resumeOffset, &image);
      if (finishResult.yielded()) {
        return finishResult;
      }
      if (finishResult.failed()) {
        return fail(finishResult.status);
      }
      if (image.kind != PdfContentXObjectKind::Image || resumeOffset <= lexer_.position() ||
          resumeOffset > currentSource_.size) {
        return fail(PdfStatus::failure(PdfError::Malformed, resumeOffset));
      }
      lexer_.setSource(currentSource_, resumeOffset);
      const PdfStatus imageStatus = appendImage(image, true);
      if (!imageStatus.ok()) {
        return fail(imageStatus);
      }
      inlineDictionary_ = false;
      inlineKey_ = InlineKey::None;
      phase_ = Phase::Tokens;
      clearOperands();
      continue;
    }

    if (arrayOpen_ && (arrayHasString_ || arrayStreamed_) && arrayItemCount_ > arrayStart_) {
      const PdfStatus status = flushTextArrayChunk();
      if (!status.ok()) {
        return fail(status);
      }
      arrayStreamed_ = true;
      continue;
    }

    PdfToken token;
    uint8_t* const stringBuffer = workspace_.scratchText + scratchTextLength_;
    const size_t stringCapacity = workspace_.scratchTextCapacity - scratchTextLength_;
    const PdfStepResult tokenResult = lexer_.next(token, budget, stringBuffer, stringCapacity);
    if (tokenResult.yielded()) {
      return tokenResult;
    }
    if (tokenResult.failed()) {
      return fail(tokenResult.status);
    }
    if (token.kind == PdfTokenKind::End) {
      bool complete = false;
      const PdfStatus status = leaveFormOrAdvanceSource(&complete);
      if (!status.ok()) {
        return fail(status);
      }
      if (complete) {
        phase_ = Phase::Done;
        return PdfStepResult::completed();
      }
      continue;
    }
    const PdfStatus status = handleToken(token);
    if (!status.ok()) {
      if (status.error == PdfError::LimitExceeded &&
          (status.offset == OPERATOR_PAGE_LIMIT || status.offset == OPERATOR_DOCUMENT_LIMIT)) {
        pageModel_->abortTextRun();
        pageModel_->addWarning(PdfPageWarning::VectorArtOmitted);
        if (status.offset == OPERATOR_DOCUMENT_LIMIT && workspace_.documentOperatorCount != nullptr) {
          *workspace_.documentOperatorCount = 0;
        }
        clearOperands();
        phase_ = Phase::Done;
        return PdfStepResult::completed();
      }
      if (status.error == PdfError::LimitExceeded && formDepth_ != 0) {
        pageModel_->addWarning(PdfPageWarning::VectorArtOmitted);
        const PdfStatus abandonStatus = abandonCurrentForm();
        if (!abandonStatus) {
          return fail(abandonStatus);
        }
        continue;
      }
      return fail(status);
    }
  }
  return budget.cancelRequested()
             ? fail(PdfStatus::failure(PdfError::Cancelled, lexer_.position()))
             : PdfStepResult::paused();
}

PdfStatus PdfContentInterpreter::copyScratch(const uint8_t* const source, const size_t length, uint16_t* const offset) {
  const size_t remaining = static_cast<size_t>(workspace_.scratchTextCapacity - scratchTextLength_);
  if (source == nullptr || offset == nullptr || length > remaining) {
    return failStatus(PdfError::LimitExceeded);
  }
  *offset = scratchTextLength_;
  if (source != workspace_.scratchText + scratchTextLength_) {
    std::memmove(workspace_.scratchText + scratchTextLength_, source, length);
  }
  scratchTextLength_ += static_cast<uint16_t>(length);
  return PdfStatus::success();
}

const uint8_t* PdfContentInterpreter::tokenText(const PdfToken& token) const {
  if (token.reserved[1] != 0) {
    return nullptr;
  }
  if (token.reserved[0] == 0) {
    return reinterpret_cast<const uint8_t*>(token.bytes);
  }
  if (workspace_.scratchText == nullptr || scratchTextLength_ > workspace_.scratchTextCapacity ||
      token.length > static_cast<uint32_t>(workspace_.scratchTextCapacity - scratchTextLength_)) {
    return nullptr;
  }
  return workspace_.scratchText + scratchTextLength_;
}

PdfStatus PdfContentInterpreter::pushOperand(const PdfContentOperand& operand) {
  if (operandCount_ >= workspace_.operandCapacity) {
    return failStatus(PdfError::LimitExceeded);
  }
  workspace_.operands[operandCount_++] = operand;
  return PdfStatus::success();
}

PdfStatus PdfContentInterpreter::pushTextOperand(const PdfContentOperandKind kind, const uint8_t* const text,
                                                 const size_t length) {
  PdfContentOperand operand;
  operand.kind = kind;
  operand.textLength = static_cast<uint16_t>(length);
  const PdfStatus status = copyScratch(text, length, &operand.textOffset);
  return status.ok() ? pushOperand(operand) : status;
}

PdfStatus PdfContentInterpreter::pushNumberOperand(const PdfToken& token) {
  PdfContentOperand operand;
  operand.kind = PdfContentOperandKind::Number;
  int64_t raw = 0;
  if (!parseWideFixed(token, &raw)) {
    return failStatus(PdfError::Malformed);
  }
  operand.setNumber(raw);
  return pushOperand(operand);
}

void PdfContentInterpreter::clearOperands() {
  operandCount_ = 0;
  arrayItemCount_ = 0;
  scratchTextLength_ = 0;
  arrayOpen_ = false;
  arrayHasString_ = false;
  arrayStreamed_ = false;
  arrayPendingAdjustment_ = INT32_MAX;
  dictionaryDepth_ = 0;
  dictionaryCapturingActualText_ = false;
  dictionaryHasActualText_ = false;
}

PdfStatus PdfContentInterpreter::handleArrayToken(const PdfToken& token) {
  if (token.kind == PdfTokenKind::ArrayEnd) {
    if (arrayStreamed_) {
      const PdfStatus status = flushTextArrayChunk();
      if (!status.ok()) {
        return status;
      }
    }
    PdfContentOperand operand;
    operand.kind = PdfContentOperandKind::Array;
    operand.firstItem = arrayStart_;
    operand.itemCount = static_cast<uint16_t>(arrayItemCount_ - arrayStart_);
    operand.reserved = arrayStreamed_ ? 1U : 0U;
    arrayOpen_ = false;
    return pushOperand(operand);
  }
  if (token.kind == PdfTokenKind::ArrayBegin || token.kind == PdfTokenKind::DictionaryBegin) {
    return failStatus(PdfError::LimitExceeded);
  }
  const bool stringItem = token.kind == PdfTokenKind::String || token.kind == PdfTokenKind::HexString;
  const uint8_t* const stringText = stringItem ? tokenText(token) : nullptr;
  const bool needsItemSpace = arrayItemCount_ >= workspace_.arrayItemCapacity;
  const bool needsTextSpace =
      stringItem && token.length > static_cast<uint32_t>(workspace_.scratchTextCapacity - scratchTextLength_);
  if (needsItemSpace || needsTextSpace) {
    // TJ arrays in real-world PDFs often contain hundreds of alternating text
    // and kerning operands. Process full chunks as they arrive instead of
    // silently discarding everything beyond the fixed workspace. A standard
    // content-stream array containing a string is a text-showing array.
    if (!text_.active || operandCount_ != 0 || (!arrayStreamed_ && !arrayHasString_ && !stringItem)) {
      return needsTextSpace ? failStatus(PdfError::LimitExceeded) : PdfStatus::success();
    }
    const PdfStatus status = flushTextArrayChunk();
    if (!status.ok()) {
      return status;
    }
    arrayStreamed_ = true;
  }
  PdfContentArrayItem item;
  if (token.kind == PdfTokenKind::Integer || token.kind == PdfTokenKind::Real) {
    item.kind = PdfContentOperandKind::Number;
    if (!parseFixed(token, &item.number)) {
      return failStatus(PdfError::Malformed);
    }
  } else if (token.kind == PdfTokenKind::String || token.kind == PdfTokenKind::HexString) {
    item.kind = PdfContentOperandKind::String;
    item.textLength = static_cast<uint16_t>(token.length);
    const PdfStatus status = copyScratch(stringText, token.length, &item.textOffset);
    if (!status.ok()) {
      return status;
    }
    arrayHasString_ = true;
  } else {
    return failStatus(PdfError::Malformed);
  }
  workspace_.arrayItems[arrayItemCount_++] = item;
  return PdfStatus::success();
}

PdfStatus PdfContentInterpreter::handleDictionaryToken(const PdfToken& token) {
  if (token.kind == PdfTokenKind::DictionaryBegin) {
    if (dictionaryDepth_ == PdfLimits::MaxContainerNesting) {
      return failStatus(PdfError::LimitExceeded);
    }
    ++dictionaryDepth_;
    return PdfStatus::success();
  }
  if (token.kind == PdfTokenKind::DictionaryEnd) {
    if (dictionaryDepth_ == 0) {
      return failStatus(PdfError::Malformed);
    }
    --dictionaryDepth_;
    if (dictionaryDepth_ == 0) {
      PdfContentOperand operand;
      if (dictionaryHasActualText_) {
        operand.kind = PdfContentOperandKind::ActualText;
        operand.textOffset = dictionaryActualTextOffset_;
        operand.textLength = dictionaryActualTextLength_;
      } else {
        operand.kind = PdfContentOperandKind::Dictionary;
      }
      dictionaryCapturingActualText_ = false;
      dictionaryHasActualText_ = false;
      return pushOperand(operand);
    }
    return PdfStatus::success();
  }
  if (dictionaryDepth_ != 1) {
    return PdfStatus::success();
  }
  if (token.kind == PdfTokenKind::Name && tokenEquals(token, "ActualText")) {
    dictionaryCapturingActualText_ = true;
    return PdfStatus::success();
  }
  if (dictionaryCapturingActualText_) {
    dictionaryCapturingActualText_ = false;
    if (token.kind != PdfTokenKind::String && token.kind != PdfTokenKind::HexString) {
      return failStatus(PdfError::Malformed);
    }
    dictionaryActualTextLength_ = static_cast<uint16_t>(token.length);
    const PdfStatus status = copyScratch(tokenText(token), token.length, &dictionaryActualTextOffset_);
    if (!status.ok()) {
      return status;
    }
    dictionaryHasActualText_ = true;
  }
  return PdfStatus::success();
}

PdfStatus PdfContentInterpreter::handleInlineDictionaryToken(const PdfToken& token) {
  if (resources_ != nullptr && resources_->consumeInlineImageToken != nullptr) {
    const PdfStatus status = resources_->consumeInlineImageToken(resources_->context, token);
    if (!status.ok()) {
      return status;
    }
  }
  if (token.kind == PdfTokenKind::Keyword && tokenEquals(token, "ID")) {
    phase_ = Phase::InlineImageData;
    return PdfStatus::success();
  }
  if (token.kind == PdfTokenKind::Name) {
    if (tokenEquals(token, "W") || tokenEquals(token, "Width")) {
      inlineKey_ = InlineKey::Width;
    } else if (tokenEquals(token, "H") || tokenEquals(token, "Height")) {
      inlineKey_ = InlineKey::Height;
    } else {
      inlineKey_ = InlineKey::None;
    }
    return PdfStatus::success();
  }
  if (inlineKey_ != InlineKey::None && (token.kind == PdfTokenKind::Integer || token.kind == PdfTokenKind::Real)) {
    int32_t raw = 0;
    if (!parseFixed(token, &raw) || raw <= 0 || raw % 65536 != 0 ||
        static_cast<uint32_t>(raw / 65536) > PdfLimits::MaxImageDimension) {
      return failStatus(PdfError::LimitExceeded);
    }
    if (inlineKey_ == InlineKey::Width) {
      inlineWidth_ = static_cast<uint32_t>(raw / 65536);
    } else {
      inlineHeight_ = static_cast<uint32_t>(raw / 65536);
    }
    inlineKey_ = InlineKey::None;
  }
  return PdfStatus::success();
}

PdfStatus PdfContentInterpreter::handleToken(const PdfToken& token) {
  if (inlineDictionary_) {
    return handleInlineDictionaryToken(token);
  }
  if (dictionaryDepth_ != 0) {
    return handleDictionaryToken(token);
  }
  if (arrayOpen_) {
    return handleArrayToken(token);
  }
  switch (token.kind) {
    case PdfTokenKind::Integer:
    case PdfTokenKind::Real:
      return pushNumberOperand(token);
    case PdfTokenKind::Name:
      return pushTextOperand(PdfContentOperandKind::Name, reinterpret_cast<const uint8_t*>(token.bytes), token.length);
    case PdfTokenKind::String:
    case PdfTokenKind::HexString: {
      const uint8_t* const text = tokenText(token);
      return text != nullptr ? pushTextOperand(PdfContentOperandKind::String, text, token.length)
                             : failStatus(PdfError::LimitExceeded);
    }
    case PdfTokenKind::ArrayBegin:
      arrayOpen_ = true;
      arrayStart_ = arrayItemCount_;
      arrayHasString_ = false;
      arrayStreamed_ = false;
      arrayPendingAdjustment_ = INT32_MAX;
      return PdfStatus::success();
    case PdfTokenKind::DictionaryBegin:
      dictionaryDepth_ = 1;
      dictionaryCapturingActualText_ = false;
      dictionaryHasActualText_ = false;
      return PdfStatus::success();
    case PdfTokenKind::Keyword: {
      if (tokenEquals(token, "BI")) {
        const PdfStatus countStatus = countOperator();
        if (!countStatus.ok()) {
          return countStatus;
        }
        if (resources_ != nullptr && resources_->consumeInlineImageToken != nullptr) {
          const PdfStatus status = resources_->consumeInlineImageToken(resources_->context, token);
          if (!status.ok()) {
            return status;
          }
        }
        clearOperands();
        inlineDictionary_ = true;
        inlineKey_ = InlineKey::None;
        inlineWidth_ = 0;
        inlineHeight_ = 0;
        return PdfStatus::success();
      }
      const PdfStatus status = processOperator(token);
      clearOperands();
      return status;
    }
    case PdfTokenKind::ArrayEnd:
    case PdfTokenKind::DictionaryEnd:
    case PdfTokenKind::End:
      return failStatus(PdfError::Malformed);
  }
  return failStatus(PdfError::Malformed);
}

PdfStatus PdfContentInterpreter::countOperator() {
  if (operatorCount_ >= PdfLimits::MaxOperatorsPerPage) {
    return PdfStatus::failure(PdfError::LimitExceeded, OPERATOR_PAGE_LIMIT);
  }
  if (workspace_.documentOperatorCount != nullptr) {
    if (*workspace_.documentOperatorCount >= PdfLimits::MaxOperatorsPerDocument) {
      return PdfStatus::failure(PdfError::LimitExceeded, OPERATOR_DOCUMENT_LIMIT);
    }
    ++*workspace_.documentOperatorCount;
  }
  ++operatorCount_;
  return PdfStatus::success();
}

PdfStatus PdfContentInterpreter::processOperator(const PdfToken& token) {
  const PdfStatus countStatus = countOperator();
  if (!countStatus.ok()) {
    return countStatus;
  }
  if (tokenEquals(token, "BT") || tokenEquals(token, "ET") || tokenEquals(token, "Tf") || tokenEquals(token, "Tm") ||
      tokenEquals(token, "Td") || tokenEquals(token, "TD") || tokenEquals(token, "T*") || tokenEquals(token, "Tc") ||
      tokenEquals(token, "Tw") || tokenEquals(token, "Tz") || tokenEquals(token, "TL") || tokenEquals(token, "Ts") ||
      tokenEquals(token, "Tr") || tokenEquals(token, "Tj") || tokenEquals(token, "TJ") || tokenEquals(token, "'") ||
      tokenEquals(token, "\"")) {
    return processTextOperator(token);
  }
  if (tokenEquals(token, "q") || tokenEquals(token, "Q") || tokenEquals(token, "cm") || tokenEquals(token, "gs") ||
      tokenEquals(token, "g") || tokenEquals(token, "rg") || tokenEquals(token, "k") ||
      tokenEquals(token, "sc") || tokenEquals(token, "scn")) {
    return processGraphicsOperator(token);
  }
  if (tokenEquals(token, "BDC") || tokenEquals(token, "BMC") || tokenEquals(token, "EMC") || tokenEquals(token, "MP") ||
      tokenEquals(token, "DP")) {
    return processMarkedContentOperator(token);
  }
  if (tokenEquals(token, "Do")) {
    return processXObjectOperator(token);
  }
  if (isVectorOperator(token)) {
    return processPathOperator(token);
  }
  return PdfStatus::success();
}

PdfStatus PdfContentInterpreter::processTextOperator(const PdfToken& token) {
  if (tokenEquals(token, "BT")) {
    if (text_.active || operandCount_ != 0) {
      return failStatus(PdfError::Malformed);
    }
    text_.matrix = {};
    text_.lineMatrix = {};
    text_.active = true;
    text_.clipPending = false;
    return PdfStatus::success();
  }
  if (tokenEquals(token, "ET")) {
    if (!text_.active || operandCount_ != 0) {
      return failStatus(PdfError::Malformed);
    }
    if (text_.clipPending) {
      graphics_.clipRepresentable = false;
      text_.clipPending = false;
    }
    text_.active = false;
    return PdfStatus::success();
  }
  if (tokenEquals(token, "Tf")) {
    if (operandCount_ != 2 || workspace_.operands[0].kind != PdfContentOperandKind::Name ||
        workspace_.operands[1].kind != PdfContentOperandKind::Number || resources_ == nullptr ||
        resources_->resolveFont == nullptr) {
      return failStatus(PdfError::Malformed);
    }
    const PdfContentOperand& name = workspace_.operands[0];
    PdfFontMap* font = nullptr;
    const PdfStatus status =
        resources_->resolveFont(resources_->context, workspace_.scratchText + name.textOffset, name.textLength, &font);
    if (!status.ok()) {
      return status;
    }
    if (font == nullptr || workspace_.operands[1].number == 0) {
      return failStatus(PdfError::UnsupportedEncoding);
    }
    text_.font = font;
    text_.fontSize.raw = workspace_.operands[1].number;
    return PdfStatus::success();
  }
  const bool textStateOperator = tokenEquals(token, "Tc") || tokenEquals(token, "Tw") ||
                                 tokenEquals(token, "Tz") || tokenEquals(token, "TL") ||
                                 tokenEquals(token, "Ts") || tokenEquals(token, "Tr");
  if (!text_.active && !textStateOperator) {
    return failStatus(PdfError::Malformed);
  }
  if (tokenEquals(token, "Tm")) {
    if (operandCount_ != 6) {
      return failStatus(PdfError::Malformed);
    }
    for (uint8_t index = 0; index < 6; ++index) {
      if (workspace_.operands[index].kind != PdfContentOperandKind::Number) {
        return failStatus(PdfError::Malformed);
      }
    }
    text_.matrix = {{workspace_.operands[0].number}, {workspace_.operands[1].number}, {workspace_.operands[2].number},
                    {workspace_.operands[3].number}, {workspace_.operands[4].number}, {workspace_.operands[5].number}};
    text_.lineMatrix = text_.matrix;
    text_.positionReset = true;
    return PdfStatus::success();
  }
  if (tokenEquals(token, "Td") || tokenEquals(token, "TD")) {
    if (operandCount_ != 2 || workspace_.operands[0].kind != PdfContentOperandKind::Number ||
        workspace_.operands[1].kind != PdfContentOperandKind::Number) {
      return failStatus(PdfError::Malformed);
    }
    const PdfFixed16 x{workspace_.operands[0].number};
    const PdfFixed16 y{workspace_.operands[1].number};
    if (tokenEquals(token, "TD") && !fixedNegate(y, &text_.leading)) {
      return failStatus(PdfError::LimitExceeded);
    }
    return translateText(x, y, true);
  }
  if (tokenEquals(token, "T*")) {
    if (operandCount_ != 0) {
      return failStatus(PdfError::Malformed);
    }
    PdfFixed16 y;
    if (!fixedNegate(text_.leading, &y)) {
      return failStatus(PdfError::LimitExceeded);
    }
    return translateText({}, y, true);
  }
  if (tokenEquals(token, "Tc") || tokenEquals(token, "Tw") || tokenEquals(token, "TL") || tokenEquals(token, "Ts")) {
    if (operandCount_ != 1 || workspace_.operands[0].kind != PdfContentOperandKind::Number) {
      return failStatus(PdfError::Malformed);
    }
    PdfFixed16* destination = nullptr;
    if (tokenEquals(token, "Tc")) {
      destination = &text_.characterSpacing;
    } else if (tokenEquals(token, "Tw")) {
      destination = &text_.wordSpacing;
    } else if (tokenEquals(token, "TL")) {
      destination = &text_.leading;
    } else {
      destination = &text_.rise;
    }
    destination->raw = workspace_.operands[0].number;
    return PdfStatus::success();
  }
  if (tokenEquals(token, "Tz")) {
    if (operandCount_ != 1 || workspace_.operands[0].kind != PdfContentOperandKind::Number ||
        !fixedDivide({workspace_.operands[0].number}, 100, &text_.horizontalScale)) {
      return failStatus(PdfError::Malformed);
    }
    return PdfStatus::success();
  }
  if (tokenEquals(token, "Tr")) {
    if (operandCount_ != 1 || workspace_.operands[0].kind != PdfContentOperandKind::Number ||
        workspace_.operands[0].number % 65536 != 0) {
      return failStatus(PdfError::Malformed);
    }
    const int32_t mode = workspace_.operands[0].number / 65536;
    if (mode < 0 || mode > 7) {
      return failStatus(PdfError::Malformed);
    }
    text_.renderMode = static_cast<uint8_t>(mode);
    return PdfStatus::success();
  }
  if (tokenEquals(token, "Tj")) {
    if (operandCount_ != 1 || workspace_.operands[0].kind != PdfContentOperandKind::String) {
      return failStatus(PdfError::Malformed);
    }
    const PdfContentOperand& string = workspace_.operands[0];
    return showString(workspace_.scratchText + string.textOffset, string.textLength);
  }
  if (tokenEquals(token, "TJ")) {
    if (operandCount_ != 1 || workspace_.operands[0].kind != PdfContentOperandKind::Array) {
      return failStatus(PdfError::Malformed);
    }
    if (workspace_.operands[0].reserved != 0) {
      return workspace_.operands[0].itemCount == 0 ? PdfStatus::success() : failStatus(PdfError::Malformed);
    }
    return showArray(workspace_.operands[0]);
  }
  if (tokenEquals(token, "'")) {
    if (operandCount_ != 1 || workspace_.operands[0].kind != PdfContentOperandKind::String) {
      return failStatus(PdfError::Malformed);
    }
    PdfFixed16 y;
    if (!fixedNegate(text_.leading, &y)) {
      return failStatus(PdfError::LimitExceeded);
    }
    const PdfStatus lineStatus = translateText({}, y, true);
    if (!lineStatus.ok()) {
      return lineStatus;
    }
    const PdfContentOperand& string = workspace_.operands[0];
    return showString(workspace_.scratchText + string.textOffset, string.textLength);
  }
  if (tokenEquals(token, "\"")) {
    if (operandCount_ != 3 || workspace_.operands[0].kind != PdfContentOperandKind::Number ||
        workspace_.operands[1].kind != PdfContentOperandKind::Number ||
        workspace_.operands[2].kind != PdfContentOperandKind::String) {
      return failStatus(PdfError::Malformed);
    }
    text_.wordSpacing.raw = workspace_.operands[0].number;
    text_.characterSpacing.raw = workspace_.operands[1].number;
    PdfFixed16 y;
    if (!fixedNegate(text_.leading, &y)) {
      return failStatus(PdfError::LimitExceeded);
    }
    const PdfStatus lineStatus = translateText({}, y, true);
    if (!lineStatus.ok()) {
      return lineStatus;
    }
    const PdfContentOperand& string = workspace_.operands[2];
    return showString(workspace_.scratchText + string.textOffset, string.textLength);
  }
  return PdfStatus::success();
}

PdfStatus PdfContentInterpreter::processGraphicsOperator(const PdfToken& token) {
  if (tokenEquals(token, "q")) {
    if (operandCount_ != 0 ||
        graphicsDepth_ >= static_cast<uint8_t>(sizeof(graphicsStack_) / sizeof(graphicsStack_[0]))) {
      return failStatus(PdfError::LimitExceeded);
    }
    graphicsStack_[graphicsDepth_] = graphics_;
    textStack_[graphicsDepth_] = text_;
    ++graphicsDepth_;
    return PdfStatus::success();
  }
  if (tokenEquals(token, "Q")) {
    if (operandCount_ != 0 || graphicsDepth_ == 0) {
      return failStatus(PdfError::Malformed);
    }
    const PdfMatrix textMatrix = text_.matrix;
    const PdfMatrix lineMatrix = text_.lineMatrix;
    const bool textActive = text_.active;
    const bool clipPending = text_.clipPending;
    const bool positionReset = text_.positionReset;
    --graphicsDepth_;
    graphics_ = graphicsStack_[graphicsDepth_];
    text_ = textStack_[graphicsDepth_];
    // q/Q save text-state parameters, but the text and line matrices and the
    // current text-object lifetime are not part of the graphics-state stack.
    // A pending text clip also belongs to the current BT/ET sequence: keep it
    // until ET either applies it or clears it before a later Q.
    text_.matrix = textMatrix;
    text_.lineMatrix = lineMatrix;
    text_.active = textActive;
    text_.clipPending = clipPending;
    text_.positionReset = positionReset;
    return PdfStatus::success();
  }
  if (tokenEquals(token, "gs")) {
    if (operandCount_ != 1 || workspace_.operands[0].kind != PdfContentOperandKind::Name) {
      return failStatus(PdfError::Malformed);
    }
    // ExtGState commonly carries ordinary transparency, overprint, or blend
    // settings. Without resolving the dictionary, treating every gs operator
    // as invisible drops valid text and text-bearing Form XObjects. Preserve
    // text; explicit hidden text rendering modes remain handled separately.
    return PdfStatus::success();
  }
  if (tokenEquals(token, "g") || tokenEquals(token, "rg") || tokenEquals(token, "k")) {
    const uint8_t expectedCount = tokenEquals(token, "g") ? 1U : (tokenEquals(token, "rg") ? 3U : 4U);
    uint8_t luminance = 0;
    if (operandCount_ != expectedCount || !colorLuminance(workspace_.operands, expectedCount, &luminance)) {
      return failStatus(PdfError::Malformed);
    }
    graphics_.nonstrokingLuminance = luminance;
    return PdfStatus::success();
  }
  if (tokenEquals(token, "sc") || tokenEquals(token, "scn")) {
    uint8_t componentCount = operandCount_;
    if (tokenEquals(token, "scn") && componentCount != 0 &&
        workspace_.operands[componentCount - 1U].kind == PdfContentOperandKind::Name) {
      --componentCount;
    }
    uint8_t luminance = 0;
    if (colorLuminance(workspace_.operands, componentCount, &luminance)) {
      graphics_.nonstrokingLuminance = luminance;
    }
    return PdfStatus::success();
  }
  if (operandCount_ != 6) {
    return failStatus(PdfError::Malformed);
  }
  for (uint8_t index = 0; index < 6; ++index) {
    if (workspace_.operands[index].kind != PdfContentOperandKind::Number) {
      return failStatus(PdfError::Malformed);
    }
  }
  PdfMatrix combined;
  if (!concatenateWide(graphics_.ctm, workspace_.operands, &combined)) {
    return failStatus(PdfError::LimitExceeded);
  }
  graphics_.ctm = combined;
  return PdfStatus::success();
}

void PdfContentInterpreter::resetCurrentPath() {
  currentPathRectangle_ = {};
  currentPathRectangleValid_ = false;
  currentPathUnrepresentable_ = false;
}

PdfStatus PdfContentInterpreter::transformedGraphicsPoint(const PdfFixed16 x, const PdfFixed16 y,
                                                          PdfFixed16* const transformedX,
                                                          PdfFixed16* const transformedY) const {
  PdfFixed16 userX;
  PdfFixed16 userY;
  return pdfTransformPoint(graphics_.ctm, x, y, &userX, &userY) &&
                 pdfTransformPoint(workspace_.pageTransform, userX, userY, transformedX, transformedY)
             ? PdfStatus::success()
             : failStatus(PdfError::LimitExceeded);
}

PdfStatus PdfContentInterpreter::transformedAxisAlignedRectangle(const PdfRectangle& rectangle,
                                                                 PdfRectangle* const transformed,
                                                                 bool* const axisAligned) const {
  if (transformed == nullptr || axisAligned == nullptr) {
    return failStatus(PdfError::InvalidArgument);
  }
  const PdfFixed16 inputX[] = {{rectangle.xMin}, {rectangle.xMax}, {rectangle.xMin}, {rectangle.xMax}};
  const PdfFixed16 inputY[] = {{rectangle.yMin}, {rectangle.yMin}, {rectangle.yMax}, {rectangle.yMax}};
  PdfFixed16 outputX[4];
  PdfFixed16 outputY[4];
  for (uint8_t corner = 0; corner < 4; ++corner) {
    const PdfStatus status = transformedGraphicsPoint(inputX[corner], inputY[corner], &outputX[corner],
                                                      &outputY[corner]);
    if (!status.ok()) {
      return status;
    }
  }

  const bool firstEdgeHorizontal = outputY[0].raw == outputY[1].raw;
  const bool firstEdgeVertical = outputX[0].raw == outputX[1].raw;
  const bool secondEdgeHorizontal = outputY[0].raw == outputY[2].raw;
  const bool secondEdgeVertical = outputX[0].raw == outputX[2].raw;
  *axisAligned = (firstEdgeHorizontal && secondEdgeVertical) || (firstEdgeVertical && secondEdgeHorizontal);
  if (!*axisAligned) {
    return PdfStatus::success();
  }

  *transformed = {outputX[0].raw, outputY[0].raw, outputX[0].raw, outputY[0].raw};
  for (uint8_t corner = 1; corner < 4; ++corner) {
    transformed->xMin = std::min(transformed->xMin, outputX[corner].raw);
    transformed->xMax = std::max(transformed->xMax, outputX[corner].raw);
    transformed->yMin = std::min(transformed->yMin, outputY[corner].raw);
    transformed->yMax = std::max(transformed->yMax, outputY[corner].raw);
  }
  return PdfStatus::success();
}

PdfStatus PdfContentInterpreter::applyClipRectangle(const PdfRectangle& rectangle) {
  if (!graphics_.clipRepresentable) {
    return PdfStatus::success();
  }
  if (!graphics_.hasClip) {
    graphics_.clip = rectangle;
    graphics_.hasClip = true;
    return PdfStatus::success();
  }
  graphics_.clip.xMin = std::max(graphics_.clip.xMin, rectangle.xMin);
  graphics_.clip.yMin = std::max(graphics_.clip.yMin, rectangle.yMin);
  graphics_.clip.xMax = std::min(graphics_.clip.xMax, rectangle.xMax);
  graphics_.clip.yMax = std::min(graphics_.clip.yMax, rectangle.yMax);
  return PdfStatus::success();
}

PdfStatus PdfContentInterpreter::processPathOperator(const PdfToken& token) {
  pageModel_->addWarning(PdfPageWarning::VectorArtOmitted);
  if (tokenEquals(token, "re")) {
    if (operandCount_ != 4) {
      return failStatus(PdfError::Malformed);
    }
    for (uint8_t index = 0; index < 4; ++index) {
      if (workspace_.operands[index].kind != PdfContentOperandKind::Number) {
        return failStatus(PdfError::Malformed);
      }
    }
    if (currentPathRectangleValid_ || currentPathUnrepresentable_) {
      currentPathUnrepresentable_ = true;
      currentPathRectangleValid_ = false;
      return PdfStatus::success();
    }
    const PdfFixed16 x{workspace_.operands[0].number};
    const PdfFixed16 y{workspace_.operands[1].number};
    PdfFixed16 xEnd;
    PdfFixed16 yEnd;
    if (!pdfFixedAdd(x, {workspace_.operands[2].number}, &xEnd) ||
        !pdfFixedAdd(y, {workspace_.operands[3].number}, &yEnd)) {
      return failStatus(PdfError::LimitExceeded);
    }
    bool axisAligned = false;
    PdfStatus status = transformedAxisAlignedRectangle({x.raw, y.raw, xEnd.raw, yEnd.raw},
                                                       &currentPathRectangle_, &axisAligned);
    if (!status.ok()) {
      return status;
    }
    if (!axisAligned) {
      currentPathUnrepresentable_ = true;
      currentPathRectangleValid_ = false;
      return PdfStatus::success();
    }
    currentPathRectangleValid_ = true;
    return PdfStatus::success();
  }
  if (tokenEquals(token, "W") || tokenEquals(token, "W*")) {
    if (operandCount_ != 0) {
      return failStatus(PdfError::Malformed);
    }
    if (!currentPathRectangleValid_ || currentPathUnrepresentable_) {
      graphics_.clipRepresentable = false;
      return PdfStatus::success();
    }
    return applyClipRectangle(currentPathRectangle_);
  }
  if (tokenEquals(token, "n") || tokenEquals(token, "S") || tokenEquals(token, "s") || tokenEquals(token, "f") ||
      tokenEquals(token, "F") || tokenEquals(token, "f*") || tokenEquals(token, "B") || tokenEquals(token, "B*") ||
      tokenEquals(token, "b") || tokenEquals(token, "b*")) {
    resetCurrentPath();
    return PdfStatus::success();
  }
  currentPathUnrepresentable_ = true;
  currentPathRectangleValid_ = false;
  return PdfStatus::success();
}

PdfStatus PdfContentInterpreter::pushMarkedContent(const PdfContentOperand* const actualText, const bool suppress) {
  if (markedDepth_ >= static_cast<uint8_t>(sizeof(markedStack_) / sizeof(markedStack_[0]))) {
    return failStatus(PdfError::LimitExceeded);
  }
  MarkedContentFrame frame;
  frame.textOffset = markedTextLength_;
  if (suppress || markedContentSuppressed()) {
    frame.flags |= kMarkedContentSuppressed;
  }
  if (actualText != nullptr && (frame.flags & kMarkedContentSuppressed) == 0) {
    if (actualText->textLength > workspace_.markedTextCapacity - markedTextLength_) {
      return failStatus(PdfError::LimitExceeded);
    }
    frame.textLength = actualText->textLength;
    frame.hasActualText = true;
    std::memcpy(workspace_.markedText + markedTextLength_, workspace_.scratchText + actualText->textOffset,
                actualText->textLength);
    markedTextLength_ += actualText->textLength;
  }
  markedStack_[markedDepth_++] = frame;
  return PdfStatus::success();
}

bool PdfContentInterpreter::markedContentSuppressed() const {
  return markedDepth_ != 0 && (markedStack_[markedDepth_ - 1U].flags & kMarkedContentSuppressed) != 0;
}

PdfStatus PdfContentInterpreter::processMarkedContentOperator(const PdfToken& token) {
  if (tokenEquals(token, "BDC")) {
    if (operandCount_ < 2 || workspace_.operands[0].kind != PdfContentOperandKind::Name) {
      return failStatus(PdfError::Malformed);
    }
    const PdfContentOperand* actualText = nullptr;
    for (uint8_t index = 1; index < operandCount_; ++index) {
      if (workspace_.operands[index].kind == PdfContentOperandKind::ActualText) {
        actualText = &workspace_.operands[index];
      }
    }
    return pushMarkedContent(actualText, false);
  }
  if (tokenEquals(token, "BMC")) {
    if (operandCount_ != 1 || workspace_.operands[0].kind != PdfContentOperandKind::Name) {
      return failStatus(PdfError::Malformed);
    }
    return pushMarkedContent(nullptr, false);
  }
  if (tokenEquals(token, "EMC")) {
    if (operandCount_ != 0 || markedDepth_ == 0) {
      return failStatus(PdfError::Malformed);
    }
    --markedDepth_;
    markedTextLength_ = markedStack_[markedDepth_].textOffset;
    return PdfStatus::success();
  }
  return PdfStatus::success();
}

PdfStatus PdfContentInterpreter::processXObjectOperator(const PdfToken& token) {
  (void)token;
  if (operandCount_ != 1 || workspace_.operands[0].kind != PdfContentOperandKind::Name || resources_ == nullptr ||
      resources_->resolveXObject == nullptr) {
    return failStatus(PdfError::Malformed);
  }
  if (markedContentSuppressed() || !graphics_.visibilityRepresentable) {
    return PdfStatus::success();
  }
  const PdfContentOperand& name = workspace_.operands[0];
  PdfContentXObject object;
  const PdfStatus status = resources_->resolveXObject(resources_->context, workspace_.scratchText + name.textOffset,
                                                      name.textLength, &object);
  if (!status.ok()) {
    return status;
  }
  if (object.kind == PdfContentXObjectKind::Image && (object.pixelWidth == 0 || object.pixelHeight == 0)) {
    pageModel_->addWarning(PdfPageWarning::OptionalImageOmitted);
    return PdfStatus::success();
  }
  const PdfStatus objectStatus =
      object.kind == PdfContentXObjectKind::Form ? enterForm(object) : appendImage(object, false);
  return objectStatus;
}

PdfStatus PdfContentInterpreter::enterForm(const PdfContentXObject& form) {
  if (!form.hasBBox) {
    pageModel_->addWarning(PdfPageWarning::VectorArtOmitted);
    return PdfStatus::success();
  }
  if (!form.content.valid() || formDepth_ >= PdfLimits::MaxFormDepth) {
    pageModel_->addWarning(PdfPageWarning::VectorArtOmitted);
    return PdfStatus::success();
  }
  for (uint8_t index = 0; index < formDepth_; ++index) {
    if (form.reference == formStack_[index].formReference) {
      pageModel_->addWarning(PdfPageWarning::VectorArtOmitted);
      return PdfStatus::success();
    }
  }
  FormFrame& frame = formStack_[formDepth_];
  frame.source = currentSource_;
  frame.resumeOffset = lexer_.position();
  frame.resources = resources_;
  frame.graphics = graphics_;
  frame.text = text_;
  frame.formReference = form.reference;
  frame.markedTextLength = markedTextLength_;
  frame.graphicsDepth = graphicsDepth_;
  frame.markedDepth = markedDepth_;
  frame.topLevelSourceIndex = topLevelSourceIndex_;
  ++formDepth_;
  maximumFormDepth_ = std::max(maximumFormDepth_, formDepth_);

  currentSource_ = form.content;
  lexer_.setSource(currentSource_);
  resources_ = form.resources != nullptr ? form.resources : resources_;
  PdfMatrix combined;
  if (!concatenate(graphics_.ctm, form.matrix, &combined)) {
    return failStatus(PdfError::LimitExceeded);
  }
  graphics_.ctm = combined;
  PdfRectangle transformed;
  bool axisAligned = false;
  PdfStatus status = transformedAxisAlignedRectangle(form.bbox, &transformed, &axisAligned);
  if (!status.ok()) {
    return status;
  }
  if (!axisAligned) {
    graphics_.clipRepresentable = false;
    pageModel_->addWarning(PdfPageWarning::VectorArtOmitted);
  } else {
    status = applyClipRectangle(transformed);
    if (!status.ok()) {
      return status;
    }
  }
  text_ = {};
  resetCurrentPath();
  return PdfStatus::success();
}

PdfStatus PdfContentInterpreter::abandonCurrentForm() {
  if (formDepth_ == 0) {
    return failStatus(PdfError::InvalidArgument);
  }
  pageModel_->abortTextRun();
  clearOperands();
  arrayOpen_ = false;
  dictionaryDepth_ = 0;
  dictionaryCapturingActualText_ = false;
  dictionaryHasActualText_ = false;
  inlineDictionary_ = false;
  --formDepth_;
  const FormFrame& frame = formStack_[formDepth_];
  currentSource_ = frame.source;
  resources_ = frame.resources;
  graphics_ = frame.graphics;
  text_ = frame.text;
  markedTextLength_ = frame.markedTextLength;
  graphicsDepth_ = frame.graphicsDepth;
  markedDepth_ = frame.markedDepth;
  topLevelSourceIndex_ = frame.topLevelSourceIndex;
  lexer_.setSource(currentSource_, frame.resumeOffset);
  return PdfStatus::success();
}

PdfStatus PdfContentInterpreter::leaveFormOrAdvanceSource(bool* const complete) {
  if (complete == nullptr) {
    return failStatus(PdfError::InvalidArgument);
  }
  *complete = false;
  const bool unfinishedObject = arrayOpen_ || dictionaryDepth_ != 0 || inlineDictionary_ || operandCount_ != 0;
  if (formDepth_ != 0) {
    const FormFrame& activeFrame = formStack_[formDepth_ - 1];
    if (unfinishedObject || graphicsDepth_ != activeFrame.graphicsDepth || markedDepth_ != activeFrame.markedDepth ||
        text_.active) {
      return failStatus(PdfError::Malformed);
    }
    clearOperands();
    --formDepth_;
    const FormFrame& frame = formStack_[formDepth_];
    currentSource_ = frame.source;
    resources_ = frame.resources;
    graphics_ = frame.graphics;
    text_ = frame.text;
    markedTextLength_ = frame.markedTextLength;
    graphicsDepth_ = frame.graphicsDepth;
    markedDepth_ = frame.markedDepth;
    topLevelSourceIndex_ = frame.topLevelSourceIndex;
    lexer_.setSource(currentSource_, frame.resumeOffset);
    return PdfStatus::success();
  }
  if (topLevelSourceIndex_ + 1 < topLevelSourceCount_) {
    ++topLevelSourceIndex_;
    currentSource_ = topLevelSources_[topLevelSourceIndex_];
    lexer_.setSource(currentSource_);
    return PdfStatus::success();
  }
  if (unfinishedObject || graphicsDepth_ != 0 || markedDepth_ != 0 || text_.active) {
    return failStatus(PdfError::Malformed);
  }
  clearOperands();
  *complete = true;
  return PdfStatus::success();
}

PdfStatus PdfContentInterpreter::translateText(const PdfFixed16 x, const PdfFixed16 y, const bool lineMatrix) {
  if (lineMatrix) {
    if (!translate(&text_.lineMatrix, x, y)) {
      return failStatus(PdfError::LimitExceeded);
    }
    text_.matrix = text_.lineMatrix;
    return PdfStatus::success();
  }
  return translate(&text_.matrix, x, y) ? PdfStatus::success() : failStatus(PdfError::LimitExceeded);
}

PdfStatus PdfContentInterpreter::adjustText(const PdfFixed16 amount) { return translateText(amount, {}, false); }

PdfStatus PdfContentInterpreter::currentTextPoint(const PdfFixed16 textX, const PdfFixed16 textY, PdfFixed16* const x,
                                                  PdfFixed16* const y) const {
  PdfFixed16 transformedX;
  PdfFixed16 transformedY;
  PdfFixed16 userX;
  PdfFixed16 userY;
  if (!pdfTransformPoint(text_.matrix, textX, textY, &transformedX, &transformedY) ||
      !pdfTransformPoint(graphics_.ctm, transformedX, transformedY, &userX, &userY) ||
      !pdfTransformPoint(workspace_.pageTransform, userX, userY, x, y)) {
    return failStatus(PdfError::LimitExceeded);
  }
  return PdfStatus::success();
}

PdfStatus PdfContentInterpreter::currentTextOrigin(PdfFixed16* const x, PdfFixed16* const y) const {
  return currentTextPoint({}, text_.rise, x, y);
}

PdfStatus PdfContentInterpreter::currentTextAscent(PdfFixed16* const x, PdfFixed16* const y) const {
  PdfFixed16 ascent;
  if (!pdfFixedAdd(text_.rise, text_.fontSize, &ascent)) {
    return failStatus(PdfError::LimitExceeded);
  }
  return currentTextPoint({}, ascent, x, y);
}

PdfStatus PdfContentInterpreter::makeRun(PdfTextRun* const run, const uint16_t flags) const {
  if (run == nullptr) {
    return failStatus(PdfError::InvalidArgument);
  }
  PdfFixed16 x;
  PdfFixed16 y;
  const PdfStatus originStatus = currentTextOrigin(&x, &y);
  if (!originStatus.ok()) {
    return originStatus;
  }
  PdfFixed16 ascentX;
  PdfFixed16 ascentY;
  const PdfStatus ascentStatus = currentTextAscent(&ascentX, &ascentY);
  if (!ascentStatus.ok()) {
    return ascentStatus;
  }
  *run = {};
  run->xMin = std::min(x.raw, ascentX.raw);
  run->xMax = std::max(x.raw, ascentX.raw);
  run->yMin = std::min(y.raw, ascentY.raw);
  run->yMax = std::max(y.raw, ascentY.raw);
  run->baselineX = x.raw;
  run->baseline = y.raw;
  run->fontId = text_.font != nullptr ? text_.font->fontId() : 0;
  run->flags = flags;
  if (graphics_.nonstrokingLuminance >= 192U) {
    run->flags |= PdfTextLight;
  }
  if (text_.font != nullptr && text_.font->bold()) {
    run->flags |= PdfTextBold;
  }
  if (arrayPendingAdjustment_ != INT32_MAX) {
    // TJ values are in thousandths of text space. Large negative values add a
    // deliberate word gap; all other adjustments are glyph kerning.
    constexpr int32_t WORD_GAP_ADJUSTMENT = -100 * 65536;
    run->flags |= arrayPendingAdjustment_ <= WORD_GAP_ADJUSTMENT
                      ? PdfTextArrayExplicitGap
                      : PdfTextArrayTightContinuation;
  }
  if (text_.positionReset) {
    run->flags |= PdfTextPositionReset;
  }
  if ((flags & PdfTextActualText) != 0 || (text_.font != nullptr && text_.font->hasExplicitWhitespace())) {
    run->flags |= PdfTextExplicitWhitespace;
  }
  return PdfStatus::success();
}

PdfStatus PdfContentInterpreter::expandRunGeometry(const uint16_t runIndex) {
  PdfFixed16 x;
  PdfFixed16 y;
  PdfStatus status = currentTextOrigin(&x, &y);
  if (!status.ok()) {
    return status;
  }
  status = pageModel_->setTextRunBaselineEnd(runIndex, x.raw, y.raw);
  if (!status.ok()) {
    return status;
  }
  status = pageModel_->expandTextRunBounds(runIndex, x.raw, y.raw);
  if (!status.ok()) {
    return status;
  }
  status = currentTextAscent(&x, &y);
  if (!status.ok()) {
    return status;
  }
  return pageModel_->expandTextRunBounds(runIndex, x.raw, y.raw);
}

PdfStatus PdfContentInterpreter::advanceGlyph(const PdfDecodedGlyph& glyph) {
  PdfFixed16 width;
  if (!fixedFromProductRatio(text_.fontSize, glyph.width, 1000, &width) ||
      !pdfFixedAdd(width, text_.characterSpacing, &width)) {
    return failStatus(PdfError::LimitExceeded);
  }
  if (glyph.sourceCode == 0x20 && !pdfFixedAdd(width, text_.wordSpacing, &width)) {
    return failStatus(PdfError::LimitExceeded);
  }
  PdfFixed16 scaled;
  if (!pdfFixedMultiply(width, text_.horizontalScale, &scaled)) {
    return failStatus(PdfError::LimitExceeded);
  }
  return adjustText(scaled);
}

PdfStatus PdfContentInterpreter::advanceFallback(const size_t byteCount) {
  if (byteCount > static_cast<size_t>(INT32_MAX / 500)) {
    return failStatus(PdfError::LimitExceeded);
  }
  PdfFixed16 width;
  if (!fixedFromProductRatio(text_.fontSize, static_cast<int32_t>(byteCount * 500), 1000, &width)) {
    return failStatus(PdfError::LimitExceeded);
  }
  PdfFixed16 scaled;
  if (!pdfFixedMultiply(width, text_.horizontalScale, &scaled)) {
    return failStatus(PdfError::LimitExceeded);
  }
  return adjustText(scaled);
}

PdfStatus PdfContentInterpreter::pageTextWrite(void* const context, const uint8_t* const source, const size_t requested,
                                               size_t* const bytesWritten) {
  if (context == nullptr || source == nullptr || bytesWritten == nullptr) {
    return PdfStatus::failure(PdfError::InvalidArgument);
  }
  PdfPageModel& model = *static_cast<PdfPageModel*>(context);
  const PdfStatus status = model.appendText(source, requested);
  *bytesWritten = status.ok() ? requested : 0;
  return status;
}

PdfStatus PdfContentInterpreter::pageOverflowTextWrite(void* const context, const uint8_t* const source,
                                                       const size_t requested, size_t* const bytesWritten) {
  if (context == nullptr || source == nullptr || bytesWritten == nullptr) {
    return PdfStatus::failure(PdfError::InvalidArgument);
  }
  PdfPageModel& model = *static_cast<PdfPageModel*>(context);
  const PdfStatus status = model.appendOverflowText(source, requested);
  *bytesWritten = status.ok() ? requested : 0;
  return status;
}

PdfStatus PdfContentInterpreter::emitActualText(MarkedContentFrame& frame) {
  PdfTextRun run;
  const PdfStatus runStatus = makeRun(&run, PdfTextActualText | (text_.renderMode == 3 ? PdfTextHidden : 0));
  if (!runStatus.ok()) {
    return runStatus;
  }
  run.sourceOrder = sourceOrder_++;
  uint16_t runIndex = pageModel_->runCount();
  const bool overflow = runIndex >= pageModel_->runCapacity();
  PdfStatus status = overflow ? pageModel_->beginOverflowTextRun(run, &runIndex) : pageModel_->beginTextRun(run);
  if (!status.ok()) {
    if (overflow && status.error == PdfError::LimitExceeded) {
      textCapacityReached_ = true;
      frame.runIndex = UINT16_MAX;
      return PdfStatus::success();
    }
    return status;
  }
  const PdfByteSink sink{pageModel_, overflow ? pageOverflowTextWrite : pageTextWrite};
  status = pdfDecodePdfTextString(workspace_.markedText + frame.textOffset, frame.textLength, sink);
  if (!status.ok()) {
    if (!overflow) {
      pageModel_->abortTextRun();
    }
    return status;
  }
  frame.flags = static_cast<uint8_t>((frame.flags & ~kMarkedContentOverflow) |
                                     (overflow ? kMarkedContentOverflow : 0U));
  frame.runIndex = runIndex;
  return PdfStatus::success();
}

PdfStatus PdfContentInterpreter::finishSemanticTextRun() {
  const PdfTextRun* const run = pageModel_ == nullptr ? nullptr : pageModel_->pendingTextRun();
  if (run == nullptr) {
    return failStatus(PdfError::InvalidArgument);
  }
  const bool readableArea = run->xMax > run->xMin && run->yMax > run->yMin;
  const bool intersectsPage =
      !workspace_.hasPageBounds ||
      (run->xMax > workspace_.pageBounds.xMin && run->xMin < workspace_.pageBounds.xMax &&
       run->yMax > workspace_.pageBounds.yMin && run->yMin < workspace_.pageBounds.yMax);
  const bool intersectsClip =
      !graphics_.hasClip ||
      (run->xMax > graphics_.clip.xMin && run->xMin < graphics_.clip.xMax && run->yMax > graphics_.clip.yMin &&
       run->yMin < graphics_.clip.yMax);
  if (!readableArea || !intersectsPage || !graphics_.clipRepresentable || !intersectsClip) {
    pageModel_->abortTextRun();
    return PdfStatus::success();
  }
  return pageModel_->finishTextRun();
}

PdfStatus PdfContentInterpreter::emitDecodedText(const uint8_t* const source, const size_t length,
                                                 const bool actualText) {
  if (source == nullptr || length == 0 || text_.font == nullptr) {
    return failStatus(PdfError::UnsupportedEncoding);
  }
  PdfTextRun run;
  uint16_t flags = actualText ? PdfTextActualText : 0;
  if (text_.renderMode == 3) {
    flags |= PdfTextHidden;
  }
  const PdfStatus runStatus = makeRun(&run, flags);
  if (!runStatus.ok()) {
    return runStatus;
  }
  run.sourceOrder = sourceOrder_++;
  uint16_t runIndex = pageModel_->runCount();
  const bool overflow = runIndex >= pageModel_->runCapacity();
  PdfStatus status = overflow ? pageModel_->beginOverflowTextRun(run, &runIndex) : pageModel_->beginTextRun(run);
  if (!status.ok()) {
    if (overflow && status.error == PdfError::LimitExceeded) {
      textCapacityReached_ = true;
      return advanceVisualText(source, length);
    }
    return status;
  }
  size_t offset = 0;
  while (offset < length) {
    PdfDecodedGlyph glyph{};
    status = text_.font->decodeNext(source + offset, length - offset, &glyph);
    if (!status && (status.error == PdfError::UnsupportedEncoding || status.error == PdfError::LimitExceeded)) {
      glyph.sourceLength = static_cast<uint8_t>(std::min<size_t>(text_.font->cid() ? 2U : 1U, length - offset));
      glyph.sourceCode = 0;
      for (uint8_t index = 0; index < glyph.sourceLength; ++index) {
        glyph.sourceCode = (glyph.sourceCode << 8U) | source[offset + index];
      }
      // Keep layout movement but do not inject U+FFFD into reflowed text. A
      // missing PDF character map cannot be repaired by the device font.
      glyph.unicode.length = 0;
      glyph.width = 500;
      status = PdfStatus::success();
    }
    if (!status.ok() || glyph.sourceLength == 0 || glyph.sourceLength > length - offset) {
      if (!overflow) {
        pageModel_->abortTextRun();
      }
      return status.ok() ? failStatus(PdfError::Malformed) : status;
    }
    status = overflow ? pageModel_->appendOverflowText(glyph.unicode.bytes, glyph.unicode.length)
                      : pageModel_->appendText(glyph.unicode.bytes, glyph.unicode.length);
    if (!status.ok()) {
      if (overflow && status.error == PdfError::LimitExceeded) {
        textCapacityReached_ = true;
        status = advanceVisualText(source + offset, length - offset);
        if (!status) {
          return status;
        }
        break;
      }
      if (!overflow && status.error == PdfError::LimitExceeded) {
        pageModel_->abortTextRun();
        textCapacityReached_ = true;
        return advanceVisualText(source + offset, length - offset);
      }
      if (!overflow) {
        pageModel_->abortTextRun();
      }
      return status;
    }
    status = advanceGlyph(glyph);
    if (!status.ok()) {
      if (!overflow) {
        pageModel_->abortTextRun();
      }
      return status;
    }
    offset += glyph.sourceLength;
  }
  status = expandRunGeometry(runIndex);
  if (!status.ok()) {
    if (!overflow) {
      pageModel_->abortTextRun();
    }
    return status;
  }
  return overflow ? PdfStatus::success() : finishSemanticTextRun();
}

PdfStatus PdfContentInterpreter::advanceVisualText(const uint8_t* const source, const size_t length) {
  if (text_.font == nullptr) {
    return advanceFallback(length);
  }
  size_t offset = 0;
  while (offset < length) {
    PdfDecodedGlyph glyph;
    const PdfStatus decodeStatus = text_.font->decodeNext(source + offset, length - offset, &glyph);
    if (!decodeStatus.ok() || glyph.sourceLength == 0 || glyph.sourceLength > length - offset) {
      return advanceFallback(length - offset);
    }
    const PdfStatus advanceStatus = advanceGlyph(glyph);
    if (!advanceStatus.ok()) {
      return advanceStatus;
    }
    offset += glyph.sourceLength;
  }
  return PdfStatus::success();
}

PdfStatus PdfContentInterpreter::showString(const uint8_t* const source, const size_t length) {
  if (source == nullptr || !text_.active) {
    return failStatus(PdfError::Malformed);
  }
  if (length == 0) {
    return PdfStatus::success();
  }
  if (textCapacityReached_) {
    const PdfStatus status = advanceVisualText(source, length);
    if (status.ok()) {
      text_.positionReset = false;
    }
    return status;
  }
  if (text_.renderMode >= 4) {
    text_.clipPending = true;
  }
  if (text_.renderMode == 7 || markedContentSuppressed()) {
    const PdfStatus status = advanceVisualText(source, length);
    if (status.ok()) {
      text_.positionReset = false;
    }
    return status;
  }
  MarkedContentFrame* actual = nullptr;
  for (uint8_t depth = markedDepth_; depth-- > 0;) {
    if (markedStack_[depth].hasActualText) {
      actual = &markedStack_[depth];
      break;
    }
  }
  if (actual != nullptr) {
    if ((actual->flags & kMarkedContentEmitted) == 0) {
      const PdfStatus emitStatus = emitActualText(*actual);
      if (!emitStatus.ok()) {
        return emitStatus;
      }
      PdfStatus status = advanceVisualText(source, length);
      if (status.ok() && actual->runIndex != UINT16_MAX) {
        status = expandRunGeometry(actual->runIndex);
        if (status.ok() && (actual->flags & kMarkedContentOverflow) == 0) {
          status = finishSemanticTextRun();
        }
      }
      if (!status.ok()) {
        return status;
      }
      actual->flags |= kMarkedContentEmitted;
      if (pageModel_->runCount() <= actual->runIndex) {
        actual->runIndex = UINT16_MAX;
      }
      text_.positionReset = false;
      return PdfStatus::success();
    }
    PdfStatus status = advanceVisualText(source, length);
    if (!status.ok() || actual->runIndex == UINT16_MAX) {
      return status;
    }
    status = expandRunGeometry(actual->runIndex);
    if (status.ok()) {
      text_.positionReset = false;
    }
    return status;
  }
  const PdfStatus status = emitDecodedText(source, length, false);
  if (status.ok()) {
    text_.positionReset = false;
  }
  return status;
}

PdfStatus PdfContentInterpreter::showArray(const PdfContentOperand& array) {
  if (array.firstItem > arrayItemCount_ || array.itemCount > arrayItemCount_ - array.firstItem) {
    return failStatus(PdfError::Malformed);
  }
  for (uint16_t ordinal = 0; ordinal < array.itemCount; ++ordinal) {
    const PdfContentArrayItem& item = workspace_.arrayItems[array.firstItem + ordinal];
    if (item.kind == PdfContentOperandKind::String) {
      const PdfStatus status = showString(workspace_.scratchText + item.textOffset, item.textLength);
      if (!status.ok()) {
        return status;
      }
      arrayPendingAdjustment_ = 0;
    } else {
      if (arrayPendingAdjustment_ != INT32_MAX) {
        const int64_t combined = static_cast<int64_t>(arrayPendingAdjustment_) + item.number;
        arrayPendingAdjustment_ =
            static_cast<int32_t>(std::max<int64_t>(INT32_MIN, std::min<int64_t>(INT32_MAX - 1LL, combined)));
      }
      const int64_t raw = -(static_cast<int64_t>(text_.fontSize.raw) * item.number) / (1000LL * 65536LL);
      if (raw < INT32_MIN || raw > INT32_MAX) {
        return failStatus(PdfError::LimitExceeded);
      }
      const PdfFixed16 adjustment{static_cast<int32_t>(raw)};
      PdfFixed16 scaled;
      if (!pdfFixedMultiply(adjustment, text_.horizontalScale, &scaled)) {
        return failStatus(PdfError::LimitExceeded);
      }
      const PdfStatus status = adjustText(scaled);
      if (!status.ok()) {
        return status;
      }
    }
  }
  return PdfStatus::success();
}

PdfStatus PdfContentInterpreter::flushTextArrayChunk() {
  if (arrayStart_ > arrayItemCount_) {
    return failStatus(PdfError::Malformed);
  }
  if (arrayItemCount_ != arrayStart_) {
    PdfContentOperand chunk;
    chunk.kind = PdfContentOperandKind::Array;
    chunk.firstItem = arrayStart_;
    chunk.itemCount = static_cast<uint16_t>(arrayItemCount_ - arrayStart_);
    const PdfStatus status = showArray(chunk);
    if (!status.ok()) {
      return status;
    }
  }
  arrayItemCount_ = 0;
  arrayStart_ = 0;
  scratchTextLength_ = 0;
  arrayHasString_ = false;
  return PdfStatus::success();
}

PdfStatus PdfContentInterpreter::appendImage(const PdfContentXObject& image, const bool inlineImage) {
  if (markedContentSuppressed() || !graphics_.visibilityRepresentable) {
    return PdfStatus::success();
  }
  const PdfFixed16 one = PdfFixed16::fromInteger(1);
  const PdfFixed16 xInputs[] = {{}, one, {}, one};
  const PdfFixed16 yInputs[] = {{}, {}, one, one};
  PdfFixed16 x;
  PdfFixed16 y;
  PdfStatus status = transformedGraphicsPoint(xInputs[0], yInputs[0], &x, &y);
  if (!status.ok()) {
    return status;
  }
  PdfImagePlacement placement;
  placement.reference = image.reference;
  placement.sourceOrder = sourceOrder_++;
  placement.pixelWidth = image.pixelWidth;
  placement.pixelHeight = image.pixelHeight;
  placement.xMin = x.raw;
  placement.xMax = x.raw;
  placement.yMin = y.raw;
  placement.yMax = y.raw;
  for (uint8_t corner = 1; corner < 4; ++corner) {
    status = transformedGraphicsPoint(xInputs[corner], yInputs[corner], &x, &y);
    if (!status.ok()) {
      return status;
    }
    placement.xMin = std::min(placement.xMin, x.raw);
    placement.xMax = std::max(placement.xMax, x.raw);
    placement.yMin = std::min(placement.yMin, y.raw);
    placement.yMax = std::max(placement.yMax, y.raw);
  }
  placement.flags = inlineImage ? PdfImageInline : 0;
  placement.imageMaskPaintLuminance = graphics_.nonstrokingLuminance;
  const bool intersectsPage =
      !workspace_.hasPageBounds ||
      (placement.xMax > workspace_.pageBounds.xMin && placement.xMin < workspace_.pageBounds.xMax &&
       placement.yMax > workspace_.pageBounds.yMin && placement.yMin < workspace_.pageBounds.yMax);
  const bool intersectsClip =
      !graphics_.hasClip ||
      (placement.xMax > graphics_.clip.xMin && placement.xMin < graphics_.clip.xMax &&
       placement.yMax > graphics_.clip.yMin && placement.yMin < graphics_.clip.yMax);
  if (!graphics_.clipRepresentable || !graphics_.visibilityRepresentable || !intersectsPage || !intersectsClip) {
    pageModel_->addWarning(PdfPageWarning::OptionalImageOmitted);
    return PdfStatus::success();
  }
  return pageModel_->appendImage(placement);
}
