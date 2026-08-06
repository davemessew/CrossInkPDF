#include "PdfIo.h"

#include <algorithm>
#include <limits>

#include "PdfCheckedMath.h"

namespace {

PdfStatus readByteRange(void* context, const uint64_t offset, uint8_t* destination, const size_t requested,
                        size_t* bytesRead) {
  if (context == nullptr) {
    return PdfStatus::failure(PdfError::InvalidArgument, offset);
  }
  auto& range = *static_cast<PdfByteRange*>(context);
  if (!pdfCheckedRange(offset, requested, range.length)) {
    return PdfStatus::failure(PdfError::InvalidOffset, offset);
  }
  uint64_t parentOffset = 0;
  if (!pdfCheckedAdd(range.offset, offset, &parentOffset)) {
    return PdfStatus::failure(PdfError::InvalidOffset, offset);
  }
  return range.parent.readAt(range.parent.context, parentOffset, destination, requested, bytesRead);
}

PdfStatus readByteStore(void* context, const uint64_t offset, uint8_t* destination, const size_t requested,
                        size_t* bytesRead) {
  if (context == nullptr) {
    return PdfStatus::failure(PdfError::InvalidArgument, offset);
  }
  auto& store = *static_cast<PdfByteStore*>(context);
  return store.readAt(store.context, offset, destination, requested, bytesRead);
}

PdfStatus writeByteStore(void* context, const uint8_t* source, const size_t requested, size_t* bytesWritten) {
  if (context == nullptr) {
    return PdfStatus::failure(PdfError::InvalidArgument);
  }
  auto& store = *static_cast<PdfByteStore*>(context);
  return store.write(store.context, source, requested, bytesWritten);
}

}  // namespace

PdfStatus pdfInitializeByteRange(const PdfByteSource& parent, const uint64_t offset, const uint64_t length,
                                 PdfByteRange* range) {
  if (range == nullptr || !parent.valid()) {
    return PdfStatus::failure(PdfError::InvalidArgument, offset);
  }
  if (!pdfCheckedRange(offset, length, parent.size)) {
    return PdfStatus::failure(PdfError::InvalidOffset, offset);
  }
  *range = {parent, offset, length};
  return PdfStatus::success();
}

PdfByteSource pdfByteRangeSource(PdfByteRange& range) { return {&range, range.length, readByteRange}; }

PdfStepResult pdfStepReadExact(const PdfByteSource& source, PdfReadExactState& state, PdfWorkBudget& budget) {
  if (!source.valid() || (state.destination == nullptr && state.length != 0) || state.completed > state.length) {
    return PdfStepResult::failure(PdfStatus::failure(PdfError::InvalidArgument, state.sourceOffset));
  }
  if (!pdfCheckedRange(state.sourceOffset, state.length, source.size)) {
    return PdfStepResult::failure(PdfStatus::failure(PdfError::InvalidOffset, state.sourceOffset));
  }
  if (state.completed == state.length) {
    return PdfStepResult::completed();
  }

  while (state.completed < state.length) {
    if (budget.cancelRequested()) {
      return PdfStepResult::failure(PdfStatus::failure(PdfError::Cancelled, state.sourceOffset + state.completed));
    }
    if (!budget.consumeOperation()) {
      return PdfStepResult::paused();
    }

    const size_t remaining = state.length - state.completed;
    const size_t requested = budget.takeBytes(remaining);
    if (requested == 0) {
      return PdfStepResult::paused();
    }

    uint64_t readOffset = 0;
    if (!pdfCheckedAdd(state.sourceOffset, state.completed, &readOffset)) {
      return PdfStepResult::failure(PdfStatus::failure(PdfError::InvalidOffset, state.sourceOffset));
    }

    size_t bytesRead = 0;
    const PdfStatus status =
        source.readAt(source.context, readOffset, state.destination + state.completed, requested, &bytesRead);
    if (!status.ok()) {
      return PdfStepResult::failure(status);
    }
    if (bytesRead > requested) {
      return PdfStepResult::failure(PdfStatus::failure(PdfError::IoFailure, readOffset));
    }
    if (bytesRead == 0) {
      return PdfStepResult::failure(PdfStatus::failure(PdfError::UnexpectedEof, readOffset));
    }
    state.completed += bytesRead;
  }

  return PdfStepResult::completed();
}

PdfStatus pdfReadExact(const PdfByteSource& source, const uint64_t offset, uint8_t* destination, const size_t length) {
  PdfReadExactState state{offset, destination, length, 0};
  while (true) {
    PdfWorkBudget budget{std::numeric_limits<uint32_t>::max(), std::numeric_limits<size_t>::max()};
    const PdfStepResult result = pdfStepReadExact(source, state, budget);
    if (result.complete()) {
      return result.status;
    }
    if (result.failed()) {
      return result.status;
    }
  }
}

PdfStatus pdfWriteExact(const PdfByteSink& sink, const uint8_t* source, const size_t length) {
  if (!sink.valid() || (source == nullptr && length != 0)) {
    return PdfStatus::failure(PdfError::InvalidArgument);
  }
  size_t completed = 0;
  while (completed < length) {
    size_t bytesWritten = 0;
    const PdfStatus status = sink.write(sink.context, source + completed, length - completed, &bytesWritten);
    if (!status.ok()) {
      return status;
    }
    if (bytesWritten == 0 || bytesWritten > length - completed) {
      return PdfStatus::failure(PdfError::IoFailure, completed);
    }
    completed += bytesWritten;
  }
  return PdfStatus::success();
}

PdfStatus pdfReadRecord(const PdfFixedRecordStore& store, const uint32_t ordinal, void* record) {
  if (!store.valid() || record == nullptr) {
    return PdfStatus::failure(PdfError::InvalidArgument, ordinal);
  }
  if (ordinal >= store.capacity) {
    return PdfStatus::failure(PdfError::InvalidOffset, ordinal);
  }
  return store.read(store.context, ordinal, record, store.recordSize);
}

PdfStatus pdfWriteRecord(const PdfFixedRecordStore& store, const uint32_t ordinal, const void* record) {
  if (!store.valid() || record == nullptr) {
    return PdfStatus::failure(PdfError::InvalidArgument, ordinal);
  }
  if (ordinal >= store.capacity) {
    return PdfStatus::failure(PdfError::InvalidOffset, ordinal);
  }
  return store.write(store.context, ordinal, record, store.recordSize);
}

PdfByteSource pdfByteStoreSource(PdfByteStore& store) {
  if (!store.valid()) {
    return {};
  }
  const uint64_t size = store.size(store.context);
  if (size > store.capacity) {
    return {};
  }
  return {&store, size, readByteStore};
}

PdfByteSink pdfByteStoreSink(PdfByteStore& store) {
  return store.valid() ? PdfByteSink{&store, writeByteStore} : PdfByteSink{};
}
