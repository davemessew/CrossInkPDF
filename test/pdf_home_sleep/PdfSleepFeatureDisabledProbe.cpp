#include <cstdint>
#include <string>
#include <vector>

#include "BookRouteSpy.h"
#include "HalStorage.h"
#include "PdfSleepPageCache.h"
#include "RecentBookProgress.h"
#include "RecentBooksStore.h"

#if defined(CROSSINK_ENABLE_PDF) && CROSSINK_ENABLE_PDF
#error "disabled PDF sleep probe compiled with PDF enabled"
#endif

namespace {

template <typename T>
void appendPod(std::vector<uint8_t>& bytes, const T value) {
  const auto* const raw = reinterpret_cast<const uint8_t*>(&value);
  bytes.insert(bytes.end(), raw, raw + sizeof(value));
}

bool legacyProgressRoutesMatch() {
  BOOK_ROUTE_SPY.reset();
  Storage.reset();

  RecentBook epub{"/books/legacy.epub", "", "", ""};
  if (RecentBookProgress::loadPercent(epub) != 42.0f || BOOK_ROUTE_SPY.epubLoads != 1 ||
      BOOK_ROUTE_SPY.epubProgressLoads != 1) {
    return false;
  }

  Storage.addFile("/xtc/progress.bin", {4, 0, 0, 0});
  RecentBook xtc{"/books/legacy.xtc", "", "", ""};
  if (RecentBookProgress::loadPercent(xtc) != 37.0f || BOOK_ROUTE_SPY.xtcLoads != 1) {
    return false;
  }

  std::vector<uint8_t> index;
  appendPod(index, uint32_t{0x54585449});
  appendPod(index, uint8_t{3});
  appendPod(index, uint32_t{1234});
  appendPod(index, int32_t{0});
  appendPod(index, int32_t{0});
  appendPod(index, int32_t{0});
  appendPod(index, int32_t{0});
  appendPod(index, uint8_t{0});
  appendPod(index, uint32_t{8});
  Storage.addFile("/txt/progress.bin", {3, 0, 0, 0});
  Storage.addFile("/txt/index.bin", index);

  RecentBook txt{"/books/legacy.txt", "", "", ""};
  RecentBook markdown{"/books/legacy.md", "", "", ""};
  if (RecentBookProgress::loadPercent(txt) != 50.0f || RecentBookProgress::loadPercent(markdown) != 50.0f ||
      BOOK_ROUTE_SPY.txtLoads != 2) {
    return false;
  }

  RecentBook unknown{"/books/disabled.pdf", "", "", ""};
  return RecentBookProgress::loadPercent(unknown) == -1.0f && RecentBookProgress::hasPercent(0.0f) &&
         !RecentBookProgress::hasPercent(-1.0f) && RecentBookProgress::formatPercent(42.0f) == "42%" &&
         RecentBookProgress::formatPercent(-1.0f).empty();
}

}  // namespace

int main() { return legacyProgressRoutesMatch() ? 0 : 1; }
