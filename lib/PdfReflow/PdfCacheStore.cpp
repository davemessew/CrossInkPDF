#include "PdfCacheStore.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <limits>
#include <new>

#include "PdfCheckedMath.h"

#if defined(__GNUC__) || defined(__clang__)
#define PDF_CACHE_STORE_NOINLINE __attribute__((noinline))
#elif defined(_MSC_VER)
#define PDF_CACHE_STORE_NOINLINE __declspec(noinline)
#else
#define PDF_CACHE_STORE_NOINLINE
#endif

namespace {

constexpr char kManifestNames[2][11] = {"manifest.a", "manifest.b"};
constexpr char kCheckpointNames[2][8] = {"build.a", "build.b"};

struct CacheHandleSource {
  const PdfCacheIo* io = nullptr;
  PdfCacheHandle handle{};
  uint64_t size = 0;

  static PdfStatus read(void* context, const uint64_t offset, uint8_t* destination, const size_t requested,
                        size_t* bytesRead) {
    auto& self = *static_cast<CacheHandleSource*>(context);
    return self.io->read(self.io->context, self.handle, offset, destination, requested, bytesRead);
  }

  PdfByteSource source() { return {this, size, read}; }
};

struct CacheHandleSink {
  const PdfCacheIo* io = nullptr;
  PdfCacheHandle handle{};

  static PdfStatus write(void* context, const uint8_t* source, const size_t requested, size_t* bytesWritten) {
    auto& self = *static_cast<CacheHandleSink*>(context);
    return self.io->write(self.io->context, self.handle, source, requested, bytesWritten);
  }

  PdfByteSink sink() { return {this, write}; }
};

PdfStatus closeHandle(const PdfCacheIo& io, PdfCacheHandle* const handle, const PdfStatus prior) {
  if (handle == nullptr || !handle->valid()) {
    return prior;
  }
  const PdfStatus closeStatus = io.close(io.context, handle);
  return prior ? closeStatus : prior;
}

bool isRecoverableSlotError(const PdfError error) {
  return error == PdfError::InvalidArgument || error == PdfError::UnexpectedEof || error == PdfError::Malformed ||
         error == PdfError::LimitExceeded;
}

bool manifestsEqual(const PdfCacheManifest& left, const PdfCacheManifest& right) {
  return left.formatVersion == right.formatVersion && left.capabilityVersion == right.capabilityVersion &&
         left.sequence == right.sequence && left.completed == right.completed &&
         left.warningFlags == right.warningFlags && pdfSourceIdentityEqual(left.source, right.source) &&
         left.generation == right.generation && left.totalWords == right.totalWords &&
         left.requiredFileCount == right.requiredFileCount && left.requiredFileBytes == right.requiredFileBytes &&
         left.requiredFileLedger == right.requiredFileLedger;
}

bool checkpointsEqual(const PdfBuildCheckpoint& left, const PdfBuildCheckpoint& right) {
  return left.sequence == right.sequence && pdfSourceIdentityEqual(left.source, right.source) &&
         left.generation == right.generation && left.phase == right.phase &&
         left.lastVerifiedPage == right.lastVerifiedPage && left.lastVerifiedObject == right.lastVerifiedObject &&
         left.emittedSections == right.emittedSections && left.emittedImages == right.emittedImages &&
         left.cumulativeWords == right.cumulativeWords && left.outputBytes == right.outputBytes &&
         left.warningFlags == right.warningFlags;
}

PDF_CACHE_STORE_NOINLINE PdfStatus loadManifestSlot(
    const PdfCacheIo& io, const char* const path,
    const PdfSourceIdentity& expectedSource,
    PdfCacheManifestSlotState* const state) {
  if (state == nullptr) {
    return PdfStatus::failure(PdfError::InvalidArgument);
  }
  PdfCacheHandle handle{};
  PdfStatus status =
      io.open(io.context, path, PdfCacheOpenMode::Read, &handle);
  if (!status) {
    return status;
  }
  PdfCacheFileMetadata metadata{};
  status = io.metadata(io.context, handle, &metadata);
  if (status && !metadata.directory && !metadata.symlinkLike) {
    CacheHandleSource source{&io, handle, metadata.size};
    status = pdfDecodeCacheManifest(source.source(), &state->manifest, {});
  } else if (status) {
    status = PdfStatus::failure(PdfError::Malformed);
  }
  status = closeHandle(io, &handle, status);
  if (!status || !state->manifest.completed) {
    return status;
  }
  state->valid = true;
  state->sourceMatches =
      pdfSourceIdentityEqual(state->manifest.source, expectedSource);
  return PdfStatus::success();
}

PDF_CACHE_STORE_NOINLINE PdfStatus loadCheckpointSlot(
    const PdfCacheIo& io, const char* const path,
    const PdfSourceIdentity& expectedSource,
    PdfBuildCheckpointSlotState* const state) {
  if (state == nullptr) {
    return PdfStatus::failure(PdfError::InvalidArgument);
  }
  PdfCacheHandle handle{};
  PdfStatus status =
      io.open(io.context, path, PdfCacheOpenMode::Read, &handle);
  if (!status) {
    return status;
  }
  PdfCacheFileMetadata metadata{};
  status = io.metadata(io.context, handle, &metadata);
  if (status && !metadata.directory && !metadata.symlinkLike) {
    CacheHandleSource source{&io, handle, metadata.size};
    status =
        pdfDecodeBuildCheckpoint(source.source(), &state->checkpoint);
  } else if (status) {
    status = PdfStatus::failure(PdfError::Malformed);
  }
  status = closeHandle(io, &handle, status);
  if (!status) {
    return status;
  }
  state->valid = true;
  state->sourceMatches =
      pdfSourceIdentityEqual(state->checkpoint.source, expectedSource);
  return PdfStatus::success();
}

PDF_CACHE_STORE_NOINLINE PdfStatus verifyCheckpointFile(
    const PdfCacheIo& io, const char* const path,
    const PdfBuildCheckpoint& checkpoint) {
  PdfCacheHandle handle{};
  PdfStatus status =
      io.open(io.context, path, PdfCacheOpenMode::Read, &handle);
  if (!status) {
    return status;
  }
  PdfCacheFileMetadata metadata{};
  status = io.metadata(io.context, handle, &metadata);
  PdfBuildCheckpoint verified{};
  if (status && !metadata.directory && !metadata.symlinkLike) {
    CacheHandleSource source{&io, handle, metadata.size};
    status = pdfDecodeBuildCheckpoint(source.source(), &verified);
    if (status && !checkpointsEqual(verified, checkpoint)) {
      status = PdfStatus::failure(PdfError::Malformed);
    }
  } else if (status) {
    status = PdfStatus::failure(PdfError::Malformed);
  }
  return closeHandle(io, &handle, status);
}

bool safeRoot(const char* const root) {
  if (root == nullptr || root[0] != '/' || root[1] == '\0' || std::strchr(root, '\\') != nullptr ||
      std::strchr(root, ':') != nullptr) {
    return false;
  }
  const size_t length = std::strlen(root);
  if (length >= PDF_CACHE_PATH_CAPACITY || root[length - 1] == '/') {
    return false;
  }
  size_t componentStart = 1;
  for (size_t index = 1; index <= length; ++index) {
    if (index != length && root[index] != '/') {
      continue;
    }
    const size_t componentLength = index - componentStart;
    if (componentLength == 0 || (componentLength == 1 && root[componentStart] == '.') ||
        (componentLength == 2 && root[componentStart] == '.' && root[componentStart + 1] == '.')) {
      return false;
    }
    componentStart = index + 1;
  }
  return true;
}

bool parseGenerationName(const PdfCacheDirEntry& entry, uint32_t* const generation) {
  constexpr char prefix[] = "gen_";
  if (generation == nullptr || !entry.directory || entry.symlinkLike || entry.nameLength <= sizeof(prefix) - 1 ||
      entry.nameLength >= sizeof(entry.name) || std::memcmp(entry.name, prefix, sizeof(prefix) - 1) != 0 ||
      entry.name[entry.nameLength] != '\0') {
    return false;
  }
  if (entry.nameLength > sizeof(prefix) && entry.name[sizeof(prefix) - 1] == '0') {
    return false;
  }
  uint32_t value = 0;
  for (size_t index = sizeof(prefix) - 1; index < entry.nameLength; ++index) {
    const unsigned char character = static_cast<unsigned char>(entry.name[index]);
    if (!std::isdigit(character)) {
      return false;
    }
    const uint32_t digit = character - static_cast<unsigned char>('0');
    if (value > (std::numeric_limits<uint32_t>::max() - digit) / 10U) {
      return false;
    }
    value = value * 10U + digit;
  }
  *generation = value;
  return true;
}

PDF_CACHE_STORE_NOINLINE PdfStatus removeGenerationPath(
    const PdfCacheIo& io, const char* const root,
    const uint32_t generation) {
  char path[PDF_CACHE_PATH_CAPACITY];
  const int length =
      std::snprintf(path, sizeof(path), "%s/gen_%lu", root,
                    static_cast<unsigned long>(generation));
  if (length < 0 || static_cast<size_t>(length) >= sizeof(path)) {
    return PdfStatus::failure(PdfError::LimitExceeded);
  }
  const PdfStatus status = io.remove(io.context, path, true);
  return !status && status.error == PdfError::InvalidOffset
             ? PdfStatus::success()
             : status;
}

PdfStatus collectGeneration(void* const context,
                            const PdfCacheDirEntry& entry) {
  auto& self = *static_cast<PdfCacheGenerationList*>(context);
  uint32_t generation = 0;
  if (!parseGenerationName(entry, &generation)) {
    return PdfStatus::success();
  }
  if (self.count == PDF_CACHE_CLEANUP_GENERATION_CAPACITY) {
    return PdfStatus::failure(PdfError::LimitExceeded);
  }
  self.generations[self.count++] = generation;
  return PdfStatus::success();
}

uint8_t slotIndex(const PdfCacheSlot slot) { return slot == PdfCacheSlot::A ? 0 : 1; }
PdfCacheSlot opposite(const PdfCacheSlot slot) { return slot == PdfCacheSlot::A ? PdfCacheSlot::B : PdfCacheSlot::A; }

template <typename T>
void resetInPlace(T& value) {
  value.~T();
  new (&value) T();
}

}  // namespace

PdfStatus pdfInitializeCacheBudget(const uint64_t sourceSize, const PdfCacheCapacity& capacity,
                                   const uint64_t requiredReserve, PdfCacheBudget* const budget) {
  if (budget == nullptr) {
    return PdfStatus::failure(PdfError::InvalidArgument);
  }
  uint64_t doubled = 0;
  uint64_t sourceLimit = PDF_CACHE_MAX_BYTES;
  if (pdfCheckedMultiply(sourceSize, 2, &doubled) &&
      pdfCheckedAdd(doubled, PDF_CACHE_SOURCE_OVERHEAD_BYTES, &sourceLimit)) {
    sourceLimit = std::clamp(sourceLimit, PDF_CACHE_MIN_BYTES, PDF_CACHE_MAX_BYTES);
  }
  budget->hardLimit = sourceLimit;
  budget->limit = sourceLimit;
  if (capacity.free.known) {
    uint64_t reserve = PDF_CACHE_MIN_FREE_RESERVE_BYTES;
    if (capacity.total.known) {
      const uint64_t fivePercent = capacity.total.value / 20U + (capacity.total.value % 20U != 0 ? 1U : 0U);
      reserve = std::max(reserve, fivePercent);
    }
    const uint64_t available = capacity.free.value > reserve ? capacity.free.value - reserve : 0;
    budget->limit = std::min(budget->limit, available);
  }
  budget->requiredReserve = std::min(requiredReserve, budget->limit);
  budget->requiredBytes = 0;
  budget->optionalBytes = 0;
  budget->optionalOmitted = false;
  return PdfStatus::success();
}

PdfStatus pdfReserveCacheBytes(PdfCacheBudget* const budget, const uint64_t bytes, const PdfCacheFileKind kind) {
  if (budget == nullptr) {
    return PdfStatus::failure(PdfError::InvalidArgument);
  }
  uint64_t used = 0;
  uint64_t projected = 0;
  if (!pdfCheckedAdd(budget->requiredBytes, budget->optionalBytes, &used) || !pdfCheckedAdd(used, bytes, &projected)) {
    return PdfStatus::failure(PdfError::LimitExceeded);
  }
  if (kind == PdfCacheFileKind::Optional) {
    const uint64_t remainingRequired =
        budget->requiredReserve > budget->requiredBytes ? budget->requiredReserve - budget->requiredBytes : 0;
    uint64_t withReserve = 0;
    if (!pdfCheckedAdd(projected, remainingRequired, &withReserve) || withReserve > budget->limit) {
      budget->optionalOmitted = true;
      return PdfStatus::failure(PdfError::Unsupported);
    }
    budget->optionalBytes += bytes;
    return PdfStatus::success();
  }
  if (projected > budget->limit) {
    return PdfStatus::failure(PdfError::InsufficientStorage);
  }
  budget->requiredBytes += bytes;
  return PdfStatus::success();
}

PdfStatus pdfOpenTrackedCacheWriter(const PdfCacheIo& io, const char* const fullPath, const char* const relativePath,
                                    const PdfCacheFileKind kind, const uint64_t byteLimit,
                                    PdfCacheTrackedWriter* const writer) {
  if (!io.valid() || fullPath == nullptr || relativePath == nullptr || writer == nullptr || byteLimit == 0) {
    return PdfStatus::failure(PdfError::InvalidArgument);
  }
  resetInPlace(*writer);
  const size_t fullLength = std::strlen(fullPath);
  const size_t relativeLength = std::strlen(relativePath);
  if (fullLength == 0 || fullLength >= sizeof(writer->fullPath) ||
      !pdfValidateCacheRelativePath(relativePath, relativeLength)) {
    return PdfStatus::failure(PdfError::InvalidArgument);
  }
  std::memcpy(writer->fullPath, fullPath, fullLength + 1);
  std::memcpy(writer->record.path, relativePath, relativeLength + 1);
  writer->record.pathLength = static_cast<uint8_t>(relativeLength);
  writer->record.crc32 = 0;
  writer->io = io;
  writer->kind = kind;
  writer->byteLimit = byteLimit;
  const PdfStatus status = io.open(io.context, fullPath, PdfCacheOpenMode::WriteTruncate, &writer->handle);
  if (!status) {
    resetInPlace(*writer);
    return status;
  }
  writer->open = true;
  return PdfStatus::success();
}

PdfStatus pdfWriteTrackedCacheFile(PdfCacheTrackedWriter* const writer, const uint8_t* const bytes,
                                   const size_t length) {
  if (writer == nullptr || !writer->open || writer->failed || (bytes == nullptr && length != 0)) {
    return PdfStatus::failure(PdfError::InvalidArgument);
  }
  uint64_t end = 0;
  if (!pdfCheckedAdd(writer->record.size, length, &end) || end > writer->byteLimit) {
    writer->failed = true;
    return PdfStatus::failure(PdfError::InsufficientStorage, writer->record.size);
  }
  if (length == 0) {
    return PdfStatus::success();
  }
  size_t bytesWritten = 0;
  const PdfStatus status = writer->io.write(writer->io.context, writer->handle, bytes, length, &bytesWritten);
  if (!status || bytesWritten != length) {
    writer->failed = true;
    return status ? PdfStatus::failure(PdfError::IoFailure, writer->record.size + bytesWritten) : status;
  }
  writer->record.crc32 = pdfCacheCrc32(bytes, length, writer->record.crc32);
  writer->record.size = end;
  return PdfStatus::success();
}

PdfStatus pdfCloseTrackedCacheFile(PdfCacheTrackedWriter* const writer, PdfRequiredFileRecord* const record) {
  if (writer == nullptr || record == nullptr || !writer->open || writer->failed) {
    return PdfStatus::failure(PdfError::InvalidArgument);
  }
  PdfStatus status = writer->io.flush(writer->io.context, writer->handle);
  if (status) {
    status = writer->io.sync(writer->io.context, writer->handle);
  }
  const PdfStatus closeStatus = writer->io.close(writer->io.context, &writer->handle);
  writer->open = false;
  if (status && !closeStatus) {
    status = closeStatus;
  }
  if (!status) {
    writer->failed = true;
    return status;
  }
  *record = writer->record;
  return PdfStatus::success();
}

void pdfAbortTrackedCacheFile(PdfCacheTrackedWriter* const writer) {
  if (writer == nullptr) {
    return;
  }
  if (writer->open && writer->handle.valid()) {
    (void)writer->io.close(writer->io.context, &writer->handle);
  }
  if (writer->fullPath[0] != '\0' && writer->io.remove != nullptr) {
    (void)writer->io.remove(writer->io.context, writer->fullPath, false);
  }
  writer->open = false;
  writer->failed = true;
}

PdfStatus PdfCacheStore::initialize(const PdfCacheIo& io, const char* const cacheRoot) {
  if (!io.valid() || !safeRoot(cacheRoot)) {
    return PdfStatus::failure(PdfError::InvalidArgument);
  }
  const size_t length = std::strlen(cacheRoot);
  io_ = io;
  std::memcpy(root_, cacheRoot, length + 1);
  return PdfStatus::success();
}

PdfStatus PdfCacheStore::formatPath(const char* const leaf, char* const destination, const size_t capacity) const {
  if (!io_.valid() || leaf == nullptr || destination == nullptr || capacity == 0 || std::strchr(leaf, '/') != nullptr ||
      std::strchr(leaf, '\\') != nullptr) {
    return PdfStatus::failure(PdfError::InvalidArgument);
  }
  const int written = std::snprintf(destination, capacity, "%s/%s", root_, leaf);
  return written >= 0 && static_cast<size_t>(written) < capacity ? PdfStatus::success()
                                                                 : PdfStatus::failure(PdfError::LimitExceeded);
}

PdfStatus PdfCacheStore::ensureGeneration(const uint32_t generation) {
  if (!io_.valid()) {
    return PdfStatus::failure(PdfError::InvalidArgument);
  }
  PdfStatus status = io_.mkdir(io_.context, root_);
  if (!status) {
    return status;
  }
  char leaf[20];
  const int length = std::snprintf(leaf, sizeof(leaf), "gen_%lu", static_cast<unsigned long>(generation));
  if (length < 0 || static_cast<size_t>(length) >= sizeof(leaf)) {
    return PdfStatus::failure(PdfError::LimitExceeded);
  }
  char path[PDF_CACHE_PATH_CAPACITY];
  status = formatPath(leaf, path, sizeof(path));
  return status ? io_.mkdir(io_.context, path) : status;
}

PdfStatus PdfCacheStore::loadManifestSlots(const PdfSourceIdentity& expectedSource,
                                           PdfCacheManifestSelection* const selection) const {
  if (!io_.valid() || selection == nullptr) {
    return PdfStatus::failure(PdfError::InvalidArgument);
  }
  resetInPlace(*selection);
  for (uint8_t index = 0; index < 2; ++index) {
    auto& state = selection->slots[index];
    char path[PDF_CACHE_PATH_CAPACITY];
    PdfStatus status = formatPath(kManifestNames[index], path, sizeof(path));
    if (!status) {
      return status;
    }
    status = loadManifestSlot(io_, path, expectedSource, &state);
    if (!status) {
      if (status.error == PdfError::InvalidOffset ||
          isRecoverableSlotError(status.error)) {
        resetInPlace(state);
        continue;
      }
      return status;
    }
    if (!state.valid) {
      resetInPlace(state);
      continue;
    }
    if (state.sourceMatches &&
        (!selection->selected ||
         pdfCacheSequenceNewer(state.manifest.sequence,
                               selection->manifest.sequence))) {
      selection->selected = true;
      selection->selectedSlot = index == 0 ? PdfCacheSlot::A : PdfCacheSlot::B;
      selection->manifest = state.manifest;
    }
  }
  return PdfStatus::success();
}

PdfStatus PdfCacheStore::commitManifest(const PdfCacheManifest& manifest, const PdfRequiredFileTableSource& files,
                                        const PdfCacheCommitEvidence& evidence, const PdfCacheManifestSelection& prior,
                                        PdfCacheManifestSelection* const committed) const {
  if (!io_.valid() || !manifest.completed || !evidence.allWritersClosed) {
    return PdfStatus::failure(PdfError::InvalidArgument);
  }
  if (evidence.requiredFileCount != manifest.requiredFileCount ||
      evidence.requiredFileBytes != manifest.requiredFileBytes ||
      evidence.requiredFileLedger != manifest.requiredFileLedger) {
    return PdfStatus::failure(PdfError::Malformed);
  }
  if (prior.selected && (!pdfSourceIdentityEqual(prior.manifest.source, manifest.source) ||
                         !pdfCacheSequenceNewer(manifest.sequence, prior.manifest.sequence))) {
    return PdfStatus::failure(PdfError::InvalidArgument);
  }

  PdfCacheSlot newestValid = PdfCacheSlot::B;
  bool hasValid = false;
  for (uint8_t index = 0; index < 2; ++index) {
    if (!prior.slots[index].valid) {
      continue;
    }
    if (!hasValid || pdfCacheSequenceNewer(prior.slots[index].manifest.sequence,
                                           prior.slots[slotIndex(newestValid)].manifest.sequence)) {
      newestValid = index == 0 ? PdfCacheSlot::A : PdfCacheSlot::B;
      hasValid = true;
    }
  }
  const PdfCacheSlot target = hasValid ? opposite(newestValid) : PdfCacheSlot::A;
  char path[PDF_CACHE_PATH_CAPACITY];
  PdfStatus status = formatPath(kManifestNames[slotIndex(target)], path, sizeof(path));
  if (!status) {
    return status;
  }
  status = io_.mkdir(io_.context, root_);
  if (!status) {
    return status;
  }
  PdfCacheHandle handle{};
  status = io_.open(io_.context, path, PdfCacheOpenMode::WriteTruncate, &handle);
  if (!status) {
    return status;
  }
  CacheHandleSink sink{&io_, handle};
  status = pdfEncodeCacheManifest(manifest, files, sink.sink());
  if (status) {
    status = io_.flush(io_.context, handle);
  }
  if (status) {
    status = io_.sync(io_.context, handle);
  }
  status = closeHandle(io_, &handle, status);
  if (!status) {
    return status;
  }

  status = io_.open(io_.context, path, PdfCacheOpenMode::Read, &handle);
  if (!status) {
    return status;
  }
  PdfCacheFileMetadata metadata{};
  status = io_.metadata(io_.context, handle, &metadata);
  PdfCacheManifest verified{};
  if (status && !metadata.directory && !metadata.symlinkLike) {
    CacheHandleSource source{&io_, handle, metadata.size};
    status = pdfDecodeCacheManifest(source.source(), &verified, {});
    if (status && !manifestsEqual(verified, manifest)) {
      status = PdfStatus::failure(PdfError::Malformed);
    }
  } else if (status) {
    status = PdfStatus::failure(PdfError::Malformed);
  }
  status = closeHandle(io_, &handle, status);
  if (!status) {
    return status;
  }

  PdfCacheManifestSelection loaded = prior;
  auto& targetState = loaded.slots[slotIndex(target)];
  targetState.valid = true;
  targetState.sourceMatches = true;
  targetState.manifest = verified;
  loaded.selected = true;
  loaded.selectedSlot = target;
  loaded.manifest = verified;
  if (committed != nullptr) {
    *committed = loaded;
  }
  return PdfStatus::success();
}

PdfStatus PdfCacheStore::loadCheckpointSlots(const PdfSourceIdentity& expectedSource,
                                             PdfBuildCheckpointSelection* const selection) const {
  if (!io_.valid() || selection == nullptr) {
    return PdfStatus::failure(PdfError::InvalidArgument);
  }
  resetInPlace(*selection);
  for (uint8_t index = 0; index < 2; ++index) {
    auto& state = selection->slots[index];
    char path[PDF_CACHE_PATH_CAPACITY];
    PdfStatus status = formatPath(kCheckpointNames[index], path, sizeof(path));
    if (!status) {
      return status;
    }
    status = loadCheckpointSlot(io_, path, expectedSource, &state);
    if (!status) {
      if (status.error == PdfError::InvalidOffset ||
          isRecoverableSlotError(status.error)) {
        resetInPlace(state);
        continue;
      }
      return status;
    }
    if (state.sourceMatches &&
        (!selection->selected ||
         pdfCacheSequenceNewer(state.checkpoint.sequence,
                               selection->checkpoint.sequence))) {
      selection->selected = true;
      selection->selectedSlot = index == 0 ? PdfCacheSlot::A : PdfCacheSlot::B;
      selection->checkpoint = state.checkpoint;
    }
  }
  return PdfStatus::success();
}

PdfStatus PdfCacheStore::commitCheckpoint(const PdfBuildCheckpoint& checkpoint) const {
  if (!io_.valid()) {
    return PdfStatus::failure(PdfError::InvalidArgument);
  }
  // The monotonically increasing sequence itself selects the inactive slot.
  // This avoids reopening both checkpoint files while the source PDF is open.
  const PdfCacheSlot target = (checkpoint.sequence & 1U) != 0 ? PdfCacheSlot::A : PdfCacheSlot::B;
  char path[PDF_CACHE_PATH_CAPACITY];
  PdfStatus status = formatPath(kCheckpointNames[slotIndex(target)], path, sizeof(path));
  if (!status) {
    return status;
  }
  status = io_.mkdir(io_.context, root_);
  if (!status) {
    return status;
  }
  PdfCacheHandle handle{};
  status = io_.open(io_.context, path, PdfCacheOpenMode::WriteTruncate, &handle);
  if (!status) {
    return status;
  }
  CacheHandleSink sink{&io_, handle};
  status = pdfEncodeBuildCheckpoint(checkpoint, sink.sink());
  if (status) {
    status = io_.flush(io_.context, handle);
  }
  if (status) {
    status = io_.sync(io_.context, handle);
  }
  status = closeHandle(io_, &handle, status);
  if (!status) {
    return status;
  }
  return verifyCheckpointFile(io_, path, checkpoint);
}

PdfStatus PdfCacheStore::listGenerations(
    PdfCacheGenerationList* const generations) const {
  if (!io_.valid() || generations == nullptr) {
    return PdfStatus::failure(PdfError::InvalidArgument);
  }
  *generations = {};
  const PdfStatus status = io_.list(
      io_.context, root_, collectGeneration, generations);
  return !status && status.error == PdfError::InvalidOffset
             ? PdfStatus::success()
             : status;
}

PdfStatus PdfCacheStore::removeGeneration(
    const uint32_t generation) const {
  if (!io_.valid() || generation == 0) {
    return PdfStatus::failure(PdfError::InvalidArgument);
  }
  return removeGenerationPath(io_, root_, generation);
}

PdfStatus PdfCacheStore::cleanupUnreferencedGenerations() const {
  if (!io_.valid()) {
    return PdfStatus::failure(PdfError::InvalidArgument);
  }
  PdfCacheManifestSelection manifests{};
  PdfStatus status = loadManifestSlots({}, &manifests);
  if (!status) {
    return status;
  }
  return cleanupUnreferencedGenerations(manifests);
}

PdfStatus PdfCacheStore::cleanupUnreferencedGenerations(
    const PdfCacheManifestSelection& manifests) const {
  if (!io_.valid()) {
    return PdfStatus::failure(PdfError::InvalidArgument);
  }
  PdfCacheGenerationList candidates{};
  PdfStatus status = listGenerations(&candidates);
  if (!status) {
    return status;
  }
  for (size_t candidateIndex = 0; candidateIndex < candidates.count; ++candidateIndex) {
    const uint32_t generation = candidates.generations[candidateIndex];
    bool protectedGeneration = false;
    for (const auto& slot : manifests.slots) {
      protectedGeneration = protectedGeneration || (slot.valid && slot.manifest.generation == generation);
    }
    if (protectedGeneration) {
      continue;
    }
    status = removeGenerationPath(io_, root_, generation);
    if (!status) {
      return status;
    }
  }
  return PdfStatus::success();
}
