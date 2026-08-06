#include "PdfDirectoryDeleteScan.h"

#include <HalStorage.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>

namespace {

int failures = 0;

void expect(const bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

void setSimulatorRoot(const std::filesystem::path& root) {
#if defined(_WIN32)
  _putenv_s("CROSSPOINT_SIM_SD", root.string().c_str());
#else
  setenv("CROSSPOINT_SIM_SD", root.string().c_str(), 1);
#endif
}

bool deletePdf(void*, const char*) { return false; }

bool clearLegacy(void*, std::string_view) { return true; }

void testHiddenTombstoneSkippedByPinnedHalStillBlocksDelete() {
  const std::filesystem::path root =
      std::filesystem::temp_directory_path() / "crossink_pdf_delete_simulator_tombstone";
  std::error_code error;
  std::filesystem::remove_all(root, error);
  std::filesystem::create_directories(root / "Books", error);
  std::ofstream(root / "Books" / ".prepared.pdf.crossink-delete", std::ios::binary).put('x');
  setSimulatorRoot(root);

  Storage.reset();
  Storage.mkdir("/Books");
  const PdfDirectoryDeleteScan::DeleteCallbacks callbacks{
      nullptr, &deletePdf, &clearLegacy};
  const auto status =
      PdfDirectoryDeleteScan::deletePdfDirectoryNoThrow("/Books", callbacks);
  expect(status == PdfDirectoryDeleteScan::Status::ReservedTombstone,
         "actual SIMULATOR branch must detect a dot-prefixed tombstone hidden by its HAL iterator");
  expect(std::filesystem::exists(root / "Books" / ".prepared.pdf.crossink-delete"),
         "SIMULATOR preflight must not mutate the hidden tombstone");

  Storage.reset();
  Storage.mkdir("/Books");
  const auto publicStatus =
      PdfDirectoryDeleteScan::deleteDirectoryNoThrow("/Books", callbacks);
  expect(publicStatus == PdfDirectoryDeleteScan::Status::ReservedTombstone,
         "public directory routing must classify a HAL-hidden SIMULATOR tombstone as strict");
  expect(Storage.exists("/Books") &&
             std::filesystem::exists(
                 root / "Books" / ".prepared.pdf.crossink-delete"),
         "public routing must never fall through to legacy removeDir for a hidden tombstone");

  std::filesystem::remove_all(root, error);
}

}  // namespace

int main() {
  testHiddenTombstoneSkippedByPinnedHalStillBlocksDelete();
  if (failures != 0) return 1;
  std::cout << "PDF_DIRECTORY_DELETE_SIMULATOR_TOMBSTONE_PASS\n";
  return 0;
}
