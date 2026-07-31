#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <numeric>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "PdfDeleteCoordinator.h"

namespace {

using namespace PdfDelete;

constexpr char kSource[] = "/Books/book.pdf";
constexpr char kTombstone[] = "/Books/.book.pdf.crossink-delete";
constexpr char kCache[] = "/.crosspoint/pdf-cache/01234567";
constexpr char kBookmarks[] = "/.crosspoint/bookmarks/01234567";
constexpr char kClippings[] = "/.crosspoint/clippings/01234567";
constexpr char kRecent[] = "/.crosspoint/recent/01234567";
constexpr char kUnrelatedSource[] = "/Books/other.pdf";
constexpr char kUnrelatedTombstone[] = "/Books/.other.pdf.crossink-delete";
constexpr char kUnrelatedCache[] = "/.crosspoint/pdf-cache/89abcdef";
constexpr char kUnrelatedBookmarks[] = "/.crosspoint/bookmarks/89abcdef";
constexpr char kUnrelatedClippings[] = "/.crosspoint/clippings/89abcdef";
constexpr char kUnrelatedRecent[] = "/.crosspoint/recent/89abcdef";

int failures = 0;

void expect(const bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

StringView view(const char* const text) { return {text, std::strlen(text)}; }

std::string text(const StringView value) {
  return value.data == nullptr ? std::string{} : std::string(value.data, value.length);
}

bool armedBegin(const BeginResult& result) {
  return result.status == Status::Ok && result.disposition == BeginDisposition::Armed && result.canAbandon;
}

bool safeBeginFailure(const BeginResult& result, const Status status) {
  return result.status == status && result.disposition == BeginDisposition::SafeFailure && !result.canAbandon;
}

Request request(const BookFormat format = BookFormat::Pdf) {
  return {{
              view(kSource),
              view(kTombstone),
              view(kCache),
              view(kBookmarks),
              view(kClippings),
              view(kRecent),
          },
          format};
}

enum class IoFaultPoint : uint8_t {
  None,
  OpenRead,
  OpenWrite,
  Size,
  Read,
  ShortRead,
  Write,
  ShortWrite,
  Flush,
  Sync,
  CloseRead,
  CloseWrite,
  RemoveBefore,
  RemoveAfter,
};

struct MemoryFile {
  std::vector<uint8_t> bytes;
};

struct FaultPlan {
  IoFaultPoint point = IoFaultPoint::None;
  uint32_t skip = 0;
  std::string path;
};

struct MemoryIo {
  std::unordered_map<std::string, MemoryFile> files;
  std::string openPath;
  OpenMode openMode = OpenMode::Read;
  size_t openHandles = 0;
  size_t maxOpenHandles = 0;
  std::vector<std::string> events;
  std::vector<FaultPlan> faults;

  void arm(const IoFaultPoint point, const uint32_t skip = 0, std::string path = {}) {
    faults.push_back({point, skip, std::move(path)});
  }

  void clearFault() { faults.clear(); }

  bool consume(const IoFaultPoint point, const std::string& path) {
    for (auto fault = faults.begin(); fault != faults.end(); ++fault) {
      if (fault->point != point || (!fault->path.empty() && fault->path != path)) continue;
      if (fault->skip > 0) {
        --fault->skip;
        return false;
      }
      faults.erase(fault);
      return true;
    }
    return false;
  }

  bool exists(const char* const path) const { return files.find(path) != files.end(); }

  const std::vector<uint8_t>& bytes(const char* const path) const { return files.at(path).bytes; }

  std::vector<uint8_t>& mutableBytes(const char* const path) { return files.at(path).bytes; }

  void put(const char* const path, const std::vector<uint8_t>& bytes) { files[path].bytes = bytes; }

  void putPrefix(const char* const path, const std::vector<uint8_t>& bytes, const size_t length) {
    files[path].bytes.assign(bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(length));
  }
};

Status openFile(void* const context, const char* const path, const OpenMode mode, Handle* const handle) {
  auto& io = *static_cast<MemoryIo*>(context);
  if (path == nullptr || handle == nullptr || handle->valid() || io.openHandles != 0) {
    return Status::InvalidArgument;
  }
  const std::string pathText(path);
  const IoFaultPoint point = mode == OpenMode::Read ? IoFaultPoint::OpenRead : IoFaultPoint::OpenWrite;
  if (io.consume(point, pathText)) return Status::IoFailure;
  if (mode == OpenMode::Read && !io.exists(path)) return Status::NotFound;

  io.openPath = pathText;
  io.openMode = mode;
  if (mode == OpenMode::WriteTruncate) io.files[pathText].bytes.clear();
  handle->value = 1;
  ++io.openHandles;
  io.maxOpenHandles = std::max(io.maxOpenHandles, io.openHandles);
  io.events.emplace_back(mode == OpenMode::Read ? "journal-open-read:" + pathText : "journal-open-write:" + pathText);
  return Status::Ok;
}

Status fileSize(void* const context, const Handle handle, size_t* const size) {
  auto& io = *static_cast<MemoryIo*>(context);
  if (!handle.valid() || size == nullptr || io.openHandles != 1) return Status::InvalidArgument;
  if (io.consume(IoFaultPoint::Size, io.openPath)) return Status::IoFailure;
  *size = io.files.at(io.openPath).bytes.size();
  return Status::Ok;
}

Status readFile(void* const context, const Handle handle, const size_t offset, uint8_t* const destination,
                const size_t length, size_t* const actual) {
  auto& io = *static_cast<MemoryIo*>(context);
  if (!handle.valid() || destination == nullptr || actual == nullptr || io.openHandles != 1 ||
      io.openMode != OpenMode::Read) {
    return Status::InvalidArgument;
  }
  if (io.consume(IoFaultPoint::Read, io.openPath)) return Status::IoFailure;
  const auto& bytes = io.files.at(io.openPath).bytes;
  if (offset > bytes.size()) return Status::IoFailure;
  size_t readable = std::min(length, bytes.size() - offset);
  if (io.consume(IoFaultPoint::ShortRead, io.openPath) && readable > 0) --readable;
  if (readable > 0) std::memcpy(destination, bytes.data() + offset, readable);
  *actual = readable;
  return Status::Ok;
}

Status writeFile(void* const context, const Handle handle, const uint8_t* const source, const size_t length,
                 size_t* const actual) {
  auto& io = *static_cast<MemoryIo*>(context);
  if (!handle.valid() || source == nullptr || actual == nullptr || io.openHandles != 1 ||
      io.openMode != OpenMode::WriteTruncate) {
    return Status::InvalidArgument;
  }
  if (io.consume(IoFaultPoint::Write, io.openPath)) {
    *actual = 0;
    return Status::IoFailure;
  }
  size_t writable = length;
  if (io.consume(IoFaultPoint::ShortWrite, io.openPath) && writable > 0) --writable;
  auto& bytes = io.files.at(io.openPath).bytes;
  bytes.insert(bytes.end(), source, source + writable);
  *actual = writable;
  io.events.emplace_back("journal-write:" + io.openPath);
  return Status::Ok;
}

Status flushFile(void* const context, const Handle handle) {
  auto& io = *static_cast<MemoryIo*>(context);
  if (!handle.valid() || io.openHandles != 1) return Status::InvalidArgument;
  io.events.emplace_back("journal-flush:" + io.openPath);
  return io.consume(IoFaultPoint::Flush, io.openPath) ? Status::IoFailure : Status::Ok;
}

Status syncFile(void* const context, const Handle handle) {
  auto& io = *static_cast<MemoryIo*>(context);
  if (!handle.valid() || io.openHandles != 1) return Status::InvalidArgument;
  io.events.emplace_back("journal-sync:" + io.openPath);
  return io.consume(IoFaultPoint::Sync, io.openPath) ? Status::IoFailure : Status::Ok;
}

Status closeFile(void* const context, Handle* const handle) {
  auto& io = *static_cast<MemoryIo*>(context);
  if (handle == nullptr || !handle->valid() || io.openHandles != 1) return Status::InvalidArgument;
  const IoFaultPoint point = io.openMode == OpenMode::Read ? IoFaultPoint::CloseRead : IoFaultPoint::CloseWrite;
  const bool fails = io.consume(point, io.openPath);
  io.events.emplace_back("journal-close:" + io.openPath);
  handle->invalidate();
  --io.openHandles;
  return fails ? Status::IoFailure : Status::Ok;
}

Status removeFile(void* const context, const char* const path) {
  auto& io = *static_cast<MemoryIo*>(context);
  if (path == nullptr || io.openHandles != 0) return Status::InvalidArgument;
  const std::string pathText(path);
  io.events.emplace_back("journal-remove:" + pathText);
  if (io.consume(IoFaultPoint::RemoveBefore, pathText)) return Status::IoFailure;
  const size_t removed = io.files.erase(pathText);
  if (io.consume(IoFaultPoint::RemoveAfter, pathText)) return Status::IoFailure;
  return removed == 0 ? Status::NotFound : Status::Ok;
}

Io makeIo(MemoryIo& memory) {
  return {&memory, &openFile, &fileSize, &readFile, &writeFile, &flushFile, &syncFile, &closeFile, &removeFile};
}

enum class Operation : uint8_t {
  Hide = 0,
  Cache,
  Bookmarks,
  Clippings,
  Recent,
  RemoveSource,
};

enum class OperationFault : uint8_t {
  None,
  Before,
  After,
};

struct DurableModel {
  explicit DurableModel(MemoryIo& journalIo) : journalIo(journalIo) {
    files.insert(kSource);
    files.insert(kUnrelatedSource);
    files.insert(kUnrelatedTombstone);
    caches.insert(kCache);
    caches.insert(kUnrelatedCache);
    bookmarkStores.insert(kBookmarks);
    bookmarkStores.insert(kUnrelatedBookmarks);
    clippingStores.insert(kClippings);
    clippingStores.insert(kUnrelatedClippings);
    recentStores.insert(kRecent);
    recentStores.insert(kUnrelatedRecent);
  }

  MemoryIo& journalIo;
  std::unordered_set<std::string> files;
  std::unordered_set<std::string> caches;
  std::unordered_set<std::string> bookmarkStores;
  std::unordered_set<std::string> clippingStores;
  std::unordered_set<std::string> recentStores;
  std::array<size_t, 6> attempts{};
  Operation faultOperation = Operation::Hide;
  OperationFault fault = OperationFault::None;
  bool rejectValidation = false;
  size_t validations = 0;

  void arm(const Operation operation, const OperationFault behavior) {
    faultOperation = operation;
    fault = behavior;
  }

  bool consume(const Operation operation, const OperationFault behavior) {
    if (faultOperation != operation || fault != behavior) return false;
    fault = OperationFault::None;
    return true;
  }

  void event(const Operation operation, const char* const name) {
    ++attempts[static_cast<size_t>(operation)];
    journalIo.events.emplace_back(std::string("op:") + name);
  }
};

bool exactTargets(const Targets& targets) {
  return text(targets.source) == kSource && text(targets.tombstone) == kTombstone && text(targets.cache) == kCache &&
         text(targets.bookmarks) == kBookmarks && text(targets.clippings) == kClippings &&
         text(targets.recent) == kRecent;
}

Status validateTargets(void* const context, const Targets& targets) {
  auto& model = *static_cast<DurableModel*>(context);
  ++model.validations;
  if (!exactTargets(targets)) return Status::InvalidArgument;
  return model.rejectValidation ? Status::Conflict : Status::Ok;
}

Status hideSource(void* const context, const Targets& targets) {
  auto& model = *static_cast<DurableModel*>(context);
  model.event(Operation::Hide, "hide-source");
  if (model.consume(Operation::Hide, OperationFault::Before)) return Status::OperationFailed;

  const std::string source = text(targets.source);
  const std::string tombstone = text(targets.tombstone);
  const bool hasSource = model.files.contains(source);
  const bool hasTombstone = model.files.contains(tombstone);
  if (hasSource && hasTombstone) return Status::Conflict;
  if (!hasSource && !hasTombstone) return Status::OperationFailed;
  if (hasSource) {
    model.files.erase(source);
    model.files.insert(tombstone);
  }
  return model.consume(Operation::Hide, OperationFault::After) ? Status::OperationFailed : Status::Ok;
}

Status purgeCache(void* const context, const Targets& targets) {
  auto& model = *static_cast<DurableModel*>(context);
  model.event(Operation::Cache, "purge-cache");
  if (model.consume(Operation::Cache, OperationFault::Before)) return Status::OperationFailed;
  model.caches.erase(text(targets.cache));
  return model.consume(Operation::Cache, OperationFault::After) ? Status::OperationFailed : Status::Ok;
}

Status purgeBookmarks(void* const context, const Targets& targets) {
  auto& model = *static_cast<DurableModel*>(context);
  model.event(Operation::Bookmarks, "purge-bookmarks");
  if (model.consume(Operation::Bookmarks, OperationFault::Before)) return Status::OperationFailed;
  model.bookmarkStores.erase(text(targets.bookmarks));
  return model.consume(Operation::Bookmarks, OperationFault::After) ? Status::OperationFailed : Status::Ok;
}

Status purgeClippings(void* const context, const Targets& targets) {
  auto& model = *static_cast<DurableModel*>(context);
  model.event(Operation::Clippings, "purge-clippings");
  if (model.consume(Operation::Clippings, OperationFault::Before)) return Status::OperationFailed;
  model.clippingStores.erase(text(targets.clippings));
  return model.consume(Operation::Clippings, OperationFault::After) ? Status::OperationFailed : Status::Ok;
}

Status purgeRecent(void* const context, const Targets& targets) {
  auto& model = *static_cast<DurableModel*>(context);
  model.event(Operation::Recent, "purge-recent");
  if (model.consume(Operation::Recent, OperationFault::Before)) return Status::OperationFailed;
  model.recentStores.erase(text(targets.recent));
  return model.consume(Operation::Recent, OperationFault::After) ? Status::OperationFailed : Status::Ok;
}

Status removeSource(void* const context, const Targets& targets) {
  auto& model = *static_cast<DurableModel*>(context);
  model.event(Operation::RemoveSource, "remove-source");
  if (model.consume(Operation::RemoveSource, OperationFault::Before)) return Status::OperationFailed;
  model.files.erase(text(targets.tombstone));
  return model.consume(Operation::RemoveSource, OperationFault::After) ? Status::OperationFailed : Status::Ok;
}

Operations makeOperations(DurableModel& model) {
  return {&model,          &validateTargets, &hideSource,  &purgeCache,
          &purgeBookmarks, &purgeClippings,  &purgeRecent, &removeSource};
}

struct Fixture {
  MemoryIo memory;
  std::array<uint8_t, kScratchCapacity> scratch{};
  DurableModel model;
  Operations operations;
  Journal journal;
  Coordinator coordinator;

  Fixture()
      : model(memory),
        operations(makeOperations(model)),
        journal(makeIo(memory), {scratch.data(), scratch.size()}),
        coordinator(journal, operations) {}
};

struct LoadedState {
  Status status = Status::Ok;
  bool selected = false;
  Slot slot = Slot::A;
  Phase phase = Phase::Prepared;
  uint32_t sequence = 0;
};

LoadedState loadAfterReboot(MemoryIo& memory) {
  std::array<uint8_t, kScratchCapacity> scratch{};
  Journal journal(makeIo(memory), {scratch.data(), scratch.size()});
  Selection selected{};
  const Status status = journal.load(&selected);
  return {status, selected.selected, selected.slot, selected.record.phase, selected.record.sequence};
}

RunResult recoverAfterReboot(Fixture& fixture, const uint8_t maxSteps = kMaxRecoverySteps) {
  std::array<uint8_t, kScratchCapacity> scratch{};
  Journal journal(makeIo(fixture.memory), {scratch.data(), scratch.size()});
  Operations operations = makeOperations(fixture.model);
  Coordinator coordinator(journal, operations);
  return coordinator.recover(maxSteps);
}

void expectDeleted(const Fixture& fixture, const std::string& context) {
  expect(!fixture.model.files.contains(kSource), context + ": source must not be visible");
  expect(!fixture.model.files.contains(kTombstone), context + ": hidden source must be removed");
  expect(!fixture.model.caches.contains(kCache), context + ": full PDF cache must be purged");
  expect(!fixture.model.bookmarkStores.contains(kBookmarks), context + ": bookmarks must be purged");
  expect(!fixture.model.clippingStores.contains(kClippings), context + ": clippings must be purged");
  expect(!fixture.model.recentStores.contains(kRecent), context + ": recent entry must be purged");
}

void expectUnrelatedPreserved(const Fixture& fixture, const std::string& context) {
  expect(fixture.model.files.contains(kUnrelatedSource), context + ": unrelated PDF must remain");
  expect(fixture.model.files.contains(kUnrelatedTombstone), context + ": unrelated hidden file must remain");
  expect(fixture.model.caches.contains(kUnrelatedCache), context + ": unrelated cache must remain");
  expect(fixture.model.bookmarkStores.contains(kUnrelatedBookmarks), context + ": unrelated bookmarks must remain");
  expect(fixture.model.clippingStores.contains(kUnrelatedClippings), context + ": unrelated clippings must remain");
  expect(fixture.model.recentStores.contains(kUnrelatedRecent), context + ": unrelated recent must remain");
}

std::vector<std::string> orderingEvents(const std::vector<std::string>& events) {
  std::vector<std::string> filtered;
  for (const auto& event : events) {
    if (event.starts_with("journal-sync:") || event.starts_with("journal-remove:") || event.starts_with("op:")) {
      filtered.push_back(event);
    }
  }
  return filtered;
}

void advanceSteps(Fixture& fixture, const size_t count, const std::string& context) {
  for (size_t index = 0; index < count; ++index) {
    const StepResult result = fixture.coordinator.step();
    expect(result.status == Status::Ok && result.disposition == StepDisposition::Advanced,
           context + ": setup step " + std::to_string(index) + " must advance");
  }
}

void testPreparedIntentPrecedesOrderedCrashRecoverableDeletion() {
  Fixture fixture;
  expect(armedBegin(fixture.coordinator.begin(request())), "begin must durably prepare PDF deletion");
  expect(std::all_of(fixture.model.attempts.begin(), fixture.model.attempts.end(),
                     [](const size_t value) { return value == 0; }),
         "begin must not invoke a destructive adapter operation");

  const RunResult result = fixture.coordinator.recover();
  expect(result.status == Status::Ok && result.disposition == RunDisposition::Complete,
         "bounded recovery must finish deletion and clear the journal");
  expectDeleted(fixture, "ordered deletion");
  expectUnrelatedPreserved(fixture, "ordered deletion");

  const std::vector<std::string> expected{
      std::string("journal-sync:") + kSlotAPath,   "op:hide-source",
      std::string("journal-sync:") + kSlotBPath,   "op:purge-cache",
      std::string("journal-sync:") + kSlotAPath,   "op:purge-bookmarks",
      std::string("journal-sync:") + kSlotBPath,   "op:purge-clippings",
      std::string("journal-sync:") + kSlotAPath,   "op:purge-recent",
      std::string("journal-sync:") + kSlotBPath,   "op:remove-source",
      std::string("journal-sync:") + kSlotAPath,   std::string("journal-remove:") + kSlotBPath,
      std::string("journal-remove:") + kSlotAPath,
  };
  expect(orderingEvents(fixture.memory.events) == expected,
         "every destructive phase must precede its durable marker and cleanup must remove old slot first");
  expect(fixture.memory.maxOpenHandles == 1, "journal must never hold more than one file handle");
  expect(!fixture.memory.exists(kSlotAPath) && !fixture.memory.exists(kSlotBPath),
         "terminal recovery must remove both journal slots");

  const auto attempts = fixture.model.attempts;
  const RunResult repeated = recoverAfterReboot(fixture);
  expect(repeated.status == Status::Ok && repeated.disposition == RunDisposition::Idle,
         "repeated recovery after journal clear must be idle");
  expect(fixture.model.attempts == attempts, "idle repeated recovery must not repeat domain operations");
}

void testPdfOnlyValidationAndDeterministicTombstone() {
  {
    std::array<char, 128> tombstone{};
    size_t length = 0;
    expect(formatTombstonePath(view(kSource), tombstone.data(), tombstone.size(), &length) == Status::Ok,
           "formatter must accept a bounded PDF path");
    expect(std::string(tombstone.data(), length) == kTombstone,
           "formatter must create the deterministic hidden sibling path");
    expect(formatTombstonePath(view(kSource), tombstone.data(), length, &length) == Status::ScratchTooSmall,
           "formatter must reserve room for a terminator");
  }

  {
    Fixture fixture;
    expect(safeBeginFailure(fixture.coordinator.begin(request(BookFormat::Unknown)), Status::InvalidArgument),
           "non-PDF format must be rejected");
    expect(fixture.model.validations == 0, "format rejection must happen before adapter validation");
    expect(fixture.memory.files.empty(), "format rejection must not create journal slots");
  }

  {
    Fixture fixture;
    Request nonPdf = request();
    nonPdf.targets.source = view("/Books/book.epub");
    nonPdf.targets.tombstone = view("/Books/.book.epub.crossink-delete");
    expect(safeBeginFailure(fixture.coordinator.begin(nonPdf), Status::InvalidArgument),
           "a non-PDF source extension must be rejected");
    expect(fixture.model.validations == 0, "extension rejection must happen before adapter validation");
  }

  {
    Fixture fixture;
    Request wrongTombstone = request();
    wrongTombstone.targets.tombstone = view("/Books/.other.pdf.crossink-delete");
    expect(safeBeginFailure(fixture.coordinator.begin(wrongTombstone), Status::InvalidArgument),
           "non-deterministic tombstone target must be rejected");
    expect(fixture.model.validations == 0, "tombstone rejection must happen before adapter validation");
  }

  {
    Fixture fixture;
    fixture.model.rejectValidation = true;
    expect(safeBeginFailure(fixture.coordinator.begin(request()), Status::Conflict),
           "adapter must be able to reject targets it cannot prove exact");
    expect(!fixture.memory.exists(kSlotAPath) && !fixture.memory.exists(kSlotBPath),
           "adapter target rejection must happen before journal mutation");
  }

  {
    Fixture fixture;
    std::string oversized(kMaxPathBytes + 1U, 'x');
    oversized[0] = '/';
    Request invalid = request();
    invalid.targets.cache = {oversized.data(), oversized.size()};
    expect(safeBeginFailure(fixture.coordinator.begin(invalid), Status::LimitExceeded),
           "oversized adapter targets must fail the fixed journal bound");
    expect(fixture.model.validations == 0, "bounded validation must happen before adapter invocation");
  }
}

void testPreparedDeletionCanBeAbandonedOnlyBeforeAnyRenameAttempt() {
  {
    Fixture fixture;
    expect(armedBegin(fixture.coordinator.begin(request())), "abandon setup begin");
    const AbandonResult abandoned = fixture.coordinator.abandonPrepared();
    expect(abandoned.status == Status::Ok && abandoned.disposition == AbandonDisposition::Cleared,
           "prepared intent may be abandoned before source rename is attempted");
    expect(fixture.model.files.contains(kSource) && !fixture.model.files.contains(kTombstone),
           "safe abandonment must keep the visible source");
    expect(fixture.model.caches.contains(kCache) && fixture.model.bookmarkStores.contains(kBookmarks) &&
               fixture.model.clippingStores.contains(kClippings) && fixture.model.recentStores.contains(kRecent),
           "safe abandonment must keep all PDF state");
    expect(!fixture.memory.exists(kSlotAPath) && !fixture.memory.exists(kSlotBPath),
           "safe abandonment must clear the prepared journal");
    const AbandonResult repeated = fixture.coordinator.abandonPrepared();
    expect(repeated.status == Status::Conflict && repeated.disposition == AbandonDisposition::Indeterminate,
           "abandonment must not be repeatable after intent is cleared");
  }

  {
    Fixture fixture;
    expect(armedBegin(fixture.coordinator.begin(request())), "attempted-rename abandon setup");
    fixture.model.arm(Operation::Hide, OperationFault::Before);
    expect(fixture.coordinator.step().status == Status::OperationFailed,
           "injected pre-rename adapter failure must surface");
    expect(fixture.model.files.contains(kSource), "pre-rename adapter failure must keep source visible");
    const AbandonResult abandoned = fixture.coordinator.abandonPrepared();
    expect(abandoned.status == Status::Conflict && abandoned.disposition == AbandonDisposition::Indeterminate,
           "ambiguous attempted rename may not discard the only recovery intent");
    const RunResult recovered = recoverAfterReboot(fixture);
    expect(recovered.status == Status::Ok && recovered.disposition == RunDisposition::Complete,
           "ambiguous attempted rename must remain recoverable");
    expectDeleted(fixture, "attempted rename recovery");
  }
}

void testRebootAtEveryDurablePhaseFinishesDeletion() {
  for (size_t completed = 0; completed <= 6; ++completed) {
    Fixture fixture;
    const std::string context = "reboot after phase step " + std::to_string(completed);
    expect(armedBegin(fixture.coordinator.begin(request())), context + ": begin");
    advanceSteps(fixture, completed, context);
    if (completed == 0) {
      expect(fixture.model.files.contains(kSource), context + ": Prepared must leave source visible");
    } else {
      expect(!fixture.model.files.contains(kSource), context + ": source must never be restored after hide");
    }

    const RunResult recovered = recoverAfterReboot(fixture);
    expect(recovered.status == Status::Ok && recovered.disposition == RunDisposition::Complete,
           context + ": recovery must complete");
    expectDeleted(fixture, context);
    expectUnrelatedPreserved(fixture, context);
    expect(fixture.memory.maxOpenHandles == 1, context + ": one-handle invariant");
  }
}

void testEveryAdapterPhaseIsIdempotentBeforeAndAfterReportedFailure() {
  constexpr std::array<Operation, 6> operations{
      Operation::Hide,      Operation::Cache,  Operation::Bookmarks,
      Operation::Clippings, Operation::Recent, Operation::RemoveSource,
  };
  constexpr std::array<Phase, 6> priorPhases{
      Phase::Prepared,        Phase::SourceHidden,    Phase::FullCachePurged,
      Phase::BookmarksPurged, Phase::ClippingsPurged, Phase::RecentsPurged,
  };

  for (size_t operationIndex = 0; operationIndex < operations.size(); ++operationIndex) {
    for (const OperationFault fault : {OperationFault::Before, OperationFault::After}) {
      Fixture fixture;
      const std::string context = "adapter fault op " + std::to_string(operationIndex) +
                                  (fault == OperationFault::Before ? " before" : " after");
      expect(armedBegin(fixture.coordinator.begin(request())), context + ": begin");
      advanceSteps(fixture, operationIndex, context);
      fixture.model.arm(operations[operationIndex], fault);

      const StepResult failed = fixture.coordinator.step();
      expect(failed.status == Status::OperationFailed && failed.durablePhase == priorPhases[operationIndex],
             context + ": durable phase must not advance on adapter failure");
      const LoadedState loaded = loadAfterReboot(fixture.memory);
      expect(loaded.status == Status::Ok && loaded.selected && loaded.phase == priorPhases[operationIndex],
             context + ": reboot must observe the prior durable phase");
      if (operationIndex == 0 && fault == OperationFault::Before) {
        expect(fixture.model.files.contains(kSource), context + ": failed rename must keep source visible");
      } else {
        expect(!fixture.model.files.contains(kSource),
               context + ": source must remain hidden after rename may have happened");
      }

      const RunResult recovered = recoverAfterReboot(fixture);
      expect(recovered.status == Status::Ok && recovered.disposition == RunDisposition::Complete,
             context + ": idempotent retry must complete");
      expect(fixture.model.attempts[operationIndex] >= 2, context + ": failed operation must be retried");
      expectDeleted(fixture, context);
      expectUnrelatedPreserved(fixture, context);
    }
  }
}

void testExistingDeterministicTombstoneFailsClosedUntilCollisionIsResolved() {
  Fixture fixture;
  fixture.model.files.insert(kTombstone);
  expect(armedBegin(fixture.coordinator.begin(request())), "collision setup begin");
  const StepResult collided = fixture.coordinator.step();
  expect(collided.status == Status::Conflict && collided.durablePhase == Phase::Prepared,
         "source plus exact tombstone collision must fail before any purge");
  expect(fixture.model.files.contains(kSource) && fixture.model.files.contains(kTombstone),
         "collision must preserve both source and pre-existing tombstone");
  expect(fixture.model.caches.contains(kCache) && fixture.model.bookmarkStores.contains(kBookmarks),
         "collision must not purge PDF state");

  fixture.model.files.erase(kTombstone);
  const RunResult recovered = recoverAfterReboot(fixture);
  expect(recovered.status == Status::Ok && recovered.disposition == RunDisposition::Complete,
         "resolved collision must recover from Prepared");
  expectDeleted(fixture, "resolved tombstone collision");
  expectUnrelatedPreserved(fixture, "resolved tombstone collision");
}

void testJournalSelectsValidSlotAcrossEveryTruncationAndCorruption() {
  Fixture encoded;
  expect(armedBegin(encoded.coordinator.begin(request())), "slot selection begin");
  const std::vector<uint8_t> older = encoded.memory.bytes(kSlotAPath);
  expect(encoded.coordinator.step().status == Status::Ok, "slot selection second record");
  const std::vector<uint8_t> newer = encoded.memory.bytes(kSlotBPath);

  for (size_t length = 0; length < newer.size(); ++length) {
    MemoryIo memory;
    memory.put(kSlotAPath, older);
    memory.putPrefix(kSlotBPath, newer, length);
    const LoadedState loaded = loadAfterReboot(memory);
    expect(loaded.status == Status::Ok && loaded.selected && loaded.slot == Slot::A && loaded.phase == Phase::Prepared,
           "every truncated newer slot must fall back to older Prepared at length " + std::to_string(length));
    expect(memory.maxOpenHandles <= 1, "truncated slot selection must preserve one-handle invariant");
  }

  for (size_t offset = 0; offset < newer.size(); ++offset) {
    MemoryIo memory;
    memory.put(kSlotAPath, older);
    auto corrupt = newer;
    corrupt[offset] ^= 0x01;
    memory.put(kSlotBPath, corrupt);
    const LoadedState loaded = loadAfterReboot(memory);
    expect(loaded.status == Status::Ok && loaded.selected && loaded.slot == Slot::A && loaded.phase == Phase::Prepared,
           "every corrupt newer byte must fall back to older Prepared at offset " + std::to_string(offset));
  }

  for (size_t offset = 0; offset < older.size(); ++offset) {
    MemoryIo memory;
    auto corrupt = older;
    corrupt[offset] ^= 0x01;
    memory.put(kSlotAPath, corrupt);
    memory.put(kSlotBPath, newer);
    const LoadedState loaded = loadAfterReboot(memory);
    expect(
        loaded.status == Status::Ok && loaded.selected && loaded.slot == Slot::B && loaded.phase == Phase::SourceHidden,
        "every corrupt older byte must select valid SourceHidden at offset " + std::to_string(offset));
  }

  {
    MemoryIo memory;
    auto corruptOlder = older;
    auto corruptNewer = newer;
    corruptOlder.back() ^= 0x01;
    corruptNewer.back() ^= 0x01;
    memory.put(kSlotAPath, corruptOlder);
    memory.put(kSlotBPath, corruptNewer);
    const LoadedState loaded = loadAfterReboot(memory);
    expect(loaded.status == Status::Corrupt && !loaded.selected,
           "two corrupt slots must fail closed instead of inventing a phase");
  }

  {
    MemoryIo memory;
    memory.put(kSlotAPath, older);
    memory.put(kSlotBPath, older);
    const LoadedState loaded = loadAfterReboot(memory);
    expect(loaded.status == Status::Corrupt && !loaded.selected,
           "equal-sequence valid slots must fail closed as ambiguous");
  }

  for (size_t length = 0; length < older.size(); ++length) {
    MemoryIo memory;
    memory.putPrefix(kSlotAPath, older, length);
    const LoadedState loaded = loadAfterReboot(memory);
    expect(loaded.status == Status::Corrupt && !loaded.selected,
           "sole truncated Prepared slot must fail closed at length " + std::to_string(length));
  }
}

void testCorruptNewestRecordAtEveryPhaseRetriesFromOlderPhase() {
  for (size_t completed = 1; completed <= 6; ++completed) {
    Fixture fixture;
    const std::string context = "corrupt newest after step " + std::to_string(completed);
    expect(armedBegin(fixture.coordinator.begin(request())), context + ": begin");
    advanceSteps(fixture, completed, context);
    const LoadedState current = loadAfterReboot(fixture.memory);
    expect(current.status == Status::Ok && current.selected, context + ": current slot must load");
    const char* const currentPath = current.slot == Slot::A ? kSlotAPath : kSlotBPath;
    auto& bytes = fixture.memory.mutableBytes(currentPath);
    bytes.resize(bytes.size() / 2U);

    const RunResult recovered = recoverAfterReboot(fixture);
    expect(recovered.status == Status::Ok && recovered.disposition == RunDisposition::Complete,
           context + ": fallback phase must finish via idempotent operations");
    expectDeleted(fixture, context);
    expectUnrelatedPreserved(fixture, context);
  }

  Fixture prepared;
  expect(armedBegin(prepared.coordinator.begin(request())), "sole corrupt Prepared setup");
  prepared.memory.mutableBytes(kSlotAPath).resize(7);
  const RunResult failed = recoverAfterReboot(prepared);
  expect(failed.status == Status::Corrupt && failed.disposition == RunDisposition::Pending,
         "sole corrupt Prepared must fail closed");
  expect(prepared.model.files.contains(kSource) && !prepared.model.files.contains(kTombstone),
         "corrupt Prepared must not mutate source");
}

void testEveryJournalWriteBoundaryFailureAtEveryPhaseRemainsRecoverable() {
  constexpr std::array<IoFaultPoint, 6> writeFaults{
      IoFaultPoint::OpenWrite, IoFaultPoint::Write, IoFaultPoint::ShortWrite,
      IoFaultPoint::Flush,     IoFaultPoint::Sync,  IoFaultPoint::CloseWrite,
  };
  constexpr std::array<IoFaultPoint, 4> readbackFaults{
      IoFaultPoint::Size,
      IoFaultPoint::Read,
      IoFaultPoint::ShortRead,
      IoFaultPoint::CloseRead,
  };
  constexpr std::array<Phase, 6> priorPhases{
      Phase::Prepared,        Phase::SourceHidden,    Phase::FullCachePurged,
      Phase::BookmarksPurged, Phase::ClippingsPurged, Phase::RecentsPurged,
  };

  for (size_t phase = 0; phase < priorPhases.size(); ++phase) {
    for (const IoFaultPoint fault : writeFaults) {
      Fixture fixture;
      const std::string context =
          "journal write fault " + std::to_string(static_cast<unsigned>(fault)) + " phase " + std::to_string(phase);
      expect(armedBegin(fixture.coordinator.begin(request())), context + ": begin");
      advanceSteps(fixture, phase, context);
      fixture.memory.arm(fault);
      const StepResult failed = fixture.coordinator.step();
      expect(failed.status != Status::Ok && failed.durablePhase == priorPhases[phase],
             context + ": phase marker failure must surface without claiming advancement");
      expect(fixture.memory.openHandles == 0, context + ": failed write must close its handle");

      const LoadedState loaded = loadAfterReboot(fixture.memory);
      expect(loaded.status == Status::Ok && loaded.selected && loaded.phase == priorPhases[phase],
             context + ": older durable phase must remain selectable");
      const RunResult recovered = recoverAfterReboot(fixture);
      expect(recovered.status == Status::Ok && recovered.disposition == RunDisposition::Complete,
             context + ": reboot retry must complete");
      expectDeleted(fixture, context);
      expectUnrelatedPreserved(fixture, context);
    }

    for (const IoFaultPoint fault : readbackFaults) {
      Fixture fixture;
      const std::string context =
          "journal readback fault " + std::to_string(static_cast<unsigned>(fault)) + " phase " + std::to_string(phase);
      expect(armedBegin(fixture.coordinator.begin(request())), context + ": begin");
      advanceSteps(fixture, phase, context);
      const uint32_t readsBeforeWrite = phase == 0 ? 2U : 3U;
      fixture.memory.arm(fault, readsBeforeWrite);
      const StepResult failed = fixture.coordinator.step();
      expect(failed.status != Status::Ok && failed.durablePhase == priorPhases[phase],
             context + ": readback failure must not claim phase advancement");
      expect(fixture.memory.openHandles == 0, context + ": failed readback must close its handle");

      const LoadedState loaded = loadAfterReboot(fixture.memory);
      expect(loaded.status == Status::Ok && loaded.selected && loaded.phase == priorPhases[phase],
             context + ": old phase must remain after failed readback");
      const RunResult recovered = recoverAfterReboot(fixture);
      expect(recovered.status == Status::Ok && recovered.disposition == RunDisposition::Complete,
             context + ": readback retry must complete");
      expectDeleted(fixture, context);
      expectUnrelatedPreserved(fixture, context);
    }
  }
}

void testPreparedIntentWriteFailuresNeverRenameSource() {
  constexpr std::array<IoFaultPoint, 10> faults{
      IoFaultPoint::OpenWrite, IoFaultPoint::Write,      IoFaultPoint::ShortWrite, IoFaultPoint::Flush,
      IoFaultPoint::Sync,      IoFaultPoint::CloseWrite, IoFaultPoint::Size,       IoFaultPoint::Read,
      IoFaultPoint::ShortRead, IoFaultPoint::CloseRead,
  };

  for (const IoFaultPoint fault : faults) {
    Fixture fixture;
    const std::string context = "Prepared write fault " + std::to_string(static_cast<unsigned>(fault));
    fixture.memory.arm(fault);
    const BeginResult failed = fixture.coordinator.begin(request());
    expect(failed.status != Status::Ok && failed.disposition == BeginDisposition::SafeFailure && !failed.canAbandon,
           context + ": begin must report a proven-safe failure");
    expect(fixture.model.files.contains(kSource) && !fixture.model.files.contains(kTombstone),
           context + ": failure before read-verified intent must not rename source");
    expect(std::all_of(fixture.model.attempts.begin(), fixture.model.attempts.end(),
                       [](const size_t value) { return value == 0; }),
           context + ": failed Prepared write must invoke no destructive operation");
    expect(fixture.memory.openHandles == 0, context + ": failed Prepared write must close handles");

    const LoadedState loaded = loadAfterReboot(fixture.memory);
    expect(loaded.status == Status::Ok && !loaded.selected,
           context + ": failed Prepared slot must be cleaned when cleanup succeeds");
    expect(armedBegin(fixture.coordinator.begin(request())), context + ": retry begin");
    const RunResult recovered = recoverAfterReboot(fixture);
    expect(recovered.status == Status::Ok && recovered.disposition == RunDisposition::Complete,
           context + ": retry must complete");
    expectDeleted(fixture, context);
  }
}

void testAmbiguousPreparedCommitReportsArmedAndAllowsAbandon() {
  constexpr std::array<IoFaultPoint, 6> durableButReportedFaults{
      IoFaultPoint::Sync, IoFaultPoint::CloseWrite, IoFaultPoint::Size,
      IoFaultPoint::Read, IoFaultPoint::ShortRead,  IoFaultPoint::CloseRead,
  };

  for (const IoFaultPoint fault : durableButReportedFaults) {
    {
      Fixture fixture;
      const std::string context = "ambiguous Prepared commit " + std::to_string(static_cast<unsigned>(fault));
      fixture.memory.arm(fault);
      fixture.memory.arm(IoFaultPoint::RemoveBefore, 0, kSlotAPath);

      const BeginResult begun = fixture.coordinator.begin(request());
      expect(begun.status != Status::Ok && begun.disposition == BeginDisposition::Armed && begun.canAbandon,
             context + ": a matching durable Prepared record must be reported as armed");
      const LoadedState loaded = loadAfterReboot(fixture.memory);
      expect(loaded.status == Status::Ok && loaded.selected && loaded.phase == Phase::Prepared,
             context + ": the reported armed result must match reboot-visible Prepared state");
      expect(fixture.model.files.contains(kSource) && !fixture.model.files.contains(kTombstone),
             context + ": reporting armed must not rename the source");

      const RunResult recovered = recoverAfterReboot(fixture);
      expect(recovered.status == Status::Ok && recovered.disposition == RunDisposition::Complete,
             context + ": reboot recovery after an armed result must complete deletion");
      expectDeleted(fixture, context);
      expectUnrelatedPreserved(fixture, context);
    }

    {
      Fixture fixture;
      const std::string context = "abandon ambiguous Prepared commit " + std::to_string(static_cast<unsigned>(fault));
      fixture.memory.arm(fault);
      fixture.memory.arm(IoFaultPoint::RemoveBefore, 0, kSlotAPath);

      const BeginResult begun = fixture.coordinator.begin(request());
      expect(begun.disposition == BeginDisposition::Armed && begun.canAbandon,
             context + ": matching Prepared must grant pre-step abandonment");
      const AbandonResult abandoned = fixture.coordinator.abandonPrepared();
      expect(abandoned.status == Status::Ok && abandoned.disposition == AbandonDisposition::Cleared,
             context + ": pre-step abandonment must clear the armed intent");
      expect(fixture.model.files.contains(kSource) && !fixture.model.files.contains(kTombstone),
             context + ": abandonment must preserve source visibility");
      expect(fixture.model.caches.contains(kCache) && fixture.model.bookmarkStores.contains(kBookmarks) &&
                 fixture.model.clippingStores.contains(kClippings) && fixture.model.recentStores.contains(kRecent),
             context + ": abandonment must preserve all PDF state");
      expect(!fixture.memory.exists(kSlotAPath) && !fixture.memory.exists(kSlotBPath),
             context + ": abandonment must remove both journal slots");
      const RunResult rebooted = recoverAfterReboot(fixture);
      expect(rebooted.status == Status::Ok && rebooted.disposition == RunDisposition::Idle,
             context + ": reboot after a cleared result must remain idle");
      expectUnrelatedPreserved(fixture, context);
    }
  }
}

void testIndeterminatePreparedWriteRemainsExplicitAndAbandonable() {
  {
    Fixture fixture;
    fixture.memory.arm(IoFaultPoint::ShortWrite);
    fixture.memory.arm(IoFaultPoint::RemoveBefore, 0, kSlotAPath);

    const BeginResult begun = fixture.coordinator.begin(request());
    expect(
        begun.status == Status::IoFailure && begun.disposition == BeginDisposition::Indeterminate && begun.canAbandon,
        "a retained corrupt begin slot must be reported as indeterminate and abandonable");
    const LoadedState rebooted = loadAfterReboot(fixture.memory);
    expect(rebooted.status == Status::Corrupt && !rebooted.selected,
           "indeterminate corrupt begin state must remain fail-closed across reboot");
    expect(fixture.model.files.contains(kSource) && !fixture.model.files.contains(kTombstone),
           "indeterminate begin state must not rename source");

    const AbandonResult abandoned = fixture.coordinator.abandonPrepared();
    expect(abandoned.status == Status::Ok && abandoned.disposition == AbandonDisposition::Cleared,
           "same-instance abandonment must clear its undecodable pre-step slot");
    expect(!fixture.memory.exists(kSlotAPath) && !fixture.memory.exists(kSlotBPath),
           "abandoning an indeterminate begin must remove both slots");
    expect(fixture.model.files.contains(kSource) && !fixture.model.files.contains(kTombstone),
           "abandoning an indeterminate begin must keep source visible");
  }

  {
    Fixture fixture;
    fixture.memory.arm(IoFaultPoint::CloseWrite);
    fixture.memory.arm(IoFaultPoint::RemoveBefore, 0, kSlotAPath);
    fixture.memory.arm(IoFaultPoint::OpenRead, 2);

    const BeginResult begun = fixture.coordinator.begin(request());
    expect(
        begun.status == Status::IoFailure && begun.disposition == BeginDisposition::Indeterminate && begun.canAbandon,
        "unreadable reconciliation after a write attempt must be explicitly indeterminate");
    const AbandonResult abandoned = fixture.coordinator.abandonPrepared();
    expect(abandoned.status == Status::Ok && abandoned.disposition == AbandonDisposition::Cleared,
           "indeterminate matching intent must retain supported pre-step abandonment");
    expect(fixture.model.files.contains(kSource) && !fixture.model.files.contains(kTombstone),
           "abandoning unreadable reconciliation must preserve source visibility");
  }

  {
    Fixture fixture;
    fixture.memory.arm(IoFaultPoint::CloseWrite);
    fixture.memory.arm(IoFaultPoint::RemoveBefore, 0, kSlotAPath);
    fixture.memory.arm(IoFaultPoint::OpenRead, 2);

    const BeginResult begun = fixture.coordinator.begin(request());
    expect(begun.disposition == BeginDisposition::Indeterminate && begun.canAbandon,
           "reboot recovery setup must report indeterminate");
    const RunResult recovered = recoverAfterReboot(fixture);
    expect(recovered.status == Status::Ok && recovered.disposition == RunDisposition::Complete,
           "reboot may complete deletion only after the caller was told recovery is required");
    expectDeleted(fixture, "indeterminate begin reboot recovery");
    expectUnrelatedPreserved(fixture, "indeterminate begin reboot recovery");
  }
}

void testPreparedFailureIsSafeOnlyAfterAbsenceIsProven() {
  for (const IoFaultPoint fault : {IoFaultPoint::CloseWrite, IoFaultPoint::Size}) {
    Fixture fixture;
    const std::string context = "proven absent Prepared " + std::to_string(static_cast<unsigned>(fault));
    fixture.memory.arm(fault);
    fixture.memory.arm(IoFaultPoint::RemoveAfter, 0, kSlotAPath);

    const BeginResult begun = fixture.coordinator.begin(request());
    expect(begun.status != Status::Ok && begun.disposition == BeginDisposition::SafeFailure && !begun.canAbandon,
           context + ": post-remove error may be safe only after reload proves absence");
    const LoadedState loaded = loadAfterReboot(fixture.memory);
    expect(loaded.status == Status::Ok && !loaded.selected,
           context + ": SafeFailure must match reboot-visible journal absence");
    const RunResult rebooted = recoverAfterReboot(fixture);
    expect(rebooted.status == Status::Ok && rebooted.disposition == RunDisposition::Idle,
           context + ": SafeFailure must never turn into deletion after reboot");
    expect(fixture.model.files.contains(kSource) && !fixture.model.files.contains(kTombstone),
           context + ": SafeFailure must keep source visible");
    expect(fixture.model.caches.contains(kCache) && fixture.model.bookmarkStores.contains(kBookmarks) &&
               fixture.model.clippingStores.contains(kClippings) && fixture.model.recentStores.contains(kRecent),
           context + ": SafeFailure must preserve all PDF state");
    expectUnrelatedPreserved(fixture, context);
  }
}

void testPreexistingPreparedIntentIsNotAdoptedForAbandonment() {
  Fixture fixture;
  expect(armedBegin(fixture.coordinator.begin(request())), "preexisting Prepared setup");

  std::array<uint8_t, kScratchCapacity> scratch{};
  Journal journal(makeIo(fixture.memory), {scratch.data(), scratch.size()});
  Operations operations = makeOperations(fixture.model);
  Coordinator restarted(journal, operations);
  const BeginResult invalid = restarted.begin(request(BookFormat::Unknown));
  expect(invalid.status == Status::InvalidArgument && invalid.disposition == BeginDisposition::Indeterminate &&
             !invalid.canAbandon,
         "validation failure must not claim SafeFailure while a reboot-visible intent exists");
  const BeginResult conflicted = restarted.begin(request());
  expect(conflicted.status == Status::Conflict && conflicted.disposition == BeginDisposition::Indeterminate &&
             !conflicted.canAbandon,
         "a restarted coordinator must not adopt an existing crash-ambiguous Prepared intent");
  const AbandonResult rejected = restarted.abandonPrepared();
  expect(rejected.status == Status::Conflict && rejected.disposition == AbandonDisposition::Indeterminate,
         "a restarted coordinator must not abandon an intent whose rename history is unknown");

  const RunResult recovered = restarted.recover();
  expect(recovered.status == Status::Ok && recovered.disposition == RunDisposition::Complete,
         "preexisting Prepared intent must remain recovery-owned");
  expectDeleted(fixture, "preexisting Prepared recovery");
  expectUnrelatedPreserved(fixture, "preexisting Prepared recovery");
}

void testAbandonCleanupReconcilesBothSlots() {
  for (const IoFaultPoint fault : {IoFaultPoint::RemoveBefore, IoFaultPoint::RemoveAfter}) {
    for (const char* const path : {kSlotBPath, kSlotAPath}) {
      Fixture fixture;
      const std::string context = "abandon cleanup " + std::to_string(static_cast<unsigned>(fault)) + " path " + path;
      expect(armedBegin(fixture.coordinator.begin(request())), context + ": begin");
      fixture.memory.arm(fault, 0, path);

      const AbandonResult abandoned = fixture.coordinator.abandonPrepared();
      const bool removedPrepared = fault == IoFaultPoint::RemoveAfter && std::strcmp(path, kSlotAPath) == 0;
      if (removedPrepared) {
        expect(abandoned.status == Status::IoFailure && abandoned.disposition == AbandonDisposition::Cleared,
               context + ": a post-remove error must report the proven-cleared state");
      } else {
        expect(abandoned.status == Status::IoFailure && abandoned.disposition == AbandonDisposition::Armed,
               context + ": a retained valid Prepared slot must remain explicitly armed");
        const AbandonResult retried = fixture.coordinator.abandonPrepared();
        expect(retried.status == Status::Ok && retried.disposition == AbandonDisposition::Cleared,
               context + ": an armed cleanup result must remain retryable");
      }
      expect(!fixture.memory.exists(kSlotAPath) && !fixture.memory.exists(kSlotBPath),
             context + ": resolved abandonment must clear both slots");
      expect(fixture.model.files.contains(kSource) && !fixture.model.files.contains(kTombstone),
             context + ": slot cleanup faults must never rename source");
      expectUnrelatedPreserved(fixture, context);
    }
  }

  Fixture unreadable;
  expect(armedBegin(unreadable.coordinator.begin(request())), "unreadable abandon cleanup begin");
  unreadable.memory.arm(IoFaultPoint::RemoveBefore, 0, kSlotAPath);
  unreadable.memory.arm(IoFaultPoint::OpenRead);
  const AbandonResult indeterminate = unreadable.coordinator.abandonPrepared();
  expect(indeterminate.status == Status::IoFailure && indeterminate.disposition == AbandonDisposition::Indeterminate,
         "failed cleanup plus failed reconciliation must report indeterminate");
  const AbandonResult retried = unreadable.coordinator.abandonPrepared();
  expect(retried.status == Status::Ok && retried.disposition == AbandonDisposition::Cleared,
         "indeterminate abandonment must retain same-instance retry authority");
  expect(unreadable.model.files.contains(kSource) && !unreadable.model.files.contains(kTombstone),
         "retrying indeterminate abandonment must preserve source visibility");
}

void testJournalCleanupRemoveFailuresPreserveRecoverability() {
  for (const IoFaultPoint fault : {IoFaultPoint::RemoveBefore, IoFaultPoint::RemoveAfter}) {
    for (const char* const path : {kSlotBPath, kSlotAPath}) {
      Fixture fixture;
      const std::string context =
          "cleanup remove fault " + std::to_string(static_cast<unsigned>(fault)) + " path " + path;
      expect(armedBegin(fixture.coordinator.begin(request())), context + ": begin");
      advanceSteps(fixture, 6, context);
      expect(loadAfterReboot(fixture.memory).phase == Phase::SourceRemoved,
             context + ": terminal phase must be durable before cleanup");
      fixture.memory.arm(fault, 0, path);
      const StepResult failed = fixture.coordinator.step();
      expect(failed.status == Status::IoFailure && failed.disposition == StepDisposition::Pending,
             context + ": remove failure must be reported");
      expectDeleted(fixture, context);

      if (fixture.memory.exists(kSlotAPath) || fixture.memory.exists(kSlotBPath)) {
        const LoadedState retained = loadAfterReboot(fixture.memory);
        expect(retained.status == Status::Ok && retained.selected && retained.phase == Phase::SourceRemoved,
               context + ": any retained journal must remain at the terminal phase");
        const RunResult retried = recoverAfterReboot(fixture);
        expect(retried.status == Status::Ok && retried.disposition == RunDisposition::Complete,
               context + ": retained terminal journal must clean up on retry");
      } else {
        const RunResult retried = recoverAfterReboot(fixture);
        expect(retried.status == Status::Ok && retried.disposition == RunDisposition::Idle,
               context + ": post-remove error with no journal must be safely idle");
      }
      expect(!fixture.memory.exists(kSlotAPath) && !fixture.memory.exists(kSlotBPath),
             context + ": retry must leave no journal");
      expectUnrelatedPreserved(fixture, context);
    }
  }
}

void testBoundedRecoveryPerformsAtMostOneDestructivePhasePerStep() {
  Fixture fixture;
  expect(armedBegin(fixture.coordinator.begin(request())), "bounded recovery begin");
  expect(fixture.coordinator.recover(0).status == Status::InvalidArgument,
         "zero recovery budget must be rejected without work");
  expect(std::all_of(fixture.model.attempts.begin(), fixture.model.attempts.end(),
                     [](const size_t value) { return value == 0; }),
         "invalid recovery budget must invoke no operation");

  for (size_t phase = 0; phase < 6; ++phase) {
    const size_t before = std::accumulate(fixture.model.attempts.begin(), fixture.model.attempts.end(), size_t{0});
    const StepResult stepped = fixture.coordinator.step();
    const size_t after = std::accumulate(fixture.model.attempts.begin(), fixture.model.attempts.end(), size_t{0});
    expect(stepped.status == Status::Ok && stepped.disposition == StepDisposition::Advanced,
           "single-step phase must advance");
    expect(after == before + 1U, "one coordinator step must invoke exactly one destructive callback");
  }
  const size_t beforeCleanup = std::accumulate(fixture.model.attempts.begin(), fixture.model.attempts.end(), size_t{0});
  expect(fixture.coordinator.step().disposition == StepDisposition::Complete, "terminal cleanup step must complete");
  const size_t afterCleanup = std::accumulate(fixture.model.attempts.begin(), fixture.model.attempts.end(), size_t{0});
  expect(afterCleanup == beforeCleanup, "journal cleanup must invoke no destructive adapter callback");
}

void testInvalidCallbackTablesAndWorkspaceFailBeforeMutation() {
  {
    Fixture fixture;
    expect(armedBegin(fixture.coordinator.begin(request())), "invalid I/O setup begin");
    Selection prepared{};
    expect(fixture.journal.load(&prepared) == Status::Ok && prepared.selected, "invalid I/O setup must load Prepared");
    const size_t filesBefore = fixture.memory.files.size();
    Io invalid = makeIo(fixture.memory);
    invalid.sync = nullptr;
    Journal journal(invalid, {fixture.scratch.data(), fixture.scratch.size()});
    Selection output{};
    expect(journal.load(&output) == Status::InvalidArgument, "load must reject missing I/O callback");
    const BeginResult begun = journal.begin({}, &output);
    expect(begun.status == Status::InvalidArgument && begun.disposition == BeginDisposition::Indeterminate &&
               !begun.canAbandon,
           "begin must reject missing I/O callback without claiming slot absence");
    expect(journal.advance(prepared, Phase::SourceHidden, &output) == Status::InvalidArgument,
           "advance must reject missing I/O callback");
    expect(journal.discardPrepared(prepared) == Status::InvalidArgument, "discard must reject missing I/O callback");
    prepared.record.phase = Phase::SourceRemoved;
    expect(journal.cleanup(prepared) == Status::InvalidArgument, "cleanup must reject missing I/O callback");
    expect(fixture.memory.files.size() == filesBefore, "invalid I/O table must not mutate journal files");
  }

  {
    Fixture fixture;
    Operations invalid = fixture.operations;
    invalid.hideSource = nullptr;
    Coordinator coordinator(fixture.journal, invalid);
    expect(safeBeginFailure(coordinator.begin(request()), Status::InvalidArgument),
           "begin must reject a missing operation callback");
    expect(coordinator.step().status == Status::InvalidArgument, "step must reject a missing operation callback");
    expect(std::all_of(fixture.model.attempts.begin(), fixture.model.attempts.end(),
                       [](const size_t value) { return value == 0; }),
           "invalid operation table must not mutate domain state");
    expect(fixture.memory.files.empty(), "invalid operation table must not mutate journal");
  }

  {
    MemoryIo memory;
    DurableModel model(memory);
    Operations operations = makeOperations(model);
    std::array<uint8_t, kScratchCapacity - 1U> undersized{};
    Journal journal(makeIo(memory), {undersized.data(), undersized.size()});
    Coordinator coordinator(journal, operations);
    const BeginResult begun = coordinator.begin(request());
    expect(begun.status == Status::ScratchTooSmall && begun.disposition == BeginDisposition::Indeterminate &&
               !begun.canAbandon,
           "workspace shortage must surface without claiming uninspected slots are absent");
    expect(std::all_of(model.attempts.begin(), model.attempts.end(), [](const size_t value) { return value == 0; }),
           "workspace shortage must invoke no destructive callback");
    expect(memory.files.empty(), "workspace shortage must create no journal file");
  }
}

}  // namespace

int main() {
  testPreparedIntentPrecedesOrderedCrashRecoverableDeletion();
  testPdfOnlyValidationAndDeterministicTombstone();
  testPreparedDeletionCanBeAbandonedOnlyBeforeAnyRenameAttempt();
  testRebootAtEveryDurablePhaseFinishesDeletion();
  testEveryAdapterPhaseIsIdempotentBeforeAndAfterReportedFailure();
  testExistingDeterministicTombstoneFailsClosedUntilCollisionIsResolved();
  testJournalSelectsValidSlotAcrossEveryTruncationAndCorruption();
  testCorruptNewestRecordAtEveryPhaseRetriesFromOlderPhase();
  testEveryJournalWriteBoundaryFailureAtEveryPhaseRemainsRecoverable();
  testPreparedIntentWriteFailuresNeverRenameSource();
  testAmbiguousPreparedCommitReportsArmedAndAllowsAbandon();
  testIndeterminatePreparedWriteRemainsExplicitAndAbandonable();
  testPreparedFailureIsSafeOnlyAfterAbsenceIsProven();
  testPreexistingPreparedIntentIsNotAdoptedForAbandonment();
  testAbandonCleanupReconcilesBothSlots();
  testJournalCleanupRemoveFailuresPreserveRecoverability();
  testBoundedRecoveryPerformsAtMostOneDestructivePhasePerStep();
  testInvalidCallbackTablesAndWorkspaceFailBeforeMutation();
  if (failures != 0) return 1;
  std::cout << "PDF_BOOK_DELETION_CORE_PASS\n";
  return 0;
}
