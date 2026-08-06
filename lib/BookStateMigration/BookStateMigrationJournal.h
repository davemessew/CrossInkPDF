#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>

namespace BookStateMigration {

constexpr char kSlotAPath[] = "/.crosspoint/book_move.a";
constexpr char kSlotBPath[] = "/.crosspoint/book_move.b";
constexpr size_t kMaxPathBytes = 1023;
constexpr size_t kEncodedPrefixBytes = 40;
constexpr size_t kEncodedCrcBytes = 4;
constexpr size_t kScratchCapacity = kEncodedPrefixBytes + (2 * kMaxPathBytes) + kEncodedCrcBytes;

enum class Status : uint8_t {
  Ok,
  NotFound,
  InvalidArgument,
  ScratchTooSmall,
  LimitExceeded,
  IoFailure,
  Corrupt,
  OperationFailed,
  Conflict,
};

enum class OpenMode : uint8_t {
  Read,
  WriteTruncate,
};

struct Handle {
  uintptr_t value = std::numeric_limits<uintptr_t>::max();

  bool valid() const { return value != std::numeric_limits<uintptr_t>::max(); }
  void invalidate() { value = std::numeric_limits<uintptr_t>::max(); }
};

struct Io {
  void* context = nullptr;
  Status (*open)(void*, const char*, OpenMode, Handle*) = nullptr;
  Status (*size)(void*, Handle, size_t*) = nullptr;
  Status (*read)(void*, Handle, size_t, uint8_t*, size_t, size_t*) = nullptr;
  Status (*write)(void*, Handle, const uint8_t*, size_t, size_t*) = nullptr;
  Status (*flush)(void*, Handle) = nullptr;
  Status (*sync)(void*, Handle) = nullptr;
  // close must invalidate the handle even when it reports a durability error.
  Status (*close)(void*, Handle*) = nullptr;
  Status (*remove)(void*, const char*) = nullptr;

  bool valid() const;
};

struct Scratch {
  // Integration owns this 2,090-byte cold-path buffer. Allocate it fallibly
  // off-stack, log allocation failure there, and then construct Journal.
  uint8_t* data = nullptr;
  size_t capacity = 0;
};

struct StringView {
  const char* data = nullptr;
  size_t length = 0;
};

enum class BookFormat : uint8_t {
  Unknown = 0,
  // Value 1 remains unused so an accidental legacy/non-PDF record is rejected.
  Pdf = 2,
};

enum class RecentsPolicy : uint8_t {
  Keep = 0,
  Remove = 1,
};

enum class Phase : uint8_t {
  Prepared = 1,
  SourceMoved,
  CacheCopied,
  BookmarksCopied,
  ClippingsCopied,
  StateVerified,
  Activated,
  OldStateRemoved,
  Abandoned,
};

struct Record {
  uint32_t sequence = 0;
  Phase phase = Phase::Prepared;
  BookFormat format = BookFormat::Unknown;
  uint64_t oldHash = 0;
  uint64_t newHash = 0;
  StringView oldPath{};
  StringView newPath{};
  // Stored in the journal so boot recovery applies the same user-visible
  // recent-books policy as the move that was originally requested.
  RecentsPolicy recentsPolicy = RecentsPolicy::Keep;
};

enum class Slot : uint8_t {
  A,
  B,
};

struct Selection {
  bool selected = false;
  Slot slot = Slot::A;
  // Path views point into Scratch and remain valid only until the next Journal
  // or codec call that uses the same buffer.
  Record record{};
};

bool sequenceNewer(uint32_t candidate, uint32_t reference);
Status encode(const Record& record, Scratch scratch, size_t* encodedLength);
Status decode(const uint8_t* bytes, size_t length, Record* record);
Status reseal(uint8_t* bytes, size_t length);

enum class HashResolution : uint8_t {
  None,
  OldHashFallback,
  NewHash,
};

struct ReadState {
  Status status = Status::Ok;
  bool migrationPresent = false;
  HashResolution resolution = HashResolution::None;
  uint64_t hash = 0;
  Phase phase = Phase::Prepared;
};

class Journal {
 public:
  Journal(Io io, Scratch scratch);

  Status load(Selection* selection);
  Status begin(const Record& record, Selection* committed);
  Status advance(const Selection& current, Phase phase, Selection* committed);
  Status cleanup(const Selection& terminal);
  ReadState readState();

 private:
  Io io_{};
  Scratch scratch_{};
};

enum class SourceLocation : uint8_t {
  OldOnly,
  NewOnly,
  Both,
  Missing,
};

struct SourceObservation {
  Status status = Status::Ok;
  SourceLocation location = SourceLocation::Missing;
};

using MigrationCallback = Status (*)(void*, const Record&);

struct OldState {
  StringView path{};
  uint64_t hash = 0;
  BookFormat format = BookFormat::Unknown;
};

using OldStateCallback = Status (*)(void*, const OldState&);

struct MigrationOperations {
  void* context = nullptr;
  SourceObservation (*locateSource)(void*, const Record&) = nullptr;
  MigrationCallback renameSource = nullptr;
  MigrationCallback copyCache = nullptr;
  MigrationCallback verifyCache = nullptr;
  MigrationCallback copyBookmarks = nullptr;
  MigrationCallback verifyBookmarks = nullptr;
  MigrationCallback copyClippings = nullptr;
  MigrationCallback verifyClippings = nullptr;
  MigrationCallback verifyState = nullptr;
  MigrationCallback activateRecent = nullptr;
  MigrationCallback activateOpenPath = nullptr;
  MigrationCallback verifyActivation = nullptr;
  OldStateCallback removeOldState = nullptr;

  bool valid() const;
};

enum class StepDisposition : uint8_t {
  Idle,
  Advanced,
  Complete,
  Abandoned,
};

struct StepResult {
  Status status = Status::Ok;
  StepDisposition disposition = StepDisposition::Idle;
  Phase durablePhase = Phase::Prepared;
  ReadState read{};
};

StepResult recoverOne(Journal& journal, const MigrationOperations& operations);

static_assert(kScratchCapacity == 2090, "Serialized journal scratch budget changed");
static_assert(kScratchCapacity > 256, "Journal scratch must remain caller-owned, never an implicit stack local");
static_assert(sizeof(Journal) <= 128, "Journal wrapper must not embed the record scratch buffer");

}  // namespace BookStateMigration
