#pragma once

#include <cstddef>
#include <cstdint>

#include "BookStateMigrationJournal.h"

namespace BookStateMigration {

constexpr uint8_t kMaxRecoverySteps = 9;

struct Request {
  StringView oldPath{};
  StringView newPath{};
  BookFormat format = BookFormat::Unknown;
  uint64_t oldHash = 0;
  uint64_t newHash = 0;
  RecentsPolicy recentsPolicy = RecentsPolicy::Keep;
};

enum class RunDisposition : uint8_t {
  Idle,
  Pending,
  Complete,
  Abandoned,
};

struct RunResult {
  Status status = Status::Ok;
  RunDisposition disposition = RunDisposition::Idle;
  Phase durablePhase = Phase::Prepared;
  ReadState read{};
  uint8_t steps = 0;
};

class Coordinator {
 public:
  Coordinator(Journal& journal, const MigrationOperations& operations);

  // Commits Prepared without invoking any domain operation. Callers can
  // therefore prove that the recovery record is durable before source rename.
  Status begin(const Request& request);
  StepResult step();
  RunResult recover(uint8_t maxSteps = kMaxRecoverySteps);
  ReadState readState();

 private:
  Journal& journal_;
  const MigrationOperations& operations_;
};

static_assert(kMaxRecoverySteps == 9, "Recovery remains a fixed, non-looping cold-path budget");
static_assert(sizeof(Coordinator) <= 32, "Coordinator must retain references only, never migration buffers");

}  // namespace BookStateMigration
