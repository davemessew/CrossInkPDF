#pragma once

#include <string>

#if defined(CROSSINK_ENABLE_PDF) && CROSSINK_ENABLE_PDF
#include <cstdint>
#include <memory>
#endif

struct RecentBook;

// Helpers for loading and displaying recent-book reading progress.
// Progress values are percentages in the 0-100 range; negative means unknown.
//
// Example:
//   const float progress = RecentBookProgress::loadPercent(book);
//   const std::string label = RecentBookProgress::formatPercent(progress);
namespace RecentBookProgress {
#if defined(CROSSINK_ENABLE_PDF) && CROSSINK_ENABLE_PDF
// Lifecycle-owned, read-only view of one PDF's completed cache products.
// The implementation and its bounded loader workspace live on the heap so
// activity task frames stay small on the ESP32-C3.
class PdfProductCache {
 public:
  PdfProductCache();
  ~PdfProductCache();

  PdfProductCache(const PdfProductCache&) = delete;
  PdfProductCache& operator=(const PdfProductCache&) = delete;
  PdfProductCache(PdfProductCache&&) = delete;
  PdfProductCache& operator=(PdfProductCache&&) = delete;

  bool initialize();
  void reset();

  // Loads and validates completed cache products. Reusing the same source path
  // and resolved cache identity during this lifecycle returns the retained
  // result without reopening files.
  bool load(const std::string& sourcePath);
  bool available() const;
  bool preservesStoredFallback() const;

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

// Applies PDF-specific cached metadata, thumbnail, chapter, and progress to a
// recent-book view. A normally uncached PDF receives the strict
// filename/empty-cover/zero-progress fallback. During an unresolved or
// pre-activation move, unavailable read-only products preserve the stored
// title/author/thumbnail instead of erasing the recent-book entry.
// Non-PDF books return false and are not changed.
bool hydratePdfBook(PdfProductCache& cache, RecentBook& book, float* progress = nullptr, std::string* chapter = nullptr,
                    std::string* fullCoverPath = nullptr);
#endif

// Loads the saved reading percentage for a recent EPUB, XTC, TXT, or Markdown book.
// Returns -1.0f when progress is unavailable or the cache cannot be read.
float loadPercent(const RecentBook& book);
// Loads the cached EPUB reading percentage without opening the EPUB.
// Returns -1.0f when progress is unavailable or the cache cannot be read.
float loadCachedEpubPercent(const RecentBook& book);
// Saves a cached EPUB reading percentage for fast Home screen rendering.
void saveCachedEpubPercent(const std::string& cachePath, float progress);
// Calculates and saves cached EPUB progress from a reader position.
void saveCachedEpubPercent(const Epub& epub, int spineIndex, int currentPage, int pageCount);
// Returns true when progress contains a known 0-100 percentage.
bool hasPercent(float progress);
// Formats progress as a rounded percentage string such as "42%".
// Returns an empty string when progress is unknown.
std::string formatPercent(float progress);
}  // namespace RecentBookProgress
