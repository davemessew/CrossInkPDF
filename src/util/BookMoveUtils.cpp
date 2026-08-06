#include "BookMoveUtils.h"

#include <BookMoveCoordinator.h>
#include <Epub.h>
#include <FsHelpers.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>
#include "PdfDeleteUtils.h"
#include <PdfSourceIdentity.h>
#include <uzlib.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <limits>

#include "BookmarkStore.h"
#include "ClippingStore.h"
#include "CrossPointState.h"
#include "RecentBooksStore.h"

namespace {

using BookStateMigration::BookFormat;
using BookStateMigration::Coordinator;
using BookStateMigration::Handle;
using BookStateMigration::Io;
using BookStateMigration::MigrationOperations;
using BookStateMigration::OldState;
using BookStateMigration::OpenMode;
using BookStateMigration::Phase;
using BookStateMigration::RecentsPolicy;
using BookStateMigration::Record;
using BookStateMigration::Request;
using BookStateMigration::RunDisposition;
using BookStateMigration::Selection;
using BookStateMigration::SourceLocation;
using BookStateMigration::SourceObservation;
using BookStateMigration::Status;
using BookStateMigration::StringView;

constexpr char CROSSPOINT_DIR[] = "/.crosspoint";
constexpr char READ_FOLDER[] = "/Read";
constexpr size_t CACHE_PATH_CAPACITY = BookStateMigration::kMaxPathBytes + 1U;
constexpr size_t CACHE_TEMP_PATH_CAPACITY = CACHE_PATH_CAPACITY + 8U;
constexpr size_t DIRECTORY_ENTRY_CAPACITY = 256;
constexpr size_t COPY_CHUNK_BYTES = 2048;
constexpr uint8_t MAX_CACHE_DEPTH = 8;

enum class JournalPresence : uint8_t {
  LookupRequired,
  KnownAbsent,
  MoveStarting,
};

enum class DirectoryNextResult : uint8_t {
  Entry,
  End,
  Error,
};

std::atomic<JournalPresence> journalPresence{JournalPresence::LookupRequired};

struct JournalIoWorkspace {
  HalFile file;
  uint8_t scratch[BookStateMigration::kScratchCapacity]{};
};

struct DirectoryFrame {
  uint64_t resumeOffset = 0;
  size_t sourceLength = 0;
  size_t destinationLength = 0;
};

struct BookMoveWorkspace {
  JournalIoWorkspace journal;
  HalFile directory;
  HalFile entry;
  uint8_t copyBuffer[COPY_CHUNK_BYTES]{};
  char sourceCachePath[CACHE_PATH_CAPACITY]{};
  char destinationCachePath[CACHE_PATH_CAPACITY]{};
  char temporaryPath[CACHE_TEMP_PATH_CAPACITY]{};
  char entryName[DIRECTORY_ENTRY_CAPACITY]{};
  DirectoryFrame frames[MAX_CACHE_DEPTH]{};
  std::string oldPath;
  std::string newPath;
  std::string oldCachePath;
  std::string newCachePath;
  BookFormat format = BookFormat::Unknown;
};

static_assert(sizeof(DirectoryFrame) <= 24, "directory cursor frame grew");
static_assert(sizeof(BookMoveWorkspace) <= 12 * 1024, "cold book-move workspace exceeded its RAM budget");

bool sameView(const StringView view, const std::string& value) {
  return view.length == value.size() && view.data != nullptr && std::memcmp(view.data, value.data(), view.length) == 0;
}

bool sameView(const StringView view, const std::string_view value) {
  return view.length == value.size() && view.data != nullptr &&
         std::memcmp(view.data, value.data(), view.length) == 0;
}

Status journalOpen(void* context, const char* path, const OpenMode mode, Handle* handle) {
  auto& workspace = *static_cast<JournalIoWorkspace*>(context);
  if (path == nullptr || handle == nullptr || handle->valid() || workspace.file.isOpen()) {
    return Status::InvalidArgument;
  }
  const bool opened = mode == OpenMode::Read ? Storage.openFileForRead("BookMove", path, workspace.file)
                                             : Storage.openFileForWrite("BookMove", path, workspace.file);
  if (!opened) {
    return mode == OpenMode::Read && !Storage.exists(path) ? Status::NotFound : Status::IoFailure;
  }
  handle->value = 1;
  return Status::Ok;
}

Status journalSize(void* context, const Handle handle, size_t* size) {
  auto& workspace = *static_cast<JournalIoWorkspace*>(context);
  if (!handle.valid() || !workspace.file.isOpen() || size == nullptr) return Status::InvalidArgument;
  const uint64_t value = workspace.file.fileSize64();
  if (value > std::numeric_limits<size_t>::max()) return Status::LimitExceeded;
  *size = static_cast<size_t>(value);
  return Status::Ok;
}

Status journalRead(void* context, const Handle handle, const size_t offset, uint8_t* destination, const size_t length,
                   size_t* actual) {
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

Status journalWrite(void* context, const Handle handle, const uint8_t* source, const size_t length, size_t* actual) {
  auto& workspace = *static_cast<JournalIoWorkspace*>(context);
  if (!handle.valid() || !workspace.file.isOpen() || source == nullptr || actual == nullptr) {
    return Status::InvalidArgument;
  }
  *actual = workspace.file.write(source, length);
  return *actual == length ? Status::Ok : Status::IoFailure;
}

Status journalFlush(void* context, const Handle handle) {
  auto& workspace = *static_cast<JournalIoWorkspace*>(context);
  if (!handle.valid() || !workspace.file.isOpen()) return Status::InvalidArgument;
  workspace.file.flush();
  return Status::Ok;
}

Status journalSync(void* context, const Handle handle) {
  auto& workspace = *static_cast<JournalIoWorkspace*>(context);
  return handle.valid() && workspace.file.isOpen() && workspace.file.sync() ? Status::Ok : Status::IoFailure;
}

Status journalClose(void* context, Handle* handle) {
  auto& workspace = *static_cast<JournalIoWorkspace*>(context);
  if (handle == nullptr || !handle->valid()) return Status::InvalidArgument;
  const bool closed = workspace.file.close();
  handle->invalidate();
  return closed ? Status::Ok : Status::IoFailure;
}

Status journalRemove(void*, const char* path) {
  if (path == nullptr) return Status::InvalidArgument;
  if (!Storage.exists(path)) return Status::NotFound;
  return Storage.remove(path) ? Status::Ok : Status::IoFailure;
}

Io journalIo(JournalIoWorkspace& workspace) {
  return {&workspace,    &journalOpen, &journalSize,  &journalRead,  &journalWrite,
          &journalFlush, &journalSync, &journalClose, &journalRemove};
}

BookFormat bookFormatForPath(const std::string& path) {
  if (FsHelpers::hasPdfExtension(path)) return BookFormat::Pdf;
  return BookFormat::Unknown;
}

uint64_t pathHash(const BookFormat format, const std::string& path) {
  if (format == BookFormat::Pdf) {
    return pdfPathHash64(path.c_str(), path.size());
  }
  return 0;
}

bool formatCachePath(const BookFormat format, const uint64_t hash, char* destination, const size_t capacity) {
  if (format != BookFormat::Pdf || destination == nullptr || capacity == 0) {
    return false;
  }
  const int written =
      std::snprintf(destination, capacity, "%s/pdf_%llu", CROSSPOINT_DIR, static_cast<unsigned long long>(hash));
  return written > 0 && static_cast<size_t>(written) < capacity;
}

bool appendPath(char* path, const size_t capacity, const char* name, size_t* length) {
  if (path == nullptr || name == nullptr || length == nullptr) return false;
  const size_t nameLength = std::strlen(name);
  const bool needsSlash = *length == 0 || path[*length - 1U] != '/';
  const size_t required = *length + static_cast<size_t>(needsSlash) + nameLength;
  if (required >= capacity) return false;
  if (needsSlash) path[(*length)++] = '/';
  std::memcpy(path + *length, name, nameLength + 1U);
  *length += nameLength;
  return true;
}

DirectoryNextResult nextDirectoryEntry(HalFile& directory, HalFile& entry) {
#if defined(SIMULATOR)
  // The pinned native simulator HAL predates the reusable, result-bearing
  // directory API. Preserve explicit EOF/error handling at this narrow seam.
  if (!entry.close()) return DirectoryNextResult::Error;
  errno = 0;
  entry = directory.openNextFile();
  if (entry) return DirectoryNextResult::Entry;
  return errno == 0 ? DirectoryNextResult::End : DirectoryNextResult::Error;
#else
  const HalDirectoryNextStatus status = directory.openNextFile(entry);
  if (status == HalDirectoryNextStatus::Entry) return DirectoryNextResult::Entry;
  if (status == HalDirectoryNextStatus::End) return DirectoryNextResult::End;
  return DirectoryNextResult::Error;
#endif
}

uint32_t crc32Update(uint32_t crc, const uint8_t* bytes, const size_t length) {
  for (size_t index = 0; index < length; ++index) {
    crc ^= bytes[index];
    for (uint8_t bit = 0; bit < 8; ++bit) {
      const uint32_t mask = 0U - (crc & 1U);
      crc = (crc >> 1U) ^ (0xedb88320U & mask);
    }
  }
  return crc;
}

bool fileDigest(BookMoveWorkspace& workspace, const char* path, uint64_t* size, uint32_t* crc) {
  if (path == nullptr || size == nullptr || crc == nullptr ||
      !Storage.openFileForRead("BookMove", path, workspace.entry)) {
    return false;
  }
  *size = 0;
  uint32_t running = UINT32_MAX;
  bool ok = true;
  while (true) {
    const int count = workspace.entry.read(workspace.copyBuffer, sizeof(workspace.copyBuffer));
    if (count < 0) {
      ok = false;
      break;
    }
    if (count == 0) break;
    running = crc32Update(running, workspace.copyBuffer, static_cast<size_t>(count));
    *size += static_cast<size_t>(count);
  }
  const bool closed = workspace.entry.close();
  *crc = ~running;
  return ok && closed;
}

bool filesEqual(BookMoveWorkspace& workspace, const char* source, const char* destination) {
  uint64_t sourceSize = 0;
  uint64_t destinationSize = 0;
  uint32_t sourceCrc = 0;
  uint32_t destinationCrc = 0;
  return fileDigest(workspace, source, &sourceSize, &sourceCrc) &&
         fileDigest(workspace, destination, &destinationSize, &destinationCrc) && sourceSize == destinationSize &&
         sourceCrc == destinationCrc;
}

bool copyFileOneHandle(BookMoveWorkspace& workspace, const char* source, const char* destination) {
  if (Storage.exists(destination) && filesEqual(workspace, source, destination)) {
    return true;
  }
  const int temporaryLength =
      std::snprintf(workspace.temporaryPath, sizeof(workspace.temporaryPath), "%s.bmcopy", destination);
  if (temporaryLength <= 0 || static_cast<size_t>(temporaryLength) >= sizeof(workspace.temporaryPath)) {
    return false;
  }
  if (Storage.exists(workspace.temporaryPath) && !Storage.remove(workspace.temporaryPath)) {
    return false;
  }
  workspace.entry = Storage.open(workspace.temporaryPath, O_WRONLY | O_CREAT | O_TRUNC);
  if (!workspace.entry || !workspace.entry.close()) {
    workspace.entry.close();
    return false;
  }

  uint64_t sourceSize = 0;
  if (!Storage.openFileForRead("BookMove", source, workspace.entry)) return false;
  sourceSize = workspace.entry.fileSize64();
  if (!workspace.entry.close()) return false;

  uint64_t offset = 0;
  while (offset < sourceSize) {
    if (!Storage.openFileForRead("BookMove", source, workspace.entry) || !workspace.entry.seek64(offset)) {
      workspace.entry.close();
      return false;
    }
    const size_t wanted = static_cast<size_t>(std::min<uint64_t>(sizeof(workspace.copyBuffer), sourceSize - offset));
    const int count = workspace.entry.read(workspace.copyBuffer, wanted);
    const bool sourceClosed = workspace.entry.close();
    if (count <= 0 || !sourceClosed) return false;

    workspace.entry = Storage.open(workspace.temporaryPath, O_WRONLY | O_CREAT);
    if (!workspace.entry || !workspace.entry.seek64(offset) ||
        workspace.entry.write(workspace.copyBuffer, static_cast<size_t>(count)) != static_cast<size_t>(count)) {
      workspace.entry.close();
      return false;
    }
    if (!workspace.entry.close()) return false;
    offset += static_cast<size_t>(count);
  }

  workspace.entry = Storage.open(workspace.temporaryPath, O_WRONLY);
  if (!workspace.entry || !workspace.entry.sync() || !workspace.entry.close() ||
      !filesEqual(workspace, source, workspace.temporaryPath)) {
    workspace.entry.close();
    return false;
  }
  if (Storage.exists(destination) && !Storage.remove(destination)) return false;
  if (!Storage.rename(workspace.temporaryPath, destination)) return false;
  return filesEqual(workspace, source, destination);
}

bool walkCacheTree(BookMoveWorkspace& workspace, const char* sourceRoot, const char* destinationRoot, const bool copy) {
  if (!Storage.exists(sourceRoot)) return true;
  workspace.directory = Storage.open(sourceRoot);
  if (!workspace.directory || !workspace.directory.isDirectory() || !workspace.directory.close()) {
    workspace.directory.close();
    return false;
  }
  if (copy && !Storage.exists(destinationRoot) && !Storage.mkdir(destinationRoot)) return false;
  if (!copy && !Storage.exists(destinationRoot)) return false;

  const size_t sourceRootLength = std::strlen(sourceRoot);
  const size_t destinationRootLength = std::strlen(destinationRoot);
  if (sourceRootLength >= sizeof(workspace.sourceCachePath) ||
      destinationRootLength >= sizeof(workspace.destinationCachePath)) {
    return false;
  }
  std::memcpy(workspace.sourceCachePath, sourceRoot, sourceRootLength + 1U);
  std::memcpy(workspace.destinationCachePath, destinationRoot, destinationRootLength + 1U);
  uint8_t depth = 0;
  workspace.frames[0] = {0, sourceRootLength, destinationRootLength};

  while (true) {
    DirectoryFrame& frame = workspace.frames[depth];
    workspace.directory = Storage.open(workspace.sourceCachePath);
    if (!workspace.directory || !workspace.directory.isDirectory() ||
        (frame.resumeOffset != 0 && !workspace.directory.seek64(frame.resumeOffset))) {
      workspace.directory.close();
      return false;
    }

    const DirectoryNextResult next = nextDirectoryEntry(workspace.directory, workspace.entry);
    if (next == DirectoryNextResult::End) {
      workspace.entry.close();
      if (!workspace.directory.close()) return false;
      if (depth == 0) return true;
      --depth;
      workspace.sourceCachePath[workspace.frames[depth].sourceLength] = '\0';
      workspace.destinationCachePath[workspace.frames[depth].destinationLength] = '\0';
      continue;
    }
    if (next != DirectoryNextResult::Entry) {
      workspace.entry.close();
      workspace.directory.close();
      return false;
    }

    const size_t nameLength = workspace.entry.getName(workspace.entryName, sizeof(workspace.entryName));
    const bool directory = workspace.entry.isDirectory();
    frame.resumeOffset = workspace.directory.position();
    const bool entriesClosed = workspace.entry.close() && workspace.directory.close();
    if (!entriesClosed || nameLength == 0 || nameLength >= sizeof(workspace.entryName) ||
        std::strcmp(workspace.entryName, ".") == 0 || std::strcmp(workspace.entryName, "..") == 0) {
      return false;
    }

    size_t sourceLength = frame.sourceLength;
    size_t destinationLength = frame.destinationLength;
    if (!appendPath(workspace.sourceCachePath, sizeof(workspace.sourceCachePath), workspace.entryName, &sourceLength) ||
        !appendPath(workspace.destinationCachePath, sizeof(workspace.destinationCachePath), workspace.entryName,
                    &destinationLength)) {
      return false;
    }

    if (directory) {
      if (depth + 1U >= MAX_CACHE_DEPTH ||
          (copy && !Storage.exists(workspace.destinationCachePath) && !Storage.mkdir(workspace.destinationCachePath))) {
        return false;
      }
      ++depth;
      workspace.frames[depth] = {0, sourceLength, destinationLength};
      continue;
    }

    const bool fileOk = copy ? copyFileOneHandle(workspace, workspace.sourceCachePath, workspace.destinationCachePath)
                             : Storage.exists(workspace.destinationCachePath) &&
                                   filesEqual(workspace, workspace.sourceCachePath, workspace.destinationCachePath);
    workspace.sourceCachePath[frame.sourceLength] = '\0';
    workspace.destinationCachePath[frame.destinationLength] = '\0';
    if (!fileOk) return false;
  }
}

Status statusFor(const bool ok) { return ok ? Status::Ok : Status::OperationFailed; }

SourceObservation locateSource(void* context, const Record& record) {
  auto& workspace = *static_cast<BookMoveWorkspace*>(context);
  if (!sameView(record.oldPath, workspace.oldPath) || !sameView(record.newPath, workspace.newPath)) {
    return {Status::Corrupt, SourceLocation::Missing};
  }
  const bool oldExists = Storage.exists(workspace.oldPath.c_str());
  const bool newExists = Storage.exists(workspace.newPath.c_str());
  if (oldExists && newExists) return {Status::Ok, SourceLocation::Both};
  if (oldExists) return {Status::Ok, SourceLocation::OldOnly};
  if (newExists) return {Status::Ok, SourceLocation::NewOnly};
  return {Status::Ok, SourceLocation::Missing};
}

Status renameSource(void* context, const Record&) {
  auto& workspace = *static_cast<BookMoveWorkspace*>(context);
  return statusFor(Storage.rename(workspace.oldPath.c_str(), workspace.newPath.c_str()));
}

Status copyCache(void* context, const Record&) {
  auto& workspace = *static_cast<BookMoveWorkspace*>(context);
  return statusFor(walkCacheTree(workspace, workspace.oldCachePath.c_str(), workspace.newCachePath.c_str(), true));
}

Status verifyCache(void* context, const Record&) {
  auto& workspace = *static_cast<BookMoveWorkspace*>(context);
  return statusFor(walkCacheTree(workspace, workspace.oldCachePath.c_str(), workspace.newCachePath.c_str(), false));
}

Status copyBookmarks(void* context, const Record&) {
  auto& workspace = *static_cast<BookMoveWorkspace*>(context);
  return statusFor(BookmarkStore::copyForFilePath(workspace.oldPath, workspace.newPath, "pdf"));
}

Status verifyBookmarks(void* context, const Record&) {
  auto& workspace = *static_cast<BookMoveWorkspace*>(context);
  return statusFor(BookmarkStore::verifyCopyForFilePath(workspace.oldPath, workspace.newPath, "pdf"));
}

Status copyClippings(void* context, const Record&) {
  auto& workspace = *static_cast<BookMoveWorkspace*>(context);
  return statusFor(ClippingStore::copyForFilePath(workspace.oldPath, workspace.newPath, "pdf"));
}

Status verifyClippings(void* context, const Record&) {
  auto& workspace = *static_cast<BookMoveWorkspace*>(context);
  return statusFor(ClippingStore::verifyCopyForFilePath(workspace.oldPath, workspace.newPath, "pdf"));
}

Status verifyState(void* context, const Record& record) {
  Status status = verifyCache(context, record);
  if (status == Status::Ok) status = verifyBookmarks(context, record);
  if (status == Status::Ok) status = verifyClippings(context, record);
  return status;
}

Status activateRecent(void* context, const Record& record) {
  auto& workspace = *static_cast<BookMoveWorkspace*>(context);
  const bool keepInRecents = record.recentsPolicy == RecentsPolicy::Keep;
  return statusFor(RECENT_BOOKS.activatePathMigration(workspace.oldPath, workspace.newPath, workspace.oldCachePath,
                                                      workspace.newCachePath, keepInRecents));
}

Status activateOpenPath(void* context, const Record&) {
  auto& workspace = *static_cast<BookMoveWorkspace*>(context);
  return statusFor(APP_STATE.activateOpenPathMigration(workspace.oldPath, workspace.newPath));
}

Status verifyActivation(void* context, const Record& record) {
  auto& workspace = *static_cast<BookMoveWorkspace*>(context);
  const bool keepInRecents = record.recentsPolicy == RecentsPolicy::Keep;
  return statusFor(RECENT_BOOKS.verifyPersistedPathMigration(workspace.oldPath, workspace.newPath, keepInRecents) &&
                   APP_STATE.verifyPersistedOpenPathMigration(workspace.oldPath, workspace.newPath));
}

Status removeOldState(void* context, const OldState& oldState) {
  auto& workspace = *static_cast<BookMoveWorkspace*>(context);
  if (!sameView(oldState.path, workspace.oldPath) || oldState.hash == 0 || oldState.format != workspace.format) {
    return Status::Corrupt;
  }
  if (Storage.exists(workspace.oldCachePath.c_str()) && !Storage.removeDir(workspace.oldCachePath.c_str())) {
    return Status::OperationFailed;
  }
  if (!BookmarkStore::deleteForFilePath(workspace.oldPath, "pdf") ||
      !ClippingStore::deleteForFilePath(workspace.oldPath, "pdf")) {
    return Status::OperationFailed;
  }
  // Source deletion is deliberately absent: once newPath exists, recovery may
  // only clean old path-keyed state.
  return Status::Ok;
}

MigrationOperations migrationOperations(BookMoveWorkspace& workspace) {
  return {&workspace,      &locateSource,     &renameSource,     &copyCache,       &verifyCache,
          &copyBookmarks,  &verifyBookmarks,  &copyClippings,    &verifyClippings, &verifyState,
          &activateRecent, &activateOpenPath, &verifyActivation, &removeOldState};
}

struct BookMoveSession {
  BookMoveWorkspace workspace;
  BookStateMigration::Journal journal;
  MigrationOperations operations;
  Request request{};
  Selection selection{};

  BookMoveSession()
      : journal(journalIo(workspace.journal), {workspace.journal.scratch, sizeof(workspace.journal.scratch)}),
        operations(migrationOperations(workspace)) {}
};

struct JournalReadSession {
  JournalIoWorkspace workspace;
  BookStateMigration::Journal journal;
  Selection selection{};

  JournalReadSession() : journal(journalIo(workspace), {workspace.scratch, sizeof(workspace.scratch)}) {}
};

static_assert(sizeof(BookMoveSession) <= 13 * 1024, "book-move session exceeded its cold-path RAM budget");
static_assert(sizeof(JournalReadSession) <= 3 * 1024, "journal-read session exceeded its RAM budget");

bool initializeWorkspace(BookMoveWorkspace& workspace, const Record& record) {
  if (record.format != BookFormat::Pdf || record.oldPath.data == nullptr || record.newPath.data == nullptr ||
      record.oldPath.length > BookStateMigration::kMaxPathBytes ||
      record.newPath.length > BookStateMigration::kMaxPathBytes) {
    return false;
  }
  workspace.oldPath.assign(record.oldPath.data, record.oldPath.length);
  workspace.newPath.assign(record.newPath.data, record.newPath.length);
  workspace.format = record.format;
  if (!formatCachePath(record.format, record.oldHash, workspace.sourceCachePath, sizeof(workspace.sourceCachePath)) ||
      !formatCachePath(record.format, record.newHash, workspace.destinationCachePath,
                       sizeof(workspace.destinationCachePath))) {
    return false;
  }
  workspace.oldCachePath = workspace.sourceCachePath;
  workspace.newCachePath = workspace.destinationCachePath;
  return true;
}

BookMoveUtils::MoveResult resultFor(const BookStateMigration::RunResult& result) {
  if (result.status == Status::Conflict) return BookMoveUtils::MoveResult::Conflict;
  if (result.status != Status::Ok) return BookMoveUtils::MoveResult::Pending;
  if (result.disposition == RunDisposition::Complete) return BookMoveUtils::MoveResult::Complete;
  if (result.disposition == RunDisposition::Abandoned) return BookMoveUtils::MoveResult::Abandoned;
  if (result.disposition == RunDisposition::Idle) return BookMoveUtils::MoveResult::NoPendingMove;
  return BookMoveUtils::MoveResult::Pending;
}

[[gnu::noinline]] BookMoveUtils::MoveResult recoverWithSession(BookMoveSession& session) {
  Selection& selection = session.selection;
  selection = {};
  const Status loadStatus = session.journal.load(&selection);
  if (loadStatus != Status::Ok) {
    return loadStatus == Status::Conflict ? BookMoveUtils::MoveResult::Conflict : BookMoveUtils::MoveResult::Pending;
  }
  if (!selection.selected) return BookMoveUtils::MoveResult::NoPendingMove;
  if (!initializeWorkspace(session.workspace, selection.record)) return BookMoveUtils::MoveResult::Invalid;

  Coordinator coordinator(session.journal, session.operations);
  return resultFor(coordinator.recover());
}

}  // namespace

namespace BookMoveUtils {

std::string buildReadFolderDestination(const std::string& srcPath) {
  const size_t lastSlash = srcPath.rfind('/');
  const std::string filename = (lastSlash != std::string::npos) ? srcPath.substr(lastSlash + 1) : srcPath;

  Storage.mkdir(READ_FOLDER);
  std::string dstPath = std::string(READ_FOLDER) + "/" + filename;
  if (!Storage.exists(dstPath.c_str())) {
    return dstPath;
  }

  const size_t dotPos = filename.rfind('.');
  const std::string base = (dotPos != std::string::npos) ? filename.substr(0, dotPos) : filename;
  const std::string ext = (dotPos != std::string::npos) ? filename.substr(dotPos) : "";
  int suffix = 2;
  do {
    dstPath = std::string(READ_FOLDER) + "/" + base + " (" + std::to_string(suffix) + ")" + ext;
    suffix++;
  } while (Storage.exists(dstPath.c_str()) && suffix < 100);
  return dstPath;
}

MoveResult moveBook(const std::string& oldPath, const std::string& newPath, const bool keepInRecents) {
  const BookFormat oldFormat = bookFormatForPath(oldPath);
  const BookFormat newFormat = bookFormatForPath(newPath);
  if (oldFormat == BookFormat::Unknown || newFormat != oldFormat) return MoveResult::Unsupported;
  if (oldPath.empty() || newPath.empty() || oldPath == newPath || oldPath.size() > BookStateMigration::kMaxPathBytes ||
      newPath.size() > BookStateMigration::kMaxPathBytes) {
    return MoveResult::Invalid;
  }

  const uint64_t oldHash = pathHash(oldFormat, oldPath);
  const uint64_t newHash = pathHash(newFormat, newPath);
  const uint32_t oldStateHash = uzlib_crc32(oldPath.data(), static_cast<unsigned int>(oldPath.size()), 0);
  const uint32_t newStateHash = uzlib_crc32(newPath.data(), static_cast<unsigned int>(newPath.size()), 0);
  if (oldHash == newHash || oldStateHash == newStateHash) {
    LOG_ERR("BookMove", "Refusing move across a path-key collision");
    return MoveResult::Conflict;
  }

  journalPresence.store(JournalPresence::MoveStarting, std::memory_order_release);
  const BookMutationFence deleteFence = PdfDeleteUtils::mutationFenceForPath(oldPath);
  if (deleteFence != BookMutationFence::Clear) {
    // Both journals eventually replace the one global recent.json. Even an
    // unrelated pending delete must finish first or two durable snapshots can
    // overwrite each other.
    journalPresence.store(JournalPresence::LookupRequired, std::memory_order_release);
    return deleteFence == BookMutationFence::Indeterminate ? MoveResult::Pending : MoveResult::Conflict;
  }
  // About 8 KiB is needed only for this cold operation: journal scratch,
  // fixed path cursors, and one 2 KiB streaming chunk. Heap allocation keeps
  // activity/task stacks bounded and is checked before any durable mutation.
  auto session = makeUniqueNoThrow<BookMoveSession>();
  if (!session) {
    LOG_ERR("BookMove", "Out of memory allocating book-move workspace");
    journalPresence.store(JournalPresence::LookupRequired, std::memory_order_release);
    return MoveResult::Pending;
  }
  Storage.mkdir(CROSSPOINT_DIR);

  const MoveResult pending = recoverWithSession(*session);
  if (pending == MoveResult::Pending || pending == MoveResult::Conflict || pending == MoveResult::Invalid) {
    journalPresence.store(JournalPresence::LookupRequired, std::memory_order_release);
    return pending;
  }
  const bool oldExists = Storage.exists(oldPath.c_str());
  const bool newExists = Storage.exists(newPath.c_str());
  if (!oldExists && newExists) {
    journalPresence.store(JournalPresence::KnownAbsent, std::memory_order_release);
    return MoveResult::Complete;
  }
  if (oldExists && newExists) {
    journalPresence.store(JournalPresence::KnownAbsent, std::memory_order_release);
    return MoveResult::Conflict;
  }
  if (!oldExists) {
    journalPresence.store(JournalPresence::KnownAbsent, std::memory_order_release);
    return MoveResult::Invalid;
  }

  BookMoveWorkspace& workspace = session->workspace;
  workspace.oldPath = oldPath;
  workspace.newPath = newPath;
  workspace.format = oldFormat;
  if (!formatCachePath(oldFormat, oldHash, workspace.sourceCachePath, sizeof(workspace.sourceCachePath)) ||
      !formatCachePath(newFormat, newHash, workspace.destinationCachePath, sizeof(workspace.destinationCachePath))) {
    journalPresence.store(JournalPresence::KnownAbsent, std::memory_order_release);
    return MoveResult::Invalid;
  }
  workspace.oldCachePath = workspace.sourceCachePath;
  workspace.newCachePath = workspace.destinationCachePath;

  Coordinator coordinator(session->journal, session->operations);
  session->request = {{workspace.oldPath.data(), workspace.oldPath.size()},
                      {workspace.newPath.data(), workspace.newPath.size()},
                      oldFormat,
                      oldHash,
                      newHash,
                      keepInRecents ? RecentsPolicy::Keep : RecentsPolicy::Remove};
  const Status beginStatus = coordinator.begin(session->request);
  if (beginStatus != Status::Ok) {
    journalPresence.store(JournalPresence::LookupRequired, std::memory_order_release);
    return beginStatus == Status::Conflict ? MoveResult::Conflict : MoveResult::Pending;
  }
  const MoveResult result = resultFor(coordinator.recover());
  journalPresence.store(result == MoveResult::Complete || result == MoveResult::Abandoned
                            ? JournalPresence::KnownAbsent
                            : JournalPresence::LookupRequired,
                        std::memory_order_release);
  return result;
}

MoveResult recoverPendingBookMove() {
  const JournalPresence presence = journalPresence.load(std::memory_order_acquire);
  if (presence == JournalPresence::KnownAbsent) return MoveResult::NoPendingMove;
  if (presence == JournalPresence::MoveStarting) return MoveResult::Pending;

  // The bounded reader holds only the 2,090-byte journal scratch and one
  // HalFile wrapper. Keep it off the task stack, and use it to avoid the full
  // copy/migration workspace on the normal no-journal boot path.
  {
    auto reader = makeUniqueNoThrow<JournalReadSession>();
    if (!reader) {
      LOG_ERR("BookMove", "Out of memory allocating book-move journal reader");
      return MoveResult::Pending;
    }
    const Status status = reader->journal.load(&reader->selection);
    if (status != Status::Ok) {
      return status == Status::Conflict ? MoveResult::Conflict : MoveResult::Pending;
    }
    if (!reader->selection.selected) {
      JournalPresence expected = JournalPresence::LookupRequired;
      if (!journalPresence.compare_exchange_strong(expected, JournalPresence::KnownAbsent,
                                                   std::memory_order_acq_rel,
                                                   std::memory_order_acquire) &&
          expected == JournalPresence::MoveStarting) {
        return MoveResult::Pending;
      }
      return MoveResult::NoPendingMove;
    }
  }

  auto session = makeUniqueNoThrow<BookMoveSession>();
  if (!session) {
    LOG_ERR("BookMove", "Out of memory allocating book-move recovery workspace");
    return MoveResult::Pending;
  }
  const MoveResult result = recoverWithSession(*session);
  journalPresence.store(result == MoveResult::Complete || result == MoveResult::NoPendingMove ||
                                result == MoveResult::Abandoned
                            ? JournalPresence::KnownAbsent
                            : JournalPresence::LookupRequired,
                        std::memory_order_release);
  return result;
}

BookMutationFence mutationFenceForPath(const std::string& bookPath) {
  return mutationFenceForPathNoPathAlloc(bookPath);
}

BookMutationFence mutationFenceForPathNoPathAlloc(
    const std::string_view bookPath) {
  const JournalPresence presence = journalPresence.load(std::memory_order_acquire);
  if (presence == JournalPresence::KnownAbsent) return BookMutationFence::Clear;
  if (presence == JournalPresence::MoveStarting) return BookMutationFence::Indeterminate;

  auto session = makeUniqueNoThrow<JournalReadSession>();
  if (!session) {
    LOG_ERR("BookMove", "Out of memory reading book-move mutation fence");
    return BookMutationFence::Indeterminate;
  }
  const Status status = session->journal.load(&session->selection);
  if (status != Status::Ok) return BookMutationFence::Indeterminate;
  if (!session->selection.selected) {
    JournalPresence expected = JournalPresence::LookupRequired;
    if (!journalPresence.compare_exchange_strong(expected, JournalPresence::KnownAbsent, std::memory_order_acq_rel,
                                                 std::memory_order_acquire) &&
        expected == JournalPresence::MoveStarting) {
      return BookMutationFence::Indeterminate;
    }
    return BookMutationFence::Clear;
  }

  const bool matchesOld = sameView(session->selection.record.oldPath, bookPath);
  const bool matchesNew = sameView(session->selection.record.newPath, bookPath);
  return matchesOld || matchesNew ? BookMutationFence::MatchingPending : BookMutationFence::UnrelatedPending;
}

bool migrationCacheHash(const std::string& bookPath, const uint64_t normalHash, uint64_t* const resolvedHash,
                        bool* const readOnlyFallback) {
  if (resolvedHash == nullptr) return false;
  *resolvedHash = normalHash;
  if (readOnlyFallback != nullptr) *readOnlyFallback = true;

  const JournalPresence presence = journalPresence.load(std::memory_order_acquire);
  if (presence == JournalPresence::KnownAbsent) {
    if (readOnlyFallback != nullptr) *readOnlyFallback = false;
    return true;
  }
  if (presence == JournalPresence::MoveStarting) return false;

  // Cache-key lookup needs only the 2,090-byte journal codec scratch and one
  // HalFile wrapper; it does not allocate the 8 KiB copy workspace.
  auto session = makeUniqueNoThrow<JournalReadSession>();
  if (!session) {
    LOG_ERR("BookMove", "Out of memory reading book-move cache fallback");
    return false;
  }
  const Status status = session->journal.load(&session->selection);
  if (status != Status::Ok) return false;
  if (!session->selection.selected) {
    JournalPresence expected = JournalPresence::LookupRequired;
    if (!journalPresence.compare_exchange_strong(expected, JournalPresence::KnownAbsent, std::memory_order_acq_rel,
                                                 std::memory_order_acquire) &&
        expected == JournalPresence::MoveStarting) {
      return false;
    }
    if (readOnlyFallback != nullptr) *readOnlyFallback = false;
    return true;
  }

  const bool matchesOld = session->selection.record.oldPath.length == bookPath.size() &&
                          std::memcmp(session->selection.record.oldPath.data, bookPath.data(), bookPath.size()) == 0;
  const bool matchesNew = session->selection.record.newPath.length == bookPath.size() &&
                          std::memcmp(session->selection.record.newPath.data, bookPath.data(), bookPath.size()) == 0;
  if (!matchesOld && !matchesNew) {
    if (readOnlyFallback != nullptr) *readOnlyFallback = false;
    return true;
  }
  const bool activated =
      session->selection.record.phase == Phase::Activated || session->selection.record.phase == Phase::OldStateRemoved;
  *resolvedHash = activated ? session->selection.record.newHash : session->selection.record.oldHash;
  if (readOnlyFallback != nullptr) *readOnlyFallback = !activated;
  return true;
}

bool migrateMovedEpubState(const std::string& oldPath, const std::string& newPath, const std::string& oldCachePath,
                           const std::string& title, const std::string& author, const bool keepInRecents) {
  bool ok = true;

  const std::string newCachePath = Epub::cachePathForFilePath(newPath, "/.crosspoint");
  if (!oldCachePath.empty() && Storage.exists(oldCachePath.c_str())) {
    if (!Storage.rename(oldCachePath.c_str(), newCachePath.c_str())) {
      LOG_ERR("BookMove", "Failed to rename cache dir %s -> %s (non-fatal)", oldCachePath.c_str(),
              newCachePath.c_str());
      ok = false;
    }
  }

  if (!BookmarkStore::migrateForFilePath(oldPath, newPath, title, author, "epub")) {
    LOG_ERR("BookMove", "Failed to migrate bookmarks for moved book %s -> %s", oldPath.c_str(), newPath.c_str());
    ok = false;
  }

  if (!ClippingStore::migrateForFilePath(oldPath, newPath, title, author, "epub")) {
    LOG_ERR("BookMove", "Failed to migrate clippings for moved book %s -> %s", oldPath.c_str(), newPath.c_str());
    ok = false;
  }

  if (keepInRecents) {
    RECENT_BOOKS.updatePath(oldPath, newPath, oldCachePath, newCachePath);
  } else {
    RECENT_BOOKS.removeByPath(oldPath);
    RECENT_BOOKS.removeByPath(newPath);
  }

  if (APP_STATE.openBookPath() == oldPath) {
    APP_STATE.openBookPath() = newPath;
    APP_STATE.saveToFile();
  }

  return ok;
}

}  // namespace BookMoveUtils
