#include "PdfDeleteUtils.h"

#include <PdfDeleteCoordinator.h>
#include <PdfSourceIdentity.h>
#include <uzlib.h>

#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>

#include <atomic>
#include <cstdio>
#include <cstring>
#include <limits>

#include "BookmarkStore.h"
#include "BookMoveUtils.h"
#include "ClippingStore.h"
#include "RecentBooksStore.h"

namespace {

using PdfDelete::BeginDisposition;
using PdfDelete::BookFormat;
using PdfDelete::Coordinator;
using PdfDelete::Handle;
using PdfDelete::Io;
using PdfDelete::OpenMode;
using PdfDelete::Operations;
using PdfDelete::Phase;
using PdfDelete::Request;
using PdfDelete::RunDisposition;
using PdfDelete::Selection;
using PdfDelete::Status;
using PdfDelete::StringView;
using PdfDelete::Targets;

constexpr char CROSSPOINT_DIRECTORY[] = "/.crosspoint";
constexpr char BOOKMARK_DIRECTORY[] = "/.crosspoint/bookmarks";
constexpr char CLIPPING_DIRECTORY[] = "/.crosspoint/clippings";
constexpr size_t TARGET_PATH_CAPACITY = PdfDelete::kMaxPathBytes + 1U;
constexpr size_t CACHE_PATH_CAPACITY = 128;
constexpr size_t STORE_PATH_CAPACITY = 64;

enum class JournalPresence : uint8_t {
  LookupRequired,
  KnownAbsent,
  DeleteStarting,
};

std::atomic<JournalPresence> journalPresence{JournalPresence::LookupRequired};

struct JournalIoWorkspace {
  HalFile file;
  uint8_t scratch[PdfDelete::kScratchCapacity]{};
};

struct DeleteWorkspace {
  JournalIoWorkspace journalIo;
  char source[TARGET_PATH_CAPACITY]{};
  char tombstone[TARGET_PATH_CAPACITY]{};
  char cache[CACHE_PATH_CAPACITY]{};
  char bookmarks[STORE_PATH_CAPACITY]{};
  char clippings[STORE_PATH_CAPACITY]{};
  Targets targets{};
};

static_assert(sizeof(DeleteWorkspace) <= 9 * 1024, "PDF delete workspace exceeded its cold-path RAM budget");

struct DeleteSession {
  DeleteWorkspace workspace;
  PdfDelete::Journal journal;
  Operations operations;

  DeleteSession();
};

struct JournalReadSession {
  JournalIoWorkspace workspace;
  PdfDelete::Journal journal;
  Selection selection{};

  JournalReadSession()
      : journal(makeJournalIo(workspace), {workspace.scratch, sizeof(workspace.scratch)}) {}

  static Io makeJournalIo(JournalIoWorkspace& workspace);
};

static_assert(sizeof(JournalReadSession) <= 7 * 1024,
              "PDF delete journal reader exceeded its RAM budget");

bool sameView(const StringView view, const char* const expected) {
  if (view.data == nullptr || expected == nullptr) return false;
  const size_t length = std::strlen(expected);
  return view.length == length && std::memcmp(view.data, expected, length) == 0;
}

bool hasPdfExtension(const char* const path, const size_t length) {
  if (path == nullptr || length < 4) return false;
  const char* const suffix = path + length - 4U;
  const auto lower = [](const char value) {
    return value >= 'A' && value <= 'Z' ? static_cast<char>(value + ('a' - 'A')) : value;
  };
  return suffix[0] == '.' && lower(suffix[1]) == 'p' && lower(suffix[2]) == 'd' && lower(suffix[3]) == 'f';
}

bool canonicalAbsolutePath(const char* const path, const size_t length) {
  if (path == nullptr || length < 2 || length > PdfDelete::kMaxPathBytes || path[0] != '/' ||
      path[length - 1U] == '/' || std::memchr(path, '\0', length) != nullptr) {
    return false;
  }
  size_t segmentStart = 1;
  for (size_t index = 1; index <= length; ++index) {
    const bool boundary = index == length || path[index] == '/';
    if (!boundary) {
      if (path[index] == '\\') return false;
      continue;
    }
    const size_t segmentLength = index - segmentStart;
    if (segmentLength == 0 || (segmentLength == 1 && path[segmentStart] == '.') ||
        (segmentLength == 2 && path[segmentStart] == '.' && path[segmentStart + 1U] == '.')) {
      return false;
    }
    segmentStart = index + 1U;
  }
  return true;
}

bool copyView(const StringView source, char* const destination, const size_t capacity) {
  if (source.data == nullptr || destination == nullptr || source.length >= capacity) return false;
  std::memcpy(destination, source.data, source.length);
  destination[source.length] = '\0';
  return true;
}

Status journalOpen(void* const context, const char* const path, const OpenMode mode, Handle* const handle) {
  auto& workspace = *static_cast<JournalIoWorkspace*>(context);
  if (path == nullptr || handle == nullptr || handle->valid() || workspace.file.isOpen()) {
    return Status::InvalidArgument;
  }
  const bool opened = mode == OpenMode::Read ? Storage.openFileForRead("PdfDelete", path, workspace.file)
                                             : Storage.openFileForWrite("PdfDelete", path, workspace.file);
  if (!opened) {
    return mode == OpenMode::Read && !Storage.exists(path) ? Status::NotFound : Status::IoFailure;
  }
  handle->value = 1;
  return Status::Ok;
}

Status journalSize(void* const context, const Handle handle, size_t* const size) {
  auto& workspace = *static_cast<JournalIoWorkspace*>(context);
  if (!handle.valid() || !workspace.file.isOpen() || size == nullptr) return Status::InvalidArgument;
  const uint64_t value = workspace.file.fileSize64();
  if (value > std::numeric_limits<size_t>::max()) return Status::LimitExceeded;
  *size = static_cast<size_t>(value);
  return Status::Ok;
}

Status journalRead(void* const context, const Handle handle, const size_t offset, uint8_t* const destination,
                   const size_t length, size_t* const actual) {
  auto& workspace = *static_cast<JournalIoWorkspace*>(context);
  if (!handle.valid() || !workspace.file.isOpen() || destination == nullptr || actual == nullptr ||
      !workspace.file.seek64(offset)) {
    return Status::IoFailure;
  }
  const int count = workspace.file.read(destination, length);
  if (count < 0) return Status::IoFailure;
  *actual = static_cast<size_t>(count);
  return Status::Ok;
}

Status journalWrite(void* const context, const Handle handle, const uint8_t* const source, const size_t length,
                    size_t* const actual) {
  auto& workspace = *static_cast<JournalIoWorkspace*>(context);
  if (!handle.valid() || !workspace.file.isOpen() || source == nullptr || actual == nullptr) {
    return Status::InvalidArgument;
  }
  *actual = workspace.file.write(source, length);
  return *actual == length ? Status::Ok : Status::IoFailure;
}

Status journalFlush(void* const context, const Handle handle) {
  auto& workspace = *static_cast<JournalIoWorkspace*>(context);
  if (!handle.valid() || !workspace.file.isOpen()) return Status::InvalidArgument;
  workspace.file.flush();
  return Status::Ok;
}

Status journalSync(void* const context, const Handle handle) {
  auto& workspace = *static_cast<JournalIoWorkspace*>(context);
  return handle.valid() && workspace.file.isOpen() && workspace.file.sync() ? Status::Ok : Status::IoFailure;
}

Status journalClose(void* const context, Handle* const handle) {
  auto& workspace = *static_cast<JournalIoWorkspace*>(context);
  if (handle == nullptr || !handle->valid()) return Status::InvalidArgument;
  const bool closed = workspace.file.close();
  handle->invalidate();
  return closed ? Status::Ok : Status::IoFailure;
}

Status journalRemove(void*, const char* const path) {
  if (path == nullptr) return Status::InvalidArgument;
  if (!Storage.exists(path)) return Status::NotFound;
  return Storage.remove(path) ? Status::Ok : Status::IoFailure;
}

Io makeJournalIo(JournalIoWorkspace& workspace) {
  return {&workspace,    &journalOpen, &journalSize,  &journalRead,  &journalWrite,
          &journalFlush, &journalSync, &journalClose, &journalRemove};
}

Io JournalReadSession::makeJournalIo(JournalIoWorkspace& workspace) { return ::makeJournalIo(workspace); }

bool formatStorePath(const char* const directory, const char* const source, const size_t sourceLength,
                     char* const destination, const size_t capacity) {
  const uint32_t crc = uzlib_crc32(source, static_cast<unsigned int>(sourceLength), 0);
  const int written = std::snprintf(destination, capacity, "%s/pdf_%lu.bin", directory,
                                    static_cast<unsigned long>(crc));
  return written > 0 && static_cast<size_t>(written) < capacity;
}

bool initializeTargets(DeleteWorkspace& workspace, const StringView source) {
  if (!canonicalAbsolutePath(source.data, source.length) || !hasPdfExtension(source.data, source.length) ||
      !copyView(source, workspace.source, sizeof(workspace.source))) {
    return false;
  }

  size_t tombstoneLength = 0;
  if (PdfDelete::formatTombstonePath(source, workspace.tombstone, sizeof(workspace.tombstone), &tombstoneLength) !=
      Status::Ok) {
    return false;
  }
  const uint64_t cacheHash = pdfPathHash64(source.data, source.length);
  if (!pdfFormatCacheRootForHash(CROSSPOINT_DIRECTORY, cacheHash, workspace.cache, sizeof(workspace.cache)) ||
      !formatStorePath(BOOKMARK_DIRECTORY, source.data, source.length, workspace.bookmarks,
                       sizeof(workspace.bookmarks)) ||
      !formatStorePath(CLIPPING_DIRECTORY, source.data, source.length, workspace.clippings,
                       sizeof(workspace.clippings))) {
    return false;
  }

  workspace.targets = {
      {workspace.source, source.length},
      {workspace.tombstone, tombstoneLength},
      {workspace.cache, std::strlen(workspace.cache)},
      {workspace.bookmarks, std::strlen(workspace.bookmarks)},
      {workspace.clippings, std::strlen(workspace.clippings)},
      {RecentBooksStore::getFilePath(), std::strlen(RecentBooksStore::getFilePath())},
  };
  return true;
}

bool exactTargets(const DeleteWorkspace& workspace, const Targets& targets) {
  return sameView(targets.source, workspace.source) && sameView(targets.tombstone, workspace.tombstone) &&
         sameView(targets.cache, workspace.cache) && sameView(targets.bookmarks, workspace.bookmarks) &&
         sameView(targets.clippings, workspace.clippings) &&
         sameView(targets.recent, RecentBooksStore::getFilePath());
}

Status validateTargets(void* const context, const Targets& targets) {
  const auto& workspace = *static_cast<const DeleteWorkspace*>(context);
  return exactTargets(workspace, targets) ? Status::Ok : Status::InvalidArgument;
}

Status hideSource(void* const context, const Targets& targets) {
  const auto& workspace = *static_cast<const DeleteWorkspace*>(context);
  if (!exactTargets(workspace, targets)) return Status::InvalidArgument;
  const bool sourceExists = Storage.exists(workspace.source);
  const bool tombstoneExists = Storage.exists(workspace.tombstone);
  if (sourceExists && tombstoneExists) return Status::Conflict;
  if (!sourceExists) return tombstoneExists ? Status::Ok : Status::OperationFailed;
  return Storage.rename(workspace.source, workspace.tombstone) ? Status::Ok : Status::OperationFailed;
}

Status purgeFullCache(void* const context, const Targets& targets) {
  const auto& workspace = *static_cast<const DeleteWorkspace*>(context);
  if (!exactTargets(workspace, targets)) return Status::InvalidArgument;
  if (!Storage.exists(workspace.cache)) return Status::Ok;
  return Storage.removeDir(workspace.cache) ? Status::Ok : Status::OperationFailed;
}

Status purgeBookmarks(void* const context, const Targets& targets) {
  const auto& workspace = *static_cast<const DeleteWorkspace*>(context);
  if (!exactTargets(workspace, targets)) return Status::InvalidArgument;
  return BookmarkStore::deletePdfForFilePathNoPathAlloc(
             std::string_view(workspace.source, targets.source.length))
             ? Status::Ok
             : Status::OperationFailed;
}

Status purgeClippings(void* const context, const Targets& targets) {
  const auto& workspace = *static_cast<const DeleteWorkspace*>(context);
  if (!exactTargets(workspace, targets)) return Status::InvalidArgument;
  return ClippingStore::deletePdfForFilePathNoPathAlloc(
             std::string_view(workspace.source, targets.source.length))
             ? Status::Ok
             : Status::OperationFailed;
}

Status purgeRecents(void* const context, const Targets& targets) {
  const auto& workspace = *static_cast<const DeleteWorkspace*>(context);
  if (!exactTargets(workspace, targets)) return Status::InvalidArgument;
  return RECENT_BOOKS.removeByPathDurablyNoPathAlloc(
             std::string_view(workspace.source, targets.source.length))
             ? Status::Ok
             : Status::OperationFailed;
}

Status removeHiddenSource(void* const context, const Targets& targets) {
  const auto& workspace = *static_cast<const DeleteWorkspace*>(context);
  if (!exactTargets(workspace, targets)) return Status::InvalidArgument;
  if (!Storage.exists(workspace.tombstone)) return Status::Ok;
  return Storage.remove(workspace.tombstone) ? Status::Ok : Status::OperationFailed;
}

Operations makeOperations(DeleteWorkspace& workspace) {
  return {&workspace,       &validateTargets, &hideSource,    &purgeFullCache,
          &purgeBookmarks, &purgeClippings,  &purgeRecents, &removeHiddenSource};
}

DeleteSession::DeleteSession()
    : journal(makeJournalIo(workspace.journalIo),
              {workspace.journalIo.scratch, sizeof(workspace.journalIo.scratch)}),
      operations(makeOperations(workspace)) {}

PdfDeleteUtils::Result resultFor(const PdfDelete::RunResult& result) {
  if (result.status == Status::Conflict) return PdfDeleteUtils::Result::Conflict;
  if (result.status != Status::Ok || result.disposition == RunDisposition::Pending) {
    return PdfDeleteUtils::Result::Pending;
  }
  if (result.disposition == RunDisposition::Complete) return PdfDeleteUtils::Result::Complete;
  return PdfDeleteUtils::Result::NoPendingDelete;
}

bool pathMatches(const StringView view, const std::string_view path) {
  return view.data != nullptr && view.length == path.size() &&
         std::memcmp(view.data, path.data(), view.length) == 0;
}

BookMutationFence classifySelection(const Selection& selection, const std::string& sourcePath) {
  if (!selection.selected) return BookMutationFence::Clear;
  return pathMatches(selection.record.targets.source, sourcePath) ? BookMutationFence::MatchingPending
                                                                  : BookMutationFence::UnrelatedPending;
}

bool acquireStart() {
  JournalPresence current = journalPresence.load(std::memory_order_acquire);
  while (current != JournalPresence::DeleteStarting) {
    if (journalPresence.compare_exchange_weak(current, JournalPresence::DeleteStarting, std::memory_order_acq_rel,
                                              std::memory_order_acquire)) {
      return true;
    }
  }
  return false;
}

void publishResult(const PdfDeleteUtils::Result result) {
  const bool absent = result == PdfDeleteUtils::Result::Complete ||
                      result == PdfDeleteUtils::Result::NoPendingDelete ||
                      result == PdfDeleteUtils::Result::Unsupported || result == PdfDeleteUtils::Result::Invalid;
  journalPresence.store(absent ? JournalPresence::KnownAbsent : JournalPresence::LookupRequired,
                        std::memory_order_release);
}

PdfDeleteUtils::Result checkMoveFence(const std::string_view sourcePath) {
  const BookMutationFence fence =
      BookMoveUtils::mutationFenceForPathNoPathAlloc(sourcePath);
  if (fence == BookMutationFence::Clear) return PdfDeleteUtils::Result::NoPendingDelete;
  return fence == BookMutationFence::Indeterminate ? PdfDeleteUtils::Result::Pending
                                                   : PdfDeleteUtils::Result::Conflict;
}

PdfDeleteUtils::Result recoverSession(DeleteSession& session, Selection selection) {
  if (!selection.selected) return PdfDeleteUtils::Result::NoPendingDelete;
  if (!initializeTargets(session.workspace, selection.record.targets.source)) {
    return PdfDeleteUtils::Result::Pending;
  }
  const std::string_view sourcePath(
      session.workspace.source, selection.record.targets.source.length);
  const PdfDeleteUtils::Result moveFence = checkMoveFence(sourcePath);
  if (moveFence != PdfDeleteUtils::Result::NoPendingDelete) return moveFence;
  Coordinator coordinator(session.journal, session.operations);
  return resultFor(coordinator.recover());
}

PdfDeleteUtils::Result deleteWithSession(DeleteSession& session,
                                         const std::string_view sourcePath) {
  if (!canonicalAbsolutePath(sourcePath.data(), sourcePath.size())) {
    return PdfDeleteUtils::Result::Invalid;
  }
  if (!hasPdfExtension(sourcePath.data(), sourcePath.size())) {
    return PdfDeleteUtils::Result::Unsupported;
  }
  if (!acquireStart()) return PdfDeleteUtils::Result::Pending;

  Selection existing{};
  const Status loaded = session.journal.load(&existing);
  if (loaded != Status::Ok) {
    publishResult(PdfDeleteUtils::Result::Pending);
    return PdfDeleteUtils::Result::Pending;
  }
  if (existing.selected) {
    const bool requestedExisting =
        pathMatches(existing.record.targets.source, sourcePath);
    const PdfDeleteUtils::Result recovered = recoverSession(session, existing);
    if (recovered != PdfDeleteUtils::Result::Complete || requestedExisting) {
      publishResult(recovered);
      return recovered;
    }
  }

  const PdfDeleteUtils::Result moveFence = checkMoveFence(sourcePath);
  if (moveFence != PdfDeleteUtils::Result::NoPendingDelete) {
    publishResult(moveFence);
    return moveFence;
  }
  if (!initializeTargets(session.workspace,
                         {sourcePath.data(), sourcePath.size()})) {
    publishResult(PdfDeleteUtils::Result::Invalid);
    return PdfDeleteUtils::Result::Invalid;
  }
  if (!Storage.exists(session.workspace.source) ||
      Storage.exists(session.workspace.tombstone)) {
    publishResult(PdfDeleteUtils::Result::Invalid);
    return PdfDeleteUtils::Result::Invalid;
  }

  Coordinator coordinator(session.journal, session.operations);
  const Request request{session.workspace.targets, BookFormat::Pdf};
  const PdfDelete::BeginResult begun = coordinator.begin(request);
  if (begun.disposition != BeginDisposition::Armed) {
    PdfDeleteUtils::Result result = PdfDeleteUtils::Result::Pending;
    if (begun.status == Status::Conflict) {
      result = PdfDeleteUtils::Result::Conflict;
    } else if (begun.status == Status::InvalidArgument ||
               begun.status == Status::LimitExceeded) {
      result = PdfDeleteUtils::Result::Invalid;
    }
    publishResult(result);
    return result;
  }

  const PdfDeleteUtils::Result result = resultFor(coordinator.recover());
  publishResult(result);
  return result;
}

}  // namespace

namespace PdfDeleteUtils {

class DirectoryDeleteSession {
 public:
  DeleteSession session;
};

void DirectoryDeleteSessionDeleter::operator()(
    DirectoryDeleteSession* const session) const {
  delete session;
}

DirectoryDeleteSessionPtr makeDirectoryDeleteSessionNoThrow() {
  auto session = makeUniqueNoThrow<DirectoryDeleteSession>();
  if (!session) {
    LOG_ERR("PdfDelete", "Out of memory allocating reusable PDF delete workspace");
    return {};
  }
  return DirectoryDeleteSessionPtr(session.release());
}

BookMutationFence mutationFenceForPath(const std::string& sourcePath) {
  const JournalPresence presence = journalPresence.load(std::memory_order_acquire);
  if (presence == JournalPresence::KnownAbsent) return BookMutationFence::Clear;
  if (presence == JournalPresence::DeleteStarting) return BookMutationFence::Indeterminate;

  auto session = makeUniqueNoThrow<JournalReadSession>();
  if (!session) {
    LOG_ERR("PdfDelete", "Out of memory reading PDF delete journal");
    return BookMutationFence::Indeterminate;
  }
  const Status status = session->journal.load(&session->selection);
  if (status != Status::Ok) return BookMutationFence::Indeterminate;
  if (!session->selection.selected) {
    JournalPresence expected = JournalPresence::LookupRequired;
    if (!journalPresence.compare_exchange_strong(expected, JournalPresence::KnownAbsent, std::memory_order_acq_rel,
                                                 std::memory_order_acquire) &&
        expected == JournalPresence::DeleteStarting) {
      return BookMutationFence::Indeterminate;
    }
    return BookMutationFence::Clear;
  }
  return classifySelection(session->selection, sourcePath);
}

Result recoverPendingPdfDelete() {
  if (journalPresence.load(std::memory_order_acquire) == JournalPresence::KnownAbsent) {
    return Result::NoPendingDelete;
  }
  if (!acquireStart()) return Result::Pending;

  // The 6,182-byte journal codec scratch is too large for the task stack, but
  // is still smaller than the full delete workspace. Decode first so an
  // ordinary boot with no journal never allocates the full recovery session.
  {
    auto reader = makeUniqueNoThrow<JournalReadSession>();
    if (!reader) {
      LOG_ERR("PdfDelete", "Out of memory allocating PDF delete journal reader");
      publishResult(Result::Pending);
      return Result::Pending;
    }
    const Status loaded = reader->journal.load(&reader->selection);
    if (loaded != Status::Ok) {
      publishResult(Result::Pending);
      return Result::Pending;
    }
    if (!reader->selection.selected) {
      publishResult(Result::NoPendingDelete);
      return Result::NoPendingDelete;
    }
  }

  auto session = makeUniqueNoThrow<DeleteSession>();
  if (!session) {
    LOG_ERR("PdfDelete", "Out of memory allocating PDF delete recovery workspace");
    publishResult(Result::Pending);
    return Result::Pending;
  }
  Selection selection{};
  const Status loaded = session->journal.load(&selection);
  Result result = loaded == Status::Ok ? recoverSession(*session, selection) : Result::Pending;
  publishResult(result);
  return result;
}

Result deletePdfBook(const std::string& sourcePath) {
  if (!canonicalAbsolutePath(sourcePath.data(), sourcePath.size())) return Result::Invalid;
  if (!hasPdfExtension(sourcePath.data(), sourcePath.size())) return Result::Unsupported;
  auto session = makeDirectoryDeleteSessionNoThrow();
  if (!session) {
    LOG_ERR("PdfDelete", "Out of memory allocating PDF delete workspace");
    return Result::Pending;
  }
  return deleteWithSession(session->session, sourcePath);
}

Result deletePdfBookNoPathAlloc(DirectoryDeleteSession& session,
                                const std::string_view sourcePath) {
  return deleteWithSession(session.session, sourcePath);
}

#if defined(PDF_DELETE_TESTING)
void resetPresenceForTest() { journalPresence.store(JournalPresence::LookupRequired, std::memory_order_release); }
void markDeleteStartingForTest() {
  journalPresence.store(JournalPresence::DeleteStarting, std::memory_order_release);
}
#endif

}  // namespace PdfDeleteUtils
