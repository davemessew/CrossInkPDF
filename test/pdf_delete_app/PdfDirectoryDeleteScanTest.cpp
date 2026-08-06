#include "PdfDirectoryDeleteScan.h"

#include <HalStorage.h>
#include <Memory.h>

#include <algorithm>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

using PdfDirectoryDeleteScan::Status;

int failures = 0;

void expect(const bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

struct CallbackLog {
  std::vector<std::string> deletedPdfs;
  std::vector<std::string> clearedLegacy;
};

bool deletePdf(void* const context, const char* const path) {
  auto& log = *static_cast<CallbackLog*>(context);
  const std::string exactPath = path == nullptr ? "" : path;
  log.deletedPdfs.push_back(exactPath);
  return Storage.remove(exactPath.c_str());
}

bool clearLegacy(void* const context, const std::string_view path) {
  auto& log = *static_cast<CallbackLog*>(context);
  log.clearedLegacy.emplace_back(path);
  return true;
}

PdfDirectoryDeleteScan::DeleteCallbacks callbacksFor(CallbackLog& log) {
  return {&log, &deletePdf, &clearLegacy};
}

void reset() {
  Storage.reset();
  TestMemory::reset();
  Storage.mkdir("/.crosspoint");
  Storage.mkdir("/Books");
}

bool hasTreeMutationEvent() {
  return std::any_of(
      Storage.events.begin(), Storage.events.end(), [](const std::string& event) {
        return event.rfind("remove:/Books", 0) == 0 ||
               event.rfind("remove-dir:/Books", 0) == 0;
      });
}

void testInvalidRootsAndCallbacksFailClosed() {
  reset();
  Storage.putFile("/Books/keep.epub", {1});
  CallbackLog log;
  for (const std::string root :
       {"", "/", "Books", "/Books/", "/Books/../Other"}) {
    expect(PdfDirectoryDeleteScan::deletePdfDirectoryNoThrow(root,
                                                          callbacksFor(log)) ==
               Status::InvalidRoot,
           "non-canonical directory root must fail closed: " + root);
  }
  expect(PdfDirectoryDeleteScan::deletePdfDirectoryNoThrow(
             "/Books", PdfDirectoryDeleteScan::DeleteCallbacks{}) ==
             Status::InvalidRoot,
         "missing mutation callbacks must fail closed");
  expect(Storage.exists("/Books/keep.epub") && log.deletedPdfs.empty() &&
             log.clearedLegacy.empty() && !hasTreeMutationEvent(),
         "invalid requests must preserve the selected tree");
}

void testRootNestedIterationAndCloseFailuresAbortBeforeMutation() {
  reset();
  Storage.putFile("/Books/keep.epub", {1});
  Storage.failNextOpenOf("/Books");
  CallbackLog log;
  expect(PdfDirectoryDeleteScan::deletePdfDirectoryNoThrow(
             "/Books", callbacksFor(log)) == Status::OpenFailure,
         "root open failure must be result-bearing");
  expect(Storage.exists("/Books/keep.epub") && !hasTreeMutationEvent(),
         "root open failure must preserve the selected tree");

  reset();
  Storage.putFile("/Books/nested/keep.epub", {1});
  Storage.failNextOpenOf("/Books/nested");
  expect(PdfDirectoryDeleteScan::deletePdfDirectoryNoThrow(
             "/Books", callbacksFor(log)) == Status::OpenFailure,
         "nested open failure must abort the complete preflight");
  expect(Storage.exists("/Books/nested/keep.epub") && !hasTreeMutationEvent(),
         "nested open failure must preserve the selected tree");

  reset();
  Storage.putFile("/Books/keep.epub", {1});
  Storage.failNextDirectoryIterationOf("/Books");
  expect(PdfDirectoryDeleteScan::deletePdfDirectoryNoThrow(
             "/Books", callbacksFor(log)) == Status::IterationFailure,
         "directory iteration failure must abort the complete preflight");
  expect(Storage.exists("/Books/keep.epub") && !hasTreeMutationEvent(),
         "iteration failure must preserve the selected tree");

  reset();
  Storage.failCloseOnCall(1);
  expect(PdfDirectoryDeleteScan::deletePdfDirectoryNoThrow(
             "/Books", callbacksFor(log)) == Status::CloseFailure,
         "directory close failure must abort the complete preflight");
  expect(Storage.exists("/Books") && !hasTreeMutationEvent(),
         "close failure must preserve the selected tree");
}

void testCheckedWorkspaceAllocationAndReservedTombstone() {
  reset();
  TestMemory::failNextAllocation = true;
  CallbackLog log;
  expect(PdfDirectoryDeleteScan::deletePdfDirectoryNoThrow(
             "/Books", callbacksFor(log)) == Status::AllocationFailure,
         "checked walker allocation failure must return without scanning");
  expect(Storage.exists("/Books") && !hasTreeMutationEvent(),
         "walker allocation failure must preserve the selected tree");

  reset();
  Storage.putFile("/Books/.held.pdf.crossink-delete", {7});
  expect(PdfDirectoryDeleteScan::deletePdfDirectoryNoThrow(
             "/Books", callbacksFor(log)) == Status::ReservedTombstone,
         "prepared PDF tombstone must block recursive directory deletion");
  expect(Storage.exists("/Books/.held.pdf.crossink-delete") &&
             !hasTreeMutationEvent(),
         "tombstone preflight must preserve the prepared source");
}

}  // namespace

int main() {
  testInvalidRootsAndCallbacksFailClosed();
  testRootNestedIterationAndCloseFailuresAbortBeforeMutation();
  testCheckedWorkspaceAllocationAndReservedTombstone();
  if (failures != 0) return 1;
  std::cout << "PDF_DIRECTORY_DELETE_SCAN_PASS\n";
  return 0;
}
