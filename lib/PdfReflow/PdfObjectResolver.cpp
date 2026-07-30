#include "PdfObjectResolver.h"

#include <algorithm>
#include <cstring>
#include <limits>

#include "PdfCheckedMath.h"

namespace {

bool tokenEquals(const PdfToken& token, const char* expected) {
  const size_t length = std::strlen(expected);
  return token.length == length && std::memcmp(token.bytes, expected, length) == 0;
}

bool parseUnsigned(const PdfToken& token, uint64_t* value) {
  if (value == nullptr || token.kind != PdfTokenKind::Integer || token.length == 0) {
    return false;
  }
  uint64_t parsed = 0;
  for (uint32_t index = 0; index < token.length; ++index) {
    const char byte = token.bytes[index];
    if (byte < '0' || byte > '9') {
      return false;
    }
    const uint8_t digit = static_cast<uint8_t>(byte - '0');
    if (parsed > (std::numeric_limits<uint64_t>::max() - digit) / 10) {
      return false;
    }
    parsed = parsed * 10 + digit;
  }
  *value = parsed;
  return true;
}

bool dictionaryUnsigned(const PdfObjectArena& arena, const uint16_t dictionaryIndex, const char* key, uint64_t* value) {
  uint16_t valueIndex = PDF_INVALID_INDEX;
  if (value == nullptr || !pdfDictionaryFind(arena, dictionaryIndex, key, &valueIndex) ||
      valueIndex >= arena.valueCount) {
    return false;
  }
  const PdfValue& item = arena.values[valueIndex];
  if (item.kind != PdfValueKind::Integer || item.integerValue < 0) {
    return false;
  }
  *value = static_cast<uint64_t>(item.integerValue);
  return true;
}

}  // namespace

PdfObjectResolver::PdfObjectResolver(const PdfByteSource& source, const PdfXrefTable& xref, uint8_t* sourceBuffer,
                                     const size_t sourceBufferSize, PdfObjectArena& arena,
                                     const PdfObjectResolverWorkspace workspace)
    : source_(source),
      xref_(xref),
      lexer_(source, sourceBuffer, sourceBufferSize),
      arena_(arena),
      parser_(lexer_, arena),
      workspace_(workspace),
      streamDecoder_(workspace.streamDecoder) {}

PdfStatus PdfObjectResolver::begin(const PdfObjectReference reference) {
  result_ = {};
  result_.reference = reference;
  activeReference_ = reference;
  resolvingObjectStream_ = false;
  objectStreamTargetFound_ = false;
  streamFilterCount_ = 0;

  PdfXrefEntry entry;
  PdfStatus status = xref_.find(reference.objectNumber, &entry);
  if (!status.ok()) {
    phase_ = Phase::Failed;
    return status;
  }
  if (entry.type == PdfXrefEntryType::Free) {
    phase_ = Phase::Failed;
    return PdfStatus::failure(PdfError::InvalidOffset, reference.objectNumber);
  }

  if (entry.type != PdfXrefEntryType::Compressed) {
    if (entry.generation != reference.generation) {
      phase_ = Phase::Failed;
      return PdfStatus::failure(PdfError::InvalidOffset, reference.objectNumber);
    }
    status = setSourceAccess(true);
    if (!status.ok()) {
      phase_ = Phase::Failed;
      return status;
    }
    return beginUncompressed(reference, entry);
  }

  if (reference.generation != 0 || !workspace_.objectStreamStore.valid() ||
      entry.offset > PdfLimits::MaxIndirectObjects || entry.objectStreamIndex >= PdfLimits::MaxIndirectObjects) {
    phase_ = Phase::Failed;
    return PdfStatus::failure(workspace_.objectStreamStore.valid() ? PdfError::Malformed : PdfError::Unsupported,
                              reference.objectNumber);
  }
  objectStreamNumber_ = static_cast<uint32_t>(entry.offset);
  objectStreamTargetIndex_ = entry.objectStreamIndex;
  if (objectStreamNumber_ == reference.objectNumber) {
    phase_ = Phase::Failed;
    return PdfStatus::failure(PdfError::Malformed, reference.objectNumber);
  }

  if (cachedObjectStreamNumber_ == objectStreamNumber_ && cachedObjectStreamSize_ != 0) {
    objectStreamObjectCount_ = cachedObjectStreamCount_;
    objectStreamFirst_ = cachedObjectStreamFirst_;
    objectStreamDecodedSize_ = cachedObjectStreamSize_;
    objectStoreSource_ = pdfByteStoreSource(workspace_.objectStreamStore);
    status = setSourceAccess(false);
    if (!status.ok() || !objectStoreSource_.valid()) {
      phase_ = Phase::Failed;
      return status.ok() ? PdfStatus::failure(PdfError::IoFailure, objectStreamNumber_) : status;
    }
    status = beginObjectStreamIndex();
    if (!status.ok()) {
      phase_ = Phase::Failed;
    }
    return status;
  }

  status = setSourceAccess(true);
  if (!status.ok()) {
    phase_ = Phase::Failed;
    return status;
  }
  PdfXrefEntry objectStreamEntry;
  status = xref_.find(objectStreamNumber_, &objectStreamEntry);
  if (!status.ok()) {
    phase_ = Phase::Failed;
    return status;
  }
  if (objectStreamEntry.type != PdfXrefEntryType::Uncompressed) {
    phase_ = Phase::Failed;
    return PdfStatus::failure(PdfError::Malformed, objectStreamNumber_);
  }
  resolvingObjectStream_ = true;
  return beginUncompressed({objectStreamNumber_, objectStreamEntry.generation}, objectStreamEntry);
}

PdfStatus PdfObjectResolver::beginUncompressed(const PdfObjectReference reference, const PdfXrefEntry& entry) {
  if (entry.offset >= source_.size) {
    phase_ = Phase::Failed;
    return PdfStatus::failure(PdfError::InvalidOffset, entry.offset);
  }
  activeReference_ = reference;
  lexer_.setSource(source_, entry.offset);
  phase_ = Phase::ObjectNumber;
  return PdfStatus::success();
}

PdfStepResult PdfObjectResolver::step(PdfWorkBudget& budget) {
  if (phase_ == Phase::Done) {
    return PdfStepResult::completed();
  }
  if (phase_ == Phase::Idle || phase_ == Phase::Failed) {
    return PdfStepResult::failure(PdfStatus::failure(PdfError::Malformed, lexer_.position()));
  }

  auto fail = [this](const PdfStatus status) {
    phase_ = Phase::Failed;
    return PdfStepResult::failure(status);
  };

  while (true) {
    if (phase_ == Phase::ParseValue || phase_ == Phase::ParseEmbeddedValue) {
      const bool embedded = phase_ == Phase::ParseEmbeddedValue;
      const PdfStepResult parseResult = parser_.step(budget);
      if (!parseResult.complete()) {
        if (parseResult.failed()) {
          phase_ = Phase::Failed;
        }
        return parseResult;
      }
      result_.rootIndex = parser_.rootIndex();
      if (embedded) {
        result_.streamOffset = 0;
        result_.streamLength = 0;
        result_.hasStream = false;
        phase_ = Phase::Done;
        return PdfStepResult::completed();
      }
      phase_ = Phase::AfterValue;
      continue;
    }

    if (phase_ == Phase::DecodeObjectStream) {
      const PdfStepResult decodeResult = streamDecoder_->step(budget);
      if (!decodeResult.complete()) {
        if (decodeResult.failed()) {
          phase_ = Phase::Failed;
        }
        return decodeResult;
      }
      PdfStatus status = setSourceAccess(false);
      if (!status.ok()) {
        return fail(status);
      }
      objectStoreSource_ = pdfByteStoreSource(workspace_.objectStreamStore);
      if (!objectStoreSource_.valid()) {
        return fail(PdfStatus::failure(PdfError::IoFailure, objectStreamNumber_));
      }
      objectStreamDecodedSize_ = objectStoreSource_.size;
      if (objectStreamFirst_ > objectStreamDecodedSize_) {
        return fail(PdfStatus::failure(PdfError::Malformed, objectStreamNumber_));
      }
      cachedObjectStreamNumber_ = objectStreamNumber_;
      cachedObjectStreamCount_ = objectStreamObjectCount_;
      cachedObjectStreamFirst_ = objectStreamFirst_;
      cachedObjectStreamSize_ = objectStreamDecodedSize_;
      status = beginObjectStreamIndex();
      if (!status.ok()) {
        return fail(status);
      }
      continue;
    }

    if (phase_ == Phase::StreamFirstEol || phase_ == Phase::StreamSecondEol) {
      const PdfStepResult readResult = pdfStepReadExact(source_, eolRead_, budget);
      if (!readResult.complete()) {
        if (readResult.failed()) {
          phase_ = Phase::Failed;
        }
        return readResult;
      }
      uint64_t streamOffset = 0;
      if (phase_ == Phase::StreamFirstEol) {
        if (eolByte_ == '\n') {
          streamOffset = eolOffset_ + 1;
        } else if (eolByte_ == '\r') {
          phase_ = Phase::StreamSecondEol;
          eolRead_ = {eolOffset_ + 1, &eolByte_, 1, 0};
          continue;
        } else {
          return fail(PdfStatus::failure(PdfError::Malformed, eolOffset_));
        }
      } else {
        if (eolByte_ != '\n') {
          return fail(PdfStatus::failure(PdfError::Malformed, eolOffset_ + 1));
        }
        streamOffset = eolOffset_ + 2;
      }
      if (!pdfCheckedRange(streamOffset, result_.streamLength, source_.size)) {
        return fail(PdfStatus::failure(PdfError::InvalidOffset, streamOffset));
      }
      if (resolvingObjectStream_) {
        const PdfStatus status = prepareObjectStream(streamOffset);
        if (!status.ok()) {
          return fail(status);
        }
        continue;
      }
      result_.streamOffset = streamOffset;
      result_.hasStream = true;
      phase_ = Phase::Done;
      return PdfStepResult::completed();
    }

    PdfToken token;
    const PdfStepResult tokenResult = lexer_.next(token, budget);
    if (!tokenResult.complete()) {
      if (tokenResult.failed()) {
        phase_ = Phase::Failed;
      }
      return tokenResult;
    }
    uint64_t value = 0;

    switch (phase_) {
      case Phase::ObjectNumber:
        if (!parseUnsigned(token, &value) || value != activeReference_.objectNumber) {
          return fail(PdfStatus::failure(PdfError::Malformed, lexer_.tokenOffset()));
        }
        phase_ = Phase::Generation;
        break;

      case Phase::Generation:
        if (!parseUnsigned(token, &value) || value != activeReference_.generation) {
          return fail(PdfStatus::failure(PdfError::Malformed, lexer_.tokenOffset()));
        }
        phase_ = Phase::ObjKeyword;
        break;

      case Phase::ObjKeyword:
        if (token.kind != PdfTokenKind::Keyword || !tokenEquals(token, "obj")) {
          return fail(PdfStatus::failure(PdfError::Malformed, lexer_.tokenOffset()));
        }
        parser_.begin();
        phase_ = Phase::ParseValue;
        break;

      case Phase::AfterValue:
        if (token.kind != PdfTokenKind::Keyword) {
          return fail(PdfStatus::failure(PdfError::Malformed, lexer_.tokenOffset()));
        }
        if (tokenEquals(token, "endobj")) {
          if (resolvingObjectStream_) {
            return fail(PdfStatus::failure(PdfError::Malformed, activeReference_.objectNumber));
          }
          phase_ = Phase::Done;
          return PdfStepResult::completed();
        }
        if (!tokenEquals(token, "stream") || result_.rootIndex >= arena_.valueCount ||
            arena_.values[result_.rootIndex].kind != PdfValueKind::Dictionary) {
          return fail(PdfStatus::failure(PdfError::Malformed, lexer_.tokenOffset()));
        }
        {
          uint16_t lengthIndex = PDF_INVALID_INDEX;
          if (!pdfDictionaryFind(arena_, result_.rootIndex, "Length", &lengthIndex) ||
              lengthIndex >= arena_.valueCount) {
            return fail(PdfStatus::failure(PdfError::Malformed, lexer_.tokenOffset()));
          }
          const PdfValue& length = arena_.values[lengthIndex];
          if (length.kind == PdfValueKind::Reference) {
            return fail(PdfStatus::failure(PdfError::Unsupported, lexer_.tokenOffset()));
          }
          if (length.kind != PdfValueKind::Integer || length.integerValue < 0) {
            return fail(PdfStatus::failure(PdfError::Malformed, lexer_.tokenOffset()));
          }
          result_.streamLength = static_cast<uint64_t>(length.integerValue);
          eolOffset_ = lexer_.position();
          eolRead_ = {eolOffset_, &eolByte_, 1, 0};
          phase_ = Phase::StreamFirstEol;
        }
        break;

      case Phase::ObjectStreamIndexNumber:
        if (!parseUnsigned(token, &value) || value > PdfLimits::MaxIndirectObjects) {
          return fail(PdfStatus::failure(PdfError::Malformed, lexer_.tokenOffset()));
        }
        objectStreamIndexObjectNumber_ = static_cast<uint32_t>(value);
        phase_ = Phase::ObjectStreamIndexOffset;
        break;

      case Phase::ObjectStreamIndexOffset: {
        const uint64_t bodyLength = objectStreamDecodedSize_ - objectStreamFirst_;
        if (!parseUnsigned(token, &value) || value > bodyLength ||
            (objectStreamIndexOrdinal_ != 0 && value < objectStreamPreviousOffset_)) {
          return fail(PdfStatus::failure(PdfError::Malformed, lexer_.tokenOffset()));
        }
        objectStreamCurrentOffset_ = value;
        if (objectStreamIndexOrdinal_ == objectStreamTargetIndex_) {
          if (objectStreamIndexObjectNumber_ != result_.reference.objectNumber) {
            return fail(PdfStatus::failure(PdfError::Malformed, result_.reference.objectNumber));
          }
          objectStreamTargetStart_ = value;
          objectStreamTargetFound_ = true;
        } else if (objectStreamIndexOrdinal_ == objectStreamTargetIndex_ + 1) {
          objectStreamTargetEnd_ = value;
        }
        objectStreamPreviousOffset_ = value;
        ++objectStreamIndexOrdinal_;
        if (objectStreamIndexOrdinal_ == objectStreamObjectCount_) {
          if (!objectStreamTargetFound_) {
            return fail(PdfStatus::failure(PdfError::Malformed, result_.reference.objectNumber));
          }
          if (objectStreamTargetIndex_ + 1 >= objectStreamObjectCount_) {
            objectStreamTargetEnd_ = bodyLength;
          }
          const PdfStatus status = beginEmbeddedObject();
          if (!status.ok()) {
            return fail(status);
          }
        } else {
          phase_ = Phase::ObjectStreamIndexNumber;
        }
        break;
      }

      default:
        return fail(PdfStatus::failure(PdfError::Malformed, lexer_.position()));
    }
  }
}

PdfStatus PdfObjectResolver::prepareObjectStream(const uint64_t streamOffset) {
  if (!workspace_.objectStreamStore.valid() || workspace_.objectStreamStore.capacity == 0 ||
      streamDecoder_ == nullptr || result_.rootIndex >= arena_.valueCount ||
      arena_.values[result_.rootIndex].kind != PdfValueKind::Dictionary) {
    return PdfStatus::failure(PdfError::Unsupported, objectStreamNumber_);
  }
  uint16_t typeIndex = PDF_INVALID_INDEX;
  if (!pdfDictionaryFind(arena_, result_.rootIndex, "Type", &typeIndex) || typeIndex >= arena_.valueCount ||
      !pdfTextEquals(arena_, arena_.values[typeIndex], "ObjStm")) {
    return PdfStatus::failure(PdfError::Malformed, objectStreamNumber_);
  }
  uint64_t count = 0;
  uint64_t first = 0;
  if (!dictionaryUnsigned(arena_, result_.rootIndex, "N", &count) || count == 0 ||
      count > PdfLimits::MaxIndirectObjects || !dictionaryUnsigned(arena_, result_.rootIndex, "First", &first)) {
    return PdfStatus::failure(PdfError::LimitExceeded, objectStreamNumber_);
  }
  if (objectStreamTargetIndex_ >= count) {
    return PdfStatus::failure(PdfError::Malformed, objectStreamTargetIndex_);
  }
  objectStreamObjectCount_ = static_cast<uint32_t>(count);
  objectStreamFirst_ = first;
  const PdfStatus filterStatus = pdfStreamFiltersFromDictionary(arena_, result_.rootIndex, streamFilters_,
                                                                PdfLimits::MaxFiltersPerStream, &streamFilterCount_);
  if (!filterStatus.ok()) {
    return filterStatus;
  }

  PdfStatus status = pdfInitializeByteRange(source_, streamOffset, result_.streamLength, &objectStreamSourceRange_);
  if (!status.ok()) {
    return status;
  }
  status = workspace_.objectStreamStore.reset(workspace_.objectStreamStore.context);
  if (!status.ok()) {
    return status;
  }
  const PdfByteSource streamSource = pdfByteRangeSource(objectStreamSourceRange_);
  const PdfByteSink streamSink = pdfByteStoreSink(workspace_.objectStreamStore);
  PdfStreamDecodeLimits limits;
  limits.maxExpandedBytes =
      std::min<uint64_t>(PdfLimits::MaxExpandedRequiredStreamBytes, workspace_.objectStreamStore.capacity);
  status = streamDecoder_->begin(streamSource, streamSink, streamFilters_, streamFilterCount_, limits);
  if (!status.ok()) {
    return status;
  }
  result_.streamOffset = 0;
  result_.streamLength = 0;
  result_.hasStream = false;
  phase_ = Phase::DecodeObjectStream;
  return PdfStatus::success();
}

PdfStatus PdfObjectResolver::beginObjectStreamIndex() {
  if (!objectStoreSource_.valid() || objectStreamFirst_ > objectStoreSource_.size ||
      objectStreamTargetIndex_ >= objectStreamObjectCount_) {
    return PdfStatus::failure(PdfError::Malformed, objectStreamNumber_);
  }
  const PdfStatus rangeStatus =
      pdfInitializeByteRange(objectStoreSource_, 0, objectStreamFirst_, &objectStreamIndexRange_);
  if (!rangeStatus.ok()) {
    return rangeStatus;
  }
  const PdfByteSource indexSource = pdfByteRangeSource(objectStreamIndexRange_);
  lexer_.setSource(indexSource);
  objectStreamIndexOrdinal_ = 0;
  objectStreamIndexObjectNumber_ = 0;
  objectStreamCurrentOffset_ = 0;
  objectStreamPreviousOffset_ = 0;
  objectStreamTargetStart_ = 0;
  objectStreamTargetEnd_ = 0;
  objectStreamTargetFound_ = false;
  phase_ = Phase::ObjectStreamIndexNumber;
  return PdfStatus::success();
}

PdfStatus PdfObjectResolver::beginEmbeddedObject() {
  if (objectStreamTargetEnd_ < objectStreamTargetStart_) {
    return PdfStatus::failure(PdfError::Malformed, result_.reference.objectNumber);
  }
  uint64_t bodyOffset = 0;
  if (!pdfCheckedAdd(objectStreamFirst_, objectStreamTargetStart_, &bodyOffset) ||
      !pdfCheckedRange(bodyOffset, objectStreamTargetEnd_ - objectStreamTargetStart_, objectStoreSource_.size)) {
    return PdfStatus::failure(PdfError::InvalidOffset, bodyOffset);
  }
  const PdfStatus rangeStatus = pdfInitializeByteRange(
      objectStoreSource_, bodyOffset, objectStreamTargetEnd_ - objectStreamTargetStart_, &objectStreamBodyRange_);
  if (!rangeStatus.ok()) {
    return rangeStatus;
  }
  const PdfByteSource bodySource = pdfByteRangeSource(objectStreamBodyRange_);
  lexer_.setSource(bodySource);
  parser_.begin();
  phase_ = Phase::ParseEmbeddedValue;
  return PdfStatus::success();
}

PdfStatus PdfObjectResolver::setSourceAccess(const bool sourceRequired) {
  if (sourceAccessRequired_ == sourceRequired) {
    return PdfStatus::success();
  }
  if (workspace_.setSourceAccess != nullptr) {
    const PdfStatus status = workspace_.setSourceAccess(workspace_.sourceAccessContext, sourceRequired);
    if (!status.ok()) {
      return status;
    }
  }
  sourceAccessRequired_ = sourceRequired;
  return PdfStatus::success();
}
