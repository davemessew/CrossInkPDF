#include "PdfResourceTracker.h"

#include <algorithm>

PdfResourceTracker::PdfResourceTracker(const PdfResourceHooks hooks) : hooks_(hooks) {}

PdfResourceSnapshot PdfResourceTracker::measure() const {
  return hooks_.measure == nullptr ? PdfResourceSnapshot{} : hooks_.measure(hooks_.context);
}

void PdfResourceTracker::emit(const PdfResourceEventKind event, const PdfResourceKind resource, const size_t bytes,
                              const PdfResourceSnapshot& snapshot) {
  if (hooks_.event == nullptr) {
    return;
  }
  hooks_.event(hooks_.context, {event, resource, bytes, currentBytes_, peakBytes_, snapshot});
}

bool PdfResourceTracker::canStart() {
  const PdfResourceSnapshot snapshot = measure();
  const bool accepted =
      snapshot.freeHeap >= PDF_MIN_FREE_HEAP_BYTES && snapshot.largestBlock >= PDF_MIN_LARGEST_BLOCK_BYTES;
  emit(accepted ? PdfResourceEventKind::StartAccepted : PdfResourceEventKind::StartRejected,
       PdfResourceKind::InflateDictionary, 0, snapshot);
  return accepted;
}

bool PdfResourceTracker::acquire(const PdfResourceKind kind, const size_t bytes) {
  for (size_t index = 0; index < liveCount_; ++index) {
    if (live_[index].kind == kind) {
      emit(PdfResourceEventKind::AccountingRejected, kind, bytes);
      return false;
    }
  }
  if (liveCount_ >= live_.size() || bytes > PDF_MAX_OWNED_HEAP_BYTES ||
      currentBytes_ > PDF_MAX_OWNED_HEAP_BYTES - bytes) {
    emit(PdfResourceEventKind::AccountingRejected, kind, bytes);
    return false;
  }

  live_[liveCount_++] = {kind, bytes};
  currentBytes_ += bytes;
  peakBytes_ = std::max(peakBytes_, currentBytes_);
  emit(PdfResourceEventKind::Acquired, kind, bytes);
  return true;
}

bool PdfResourceTracker::release(const PdfResourceKind kind) {
  if (liveCount_ == 0 || live_[liveCount_ - 1].kind != kind) {
    emit(PdfResourceEventKind::AccountingRejected, kind, 0);
    return false;
  }

  const size_t bytes = live_[liveCount_ - 1].bytes;
  --liveCount_;
  currentBytes_ -= bytes;
  emit(PdfResourceEventKind::Released, kind, bytes);
  return true;
}

bool PdfResourceTracker::runtimeWithinLimits() {
  const PdfResourceSnapshot snapshot = measure();
  const bool accepted = currentBytes_ <= PDF_MAX_OWNED_HEAP_BYTES && snapshot.stackMargin >= PDF_MIN_STACK_MARGIN_BYTES;
  if (!accepted) {
    emit(PdfResourceEventKind::RuntimeRejected, PdfResourceKind::InflateDictionary, 0, snapshot);
  }
  return accepted;
}
