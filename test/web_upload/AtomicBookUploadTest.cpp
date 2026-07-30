#include <AtomicBookUpload.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "TestSha256.h"

namespace {

using BookUpload::AtomicUploadIo;
using BookUpload::AtomicUploadState;
using BookUpload::AtomicUploadStatus;

static_assert(sizeof(decltype(AtomicUploadState::digest)) == BookUpload::kAtomicUploadSha256Size,
              "stream verification must retain the complete SHA-256 digest");
static_assert(sizeof(TestSha256::Context) <= BookUpload::kAtomicUploadSha256ContextCapacity,
              "host SHA-256 context must fit the production fixed workspace");

class MemoryUploadIo {
 public:
  enum class Fault {
    None,
    OpenWrite,
    ShortWrite,
    Flush,
    Sync,
    CloseWrite,
    OpenRead,
    Read,
    CloseRead,
    CorruptRead,
    RenameBackup,
    RenamePromote,
    RenameRestore,
    RemoveTemp,
    RemoveBackup,
    RemoveMarker,
    RemoveTarget,
    MarkerOpenWrite,
    MarkerWrite,
    MarkerFlush,
    MarkerSync,
    MarkerCloseWrite,
    MarkerOpenRead,
    MarkerRead,
    MarkerCloseRead,
    MarkerCorruptRead,
  };

  std::map<std::string, std::vector<uint8_t>> files;
  std::string openPath;
  size_t readPosition = 0;
  bool writable = false;
  bool handleOpen = false;
  size_t openHandles = 0;
  size_t maximumOpenHandles = 0;
  size_t committedCalls = 0;
  bool commitSucceeds = true;
  std::string committedPath;
  bool derivedCacheExists = true;
  bool backupPresentAtCommit = false;
  bool markerPresentAtCommit = false;
  bool tempPresentAtCommit = false;
  std::vector<uint8_t> targetBytesAtCommit;
  std::vector<uint8_t> capturedMarker;
  std::vector<uint8_t> forcedReadback;
  Fault fault = Fault::None;
  bool failAfter = false;
  bool faultConsumed = false;
  std::vector<std::string> operations;

  AtomicUploadIo callbacks() {
    AtomicUploadIo io{};
    io.context = this;
    io.exists = &exists;
    io.openWrite = &openWrite;
    io.write = &write;
    io.flush = &flush;
    io.sync = &sync;
    io.close = &close;
    io.openRead = &openRead;
    io.read = &read;
    io.remove = &remove;
    io.rename = &rename;
    io.sha256.contextSize = sizeof(TestSha256::Context);
    io.sha256.start = &TestSha256::start;
    io.sha256.update = &TestSha256::update;
    io.sha256.finish = &TestSha256::finish;
    io.sha256.abort = &TestSha256::abort;
    return io;
  }

  static bool exists(void* context, const char* path) {
    return self(context).files.find(path) != self(context).files.end();
  }

  static bool openWrite(void* context, const char* path) {
    auto& fs = self(context);
    fs.operations.emplace_back(std::string("open-write:") + path);
    const Fault operation = endsWith(path, ".crossink-upload.commit") ? Fault::MarkerOpenWrite : Fault::OpenWrite;
    if (fs.matches(operation)) {
      if (fs.failAfter) fs.files[path].clear();
      fs.faultConsumed = true;
      return false;
    }
    if (fs.handleOpen) return false;
    fs.files[path].clear();
    fs.openPath = path;
    fs.readPosition = 0;
    fs.writable = true;
    fs.handleOpen = true;
    ++fs.openHandles;
    fs.maximumOpenHandles = std::max(fs.maximumOpenHandles, fs.openHandles);
    return true;
  }

  static size_t write(void* context, const uint8_t* data, size_t length) {
    auto& fs = self(context);
    fs.operations.emplace_back("write");
    if (!fs.writable || !fs.handleOpen || fs.openPath.empty()) return 0;
    const bool marker = endsWith(fs.openPath, ".crossink-upload.commit");
    const Fault operation = marker ? Fault::MarkerWrite : Fault::ShortWrite;
    if (fs.matches(operation)) {
      fs.faultConsumed = true;
      if (fs.failAfter) {
        auto& bytes = fs.files[fs.openPath];
        bytes.insert(bytes.end(), data, data + length);
        return length == 0 ? 0 : length - 1;
      }
      const size_t shortLength = length == 0 ? 0 : length - 1;
      auto& bytes = fs.files[fs.openPath];
      bytes.insert(bytes.end(), data, data + shortLength);
      return shortLength;
    }
    auto& bytes = fs.files[fs.openPath];
    bytes.insert(bytes.end(), data, data + length);
    return length;
  }

  static bool flush(void* context) {
    auto& fs = self(context);
    fs.operations.emplace_back("flush");
    const Fault operation = endsWith(fs.openPath, ".crossink-upload.commit") ? Fault::MarkerFlush : Fault::Flush;
    return !fs.consume(operation);
  }

  static bool sync(void* context) {
    auto& fs = self(context);
    fs.operations.emplace_back("sync");
    const Fault operation = endsWith(fs.openPath, ".crossink-upload.commit") ? Fault::MarkerSync : Fault::Sync;
    return !fs.consume(operation);
  }

  static bool close(void* context) {
    auto& fs = self(context);
    fs.operations.emplace_back("close");
    Fault operation = fs.writable ? Fault::CloseWrite : Fault::CloseRead;
    if (endsWith(fs.openPath, ".crossink-upload.commit")) {
      operation = fs.writable ? Fault::MarkerCloseWrite : Fault::MarkerCloseRead;
    }
    const bool failed = fs.consume(operation);
    fs.openPath.clear();
    fs.readPosition = 0;
    fs.writable = false;
    if (fs.handleOpen) {
      fs.handleOpen = false;
      --fs.openHandles;
    }
    return !failed;
  }

  static bool openRead(void* context, const char* path) {
    auto& fs = self(context);
    fs.operations.emplace_back(std::string("open-read:") + path);
    const Fault operation = endsWith(path, ".crossink-upload.commit") ? Fault::MarkerOpenRead : Fault::OpenRead;
    if (fs.matches(operation)) {
      fs.faultConsumed = true;
      return false;
    }
    if (fs.handleOpen) return false;
    if (!exists(context, path)) return false;
    fs.openPath = path;
    fs.readPosition = 0;
    fs.writable = false;
    fs.handleOpen = true;
    ++fs.openHandles;
    fs.maximumOpenHandles = std::max(fs.maximumOpenHandles, fs.openHandles);
    return true;
  }

  static int read(void* context, uint8_t* destination, size_t capacity) {
    auto& fs = self(context);
    fs.operations.emplace_back("read");
    const bool marker = endsWith(fs.openPath, ".crossink-upload.commit");
    const Fault operation = marker ? Fault::MarkerRead : Fault::Read;
    if (fs.matches(operation) && !fs.failAfter) {
      fs.faultConsumed = true;
      return -1;
    }
    if (!fs.handleOpen || fs.openPath.empty() || fs.writable) return -1;
    const bool forceTempReadback = endsWith(fs.openPath, ".crossink-upload.tmp") && !fs.forcedReadback.empty();
    const auto& bytes = forceTempReadback ? fs.forcedReadback : fs.files.at(fs.openPath);
    const size_t count = std::min(capacity, bytes.size() - fs.readPosition);
    if (count == 0) return 0;
    memcpy(destination, bytes.data() + fs.readPosition, count);
    if (marker && fs.consume(Fault::MarkerCorruptRead)) destination[0] ^= 0x80U;
    if (!marker && fs.consume(Fault::CorruptRead)) destination[0] ^= 0x80U;
    fs.readPosition += count;
    if (fs.matches(operation)) {
      fs.faultConsumed = true;
      return -1;
    }
    return static_cast<int>(count);
  }

  static bool remove(void* context, const char* path) {
    auto& fs = self(context);
    fs.operations.emplace_back(std::string("remove:") + path);
    Fault operation = Fault::RemoveTarget;
    if (endsWith(path, ".crossink-upload.tmp")) operation = Fault::RemoveTemp;
    if (endsWith(path, ".crossink-upload.bak")) operation = Fault::RemoveBackup;
    if (endsWith(path, ".crossink-upload.commit")) operation = Fault::RemoveMarker;
    const bool failed = fs.matches(operation);
    if (failed && !fs.failAfter) {
      fs.faultConsumed = true;
      return false;
    }
    const bool removed = fs.files.erase(path) != 0;
    if (failed) {
      fs.faultConsumed = true;
      return false;
    }
    return removed;
  }

  static bool rename(void* context, const char* source, const char* destination) {
    auto& fs = self(context);
    fs.operations.emplace_back(std::string("rename:") + source + "->" + destination);
    Fault operation = Fault::RenameRestore;
    if (endsWith(destination, ".crossink-upload.bak")) operation = Fault::RenameBackup;
    if (endsWith(source, ".crossink-upload.tmp")) operation = Fault::RenamePromote;
    const bool failed = fs.matches(operation);
    if (failed && !fs.failAfter) {
      fs.faultConsumed = true;
      return false;
    }
    const auto sourceIt = fs.files.find(source);
    if (sourceIt == fs.files.end() || exists(context, destination)) return false;
    fs.files.emplace(destination, std::move(sourceIt->second));
    fs.files.erase(sourceIt);
    if (failed) {
      fs.faultConsumed = true;
      return false;
    }
    return true;
  }

  static bool committed(void* context, const char* path) {
    auto& fs = self(context);
    ++fs.committedCalls;
    fs.committedPath = path;
    const std::string target(path);
    const size_t slash = target.find_last_of('/');
    const size_t fileStart = slash == std::string::npos ? 0 : slash + 1;
    const std::string hiddenBase = target.substr(0, fileStart) + "." + target.substr(fileStart);
    fs.backupPresentAtCommit = fs.files.find(hiddenBase + ".crossink-upload.bak") != fs.files.end();
    fs.markerPresentAtCommit = fs.files.find(hiddenBase + ".crossink-upload.commit") != fs.files.end();
    fs.tempPresentAtCommit = fs.files.find(hiddenBase + ".crossink-upload.tmp") != fs.files.end();
    fs.targetBytesAtCommit = fs.files.at(target);
    if (fs.markerPresentAtCommit) fs.capturedMarker = fs.files.at(hiddenBase + ".crossink-upload.commit");
    if (fs.commitSucceeds) fs.derivedCacheExists = false;
    return fs.commitSucceeds;
  }

 private:
  static bool endsWith(const std::string& value, const char* suffix) {
    const size_t suffixLength = strlen(suffix);
    return value.size() >= suffixLength && value.compare(value.size() - suffixLength, suffixLength, suffix) == 0;
  }
  bool matches(Fault candidate) const { return !faultConsumed && fault == candidate; }
  bool consume(Fault candidate) {
    if (!matches(candidate)) return false;
    faultConsumed = true;
    return true;
  }
  static MemoryUploadIo& self(void* context) { return *static_cast<MemoryUploadIo*>(context); }
};

std::string artifactPath(const char* target, const char* suffix) {
  const std::string path(target);
  const size_t slash = path.find_last_of('/');
  const size_t fileStart = slash == std::string::npos ? 0 : slash + 1;
  return path.substr(0, fileStart) + "." + path.substr(fileStart) + suffix;
}

uint32_t legacyFnv32(const std::vector<uint8_t>& bytes) {
  uint32_t digest = UINT32_C(2166136261);
  for (const uint8_t byte : bytes) {
    digest ^= byte;
    digest *= UINT32_C(16777619);
  }
  return digest;
}

AtomicUploadStatus uploadBytes(MemoryUploadIo& fs, AtomicUploadState& upload, const char* path,
                               const std::vector<uint8_t>& bytes,
                               uint64_t declaredSize = BookUpload::kUnknownUploadSize) {
  AtomicUploadIo io = fs.callbacks();
  uint8_t verifyBuffer[3] = {};
  const AtomicUploadStatus beginStatus = BookUpload::begin(upload, io, path, declaredSize);
  if (beginStatus != AtomicUploadStatus::Ok) return beginStatus;
  const AtomicUploadStatus writeStatus = BookUpload::write(upload, io, bytes.data(), bytes.size());
  if (writeStatus != AtomicUploadStatus::Ok) return writeStatus;
  return BookUpload::finish(upload, io, bytes.size(), verifyBuffer, sizeof(verifyBuffer),
                            {&fs, &MemoryUploadIo::committed});
}

TEST(TestSha256Test, MatchesKnownAbcDigest) {
  alignas(TestSha256::Context) uint8_t workspace[sizeof(TestSha256::Context)]{};
  constexpr uint8_t expected[BookUpload::kAtomicUploadSha256Size] = {
      0xba, 0x78, 0x16, 0xbf, 0x8f, 0x01, 0xcf, 0xea, 0x41, 0x41, 0x40, 0xde, 0x5d, 0xae, 0x22, 0x23,
      0xb0, 0x03, 0x61, 0xa3, 0x96, 0x17, 0x7a, 0x9c, 0xb4, 0x10, 0xff, 0x61, 0xf2, 0x00, 0x15, 0xad,
  };
  uint8_t digest[BookUpload::kAtomicUploadSha256Size]{};
  constexpr uint8_t abc[] = {'a', 'b', 'c'};

  ASSERT_TRUE(TestSha256::start(workspace));
  ASSERT_TRUE(TestSha256::update(workspace, abc, 1));
  ASSERT_TRUE(TestSha256::update(workspace, abc + 1, 2));
  ASSERT_TRUE(TestSha256::finish(workspace, digest));
  EXPECT_EQ(memcmp(digest, expected, sizeof(expected)), 0);
}

TEST(AtomicBookUploadTest, UnknownAndKnownLengthUploadsPreserveExactBytes) {
  constexpr uint8_t payload[] = {0x00, 0x25, 0x50, 0x44, 0x46, 0xff, 0x0a};
  constexpr const char* httpPath = "/books/http.pdf";
  constexpr const char* wsPath = "/books/ws.pdf";
  uint8_t verifyBuffer[3] = {};
  MemoryUploadIo fs;
  AtomicUploadIo io = fs.callbacks();
  AtomicUploadState upload{};

  ASSERT_EQ(BookUpload::begin(upload, io, httpPath, BookUpload::kUnknownUploadSize), AtomicUploadStatus::Ok);
  ASSERT_EQ(BookUpload::write(upload, io, payload, 2), AtomicUploadStatus::Ok);
  ASSERT_EQ(BookUpload::write(upload, io, payload + 2, sizeof(payload) - 2), AtomicUploadStatus::Ok);
  ASSERT_EQ(BookUpload::finish(upload, io, sizeof(payload), verifyBuffer, sizeof(verifyBuffer),
                               {&fs, &MemoryUploadIo::committed}),
            AtomicUploadStatus::Ok);

  ASSERT_EQ(BookUpload::begin(upload, io, wsPath, sizeof(payload)), AtomicUploadStatus::Ok);
  ASSERT_EQ(BookUpload::write(upload, io, payload, sizeof(payload)), AtomicUploadStatus::Ok);
  ASSERT_EQ(BookUpload::finish(upload, io, sizeof(payload), verifyBuffer, sizeof(verifyBuffer),
                               {&fs, &MemoryUploadIo::committed}),
            AtomicUploadStatus::Ok);

  EXPECT_EQ(fs.files.at(httpPath), std::vector<uint8_t>(payload, payload + sizeof(payload)));
  EXPECT_EQ(fs.files.at(wsPath), std::vector<uint8_t>(payload, payload + sizeof(payload)));
  EXPECT_EQ(fs.committedCalls, 2U);
  EXPECT_EQ(fs.maximumOpenHandles, 1U);
}

TEST(AtomicBookUploadTest, AbortOfOverwritePreservesOriginalAndDerivedCache) {
  constexpr const char* path = "/books/existing.pdf";
  const std::vector<uint8_t> original = {'o', 'l', 'd'};
  const uint8_t partial[] = {'n', 'e'};
  MemoryUploadIo fs;
  fs.files[path] = original;
  AtomicUploadIo io = fs.callbacks();
  AtomicUploadState upload{};

  ASSERT_EQ(BookUpload::begin(upload, io, path, 3), AtomicUploadStatus::Ok);
  ASSERT_EQ(BookUpload::write(upload, io, partial, sizeof(partial)), AtomicUploadStatus::Ok);
  EXPECT_EQ(BookUpload::abort(upload, io), AtomicUploadStatus::Ok);

  EXPECT_EQ(fs.files.at(path), original);
  EXPECT_TRUE(fs.derivedCacheExists);
  EXPECT_EQ(fs.committedCalls, 0U);
  EXPECT_EQ(fs.files.find(artifactPath(path, ".crossink-upload.tmp")), fs.files.end());
  EXPECT_EQ(fs.files.find(artifactPath(path, ".crossink-upload.bak")), fs.files.end());
  EXPECT_EQ(fs.openHandles, 0U);
}

TEST(AtomicBookUploadTest, ZeroByteOverwriteCommitsOnlyAfterReadback) {
  constexpr const char* path = "/books/empty.pdf";
  MemoryUploadIo fs;
  fs.files[path] = {'o', 'l', 'd'};
  AtomicUploadState upload{};

  ASSERT_EQ(uploadBytes(fs, upload, path, {}, 0), AtomicUploadStatus::Ok);

  EXPECT_TRUE(fs.files.at(path).empty());
  EXPECT_FALSE(fs.derivedCacheExists);
  EXPECT_EQ(fs.committedCalls, 1U);
  EXPECT_EQ(fs.openHandles, 0U);
}

TEST(AtomicBookUploadTest, FreshTargetRenameFailAfterKeepsDurableMarkerThroughInvalidation) {
  constexpr const char* path = "/books/new.pdf";
  const std::vector<uint8_t> replacement = {'n', 'e', 'w'};
  MemoryUploadIo fs;
  fs.fault = MemoryUploadIo::Fault::RenamePromote;
  fs.failAfter = true;
  AtomicUploadState upload{};

  EXPECT_EQ(uploadBytes(fs, upload, path, replacement, replacement.size()), AtomicUploadStatus::Ok);
  EXPECT_EQ(fs.files.at(path), replacement);
  EXPECT_EQ(fs.committedCalls, 1U);
  EXPECT_TRUE(fs.markerPresentAtCommit);
  EXPECT_EQ(fs.files.find(artifactPath(path, ".crossink-upload.commit")), fs.files.end());
  EXPECT_EQ(fs.maximumOpenHandles, 1U);
}

TEST(AtomicBookUploadTest, FailedCommitHookRestoresOldCanonicalAndNeverReportsSuccess) {
  constexpr const char* path = "/books/existing.pdf";
  const std::vector<uint8_t> original = {'o', 'l', 'd'};
  const std::vector<uint8_t> replacement = {'n', 'e', 'w'};
  MemoryUploadIo fs;
  fs.files[path] = original;
  fs.commitSucceeds = false;
  AtomicUploadState upload{};

  EXPECT_EQ(uploadBytes(fs, upload, path, replacement, replacement.size()), AtomicUploadStatus::CommitHookFailed);
  EXPECT_EQ(fs.files.at(path), original);
  EXPECT_EQ(fs.committedCalls, 1U);
  EXPECT_TRUE(fs.markerPresentAtCommit);
  EXPECT_TRUE(fs.backupPresentAtCommit);
  EXPECT_TRUE(fs.derivedCacheExists);
  EXPECT_EQ(fs.files.find(artifactPath(path, ".crossink-upload.commit")), fs.files.end());
  EXPECT_EQ(fs.files.find(artifactPath(path, ".crossink-upload.bak")), fs.files.end());
  EXPECT_EQ(fs.maximumOpenHandles, 1U);
}

TEST(AtomicBookUploadTest, FailedFreshCommitRetainsMarkerUntilRollbackCanComplete) {
  constexpr const char* path = "/books/new.pdf";
  const std::vector<uint8_t> replacement = {'n', 'e', 'w'};
  const std::string marker = artifactPath(path, ".crossink-upload.commit");
  MemoryUploadIo fs;
  fs.commitSucceeds = false;
  fs.fault = MemoryUploadIo::Fault::RemoveTarget;
  AtomicUploadState upload{};

  EXPECT_EQ(uploadBytes(fs, upload, path, replacement, replacement.size()), AtomicUploadStatus::CleanupFailed);
  EXPECT_EQ(fs.files.at(path), replacement);
  EXPECT_NE(fs.files.find(marker), fs.files.end());
  EXPECT_EQ(fs.committedCalls, 1U);
  EXPECT_TRUE(fs.derivedCacheExists);

  fs.fault = MemoryUploadIo::Fault::None;
  fs.faultConsumed = false;
  AtomicUploadIo io = fs.callbacks();
  ASSERT_EQ(BookUpload::begin(upload, io, path, replacement.size()), AtomicUploadStatus::Ok);
  EXPECT_EQ(fs.files.find(path), fs.files.end());
  EXPECT_EQ(fs.files.find(marker), fs.files.end());
  EXPECT_EQ(BookUpload::abort(upload, io), AtomicUploadStatus::Ok);
  EXPECT_EQ(fs.maximumOpenHandles, 1U);
}

TEST(AtomicBookUploadTest, Sha256ReadbackRejectsKnownFnv32Collision) {
  constexpr const char* path = "/books/existing.pdf";
  const std::vector<uint8_t> original = {'o', 'l', 'd'};
  const std::vector<uint8_t> uploaded = {0x42, 0xf6, 0xc6, 0x68, 0x4d, 0x7f, 0xc0, 0xa0};
  const std::vector<uint8_t> readBack = {0x35, 0xa9, 0xb4, 0xf6, 0xa4, 0x6c, 0x8e, 0x34};
  ASSERT_EQ(legacyFnv32(uploaded), legacyFnv32(readBack));
  ASSERT_NE(uploaded, readBack);
  MemoryUploadIo fs;
  fs.files[path] = original;
  fs.forcedReadback = readBack;
  AtomicUploadState upload{};

  EXPECT_EQ(uploadBytes(fs, upload, path, uploaded, uploaded.size()), AtomicUploadStatus::VerificationFailed);
  EXPECT_EQ(fs.files.at(path), original);
  EXPECT_EQ(fs.committedCalls, 0U);
  EXPECT_TRUE(fs.derivedCacheExists);
  EXPECT_EQ(fs.maximumOpenHandles, 1U);
}

TEST(AtomicBookUploadTest, MissingSha256CallbacksAreRejectedBeforeFilesystemMutation) {
  constexpr const char* path = "/books/existing.pdf";
  const std::vector<uint8_t> original = {'o', 'l', 'd'};
  MemoryUploadIo fs;
  fs.files[path] = original;
  const AtomicUploadIo valid = fs.callbacks();

  for (int missing = 0; missing < 6; ++missing) {
    SCOPED_TRACE(missing);
    AtomicUploadIo io = valid;
    if (missing == 0) io.sha256.contextSize = 0;
    if (missing == 1) io.sha256.contextSize = BookUpload::kAtomicUploadSha256ContextCapacity + 1;
    if (missing == 2) io.sha256.start = nullptr;
    if (missing == 3) io.sha256.update = nullptr;
    if (missing == 4) io.sha256.finish = nullptr;
    if (missing == 5) io.sha256.abort = nullptr;
    AtomicUploadState upload{};
    const size_t operationsBefore = fs.operations.size();

    EXPECT_EQ(BookUpload::begin(upload, io, path, 3), AtomicUploadStatus::InvalidArgument);
    EXPECT_EQ(fs.operations.size(), operationsBefore);
    EXPECT_EQ(fs.files.at(path), original);
    EXPECT_EQ(fs.openHandles, 0U);
  }
}

TEST(AtomicBookUploadTest, BusyAdmissionClearsPriorHttpSuccessAndPreservesActiveWsTransaction) {
  constexpr const char* wsPath = "/books/ws.pdf";
  MemoryUploadIo fs;
  AtomicUploadIo io = fs.callbacks();
  AtomicUploadState upload{};
  ASSERT_EQ(BookUpload::begin(upload, io, wsPath, 1), AtomicUploadStatus::Ok);
  BookUpload::UploadTransportResponse priorHttpResponse{true, false};

  EXPECT_EQ(BookUpload::admitTransportStart(BookUpload::isActive(upload), priorHttpResponse), AtomicUploadStatus::Busy);
  EXPECT_FALSE(priorHttpResponse.success);
  EXPECT_TRUE(priorHttpResponse.hasError);
  EXPECT_EQ(BookUpload::httpResponseStatus(priorHttpResponse), 400);
  EXPECT_EQ(BookUpload::httpResponseStatus(BookUpload::UploadTransportResponse{true, true}), 400);
  EXPECT_TRUE(BookUpload::isActive(upload));
  EXPECT_EQ(fs.openHandles, 1U);

  EXPECT_EQ(BookUpload::abort(upload, io), AtomicUploadStatus::Ok);
  EXPECT_EQ(fs.openHandles, 0U);
}

TEST(AtomicBookUploadTest, SizeMismatchAndReadbackCorruptionNeverReplaceOriginal) {
  constexpr const char* path = "/books/existing.pdf";
  const std::vector<uint8_t> original = {'o', 'l', 'd'};
  const uint8_t replacement[] = {'n', 'e', 'w'};
  uint8_t verifyBuffer[2] = {};
  MemoryUploadIo fs;
  fs.files[path] = original;
  AtomicUploadIo io = fs.callbacks();
  AtomicUploadState upload{};

  ASSERT_EQ(BookUpload::begin(upload, io, path, 2), AtomicUploadStatus::Ok);
  EXPECT_EQ(BookUpload::write(upload, io, replacement, sizeof(replacement)), AtomicUploadStatus::SizeMismatch);
  EXPECT_EQ(fs.files.at(path), original);
  EXPECT_TRUE(fs.derivedCacheExists);

  fs.fault = MemoryUploadIo::Fault::CorruptRead;
  fs.faultConsumed = false;
  ASSERT_EQ(BookUpload::begin(upload, io, path, sizeof(replacement)), AtomicUploadStatus::Ok);
  ASSERT_EQ(BookUpload::write(upload, io, replacement, sizeof(replacement)), AtomicUploadStatus::Ok);
  EXPECT_EQ(BookUpload::finish(upload, io, sizeof(replacement), verifyBuffer, sizeof(verifyBuffer),
                               {&fs, &MemoryUploadIo::committed}),
            AtomicUploadStatus::VerificationFailed);
  EXPECT_EQ(fs.files.at(path), original);
  EXPECT_TRUE(fs.derivedCacheExists);
  EXPECT_EQ(fs.committedCalls, 0U);
}

struct FaultCase {
  MemoryUploadIo::Fault fault;
  bool failAfter;
  AtomicUploadStatus expected;
};

class AtomicBookUploadIoFaultTest : public testing::TestWithParam<FaultCase> {};

TEST_P(AtomicBookUploadIoFaultTest, FailedReplacementPreservesOriginalAndCanRetry) {
  constexpr const char* path = "/books/existing.pdf";
  const std::vector<uint8_t> original = {'o', 'l', 'd'};
  const std::vector<uint8_t> replacement = {'n', 'e', 'w'};
  MemoryUploadIo fs;
  fs.files[path] = original;
  fs.fault = GetParam().fault;
  fs.failAfter = GetParam().failAfter;
  AtomicUploadState upload{};

  EXPECT_EQ(uploadBytes(fs, upload, path, replacement, replacement.size()), GetParam().expected);
  EXPECT_EQ(fs.files.at(path), original);
  EXPECT_EQ(fs.openHandles, 0U);
  EXPECT_LE(fs.maximumOpenHandles, 1U);
  if (GetParam().fault == MemoryUploadIo::Fault::RemoveBackup) {
    EXPECT_EQ(fs.committedCalls, 1U);
    EXPECT_FALSE(fs.derivedCacheExists);
  } else {
    EXPECT_EQ(fs.committedCalls, 0U);
    EXPECT_TRUE(fs.derivedCacheExists);
  }

  fs.fault = MemoryUploadIo::Fault::None;
  fs.faultConsumed = false;
  ASSERT_EQ(uploadBytes(fs, upload, path, replacement, replacement.size()), AtomicUploadStatus::Ok);
  EXPECT_EQ(fs.files.at(path), replacement);
  EXPECT_EQ(fs.committedCalls, GetParam().fault == MemoryUploadIo::Fault::RemoveBackup ? 2U : 1U);
  EXPECT_EQ(fs.openHandles, 0U);
  EXPECT_EQ(fs.files.find(artifactPath(path, ".crossink-upload.tmp")), fs.files.end());
  EXPECT_EQ(fs.files.find(artifactPath(path, ".crossink-upload.bak")), fs.files.end());
}

INSTANTIATE_TEST_SUITE_P(
    EveryStreamingAndPromotionFault, AtomicBookUploadIoFaultTest,
    testing::Values(FaultCase{MemoryUploadIo::Fault::OpenWrite, false, AtomicUploadStatus::OpenWriteFailed},
                    FaultCase{MemoryUploadIo::Fault::ShortWrite, false, AtomicUploadStatus::WriteFailed},
                    FaultCase{MemoryUploadIo::Fault::Flush, false, AtomicUploadStatus::FlushFailed},
                    FaultCase{MemoryUploadIo::Fault::Sync, false, AtomicUploadStatus::SyncFailed},
                    FaultCase{MemoryUploadIo::Fault::CloseWrite, false, AtomicUploadStatus::CloseFailed},
                    FaultCase{MemoryUploadIo::Fault::OpenRead, false, AtomicUploadStatus::OpenReadFailed},
                    FaultCase{MemoryUploadIo::Fault::Read, false, AtomicUploadStatus::ReadFailed},
                    FaultCase{MemoryUploadIo::Fault::CloseRead, false, AtomicUploadStatus::CloseFailed},
                    FaultCase{MemoryUploadIo::Fault::RenameBackup, false, AtomicUploadStatus::BackupFailed},
                    FaultCase{MemoryUploadIo::Fault::RenamePromote, false, AtomicUploadStatus::PromotionFailed},
                    FaultCase{MemoryUploadIo::Fault::RemoveBackup, false, AtomicUploadStatus::CleanupFailed}));

TEST(AtomicBookUploadTest, RenameFailAfterMutationIsAcceptedForBackupAndPromotion) {
  constexpr const char* path = "/books/existing.pdf";
  const std::vector<uint8_t> original = {'o', 'l', 'd'};
  const std::vector<uint8_t> replacement = {'n', 'e', 'w'};
  for (const MemoryUploadIo::Fault fault :
       {MemoryUploadIo::Fault::RenameBackup, MemoryUploadIo::Fault::RenamePromote}) {
    SCOPED_TRACE(static_cast<int>(fault));
    MemoryUploadIo fs;
    fs.files[path] = original;
    fs.fault = fault;
    fs.failAfter = true;
    AtomicUploadState upload{};

    EXPECT_EQ(uploadBytes(fs, upload, path, replacement, replacement.size()), AtomicUploadStatus::Ok);
    EXPECT_EQ(fs.files.at(path), replacement);
    EXPECT_EQ(fs.committedCalls, 1U);
    EXPECT_TRUE(fs.markerPresentAtCommit);
    EXPECT_TRUE(fs.backupPresentAtCommit);
    EXPECT_EQ(fs.files.find(artifactPath(path, ".crossink-upload.commit")), fs.files.end());
    EXPECT_EQ(fs.files.find(artifactPath(path, ".crossink-upload.bak")), fs.files.end());
    EXPECT_EQ(fs.maximumOpenHandles, 1U);
  }
}

struct MarkerFaultCase {
  MemoryUploadIo::Fault fault;
  bool failAfter;
};

class AtomicBookUploadMarkerFaultTest : public testing::TestWithParam<MarkerFaultCase> {};

TEST_P(AtomicBookUploadMarkerFaultTest, MarkerFailurePreservesOriginalAndRetryIsSafe) {
  constexpr const char* path = "/books/existing.pdf";
  const std::vector<uint8_t> original = {'o', 'l', 'd'};
  const std::vector<uint8_t> replacement = {'n', 'e', 'w'};
  MemoryUploadIo fs;
  fs.files[path] = original;
  fs.fault = GetParam().fault;
  fs.failAfter = GetParam().failAfter;
  AtomicUploadState upload{};

  const AtomicUploadStatus status = uploadBytes(fs, upload, path, replacement, replacement.size());
  const bool markerRemovalApplied = GetParam().fault == MemoryUploadIo::Fault::RemoveMarker && GetParam().failAfter;
  if (markerRemovalApplied) {
    EXPECT_EQ(status, AtomicUploadStatus::Ok);
    EXPECT_EQ(fs.files.at(path), replacement);
  } else {
    EXPECT_EQ(status, GetParam().fault == MemoryUploadIo::Fault::RemoveMarker ? AtomicUploadStatus::CleanupFailed
                                                                              : AtomicUploadStatus::MarkerFailed);
    EXPECT_EQ(fs.files.at(path), original);
  }
  EXPECT_EQ(fs.openHandles, 0U);
  EXPECT_LE(fs.maximumOpenHandles, 1U);

  fs.fault = MemoryUploadIo::Fault::None;
  fs.faultConsumed = false;
  ASSERT_EQ(uploadBytes(fs, upload, path, replacement, replacement.size()), AtomicUploadStatus::Ok);
  EXPECT_EQ(fs.files.at(path), replacement);
  EXPECT_EQ(fs.files.find(artifactPath(path, ".crossink-upload.commit")), fs.files.end());
  EXPECT_EQ(fs.files.find(artifactPath(path, ".crossink-upload.bak")), fs.files.end());
  EXPECT_EQ(fs.openHandles, 0U);
}

INSTANTIATE_TEST_SUITE_P(EveryMarkerIoFailure, AtomicBookUploadMarkerFaultTest,
                         testing::Values(MarkerFaultCase{MemoryUploadIo::Fault::MarkerOpenWrite, false},
                                         MarkerFaultCase{MemoryUploadIo::Fault::MarkerOpenWrite, true},
                                         MarkerFaultCase{MemoryUploadIo::Fault::MarkerWrite, false},
                                         MarkerFaultCase{MemoryUploadIo::Fault::MarkerWrite, true},
                                         MarkerFaultCase{MemoryUploadIo::Fault::MarkerFlush, false},
                                         MarkerFaultCase{MemoryUploadIo::Fault::MarkerFlush, true},
                                         MarkerFaultCase{MemoryUploadIo::Fault::MarkerSync, false},
                                         MarkerFaultCase{MemoryUploadIo::Fault::MarkerSync, true},
                                         MarkerFaultCase{MemoryUploadIo::Fault::MarkerCloseWrite, false},
                                         MarkerFaultCase{MemoryUploadIo::Fault::MarkerCloseWrite, true},
                                         MarkerFaultCase{MemoryUploadIo::Fault::MarkerOpenRead, false},
                                         MarkerFaultCase{MemoryUploadIo::Fault::MarkerOpenRead, true},
                                         MarkerFaultCase{MemoryUploadIo::Fault::MarkerRead, false},
                                         MarkerFaultCase{MemoryUploadIo::Fault::MarkerRead, true},
                                         MarkerFaultCase{MemoryUploadIo::Fault::MarkerCloseRead, false},
                                         MarkerFaultCase{MemoryUploadIo::Fault::MarkerCloseRead, true},
                                         MarkerFaultCase{MemoryUploadIo::Fault::MarkerCorruptRead, false},
                                         MarkerFaultCase{MemoryUploadIo::Fault::RemoveMarker, false},
                                         MarkerFaultCase{MemoryUploadIo::Fault::RemoveMarker, true}));

TEST(AtomicBookUploadTest, FailedAbortCleanupIsRecoveredWithoutDeletingOriginal) {
  constexpr const char* path = "/books/existing.pdf";
  const std::vector<uint8_t> original = {'o', 'l', 'd'};
  const uint8_t partial[] = {'n'};
  MemoryUploadIo fs;
  fs.files[path] = original;
  fs.fault = MemoryUploadIo::Fault::RemoveTemp;
  AtomicUploadIo io = fs.callbacks();
  AtomicUploadState upload{};

  ASSERT_EQ(BookUpload::begin(upload, io, path, 2), AtomicUploadStatus::Ok);
  ASSERT_EQ(BookUpload::write(upload, io, partial, sizeof(partial)), AtomicUploadStatus::Ok);
  EXPECT_EQ(BookUpload::abort(upload, io), AtomicUploadStatus::CleanupFailed);
  EXPECT_EQ(fs.files.at(path), original);
  EXPECT_NE(fs.files.find(artifactPath(path, ".crossink-upload.tmp")), fs.files.end());

  fs.fault = MemoryUploadIo::Fault::None;
  fs.faultConsumed = false;
  EXPECT_EQ(uploadBytes(fs, upload, path, {'n', 'e'}, 2), AtomicUploadStatus::Ok);
  EXPECT_EQ(fs.files.at(path), (std::vector<uint8_t>{'n', 'e'}));
}

TEST(AtomicBookUploadTest, RebootRecoveryPrefersBackupAndNeverDeletesOnlyValidCopy) {
  constexpr const char* path = "/books/existing.pdf";
  const std::vector<uint8_t> original = {'o', 'l', 'd'};
  const std::vector<uint8_t> replacement = {'n', 'e', 'w'};
  const std::string temporary = artifactPath(path, ".crossink-upload.tmp");
  const std::string backup = artifactPath(path, ".crossink-upload.bak");

  for (int state = 0; state < 4; ++state) {
    SCOPED_TRACE(state);
    MemoryUploadIo fs;
    if (state == 0) {
      fs.files[path] = original;
      fs.files[temporary] = replacement;
    } else if (state == 1) {
      fs.files[backup] = original;
      fs.files[temporary] = replacement;
    } else if (state == 2) {
      fs.files[path] = replacement;
      fs.files[backup] = original;
    } else {
      fs.files[backup] = original;
    }
    AtomicUploadState upload{};
    AtomicUploadIo io = fs.callbacks();

    ASSERT_EQ(BookUpload::begin(upload, io, path, replacement.size()), AtomicUploadStatus::Ok);
    EXPECT_EQ(fs.files.at(path), original);
    EXPECT_EQ(fs.files.find(backup), fs.files.end());
    EXPECT_NE(fs.files.find(temporary), fs.files.end());
    EXPECT_EQ(BookUpload::abort(upload, io), AtomicUploadStatus::Ok);
    EXPECT_EQ(fs.files.at(path), original);
    EXPECT_EQ(fs.files.find(temporary), fs.files.end());
  }
}

TEST(AtomicBookUploadTest, RecoveryFailureLeavesOldBytesInTargetOrBackupForRetry) {
  constexpr const char* path = "/books/existing.pdf";
  const std::vector<uint8_t> original = {'o', 'l', 'd'};
  const std::string backup = artifactPath(path, ".crossink-upload.bak");
  MemoryUploadIo fs;
  fs.files[backup] = original;
  fs.fault = MemoryUploadIo::Fault::RenameRestore;
  AtomicUploadState upload{};
  AtomicUploadIo io = fs.callbacks();

  EXPECT_EQ(BookUpload::begin(upload, io, path, 3), AtomicUploadStatus::RecoveryFailed);
  EXPECT_EQ(fs.files.find(path), fs.files.end());
  EXPECT_EQ(fs.files.at(backup), original);

  fs.fault = MemoryUploadIo::Fault::None;
  fs.faultConsumed = false;
  EXPECT_EQ(BookUpload::begin(upload, io, path, 3), AtomicUploadStatus::Ok);
  EXPECT_EQ(fs.files.at(path), original);
  EXPECT_EQ(BookUpload::abort(upload, io), AtomicUploadStatus::Ok);
}

TEST(AtomicBookUploadTest, RecoveryRemoveFaultNeverDeletesBothCopiesAndFailAfterIsObserved) {
  constexpr const char* path = "/books/existing.pdf";
  const std::vector<uint8_t> original = {'o', 'l', 'd'};
  const std::vector<uint8_t> replacement = {'n', 'e', 'w'};
  const std::string backup = artifactPath(path, ".crossink-upload.bak");

  MemoryUploadIo failBefore;
  failBefore.files[path] = replacement;
  failBefore.files[backup] = original;
  failBefore.fault = MemoryUploadIo::Fault::RemoveTarget;
  AtomicUploadState beforeUpload{};
  AtomicUploadIo beforeIo = failBefore.callbacks();
  EXPECT_EQ(BookUpload::begin(beforeUpload, beforeIo, path, replacement.size()), AtomicUploadStatus::RecoveryFailed);
  EXPECT_EQ(failBefore.files.at(path), replacement);
  EXPECT_EQ(failBefore.files.at(backup), original);
  failBefore.fault = MemoryUploadIo::Fault::None;
  failBefore.faultConsumed = false;
  ASSERT_EQ(BookUpload::begin(beforeUpload, beforeIo, path, replacement.size()), AtomicUploadStatus::Ok);
  EXPECT_EQ(failBefore.files.at(path), original);
  EXPECT_EQ(BookUpload::abort(beforeUpload, beforeIo), AtomicUploadStatus::Ok);

  MemoryUploadIo failAfter;
  failAfter.files[path] = replacement;
  failAfter.files[backup] = original;
  failAfter.fault = MemoryUploadIo::Fault::RemoveTarget;
  failAfter.failAfter = true;
  AtomicUploadState afterUpload{};
  AtomicUploadIo afterIo = failAfter.callbacks();
  ASSERT_EQ(BookUpload::begin(afterUpload, afterIo, path, replacement.size()), AtomicUploadStatus::Ok);
  EXPECT_EQ(failAfter.files.at(path), original);
  EXPECT_EQ(failAfter.files.find(backup), failAfter.files.end());
  EXPECT_EQ(BookUpload::abort(afterUpload, afterIo), AtomicUploadStatus::Ok);
}

TEST(AtomicBookUploadTest, FailAfterArtifactRemovalIsAcceptedOnlyWhenArtifactIsGone) {
  constexpr const char* path = "/books/existing.pdf";
  const std::vector<uint8_t> original = {'o', 'l', 'd'};
  const std::vector<uint8_t> replacement = {'n', 'e', 'w'};

  MemoryUploadIo abortFs;
  abortFs.files[path] = original;
  AtomicUploadState abortUpload{};
  AtomicUploadIo abortIo = abortFs.callbacks();
  ASSERT_EQ(BookUpload::begin(abortUpload, abortIo, path, replacement.size()), AtomicUploadStatus::Ok);
  ASSERT_EQ(BookUpload::write(abortUpload, abortIo, replacement.data(), 1), AtomicUploadStatus::Ok);
  abortFs.fault = MemoryUploadIo::Fault::RemoveTemp;
  abortFs.failAfter = true;
  EXPECT_EQ(BookUpload::abort(abortUpload, abortIo), AtomicUploadStatus::Ok);
  EXPECT_EQ(abortFs.files.at(path), original);
  EXPECT_EQ(abortFs.files.find(artifactPath(path, ".crossink-upload.tmp")), abortFs.files.end());

  MemoryUploadIo commitFs;
  commitFs.files[path] = original;
  commitFs.fault = MemoryUploadIo::Fault::RemoveBackup;
  commitFs.failAfter = true;
  AtomicUploadState commitUpload{};
  EXPECT_EQ(uploadBytes(commitFs, commitUpload, path, replacement, replacement.size()), AtomicUploadStatus::Ok);
  EXPECT_EQ(commitFs.files.at(path), replacement);
  EXPECT_EQ(commitFs.committedCalls, 1U);
  EXPECT_EQ(commitFs.files.find(artifactPath(path, ".crossink-upload.bak")), commitFs.files.end());
}

TEST(AtomicBookUploadTest, InvalidationRunsWhileBackupStillMakesPowerLossRecoverable) {
  constexpr const char* path = "/books/existing.pdf";
  const std::vector<uint8_t> original = {'o', 'l', 'd'};
  const std::vector<uint8_t> replacement = {'n', 'e', 'w'};
  const std::string backup = artifactPath(path, ".crossink-upload.bak");
  const std::string marker = artifactPath(path, ".crossink-upload.commit");
  MemoryUploadIo committedFs;
  committedFs.files[path] = original;
  AtomicUploadState committedUpload{};

  ASSERT_EQ(uploadBytes(committedFs, committedUpload, path, replacement, replacement.size()), AtomicUploadStatus::Ok);
  EXPECT_TRUE(committedFs.backupPresentAtCommit);
  EXPECT_TRUE(committedFs.markerPresentAtCommit);
  EXPECT_FALSE(committedFs.tempPresentAtCommit);
  EXPECT_EQ(committedFs.targetBytesAtCommit, replacement);
  EXPECT_FALSE(committedFs.derivedCacheExists);
  EXPECT_EQ(committedFs.files.find(backup), committedFs.files.end());

  // Power loss immediately before the hook: promotion is visible, backup is
  // durable, and old derived cache still matches the recoverable old source.
  MemoryUploadIo beforeHook;
  beforeHook.files[path] = replacement;
  beforeHook.files[backup] = original;
  beforeHook.files[marker] = committedFs.capturedMarker;
  AtomicUploadState beforeUpload{};
  AtomicUploadIo beforeIo = beforeHook.callbacks();
  ASSERT_EQ(BookUpload::begin(beforeUpload, beforeIo, path, replacement.size()), AtomicUploadStatus::Ok);
  EXPECT_EQ(beforeHook.files.at(path), original);
  EXPECT_EQ(beforeHook.files.find(marker), beforeHook.files.end());
  EXPECT_TRUE(beforeHook.derivedCacheExists);
  EXPECT_EQ(BookUpload::abort(beforeUpload, beforeIo), AtomicUploadStatus::Ok);

  // Power loss immediately after the hook sees the same recoverable files, but
  // derived cache is already invalidated. Recovery can return to old bytes and
  // an absent cache is safe to rebuild.
  MemoryUploadIo afterHook;
  afterHook.files[path] = replacement;
  afterHook.files[backup] = original;
  afterHook.files[marker] = committedFs.capturedMarker;
  afterHook.derivedCacheExists = false;
  AtomicUploadState afterUpload{};
  AtomicUploadIo afterIo = afterHook.callbacks();
  ASSERT_EQ(BookUpload::begin(afterUpload, afterIo, path, replacement.size()), AtomicUploadStatus::Ok);
  EXPECT_EQ(afterHook.files.at(path), original);
  EXPECT_EQ(afterHook.files.find(marker), afterHook.files.end());
  EXPECT_FALSE(afterHook.derivedCacheExists);
  EXPECT_EQ(BookUpload::abort(afterUpload, afterIo), AtomicUploadStatus::Ok);
}

TEST(AtomicBookUploadTest, FreshZeroBytePromotionMarkerMakesRebootRollbackUnambiguous) {
  constexpr const char* path = "/books/new-empty.pdf";
  const std::string marker = artifactPath(path, ".crossink-upload.commit");
  MemoryUploadIo donor;
  AtomicUploadState donorUpload{};
  ASSERT_EQ(uploadBytes(donor, donorUpload, path, {}, 0), AtomicUploadStatus::Ok);
  ASSERT_EQ(donor.capturedMarker.size(), BookUpload::kAtomicUploadCommitMarkerSize);

  MemoryUploadIo rebooted;
  rebooted.files[path] = {};
  rebooted.files[marker] = donor.capturedMarker;
  AtomicUploadIo io = rebooted.callbacks();
  AtomicUploadState upload{};

  ASSERT_EQ(BookUpload::begin(upload, io, path, 1), AtomicUploadStatus::Ok);
  EXPECT_EQ(rebooted.files.find(path), rebooted.files.end());
  EXPECT_EQ(rebooted.files.find(marker), rebooted.files.end());
  EXPECT_TRUE(BookUpload::isActive(upload));
  EXPECT_EQ(BookUpload::abort(upload, io), AtomicUploadStatus::Ok);
  EXPECT_EQ(rebooted.maximumOpenHandles, 1U);
}

TEST(AtomicBookUploadTest, TruncatedOrCorruptMarkerFailsClosedWithEveryRecoverableCopyIntact) {
  constexpr const char* path = "/books/existing.pdf";
  const std::vector<uint8_t> original = {'o', 'l', 'd'};
  const std::vector<uint8_t> replacement = {'n', 'e', 'w'};
  const std::string backup = artifactPath(path, ".crossink-upload.bak");
  const std::string marker = artifactPath(path, ".crossink-upload.commit");
  MemoryUploadIo donor;
  donor.files[path] = original;
  AtomicUploadState donorUpload{};
  ASSERT_EQ(uploadBytes(donor, donorUpload, path, replacement, replacement.size()), AtomicUploadStatus::Ok);
  ASSERT_FALSE(donor.capturedMarker.empty());

  for (const bool truncate : {false, true}) {
    SCOPED_TRACE(truncate);
    MemoryUploadIo rebooted;
    rebooted.files[path] = replacement;
    rebooted.files[backup] = original;
    rebooted.files[marker] = donor.capturedMarker;
    if (truncate) {
      rebooted.files[marker].pop_back();
    } else {
      rebooted.files[marker][23] ^= 0x40U;
    }
    AtomicUploadIo io = rebooted.callbacks();
    AtomicUploadState upload{};

    EXPECT_EQ(BookUpload::begin(upload, io, path, replacement.size()), AtomicUploadStatus::RecoveryFailed);
    EXPECT_EQ(rebooted.files.at(path), replacement);
    EXPECT_EQ(rebooted.files.at(backup), original);
    EXPECT_NE(rebooted.files.find(marker), rebooted.files.end());
    EXPECT_EQ(rebooted.openHandles, 0U);
    EXPECT_LE(rebooted.maximumOpenHandles, 1U);
  }
}

TEST(AtomicBookUploadTest, TargetSpecificMarkersRecoverIndependentlyWhenTwoArtifactsCoexist) {
  constexpr const char* firstPath = "/books/a.pdf";
  constexpr const char* secondPath = "/books/b.pdf";
  const std::vector<uint8_t> firstOld = {'a', '0'};
  const std::vector<uint8_t> firstNew = {'a', '1'};
  const std::vector<uint8_t> secondOld = {'b', '0'};
  const std::vector<uint8_t> secondNew = {'b', '1'};

  MemoryUploadIo firstDonor;
  firstDonor.files[firstPath] = firstOld;
  AtomicUploadState firstDonorUpload{};
  ASSERT_EQ(uploadBytes(firstDonor, firstDonorUpload, firstPath, firstNew, firstNew.size()), AtomicUploadStatus::Ok);
  MemoryUploadIo secondDonor;
  secondDonor.files[secondPath] = secondOld;
  AtomicUploadState secondDonorUpload{};
  ASSERT_EQ(uploadBytes(secondDonor, secondDonorUpload, secondPath, secondNew, secondNew.size()),
            AtomicUploadStatus::Ok);
  ASSERT_NE(firstDonor.capturedMarker, secondDonor.capturedMarker);

  const std::string firstBackup = artifactPath(firstPath, ".crossink-upload.bak");
  const std::string firstMarker = artifactPath(firstPath, ".crossink-upload.commit");
  const std::string secondBackup = artifactPath(secondPath, ".crossink-upload.bak");
  const std::string secondMarker = artifactPath(secondPath, ".crossink-upload.commit");
  MemoryUploadIo rebooted;
  rebooted.files[firstPath] = firstNew;
  rebooted.files[firstBackup] = firstOld;
  rebooted.files[firstMarker] = firstDonor.capturedMarker;
  rebooted.files[secondPath] = secondNew;
  rebooted.files[secondBackup] = secondOld;
  rebooted.files[secondMarker] = secondDonor.capturedMarker;
  AtomicUploadIo io = rebooted.callbacks();
  AtomicUploadState upload{};

  ASSERT_EQ(BookUpload::begin(upload, io, firstPath, 1), AtomicUploadStatus::Ok);
  EXPECT_EQ(rebooted.files.at(firstPath), firstOld);
  EXPECT_EQ(rebooted.files.find(firstMarker), rebooted.files.end());
  EXPECT_EQ(rebooted.files.at(secondPath), secondNew);
  EXPECT_EQ(rebooted.files.at(secondBackup), secondOld);
  EXPECT_NE(rebooted.files.find(secondMarker), rebooted.files.end());
  ASSERT_EQ(BookUpload::abort(upload, io), AtomicUploadStatus::Ok);

  ASSERT_EQ(BookUpload::begin(upload, io, secondPath, 1), AtomicUploadStatus::Ok);
  EXPECT_EQ(rebooted.files.at(secondPath), secondOld);
  EXPECT_EQ(rebooted.files.find(secondMarker), rebooted.files.end());
  EXPECT_EQ(BookUpload::abort(upload, io), AtomicUploadStatus::Ok);
  EXPECT_EQ(rebooted.maximumOpenHandles, 1U);
}

struct MarkerRecoveryFaultCase {
  MemoryUploadIo::Fault fault;
  bool failAfter;
};

class AtomicBookUploadMarkerRecoveryFaultTest : public testing::TestWithParam<MarkerRecoveryFaultCase> {};

TEST_P(AtomicBookUploadMarkerRecoveryFaultTest, ReadFailureLeavesMarkerTargetAndBackupForNextBoot) {
  constexpr const char* path = "/books/existing.pdf";
  const std::vector<uint8_t> original = {'o', 'l', 'd'};
  const std::vector<uint8_t> replacement = {'n', 'e', 'w'};
  MemoryUploadIo donor;
  donor.files[path] = original;
  AtomicUploadState donorUpload{};
  ASSERT_EQ(uploadBytes(donor, donorUpload, path, replacement, replacement.size()), AtomicUploadStatus::Ok);

  const std::string backup = artifactPath(path, ".crossink-upload.bak");
  const std::string marker = artifactPath(path, ".crossink-upload.commit");
  MemoryUploadIo rebooted;
  rebooted.files[path] = replacement;
  rebooted.files[backup] = original;
  rebooted.files[marker] = donor.capturedMarker;
  rebooted.fault = GetParam().fault;
  rebooted.failAfter = GetParam().failAfter;
  AtomicUploadIo io = rebooted.callbacks();
  AtomicUploadState upload{};

  EXPECT_EQ(BookUpload::begin(upload, io, path, replacement.size()), AtomicUploadStatus::RecoveryFailed);
  EXPECT_EQ(rebooted.files.at(path), replacement);
  EXPECT_EQ(rebooted.files.at(backup), original);
  EXPECT_NE(rebooted.files.find(marker), rebooted.files.end());
  EXPECT_EQ(rebooted.openHandles, 0U);
  EXPECT_LE(rebooted.maximumOpenHandles, 1U);

  rebooted.fault = MemoryUploadIo::Fault::None;
  rebooted.faultConsumed = false;
  ASSERT_EQ(BookUpload::begin(upload, io, path, replacement.size()), AtomicUploadStatus::Ok);
  EXPECT_EQ(rebooted.files.at(path), original);
  EXPECT_EQ(BookUpload::abort(upload, io), AtomicUploadStatus::Ok);
}

INSTANTIATE_TEST_SUITE_P(EveryMarkerRecoveryReadFailure, AtomicBookUploadMarkerRecoveryFaultTest,
                         testing::Values(MarkerRecoveryFaultCase{MemoryUploadIo::Fault::MarkerOpenRead, false},
                                         MarkerRecoveryFaultCase{MemoryUploadIo::Fault::MarkerOpenRead, true},
                                         MarkerRecoveryFaultCase{MemoryUploadIo::Fault::MarkerRead, false},
                                         MarkerRecoveryFaultCase{MemoryUploadIo::Fault::MarkerRead, true},
                                         MarkerRecoveryFaultCase{MemoryUploadIo::Fault::MarkerCloseRead, false},
                                         MarkerRecoveryFaultCase{MemoryUploadIo::Fault::MarkerCloseRead, true}));

TEST(AtomicBookUploadTest, BusyAndOverlongTargetsOpenNothingAndArtifactsAreTargetSpecific) {
  const std::string first = "/books/a.pdf";
  const std::string second = "/books/b.pdf";
  const std::string overlong(250, 'a');
  MemoryUploadIo fs;
  AtomicUploadState upload{};
  AtomicUploadIo io = fs.callbacks();

  ASSERT_NE(artifactPath(first.c_str(), ".crossink-upload.tmp"), artifactPath(second.c_str(), ".crossink-upload.tmp"));
  ASSERT_EQ(BookUpload::begin(upload, io, first.c_str(), 1), AtomicUploadStatus::Ok);
  EXPECT_EQ(BookUpload::begin(upload, io, first.c_str(), 1), AtomicUploadStatus::Busy);
  EXPECT_EQ(BookUpload::abort(upload, io), AtomicUploadStatus::Ok);

  const size_t operationsBefore = fs.operations.size();
  EXPECT_EQ(BookUpload::begin(upload, io, overlong.c_str(), 1), AtomicUploadStatus::PathTooLong);
  EXPECT_EQ(fs.operations.size(), operationsBefore);
}

}  // namespace
