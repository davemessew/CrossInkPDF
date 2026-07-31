#include "PdfDeleteCoordinator.h"

namespace PdfDelete {
namespace {

StepResult failure(const Status status, const Selection& selection) {
  return {status, StepDisposition::Pending, selection.selected ? selection.record.phase : Phase::Prepared};
}

StepResult advanceResult(Journal& journal, const Selection& selection, const Phase next) {
  Selection committed{};
  const Status status = journal.advance(selection, next, &committed);
  if (status != Status::Ok) return failure(status, selection);
  return {Status::Ok, StepDisposition::Advanced, next};
}

BeginResult classifyPreflightFailure(Journal& journal, const Status status, const bool locallyAbandonable) {
  Selection existing{};
  const Status loaded = journal.load(&existing);
  if (loaded == Status::Ok && !existing.selected) {
    return {status, BeginDisposition::SafeFailure, false};
  }
  const bool canAbandon =
      locallyAbandonable && (loaded != Status::Ok || (existing.selected && existing.record.phase == Phase::Prepared));
  return {status, BeginDisposition::Indeterminate, canAbandon};
}

}  // namespace

bool Operations::valid() const {
  return context != nullptr && validateTargets != nullptr && hideSource != nullptr && purgeFullCache != nullptr &&
         purgeBookmarks != nullptr && purgeClippings != nullptr && purgeRecents != nullptr &&
         removeHiddenSource != nullptr;
}

Coordinator::Coordinator(Journal& journal, const Operations& operations) : journal_(journal), operations_(operations) {}

BeginResult Coordinator::begin(const Request& request) {
  if (!operations_.valid() || request.format != BookFormat::Pdf) {
    return classifyPreflightFailure(journal_, Status::InvalidArgument, beganHere_ && !stepAttempted_);
  }
  Status status = validateDeleteTargets(request.targets);
  if (status != Status::Ok) {
    return classifyPreflightFailure(journal_, status, beganHere_ && !stepAttempted_);
  }
  status = operations_.validateTargets(operations_.context, request.targets);
  if (status != Status::Ok) {
    return classifyPreflightFailure(journal_, status, beganHere_ && !stepAttempted_);
  }

  const Record prepared{0, Phase::Prepared, request.format, request.targets};
  const BeginResult result = journal_.begin(prepared, nullptr);
  if (result.canAbandon) {
    beganHere_ = true;
    stepAttempted_ = false;
  }
  return result;
}

AbandonResult Coordinator::abandonPrepared() {
  if (!beganHere_ || stepAttempted_) return {Status::Conflict, AbandonDisposition::Indeterminate};
  const Status status = journal_.discardBeginAttempt();
  if (status == Status::Ok) {
    beganHere_ = false;
    return {Status::Ok, AbandonDisposition::Cleared};
  }

  Selection reconciled{};
  const Status reconciliation = journal_.load(&reconciled);
  if (reconciliation != Status::Ok) return {status, AbandonDisposition::Indeterminate};
  if (!reconciled.selected) {
    beganHere_ = false;
    return {status, AbandonDisposition::Cleared};
  }
  if (reconciled.record.phase == Phase::Prepared) {
    return {status, AbandonDisposition::Armed};
  }
  return {status, AbandonDisposition::Indeterminate};
}

StepResult Coordinator::step() {
  stepAttempted_ = true;
  if (!operations_.valid()) return {Status::InvalidArgument, StepDisposition::Pending, Phase::Prepared};

  Selection selection{};
  Status status = journal_.load(&selection);
  if (status != Status::Ok) return failure(status, selection);
  if (!selection.selected) return {Status::Ok, StepDisposition::Idle, Phase::Prepared};

  status = validateDeleteTargets(selection.record.targets);
  if (status == Status::Ok) {
    status = operations_.validateTargets(operations_.context, selection.record.targets);
  }
  if (status != Status::Ok) return failure(status, selection);

  TargetCallback operation = nullptr;
  Phase next = selection.record.phase;
  switch (selection.record.phase) {
    case Phase::Prepared:
      operation = operations_.hideSource;
      next = Phase::SourceHidden;
      break;
    case Phase::SourceHidden:
      operation = operations_.purgeFullCache;
      next = Phase::FullCachePurged;
      break;
    case Phase::FullCachePurged:
      operation = operations_.purgeBookmarks;
      next = Phase::BookmarksPurged;
      break;
    case Phase::BookmarksPurged:
      operation = operations_.purgeClippings;
      next = Phase::ClippingsPurged;
      break;
    case Phase::ClippingsPurged:
      operation = operations_.purgeRecents;
      next = Phase::RecentsPurged;
      break;
    case Phase::RecentsPurged:
      operation = operations_.removeHiddenSource;
      next = Phase::SourceRemoved;
      break;
    case Phase::SourceRemoved:
      status = journal_.cleanup(selection);
      return status == Status::Ok ? StepResult{Status::Ok, StepDisposition::Complete, Phase::SourceRemoved}
                                  : failure(status, selection);
  }

  status = operation(operations_.context, selection.record.targets);
  if (status != Status::Ok) return failure(status, selection);
  return advanceResult(journal_, selection, next);
}

RunResult Coordinator::recover(const uint8_t maxSteps) {
  RunResult result{};
  if (maxSteps == 0 || maxSteps > kMaxRecoverySteps) {
    result.status = Status::InvalidArgument;
    result.disposition = RunDisposition::Pending;
    return result;
  }

  for (uint8_t index = 0; index < maxSteps; ++index) {
    const StepResult stepped = step();
    result.status = stepped.status;
    result.durablePhase = stepped.durablePhase;
    result.steps = static_cast<uint8_t>(index + 1U);
    if (stepped.status != Status::Ok) {
      result.disposition = RunDisposition::Pending;
      return result;
    }
    if (stepped.disposition == StepDisposition::Complete) {
      result.disposition = RunDisposition::Complete;
      return result;
    }
    if (stepped.disposition == StepDisposition::Idle) {
      result.disposition = RunDisposition::Idle;
      return result;
    }
  }
  result.disposition = RunDisposition::Pending;
  return result;
}

}  // namespace PdfDelete
