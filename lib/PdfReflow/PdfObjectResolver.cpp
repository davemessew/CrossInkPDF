#include "PdfObjectResolver.h"

#include <algorithm>
#include <cstring>
#include <limits>

#include "PdfCheckedMath.h"
#include "PdfStreamBoundary.h"

namespace {

constexpr uint64_t kResolvingIndirectLength = std::numeric_limits<uint64_t>::max();
constexpr uint64_t kResolvedIndirectLength = std::numeric_limits<uint64_t>::max() - 1U;

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

PdfStatus uniqueDictionaryValue(const PdfObjectArena& arena, const uint16_t dictionaryIndex, const char* key,
                                uint16_t* const valueIndex) {
  if (key == nullptr || valueIndex == nullptr || dictionaryIndex >= arena.valueCount ||
      arena.values[dictionaryIndex].kind != PdfValueKind::Dictionary) {
    return PdfStatus::failure(PdfError::Malformed, dictionaryIndex);
  }
  const size_t keyLength = std::strlen(key);
  const PdfValue& dictionary = arena.values[dictionaryIndex];
  uint16_t entryIndex = dictionary.firstLink;
  uint8_t matches = 0;
  for (uint16_t ordinal = 0; ordinal < dictionary.count; ++ordinal) {
    if (entryIndex >= arena.dictionaryCount) {
      return PdfStatus::failure(PdfError::Malformed, dictionaryIndex);
    }
    const PdfDictionaryEntry& entry = arena.dictionaryEntries[entryIndex];
    if (entry.keyLength == keyLength && static_cast<uint32_t>(entry.keyOffset) + entry.keyLength <= arena.textLength &&
        std::memcmp(arena.text + entry.keyOffset, key, keyLength) == 0) {
      if (++matches > 1 || entry.valueIndex >= arena.valueCount) {
        return PdfStatus::failure(PdfError::Malformed, dictionaryIndex);
      }
      *valueIndex = entry.valueIndex;
    }
    entryIndex = entry.next;
  }
  return matches == 1 ? PdfStatus::success() : PdfStatus::failure(PdfError::Malformed, dictionaryIndex);
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
      streamDecoder_(workspace.streamDecoder) {
  workspace_.decodeLimits.maxExpandedBytes =
      std::min<uint64_t>(workspace_.decodeLimits.maxExpandedBytes, PdfLimits::MaxExpandedRequiredStreamBytes);
  workspace_.decodeLimits.maxExpansionRatio =
      std::min<uint16_t>(workspace_.decodeLimits.maxExpansionRatio, PdfLimits::MaxExpansionRatio);
}

uint64_t PdfObjectResolver::currentStreamBytes() const {
  if (phase_ == Phase::DecodeObjectStream && streamDecoder_ != nullptr) {
    return streamDecoder_->outputBytes();
  }
  return objectStreamDecodedSize_;
}

uint64_t PdfObjectResolver::takeCompletedStreamBytes() {
  const uint64_t completed = completedStreamBytes_;
  completedStreamBytes_ = 0;
  return completed;
}

PdfStatus PdfObjectResolver::begin(const PdfObjectReference reference) {
  if (resolvingIndirectLength() || hasResolvedIndirectLength()) {
    cachedObjectStreamNumber_ = 0;
    cachedObjectStreamCount_ = 0;
    cachedObjectStreamFirst_ = 0;
    cachedObjectStreamSize_ = 0;
    cachedObjectStreamUsesStore_ = false;
  }
  result_ = {};
  result_.reference = reference;
  activeReference_ = reference;
  resolvingObjectStream_ = false;
  objectStreamTargetFound_ = false;
  objectStreamDecodedSize_ = 0;
  objectStreamUsesStore_ = false;
  publishObjectStream_ = false;
  streamFilterCount_ = 0;
  const PdfStatus status = xref_.beginFind(reference.objectNumber, &xrefLookup_);
  phase_ = status ? Phase::SelectXref : Phase::Failed;
  return status;
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
    if (budget.cancelRequested()) {
      return fail(PdfStatus::failure(PdfError::Cancelled, activeReference_.objectNumber));
    }
    if (budget.stopRequested()) {
      return PdfStepResult::paused();
    }

    if (phase_ == Phase::SelectXref) {
      const PdfStepResult access = stepSourceAccess(PdfObjectResolverReader::Xref, budget);
      if (!access.complete()) {
        return access.failed() ? fail(access.status) : access;
      }
      phase_ = Phase::LookupReference;
      return PdfStepResult::paused();
    }

    if (phase_ == Phase::LookupReference) {
      const PdfStepResult lookup = xref_.stepFind(xrefLookup_, &xrefLookup_.entry, budget);
      if (!lookup.complete()) {
        return lookup.failed() ? fail(lookup.status) : lookup;
      }
      if (xrefLookup_.entry.type == PdfXrefEntryType::Free) {
        return fail(PdfStatus::failure(PdfError::InvalidOffset, lookupTargetReference().objectNumber));
      }
      if (xrefLookup_.entry.type != PdfXrefEntryType::Compressed) {
        if (xrefLookup_.entry.generation != lookupTargetReference().generation) {
          return fail(PdfStatus::failure(PdfError::InvalidOffset, lookupTargetReference().objectNumber));
        }
        phase_ = Phase::SelectUncompressedSource;
        return PdfStepResult::paused();
      }

      if (lookupTargetReference().generation != 0 || xrefLookup_.entry.offset > PdfLimits::MaxIndirectObjects ||
          xrefLookup_.entry.objectStreamIndex >= PdfLimits::MaxIndirectObjects) {
        return fail(PdfStatus::failure(PdfError::Malformed, lookupTargetReference().objectNumber));
      }
      objectStreamNumber_ = static_cast<uint32_t>(xrefLookup_.entry.offset);
      objectStreamTargetIndex_ = xrefLookup_.entry.objectStreamIndex;
      if (objectStreamNumber_ == lookupTargetReference().objectNumber) {
        return fail(PdfStatus::failure(PdfError::Malformed, lookupTargetReference().objectNumber));
      }

      if (!resolvingIndirectLength() && !hasResolvedIndirectLength() &&
          cachedObjectStreamNumber_ == objectStreamNumber_ && cachedObjectStreamSize_ != 0) {
        objectStreamObjectCount_ = cachedObjectStreamCount_;
        objectStreamFirst_ = cachedObjectStreamFirst_;
        objectStreamDecodedSize_ = cachedObjectStreamSize_;
        objectStreamUsesStore_ = cachedObjectStreamUsesStore_;
        publishObjectStream_ = false;
        phase_ = objectStreamUsesStore_ ? Phase::SelectObjectStore : Phase::SelectDirectObjectStream;
        return PdfStepResult::paused();
      }

      const PdfStatus status = xref_.beginFind(objectStreamNumber_, &xrefLookup_);
      if (!status) {
        return fail(status);
      }
      phase_ = Phase::LookupObjectStream;
      return PdfStepResult::paused();
    }

    if (phase_ == Phase::LookupObjectStream) {
      const PdfStepResult lookup = xref_.stepFind(xrefLookup_, &xrefLookup_.entry, budget);
      if (!lookup.complete()) {
        return lookup.failed() ? fail(lookup.status) : lookup;
      }
      if (xrefLookup_.entry.type != PdfXrefEntryType::Uncompressed) {
        return fail(PdfStatus::failure(PdfError::Malformed, objectStreamNumber_));
      }
      phase_ = Phase::SelectObjectStreamSource;
      return PdfStepResult::paused();
    }

    if (phase_ == Phase::SelectUncompressedSource || phase_ == Phase::SelectObjectStreamSource) {
      const bool objectStream = phase_ == Phase::SelectObjectStreamSource;
      const PdfStepResult access = stepSourceAccess(PdfObjectResolverReader::Source, budget);
      if (!access.complete()) {
        return access.failed() ? fail(access.status) : access;
      }
      resolvingObjectStream_ = objectStream;
      const PdfObjectReference reference =
          objectStream ? PdfObjectReference{objectStreamNumber_, xrefLookup_.entry.generation}
                       : lookupTargetReference();
      const PdfStatus status = beginUncompressed(reference, xrefLookup_.entry);
      if (!status) {
        return fail(status);
      }
      return PdfStepResult::paused();
    }

    if (phase_ == Phase::ConfigureObjectStream) {
      if (!budget.consumeOperation()) {
        return PdfStepResult::paused();
      }
      const PdfStatus status = configureObjectStream(pendingObjectStreamOffset_);
      if (!status) {
        return fail(status);
      }
      return PdfStepResult::paused();
    }

    if (phase_ == Phase::SelectObjectStoreWriter) {
      const PdfStepResult access = stepSourceAccess(PdfObjectResolverReader::ObjectStoreWriter, budget);
      if (!access.complete()) {
        return access.failed() ? fail(access.status) : access;
      }
      phase_ = Phase::ResetObjectStreamStore;
      return PdfStepResult::paused();
    }

    if (phase_ == Phase::ResetObjectStreamStore) {
      if (!budget.consumeOperation()) {
        return PdfStepResult::paused();
      }
      clearObjectStreamCache();
      const PdfStatus status = workspace_.objectStreamStore.reset(workspace_.objectStreamStore.context);
      if (!status) {
        return fail(status);
      }
      phase_ = Phase::BeginObjectStreamDecode;
      return PdfStepResult::paused();
    }

    if (phase_ == Phase::BeginObjectStreamDecode) {
      if (!budget.consumeOperation()) {
        return PdfStepResult::paused();
      }
      const PdfStatus status = beginObjectStreamDecode();
      if (!status) {
        return fail(status);
      }
      return PdfStepResult::paused();
    }

    if (phase_ == Phase::SelectObjectStore || phase_ == Phase::SelectDirectObjectStream) {
      const bool stored = phase_ == Phase::SelectObjectStore;
      const PdfStepResult access =
          stepSourceAccess(stored ? PdfObjectResolverReader::ObjectStore : PdfObjectResolverReader::Source, budget);
      if (!access.complete()) {
        return access.failed() ? fail(access.status) : access;
      }
      phase_ = Phase::ActivateObjectStream;
      return PdfStepResult::paused();
    }

    if (phase_ == Phase::ActivateObjectStream) {
      if (!budget.consumeOperation()) {
        return PdfStepResult::paused();
      }
      const PdfStatus status = activateObjectStream();
      if (!status) {
        return fail(status);
      }
      return PdfStepResult::paused();
    }

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
        if (resolvingIndirectLength()) {
          const PdfStatus status = finishIndirectLength();
          if (!status.ok()) {
            return fail(status);
          }
          return PdfStepResult::paused();
        }
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
      objectStreamDecodedSize_ = streamDecoder_->outputBytes();
      phase_ = Phase::SelectObjectStore;
      return PdfStepResult::paused();
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
      const uint64_t streamEnd = streamOffset + result_.streamLength;
      const uint64_t available = source_.size - streamEnd;
      static_assert(sizeof(objectStreamSourceRange_) >= PdfStreamBoundaryLookaheadBytes);
      const size_t boundaryLength = static_cast<size_t>(std::min<uint64_t>(available, PdfStreamBoundaryLookaheadBytes));
      if (boundaryLength < PdfMinimumStreamBoundaryBytes) {
        return fail(PdfStatus::failure(PdfError::Malformed, streamEnd));
      }
      result_.streamOffset = streamOffset;
      eolRead_ = {streamEnd, reinterpret_cast<uint8_t*>(&objectStreamSourceRange_), boundaryLength, 0};
      phase_ = Phase::ValidateStreamBoundary;
      continue;
    }

    if (phase_ == Phase::ValidateStreamBoundary) {
      const PdfStepResult readResult = pdfStepReadExact(source_, eolRead_, budget);
      if (!readResult.complete()) {
        return readResult.failed() ? fail(readResult.status) : readResult;
      }
      if (!pdfValidateStreamBoundary(reinterpret_cast<const uint8_t*>(&objectStreamSourceRange_), eolRead_.length)) {
        return fail(PdfStatus::failure(PdfError::Malformed, result_.streamOffset + result_.streamLength));
      }
      if (resolvingObjectStream_) {
        pendingObjectStreamOffset_ = result_.streamOffset;
        phase_ = Phase::ConfigureObjectStream;
        continue;
      }
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
        if (resolvingIndirectLength() && activeReference_ == lookupTargetReference()) {
          if (!tokenEquals(token, "endobj")) {
            return fail(PdfStatus::failure(PdfError::Malformed, lexer_.tokenOffset()));
          }
          const PdfStatus status = finishIndirectLength();
          if (!status.ok()) {
            return fail(status);
          }
          return PdfStepResult::paused();
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
          const PdfStatus lengthStatus = uniqueDictionaryValue(arena_, result_.rootIndex, "Length", &lengthIndex);
          if (!lengthStatus.ok()) {
            return fail(lengthStatus);
          }
          const PdfValue& length = arena_.values[lengthIndex];
          if (length.kind == PdfValueKind::Reference) {
            const PdfObjectReference reference{length.objectNumber, length.generation};
            if (hasResolvedIndirectLength()) {
              if (cachedObjectStreamNumber_ != reference.objectNumber ||
                  cachedObjectStreamCount_ != reference.generation) {
                return fail(PdfStatus::failure(PdfError::Malformed, lexer_.tokenOffset()));
              }
              result_.streamLength = cachedObjectStreamFirst_;
            } else {
              const PdfStatus status = beginIndirectLength(reference);
              if (!status.ok()) {
                return fail(status);
              }
              return PdfStepResult::paused();
            }
          } else {
            if (length.kind != PdfValueKind::Integer || length.integerValue < 0) {
              return fail(PdfStatus::failure(PdfError::Malformed, lexer_.tokenOffset()));
            }
            result_.streamLength = static_cast<uint64_t>(length.integerValue);
          }
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
          if (objectStreamIndexObjectNumber_ != lookupTargetReference().objectNumber) {
            return fail(PdfStatus::failure(PdfError::Malformed, lookupTargetReference().objectNumber));
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
            return fail(PdfStatus::failure(PdfError::Malformed, lookupTargetReference().objectNumber));
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

bool PdfObjectResolver::resolvingIndirectLength() const { return cachedObjectStreamSize_ == kResolvingIndirectLength; }

bool PdfObjectResolver::hasResolvedIndirectLength() const { return cachedObjectStreamSize_ == kResolvedIndirectLength; }

PdfObjectReference PdfObjectResolver::lookupTargetReference() const {
  return resolvingIndirectLength()
             ? PdfObjectReference{cachedObjectStreamNumber_, static_cast<uint16_t>(cachedObjectStreamCount_)}
             : result_.reference;
}

PdfStatus PdfObjectResolver::beginIndirectLength(const PdfObjectReference reference) {
  if (resolvingIndirectLength() || hasResolvedIndirectLength() || reference == result_.reference ||
      reference == activeReference_) {
    return PdfStatus::failure(PdfError::Malformed, reference.objectNumber);
  }
  cachedObjectStreamNumber_ = reference.objectNumber;
  cachedObjectStreamCount_ = reference.generation;
  cachedObjectStreamFirst_ = 0;
  cachedObjectStreamSize_ = kResolvingIndirectLength;
  activeReference_ = reference;
  resolvingObjectStream_ = false;
  objectStreamTargetFound_ = false;
  streamFilterCount_ = 0;
  const PdfStatus status = xref_.beginFind(reference.objectNumber, &xrefLookup_);
  phase_ = status.ok() ? Phase::SelectXref : Phase::Failed;
  return status;
}

PdfStatus PdfObjectResolver::finishIndirectLength() {
  if (!resolvingIndirectLength() || result_.rootIndex >= arena_.valueCount) {
    return PdfStatus::failure(PdfError::Malformed, lookupTargetReference().objectNumber);
  }
  const PdfValue& value = arena_.values[result_.rootIndex];
  if (value.kind != PdfValueKind::Integer || value.integerValue < 0) {
    return PdfStatus::failure(PdfError::Malformed, lookupTargetReference().objectNumber);
  }
  cachedObjectStreamFirst_ = static_cast<uint64_t>(value.integerValue);
  cachedObjectStreamSize_ = kResolvedIndirectLength;

  result_.rootIndex = PDF_INVALID_INDEX;
  result_.streamOffset = 0;
  result_.streamLength = 0;
  result_.hasStream = false;
  activeReference_ = result_.reference;
  resolvingObjectStream_ = false;
  objectStreamTargetFound_ = false;
  streamFilterCount_ = 0;
  const PdfStatus status = xref_.beginFind(result_.reference.objectNumber, &xrefLookup_);
  phase_ = status.ok() ? Phase::SelectXref : Phase::Failed;
  return status;
}

PdfStatus PdfObjectResolver::configureObjectStream(const uint64_t streamOffset) {
  if (result_.rootIndex >= arena_.valueCount || arena_.values[result_.rootIndex].kind != PdfValueKind::Dictionary) {
    return PdfStatus::failure(PdfError::Malformed, objectStreamNumber_);
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
  if (workspace_.decodeLimits.maxExpandedBytes == 0 || workspace_.decodeLimits.maxExpansionRatio == 0) {
    return PdfStatus::failure(PdfError::ExpansionLimit, objectStreamNumber_);
  }
  if (!resolvingIndirectLength() && !hasResolvedIndirectLength() && cachedObjectStreamNumber_ != 0 &&
      cachedObjectStreamNumber_ != objectStreamNumber_) {
    clearObjectStreamCache();
  }

  const PdfStatus status =
      pdfInitializeByteRange(source_, streamOffset, result_.streamLength, &objectStreamSourceRange_);
  if (!status.ok()) {
    return status;
  }
  objectStreamUsesStore_ = streamFilterCount_ != 0;
  publishObjectStream_ = true;
  objectStreamDecodedSize_ = 0;
  result_.streamOffset = 0;
  result_.streamLength = 0;
  result_.hasStream = false;

  if (!objectStreamUsesStore_) {
    objectStreamDecodedSize_ = objectStreamSourceRange_.length;
    if (objectStreamDecodedSize_ > workspace_.decodeLimits.maxExpandedBytes) {
      return PdfStatus::failure(PdfError::ExpansionLimit, objectStreamDecodedSize_);
    }
    phase_ = Phase::SelectDirectObjectStream;
    return PdfStatus::success();
  }

  if (!workspace_.objectStreamStore.valid() || workspace_.objectStreamStore.capacity == 0 ||
      streamDecoder_ == nullptr) {
    return PdfStatus::failure(PdfError::Unsupported, objectStreamNumber_);
  }
  phase_ = Phase::SelectObjectStoreWriter;
  return PdfStatus::success();
}

PdfStatus PdfObjectResolver::beginObjectStreamDecode() {
  const PdfByteSource streamSource = pdfByteRangeSource(objectStreamSourceRange_);
  const PdfByteSink streamSink = pdfByteStoreSink(workspace_.objectStreamStore);
  PdfStreamDecodeLimits limits = workspace_.decodeLimits;
  limits.maxExpandedBytes = std::min(limits.maxExpandedBytes, workspace_.objectStreamStore.capacity);
  if (limits.maxExpandedBytes == 0) {
    return PdfStatus::failure(PdfError::ExpansionLimit, objectStreamNumber_);
  }
  const PdfStatus status = streamDecoder_->begin(streamSource, streamSink, streamFilters_, streamFilterCount_, limits);
  if (!status.ok()) {
    return status;
  }
  phase_ = Phase::DecodeObjectStream;
  return PdfStatus::success();
}

PdfStatus PdfObjectResolver::activateObjectStream() {
  objectStoreSource_ = objectStreamUsesStore_ ? pdfByteStoreSource(workspace_.objectStreamStore)
                                              : pdfByteRangeSource(objectStreamSourceRange_);
  if (!objectStoreSource_.valid() || objectStoreSource_.size != objectStreamDecodedSize_) {
    return PdfStatus::failure(PdfError::IoFailure, objectStreamNumber_);
  }
  if (objectStreamFirst_ > objectStreamDecodedSize_) {
    return PdfStatus::failure(PdfError::Malformed, objectStreamNumber_);
  }
  if (publishObjectStream_) {
    if (objectStreamDecodedSize_ > workspace_.decodeLimits.maxExpandedBytes ||
        completedStreamBytes_ > std::numeric_limits<uint64_t>::max() - objectStreamDecodedSize_) {
      return PdfStatus::failure(PdfError::ExpansionLimit, objectStreamDecodedSize_);
    }
    workspace_.decodeLimits.maxExpandedBytes -= objectStreamDecodedSize_;
    completedStreamBytes_ += objectStreamDecodedSize_;
    if (!resolvingIndirectLength() && !hasResolvedIndirectLength()) {
      cachedObjectStreamNumber_ = objectStreamNumber_;
      cachedObjectStreamCount_ = objectStreamObjectCount_;
      cachedObjectStreamFirst_ = objectStreamFirst_;
      cachedObjectStreamSize_ = objectStreamDecodedSize_;
      cachedObjectStreamUsesStore_ = objectStreamUsesStore_;
    }
    publishObjectStream_ = false;
  }
  return beginObjectStreamIndex();
}

PdfStatus PdfObjectResolver::beginObjectStreamIndex() {
  if (!objectStoreSource_.valid() || objectStreamFirst_ > objectStoreSource_.size ||
      objectStreamTargetIndex_ >= objectStreamObjectCount_) {
    return PdfStatus::failure(PdfError::Malformed, objectStreamNumber_);
  }
  const PdfStatus rangeStatus =
      pdfInitializeByteRange(objectStoreSource_, 0, objectStreamFirst_, &objectStreamSliceRange_);
  if (!rangeStatus.ok()) {
    return rangeStatus;
  }
  const PdfByteSource indexSource = pdfByteRangeSource(objectStreamSliceRange_);
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
    return PdfStatus::failure(PdfError::Malformed, lookupTargetReference().objectNumber);
  }
  uint64_t bodyOffset = 0;
  if (!pdfCheckedAdd(objectStreamFirst_, objectStreamTargetStart_, &bodyOffset) ||
      !pdfCheckedRange(bodyOffset, objectStreamTargetEnd_ - objectStreamTargetStart_, objectStoreSource_.size)) {
    return PdfStatus::failure(PdfError::InvalidOffset, bodyOffset);
  }
  const PdfStatus rangeStatus = pdfInitializeByteRange(
      objectStoreSource_, bodyOffset, objectStreamTargetEnd_ - objectStreamTargetStart_, &objectStreamSliceRange_);
  if (!rangeStatus.ok()) {
    return rangeStatus;
  }
  const PdfByteSource bodySource = pdfByteRangeSource(objectStreamSliceRange_);
  lexer_.setSource(bodySource);
  parser_.begin();
  phase_ = Phase::ParseEmbeddedValue;
  return PdfStatus::success();
}

void PdfObjectResolver::clearObjectStreamCache() {
  if (resolvingIndirectLength() || hasResolvedIndirectLength()) {
    return;
  }
  cachedObjectStreamNumber_ = 0;
  cachedObjectStreamCount_ = 0;
  cachedObjectStreamFirst_ = 0;
  cachedObjectStreamSize_ = 0;
  cachedObjectStreamUsesStore_ = false;
}

PdfStepResult PdfObjectResolver::stepSourceAccess(const PdfObjectResolverReader reader, PdfWorkBudget& budget) {
  if (sourceAccessKnown_ && sourceAccess_ == reader) {
    return PdfStepResult::completed();
  }
  if (workspace_.setSourceAccess == nullptr) {
    sourceAccess_ = reader;
    sourceAccessKnown_ = true;
    return PdfStepResult::completed();
  }
  if (budget.cancelRequested()) {
    return PdfStepResult::failure(PdfStatus::failure(PdfError::Cancelled, activeReference_.objectNumber));
  }
  if (budget.stopRequested()) {
    return PdfStepResult::paused();
  }
  const PdfStepResult result = workspace_.setSourceAccess(workspace_.sourceAccessContext, reader, budget);
  if (result.complete()) {
    sourceAccess_ = reader;
    sourceAccessKnown_ = true;
  }
  return result;
}
