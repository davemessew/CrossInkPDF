#include <HalStorage.h>

#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

#include "CrossPointSettings.h"
#include "NextBookFinder.h"
#include "ScreenshotInfo.h"

namespace {

static_assert(static_cast<uint8_t>(ScreenshotInfo::ReaderType::None) == 0);
static_assert(static_cast<uint8_t>(ScreenshotInfo::ReaderType::Epub) == 1);
static_assert(static_cast<uint8_t>(ScreenshotInfo::ReaderType::Txt) == 2);
static_assert(static_cast<uint8_t>(ScreenshotInfo::ReaderType::Xtc) == 3);
static_assert(static_cast<uint8_t>(ScreenshotInfo::ReaderType::Pdf) == 4);
static_assert(sizeof(ScreenshotInfo::ReaderType) == sizeof(uint8_t));

int failures = 0;

void expect(const bool condition, const char* const message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

void expectNames(const std::vector<std::string>& actual, const std::vector<std::string>& expected,
                 const char* const message) {
  if (actual == expected) {
    return;
  }
  std::cerr << "FAIL: " << message << "\n  expected:";
  for (const auto& name : expected) {
    std::cerr << ' ' << name;
  }
  std::cerr << "\n  actual:";
  for (const auto& name : actual) {
    std::cerr << ' ' << name;
  }
  std::cerr << '\n';
  ++failures;
}

void testPdfParticipatesInBoundedNaturalOrdering() {
  Storage.setDirectory(
      "/books",
      {
          {"book10.pdf", false},
          {"book5.xtc", false},
          {"book2.PDF", false},
          {"book7.md", false},
          {"book4.txt", false},
          {"book3.epub", false},
          {"book6.xtch", false},
          {"cover.bmp", false},
          {"nested", true},
          {"book1.epub", false},
      });
  SETTINGS.showHiddenFiles = 0;

  const auto next = NextBookFinder::findNextBooks("/books/book1.epub", 3);

  expectNames(next, {"book2.PDF", "book3.epub", "book4.txt"},
              "PDF must join existing book formats in natural order while maxCount stays binding");
  expect(next.size() == 3, "result must never exceed maxCount");
  expect(Storage.openCalls() == 1, "next-book scan must open the directory once");
  expect(Storage.openNextCalls() == 11, "next-book scan must remain a single directory pass");
}

void testPdfCanBeTheCurrentBook() {
  Storage.setDirectory(
      "/books",
      {
          {"book10.PdF", false},
          {"book1.epub", false},
          {"book3.txt", false},
          {"book2.pdf", false},
      });

  expectNames(NextBookFinder::findNextBooks("/books/book2.pdf", 4), {"book3.txt", "book10.PdF"},
              "a current PDF must use the same natural-order successor rules");
}

void testExistingFormatsAndExclusionsRemainUnchanged() {
  Storage.setDirectory(
      "/library",
      {
          {"a.epub", false},
          {"b.epub", false},
          {"c.txt", false},
          {"d.xtc", false},
          {"e.xtch", false},
          {"f.md", false},
          {"g.bmp", false},
          {"h.png", false},
          {"i.bin", false},
          {"j.epub", true},
          {".k.epub", false},
      });
  SETTINGS.showHiddenFiles = 0;

  expectNames(NextBookFinder::findNextBooks("/library/a.epub", 8),
              {"b.epub", "c.txt", "d.xtc", "e.xtch", "f.md"},
              "EPUB, TXT, XTC/XTCH, Markdown, hidden, directory, and viewer behavior must stay unchanged");
}

void testZeroLimitAndMissingDirectoryRemainEmpty() {
  Storage.setDirectory("/books", {{"book2.pdf", false}});
  expect(NextBookFinder::findNextBooks("/books/book1.epub", 0).empty(), "zero maxCount must stay allocation-free");
  expect(Storage.openCalls() == 0, "zero maxCount must not touch storage");

  Storage.setOpenFailure();
  expect(NextBookFinder::findNextBooks("/books/book1.epub", 2).empty(), "missing directory must stay empty");
}

}  // namespace

int main() {
  testPdfParticipatesInBoundedNaturalOrdering();
  testPdfCanBeTheCurrentBook();
  testExistingFormatsAndExclusionsRemainUnchanged();
  testZeroLimitAndMissingDirectoryRemainEmpty();

  if (failures != 0) {
    return 1;
  }
  std::cout << "PDF_FORMAT_ROUTING_BEHAVIOR_PASS\n";
  return 0;
}
