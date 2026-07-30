#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "BookStateMigrationJournal.h"

namespace {

using namespace BookStateMigration;

enum class FaultPoint : uint8_t {
  None,
  Open,
  Size,
  Read,
  Write,
  Flush,
  Sync,
  Close,
  Remove,
};

class TestIo {
 public:
  Io io() {
    return {this, openThunk, sizeThunk, readThunk, writeThunk, flushThunk, syncThunk, closeThunk, removeThunk};
  }

  void fail(FaultPoint point, uint32_t occurrence = 1) {
    fault_ = point;
    occurrence_ = occurrence;
    seen_ = 0;
    failAfter_ = false;
  }

  void failAfter(FaultPoint point, uint32_t occurrence = 1) {
    fault_ = point;
    occurrence_ = occurrence;
    seen_ = 0;
    failAfter_ = true;
  }

  void clearFault() {
    fault_ = FaultPoint::None;
    occurrence_ = 0;
    seen_ = 0;
    failAfter_ = false;
  }

  void setWriteAllowance(size_t bytes) { writeAllowance_ = bytes; }
  void clearWriteAllowance() { writeAllowance_ = std::numeric_limits<size_t>::max(); }
  void setReadAllowance(size_t bytes) { readAllowance_ = bytes; }
  void clearReadAllowance() { readAllowance_ = std::numeric_limits<size_t>::max(); }

  void put(const char* path, const uint8_t* bytes, size_t length) {
    files_[path] = std::vector<uint8_t>(bytes, bytes + length);
  }

  void put(const char* path, const std::vector<uint8_t>& bytes) { files_[path] = bytes; }
  bool exists(const char* path) const { return files_.find(path) != files_.end(); }
  const std::vector<uint8_t>& bytes(const char* path) const { return files_.at(path); }
  std::vector<uint8_t>& mutableBytes(const char* path) { return files_.at(path); }
  size_t openHandles() const { return open_ ? 1U : 0U; }
  size_t maxOpenHandles() const { return maxOpen_; }
  size_t calls(FaultPoint point) const { return calls_[static_cast<size_t>(point)]; }

 private:
  static Status openThunk(void* context, const char* path, OpenMode mode, Handle* handle) {
    return static_cast<TestIo*>(context)->open(path, mode, handle);
  }
  static Status sizeThunk(void* context, Handle handle, size_t* size) {
    return static_cast<TestIo*>(context)->size(handle, size);
  }
  static Status readThunk(void* context, Handle handle, size_t offset, uint8_t* bytes, size_t requested,
                          size_t* actual) {
    return static_cast<TestIo*>(context)->read(handle, offset, bytes, requested, actual);
  }
  static Status writeThunk(void* context, Handle handle, const uint8_t* bytes, size_t requested, size_t* actual) {
    return static_cast<TestIo*>(context)->write(handle, bytes, requested, actual);
  }
  static Status flushThunk(void* context, Handle handle) { return static_cast<TestIo*>(context)->flush(handle); }
  static Status syncThunk(void* context, Handle handle) { return static_cast<TestIo*>(context)->sync(handle); }
  static Status closeThunk(void* context, Handle* handle) { return static_cast<TestIo*>(context)->close(handle); }
  static Status removeThunk(void* context, const char* path) {
    return static_cast<TestIo*>(context)->remove(path);
  }

  bool shouldFail(FaultPoint point) {
    ++calls_[static_cast<size_t>(point)];
    if (fault_ != point) {
      return false;
    }
    ++seen_;
    return seen_ == occurrence_;
  }

  Status open(const char* path, OpenMode mode, Handle* handle) {
    if (path == nullptr || handle == nullptr || open_) {
      return Status::InvalidArgument;
    }
    if (shouldFail(FaultPoint::Open)) {
      return Status::IoFailure;
    }
    auto found = files_.find(path);
    if (mode == OpenMode::Read) {
      if (found == files_.end()) {
        return Status::NotFound;
      }
    } else {
      files_[path].clear();
    }
    path_ = path;
    writable_ = mode == OpenMode::WriteTruncate;
    position_ = 0;
    open_ = true;
    maxOpen_ = std::max(maxOpen_, size_t{1});
    handle->value = 1;
    return Status::Ok;
  }

  Status size(Handle handle, size_t* sizeOut) {
    if (!open_ || !handle.valid() || sizeOut == nullptr) {
      return Status::InvalidArgument;
    }
    if (shouldFail(FaultPoint::Size)) {
      return Status::IoFailure;
    }
    *sizeOut = files_.at(path_).size();
    return Status::Ok;
  }

  Status read(Handle handle, size_t offset, uint8_t* destination, size_t requested, size_t* actual) {
    if (!open_ || !handle.valid() || destination == nullptr || actual == nullptr) {
      return Status::InvalidArgument;
    }
    *actual = 0;
    if (shouldFail(FaultPoint::Read)) {
      return Status::IoFailure;
    }
    const auto& source = files_.at(path_);
    if (offset > source.size()) {
      return Status::IoFailure;
    }
    *actual = std::min({requested, source.size() - offset, readAllowance_});
    if (*actual != 0) {
      std::memcpy(destination, source.data() + offset, *actual);
    }
    return Status::Ok;
  }

  Status write(Handle handle, const uint8_t* source, size_t requested, size_t* actual) {
    if (!open_ || !writable_ || !handle.valid() || source == nullptr || actual == nullptr) {
      return Status::InvalidArgument;
    }
    *actual = 0;
    if (shouldFail(FaultPoint::Write)) {
      return Status::IoFailure;
    }
    const size_t accepted = std::min(requested, writeAllowance_);
    auto& destination = files_.at(path_);
    destination.insert(destination.end(), source, source + accepted);
    position_ += accepted;
    writeAllowance_ -= accepted;
    *actual = accepted;
    return Status::Ok;
  }

  Status flush(Handle handle) {
    if (!open_ || !handle.valid()) {
      return Status::InvalidArgument;
    }
    return shouldFail(FaultPoint::Flush) ? Status::IoFailure : Status::Ok;
  }

  Status sync(Handle handle) {
    if (!open_ || !handle.valid()) {
      return Status::InvalidArgument;
    }
    return shouldFail(FaultPoint::Sync) ? Status::IoFailure : Status::Ok;
  }

  Status close(Handle* handle) {
    if (!open_ || handle == nullptr || !handle->valid()) {
      return Status::InvalidArgument;
    }
    const bool failed = shouldFail(FaultPoint::Close);
    open_ = false;
    writable_ = false;
    path_.clear();
    handle->invalidate();
    return failed ? Status::IoFailure : Status::Ok;
  }

  Status remove(const char* path) {
    if (path == nullptr) {
      return Status::InvalidArgument;
    }
    const bool triggered = shouldFail(FaultPoint::Remove);
    if (triggered && !failAfter_) {
      return Status::IoFailure;
    }
    const auto found = files_.find(path);
    if (found == files_.end()) {
      return Status::NotFound;
    }
    files_.erase(found);
    return triggered ? Status::IoFailure : Status::Ok;
  }

  std::map<std::string, std::vector<uint8_t>> files_;
  std::array<size_t, 9> calls_{};
  FaultPoint fault_ = FaultPoint::None;
  uint32_t occurrence_ = 0;
  uint32_t seen_ = 0;
  bool failAfter_ = false;
  size_t writeAllowance_ = std::numeric_limits<size_t>::max();
  size_t readAllowance_ = std::numeric_limits<size_t>::max();
  bool open_ = false;
  bool writable_ = false;
  size_t position_ = 0;
  size_t maxOpen_ = 0;
  std::string path_;
};

struct Domain {
  enum class Action : uint8_t {
    None,
    Rename,
    CopyCache,
    VerifyCache,
    CopyBookmarks,
    VerifyBookmarks,
    CopyClippings,
    VerifyClippings,
    VerifyState,
    ActivateRecent,
    ActivateOpenPath,
    VerifyActivation,
    RemoveOldState,
  };

  MigrationOperations operations() {
    return {this,
            locateThunk,
            renameThunk,
            copyCacheThunk,
            verifyCacheThunk,
            copyBookmarksThunk,
            verifyBookmarksThunk,
            copyClippingsThunk,
            verifyClippingsThunk,
            verifyStateThunk,
            activateRecentThunk,
            activateOpenPathThunk,
            verifyActivationThunk,
            removeOldStateThunk};
  }

  static SourceObservation locateThunk(void* context, const Record&) {
    auto& self = *static_cast<Domain*>(context);
    ++self.locateCalls;
    if (self.failLocateOnCall == self.locateCalls) {
      return {Status::OperationFailed, SourceLocation::Missing};
    }
    return {self.locateStatus, self.location};
  }

#define DOMAIN_CALLBACK(name, action)                    \
  static Status name##Thunk(void* context, const Record&) { \
    return static_cast<Domain*>(context)->run(action);   \
  }
  DOMAIN_CALLBACK(rename, Action::Rename)
  DOMAIN_CALLBACK(copyCache, Action::CopyCache)
  DOMAIN_CALLBACK(verifyCache, Action::VerifyCache)
  DOMAIN_CALLBACK(copyBookmarks, Action::CopyBookmarks)
  DOMAIN_CALLBACK(verifyBookmarks, Action::VerifyBookmarks)
  DOMAIN_CALLBACK(copyClippings, Action::CopyClippings)
  DOMAIN_CALLBACK(verifyClippings, Action::VerifyClippings)
  DOMAIN_CALLBACK(verifyState, Action::VerifyState)
  DOMAIN_CALLBACK(activateRecent, Action::ActivateRecent)
  DOMAIN_CALLBACK(activateOpenPath, Action::ActivateOpenPath)
  DOMAIN_CALLBACK(verifyActivation, Action::VerifyActivation)
#undef DOMAIN_CALLBACK

  static Status removeOldStateThunk(void* context, const OldState& oldState) {
    auto& self = *static_cast<Domain*>(context);
    self.cleanedPath.assign(oldState.path.data, oldState.path.length);
    self.cleanedHash = oldState.hash;
    self.cleanedFormat = oldState.format;
    return self.run(Action::RemoveOldState);
  }

  Status run(Action action) {
    events.push_back(action);
    ++actionCalls[static_cast<size_t>(action)];
    if (action == Action::Rename && failRenameAfterMutation) {
      location = SourceLocation::NewOnly;
      return Status::OperationFailed;
    }
    if (action == failAction) {
      if (action == Action::Rename && setLocationOnRenameFailure) {
        location = locationOnRenameFailure;
      }
      return Status::OperationFailed;
    }
    if (action == Action::Rename) {
      location = SourceLocation::NewOnly;
    }
    return Status::Ok;
  }

  size_t count(Action action) const { return actionCalls[static_cast<size_t>(action)]; }

  Status locateStatus = Status::Ok;
  SourceLocation location = SourceLocation::OldOnly;
  Action failAction = Action::None;
  bool failRenameAfterMutation = false;
  bool setLocationOnRenameFailure = false;
  SourceLocation locationOnRenameFailure = SourceLocation::OldOnly;
  size_t locateCalls = 0;
  size_t failLocateOnCall = 0;
  std::array<size_t, 14> actionCalls{};
  std::vector<Action> events;
  std::string cleanedPath;
  uint64_t cleanedHash = 0;
  BookFormat cleanedFormat = BookFormat::Unknown;
};

enum class DurableAction : uint8_t {
  Locate,
  Rename,
  CopyCache,
  VerifyCache,
  CopyBookmarks,
  VerifyBookmarks,
  CopyClippings,
  VerifyClippings,
  VerifyState,
  ActivateRecent,
  ActivateOpenPath,
  VerifyActivation,
  RemoveOldState,
};

enum class FailureTiming : uint8_t {
  None,
  Before,
  After,
};

class DurableDomain {
 public:
  MigrationOperations operations() {
    return {this,
            locateThunk,
            renameThunk,
            copyCacheThunk,
            verifyCacheThunk,
            copyBookmarksThunk,
            verifyBookmarksThunk,
            copyClippingsThunk,
            verifyClippingsThunk,
            verifyStateThunk,
            activateRecentThunk,
            activateOpenPathThunk,
            verifyActivationThunk,
            removeOldStateThunk};
  }

  void fail(DurableAction action, FailureTiming timing) {
    failAction = action;
    failTiming = timing;
  }

  void clearFailure() { failTiming = FailureTiming::None; }

  static SourceObservation locateThunk(void* context, const Record&) {
    auto& self = *static_cast<DurableDomain*>(context);
    self.events.push_back(DurableAction::Locate);
    if (self.shouldFail(DurableAction::Locate, FailureTiming::Before) ||
        self.shouldFail(DurableAction::Locate, FailureTiming::After)) {
      return {Status::OperationFailed, SourceLocation::Missing};
    }
    if (self.oldSource && self.newSource) {
      return {Status::Ok, SourceLocation::Both};
    }
    if (self.oldSource) {
      return {Status::Ok, SourceLocation::OldOnly};
    }
    if (self.newSource) {
      return {Status::Ok, SourceLocation::NewOnly};
    }
    return {Status::Ok, SourceLocation::Missing};
  }

#define DURABLE_CALLBACK(name, action)                      \
  static Status name##Thunk(void* context, const Record&) { \
    return static_cast<DurableDomain*>(context)->run(action); \
  }
  DURABLE_CALLBACK(rename, DurableAction::Rename)
  DURABLE_CALLBACK(copyCache, DurableAction::CopyCache)
  DURABLE_CALLBACK(verifyCache, DurableAction::VerifyCache)
  DURABLE_CALLBACK(copyBookmarks, DurableAction::CopyBookmarks)
  DURABLE_CALLBACK(verifyBookmarks, DurableAction::VerifyBookmarks)
  DURABLE_CALLBACK(copyClippings, DurableAction::CopyClippings)
  DURABLE_CALLBACK(verifyClippings, DurableAction::VerifyClippings)
  DURABLE_CALLBACK(verifyState, DurableAction::VerifyState)
  DURABLE_CALLBACK(activateRecent, DurableAction::ActivateRecent)
  DURABLE_CALLBACK(activateOpenPath, DurableAction::ActivateOpenPath)
  DURABLE_CALLBACK(verifyActivation, DurableAction::VerifyActivation)
#undef DURABLE_CALLBACK

  static Status removeOldStateThunk(void* context, const OldState& oldState) {
    auto& self = *static_cast<DurableDomain*>(context);
    if (std::string(oldState.path.data, oldState.path.length) != "/old.epub" ||
        oldState.hash != 0x0102030405060708ULL || oldState.format != BookFormat::Pdf) {
      return Status::InvalidArgument;
    }
    return self.run(DurableAction::RemoveOldState);
  }

  Status run(DurableAction action) {
    events.push_back(action);
    if (shouldFail(action, FailureTiming::Before)) {
      return Status::OperationFailed;
    }
    Status mutation = mutate(action);
    if (mutation != Status::Ok) {
      return mutation;
    }
    return shouldFail(action, FailureTiming::After) ? Status::OperationFailed : Status::Ok;
  }

  Status mutate(DurableAction action) {
    switch (action) {
      case DurableAction::Locate:
        return Status::InvalidArgument;
      case DurableAction::Rename:
        oldSource = false;
        newSource = true;
        return Status::Ok;
      case DurableAction::CopyCache:
        cacheCopied = true;
        return Status::Ok;
      case DurableAction::VerifyCache:
        if (!cacheCopied) {
          return Status::OperationFailed;
        }
        cacheVerified = true;
        return Status::Ok;
      case DurableAction::CopyBookmarks:
        bookmarksCopied = true;
        return Status::Ok;
      case DurableAction::VerifyBookmarks:
        if (!bookmarksCopied) {
          return Status::OperationFailed;
        }
        bookmarksVerified = true;
        return Status::Ok;
      case DurableAction::CopyClippings:
        clippingsCopied = true;
        return Status::Ok;
      case DurableAction::VerifyClippings:
        if (!clippingsCopied) {
          return Status::OperationFailed;
        }
        clippingsVerified = true;
        return Status::Ok;
      case DurableAction::VerifyState:
        if (!cacheVerified || !bookmarksVerified || !clippingsVerified) {
          return Status::OperationFailed;
        }
        stateVerified = true;
        return Status::Ok;
      case DurableAction::ActivateRecent:
        if (!stateVerified) {
          return Status::OperationFailed;
        }
        recentActivated = true;
        return Status::Ok;
      case DurableAction::ActivateOpenPath:
        if (!recentActivated) {
          return Status::OperationFailed;
        }
        openPathActivated = true;
        return Status::Ok;
      case DurableAction::VerifyActivation:
        if (!recentActivated || !openPathActivated) {
          return Status::OperationFailed;
        }
        activationVerified = true;
        return Status::Ok;
      case DurableAction::RemoveOldState:
        if (!activationVerified) {
          removedBeforeActivation = true;
          return Status::OperationFailed;
        }
        oldStatePresent = false;
        return Status::Ok;
    }
    return Status::InvalidArgument;
  }

  bool shouldFail(DurableAction action, FailureTiming timing) const {
    return failTiming == timing && failAction == action;
  }

  bool oldSource = true;
  bool newSource = false;
  bool cacheCopied = false;
  bool cacheVerified = false;
  bool bookmarksCopied = false;
  bool bookmarksVerified = false;
  bool clippingsCopied = false;
  bool clippingsVerified = false;
  bool stateVerified = false;
  bool recentActivated = false;
  bool openPathActivated = false;
  bool activationVerified = false;
  bool oldStatePresent = true;
  bool removedBeforeActivation = false;
  DurableAction failAction = DurableAction::Locate;
  FailureTiming failTiming = FailureTiming::None;
  std::vector<DurableAction> events;
};

struct Fixture {
  Fixture() : journal(io.io(), {scratch.data(), scratch.size()}) {}

  Record input(Phase phase = Phase::Prepared, uint32_t sequence = 0) const {
    return {sequence,
            phase,
            BookFormat::Pdf,
            0x0102030405060708ULL,
            0x1112131415161718ULL,
            {oldPath.data(), oldPath.size()},
            {newPath.data(), newPath.size()}};
  }

  Selection load() {
    Selection selection{};
    EXPECT_EQ(journal.load(&selection), Status::Ok);
    return selection;
  }

  TestIo io;
  std::array<uint8_t, kScratchCapacity> scratch{};
  Journal journal;
  const std::string oldPath = "/old.epub";
  const std::string newPath = "/Read/new.epub";
};

void expectRecord(const Record& record, Phase phase, uint32_t sequence) {
  EXPECT_EQ(record.sequence, sequence);
  EXPECT_EQ(record.phase, phase);
  EXPECT_EQ(record.format, BookFormat::Pdf);
  EXPECT_EQ(record.oldHash, 0x0102030405060708ULL);
  EXPECT_EQ(record.newHash, 0x1112131415161718ULL);
  EXPECT_EQ(std::string(record.oldPath.data, record.oldPath.length), "/old.epub");
  EXPECT_EQ(std::string(record.newPath.data, record.newPath.length), "/Read/new.epub");
}

Phase phaseBefore(DurableAction action) {
  switch (action) {
    case DurableAction::Rename:
      return Phase::Prepared;
    case DurableAction::CopyCache:
    case DurableAction::VerifyCache:
      return Phase::SourceMoved;
    case DurableAction::CopyBookmarks:
    case DurableAction::VerifyBookmarks:
      return Phase::CacheCopied;
    case DurableAction::CopyClippings:
    case DurableAction::VerifyClippings:
      return Phase::BookmarksCopied;
    case DurableAction::VerifyState:
      return Phase::ClippingsCopied;
    case DurableAction::ActivateRecent:
    case DurableAction::ActivateOpenPath:
    case DurableAction::VerifyActivation:
      return Phase::StateVerified;
    case DurableAction::RemoveOldState:
      return Phase::Activated;
    case DurableAction::Locate:
      return Phase::Prepared;
  }
  return Phase::Prepared;
}

Phase phaseAfter(DurableAction action) {
  switch (action) {
    case DurableAction::Rename:
      return Phase::SourceMoved;
    case DurableAction::CopyCache:
    case DurableAction::VerifyCache:
      return Phase::CacheCopied;
    case DurableAction::CopyBookmarks:
    case DurableAction::VerifyBookmarks:
      return Phase::BookmarksCopied;
    case DurableAction::CopyClippings:
    case DurableAction::VerifyClippings:
      return Phase::ClippingsCopied;
    case DurableAction::VerifyState:
      return Phase::StateVerified;
    case DurableAction::ActivateRecent:
    case DurableAction::ActivateOpenPath:
    case DurableAction::VerifyActivation:
      return Phase::Activated;
    case DurableAction::RemoveOldState:
      return Phase::OldStateRemoved;
    case DurableAction::Locate:
      return Phase::Prepared;
  }
  return Phase::Prepared;
}

std::vector<DurableAction> expectedFaultEvents(DurableAction action) {
  switch (action) {
    case DurableAction::Rename:
      return {DurableAction::Locate, DurableAction::Rename, DurableAction::Locate};
    case DurableAction::CopyCache:
      return {DurableAction::CopyCache};
    case DurableAction::VerifyCache:
      return {DurableAction::CopyCache, DurableAction::VerifyCache};
    case DurableAction::CopyBookmarks:
      return {DurableAction::CopyBookmarks};
    case DurableAction::VerifyBookmarks:
      return {DurableAction::CopyBookmarks, DurableAction::VerifyBookmarks};
    case DurableAction::CopyClippings:
      return {DurableAction::CopyClippings};
    case DurableAction::VerifyClippings:
      return {DurableAction::CopyClippings, DurableAction::VerifyClippings};
    case DurableAction::VerifyState:
      return {DurableAction::VerifyState};
    case DurableAction::ActivateRecent:
      return {DurableAction::ActivateRecent};
    case DurableAction::ActivateOpenPath:
      return {DurableAction::ActivateRecent, DurableAction::ActivateOpenPath};
    case DurableAction::VerifyActivation:
      return {DurableAction::ActivateRecent, DurableAction::ActivateOpenPath, DurableAction::VerifyActivation};
    case DurableAction::RemoveOldState:
      return {DurableAction::RemoveOldState};
    case DurableAction::Locate:
      return {DurableAction::Locate};
  }
  return {};
}

void driveToPhase(Fixture& fixture, DurableDomain& domain, Phase target) {
  while (true) {
    const Selection current = fixture.load();
    ASSERT_TRUE(current.selected);
    if (current.record.phase == target) {
      return;
    }
    const StepResult result = recoverOne(fixture.journal, domain.operations());
    ASSERT_EQ(result.status, Status::Ok);
    ASSERT_EQ(result.disposition, StepDisposition::Advanced);
  }
}

void clearIoCallback(Io* io, uint8_t index) {
  ASSERT_NE(io, nullptr);
  switch (index) {
    case 0:
      io->open = nullptr;
      break;
    case 1:
      io->size = nullptr;
      break;
    case 2:
      io->read = nullptr;
      break;
    case 3:
      io->write = nullptr;
      break;
    case 4:
      io->flush = nullptr;
      break;
    case 5:
      io->sync = nullptr;
      break;
    case 6:
      io->close = nullptr;
      break;
    case 7:
      io->remove = nullptr;
      break;
    default:
      FAIL() << unsigned(index);
  }
}

TEST(BookStateMigrationCodec, ExactGoldenAndStrictTruncationAndCrcValidation) {
  std::array<uint8_t, kScratchCapacity> scratch{};
  const std::string oldPath = "/old.epub";
  const std::string newPath = "/Read/new.epub";
  const Record input{0x01020304,
                     Phase::Prepared,
                     BookFormat::Pdf,
                     0x0102030405060708ULL,
                     0x1112131415161718ULL,
                     {oldPath.data(), oldPath.size()},
                     {newPath.data(), newPath.size()}};
  size_t encodedLength = 0;
  ASSERT_EQ(encode(input, {scratch.data(), scratch.size()}, &encodedLength), Status::Ok);

  const std::vector<uint8_t> golden{
      0x42, 0x4d, 0x4a, 0x31, 0x01, 0x00, 0x28, 0x00, 0x04, 0x03, 0x02, 0x01, 0x01, 0x02, 0xa5, 0x00,
      0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01, 0x18, 0x17, 0x16, 0x15, 0x14, 0x13, 0x12, 0x11,
      0x09, 0x00, 0x0e, 0x00, 0x17, 0x00, 0x00, 0x00, 0x2f, 0x6f, 0x6c, 0x64, 0x2e, 0x65, 0x70, 0x75,
      0x62, 0x2f, 0x52, 0x65, 0x61, 0x64, 0x2f, 0x6e, 0x65, 0x77, 0x2e, 0x65, 0x70, 0x75, 0x62, 0xd2,
      0x2d, 0xbc, 0xa2};
  ASSERT_EQ(encodedLength, golden.size());
  EXPECT_EQ(std::vector<uint8_t>(scratch.begin(), scratch.begin() + encodedLength), golden);

  Record decoded{};
  ASSERT_EQ(decode(scratch.data(), encodedLength, &decoded), Status::Ok);
  expectRecord(decoded, Phase::Prepared, 0x01020304);
  for (size_t length = 0; length < encodedLength; ++length) {
    EXPECT_EQ(decode(scratch.data(), length, &decoded), Status::Corrupt) << "length=" << length;
  }
  for (uint8_t bit = 0; bit < 32; ++bit) {
    auto corrupt = golden;
    corrupt[corrupt.size() - 4 + bit / 8] ^= static_cast<uint8_t>(1U << (bit % 8));
    EXPECT_EQ(decode(corrupt.data(), corrupt.size(), &decoded), Status::Corrupt) << "crc-bit=" << unsigned(bit);
  }
}

TEST(BookStateMigrationCodec, RejectsOversizeAndNullScratchWithoutMutation) {
  std::string tooLong(kMaxPathBytes + 1, 'x');
  const Record input{0, Phase::Prepared, BookFormat::Epub, 1, 2, {tooLong.data(), tooLong.size()}, {"b", 1}};
  size_t encodedLength = 77;
  EXPECT_EQ(encode(input, {nullptr, 0}, &encodedLength), Status::ScratchTooSmall);
  EXPECT_EQ(encodedLength, 77u);
  std::array<uint8_t, kScratchCapacity> scratch{};
  scratch.fill(0x7a);
  EXPECT_EQ(encode(input, {scratch.data(), scratch.size()}, &encodedLength), Status::LimitExceeded);
  EXPECT_TRUE(std::all_of(scratch.begin(), scratch.end(), [](uint8_t byte) { return byte == 0x7a; }));
}

TEST(BookStateMigrationSlots, SelectsNewestValidSlotAcrossSequenceWrap) {
  Fixture fixture;
  Selection first{};
  ASSERT_EQ(fixture.journal.begin(fixture.input(), &first), Status::Ok);
  EXPECT_EQ(first.slot, Slot::A);
  ASSERT_EQ(fixture.journal.advance(first, Phase::SourceMoved, &first), Status::Ok);
  EXPECT_EQ(first.slot, Slot::B);

  auto& bytesA = fixture.io.mutableBytes(kSlotAPath);
  bytesA[8] = 0xff;
  bytesA[9] = 0xff;
  bytesA[10] = 0xff;
  bytesA[11] = 0xff;
  reseal(bytesA.data(), bytesA.size());
  auto& bytesB = fixture.io.mutableBytes(kSlotBPath);
  bytesB[8] = 0;
  bytesB[9] = 0;
  bytesB[10] = 0;
  bytesB[11] = 0;
  reseal(bytesB.data(), bytesB.size());

  const Selection wrapped = fixture.load();
  ASSERT_TRUE(wrapped.selected);
  EXPECT_EQ(wrapped.slot, Slot::B);
  expectRecord(wrapped.record, Phase::SourceMoved, 0);
  EXPECT_TRUE(sequenceNewer(0, UINT32_MAX));
  EXPECT_FALSE(sequenceNewer(UINT32_MAX, 0));
}

TEST(BookStateMigrationSlots, CorruptSlotFailsClosedUnlessOtherSlotIsValid) {
  Fixture fixture;
  fixture.io.put(kSlotAPath, std::vector<uint8_t>{1, 2, 3});
  Selection selection{};
  EXPECT_EQ(fixture.journal.load(&selection), Status::Corrupt);
  EXPECT_FALSE(selection.selected);

  fixture.io = TestIo{};
  Journal journal(fixture.io.io(), {fixture.scratch.data(), fixture.scratch.size()});
  ASSERT_EQ(journal.begin(fixture.input(), &selection), Status::Ok);
  fixture.io.put(kSlotBPath, std::vector<uint8_t>{1, 2, 3});
  EXPECT_EQ(journal.load(&selection), Status::Ok);
  ASSERT_TRUE(selection.selected);
  expectRecord(selection.record, Phase::Prepared, 1);
}

TEST(BookStateMigrationSlots, EveryTruncationOfEitherPhysicalSlotFailsClosed) {
  Fixture encoded;
  Selection committed{};
  ASSERT_EQ(encoded.journal.begin(encoded.input(), &committed), Status::Ok);
  const std::vector<uint8_t> complete = encoded.io.bytes(kSlotAPath);

  for (const char* const path : {kSlotAPath, kSlotBPath}) {
    for (size_t length = 0; length < complete.size(); ++length) {
      Fixture fixture;
      fixture.io.put(path, complete.data(), length);
      Selection selected{};
      EXPECT_EQ(fixture.journal.load(&selected), Status::Corrupt) << path << " length=" << length;
      EXPECT_FALSE(selected.selected);
    }
  }
}

TEST(BookStateMigrationSlots, EverySingleByteCorruptionOfEitherPhysicalSlotFailsClosed) {
  Fixture encoded;
  Selection committed{};
  ASSERT_EQ(encoded.journal.begin(encoded.input(), &committed), Status::Ok);
  const std::vector<uint8_t> complete = encoded.io.bytes(kSlotAPath);

  for (const char* const path : {kSlotAPath, kSlotBPath}) {
    for (size_t offset = 0; offset < complete.size(); ++offset) {
      Fixture fixture;
      auto corrupt = complete;
      corrupt[offset] ^= 0x01;
      fixture.io.put(path, corrupt);
      Selection selected{};
      EXPECT_EQ(fixture.journal.load(&selected), Status::Corrupt) << path << " offset=" << offset;
      EXPECT_FALSE(selected.selected);
      EXPECT_EQ(fixture.io.openHandles(), 0u);
    }
  }
}

TEST(BookStateMigrationSlots, EveryTornOrCorruptNewerPhysicalSlotPreservesOlderAuthoritativeRecord) {
  Fixture encoded;
  Selection selected{};
  ASSERT_EQ(encoded.journal.begin(encoded.input(), &selected), Status::Ok);
  const std::vector<uint8_t> older = encoded.io.bytes(kSlotAPath);
  ASSERT_EQ(encoded.journal.advance(selected, Phase::SourceMoved, &selected), Status::Ok);
  const std::vector<uint8_t> newer = encoded.io.bytes(kSlotBPath);

  for (const Slot corruptSlot : {Slot::A, Slot::B}) {
    const char* const corruptPath = corruptSlot == Slot::A ? kSlotAPath : kSlotBPath;
    const char* const validPath = corruptSlot == Slot::A ? kSlotBPath : kSlotAPath;
    for (size_t length = 0; length < newer.size(); ++length) {
      Fixture fixture;
      fixture.io.put(validPath, older);
      fixture.io.put(corruptPath, newer.data(), length);
      const Selection recovered = fixture.load();
      ASSERT_TRUE(recovered.selected);
      EXPECT_EQ(recovered.record.phase, Phase::Prepared);
      EXPECT_EQ(recovered.record.sequence, 1u);
      EXPECT_EQ(fixture.io.openHandles(), 0u);
    }
    for (size_t offset = 0; offset < newer.size(); ++offset) {
      Fixture fixture;
      auto corrupt = newer;
      corrupt[offset] ^= 0x01;
      fixture.io.put(validPath, older);
      fixture.io.put(corruptPath, corrupt);
      const Selection recovered = fixture.load();
      ASSERT_TRUE(recovered.selected);
      EXPECT_EQ(recovered.record.phase, Phase::Prepared);
      EXPECT_EQ(recovered.record.sequence, 1u);
      EXPECT_EQ(fixture.io.openHandles(), 0u);
    }
  }
}

TEST(BookStateMigrationJournal, EveryTornInactiveWriteAndDurabilityFaultKeepsOlderSelection) {
  Fixture sizing;
  Selection old{};
  ASSERT_EQ(sizing.journal.begin(sizing.input(), &old), Status::Ok);
  ASSERT_EQ(sizing.journal.advance(old, Phase::SourceMoved, &old), Status::Ok);
  const size_t recordLength = sizing.io.bytes(kSlotBPath).size();

  for (size_t prefix = 0; prefix < recordLength; ++prefix) {
    Fixture fixture;
    Selection selected{};
    ASSERT_EQ(fixture.journal.begin(fixture.input(), &selected), Status::Ok);
    fixture.io.setWriteAllowance(prefix);
    EXPECT_EQ(fixture.journal.advance(selected, Phase::SourceMoved, nullptr), Status::IoFailure) << prefix;
    fixture.io.clearWriteAllowance();
    fixture.io.clearFault();
    const Selection recovered = fixture.load();
    ASSERT_TRUE(recovered.selected);
    expectRecord(recovered.record, Phase::Prepared, 1);
    EXPECT_EQ(fixture.io.openHandles(), 0u);
  }

  for (const FaultPoint point :
       {FaultPoint::Open, FaultPoint::Write, FaultPoint::Flush, FaultPoint::Sync, FaultPoint::Close,
        FaultPoint::Read}) {
    Fixture fixture;
    Selection selected{};
    ASSERT_EQ(fixture.journal.begin(fixture.input(), &selected), Status::Ok);
    fixture.io.fail(point);
    EXPECT_NE(fixture.journal.advance(selected, Phase::SourceMoved, nullptr), Status::Ok) << unsigned(point);
    fixture.io.clearFault();
    const Selection recovered = fixture.load();
    ASSERT_TRUE(recovered.selected);
    expectRecord(recovered.record, Phase::Prepared, 1);
    EXPECT_EQ(fixture.io.openHandles(), 0u);
  }
}

TEST(BookStateMigrationJournal, SizeReopenShortReadAndReadCloseFaultsCloseHandlesAndRemainRetryable) {
  {
    Fixture fixture;
    Selection selected{};
    ASSERT_EQ(fixture.journal.begin(fixture.input(), &selected), Status::Ok);
    fixture.io.fail(FaultPoint::Size);
    EXPECT_EQ(fixture.journal.load(&selected), Status::IoFailure);
    EXPECT_EQ(fixture.io.openHandles(), 0u);
    fixture.io.clearFault();
    EXPECT_EQ(fixture.journal.load(&selected), Status::Ok);
    EXPECT_TRUE(selected.selected);
  }

  {
    Fixture fixture;
    Selection selected{};
    ASSERT_EQ(fixture.journal.begin(fixture.input(), &selected), Status::Ok);
    fixture.io.fail(FaultPoint::Open, 2);
    EXPECT_EQ(fixture.journal.advance(selected, Phase::SourceMoved, nullptr), Status::IoFailure);
    EXPECT_EQ(fixture.io.openHandles(), 0u);
    fixture.io.clearFault();
    const Selection recovered = fixture.load();
    EXPECT_EQ(recovered.record.phase, Phase::Prepared);
  }

  {
    Fixture fixture;
    Selection selected{};
    ASSERT_EQ(fixture.journal.begin(fixture.input(), &selected), Status::Ok);
    fixture.io.setReadAllowance(fixture.io.bytes(kSlotAPath).size() - 1);
    EXPECT_EQ(fixture.journal.load(&selected), Status::Corrupt);
    EXPECT_EQ(fixture.io.openHandles(), 0u);
    fixture.io.clearReadAllowance();
    EXPECT_EQ(fixture.journal.load(&selected), Status::Ok);
    EXPECT_TRUE(selected.selected);
  }

  {
    Fixture fixture;
    Selection selected{};
    ASSERT_EQ(fixture.journal.begin(fixture.input(), &selected), Status::Ok);
    fixture.io.fail(FaultPoint::Close);
    EXPECT_EQ(fixture.journal.load(&selected), Status::IoFailure);
    EXPECT_EQ(fixture.io.openHandles(), 0u);
    fixture.io.clearFault();
    EXPECT_EQ(fixture.journal.load(&selected), Status::Ok);
    EXPECT_TRUE(selected.selected);
  }
}

TEST(BookStateMigrationJournal, EveryReadbackBoundaryFaultPreservesOlderAuthoritativeSlot) {
  for (const FaultPoint point : {FaultPoint::Size, FaultPoint::Read}) {
    Fixture fixture;
    Selection selected{};
    ASSERT_EQ(fixture.journal.begin(fixture.input(), &selected), Status::Ok);
    fixture.io.fail(point);
    EXPECT_EQ(fixture.journal.advance(selected, Phase::SourceMoved, nullptr), Status::IoFailure);
    fixture.io.clearFault();
    const Selection recovered = fixture.load();
    ASSERT_TRUE(recovered.selected);
    EXPECT_EQ(recovered.record.phase, Phase::Prepared);
    EXPECT_EQ(fixture.io.openHandles(), 0u);
  }

  for (const std::pair<FaultPoint, uint32_t>& fault :
       {std::pair{FaultPoint::Open, 2U}, std::pair{FaultPoint::Close, 2U}}) {
    Fixture fixture;
    Selection selected{};
    ASSERT_EQ(fixture.journal.begin(fixture.input(), &selected), Status::Ok);
    fixture.io.fail(fault.first, fault.second);
    EXPECT_EQ(fixture.journal.advance(selected, Phase::SourceMoved, nullptr), Status::IoFailure);
    fixture.io.clearFault();
    const Selection recovered = fixture.load();
    ASSERT_TRUE(recovered.selected);
    EXPECT_EQ(recovered.record.phase, Phase::Prepared);
    EXPECT_EQ(fixture.io.openHandles(), 0u);
  }

  Fixture fixture;
  Selection selected{};
  ASSERT_EQ(fixture.journal.begin(fixture.input(), &selected), Status::Ok);
  fixture.io.setReadAllowance(fixture.io.bytes(kSlotAPath).size() - 1);
  EXPECT_EQ(fixture.journal.advance(selected, Phase::SourceMoved, nullptr), Status::Corrupt);
  fixture.io.clearReadAllowance();
  const Selection recovered = fixture.load();
  ASSERT_TRUE(recovered.selected);
  EXPECT_EQ(recovered.record.phase, Phase::Prepared);
  EXPECT_EQ(fixture.io.openHandles(), 0u);
}

TEST(BookStateMigrationJournal, InvalidCallbackTablesRejectEveryPublicMutationBeforeIo) {
  {
    Fixture fixture;
    Selection selected{};
    ASSERT_EQ(fixture.journal.begin(fixture.input(), &selected), Status::Ok);
    Io invalid = fixture.io.io();
    invalid.remove = nullptr;
    Journal journal(invalid, {fixture.scratch.data(), fixture.scratch.size()});
    EXPECT_EQ(journal.advance(selected, Phase::SourceMoved, nullptr), Status::InvalidArgument);
    EXPECT_FALSE(fixture.io.exists(kSlotBPath));
  }

  {
    Fixture fixture;
    Selection terminal{};
    ASSERT_EQ(fixture.journal.begin(fixture.input(), &terminal), Status::Ok);
    fixture.io.put(kSlotBPath, std::vector<uint8_t>{1});
    terminal.record.phase = Phase::OldStateRemoved;
    Io invalid = fixture.io.io();
    invalid.write = nullptr;
    Journal journal(invalid, {fixture.scratch.data(), fixture.scratch.size()});
    EXPECT_EQ(journal.cleanup(terminal), Status::InvalidArgument);
    EXPECT_TRUE(fixture.io.exists(kSlotAPath));
    EXPECT_TRUE(fixture.io.exists(kSlotBPath));
  }

  {
    TestIo storage;
    std::array<uint8_t, kScratchCapacity> scratch{};
    Io invalid = storage.io();
    invalid.flush = nullptr;
    Journal journal(invalid, {scratch.data(), scratch.size()});
    Selection selected{};
    EXPECT_EQ(journal.load(&selected), Status::InvalidArgument);
    EXPECT_EQ(journal.begin({}, &selected), Status::InvalidArgument);
    EXPECT_EQ(journal.readState().status, Status::InvalidArgument);
    EXPECT_EQ(storage.calls(FaultPoint::Open), 0u);
    EXPECT_EQ(storage.calls(FaultPoint::Remove), 0u);
  }
}

TEST(BookStateMigrationJournal, EveryMissingIoCallbackRejectsEveryPublicMethodWithoutMutation) {
  for (uint8_t missing = 0; missing < 8; ++missing) {
    Fixture fixture;
    Selection selected{};
    ASSERT_EQ(fixture.journal.begin(fixture.input(), &selected), Status::Ok);
    Selection terminal = selected;
    terminal.record.phase = Phase::OldStateRemoved;
    Io invalid = fixture.io.io();
    clearIoCallback(&invalid, missing);
    Journal journal(invalid, {fixture.scratch.data(), fixture.scratch.size()});
    const size_t opensBefore = fixture.io.calls(FaultPoint::Open);
    const size_t removalsBefore = fixture.io.calls(FaultPoint::Remove);

    Selection output{};
    EXPECT_EQ(journal.load(&output), Status::InvalidArgument) << unsigned(missing);
    EXPECT_EQ(journal.begin(fixture.input(), &output), Status::InvalidArgument) << unsigned(missing);
    EXPECT_EQ(journal.advance(selected, Phase::SourceMoved, &output), Status::InvalidArgument) << unsigned(missing);
    EXPECT_EQ(journal.cleanup(terminal), Status::InvalidArgument) << unsigned(missing);
    EXPECT_EQ(journal.readState().status, Status::InvalidArgument) << unsigned(missing);
    EXPECT_EQ(fixture.io.calls(FaultPoint::Open), opensBefore) << unsigned(missing);
    EXPECT_EQ(fixture.io.calls(FaultPoint::Remove), removalsBefore) << unsigned(missing);
    EXPECT_TRUE(fixture.io.exists(kSlotAPath));
    EXPECT_FALSE(fixture.io.exists(kSlotBPath));
  }
}

TEST(BookStateMigrationJournal, EveryMissingMigrationCallbackRejectsBeforeJournalIoOrDomainMutation) {
  for (uint8_t missing = 0; missing < 13; ++missing) {
    Fixture fixture;
    DurableDomain domain;
    Selection selected{};
    ASSERT_EQ(fixture.journal.begin(fixture.input(), &selected), Status::Ok);
    MigrationOperations invalid = domain.operations();
    switch (missing) {
      case 0:
        invalid.locateSource = nullptr;
        break;
      case 1:
        invalid.renameSource = nullptr;
        break;
      case 2:
        invalid.copyCache = nullptr;
        break;
      case 3:
        invalid.verifyCache = nullptr;
        break;
      case 4:
        invalid.copyBookmarks = nullptr;
        break;
      case 5:
        invalid.verifyBookmarks = nullptr;
        break;
      case 6:
        invalid.copyClippings = nullptr;
        break;
      case 7:
        invalid.verifyClippings = nullptr;
        break;
      case 8:
        invalid.verifyState = nullptr;
        break;
      case 9:
        invalid.activateRecent = nullptr;
        break;
      case 10:
        invalid.activateOpenPath = nullptr;
        break;
      case 11:
        invalid.verifyActivation = nullptr;
        break;
      case 12:
        invalid.removeOldState = nullptr;
        break;
      default:
        FAIL() << unsigned(missing);
    }
    const size_t opensBefore = fixture.io.calls(FaultPoint::Open);
    EXPECT_EQ(recoverOne(fixture.journal, invalid).status, Status::InvalidArgument);
    EXPECT_EQ(fixture.io.calls(FaultPoint::Open), opensBefore);
    EXPECT_TRUE(domain.events.empty());
    EXPECT_EQ(fixture.load().record.phase, Phase::Prepared);
  }
}

TEST(BookStateMigrationJournal, PreparedCommitFaultsHappenBeforeAnySourceOrStateMutation) {
  for (const FaultPoint point :
       {FaultPoint::Open, FaultPoint::Write, FaultPoint::Flush, FaultPoint::Sync, FaultPoint::Close,
        FaultPoint::Read}) {
    Fixture fixture;
    Domain domain;
    fixture.io.fail(point);
    Selection committed{};
    EXPECT_NE(fixture.journal.begin(fixture.input(), &committed), Status::Ok) << unsigned(point);
    EXPECT_FALSE(committed.selected);
    EXPECT_TRUE(domain.events.empty());
    EXPECT_EQ(domain.location, SourceLocation::OldOnly);
    fixture.io.clearFault();
    Selection recovered{};
    const Status loadStatus = fixture.journal.load(&recovered);
    EXPECT_TRUE(loadStatus == Status::Ok || loadStatus == Status::Corrupt);
    EXPECT_FALSE(recovered.selected);
    EXPECT_EQ(fixture.io.openHandles(), 0u);
  }
}

TEST(BookStateMigrationJournal, NullOrUndersizedCallerScratchPerformsNoIo) {
  TestIo io;
  Journal nullJournal(io.io(), {nullptr, 0});
  Selection selection{};
  EXPECT_EQ(nullJournal.load(&selection), Status::ScratchTooSmall);
  EXPECT_EQ(nullJournal.begin({}, &selection), Status::ScratchTooSmall);
  EXPECT_EQ(io.calls(FaultPoint::Open), 0u);

  std::array<uint8_t, kScratchCapacity - 1> small{};
  Journal smallJournal(io.io(), {small.data(), small.size()});
  EXPECT_EQ(smallJournal.load(&selection), Status::ScratchTooSmall);
  EXPECT_EQ(io.calls(FaultPoint::Open), 0u);
}

TEST(BookStateMigrationRecovery, RebootAfterEveryDurablePhaseAndFallbackSwitchesOnlyAtActivation) {
  Fixture fixture;
  Domain domain;
  Selection selected{};
  ASSERT_EQ(fixture.journal.begin(fixture.input(), &selected), Status::Ok);

  const std::array<Phase, 7> expected{
      Phase::SourceMoved,      Phase::CacheCopied,    Phase::BookmarksCopied, Phase::ClippingsCopied,
      Phase::StateVerified,    Phase::Activated,      Phase::OldStateRemoved};
  for (const Phase phase : expected) {
    Journal rebooted(fixture.io.io(), {fixture.scratch.data(), fixture.scratch.size()});
    const StepResult result = recoverOne(rebooted, domain.operations());
    ASSERT_EQ(result.status, Status::Ok) << unsigned(phase);
    ASSERT_EQ(result.disposition, StepDisposition::Advanced);
    EXPECT_EQ(result.durablePhase, phase);
    const ReadState read = rebooted.readState();
    ASSERT_TRUE(read.migrationPresent);
    EXPECT_EQ(read.phase, phase);
    if (phase < Phase::Activated) {
      EXPECT_EQ(read.resolution, HashResolution::OldHashFallback);
      EXPECT_EQ(read.hash, 0x0102030405060708ULL);
    } else {
      EXPECT_EQ(read.resolution, HashResolution::NewHash);
      EXPECT_EQ(read.hash, 0x1112131415161718ULL);
    }
  }

  Journal rebooted(fixture.io.io(), {fixture.scratch.data(), fixture.scratch.size()});
  const StepResult cleanup = recoverOne(rebooted, domain.operations());
  EXPECT_EQ(cleanup.status, Status::Ok);
  EXPECT_EQ(cleanup.disposition, StepDisposition::Complete);
  EXPECT_FALSE(fixture.io.exists(kSlotAPath));
  EXPECT_FALSE(fixture.io.exists(kSlotBPath));
  EXPECT_FALSE(rebooted.readState().migrationPresent);
  EXPECT_EQ(fixture.io.maxOpenHandles(), 1u);
  EXPECT_EQ(domain.cleanedPath, "/old.epub");
  EXPECT_NE(domain.cleanedPath, "/Read/new.epub");
  EXPECT_EQ(domain.cleanedHash, 0x0102030405060708ULL);
  EXPECT_EQ(domain.cleanedFormat, BookFormat::Pdf);
}

TEST(BookStateMigrationRecovery, StatefulDurableModelPreservesExactOrderAndActivationBoundary) {
  Fixture fixture;
  DurableDomain domain;
  Selection selected{};
  ASSERT_EQ(fixture.journal.begin(fixture.input(), &selected), Status::Ok);

  const std::array<Phase, 7> phases{
      Phase::SourceMoved,      Phase::CacheCopied,    Phase::BookmarksCopied, Phase::ClippingsCopied,
      Phase::StateVerified,    Phase::Activated,      Phase::OldStateRemoved};
  for (const Phase expected : phases) {
    Journal rebooted(fixture.io.io(), {fixture.scratch.data(), fixture.scratch.size()});
    const StepResult result = recoverOne(rebooted, domain.operations());
    ASSERT_EQ(result.status, Status::Ok);
    ASSERT_EQ(result.durablePhase, expected);
    const ReadState state = rebooted.readState();
    if (expected < Phase::Activated) {
      EXPECT_EQ(state.resolution, HashResolution::OldHashFallback);
      EXPECT_TRUE(domain.oldStatePresent);
    } else {
      EXPECT_EQ(state.resolution, HashResolution::NewHash);
    }
    EXPECT_TRUE(domain.newSource);
    EXPECT_FALSE(domain.removedBeforeActivation);
  }

  const std::vector<DurableAction> expected{
      DurableAction::Locate,           DurableAction::Rename,          DurableAction::Locate,
      DurableAction::CopyCache,        DurableAction::VerifyCache,     DurableAction::CopyBookmarks,
      DurableAction::VerifyBookmarks,  DurableAction::CopyClippings,   DurableAction::VerifyClippings,
      DurableAction::VerifyState,      DurableAction::ActivateRecent,  DurableAction::ActivateOpenPath,
      DurableAction::VerifyActivation, DurableAction::RemoveOldState};
  EXPECT_EQ(domain.events, expected);
  EXPECT_FALSE(domain.oldSource);
  EXPECT_TRUE(domain.newSource);
  EXPECT_TRUE(domain.cacheVerified);
  EXPECT_TRUE(domain.bookmarksVerified);
  EXPECT_TRUE(domain.clippingsVerified);
  EXPECT_TRUE(domain.stateVerified);
  EXPECT_TRUE(domain.activationVerified);
  EXPECT_FALSE(domain.oldStatePresent);
  EXPECT_FALSE(domain.removedBeforeActivation);
}

TEST(BookStateMigrationRecovery, StatefulFailuresBeforeAndAfterAreOrderedIdempotentAndRebootSafe) {
  const std::array<DurableAction, 12> actions{
      DurableAction::Rename,           DurableAction::CopyCache,        DurableAction::VerifyCache,
      DurableAction::CopyBookmarks,    DurableAction::VerifyBookmarks,  DurableAction::CopyClippings,
      DurableAction::VerifyClippings,  DurableAction::VerifyState,      DurableAction::ActivateRecent,
      DurableAction::ActivateOpenPath, DurableAction::VerifyActivation, DurableAction::RemoveOldState};

  for (const DurableAction action : actions) {
    for (const FailureTiming timing : {FailureTiming::Before, FailureTiming::After}) {
      Fixture fixture;
      DurableDomain domain;
      Selection selected{};
      ASSERT_EQ(fixture.journal.begin(fixture.input(), &selected), Status::Ok);
      driveToPhase(fixture, domain, phaseBefore(action));
      domain.events.clear();
      domain.fail(action, timing);

      Journal rebootedAtPhase(fixture.io.io(), {fixture.scratch.data(), fixture.scratch.size()});
      const StepResult failed = recoverOne(rebootedAtPhase, domain.operations());
      EXPECT_EQ(domain.events, expectedFaultEvents(action)) << unsigned(action) << "/" << unsigned(timing);
      EXPECT_FALSE(domain.removedBeforeActivation);

      if (action == DurableAction::Rename && timing == FailureTiming::Before) {
        EXPECT_EQ(failed.status, Status::OperationFailed);
        EXPECT_EQ(fixture.load().record.phase, Phase::Abandoned);
        EXPECT_TRUE(domain.oldSource);
        EXPECT_FALSE(domain.newSource);
        domain.clearFailure();
        Journal cleanupReboot(fixture.io.io(), {fixture.scratch.data(), fixture.scratch.size()});
        EXPECT_EQ(recoverOne(cleanupReboot, domain.operations()).disposition, StepDisposition::Abandoned);
        continue;
      }

      if (action == DurableAction::Rename) {
        EXPECT_EQ(failed.status, Status::Ok);
        EXPECT_EQ(failed.durablePhase, Phase::SourceMoved);
        EXPECT_FALSE(domain.oldSource);
        EXPECT_TRUE(domain.newSource);
        domain.clearFailure();
        Journal resumeReboot(fixture.io.io(), {fixture.scratch.data(), fixture.scratch.size()});
        EXPECT_EQ(recoverOne(resumeReboot, domain.operations()).durablePhase, Phase::CacheCopied);
        continue;
      }

      EXPECT_EQ(failed.status, Status::OperationFailed) << unsigned(action) << "/" << unsigned(timing);
      EXPECT_EQ(fixture.load().record.phase, phaseBefore(action));
      const ReadState fallback = fixture.journal.readState();
      if (phaseBefore(action) < Phase::Activated) {
        EXPECT_EQ(fallback.resolution, HashResolution::OldHashFallback);
        EXPECT_EQ(fallback.hash, 0x0102030405060708ULL);
        EXPECT_TRUE(domain.oldStatePresent);
      } else {
        EXPECT_EQ(fallback.resolution, HashResolution::NewHash);
        EXPECT_EQ(fallback.hash, 0x1112131415161718ULL);
      }
      EXPECT_TRUE(domain.newSource);

      domain.clearFailure();
      domain.events.clear();
      Journal retryReboot(fixture.io.io(), {fixture.scratch.data(), fixture.scratch.size()});
      const StepResult retried = recoverOne(retryReboot, domain.operations());
      EXPECT_EQ(retried.status, Status::Ok) << unsigned(action) << "/" << unsigned(timing);
      EXPECT_EQ(retried.durablePhase, phaseAfter(action));
      EXPECT_TRUE(domain.newSource);
      EXPECT_FALSE(domain.removedBeforeActivation);
      if (phaseAfter(action) == Phase::Activated) {
        EXPECT_EQ(retryReboot.readState().resolution, HashResolution::NewHash);
      }
      if (action == DurableAction::RemoveOldState) {
        EXPECT_FALSE(domain.oldStatePresent);
      }
    }
  }
}

TEST(BookStateMigrationRecovery, EveryDomainFaultLeavesPhaseRetryableWithoutLaterMutation) {
  const std::array<Domain::Action, 13> actions{
      Domain::Action::Rename,          Domain::Action::CopyCache,        Domain::Action::VerifyCache,
      Domain::Action::CopyBookmarks,   Domain::Action::VerifyBookmarks,  Domain::Action::CopyClippings,
      Domain::Action::VerifyClippings, Domain::Action::VerifyState,      Domain::Action::ActivateRecent,
      Domain::Action::ActivateOpenPath, Domain::Action::VerifyActivation, Domain::Action::RemoveOldState,
      Domain::Action::None};

  for (const Domain::Action failed : actions) {
    if (failed == Domain::Action::None) {
      continue;
    }
    Fixture fixture;
    Domain domain;
    Selection selected{};
    ASSERT_EQ(fixture.journal.begin(fixture.input(), &selected), Status::Ok);

    while (true) {
      const Selection before = fixture.load();
      const Phase phaseBefore = before.record.phase;
      domain.failAction = failed;
      const size_t failedBefore = domain.count(failed);
      const StepResult result = recoverOne(fixture.journal, domain.operations());
      if (domain.count(failed) != failedBefore) {
        EXPECT_EQ(result.status, Status::OperationFailed) << unsigned(failed);
        const Selection after = fixture.load();
        if (failed == Domain::Action::Rename) {
          EXPECT_EQ(after.record.phase, Phase::Abandoned);
          EXPECT_EQ(domain.location, SourceLocation::OldOnly);
        } else {
          EXPECT_EQ(after.record.phase, phaseBefore);
        }
        const size_t eventsAtFailure = domain.events.size();
        domain.failAction = Domain::Action::None;
        const StepResult retry = recoverOne(fixture.journal, domain.operations());
        EXPECT_EQ(retry.status, Status::Ok);
        if (failed == Domain::Action::Rename) {
          EXPECT_EQ(retry.disposition, StepDisposition::Abandoned);
        } else {
          EXPECT_GT(domain.events.size(), eventsAtFailure);
        }
        break;
      }
      ASSERT_EQ(result.status, Status::Ok);
    }
  }
}

TEST(BookStateMigrationRecovery, CrashAfterRenameBeforePhaseCommitNeverRenamesAgain) {
  Fixture fixture;
  Domain domain;
  domain.location = SourceLocation::NewOnly;
  Selection selected{};
  ASSERT_EQ(fixture.journal.begin(fixture.input(), &selected), Status::Ok);

  const StepResult result = recoverOne(fixture.journal, domain.operations());
  EXPECT_EQ(result.status, Status::Ok);
  EXPECT_EQ(result.durablePhase, Phase::SourceMoved);
  EXPECT_EQ(domain.count(Domain::Action::Rename), 0u);
}

TEST(BookStateMigrationRecovery, RenameErrorAfterMutationDurablyAdvancesAndSurvivesReboot) {
  Fixture fixture;
  Domain domain;
  domain.failRenameAfterMutation = true;
  Selection selected{};
  ASSERT_EQ(fixture.journal.begin(fixture.input(), &selected), Status::Ok);

  const StepResult moved = recoverOne(fixture.journal, domain.operations());
  ASSERT_EQ(moved.status, Status::Ok);
  EXPECT_EQ(moved.disposition, StepDisposition::Advanced);
  EXPECT_EQ(moved.durablePhase, Phase::SourceMoved);
  EXPECT_EQ(domain.location, SourceLocation::NewOnly);
  EXPECT_EQ(domain.count(Domain::Action::Rename), 1u);
  EXPECT_EQ(fixture.load().record.phase, Phase::SourceMoved);

  Journal rebooted(fixture.io.io(), {fixture.scratch.data(), fixture.scratch.size()});
  domain.failRenameAfterMutation = false;
  const StepResult resumed = recoverOne(rebooted, domain.operations());
  EXPECT_EQ(resumed.status, Status::Ok);
  EXPECT_EQ(resumed.durablePhase, Phase::CacheCopied);
  EXPECT_EQ(domain.count(Domain::Action::Rename), 1u);
}

TEST(BookStateMigrationRecovery, RenameErrorPostObservationFailsClosedWithoutAbandoningAmbiguousSource) {
  for (const SourceLocation location : {SourceLocation::Both, SourceLocation::Missing}) {
    Fixture fixture;
    Domain domain;
    domain.failAction = Domain::Action::Rename;
    domain.setLocationOnRenameFailure = true;
    domain.locationOnRenameFailure = location;
    Selection selected{};
    ASSERT_EQ(fixture.journal.begin(fixture.input(), &selected), Status::Ok);

    const StepResult result = recoverOne(fixture.journal, domain.operations());
    EXPECT_EQ(result.status, location == SourceLocation::Both ? Status::Conflict : Status::NotFound);
    EXPECT_EQ(fixture.load().record.phase, Phase::Prepared);
    EXPECT_TRUE(fixture.io.exists(kSlotAPath));
    EXPECT_FALSE(fixture.io.exists(kSlotBPath));
  }

  Fixture fixture;
  Domain domain;
  domain.failAction = Domain::Action::Rename;
  domain.failLocateOnCall = 2;
  Selection selected{};
  ASSERT_EQ(fixture.journal.begin(fixture.input(), &selected), Status::Ok);
  EXPECT_EQ(recoverOne(fixture.journal, domain.operations()).status, Status::OperationFailed);
  EXPECT_EQ(fixture.load().record.phase, Phase::Prepared);
  EXPECT_TRUE(fixture.io.exists(kSlotAPath));
  EXPECT_FALSE(fixture.io.exists(kSlotBPath));
}

TEST(BookStateMigrationRecovery, RepeatedRecoveryIsIdempotentAfterJournalCommitFault) {
  Fixture fixture;
  Domain domain;
  Selection selected{};
  ASSERT_EQ(fixture.journal.begin(fixture.input(), &selected), Status::Ok);
  ASSERT_EQ(recoverOne(fixture.journal, domain.operations()).status, Status::Ok);

  fixture.io.fail(FaultPoint::Write);
  EXPECT_EQ(recoverOne(fixture.journal, domain.operations()).status, Status::IoFailure);
  EXPECT_EQ(domain.count(Domain::Action::CopyCache), 1u);
  EXPECT_EQ(domain.count(Domain::Action::VerifyCache), 1u);
  fixture.io.clearFault();
  EXPECT_EQ(recoverOne(fixture.journal, domain.operations()).status, Status::Ok);
  EXPECT_EQ(domain.count(Domain::Action::CopyCache), 2u);
  EXPECT_EQ(domain.count(Domain::Action::VerifyCache), 2u);
  EXPECT_EQ(fixture.load().record.phase, Phase::CacheCopied);
}

TEST(BookStateMigrationRecovery, TerminalCleanupFaultLeavesRecoverableTerminalRecord) {
  Fixture fixture;
  Domain domain;
  Selection selected{};
  ASSERT_EQ(fixture.journal.begin(fixture.input(), &selected), Status::Ok);
  while (fixture.load().record.phase != Phase::OldStateRemoved) {
    ASSERT_EQ(recoverOne(fixture.journal, domain.operations()).status, Status::Ok);
  }

  fixture.io.fail(FaultPoint::Remove);
  EXPECT_EQ(recoverOne(fixture.journal, domain.operations()).status, Status::IoFailure);
  fixture.io.clearFault();
  const Selection terminal = fixture.load();
  ASSERT_TRUE(terminal.selected);
  EXPECT_EQ(terminal.record.phase, Phase::OldStateRemoved);
  EXPECT_EQ(recoverOne(fixture.journal, domain.operations()).disposition, StepDisposition::Complete);
}

TEST(BookStateMigrationRecovery, EitherTerminalSlotRemovalFaultLeavesNewestTerminalRecordRetryable) {
  for (const uint32_t occurrence : {1U, 2U}) {
    Fixture fixture;
    Domain domain;
    Selection selected{};
    ASSERT_EQ(fixture.journal.begin(fixture.input(), &selected), Status::Ok);
    while (fixture.load().record.phase != Phase::OldStateRemoved) {
      ASSERT_EQ(recoverOne(fixture.journal, domain.operations()).status, Status::Ok);
    }

    fixture.io.fail(FaultPoint::Remove, occurrence);
    EXPECT_EQ(recoverOne(fixture.journal, domain.operations()).status, Status::IoFailure) << occurrence;
    fixture.io.clearFault();
    const Selection terminal = fixture.load();
    ASSERT_TRUE(terminal.selected);
    EXPECT_EQ(terminal.record.phase, Phase::OldStateRemoved);
    EXPECT_EQ(recoverOne(fixture.journal, domain.operations()).disposition, StepDisposition::Complete);
  }
}

TEST(BookStateMigrationRecovery, TerminalRemovalFailAfterMutationIsSafeAcrossReboot) {
  for (const uint32_t occurrence : {1U, 2U}) {
    Fixture fixture;
    DurableDomain domain;
    Selection selected{};
    ASSERT_EQ(fixture.journal.begin(fixture.input(), &selected), Status::Ok);
    driveToPhase(fixture, domain, Phase::OldStateRemoved);
    ASSERT_FALSE(domain.oldStatePresent);
    ASSERT_TRUE(domain.newSource);

    fixture.io.failAfter(FaultPoint::Remove, occurrence);
    EXPECT_EQ(recoverOne(fixture.journal, domain.operations()).status, Status::IoFailure);
    EXPECT_EQ(fixture.io.openHandles(), 0u);
    fixture.io.clearFault();

    Journal rebooted(fixture.io.io(), {fixture.scratch.data(), fixture.scratch.size()});
    const StepResult recovered = recoverOne(rebooted, domain.operations());
    if (occurrence == 1) {
      EXPECT_EQ(recovered.disposition, StepDisposition::Complete);
    } else {
      EXPECT_EQ(recovered.disposition, StepDisposition::Idle);
    }
    EXPECT_TRUE(domain.newSource);
    EXPECT_FALSE(domain.oldStatePresent);
    EXPECT_FALSE(domain.removedBeforeActivation);
  }
}

TEST(BookStateMigrationRecovery, AbandonedCleanupFaultLeavesRecoverableTerminalRecord) {
  Fixture fixture;
  Domain domain;
  domain.failAction = Domain::Action::Rename;
  Selection selected{};
  ASSERT_EQ(fixture.journal.begin(fixture.input(), &selected), Status::Ok);
  ASSERT_EQ(recoverOne(fixture.journal, domain.operations()).status, Status::OperationFailed);
  ASSERT_EQ(fixture.load().record.phase, Phase::Abandoned);

  fixture.io.fail(FaultPoint::Remove);
  EXPECT_EQ(recoverOne(fixture.journal, domain.operations()).status, Status::IoFailure);
  fixture.io.clearFault();
  EXPECT_EQ(fixture.load().record.phase, Phase::Abandoned);
  EXPECT_EQ(recoverOne(fixture.journal, domain.operations()).disposition, StepDisposition::Abandoned);
  EXPECT_EQ(domain.location, SourceLocation::OldOnly);
  EXPECT_EQ(domain.count(Domain::Action::RemoveOldState), 0u);
}

TEST(BookStateMigrationRecovery, CorruptJournalAndSourceConflictCauseNoDomainMutation) {
  Fixture fixture;
  Domain domain;
  fixture.io.put(kSlotAPath, std::vector<uint8_t>{1, 2, 3, 4});
  EXPECT_EQ(recoverOne(fixture.journal, domain.operations()).status, Status::Corrupt);
  EXPECT_TRUE(domain.events.empty());

  fixture.io = TestIo{};
  Journal readFailureJournal(fixture.io.io(), {fixture.scratch.data(), fixture.scratch.size()});
  Selection readFailureSelection{};
  ASSERT_EQ(readFailureJournal.begin(fixture.input(), &readFailureSelection), Status::Ok);
  fixture.io.fail(FaultPoint::Read);
  EXPECT_EQ(recoverOne(readFailureJournal, domain.operations()).status, Status::IoFailure);
  EXPECT_TRUE(domain.events.empty());
  fixture.io.clearFault();

  fixture.io = TestIo{};
  Journal journal(fixture.io.io(), {fixture.scratch.data(), fixture.scratch.size()});
  Selection selected{};
  ASSERT_EQ(journal.begin(fixture.input(), &selected), Status::Ok);
  domain.location = SourceLocation::Both;
  EXPECT_EQ(recoverOne(journal, domain.operations()).status, Status::Conflict);
  EXPECT_TRUE(domain.events.empty());
  EXPECT_EQ(journal.readState().resolution, HashResolution::OldHashFallback);
}

}  // namespace
