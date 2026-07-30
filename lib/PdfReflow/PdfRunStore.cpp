#include "PdfRunStore.h"

#include <algorithm>
#include <cstring>
#include <limits>

#include "PdfIo.h"
#include "PdfLimits.h"

PdfStatus PdfRunStore::reset() {
  if (reductionActive_) {
    return PdfStatus::failure(PdfError::InvalidArgument);
  }
  initialized_ = false;
  if (workspace_.memoryRuns == nullptr || workspace_.memoryRunCapacity == 0 || workspace_.memoryText == nullptr ||
      workspace_.memoryTextCapacity == 0 || workspace_.memoryTextCapacity > std::numeric_limits<uint32_t>::max() ||
      !lifecycle_.valid()) {
    return PdfStatus::failure(PdfError::InvalidArgument);
  }
  const bool hasRunSpill = workspace_.spillRuns.valid();
  const bool hasTextSpill = workspace_.spillText.valid();
  if (hasRunSpill != hasTextSpill ||
      (hasRunSpill && (workspace_.spillRuns.recordSize != sizeof(PdfTextRun) ||
                       workspace_.spillText.capacity > std::numeric_limits<uint32_t>::max()))) {
    return PdfStatus::failure(PdfError::InvalidArgument);
  }
  if (hasTextSpill) {
    const PdfStatus status = workspace_.spillText.reset(workspace_.spillText.context);
    if (!status.ok()) {
      return status;
    }
  }
  runCount_ = 0;
  metadataSpillCount_ = 0;
  memoryTextLength_ = 0;
  totalTextLength_ = 0;
  textSpilling_ = false;
  continuationOffset_ = 0;
  initialized_ = true;
  return PdfStatus::success();
}

PdfStatus PdfRunStore::append(const PdfTextRun& input, const uint8_t* const text, const size_t textLength) {
  if (!initialized_ || reductionActive_ || text == nullptr || textLength == 0 || (input.flags & TextInSpillFlag) != 0) {
    return PdfStatus::failure(PdfError::InvalidArgument, runCount_);
  }
  if (textLength > std::numeric_limits<uint32_t>::max() ||
      totalTextLength_ > PdfLimits::MaxExpandedRequiredStreamBytes ||
      textLength > PdfLimits::MaxExpandedRequiredStreamBytes - totalTextLength_ ||
      runCount_ >= PdfLimits::MaxOperatorsPerPage) {
    return PdfStatus::failure(PdfError::LimitExceeded, runCount_);
  }

  const bool metadataInSpill = runCount_ >= workspace_.memoryRunCapacity;
  if (metadataInSpill && (!workspace_.spillRuns.valid() || metadataSpillCount_ >= workspace_.spillRuns.capacity)) {
    return PdfStatus::failure(PdfError::InsufficientStorage, runCount_);
  }

  const bool useTextSpill = textSpilling_ || textLength > workspace_.memoryTextCapacity - memoryTextLength_;
  if (useTextSpill) {
    if (!workspace_.spillText.valid()) {
      return PdfStatus::failure(PdfError::InsufficientStorage, totalTextLength_);
    }
    const uint64_t spillLength = workspace_.spillText.size(workspace_.spillText.context);
    if (spillLength > workspace_.spillText.capacity || textLength > workspace_.spillText.capacity - spillLength ||
        spillLength > std::numeric_limits<uint32_t>::max()) {
      return PdfStatus::failure(PdfError::InsufficientStorage, spillLength);
    }
  }

  PdfTextRun stored = input;
  stored.textLength = static_cast<uint32_t>(textLength);
  if (useTextSpill) {
    textSpilling_ = true;
    const uint64_t spillOffset = workspace_.spillText.size(workspace_.spillText.context);
    stored.textOffset = static_cast<uint32_t>(spillOffset);
    stored.flags |= TextInSpillFlag;
    const PdfStatus status = pdfWriteExact(pdfByteStoreSink(workspace_.spillText), text, textLength);
    if (!status.ok()) {
      return status;
    }
  } else {
    stored.textOffset = static_cast<uint32_t>(memoryTextLength_);
    std::memcpy(workspace_.memoryText + memoryTextLength_, text, textLength);
  }

  PdfStatus status = PdfStatus::success();
  if (metadataInSpill) {
    status = pdfWriteRecord(workspace_.spillRuns, metadataSpillCount_, &stored);
  } else {
    workspace_.memoryRuns[runCount_] = stored;
  }
  if (!status.ok()) {
    return status;
  }

  if (!useTextSpill) {
    memoryTextLength_ += textLength;
  }
  if (metadataInSpill) {
    ++metadataSpillCount_;
  }
  totalTextLength_ += textLength;
  ++runCount_;
  return PdfStatus::success();
}

PdfStatus PdfRunStore::beginReduction(const uint64_t continuationOffset) {
  if (!initialized_ || reductionActive_) {
    return PdfStatus::failure(PdfError::InvalidArgument, continuationOffset);
  }
  if (spilled() && lifecycle_.closeSourceForSpillRead != nullptr) {
    const PdfStatus status = lifecycle_.closeSourceForSpillRead(lifecycle_.context, continuationOffset);
    if (!status.ok()) {
      return status;
    }
  }
  continuationOffset_ = continuationOffset;
  reductionActive_ = true;
  return PdfStatus::success();
}

PdfStatus PdfRunStore::endReduction() {
  if (!initialized_ || !reductionActive_) {
    return PdfStatus::failure(PdfError::InvalidArgument, continuationOffset_);
  }
  if (spilled() && lifecycle_.closeSpillAndReopenSource != nullptr) {
    const PdfStatus status = lifecycle_.closeSpillAndReopenSource(lifecycle_.context, continuationOffset_);
    if (!status.ok()) {
      return status;
    }
  }
  reductionActive_ = false;
  return PdfStatus::success();
}

PdfStatus PdfRunStore::readStoredRun(const uint32_t ordinal, PdfTextRun* const run) const {
  if (!initialized_ || run == nullptr || ordinal >= runCount_) {
    return PdfStatus::failure(PdfError::InvalidOffset, ordinal);
  }
  if (ordinal < workspace_.memoryRunCapacity) {
    *run = workspace_.memoryRuns[ordinal];
    return PdfStatus::success();
  }
  if (!reductionActive_) {
    return PdfStatus::failure(PdfError::IoFailure, ordinal);
  }
  return pdfReadRecord(workspace_.spillRuns, ordinal - workspace_.memoryRunCapacity, run);
}

PdfStatus PdfRunStore::readRun(const uint32_t ordinal, PdfTextRun* const run) const {
  const PdfStatus status = readStoredRun(ordinal, run);
  if (status.ok()) {
    run->flags &= static_cast<uint16_t>(~TextInSpillFlag);
  }
  return status;
}

PdfStatus PdfRunStore::readText(const uint32_t ordinal, const uint32_t offset, uint8_t* const destination,
                                const size_t requested, size_t* const bytesRead) const {
  if (destination == nullptr || bytesRead == nullptr) {
    return PdfStatus::failure(PdfError::InvalidArgument, ordinal);
  }
  PdfTextRun run{};
  PdfStatus status = readStoredRun(ordinal, &run);
  if (!status.ok()) {
    return status;
  }
  if (offset > run.textLength) {
    return PdfStatus::failure(PdfError::InvalidOffset, offset);
  }
  const size_t count = std::min<size_t>(requested, run.textLength - offset);
  *bytesRead = 0;
  if (count == 0) {
    return PdfStatus::success();
  }
  if ((run.flags & TextInSpillFlag) != 0) {
    if (!reductionActive_) {
      return PdfStatus::failure(PdfError::IoFailure, ordinal);
    }
    const uint64_t sourceOffset = static_cast<uint64_t>(run.textOffset) + offset;
    const PdfStatus readStatus =
        workspace_.spillText.readAt(workspace_.spillText.context, sourceOffset, destination, count, bytesRead);
    if (!readStatus.ok()) {
      return readStatus;
    }
    return *bytesRead <= count ? PdfStatus::success() : PdfStatus::failure(PdfError::Malformed, sourceOffset);
  }
  if (run.textOffset > memoryTextLength_ || offset > memoryTextLength_ - run.textOffset ||
      count > memoryTextLength_ - run.textOffset - offset) {
    return PdfStatus::failure(PdfError::InvalidOffset, run.textOffset);
  }
  std::memcpy(destination, workspace_.memoryText + run.textOffset + offset, count);
  *bytesRead = count;
  return PdfStatus::success();
}

PdfStatus PdfRunStore::readTextExact(const uint32_t ordinal, const uint32_t offset, uint8_t* const destination,
                                     const size_t length) const {
  if ((destination == nullptr && length != 0) || length > std::numeric_limits<uint32_t>::max()) {
    return PdfStatus::failure(PdfError::InvalidArgument, ordinal);
  }
  PdfTextRun run{};
  PdfStatus status = readRun(ordinal, &run);
  if (!status.ok()) {
    return status;
  }
  if (offset > run.textLength || length > run.textLength - offset) {
    return PdfStatus::failure(PdfError::InvalidOffset, offset);
  }
  size_t completed = 0;
  while (completed < length) {
    size_t bytesRead = 0;
    status = readText(ordinal, offset + static_cast<uint32_t>(completed), destination + completed, length - completed,
                      &bytesRead);
    if (!status.ok()) {
      return status;
    }
    if (bytesRead == 0) {
      return PdfStatus::failure(PdfError::UnexpectedEof, offset + completed);
    }
    completed += bytesRead;
  }
  return PdfStatus::success();
}
