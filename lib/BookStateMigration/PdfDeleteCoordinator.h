#pragma once

#include <cstdint>

#include "PdfDeleteJournal.h"

namespace PdfDelete {

constexpr uint8_t kMaxRecoverySteps = 7;

using TargetCallback = Status (*)(void*, const Targets&);

struct Operations {
  void* context = nullptr;
  // The adapter must canonicalize and re-check every supplied target. In
  // particular, source and tombstone must be on the same mounted SD volume,
  // and cache/store/recent targets must belong only to source.
  TargetCallback validateTargets = nullptr;
  // Atomically rename source to the deterministic tombstone. This callback
  // must fail on source+tombstone collision and accept tombstone-only as an
  // idempotent replay.
  TargetCallback hideSource = nullptr;
  // All mutation callbacks are replayable. Missing exact targets are success;
  // implementations must never enumerate or recursively delete a broad root.
  TargetCallback purgeFullCache = nullptr;
  TargetCallback purgeBookmarks = nullptr;
  TargetCallback purgeClippings = nullptr;
  TargetCallback purgeRecents = nullptr;
  // Remove only the exact hidden source and accept an already-missing file.
  TargetCallback removeHiddenSource = nullptr;

  bool valid() const;
};

struct Request {
  Targets targets{};
  BookFormat format = BookFormat::Unknown;
};

enum class AbandonDisposition : uint8_t {
  Cleared,
  Armed,
  Indeterminate,
};

struct AbandonResult {
  // Preserves the first failure for diagnostics. Disposition reports the
  // reconciled journal state and is authoritative.
  Status status = Status::Ok;
  AbandonDisposition disposition = AbandonDisposition::Indeterminate;
};

enum class StepDisposition : uint8_t {
  Idle,
  Advanced,
  Complete,
  Pending,
};

struct StepResult {
  Status status = Status::Ok;
  StepDisposition disposition = StepDisposition::Idle;
  Phase durablePhase = Phase::Prepared;
};

enum class RunDisposition : uint8_t {
  Idle,
  Pending,
  Complete,
};

struct RunResult {
  Status status = Status::Ok;
  RunDisposition disposition = RunDisposition::Idle;
  Phase durablePhase = Phase::Prepared;
  uint8_t steps = 0;
};

class Coordinator {
 public:
  Coordinator(Journal& journal, const Operations& operations);

  // Commits and read-verifies Prepared without invoking a destructive callback.
  // Callers must branch on disposition, not status alone: an I/O error may
  // still report Armed when reconciliation finds the matching durable intent.
  BeginResult begin(const Request& request);
  // Allowed only on this Coordinator instance after begin and before step.
  // Once a rename callback has been attempted, its result is crash-ambiguous:
  // recovery must finish deletion and must never restore source visibility.
  AbandonResult abandonPrepared();
  // Performs at most one adapter mutation and then commits its next phase.
  StepResult step();
  RunResult recover(uint8_t maxSteps = kMaxRecoverySteps);

 private:
  Journal& journal_;
  const Operations& operations_;
  bool beganHere_ = false;
  bool stepAttempted_ = false;
};

static_assert(kMaxRecoverySteps == 7, "Deletion recovery remains a fixed cold-path budget");
static_assert(sizeof(Coordinator) <= 32, "Coordinator must retain references and latches only");
static_assert(sizeof(AbandonResult) <= 2, "Abandon result must remain a compact value");

}  // namespace PdfDelete
