#include "BookMoveCoordinator.h"

namespace BookStateMigration {

Coordinator::Coordinator(Journal& journal, const MigrationOperations& operations)
    : journal_(journal), operations_(operations) {}

Status Coordinator::begin(const Request& request) {
  if (!operations_.valid()) {
    return Status::InvalidArgument;
  }
  if (request.oldHash == request.newHash) {
    return Status::Conflict;
  }
  const Record record{
      0,
      Phase::Prepared,
      request.format,
      request.oldHash,
      request.newHash,
      request.oldPath,
      request.newPath,
      request.recentsPolicy,
  };
  return journal_.begin(record, nullptr);
}

StepResult Coordinator::step() { return recoverOne(journal_, operations_); }

RunResult Coordinator::recover(const uint8_t maxSteps) {
  RunResult result{};
  if (maxSteps == 0 || maxSteps > kMaxRecoverySteps) {
    result.status = Status::InvalidArgument;
    return result;
  }

  for (uint8_t index = 0; index < maxSteps; ++index) {
    const StepResult stepped = step();
    result.status = stepped.status;
    result.durablePhase = stepped.durablePhase;
    result.read = stepped.read;
    result.steps = static_cast<uint8_t>(index + 1U);

    if (stepped.status != Status::Ok) {
      result.disposition = RunDisposition::Pending;
      return result;
    }
    if (stepped.disposition == StepDisposition::Complete) {
      result.disposition = RunDisposition::Complete;
      return result;
    }
    if (stepped.disposition == StepDisposition::Abandoned) {
      result.disposition = RunDisposition::Abandoned;
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

ReadState Coordinator::readState() { return journal_.readState(); }

}  // namespace BookStateMigration
