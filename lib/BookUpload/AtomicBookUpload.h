#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>

namespace BookUpload {

constexpr size_t kAtomicUploadPathCapacity = 256;
constexpr size_t kAtomicUploadSha256Size = 32;
constexpr size_t kAtomicUploadSha256ContextCapacity = 128;
constexpr size_t kAtomicUploadCommitMarkerSize = 128;
constexpr uint64_t kUnknownUploadSize = std::numeric_limits<uint64_t>::max();

enum class AtomicUploadStatus : uint8_t {
  Ok,
  InvalidArgument,
  Busy,
  PathTooLong,
  RecoveryFailed,
  OpenWriteFailed,
  SizeMismatch,
  WriteFailed,
  FlushFailed,
  SyncFailed,
  CloseFailed,
  OpenReadFailed,
  ReadFailed,
  DigestFailed,
  VerificationFailed,
  BackupFailed,
  MarkerFailed,
  PromotionFailed,
  CommitHookFailed,
  CleanupFailed,
  NotActive,
};

struct AtomicUploadSha256 {
  size_t contextSize = 0;
  bool (*start)(void* context) = nullptr;
  bool (*update)(void* context, const uint8_t* data, size_t length) = nullptr;
  // finish must release internal resources even when it reports failure.
  bool (*finish)(void* context, uint8_t digest[kAtomicUploadSha256Size]) = nullptr;
  void (*abort)(void* context) = nullptr;
};

struct AtomicUploadIo {
  void* context = nullptr;
  bool (*exists)(void* context, const char* path) = nullptr;
  bool (*openWrite)(void* context, const char* path) = nullptr;
  size_t (*write)(void* context, const uint8_t* data, size_t length) = nullptr;
  bool (*flush)(void* context) = nullptr;
  bool (*sync)(void* context) = nullptr;
  // close must release the handle even when it reports a media error.
  bool (*close)(void* context) = nullptr;
  bool (*openRead)(void* context, const char* path) = nullptr;
  // Returns bytes read, zero at EOF, or a negative value on failure.
  int (*read)(void* context, uint8_t* destination, size_t capacity) = nullptr;
  bool (*remove)(void* context, const char* path) = nullptr;
  bool (*rename)(void* context, const char* source, const char* destination) = nullptr;
  AtomicUploadSha256 sha256{};
};

struct AtomicUploadCommitHook {
  void* context = nullptr;
  bool (*committed)(void* context, const char* targetPath) = nullptr;
};

enum class AtomicUploadPhase : uint8_t {
  Idle,
  Writing,
};

// This state intentionally owns its bounded path workspace. Keep it as a
// long-lived server member; it is too large for an ESP32-C3 task stack local.
struct AtomicUploadState {
  char targetPath[kAtomicUploadPathCapacity]{};
  char artifactPath[kAtomicUploadPathCapacity]{};
  uint64_t expectedSize = kUnknownUploadSize;
  uint64_t receivedSize = 0;
  uint8_t digest[kAtomicUploadSha256Size]{};
  alignas(std::max_align_t) uint8_t sha256Context[kAtomicUploadSha256ContextCapacity]{};
  AtomicUploadPhase phase = AtomicUploadPhase::Idle;
  bool fileOpen = false;
  bool digestActive = false;
};

static_assert(sizeof(AtomicUploadState) <= 704, "Atomic upload state exceeded its fixed DRAM budget");

struct UploadTransportResponse {
  bool success = false;
  bool hasError = false;
};

AtomicUploadStatus admitTransportStart(bool transactionActive, UploadTransportResponse& response);
uint16_t httpResponseStatus(const UploadTransportResponse& response);

AtomicUploadStatus begin(AtomicUploadState& state, const AtomicUploadIo& io, const char* targetPath,
                         uint64_t expectedSize);
AtomicUploadStatus write(AtomicUploadState& state, const AtomicUploadIo& io, const uint8_t* data, size_t length);
AtomicUploadStatus finish(AtomicUploadState& state, const AtomicUploadIo& io, uint64_t expectedSize,
                          uint8_t* verifyBuffer, size_t verifyBufferSize,
                          AtomicUploadCommitHook commitHook = AtomicUploadCommitHook{});
AtomicUploadStatus abort(AtomicUploadState& state, const AtomicUploadIo& io);
bool isActive(const AtomicUploadState& state);

}  // namespace BookUpload
