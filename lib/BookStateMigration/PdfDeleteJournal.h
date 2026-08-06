#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>

namespace PdfDelete {

constexpr char kSlotAPath[] = "/.crosspoint/pdf_delete.a";
constexpr char kSlotBPath[] = "/.crosspoint/pdf_delete.b";
constexpr char kTombstoneSuffix[] = ".crossink-delete";
constexpr size_t kMaxPathBytes = 1023;
constexpr size_t kTargetCount = 6;
constexpr size_t kEncodedPrefixBytes = 40;
constexpr size_t kEncodedCrcBytes = 4;
constexpr size_t kScratchCapacity = kEncodedPrefixBytes + (kTargetCount * kMaxPathBytes) + kEncodedCrcBytes;

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
  // Integration owns this 6,182-byte cold-path workspace. Allocate it
  // fallibly off-stack, log allocation failure there, and reuse it for the
  // lifetime of Journal.
  uint8_t* data = nullptr;
  size_t capacity = 0;
};

struct StringView {
  const char* data = nullptr;
  size_t length = 0;
};

enum class BookFormat : uint8_t {
  Unknown = 0,
  Pdf = 1,
};

enum class Phase : uint8_t {
  Prepared = 1,
  SourceHidden,
  FullCachePurged,
  BookmarksPurged,
  ClippingsPurged,
  RecentsPurged,
  SourceRemoved,
};

struct Targets {
  StringView source{};
  StringView tombstone{};
  StringView cache{};
  StringView bookmarks{};
  StringView clippings{};
  StringView recent{};
};

struct Record {
  uint32_t sequence = 0;
  Phase phase = Phase::Prepared;
  BookFormat format = BookFormat::Unknown;
  Targets targets{};
};

enum class Slot : uint8_t {
  A,
  B,
};

struct Selection {
  bool selected = false;
  Slot slot = Slot::A;
  // Views point into Scratch and remain valid only until the next Journal call.
  Record record{};
};

enum class BeginDisposition : uint8_t {
  // This call did not leave a matching Prepared intent behind.
  SafeFailure,
  // A matching Prepared intent is durable. The same Coordinator may abandon it
  // until a deletion step is attempted.
  Armed,
  // Slot state could not be proven absent or matching. Callers must not assume
  // that a failed request is safe to forget.
  Indeterminate,
};

struct BeginResult {
  // Preserves the original operation status for diagnostics. Disposition is
  // authoritative because an I/O error can still leave a durable intent.
  Status status = Status::Ok;
  BeginDisposition disposition = BeginDisposition::SafeFailure;
  bool canAbandon = false;
};

bool sequenceNewer(uint32_t candidate, uint32_t reference);
Status formatTombstonePath(StringView source, char* destination, size_t capacity, size_t* length);
Status validateDeleteTargets(const Targets& targets);
Status encode(const Record& record, Scratch scratch, size_t* encodedLength);
Status decode(const uint8_t* bytes, size_t length, Record* record);

class Journal {
 public:
  Journal(Io io, Scratch scratch);

  Status load(Selection* selection);
  BeginResult begin(const Record& record, Selection* committed);
  Status advance(const Selection& current, Phase phase, Selection* committed);
  Status cleanup(const Selection& terminal);
  // Coordinator may call this only for its own begin attempt before step().
  // Slot B is removed first so a failed cleanup cannot erase the only
  // potentially valid Prepared record in slot A.
  Status discardBeginAttempt();
  Status discardPrepared(const Selection& prepared);

 private:
  Io io_{};
  Scratch scratch_{};
};

static_assert(kScratchCapacity == 6182, "Serialized PDF deletion scratch budget changed");
static_assert(kScratchCapacity > 256, "Journal scratch must remain caller-owned, never an implicit stack local");
static_assert(sizeof(Journal) <= 128, "Journal wrapper must not embed record scratch");
static_assert(sizeof(BeginResult) <= 4, "Begin result must remain a compact value");

}  // namespace PdfDelete
