#include "AtomicBookUpload.h"

#include <cstring>
#include <limits>

namespace BookUpload {
namespace {

constexpr char kTemporarySuffix[] = ".crossink-upload.tmp";
constexpr char kBackupSuffix[] = ".crossink-upload.bak";
constexpr char kCommitSuffix[] = ".crossink-upload.commit";
constexpr uint8_t kCommitMagic[8] = {'C', 'R', 'O', 'S', 'S', 'U', 'P', '1'};
constexpr uint8_t kCommitVersion = 1;
constexpr uint8_t kCommitHadTarget = 0x01;
constexpr size_t kCommitPayloadSize = 64;
constexpr size_t kCommitMarkerSize = kAtomicUploadCommitMarkerSize;
constexpr size_t kCommitVersionOffset = 8;
constexpr size_t kCommitFlagsOffset = 9;
constexpr size_t kCommitPathLengthOffset = 10;
constexpr size_t kCommitSizeOffset = 12;
constexpr size_t kCommitDigestOffset = 20;
constexpr size_t kCommitPathCrcOffset = 52;
constexpr size_t kCommitGuardOffset = 56;
constexpr size_t kCommitCrcOffset = 60;
constexpr uint32_t kCommitGuard = UINT32_C(0xc35a91e7);

static_assert(kCommitMarkerSize <= kAtomicUploadSha256ContextCapacity,
              "commit marker must fit the reusable SHA-256 workspace");
static_assert(kCommitMarkerSize == kCommitPayloadSize * 2, "commit marker redundancy layout changed");

bool validIo(const AtomicUploadIo& io) {
  return io.exists != nullptr && io.openWrite != nullptr && io.write != nullptr && io.flush != nullptr &&
         io.sync != nullptr && io.close != nullptr && io.openRead != nullptr && io.read != nullptr &&
         io.remove != nullptr && io.rename != nullptr && io.sha256.contextSize != 0 &&
         io.sha256.contextSize <= kAtomicUploadSha256ContextCapacity && io.sha256.start != nullptr &&
         io.sha256.update != nullptr && io.sha256.finish != nullptr && io.sha256.abort != nullptr;
}

bool validCommitHook(const AtomicUploadCommitHook& hook) { return hook.committed != nullptr; }

uint32_t crc32(const uint8_t* const data, const size_t length) {
  uint32_t crc = UINT32_MAX;
  for (size_t index = 0; index < length; ++index) {
    crc ^= data[index];
    for (uint8_t bit = 0; bit < 8; ++bit) {
      const uint32_t mask = 0U - (crc & 1U);
      crc = (crc >> 1U) ^ (UINT32_C(0xedb88320) & mask);
    }
  }
  return ~crc;
}

void storeUint16(uint8_t* const destination, const uint16_t value) {
  destination[0] = static_cast<uint8_t>(value);
  destination[1] = static_cast<uint8_t>(value >> 8U);
}

uint16_t loadUint16(const uint8_t* const source) {
  return static_cast<uint16_t>(source[0]) | static_cast<uint16_t>(source[1]) << 8U;
}

void storeUint32(uint8_t* const destination, const uint32_t value) {
  for (size_t index = 0; index < 4; ++index) {
    destination[index] = static_cast<uint8_t>(value >> (index * 8U));
  }
}

uint32_t loadUint32(const uint8_t* const source) {
  uint32_t value = 0;
  for (size_t index = 0; index < 4; ++index) {
    value |= static_cast<uint32_t>(source[index]) << (index * 8U);
  }
  return value;
}

void storeUint64(uint8_t* const destination, const uint64_t value) {
  storeUint32(destination, static_cast<uint32_t>(value));
  storeUint32(destination + sizeof(uint32_t), static_cast<uint32_t>(value >> 32U));
}

bool copyTargetPath(AtomicUploadState& state, const char* const targetPath) {
  if (targetPath == nullptr || targetPath[0] == '\0') return false;
  const size_t length = strnlen(targetPath, kAtomicUploadPathCapacity);
  if (length == 0 || length >= kAtomicUploadPathCapacity) return false;
  memcpy(state.targetPath, targetPath, length + 1);
  return true;
}

bool buildArtifactPath(AtomicUploadState& state, const char* const suffix) {
  const char* const slash = strrchr(state.targetPath, '/');
  const size_t directoryLength = slash == nullptr ? 0 : static_cast<size_t>(slash - state.targetPath + 1);
  const char* const fileName = state.targetPath + directoryLength;
  const size_t fileNameLength = strlen(fileName);
  const size_t suffixLength = strlen(suffix);
  if (fileNameLength == 0 || directoryLength + 1 + fileNameLength + suffixLength >= kAtomicUploadPathCapacity) {
    return false;
  }
  memcpy(state.artifactPath, state.targetPath, directoryLength);
  state.artifactPath[directoryLength] = '.';
  memcpy(state.artifactPath + directoryLength + 1, fileName, fileNameLength);
  memcpy(state.artifactPath + directoryLength + 1 + fileNameLength, suffix, suffixLength + 1);
  return true;
}

bool removeIfPresent(const AtomicUploadIo& io, const char* const path) {
  return !io.exists(io.context, path) || io.remove(io.context, path) || !io.exists(io.context, path);
}

bool observedRename(const AtomicUploadIo& io, const char* const source, const char* const destination) {
  io.rename(io.context, source, destination);
  return !io.exists(io.context, source) && io.exists(io.context, destination);
}

void discardDigest(AtomicUploadState& state, const AtomicUploadIo& io) {
  if (!state.digestActive) return;
  io.sha256.abort(state.sha256Context);
  state.digestActive = false;
}

bool startDigest(AtomicUploadState& state, const AtomicUploadIo& io) {
  if (state.digestActive || !io.sha256.start(state.sha256Context)) return false;
  state.digestActive = true;
  return true;
}

bool finishDigest(AtomicUploadState& state, const AtomicUploadIo& io, uint8_t digest[kAtomicUploadSha256Size]) {
  if (!state.digestActive) return false;
  const bool finished = io.sha256.finish(state.sha256Context, digest);
  state.digestActive = false;
  return finished;
}

void resetState(AtomicUploadState& state) {
  state.expectedSize = kUnknownUploadSize;
  state.receivedSize = 0;
  memset(state.digest, 0, sizeof(state.digest));
  memset(state.sha256Context, 0, sizeof(state.sha256Context));
  state.phase = AtomicUploadPhase::Idle;
  state.fileOpen = false;
  state.digestActive = false;
}

bool restoreBackup(AtomicUploadState& state, const AtomicUploadIo& io) {
  if (!buildArtifactPath(state, kBackupSuffix)) return false;
  if (!io.exists(io.context, state.artifactPath)) {
    return io.exists(io.context, state.targetPath);
  }
  if (io.exists(io.context, state.targetPath) && !removeIfPresent(io, state.targetPath)) {
    return false;
  }
  return observedRename(io, state.artifactPath, state.targetPath);
}

void buildCommitMarker(AtomicUploadState& state, const bool hadTarget) {
  uint8_t* const marker = state.sha256Context;
  memset(marker, 0, kCommitMarkerSize);
  memcpy(marker, kCommitMagic, sizeof(kCommitMagic));
  marker[kCommitVersionOffset] = kCommitVersion;
  marker[kCommitFlagsOffset] = hadTarget ? kCommitHadTarget : 0;
  const size_t targetLength = strlen(state.targetPath);
  storeUint16(marker + kCommitPathLengthOffset, static_cast<uint16_t>(targetLength));
  storeUint64(marker + kCommitSizeOffset, state.receivedSize);
  memcpy(marker + kCommitDigestOffset, state.digest, sizeof(state.digest));
  storeUint32(marker + kCommitPathCrcOffset, crc32(reinterpret_cast<const uint8_t*>(state.targetPath), targetLength));
  storeUint32(marker + kCommitGuardOffset, kCommitGuard);
  storeUint32(marker + kCommitCrcOffset, crc32(marker, kCommitCrcOffset));
  for (size_t index = 0; index < kCommitPayloadSize; ++index) {
    marker[kCommitPayloadSize + index] = static_cast<uint8_t>(~marker[index]);
  }
}

bool validCommitMarker(const AtomicUploadState& state, bool& hadTarget) {
  const uint8_t* const marker = state.sha256Context;
  for (size_t index = 0; index < kCommitPayloadSize; ++index) {
    if (static_cast<uint8_t>(marker[index] ^ marker[kCommitPayloadSize + index]) != UINT8_MAX) return false;
  }
  if (memcmp(marker, kCommitMagic, sizeof(kCommitMagic)) != 0 || marker[kCommitVersionOffset] != kCommitVersion ||
      (marker[kCommitFlagsOffset] & ~kCommitHadTarget) != 0 ||
      loadUint32(marker + kCommitGuardOffset) != kCommitGuard ||
      loadUint32(marker + kCommitCrcOffset) != crc32(marker, kCommitCrcOffset)) {
    return false;
  }
  const size_t targetLength = strlen(state.targetPath);
  if (loadUint16(marker + kCommitPathLengthOffset) != targetLength ||
      loadUint32(marker + kCommitPathCrcOffset) !=
          crc32(reinterpret_cast<const uint8_t*>(state.targetPath), targetLength)) {
    return false;
  }
  hadTarget = (marker[kCommitFlagsOffset] & kCommitHadTarget) != 0;
  return true;
}

bool readCommitMarker(AtomicUploadState& state, const AtomicUploadIo& io, bool& hadTarget) {
  if (!buildArtifactPath(state, kCommitSuffix) || !io.openRead(io.context, state.artifactPath)) return false;
  state.fileOpen = true;
  size_t offset = 0;
  bool readOk = true;
  while (offset < kCommitMarkerSize) {
    const int count = io.read(io.context, state.sha256Context + offset, kCommitMarkerSize - offset);
    if (count <= 0 || static_cast<size_t>(count) > kCommitMarkerSize - offset) {
      readOk = false;
      break;
    }
    offset += static_cast<size_t>(count);
  }
  if (readOk) {
    uint8_t extra = 0;
    readOk = io.read(io.context, &extra, 1) == 0;
  }
  const bool closeOk = io.close(io.context);
  state.fileOpen = false;
  return readOk && closeOk && validCommitMarker(state, hadTarget);
}

bool rollbackMarkedCommit(AtomicUploadState& state, const AtomicUploadIo& io, const bool hadTarget) {
  if (hadTarget) {
    if (!buildArtifactPath(state, kBackupSuffix) || !io.exists(io.context, state.artifactPath) ||
        !restoreBackup(state, io)) {
      return false;
    }
  } else {
    if (!buildArtifactPath(state, kBackupSuffix) || io.exists(io.context, state.artifactPath)) return false;
    const bool targetExists = io.exists(io.context, state.targetPath);
    if (!buildArtifactPath(state, kTemporarySuffix)) return false;
    const bool tempExists = io.exists(io.context, state.artifactPath);
    if (targetExists && tempExists) return false;
    if (targetExists && !removeIfPresent(io, state.targetPath)) return false;
  }

  if (!buildArtifactPath(state, kTemporarySuffix) || !removeIfPresent(io, state.artifactPath)) return false;
  return buildArtifactPath(state, kCommitSuffix) && removeIfPresent(io, state.artifactPath);
}

bool recoverArtifacts(AtomicUploadState& state, const AtomicUploadIo& io) {
  if (!buildArtifactPath(state, kCommitSuffix)) return false;
  if (io.exists(io.context, state.artifactPath)) {
    bool hadTarget = false;
    if (!readCommitMarker(state, io, hadTarget)) return false;
    return rollbackMarkedCommit(state, io, hadTarget);
  }

  if (!buildArtifactPath(state, kBackupSuffix)) return false;
  if (io.exists(io.context, state.artifactPath) && !restoreBackup(state, io)) return false;
  if (!buildArtifactPath(state, kTemporarySuffix)) return false;
  return removeIfPresent(io, state.artifactPath);
}

AtomicUploadStatus closeAndDiscardTemp(AtomicUploadState& state, const AtomicUploadIo& io,
                                       const AtomicUploadStatus failureStatus) {
  discardDigest(state, io);
  bool closeOk = true;
  if (state.fileOpen) {
    closeOk = io.close(io.context);
    state.fileOpen = false;
  }
  const bool pathOk = buildArtifactPath(state, kTemporarySuffix);
  const bool removeOk = pathOk && removeIfPresent(io, state.artifactPath);
  resetState(state);
  if (!closeOk) return AtomicUploadStatus::CloseFailed;
  if (!removeOk) return AtomicUploadStatus::CleanupFailed;
  return failureStatus;
}

AtomicUploadStatus rollbackPendingCommit(AtomicUploadState& state, const AtomicUploadIo& io, const bool hadTarget,
                                         const AtomicUploadStatus failureStatus) {
  discardDigest(state, io);
  if (state.fileOpen) {
    io.close(io.context);
    state.fileOpen = false;
  }
  const bool rolledBack = rollbackMarkedCommit(state, io, hadTarget);
  resetState(state);
  return rolledBack ? failureStatus : AtomicUploadStatus::CleanupFailed;
}

bool createCommitMarker(AtomicUploadState& state, const AtomicUploadIo& io, const bool hadTarget,
                        uint8_t* const verifyBuffer, const size_t verifyBufferSize) {
  buildCommitMarker(state, hadTarget);
  if (!buildArtifactPath(state, kCommitSuffix)) return false;
  if (!io.openWrite(io.context, state.artifactPath)) {
    removeIfPresent(io, state.artifactPath);
    return false;
  }
  state.fileOpen = true;
  const bool writeOk = io.write(io.context, state.sha256Context, kCommitMarkerSize) == kCommitMarkerSize;
  const bool flushOk = writeOk && io.flush(io.context);
  const bool syncOk = flushOk && io.sync(io.context);
  const bool closeOk = io.close(io.context);
  state.fileOpen = false;
  if (!writeOk || !flushOk || !syncOk || !closeOk) {
    removeIfPresent(io, state.artifactPath);
    return false;
  }

  if (!io.openRead(io.context, state.artifactPath)) {
    removeIfPresent(io, state.artifactPath);
    return false;
  }
  state.fileOpen = true;
  size_t offset = 0;
  bool verified = true;
  while (offset < kCommitMarkerSize) {
    const size_t capacity =
        (verifyBufferSize < kCommitMarkerSize - offset) ? verifyBufferSize : kCommitMarkerSize - offset;
    const int count = io.read(io.context, verifyBuffer, capacity);
    if (count <= 0 || static_cast<size_t>(count) > capacity ||
        memcmp(verifyBuffer, state.sha256Context + offset, static_cast<size_t>(count)) != 0) {
      verified = false;
      break;
    }
    offset += static_cast<size_t>(count);
  }
  if (verified) {
    verified = io.read(io.context, verifyBuffer, 1) == 0;
  }
  const bool verifyCloseOk = io.close(io.context);
  state.fileOpen = false;
  if (!verified || !verifyCloseOk) {
    removeIfPresent(io, state.artifactPath);
    return false;
  }
  return true;
}

}  // namespace

AtomicUploadStatus admitTransportStart(const bool transactionActive, UploadTransportResponse& response) {
  response.success = false;
  response.hasError = transactionActive;
  return transactionActive ? AtomicUploadStatus::Busy : AtomicUploadStatus::Ok;
}

uint16_t httpResponseStatus(const UploadTransportResponse& response) {
  return (!response.hasError && response.success) ? 200 : 400;
}

AtomicUploadStatus begin(AtomicUploadState& state, const AtomicUploadIo& io, const char* const targetPath,
                         const uint64_t expectedSize) {
  if (!validIo(io) || targetPath == nullptr) return AtomicUploadStatus::InvalidArgument;
  if (isActive(state)) return AtomicUploadStatus::Busy;

  resetState(state);
  memset(state.targetPath, 0, sizeof(state.targetPath));
  memset(state.artifactPath, 0, sizeof(state.artifactPath));
  if (!copyTargetPath(state, targetPath) || !buildArtifactPath(state, kTemporarySuffix) ||
      !buildArtifactPath(state, kBackupSuffix) || !buildArtifactPath(state, kCommitSuffix)) {
    resetState(state);
    return AtomicUploadStatus::PathTooLong;
  }
  if (!recoverArtifacts(state, io)) {
    resetState(state);
    return AtomicUploadStatus::RecoveryFailed;
  }
  if (!startDigest(state, io)) {
    resetState(state);
    return AtomicUploadStatus::DigestFailed;
  }
  if (!buildArtifactPath(state, kTemporarySuffix) || !io.openWrite(io.context, state.artifactPath)) {
    discardDigest(state, io);
    if (state.artifactPath[0] != '\0') removeIfPresent(io, state.artifactPath);
    resetState(state);
    return AtomicUploadStatus::OpenWriteFailed;
  }

  state.expectedSize = expectedSize;
  state.receivedSize = 0;
  state.phase = AtomicUploadPhase::Writing;
  state.fileOpen = true;
  return AtomicUploadStatus::Ok;
}

AtomicUploadStatus write(AtomicUploadState& state, const AtomicUploadIo& io, const uint8_t* const data,
                         const size_t length) {
  if (!validIo(io) || (length != 0 && data == nullptr)) return AtomicUploadStatus::InvalidArgument;
  if (!isActive(state) || !state.fileOpen || !state.digestActive) return AtomicUploadStatus::NotActive;
  if (length > std::numeric_limits<uint64_t>::max() - state.receivedSize ||
      (state.expectedSize != kUnknownUploadSize && length > state.expectedSize - state.receivedSize)) {
    return closeAndDiscardTemp(state, io, AtomicUploadStatus::SizeMismatch);
  }
  if (length == 0) return AtomicUploadStatus::Ok;

  const size_t written = io.write(io.context, data, length);
  if (written != length) {
    return closeAndDiscardTemp(state, io, AtomicUploadStatus::WriteFailed);
  }
  if (!io.sha256.update(state.sha256Context, data, length)) {
    return closeAndDiscardTemp(state, io, AtomicUploadStatus::DigestFailed);
  }
  state.receivedSize += length;
  return AtomicUploadStatus::Ok;
}

AtomicUploadStatus finish(AtomicUploadState& state, const AtomicUploadIo& io, const uint64_t expectedSize,
                          uint8_t* const verifyBuffer, const size_t verifyBufferSize,
                          const AtomicUploadCommitHook commitHook) {
  if (!validIo(io) || !validCommitHook(commitHook) || verifyBuffer == nullptr || verifyBufferSize == 0) {
    return AtomicUploadStatus::InvalidArgument;
  }
  if (!isActive(state) || !state.fileOpen || !state.digestActive) return AtomicUploadStatus::NotActive;
  if (expectedSize == kUnknownUploadSize || state.receivedSize != expectedSize ||
      (state.expectedSize != kUnknownUploadSize && state.expectedSize != expectedSize)) {
    return closeAndDiscardTemp(state, io, AtomicUploadStatus::SizeMismatch);
  }

  if (!io.flush(io.context)) return closeAndDiscardTemp(state, io, AtomicUploadStatus::FlushFailed);
  if (!io.sync(io.context)) return closeAndDiscardTemp(state, io, AtomicUploadStatus::SyncFailed);
  const bool writeCloseOk = io.close(io.context);
  state.fileOpen = false;
  if (!writeCloseOk) return closeAndDiscardTemp(state, io, AtomicUploadStatus::CloseFailed);
  if (!finishDigest(state, io, state.digest)) {
    return closeAndDiscardTemp(state, io, AtomicUploadStatus::DigestFailed);
  }

  if (!startDigest(state, io)) return closeAndDiscardTemp(state, io, AtomicUploadStatus::DigestFailed);
  if (!buildArtifactPath(state, kTemporarySuffix) || !io.openRead(io.context, state.artifactPath)) {
    return closeAndDiscardTemp(state, io, AtomicUploadStatus::OpenReadFailed);
  }
  state.fileOpen = true;

  uint64_t verifiedSize = 0;
  while (true) {
    const int count = io.read(io.context, verifyBuffer, verifyBufferSize);
    if (count < 0 || static_cast<size_t>(count) > verifyBufferSize) {
      return closeAndDiscardTemp(state, io, AtomicUploadStatus::ReadFailed);
    }
    if (count == 0) break;
    const size_t byteCount = static_cast<size_t>(count);
    if (verifiedSize > expectedSize || byteCount > expectedSize - verifiedSize) {
      return closeAndDiscardTemp(state, io, AtomicUploadStatus::VerificationFailed);
    }
    if (!io.sha256.update(state.sha256Context, verifyBuffer, byteCount)) {
      return closeAndDiscardTemp(state, io, AtomicUploadStatus::DigestFailed);
    }
    verifiedSize += byteCount;
  }
  const bool readCloseOk = io.close(io.context);
  state.fileOpen = false;
  if (!readCloseOk) return closeAndDiscardTemp(state, io, AtomicUploadStatus::CloseFailed);

  uint8_t verifiedDigest[kAtomicUploadSha256Size]{};
  if (!finishDigest(state, io, verifiedDigest)) {
    return closeAndDiscardTemp(state, io, AtomicUploadStatus::DigestFailed);
  }
  if (verifiedSize != expectedSize || memcmp(verifiedDigest, state.digest, sizeof(verifiedDigest)) != 0) {
    return closeAndDiscardTemp(state, io, AtomicUploadStatus::VerificationFailed);
  }

  const bool hadTarget = io.exists(io.context, state.targetPath);
  if (hadTarget) {
    if (!buildArtifactPath(state, kBackupSuffix) || !observedRename(io, state.targetPath, state.artifactPath)) {
      const bool restored = restoreBackup(state, io);
      const bool tempOk = buildArtifactPath(state, kTemporarySuffix) && removeIfPresent(io, state.artifactPath);
      resetState(state);
      if (!restored || !tempOk) return AtomicUploadStatus::CleanupFailed;
      return AtomicUploadStatus::BackupFailed;
    }
  }

  if (!createCommitMarker(state, io, hadTarget, verifyBuffer, verifyBufferSize)) {
    bool rollbackOk = true;
    if (hadTarget) rollbackOk = restoreBackup(state, io);
    const bool tempOk = buildArtifactPath(state, kTemporarySuffix) && removeIfPresent(io, state.artifactPath);
    const bool markerOk = buildArtifactPath(state, kCommitSuffix) && removeIfPresent(io, state.artifactPath);
    resetState(state);
    return rollbackOk && tempOk && markerOk ? AtomicUploadStatus::MarkerFailed : AtomicUploadStatus::CleanupFailed;
  }

  if (!buildArtifactPath(state, kTemporarySuffix) || !observedRename(io, state.artifactPath, state.targetPath)) {
    return rollbackPendingCommit(state, io, hadTarget, AtomicUploadStatus::PromotionFailed);
  }

  if (!commitHook.committed(commitHook.context, state.targetPath)) {
    return rollbackPendingCommit(state, io, hadTarget, AtomicUploadStatus::CommitHookFailed);
  }

  if (!buildArtifactPath(state, kCommitSuffix) || !removeIfPresent(io, state.artifactPath)) {
    return rollbackPendingCommit(state, io, hadTarget, AtomicUploadStatus::CleanupFailed);
  }
  if (hadTarget && (!buildArtifactPath(state, kBackupSuffix) || !removeIfPresent(io, state.artifactPath))) {
    restoreBackup(state, io);
    resetState(state);
    return AtomicUploadStatus::CleanupFailed;
  }

  resetState(state);
  return AtomicUploadStatus::Ok;
}

AtomicUploadStatus abort(AtomicUploadState& state, const AtomicUploadIo& io) {
  if (!validIo(io)) return AtomicUploadStatus::InvalidArgument;
  if (!isActive(state)) return AtomicUploadStatus::Ok;
  return closeAndDiscardTemp(state, io, AtomicUploadStatus::Ok);
}

bool isActive(const AtomicUploadState& state) { return state.phase == AtomicUploadPhase::Writing; }

}  // namespace BookUpload
