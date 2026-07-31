#include "PdfDirectoryDeleteScan.h"

#include <HalStorage.h>
#include <Memory.h>

#include <algorithm>
#include <iostream>
#include <string>
#include <vector>

namespace {

int failures = 0;
size_t pureDirectoryIterations = 0;
size_t pureExplicitHeapBytes = 0;

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
  log.deletedPdfs.emplace_back(path == nullptr ? "" : path);
  Storage.events.emplace_back(std::string("pdf:") + (path == nullptr ? "" : path));
  return true;
}

void clearLegacy(void* const context, const std::string& path) {
  auto& log = *static_cast<CallbackLog*>(context);
  log.clearedLegacy.emplace_back(path);
  Storage.events.emplace_back("clear:" + path);
}

void reset() {
  Storage.reset();
  TestMemory::reset();
  Storage.mkdir("/.crosspoint");
  Storage.mkdir("/Books");
}

void testWideAndDeepPureEpubUsesExactLegacyDeletePathWithoutPdfWorkspace() {
  reset();
  constexpr size_t kWideDirectories = 70;
  constexpr size_t kDeepDirectories = 80;
  for (size_t index = 0; index < kWideDirectories; ++index) {
    Storage.putFile("/Books/w" + std::to_string(index) + "/book.epub", {1});
  }
  std::string deepPath = "/Books";
  for (size_t depth = 0; depth < kDeepDirectories; ++depth) {
    deepPath += "/d";
    Storage.putFile(deepPath + "/book.epub", {2});
  }

  // The former unconditional PDF scan allocated exactly 34,976 bytes. A pure
  // legacy tree must never request that allocation or depend on its success.
  TestMemory::failAtOrAboveBytes = 34976U;
  CallbackLog log;
  const PdfDirectoryDeleteScan::DeleteCallbacks callbacks{
      &log, &deletePdf, &clearLegacy};
  const PdfDirectoryDeleteScan::Status status =
      PdfDirectoryDeleteScan::deleteDirectoryNoThrow("/Books", callbacks);

  expect(status == PdfDirectoryDeleteScan::Status::Complete,
         "wide/deep pure EPUB tree must take the successful legacy delete path");
  expect(!Storage.exists("/Books"),
         "legacy recursive directory removal result must remain successful");
  expect(log.deletedPdfs.empty(),
         "pure EPUB deletion must never enter the PDF journal callback");
  expect(log.clearedLegacy.size() == kWideDirectories + kDeepDirectories,
         "legacy metadata cleanup must retain every EPUB path");
  const auto remove = std::find(Storage.events.begin(), Storage.events.end(),
                                "remove-dir:/Books");
  const auto firstClear = std::find_if(
      Storage.events.begin(), Storage.events.end(),
      [](const std::string& event) { return event.rfind("clear:", 0) == 0; });
  expect(remove != Storage.events.end() && firstClear != Storage.events.end() &&
             remove < firstClear,
         "legacy metadata must still clear only after recursive removal");
  expect(Storage.pathOpenCount(PdfDirectoryDeleteScan::kSpoolTempPath) == 0 &&
             Storage.pathOpenCount(PdfDirectoryDeleteScan::kSpoolSealedPath) == 0 &&
             Storage.pathWrittenBytes(PdfDirectoryDeleteScan::kSpoolTempPath) == 0 &&
             Storage.pathWrittenBytes(PdfDirectoryDeleteScan::kSpoolSealedPath) == 0,
         "pure EPUB discovery must never create, open, or write a PDF spool");
  expect(Storage.maximumDirectDirectoryReaders() <= 1,
         "discovery must hold at most one direct directory reader");
  expect(std::none_of(TestMemory::allocationSizes.begin(),
                      TestMemory::allocationSizes.end(),
                      [](const size_t bytes) { return bytes >= 34976U; }),
         "pure EPUB deletion must not request the former PDF workspace");

  pureDirectoryIterations = Storage.directoryIterationCalls();
  pureExplicitHeapBytes =
      TestMemory::allocationSizes.empty() ? 0U : TestMemory::allocationSizes.front();
}

}  // namespace

int main() {
  testWideAndDeepPureEpubUsesExactLegacyDeletePathWithoutPdfWorkspace();
  if (failures != 0) return 1;
  std::cout << "PDF_DIRECTORY_DELETE_LEGACY_PATH_PASS"
            << " iterations=" << pureDirectoryIterations
            << " explicit_heap_bytes=" << pureExplicitHeapBytes << '\n';
  return 0;
}
