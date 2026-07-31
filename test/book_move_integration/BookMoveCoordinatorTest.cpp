#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
#include <unordered_map>
#include <vector>

#include "BookMoveCoordinator.h"

namespace {

using namespace BookStateMigration;

struct MemoryFile {
  std::vector<uint8_t> bytes;
};

struct MemoryIo {
  std::unordered_map<std::string, MemoryFile> files;
  std::string openPath;
  OpenMode openMode = OpenMode::Read;
  size_t openHandles = 0;
  size_t maxOpenHandles = 0;
  std::vector<std::string> events;
};

Status openFile(void* context, const char* path, OpenMode mode, Handle* handle) {
  auto& io = *static_cast<MemoryIo*>(context);
  if (handle == nullptr || handle->valid()) return Status::InvalidArgument;
  if (mode == OpenMode::Read && !io.files.contains(path)) return Status::NotFound;
  io.openPath = path;
  io.openMode = mode;
  if (mode == OpenMode::WriteTruncate) io.files[path].bytes.clear();
  handle->value = 1;
  ++io.openHandles;
  io.maxOpenHandles = std::max(io.maxOpenHandles, io.openHandles);
  io.events.emplace_back(mode == OpenMode::Read ? "journal-read-open" : "journal-write-open");
  return Status::Ok;
}

Status fileSize(void* context, Handle handle, size_t* size) {
  auto& io = *static_cast<MemoryIo*>(context);
  if (!handle.valid() || size == nullptr) return Status::InvalidArgument;
  *size = io.files[io.openPath].bytes.size();
  return Status::Ok;
}

Status readFile(void* context, Handle handle, size_t offset, uint8_t* destination, size_t length, size_t* actual) {
  auto& io = *static_cast<MemoryIo*>(context);
  if (!handle.valid() || destination == nullptr || actual == nullptr) return Status::InvalidArgument;
  const auto& bytes = io.files[io.openPath].bytes;
  if (offset > bytes.size()) return Status::IoFailure;
  *actual = std::min(length, bytes.size() - offset);
  std::memcpy(destination, bytes.data() + offset, *actual);
  return Status::Ok;
}

Status writeFile(void* context, Handle handle, const uint8_t* source, size_t length, size_t* actual) {
  auto& io = *static_cast<MemoryIo*>(context);
  if (!handle.valid() || source == nullptr || actual == nullptr || io.openMode != OpenMode::WriteTruncate) {
    return Status::InvalidArgument;
  }
  auto& bytes = io.files[io.openPath].bytes;
  bytes.insert(bytes.end(), source, source + length);
  *actual = length;
  io.events.emplace_back("journal-write");
  return Status::Ok;
}

Status flushFile(void* context, Handle handle) {
  if (!handle.valid()) return Status::InvalidArgument;
  static_cast<MemoryIo*>(context)->events.emplace_back("journal-flush");
  return Status::Ok;
}

Status syncFile(void* context, Handle handle) {
  if (!handle.valid()) return Status::InvalidArgument;
  static_cast<MemoryIo*>(context)->events.emplace_back("journal-sync");
  return Status::Ok;
}

Status closeFile(void* context, Handle* handle) {
  auto& io = *static_cast<MemoryIo*>(context);
  if (handle == nullptr || !handle->valid() || io.openHandles != 1) return Status::InvalidArgument;
  handle->invalidate();
  --io.openHandles;
  io.events.emplace_back("journal-close");
  return Status::Ok;
}

Status removeFile(void* context, const char* path) {
  auto& io = *static_cast<MemoryIo*>(context);
  io.files.erase(path);
  io.events.emplace_back("journal-remove");
  return Status::Ok;
}

Io makeIo(MemoryIo& memory) {
  return {&memory, &openFile, &fileSize, &readFile, &writeFile, &flushFile, &syncFile, &closeFile, &removeFile};
}

struct DurableModel {
  MemoryIo* journalIo = nullptr;
  bool oldSource = true;
  bool newSource = false;
  bool oldCache = true;
  bool newCache = false;
  bool oldBookmarks = true;
  bool newBookmarks = false;
  bool oldClippings = true;
  bool newClippings = false;
  bool recentOld = true;
  bool recentNew = false;
  bool openOld = true;
  bool openNew = false;
  std::vector<std::string> events;

  explicit DurableModel(MemoryIo* io) : journalIo(io) {}
};

SourceObservation locateSource(void* context, const Record&) {
  const auto& model = *static_cast<DurableModel*>(context);
  SourceLocation location = SourceLocation::Missing;
  if (model.oldSource && model.newSource) location = SourceLocation::Both;
  if (model.oldSource && !model.newSource) location = SourceLocation::OldOnly;
  if (!model.oldSource && model.newSource) location = SourceLocation::NewOnly;
  return {Status::Ok, location};
}

Status renameSource(void* context, const Record&) {
  auto& model = *static_cast<DurableModel*>(context);
  model.events.emplace_back("rename-source");
  model.oldSource = false;
  model.newSource = true;
  return Status::Ok;
}

Status copyCache(void* context, const Record&) {
  auto& model = *static_cast<DurableModel*>(context);
  model.events.emplace_back("copy-cache");
  model.newCache = model.oldCache;
  return Status::Ok;
}

Status verifyCache(void* context, const Record&) {
  auto& model = *static_cast<DurableModel*>(context);
  model.events.emplace_back("verify-cache");
  return model.oldCache == model.newCache ? Status::Ok : Status::OperationFailed;
}

Status copyBookmarks(void* context, const Record&) {
  auto& model = *static_cast<DurableModel*>(context);
  model.events.emplace_back("copy-bookmarks");
  model.newBookmarks = model.oldBookmarks;
  return Status::Ok;
}

Status verifyBookmarks(void* context, const Record&) {
  auto& model = *static_cast<DurableModel*>(context);
  model.events.emplace_back("verify-bookmarks");
  return model.oldBookmarks == model.newBookmarks ? Status::Ok : Status::OperationFailed;
}

Status copyClippings(void* context, const Record&) {
  auto& model = *static_cast<DurableModel*>(context);
  model.events.emplace_back("copy-clippings");
  model.newClippings = model.oldClippings;
  return Status::Ok;
}

Status verifyClippings(void* context, const Record&) {
  auto& model = *static_cast<DurableModel*>(context);
  model.events.emplace_back("verify-clippings");
  return model.oldClippings == model.newClippings ? Status::Ok : Status::OperationFailed;
}

Status verifyState(void* context, const Record&) {
  auto& model = *static_cast<DurableModel*>(context);
  model.events.emplace_back("verify-state");
  return model.newCache == model.oldCache && model.newBookmarks == model.oldBookmarks &&
                 model.newClippings == model.oldClippings
             ? Status::Ok
             : Status::OperationFailed;
}

Status activateRecent(void* context, const Record&) {
  auto& model = *static_cast<DurableModel*>(context);
  model.events.emplace_back("activate-recent");
  model.recentOld = false;
  model.recentNew = true;
  return Status::Ok;
}

Status activateOpen(void* context, const Record&) {
  auto& model = *static_cast<DurableModel*>(context);
  model.events.emplace_back("activate-open");
  model.openOld = false;
  model.openNew = true;
  return Status::Ok;
}

Status verifyActivation(void* context, const Record&) {
  auto& model = *static_cast<DurableModel*>(context);
  model.events.emplace_back("verify-activation");
  return !model.recentOld && model.recentNew && !model.openOld && model.openNew ? Status::Ok : Status::OperationFailed;
}

Status removeOldState(void* context, const OldState&) {
  auto& model = *static_cast<DurableModel*>(context);
  model.events.emplace_back("remove-old-state");
  model.oldCache = false;
  model.oldBookmarks = false;
  model.oldClippings = false;
  return model.newSource && model.newCache && model.newBookmarks && model.newClippings ? Status::Ok
                                                                                       : Status::OperationFailed;
}

MigrationOperations makeOperations(DurableModel& model) {
  return {&model,          &locateSource,    &renameSource,     &copyCache,       &verifyCache,
          &copyBookmarks,  &verifyBookmarks, &copyClippings,    &verifyClippings, &verifyState,
          &activateRecent, &activateOpen,    &verifyActivation, &removeOldState};
}

int failures = 0;

void expect(bool condition, const char* message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

void testPreparedJournalIsDurableBeforeSourceRename() {
  MemoryIo memory;
  std::array<uint8_t, kScratchCapacity> scratch{};
  Journal journal(makeIo(memory), {scratch.data(), scratch.size()});
  DurableModel model{&memory};
  const MigrationOperations operations = makeOperations(model);
  Coordinator coordinator(journal, operations);

  const Request request{{"/Books/old.pdf", 14}, {"/Read/old.pdf", 13}, BookFormat::Pdf, 11, 22};
  const Status status = coordinator.begin(request);

  expect(status == Status::Ok, "begin must durably prepare the journal");
  expect(model.oldSource && !model.newSource, "begin must not rename the source");
  expect(model.events.empty(), "begin must not invoke domain operations");
  const auto sync = std::find(memory.events.begin(), memory.events.end(), "journal-sync");
  expect(sync != memory.events.end(), "prepared journal must be synced");

  const StepResult first = coordinator.step();
  expect(first.status == Status::Ok && first.durablePhase == Phase::SourceMoved,
         "first recovery step must durably record source movement");
  expect(!model.oldSource && model.newSource, "first recovery step must rename source");
}

void testBoundedRunCopiesVerifiesActivatesThenCleansOldState() {
  MemoryIo memory;
  std::array<uint8_t, kScratchCapacity> scratch{};
  Journal journal(makeIo(memory), {scratch.data(), scratch.size()});
  DurableModel model{&memory};
  const MigrationOperations operations = makeOperations(model);
  Coordinator coordinator(journal, operations);
  const Request request{{"/Books/old.pdf", 14}, {"/Read/old.pdf", 13}, BookFormat::Pdf, 101, 202};
  expect(coordinator.begin(request) == Status::Ok, "PDF move must prepare");

  const RunResult result = coordinator.recover();

  expect(result.status == Status::Ok && result.disposition == RunDisposition::Complete,
         "bounded recovery must reach journal cleanup");
  expect(model.newSource, "new source must remain after recovery");
  expect(!model.oldCache && model.newCache, "old cache must be removed only after verified copy");
  expect(!model.oldBookmarks && model.newBookmarks, "old bookmarks must be removed only after verified copy");
  expect(!model.oldClippings && model.newClippings, "old clippings must be removed only after verified copy");
  expect(!model.recentOld && model.recentNew && !model.openOld && model.openNew,
         "recent/open path activation must point at the new source");
  const std::vector<std::string> expected = {
      "rename-source",    "copy-cache",   "verify-cache",    "copy-bookmarks", "verify-bookmarks",  "copy-clippings",
      "verify-clippings", "verify-state", "activate-recent", "activate-open",  "verify-activation", "remove-old-state",
  };
  expect(model.events == expected, "domain operations must preserve the durable phase order");
  expect(memory.maxOpenHandles == 1, "journal I/O must keep one handle open at a time");
  expect(!memory.files.contains(kSlotAPath) && !memory.files.contains(kSlotBPath),
         "terminal recovery must remove both journal slots");
}

void testEqualPathHashesFailBeforeJournalOrSourceMutation() {
  MemoryIo memory;
  std::array<uint8_t, kScratchCapacity> scratch{};
  Journal journal(makeIo(memory), {scratch.data(), scratch.size()});
  DurableModel model{&memory};
  const MigrationOperations operations = makeOperations(model);
  Coordinator coordinator(journal, operations);
  const Request request{{"/Books/a.pdf", 12}, {"/Read/a.pdf", 11}, BookFormat::Pdf, 77, 77};

  expect(coordinator.begin(request) == Status::Conflict, "a cache-key collision must fail closed");
  expect(memory.events.empty() && model.events.empty(), "collision rejection must precede all durable mutation");
  expect(model.oldSource && !model.newSource, "collision rejection must preserve the old source");
}

void testRebootFallsBackFromCorruptNewestJournalWithoutRepeatingRename() {
  MemoryIo memory;
  std::array<uint8_t, kScratchCapacity> scratch{};
  Journal journal(makeIo(memory), {scratch.data(), scratch.size()});
  DurableModel model{&memory};
  const MigrationOperations operations = makeOperations(model);
  Coordinator coordinator(journal, operations);
  const Request request{{"/Books/reboot.pdf", 17}, {"/Read/reboot.pdf", 16}, BookFormat::Pdf, 303, 404};

  expect(coordinator.begin(request) == Status::Ok, "corruption witness must prepare the move");
  const std::string preparedSlot = memory.files.contains(kSlotAPath) ? kSlotAPath : kSlotBPath;
  expect(coordinator.step().durablePhase == Phase::SourceMoved,
         "corruption witness must first durably move the source");
  const std::string newestSlot = preparedSlot == kSlotAPath ? kSlotBPath : kSlotAPath;
  expect(memory.files.contains(newestSlot) && !memory.files[newestSlot].bytes.empty(),
         "source-moved phase must occupy the alternate journal slot");
  memory.files[newestSlot].bytes.back() ^= 0x80U;

  std::array<uint8_t, kScratchCapacity> rebootScratch{};
  Journal rebootJournal(makeIo(memory), {rebootScratch.data(), rebootScratch.size()});
  Coordinator rebootCoordinator(rebootJournal, operations);
  const RunResult recovered = rebootCoordinator.recover();

  expect(recovered.status == Status::Ok && recovered.disposition == RunDisposition::Complete,
         "reboot must recover from the older valid journal slot");
  expect(std::count(model.events.begin(), model.events.end(), "rename-source") == 1,
         "fallback recovery must observe the new source instead of renaming twice");
  expect(model.newSource && !model.oldSource, "corrupt-journal recovery must preserve the already-moved source");
}

void testRecentsPolicyRoundTripsAcrossJournalReboot() {
  MemoryIo memory;
  std::array<uint8_t, kScratchCapacity> scratch{};
  Journal journal(makeIo(memory), {scratch.data(), scratch.size()});
  DurableModel model{&memory};
  const MigrationOperations operations = makeOperations(model);
  Coordinator coordinator(journal, operations);
  const Request request{{"/Books/remove.pdf", 17}, {"/Read/remove.pdf", 16}, BookFormat::Pdf, 505, 606,
                        RecentsPolicy::Remove};
  expect(coordinator.begin(request) == Status::Ok, "remove-from-recents policy must be durably prepared");

  std::array<uint8_t, kScratchCapacity> rebootScratch{};
  Journal rebootJournal(makeIo(memory), {rebootScratch.data(), rebootScratch.size()});
  Selection rebootSelection{};
  expect(rebootJournal.load(&rebootSelection) == Status::Ok && rebootSelection.selected &&
             rebootSelection.record.recentsPolicy == RecentsPolicy::Remove,
         "journal reboot must retain the remove-from-recents policy");
}

}  // namespace

int main() {
  testPreparedJournalIsDurableBeforeSourceRename();
  testBoundedRunCopiesVerifiesActivatesThenCleansOldState();
  testEqualPathHashesFailBeforeJournalOrSourceMutation();
  testRebootFallsBackFromCorruptNewestJournalWithoutRepeatingRename();
  testRecentsPolicyRoundTripsAcrossJournalReboot();
  if (failures != 0) return 1;
  std::cout << "BOOK_MOVE_COORDINATOR_PASS\n";
  return 0;
}
