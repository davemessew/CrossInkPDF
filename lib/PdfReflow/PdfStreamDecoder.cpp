#include "PdfStreamDecoder.h"

#include <algorithm>
#include <cstring>
#include <limits>

#include "PdfCheckedMath.h"
#include "PdfObjectParser.h"

namespace {

constexpr size_t MIN_DECODER_BUFFER_BYTES = 128;
constexpr size_t MIN_INFLATE_INPUT_BYTES = 64;
constexpr size_t INFLATE_INPUT_DIVISOR = 2;
constexpr size_t INFLATE_OUTPUT_SAFETY_BYTES = 64;

bool isWhitespace(const uint8_t byte) {
  return byte == 0 || byte == '\t' || byte == '\n' || byte == '\f' || byte == '\r' || byte == ' ';
}

int hexValue(const uint8_t byte) {
  if (byte >= '0' && byte <= '9') {
    return byte - '0';
  }
  if (byte >= 'A' && byte <= 'F') {
    return byte - 'A' + 10;
  }
  if (byte >= 'a' && byte <= 'f') {
    return byte - 'a' + 10;
  }
  return -1;
}

bool textEquals(const PdfObjectArena& arena, const PdfValue& value, const char* expected) {
  return value.kind == PdfValueKind::Name && pdfTextEquals(arena, value, expected);
}

PdfStreamFilter filterFromName(const PdfObjectArena& arena, const PdfValue& value) {
  if (textEquals(arena, value, "ASCIIHexDecode") || textEquals(arena, value, "AHx")) {
    return PdfStreamFilter::ASCIIHex;
  }
  if (textEquals(arena, value, "ASCII85Decode") || textEquals(arena, value, "A85")) {
    return PdfStreamFilter::ASCII85;
  }
  if (textEquals(arena, value, "FlateDecode") || textEquals(arena, value, "Fl")) {
    return PdfStreamFilter::Flate;
  }
  if (textEquals(arena, value, "LZWDecode") || textEquals(arena, value, "LZW")) {
    return PdfStreamFilter::Lzw;
  }
  return PdfStreamFilter::Unsupported;
}

bool dictionaryKeyEquals(const PdfObjectArena& arena, const PdfDictionaryEntry& entry, const char* expected) {
  const size_t expectedLength = std::strlen(expected);
  return expectedLength == entry.keyLength &&
         static_cast<uint32_t>(entry.keyOffset) + entry.keyLength <= arena.textLength &&
         std::memcmp(arena.text + entry.keyOffset, expected, expectedLength) == 0;
}

PdfStatus validateDefaultDecodeParameters(const PdfObjectArena& arena, const uint16_t dictionaryIndex,
                                          const PdfStreamFilter filter) {
  if (dictionaryIndex >= arena.valueCount || arena.values[dictionaryIndex].kind != PdfValueKind::Dictionary) {
    return PdfStatus::failure(PdfError::Malformed, dictionaryIndex);
  }
  const PdfValue& dictionary = arena.values[dictionaryIndex];
  if (dictionary.count == 0) {
    return PdfStatus::success();
  }
  if (filter != PdfStreamFilter::Flate) {
    return PdfStatus::failure(PdfError::UnsupportedFilter, dictionaryIndex);
  }
  uint16_t entryIndex = dictionary.firstLink;
  uint8_t seenKeys = 0;
  for (uint16_t ordinal = 0; ordinal < dictionary.count; ++ordinal) {
    if (entryIndex >= arena.dictionaryCount) {
      return PdfStatus::failure(PdfError::Malformed, dictionaryIndex);
    }
    const PdfDictionaryEntry& entry = arena.dictionaryEntries[entryIndex];
    if (entry.valueIndex >= arena.valueCount) {
      return PdfStatus::failure(PdfError::Malformed, dictionaryIndex);
    }

    uint8_t keyMask = 0;
    if (dictionaryKeyEquals(arena, entry, "Predictor")) {
      keyMask = 1U << 0;
    } else if (dictionaryKeyEquals(arena, entry, "Colors")) {
      keyMask = 1U << 1;
    } else if (dictionaryKeyEquals(arena, entry, "BitsPerComponent")) {
      keyMask = 1U << 2;
    } else if (dictionaryKeyEquals(arena, entry, "Columns")) {
      keyMask = 1U << 3;
    } else {
      return PdfStatus::failure(PdfError::UnsupportedFilter, dictionaryIndex);
    }
    if ((seenKeys & keyMask) != 0) {
      return PdfStatus::failure(PdfError::Malformed, dictionaryIndex);
    }
    seenKeys = static_cast<uint8_t>(seenKeys | keyMask);

    const PdfValue& parameter = arena.values[entry.valueIndex];
    if (parameter.kind != PdfValueKind::Integer || parameter.integerValue <= 0) {
      return PdfStatus::failure(PdfError::Malformed, entry.valueIndex);
    }
    if (keyMask == (1U << 0) && parameter.integerValue != 1) {
      return PdfStatus::failure(PdfError::UnsupportedFilter, static_cast<uint64_t>(parameter.integerValue));
    }
    entryIndex = entry.next;
  }
  return PdfStatus::success();
}

PdfStatus validateDecodeParameters(const PdfObjectArena& arena, const uint16_t dictionaryIndex,
                                   const PdfStreamFilter* filters, const uint8_t filterCount) {
  uint16_t parametersIndex = PDF_INVALID_INDEX;
  if (!pdfDictionaryFind(arena, dictionaryIndex, "DecodeParms", &parametersIndex)) {
    return PdfStatus::success();
  }
  if (parametersIndex >= arena.valueCount) {
    return PdfStatus::failure(PdfError::Malformed, dictionaryIndex);
  }
  const PdfValue& parameters = arena.values[parametersIndex];
  if (parameters.kind == PdfValueKind::Null) {
    return PdfStatus::success();
  }
  if (filterCount == 0) {
    return PdfStatus::failure(PdfError::Malformed, parametersIndex);
  }
  if (parameters.kind == PdfValueKind::Dictionary) {
    return filterCount == 1 ? validateDefaultDecodeParameters(arena, parametersIndex, filters[0])
                            : PdfStatus::failure(PdfError::Malformed, parametersIndex);
  }
  if (parameters.kind != PdfValueKind::Array || parameters.count != filterCount) {
    return PdfStatus::failure(PdfError::Malformed, parametersIndex);
  }
  for (uint8_t ordinal = 0; ordinal < filterCount; ++ordinal) {
    uint16_t itemIndex = PDF_INVALID_INDEX;
    if (!pdfArrayAt(arena, parametersIndex, ordinal, &itemIndex) || itemIndex >= arena.valueCount) {
      return PdfStatus::failure(PdfError::Malformed, parametersIndex);
    }
    if (arena.values[itemIndex].kind == PdfValueKind::Null) {
      continue;
    }
    const PdfStatus status = validateDefaultDecodeParameters(arena, itemIndex, filters[ordinal]);
    if (!status.ok()) {
      return status;
    }
  }
  return PdfStatus::success();
}

}  // namespace

PdfStreamDecoder::PdfStreamDecoder(const PdfStreamDecoderWorkspace workspace) : workspace_(workspace) {
  inflateContext_.owner = this;
}

PdfStatus PdfStreamDecoder::begin(const PdfByteSource& source, const PdfByteSink& sink, const PdfStreamFilter* filters,
                                  const uint8_t filterCount, const PdfStreamDecodeLimits limits, const bool required) {
  source_ = {};
  sink_ = {};
  limits_ = limits;
  phase_ = Phase::Idle;
  sourceBufferPosition_ = 0;
  sourceBufferLength_ = 0;
  pendingOutputLength_ = 0;
  pendingOutputWritten_ = 0;
  inflateInputLength_ = 0;
  sourceOffset_ = 0;
  inputBytes_ = 0;
  outputBytes_ = 0;
  filterCount_ = 0;
  preFlateStages_ = 0;
  zlibHeaderLength_ = 0;
  hasFlate_ = false;
  inflateInputAtEnd_ = false;
  finishAfterFlush_ = false;
  omitted_ = false;
  callbackStatus_ = PdfStatus::success();
  activeBudget_ = nullptr;
  for (FilterState& state : filterStates_) {
    state = {};
  }

  if (!source.valid() || !sink.valid() || workspace_.sourceBuffer == nullptr || workspace_.sourceBufferSize == 0 ||
      workspace_.sourceBufferSize > PdfLimits::SourceBufferBytes || workspace_.outputBuffer == nullptr ||
      workspace_.outputBufferSize < MIN_DECODER_BUFFER_BYTES ||
      workspace_.outputBufferSize > PdfLimits::DecoderOutputBytes || limits.maxExpandedBytes == 0 ||
      limits.maxExpansionRatio == 0 || (filterCount != 0 && filters == nullptr)) {
    phase_ = Phase::Failed;
    return PdfStatus::failure(PdfError::InvalidArgument);
  }
  if (filterCount > PdfLimits::MaxFiltersPerStream) {
    phase_ = Phase::Failed;
    return PdfStatus::failure(PdfError::LimitExceeded, filterCount);
  }

  for (uint8_t index = 0; index < filterCount; ++index) {
    const PdfStreamFilter filter = filters[index];
    if (filter == PdfStreamFilter::Lzw || filter == PdfStreamFilter::Unsupported) {
      if (!required) {
        omitted_ = true;
        phase_ = Phase::Done;
        return PdfStatus::success();
      }
      phase_ = Phase::Failed;
      return PdfStatus::failure(PdfError::UnsupportedFilter, index);
    }
    if (filter == PdfStreamFilter::Flate) {
      if (hasFlate_ || index + 1 != filterCount) {
        phase_ = Phase::Failed;
        return PdfStatus::failure(PdfError::UnsupportedFilter, index);
      }
      hasFlate_ = true;
    }
    filters_[index] = filter;
  }

  source_ = source;
  sink_ = sink;
  filterCount_ = filterCount;
  preFlateStages_ = hasFlate_ ? static_cast<uint8_t>(filterCount - 1) : filterCount;
  if (hasFlate_) {
    if (workspace_.inflateDictionary == nullptr || workspace_.inflateDictionarySize < PdfLimits::UzlibDictionaryBytes) {
      phase_ = Phase::Failed;
      return PdfStatus::failure(PdfError::InvalidArgument);
    }
    inflateInputCapacity_ = workspace_.outputBufferSize / INFLATE_INPUT_DIVISOR;
    if (inflateInputCapacity_ < MIN_INFLATE_INPUT_BYTES ||
        workspace_.outputBufferSize - inflateInputCapacity_ < MIN_INFLATE_INPUT_BYTES) {
      phase_ = Phase::Failed;
      return PdfStatus::failure(PdfError::InvalidArgument);
    }
    finalOutputOffset_ = inflateInputCapacity_;
    finalOutputCapacity_ = workspace_.outputBufferSize - inflateInputCapacity_;
    finalOutputCapacity_ = std::min(finalOutputCapacity_, inflateInputCapacity_ - INFLATE_OUTPUT_SAFETY_BYTES);
    if (!inflateContext_.reader.initWithExternalDictionary(workspace_.inflateDictionary,
                                                           workspace_.inflateDictionarySize)) {
      phase_ = Phase::Failed;
      return PdfStatus::failure(PdfError::InsufficientMemory);
    }
    inflateInitialized_ = true;
    inflateContext_.reader.setReadCallback(inflateReadCallback);
    phase_ = Phase::ZlibHeader;
  } else {
    inflateContext_.reader.deinit();
    inflateInitialized_ = false;
    inflateInputCapacity_ = 0;
    finalOutputOffset_ = 0;
    finalOutputCapacity_ = workspace_.outputBufferSize;
    phase_ = Phase::Decode;
  }
  return PdfStatus::success();
}

PdfStepResult PdfStreamDecoder::step(PdfWorkBudget& budget) {
  if (phase_ == Phase::Done) {
    return PdfStepResult::completed();
  }
  if (phase_ == Phase::Idle || phase_ == Phase::Failed) {
    return PdfStepResult::failure(PdfStatus::failure(PdfError::Malformed, sourceOffset_));
  }
  if (budget.cancelRequested()) {
    return fail(PdfStatus::failure(PdfError::Cancelled, sourceOffset_));
  }
  activeBudget_ = &budget;

  while (true) {
    if (phase_ == Phase::Flush) {
      const PullResult flushResult = flushPending(budget);
      if (flushResult.state == PullState::Yielded) {
        activeBudget_ = nullptr;
        return PdfStepResult::paused();
      }
      if (flushResult.state == PullState::Failed) {
        activeBudget_ = nullptr;
        return fail(flushResult.status);
      }
      phase_ = finishAfterFlush_ ? Phase::Done : Phase::Decode;
      if (phase_ == Phase::Done) {
        activeBudget_ = nullptr;
        return PdfStepResult::completed();
      }
      continue;
    }

    if (phase_ == Phase::ZlibHeader) {
      while (zlibHeaderLength_ < sizeof(zlibHeader_)) {
        uint8_t byte = 0;
        const PullResult result = pull(preFlateStages_, &byte);
        if (result.state == PullState::Yielded) {
          activeBudget_ = nullptr;
          return PdfStepResult::paused();
        }
        if (result.state == PullState::Failed) {
          activeBudget_ = nullptr;
          return fail(result.status);
        }
        if (result.state == PullState::End) {
          activeBudget_ = nullptr;
          return fail(PdfStatus::failure(PdfError::UnexpectedEof, inputBytes_));
        }
        zlibHeader_[zlibHeaderLength_++] = byte;
      }
      const uint16_t header = static_cast<uint16_t>(zlibHeader_[0]) << 8 | zlibHeader_[1];
      if ((zlibHeader_[0] & 0x0f) != 8 || (zlibHeader_[0] >> 4) > 7 || header % 31 != 0 ||
          (zlibHeader_[1] & 0x20) != 0) {
        activeBudget_ = nullptr;
        return fail(PdfStatus::failure(PdfError::Malformed, 0));
      }
      uzlib_uncomp* inflate = inflateContext_.reader.raw();
      inflate->checksum_type = TINF_CHKSUM_ADLER;
      inflate->checksum = 1;
      phase_ = Phase::Decode;
      continue;
    }

    if (!hasFlate_) {
      pendingOutputLength_ = 0;
      pendingOutputWritten_ = 0;
      finishAfterFlush_ = false;
      while (pendingOutputLength_ < finalOutputCapacity_) {
        uint8_t byte = 0;
        const PullResult result = pull(filterCount_, &byte);
        if (result.state == PullState::Byte) {
          const PdfStatus growth = validateGrowth(1);
          if (!growth.ok()) {
            activeBudget_ = nullptr;
            return fail(growth);
          }
          workspace_.outputBuffer[pendingOutputLength_++] = byte;
          continue;
        }
        if (result.state == PullState::Failed) {
          activeBudget_ = nullptr;
          return fail(result.status);
        }
        if (result.state == PullState::End) {
          finishAfterFlush_ = true;
        }
        if (pendingOutputLength_ == 0) {
          activeBudget_ = nullptr;
          if (result.state == PullState::Yielded) {
            return PdfStepResult::paused();
          }
          phase_ = Phase::Done;
          return PdfStepResult::completed();
        }
        phase_ = Phase::Flush;
        break;
      }
      if (pendingOutputLength_ == finalOutputCapacity_) {
        phase_ = Phase::Flush;
      }
      continue;
    }

    while (!inflateInputAtEnd_ && inflateInputLength_ < inflateInputCapacity_) {
      uint8_t byte = 0;
      const PullResult result = pull(preFlateStages_, &byte);
      if (result.state == PullState::Byte) {
        workspace_.outputBuffer[inflateInputLength_++] = byte;
        continue;
      }
      if (result.state == PullState::End) {
        inflateInputAtEnd_ = true;
        break;
      }
      if (result.state == PullState::Yielded) {
        activeBudget_ = nullptr;
        return PdfStepResult::paused();
      }
      activeBudget_ = nullptr;
      return fail(result.status);
    }
    if (inflateInputLength_ == 0) {
      activeBudget_ = nullptr;
      return fail(PdfStatus::failure(PdfError::UnexpectedEof, inputBytes_));
    }
    if (!budget.consumeOperation()) {
      activeBudget_ = nullptr;
      return PdfStepResult::paused();
    }

    uzlib_uncomp* inflate = inflateContext_.reader.raw();
    inflate->source = workspace_.outputBuffer;
    inflate->source_limit = workspace_.outputBuffer + inflateInputLength_;
    inflate->eof = false;
    uint8_t* destination = workspace_.outputBuffer + finalOutputOffset_;
    inflate->dest = destination;
    inflate->dest_limit = destination + finalOutputCapacity_;
    callbackStatus_ = PdfStatus::success();
    const int inflateResult = uzlib_uncompress_chksum(inflate);
    const size_t produced = static_cast<size_t>(inflate->dest - destination);
    const size_t consumed = static_cast<size_t>(inflate->source - workspace_.outputBuffer);
    if (consumed > inflateInputLength_) {
      activeBudget_ = nullptr;
      return fail(PdfStatus::failure(PdfError::Malformed, inputBytes_));
    }
    const size_t remaining = inflateInputLength_ - consumed;
    if (remaining != 0) {
      std::memmove(workspace_.outputBuffer, workspace_.outputBuffer + consumed, remaining);
    }
    inflateInputLength_ = remaining;

    if (!callbackStatus_.ok()) {
      activeBudget_ = nullptr;
      if (callbackStatus_.error == PdfError::BudgetExhausted) {
        return fail(PdfStatus::failure(PdfError::ExpansionLimit, inputBytes_));
      }
      return fail(callbackStatus_);
    }
    if (inflateResult < 0 || (inflateResult == TINF_DONE && inflate->eof)) {
      activeBudget_ = nullptr;
      return fail(PdfStatus::failure(inflate->eof ? PdfError::UnexpectedEof : PdfError::Malformed, inputBytes_));
    }
    if (produced == 0 && inflateResult != TINF_DONE) {
      activeBudget_ = nullptr;
      return fail(
          PdfStatus::failure(inflateInputAtEnd_ ? PdfError::UnexpectedEof : PdfError::ExpansionLimit, inputBytes_));
    }
    const PdfStatus growth = validateGrowth(produced);
    if (!growth.ok()) {
      activeBudget_ = nullptr;
      return fail(growth);
    }
    pendingOutputLength_ = produced;
    pendingOutputWritten_ = 0;
    finishAfterFlush_ = inflateResult == TINF_DONE;
    if (pendingOutputLength_ == 0) {
      phase_ = Phase::Done;
      activeBudget_ = nullptr;
      return PdfStepResult::completed();
    }
    phase_ = Phase::Flush;
  }
}

PdfStreamDecoder::PullResult PdfStreamDecoder::pull(const uint8_t stage, uint8_t* byte) {
  if (byte == nullptr) {
    return {PullState::Failed, PdfStatus::failure(PdfError::InvalidArgument, sourceOffset_)};
  }
  if (stage == 0) {
    return pullRaw(byte);
  }
  if (stage > filterCount_) {
    return {PullState::Failed, PdfStatus::failure(PdfError::Malformed, sourceOffset_)};
  }
  switch (filters_[stage - 1]) {
    case PdfStreamFilter::ASCIIHex:
      return pullAsciiHex(stage, byte);
    case PdfStreamFilter::ASCII85:
      return pullAscii85(stage, byte);
    default:
      return {PullState::Failed, PdfStatus::failure(PdfError::UnsupportedFilter, stage - 1)};
  }
}

PdfStreamDecoder::PullResult PdfStreamDecoder::pullRaw(uint8_t* byte) {
  if (sourceBufferPosition_ < sourceBufferLength_) {
    *byte = workspace_.sourceBuffer[sourceBufferPosition_++];
    ++inputBytes_;
    return {PullState::Byte, PdfStatus::success()};
  }
  if (sourceOffset_ >= source_.size) {
    return {PullState::End, PdfStatus::success()};
  }
  if (activeBudget_ == nullptr) {
    return {PullState::Failed, PdfStatus::failure(PdfError::InvalidArgument, sourceOffset_)};
  }
  if (activeBudget_->cancelRequested()) {
    return {PullState::Failed, PdfStatus::failure(PdfError::Cancelled, sourceOffset_)};
  }
  if (!activeBudget_->consumeOperation()) {
    return {PullState::Yielded, PdfStatus::failure(PdfError::BudgetExhausted, sourceOffset_)};
  }
  const uint64_t remaining64 = source_.size - sourceOffset_;
  const size_t remaining = remaining64 > std::numeric_limits<size_t>::max() ? std::numeric_limits<size_t>::max()
                                                                            : static_cast<size_t>(remaining64);
  const size_t requested = activeBudget_->takeBytes(std::min(workspace_.sourceBufferSize, remaining));
  if (requested == 0) {
    return {PullState::Yielded, PdfStatus::failure(PdfError::BudgetExhausted, sourceOffset_)};
  }
  size_t bytesRead = 0;
  const PdfStatus status =
      source_.readAt(source_.context, sourceOffset_, workspace_.sourceBuffer, requested, &bytesRead);
  if (!status.ok()) {
    return {PullState::Failed, status};
  }
  if (bytesRead == 0 || bytesRead > requested) {
    return {PullState::Failed,
            PdfStatus::failure(bytesRead == 0 ? PdfError::UnexpectedEof : PdfError::IoFailure, sourceOffset_)};
  }
  sourceOffset_ += bytesRead;
  sourceBufferLength_ = bytesRead;
  sourceBufferPosition_ = 1;
  *byte = workspace_.sourceBuffer[0];
  ++inputBytes_;
  return {PullState::Byte, PdfStatus::success()};
}

PdfStreamDecoder::PullResult PdfStreamDecoder::pullAsciiHex(const uint8_t stage, uint8_t* byte) {
  FilterState& state = filterStates_[stage - 1];
  if (state.outputPosition < state.outputLength) {
    *byte = state.output[state.outputPosition++];
    return {PullState::Byte, PdfStatus::success()};
  }
  state.outputPosition = 0;
  state.outputLength = 0;
  if (state.terminated) {
    return {PullState::End, PdfStatus::success()};
  }
  while (true) {
    uint8_t input = 0;
    const PullResult result = pull(stage - 1, &input);
    if (result.state == PullState::End) {
      return {PullState::Failed, PdfStatus::failure(PdfError::UnexpectedEof, inputBytes_)};
    }
    if (result.state != PullState::Byte) {
      return result;
    }
    if (isWhitespace(input)) {
      continue;
    }
    if (input == '>') {
      state.terminated = true;
      if (!state.hasHighNibble) {
        return {PullState::End, PdfStatus::success()};
      }
      state.hasHighNibble = false;
      *byte = static_cast<uint8_t>(state.highNibble << 4);
      return {PullState::Byte, PdfStatus::success()};
    }
    const int nibble = hexValue(input);
    if (nibble < 0) {
      return {PullState::Failed, PdfStatus::failure(PdfError::Malformed, inputBytes_ - 1)};
    }
    if (!state.hasHighNibble) {
      state.highNibble = static_cast<uint8_t>(nibble);
      state.hasHighNibble = true;
      continue;
    }
    state.hasHighNibble = false;
    *byte = static_cast<uint8_t>((state.highNibble << 4) | nibble);
    return {PullState::Byte, PdfStatus::success()};
  }
}

PdfStreamDecoder::PullResult PdfStreamDecoder::pullAscii85(const uint8_t stage, uint8_t* byte) {
  FilterState& state = filterStates_[stage - 1];
  if (state.outputPosition < state.outputLength) {
    *byte = state.output[state.outputPosition++];
    return {PullState::Byte, PdfStatus::success()};
  }
  state.outputPosition = 0;
  state.outputLength = 0;
  if (state.terminated) {
    return {PullState::End, PdfStatus::success()};
  }

  while (true) {
    uint8_t input = 0;
    const PullResult result = pull(stage - 1, &input);
    if (result.state == PullState::End) {
      return {PullState::Failed, PdfStatus::failure(PdfError::UnexpectedEof, inputBytes_)};
    }
    if (result.state != PullState::Byte) {
      return result;
    }
    if (isWhitespace(input)) {
      continue;
    }
    if (state.pendingTerminator) {
      if (input != '>') {
        return {PullState::Failed, PdfStatus::failure(PdfError::Malformed, inputBytes_ - 1)};
      }
      state.pendingTerminator = false;
      state.terminated = true;
      if (state.groupLength == 0) {
        return {PullState::End, PdfStatus::success()};
      }
      if (state.groupLength == 1) {
        return {PullState::Failed, PdfStatus::failure(PdfError::Malformed, inputBytes_ - 1)};
      }
      const uint8_t originalLength = state.groupLength;
      while (state.groupLength < 5) {
        state.group[state.groupLength++] = 84;
      }
      uint64_t value = 0;
      for (uint8_t index = 0; index < 5; ++index) {
        value = value * 85 + state.group[index];
      }
      if (value > UINT32_MAX) {
        return {PullState::Failed, PdfStatus::failure(PdfError::Malformed, inputBytes_ - 1)};
      }
      const uint32_t word = static_cast<uint32_t>(value);
      for (uint8_t index = 0; index < 4; ++index) {
        state.output[index] = static_cast<uint8_t>(word >> (24 - index * 8));
      }
      state.outputLength = static_cast<uint8_t>(originalLength - 1);
      state.outputPosition = 1;
      *byte = state.output[0];
      return {PullState::Byte, PdfStatus::success()};
    }
    if (input == '~') {
      state.pendingTerminator = true;
      continue;
    }
    if (input == 'z') {
      if (state.groupLength != 0) {
        return {PullState::Failed, PdfStatus::failure(PdfError::Malformed, inputBytes_ - 1)};
      }
      state.outputLength = 4;
      state.outputPosition = 1;
      std::memset(state.output, 0, sizeof(state.output));
      *byte = 0;
      return {PullState::Byte, PdfStatus::success()};
    }
    if (input < '!' || input > 'u') {
      return {PullState::Failed, PdfStatus::failure(PdfError::Malformed, inputBytes_ - 1)};
    }
    state.group[state.groupLength++] = static_cast<uint8_t>(input - '!');
    if (state.groupLength != 5) {
      continue;
    }
    uint64_t value = 0;
    for (uint8_t index = 0; index < 5; ++index) {
      value = value * 85 + state.group[index];
    }
    if (value > UINT32_MAX) {
      return {PullState::Failed, PdfStatus::failure(PdfError::Malformed, inputBytes_ - 1)};
    }
    const uint32_t word = static_cast<uint32_t>(value);
    for (uint8_t index = 0; index < 4; ++index) {
      state.output[index] = static_cast<uint8_t>(word >> (24 - index * 8));
    }
    state.groupLength = 0;
    state.outputLength = 4;
    state.outputPosition = 1;
    *byte = state.output[0];
    return {PullState::Byte, PdfStatus::success()};
  }
}

PdfStreamDecoder::PullResult PdfStreamDecoder::flushPending(PdfWorkBudget& budget) {
  const uint8_t* source = workspace_.outputBuffer + (hasFlate_ ? finalOutputOffset_ : 0);
  while (pendingOutputWritten_ < pendingOutputLength_) {
    if (budget.cancelRequested()) {
      return {PullState::Failed, PdfStatus::failure(PdfError::Cancelled, outputBytes_)};
    }
    if (!budget.consumeOperation()) {
      return {PullState::Yielded, PdfStatus::failure(PdfError::BudgetExhausted, outputBytes_)};
    }
    const size_t requested = budget.takeBytes(pendingOutputLength_ - pendingOutputWritten_);
    if (requested == 0) {
      return {PullState::Yielded, PdfStatus::failure(PdfError::BudgetExhausted, outputBytes_)};
    }
    size_t written = 0;
    const PdfStatus status = sink_.write(sink_.context, source + pendingOutputWritten_, requested, &written);
    if (!status.ok()) {
      return {PullState::Failed, status};
    }
    if (written == 0 || written > requested) {
      return {PullState::Failed, PdfStatus::failure(PdfError::IoFailure, outputBytes_)};
    }
    // The sink may deliberately accept only a prefix (for example, one
    // complete fixed-size record). Charge the bytes actually accepted and
    // retain the unwritten suffix for the next flush operation.
    budget.bytesRemaining += requested - written;
    pendingOutputWritten_ += written;
    outputBytes_ += written;
  }
  pendingOutputLength_ = 0;
  pendingOutputWritten_ = 0;
  return {PullState::End, PdfStatus::success()};
}

PdfStatus PdfStreamDecoder::validateGrowth(const uint64_t additionalBytes) const {
  uint64_t total = 0;
  if (!pdfCheckedAdd(outputBytes_, pendingOutputLength_, &total) || !pdfCheckedAdd(total, additionalBytes, &total) ||
      total > limits_.maxExpandedBytes) {
    return PdfStatus::failure(PdfError::ExpansionLimit, outputBytes_);
  }
  uint64_t ratioLimit = 0;
  if (!pdfCheckedMultiply(inputBytes_, limits_.maxExpansionRatio, &ratioLimit) || total > ratioLimit) {
    return PdfStatus::failure(PdfError::ExpansionLimit, outputBytes_);
  }
  return PdfStatus::success();
}

PdfStepResult PdfStreamDecoder::fail(const PdfStatus status) {
  phase_ = Phase::Failed;
  activeBudget_ = nullptr;
  return PdfStepResult::failure(status);
}

int PdfStreamDecoder::inflateReadCallback(uzlib_uncomp* uncomp) {
  auto* context = reinterpret_cast<InflateContext*>(uncomp);
  if (context == nullptr || context->owner == nullptr) {
    return -1;
  }
  return context->owner->refillInflateInput(uncomp);
}

int PdfStreamDecoder::refillInflateInput(uzlib_uncomp*) {
  callbackStatus_ =
      PdfStatus::failure(inflateInputAtEnd_ ? PdfError::UnexpectedEof : PdfError::ExpansionLimit, inputBytes_);
  return -1;
}

PdfStatus pdfStreamFiltersFromDictionary(const PdfObjectArena& arena, const uint16_t dictionaryIndex,
                                         PdfStreamFilter* filters, const uint8_t filterCapacity, uint8_t* filterCount) {
  if (filters == nullptr || filterCount == nullptr || dictionaryIndex >= arena.valueCount ||
      arena.values[dictionaryIndex].kind != PdfValueKind::Dictionary) {
    return PdfStatus::failure(PdfError::InvalidArgument, dictionaryIndex);
  }
  *filterCount = 0;
  const PdfValue& dictionary = arena.values[dictionaryIndex];
  uint16_t entryIndex = dictionary.firstLink;
  uint8_t filterKeys = 0;
  uint8_t decodeParameterKeys = 0;
  for (uint16_t ordinal = 0; ordinal < dictionary.count; ++ordinal) {
    if (entryIndex >= arena.dictionaryCount) {
      return PdfStatus::failure(PdfError::Malformed, dictionaryIndex);
    }
    const PdfDictionaryEntry& entry = arena.dictionaryEntries[entryIndex];
    if (dictionaryKeyEquals(arena, entry, "Filter")) {
      ++filterKeys;
    } else if (dictionaryKeyEquals(arena, entry, "DecodeParms")) {
      ++decodeParameterKeys;
    }
    if (filterKeys > 1 || decodeParameterKeys > 1) {
      return PdfStatus::failure(PdfError::Malformed, dictionaryIndex);
    }
    entryIndex = entry.next;
  }
  uint16_t valueIndex = PDF_INVALID_INDEX;
  if (!pdfDictionaryFind(arena, dictionaryIndex, "Filter", &valueIndex)) {
    return validateDecodeParameters(arena, dictionaryIndex, filters, 0);
  }
  if (valueIndex >= arena.valueCount) {
    return PdfStatus::failure(PdfError::Malformed, dictionaryIndex);
  }
  const PdfValue& value = arena.values[valueIndex];
  if (value.kind == PdfValueKind::Name) {
    if (filterCapacity == 0) {
      return PdfStatus::failure(PdfError::LimitExceeded, dictionaryIndex);
    }
    filters[0] = filterFromName(arena, value);
    *filterCount = 1;
  } else {
    if (value.kind != PdfValueKind::Array || value.count > PdfLimits::MaxFiltersPerStream ||
        value.count > filterCapacity) {
      return PdfStatus::failure(PdfError::LimitExceeded, dictionaryIndex);
    }
    for (uint16_t ordinal = 0; ordinal < value.count; ++ordinal) {
      uint16_t itemIndex = PDF_INVALID_INDEX;
      if (!pdfArrayAt(arena, valueIndex, ordinal, &itemIndex) || itemIndex >= arena.valueCount ||
          arena.values[itemIndex].kind != PdfValueKind::Name) {
        return PdfStatus::failure(PdfError::Malformed, dictionaryIndex);
      }
      filters[ordinal] = filterFromName(arena, arena.values[itemIndex]);
    }
    *filterCount = static_cast<uint8_t>(value.count);
  }
  return validateDecodeParameters(arena, dictionaryIndex, filters, *filterCount);
}
