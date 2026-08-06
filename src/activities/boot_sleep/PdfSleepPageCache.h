#pragma once

#if defined(CROSSINK_ENABLE_PDF) && CROSSINK_ENABLE_PDF

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

class GfxRenderer;

namespace RecentBookProgress {
class PdfProductCache;
}

// Captured while the PDF reader is still alive, before its per-book settings
// and orientation are restored by onExit().
struct PdfSleepPageLayout {
  int fontId = 0;
  int marginLeft = 0;
  int marginTop = 0;
  uint16_t viewportWidth = 0;
  uint16_t viewportHeight = 0;
  uint8_t orientation = 0;
  uint8_t backgroundColor = 0xff;
  bool foregroundBlack = true;
  bool valid = false;
};

// Implemented beside SleepActivity because it uses the active reader settings
// and UI metrics. Non-PDF and non-overlay construction returns an invalid POD
// without consulting the renderer.
PdfSleepPageLayout capturePdfSleepPageLayoutForSleep(GfxRenderer& renderer, bool canSnapshotOverlayBackground,
                                                     const std::string& currentBookPath);

// Sleep-only, read-only view of completed PDF products. Cache identity is
// derived from the path/move alias; loading never opens the PDF source.
class PdfSleepProductCache final {
 public:
  PdfSleepProductCache();
  ~PdfSleepProductCache();

  PdfSleepProductCache(const PdfSleepProductCache&) = delete;
  PdfSleepProductCache& operator=(const PdfSleepProductCache&) = delete;
  PdfSleepProductCache(PdfSleepProductCache&&) = delete;
  PdfSleepProductCache& operator=(PdfSleepProductCache&&) = delete;

  bool load(const std::string& sourcePath);
  bool available() const;
  void reset();

  const char* title() const;
  const char* author() const;
  const char* chapter() const;
  const char* coverPath() const;
  const char* thumbnailPath() const;
  const char* cacheRoot() const;
  uint16_t currentSection() const;
  uint32_t currentWord() const;
  uint32_t totalWords() const;
  uint32_t currentSectionFirstWordOrdinal() const;
  uint32_t currentSectionWordCount() const;
  float progressPercent() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

// PDF-only, cold-path view of one already-persisted reader page. It never
// creates or repairs cache files and never opens the PDF, XHTML, or image
// products. One sidecar handle and one section handle are used sequentially.
class PdfSleepPageCache final {
 public:
  static constexpr size_t MAX_SERIALIZED_PAGE_BYTES = 64U * 1024U;
  static constexpr uint16_t MAX_LAYOUT_PAGES = 4096;
  // Global decoder budgets bound both validation and direct-render work. They
  // are deliberately below what a malicious 64 KiB record could encode while
  // remaining well above one 800x480 reflow page.
  static constexpr uint16_t MAX_PAGE_ELEMENTS = 256;
  static constexpr uint16_t MAX_TOTAL_TEXT_BLOCKS = 512;
  static constexpr uint16_t MAX_TOTAL_WORDS = 4096;
  static constexpr uint32_t MAX_TOTAL_TEXT_BYTES = 48U * 1024U;
  static constexpr uint16_t MAX_TOTAL_TABLE_ROWS = 128;
  static constexpr uint16_t MAX_TOTAL_TABLE_CELLS = 512;
  static constexpr uint32_t MAX_DECODED_WORK_UNITS = 96U * 1024U;

  PdfSleepPageCache();
  ~PdfSleepPageCache();

  PdfSleepPageCache(const PdfSleepPageCache&) = delete;
  PdfSleepPageCache& operator=(const PdfSleepPageCache&) = delete;
  PdfSleepPageCache(PdfSleepPageCache&&) = delete;
  PdfSleepPageCache& operator=(PdfSleepPageCache&&) = delete;

  bool load(const PdfSleepProductCache& product, const PdfSleepPageLayout& layout);
  bool available() const;

  // The page is released before returning so a PNG overlay decoder never
  // competes with retained page allocations.
  bool renderTextAndRelease(GfxRenderer& renderer);
  void reset();

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

#endif  // defined(CROSSINK_ENABLE_PDF) && CROSSINK_ENABLE_PDF
