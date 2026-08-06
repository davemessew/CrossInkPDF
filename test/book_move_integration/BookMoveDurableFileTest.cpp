#include <BookMoveDurableFile.h>
#include <HalStorage.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

namespace BookMoveDurableFile {
bool restoreCanonicalForRead(const char* canonical, const char* temporary, const char* backup);
}

namespace {

using BookMoveDurableFile::Payload;

constexpr char kCanonical[] = "/state.bin";
constexpr char kTemporary[] = "/state.bin.move.tmp";
constexpr char kBackup[] = "/state.bin.move.bak";
constexpr char kDeleteTemporary[] = "/state.bin.delete.tmp";
constexpr char kDeleteBackup[] = "/state.bin.delete.bak";

struct PayloadContext {
  std::vector<uint8_t> desired;
  size_t verificationCalls = 0;
  size_t failVerificationCall = 0;
};

int failures = 0;

void expect(const bool condition, const char* const message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

std::vector<uint8_t> bytes(const char* const text) {
  const auto* const begin = reinterpret_cast<const uint8_t*>(text);
  return {begin, begin + std::strlen(text)};
}

bool writePayload(void* const context, void* const fileContext) {
  const auto& payload = *static_cast<PayloadContext*>(context);
  auto& file = *static_cast<FsFile*>(fileContext);
  return file.write(payload.desired.data(), payload.desired.size()) == payload.desired.size();
}

bool verifyPayload(void* const context, const char* const path) {
  auto& payload = *static_cast<PayloadContext*>(context);
  ++payload.verificationCalls;
  if (payload.failVerificationCall == payload.verificationCalls) return false;

  HalFile file;
  if (!Storage.openFileForRead("MoveTest", path, file)) return false;
  std::vector<uint8_t> actual(payload.desired.size());
  const int read = file.read(actual.data(), actual.size());
  const bool exactSize = file.fileSize64() == payload.desired.size();
  const bool closed = file.close();
  return read == static_cast<int>(actual.size()) && exactSize && closed && actual == payload.desired;
}

Payload makePayload(PayloadContext& context) { return {&context, &writePayload, &verifyPayload}; }

void seedOldCanonical() { Storage.putFile(kCanonical, bytes("old-state")); }

void expectOldCanonical(const char* const message) {
  expect(Storage.exists(kCanonical) && Storage.bytes(kCanonical) == bytes("old-state"), message);
}

void testShortWriteSyncAndCloseFaultsKeepCanonical() {
  for (uint8_t fault = 0; fault < 3; ++fault) {
    Storage.reset();
    seedOldCanonical();
    PayloadContext context{bytes("new-state")};
    if (fault == 0) Storage.failNextShortWrite();
    if (fault == 1) Storage.failNextSync();
    if (fault == 2) {
      // Initial canonical verification closes once; fail the temporary close.
      Storage.failCloseOnCall(2);
    }

    expect(!BookMoveDurableFile::replace(kCanonical, kTemporary, kBackup, makePayload(context)),
           "durability fault must reject replacement");
    expectOldCanonical("durability fault must preserve the prior canonical");
    expect(!Storage.exists(kTemporary) && !Storage.exists(kBackup),
           "durability fault must leave no promoted transaction artifacts");
  }
}

void testReadbackFailureKeepsCanonical() {
  Storage.reset();
  seedOldCanonical();
  PayloadContext context{bytes("new-state")};
  context.failVerificationCall = 2;  // old canonical check, then temp readback

  expect(!BookMoveDurableFile::replace(kCanonical, kTemporary, kBackup, makePayload(context)),
         "temporary readback failure must reject replacement");
  expectOldCanonical("temporary readback failure must preserve prior canonical");
}

void testPromotionFailureRollsBackCanonical() {
  Storage.reset();
  seedOldCanonical();
  PayloadContext context{bytes("new-state")};
  Storage.failRenameOnCall(2);  // canonical->backup succeeds, temp->canonical fails

  expect(!BookMoveDurableFile::replace(kCanonical, kTemporary, kBackup, makePayload(context)),
         "promotion failure must be reported");
  expectOldCanonical("promotion failure must restore the prior canonical");
}

void testPowerLossArtifactsRecoverIdempotently() {
  Storage.reset();
  Storage.putFile(kTemporary, bytes("new-state"));
  Storage.putFile(kBackup, bytes("old-state"));
  PayloadContext context{bytes("new-state")};

  expect(BookMoveDurableFile::replace(kCanonical, kTemporary, kBackup, makePayload(context)),
         "retry must recover a crash between backup and promotion");
  expect(Storage.bytes(kCanonical) == bytes("new-state"), "retry must publish the desired canonical bytes");
  expect(!Storage.exists(kTemporary) && !Storage.exists(kBackup), "successful retry must clean transaction artifacts");

  Storage.putFile(kBackup, bytes("old-state"));
  expect(BookMoveDurableFile::replace(kCanonical, kTemporary, kBackup, makePayload(context)),
         "retry after promotion must recognize the desired canonical");
  expect(Storage.bytes(kCanonical) == bytes("new-state") && !Storage.exists(kBackup),
         "post-promotion retry must keep the new canonical and clean backup");
}

void testCorruptCanonicalIsReplacedAndOneHandleIsUsed() {
  Storage.reset();
  Storage.putFile(kCanonical, bytes("truncated"));
  PayloadContext context{bytes("new-state")};

  expect(BookMoveDurableFile::replace(kCanonical, kTemporary, kBackup, makePayload(context)),
         "verified source snapshot must replace a corrupt destination");
  expect(Storage.bytes(kCanonical) == bytes("new-state"), "corrupt destination must be replaced byte-exactly");
  expect(Storage.maximumFileHandles() == 1, "durable replacement must use at most one file handle");

  const auto flush = std::find(Storage.events.begin(), Storage.events.end(), std::string("flush:") + kTemporary);
  const auto sync = std::find(Storage.events.begin(), Storage.events.end(), std::string("sync:") + kTemporary);
  const auto close = std::find(Storage.events.begin(), Storage.events.end(), std::string("close:") + kTemporary);
  expect(flush != Storage.events.end() && sync != Storage.events.end() && close != Storage.events.end() &&
             flush < sync && sync < close,
         "temporary bytes must flush, sync, and close before readback/promotion");
}

void testDeleteArtifactRebootRestoresUnrelatedRecentBeforeReplay() {
  Storage.reset();
  Storage.putFile(kDeleteTemporary, bytes("unrelated"));
  Storage.putFile(kDeleteBackup, bytes("deleted-source,unrelated"));

  expect(BookMoveDurableFile::restoreCanonicalForRead(kCanonical, kDeleteTemporary, kDeleteBackup),
         "boot must restore the pre-delete recents snapshot before loading");
  expect(Storage.bytes(kCanonical) == bytes("deleted-source,unrelated"),
         "boot load must still see the unrelated recent and the replay target");
  expect(!Storage.exists(kDeleteTemporary) && !Storage.exists(kDeleteBackup),
         "restoring the pre-delete snapshot must clear partial delete artifacts");

  PayloadContext replay{bytes("unrelated")};
  expect(BookMoveDurableFile::replace(kCanonical, kDeleteTemporary, kDeleteBackup, makePayload(replay)),
         "pending delete replay must durably remove only its source");
  expect(Storage.bytes(kCanonical) == bytes("unrelated"),
         "pending delete replay must preserve unrelated recent entries");
}

}  // namespace

int main() {
  testShortWriteSyncAndCloseFaultsKeepCanonical();
  testReadbackFailureKeepsCanonical();
  testPromotionFailureRollsBackCanonical();
  testPowerLossArtifactsRecoverIdempotently();
  testCorruptCanonicalIsReplacedAndOneHandleIsUsed();
  testDeleteArtifactRebootRestoresUnrelatedRecentBeforeReplay();
  if (failures != 0) return 1;
  std::cout << "BOOK_MOVE_DURABLE_FILE_PASS\n";
  return 0;
}
