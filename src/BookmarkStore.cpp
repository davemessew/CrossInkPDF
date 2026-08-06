#include "BookmarkStore.h"

#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>
#include <Serialization.h>
#include <uzlib.h>

#include <algorithm>
#include <cstring>
#include <functional>
#include <limits>

#include "util/BookMoveDurableFile.h"

namespace {
constexpr uint8_t LEGACY_VERSION = 2;
constexpr uint8_t COUNT_U16_VERSION = 3;
constexpr uint8_t PARAGRAPH_ANCHOR_VERSION = 4;
constexpr uint8_t VERSION = 5;
// Stored count is uint16_t in v3+, but we keep an in-memory safety cap for ESP32-C3 RAM.
constexpr uint16_t MAX_BOOKMARKS = 1024;
constexpr size_t INITIAL_BOOKMARK_RESERVE = 8;
constexpr char BOOKMARKS_DIR[] = "/.crosspoint/bookmarks";
constexpr char READ_FOLDER[] = "/Read";
constexpr char PDF_TRANSACTION_TEMP_SUFFIX[] = ".tmp";
constexpr char PDF_TRANSACTION_BACKUP_SUFFIX[] = ".bak";
constexpr char PDF_STORE_PREFIX[] = "/.crosspoint/bookmarks/pdf_";

struct BookmarkFileHeader {
  std::string title;
  std::string author;
  std::string path;
  std::string bookType;
  uint16_t count = 0;
};

struct PdfBookmarkMigrationScratch {
  BookmarkStore store;
  std::vector<Bookmark> primary;
  std::vector<Bookmark> secondary;
  std::string sourceCurrentPath;
  std::string sourceLegacyPath;
  std::string destinationCurrentPath;
  std::string destinationLegacyPath;
  std::string temporaryPath;
  std::string backupPath;
};
static_assert(sizeof(PdfBookmarkMigrationScratch) <= 640);

struct PdfBookmarkLoadScratch {
  BookmarkStore candidate;
  std::vector<Bookmark> legacyBookmarks;
  std::string currentPath;
  std::string legacyPath;
};
static_assert(sizeof(PdfBookmarkLoadScratch) <= 320);

bool readExpectedBytes(FsFile& file, const void* const expected, const size_t length) {
  const auto* expectedBytes = static_cast<const uint8_t*>(expected);
  uint8_t actual[64];
  size_t offset = 0;
  while (offset < length) {
    const size_t chunk = std::min(sizeof(actual), length - offset);
    if (file.read(actual, chunk) != static_cast<int>(chunk) ||
        std::memcmp(actual, expectedBytes + offset, chunk) != 0) {
      return false;
    }
    offset += chunk;
  }
  return true;
}

template <typename T>
bool readExpectedPod(FsFile& file, const T& expected) {
  return readExpectedBytes(file, &expected, sizeof(expected));
}

bool readExpectedString(FsFile& file, const std::string& expected) {
  const uint32_t length = static_cast<uint32_t>(expected.size());
  return readExpectedPod(file, length) && readExpectedBytes(file, expected.data(), expected.size());
}

const Bookmark* pdfBookmarkAt(const std::vector<Bookmark>& bookmarks, const Bookmark* const appended,
                              const size_t removeIndex, const size_t logicalIndex) {
  size_t candidate = 0;
  for (size_t index = 0; index < bookmarks.size(); ++index) {
    if (index == removeIndex) continue;
    if (candidate++ == logicalIndex) return &bookmarks[index];
  }
  return appended != nullptr && candidate == logicalIndex ? appended : nullptr;
}

size_t pdfBookmarkCount(const std::vector<Bookmark>& bookmarks, const Bookmark* const appended,
                        const size_t removeIndex, const bool clear) {
  if (clear) return 0;
  return bookmarks.size() - static_cast<size_t>(removeIndex < bookmarks.size()) +
         static_cast<size_t>(appended != nullptr);
}

bool isPdfStorePath(const std::string& path) {
  return path.compare(0, sizeof(PDF_STORE_PREFIX) - 1U, PDF_STORE_PREFIX) == 0;
}

bool hasSuffix(const char* const value, const char* const suffix) {
  if (value == nullptr || suffix == nullptr) return false;
  const size_t valueLength = std::strlen(value);
  const size_t suffixLength = std::strlen(suffix);
  return valueLength >= suffixLength && std::memcmp(value + valueLength - suffixLength, suffix, suffixLength) == 0;
}

bool isPdfTransactionArtifact(const char* const name) {
  return hasSuffix(name, PDF_TRANSACTION_TEMP_SUFFIX) || hasSuffix(name, PDF_TRANSACTION_BACKUP_SUFFIX);
}

std::string currentStoreFilePathForBook(const std::string& filePath, const std::string& bookType) {
  const uint32_t crc = uzlib_crc32(filePath.data(), static_cast<unsigned int>(filePath.size()), 0);
  return std::string(BOOKMARKS_DIR) + "/" + bookType + "_" + std::to_string(crc) + ".bin";
}

std::string legacyStoreFilePathForBook(const std::string& filePath, const std::string& bookType) {
  return std::string(BOOKMARKS_DIR) + "/" + bookType + "_" + std::to_string(std::hash<std::string>{}(filePath)) +
         ".bin";
}

bool pdfBookmarkStoreFilePathForBook(const std::string& filePath, const bool legacy, std::string& output) {
  constexpr size_t PATH_CAPACITY = 64;
  char path[PATH_CAPACITY];
  const unsigned long long hash = legacy ? static_cast<unsigned long long>(std::hash<std::string>{}(filePath))
                                         : static_cast<unsigned long long>(uzlib_crc32(
                                               filePath.data(), static_cast<unsigned int>(filePath.size()), 0));
  const int written = snprintf(path, sizeof(path), "%s/pdf_%llu.bin", BOOKMARKS_DIR, hash);
  if (written < 0 || static_cast<size_t>(written) >= sizeof(path)) {
    LOG_ERR("BKS", "PDF bookmark store path exceeds %u bytes", static_cast<unsigned>(sizeof(path)));
    return false;
  }
  output.assign(path, static_cast<size_t>(written));
  return true;
}

bool readBookmarkCount(FsFile& file, const uint8_t version, uint16_t& count) {
  if (version == LEGACY_VERSION) {
    uint8_t legacyCount = 0;
    serialization::readPod(file, legacyCount);
    count = legacyCount;
    return true;
  }

  if (version == COUNT_U16_VERSION || version == PARAGRAPH_ANCHOR_VERSION || version == VERSION) {
    serialization::readPod(file, count);
    return true;
  }

  return false;
}

bool bookmarksMatchIdentity(const Bookmark& a, const Bookmark& b, const bool stablePdfIds) {
  if (stablePdfIds) {
    return a.paragraphIndex != 0 && a.paragraphIndex != UINT16_MAX && a.paragraphIndex == b.paragraphIndex;
  }
  return a.spineIndex == b.spineIndex && a.progress == b.progress;
}

bool bookmarksMatchExactly(const Bookmark& a, const Bookmark& b) {
  return a.spineIndex == b.spineIndex && a.progress == b.progress && a.timestamp == b.timestamp &&
         a.paragraphIndex == b.paragraphIndex &&
         std::memcmp(a.chapterTitle, b.chapterTitle, sizeof(a.chapterTitle)) == 0 &&
         std::memcmp(a.snippet, b.snippet, sizeof(a.snippet)) == 0;
}

bool mergeBookmarks(std::vector<Bookmark>& dst, const std::vector<Bookmark>& src, const bool stablePdfIds) {
  bool mergedAny = false;
  for (const auto& bookmark : src) {
    const auto it = std::find_if(dst.begin(), dst.end(), [&](const Bookmark& existing) {
      return bookmarksMatchIdentity(existing, bookmark, stablePdfIds);
    });
    if (it != dst.end()) {
      continue;
    }
    if (dst.size() >= MAX_BOOKMARKS) {
      LOG_ERR("BKS", "Bookmark limit (%u) reached while merging legacy bookmarks", MAX_BOOKMARKS);
      break;
    }
    dst.push_back(bookmark);
    mergedAny = true;
  }
  return mergedAny;
}

bool deleteBookmarkStorePath(const std::string& path, const std::string& reasonTag) {
  if (!Storage.exists(path.c_str())) {
    return true;
  }
  if (!Storage.remove(path.c_str())) {
    LOG_ERR("BKS", "Failed to delete %s bookmark file: %s", reasonTag.c_str(), path.c_str());
    return false;
  }
  return true;
}

bool deletePdfBookmarkStorePaths(const std::string& currentPath, const std::string& legacyPath) {
  const std::string currentBackup = currentPath + PDF_TRANSACTION_BACKUP_SUFFIX;
  const std::string currentTemporary = currentPath + PDF_TRANSACTION_TEMP_SUFFIX;
  const std::string legacyBackup = legacyPath + PDF_TRANSACTION_BACKUP_SUFFIX;
  const std::string legacyTemporary = legacyPath + PDF_TRANSACTION_TEMP_SUFFIX;

  // Delete rollback material first and the authoritative canonical path last.
  // Any failure before that last step leaves a recoverable committed state.
  if (!deleteBookmarkStorePath(currentBackup, "PDF backup") ||
      !deleteBookmarkStorePath(currentTemporary, "PDF temporary")) {
    return false;
  }
  if (legacyPath != currentPath && (!deleteBookmarkStorePath(legacyBackup, "legacy PDF backup") ||
                                    !deleteBookmarkStorePath(legacyTemporary, "legacy PDF temporary") ||
                                    !deleteBookmarkStorePath(legacyPath, "legacy PDF canonical"))) {
    return false;
  }
  return deleteBookmarkStorePath(currentPath, "PDF canonical");
}

std::string fileNameFromPath(const std::string& path) {
  const size_t lastSlash = path.rfind('/');
  return (lastSlash != std::string::npos) ? path.substr(lastSlash + 1) : path;
}

bool isInReadFolder(const std::string& path) {
  constexpr size_t n = sizeof(READ_FOLDER) - 1;
  return path.size() > n && path.compare(0, n, READ_FOLDER) == 0 && path[n] == '/';
}

bool isReadFolderCollisionVariant(const std::string& originalBase, const std::string& candidateBase) {
  if (candidateBase.size() <= originalBase.size() + 4) {
    return false;
  }
  if (candidateBase.compare(0, originalBase.size(), originalBase) != 0) {
    return false;
  }
  if (candidateBase.compare(originalBase.size(), 2, " (") != 0 || candidateBase.back() != ')') {
    return false;
  }
  for (size_t i = originalBase.size() + 2; i + 1 < candidateBase.size(); i++) {
    if (candidateBase[i] < '0' || candidateBase[i] > '9') {
      return false;
    }
  }
  return true;
}

bool resolveMovedToReadDestinationPath(const std::string& originalPath, std::string& resolvedPath) {
  const std::string fileName = fileNameFromPath(originalPath);
  if (fileName.empty() || !Storage.exists(READ_FOLDER)) {
    return false;
  }

  const std::string exactPath = std::string(READ_FOLDER) + "/" + fileName;
  if (Storage.exists(exactPath.c_str())) {
    resolvedPath = exactPath;
    return true;
  }

  const size_t dotPos = fileName.rfind('.');
  const std::string originalBase = (dotPos != std::string::npos) ? fileName.substr(0, dotPos) : fileName;
  const std::string originalExt = (dotPos != std::string::npos) ? fileName.substr(dotPos) : "";

  std::string matchedPath;
  size_t matchCount = 0;
  for (const auto& entry : Storage.listFiles(READ_FOLDER)) {
    const std::string candidateName = entry.c_str();
    const size_t candidateDotPos = candidateName.rfind('.');
    const std::string candidateBase =
        (candidateDotPos != std::string::npos) ? candidateName.substr(0, candidateDotPos) : candidateName;
    const std::string candidateExt =
        (candidateDotPos != std::string::npos) ? candidateName.substr(candidateDotPos) : "";

    if (candidateExt != originalExt || !isReadFolderCollisionVariant(originalBase, candidateBase)) {
      continue;
    }

    matchedPath = std::string(READ_FOLDER) + "/" + candidateName;
    matchCount++;
    if (matchCount > 1) {
      return false;
    }
  }

  if (matchCount == 1) {
    resolvedPath = matchedPath;
    return true;
  }
  return false;
}

bool readBookmarkFileHeader(const std::string& fullPath, const char* name, BookmarkFileHeader& header) {
  FsFile f;
  if (!Storage.openFileForRead("BKS", fullPath, f)) {
    return false;
  }

  if (f.available() < static_cast<int>(sizeof(uint8_t))) {
    f.close();
    return false;
  }
  uint8_t version = 0;
  serialization::readPod(f, version);
  if (version != LEGACY_VERSION && version != COUNT_U16_VERSION && version != PARAGRAPH_ANCHOR_VERSION &&
      version != VERSION) {
    f.close();
    return false;
  }

  if (f.available() < static_cast<int>(version == LEGACY_VERSION ? sizeof(uint8_t) : sizeof(uint16_t))) {
    f.close();
    return false;
  }
  if (!readBookmarkCount(f, version, header.count)) {
    f.close();
    return false;
  }

  auto readCheckedString = [&f](std::string& s) -> bool {
    uint32_t len;
    if (f.available() < static_cast<int>(sizeof(len))) return false;
    serialization::readPod(f, len);
    if (f.available() < static_cast<int>(len)) return false;
    s.resize(len);
    f.read(reinterpret_cast<uint8_t*>(&s[0]), len);
    return true;
  };

  if (!readCheckedString(header.title) || !readCheckedString(header.author) || !readCheckedString(header.path)) {
    f.close();
    return false;
  }
  f.close();

  header.bookType = "epub";
  const std::string nameStr = name ? name : "";
  const size_t underscorePos = nameStr.find('_');
  if (underscorePos != std::string::npos) {
    header.bookType = nameStr.substr(0, underscorePos);
  }
  return true;
}
}  // namespace

BookmarkStore BookmarkStore::instance;

bool BookmarkStore::loadPdfForBook(const std::string& filePath, const std::string& title, const std::string& author) {
  // This cold path needs a Store, a second record vector, and two path owners.
  // Keep the fixed control state off the task stack in one checked allocation.
  auto scratch = makeUniqueNoThrow<PdfBookmarkLoadScratch>();
  if (!scratch) {
    LOG_ERR("BKS", "Out of memory allocating %u-byte PDF bookmark load scratch",
            static_cast<unsigned>(sizeof(PdfBookmarkLoadScratch)));
    return false;
  }
  if (!pdfBookmarkStoreFilePathForBook(filePath, false, scratch->currentPath) ||
      !pdfBookmarkStoreFilePathForBook(filePath, true, scratch->legacyPath)) {
    return false;
  }

  BookmarkStore& candidate = scratch->candidate;
  candidate.bookFilePath = filePath;
  candidate.bookTitle = title;
  candidate.bookAuthor = author;
  candidate.dirty = false;
  if (candidate.bookmarks.capacity() < INITIAL_BOOKMARK_RESERVE) {
    candidate.bookmarks.reserve(INITIAL_BOOKMARK_RESERVE);
  }

  candidate.storeFilePath = scratch->currentPath;
  if (!candidate.recoverPdfTransaction()) {
    LOG_ERR("BKS", "Failed to recover PDF bookmark transaction: %s", scratch->currentPath.c_str());
    return false;
  }
  if (scratch->legacyPath != scratch->currentPath) {
    candidate.storeFilePath = scratch->legacyPath;
    if (!candidate.recoverPdfTransaction()) {
      LOG_ERR("BKS", "Failed to recover legacy PDF bookmark transaction: %s", scratch->legacyPath.c_str());
      return false;
    }
  }

  const bool hasCurrentFile = Storage.exists(scratch->currentPath.c_str());
  const bool hasLegacyFile = scratch->legacyPath != scratch->currentPath && Storage.exists(scratch->legacyPath.c_str());
  bool needsRewrite = false;
  if (hasCurrentFile && !candidate.readFromFile(scratch->currentPath, candidate.bookmarks, needsRewrite)) {
    LOG_ERR("BKS", "Failed to load canonical PDF bookmark file: %s", scratch->currentPath.c_str());
    return false;
  }

  bool legacyNeedsRewrite = false;
  bool mergedLegacyBookmarks = false;
  if (hasLegacyFile) {
    if (!candidate.readFromFile(scratch->legacyPath, scratch->legacyBookmarks, legacyNeedsRewrite)) {
      LOG_ERR("BKS", "Failed to load legacy PDF bookmark file: %s", scratch->legacyPath.c_str());
      return false;
    }
    mergedLegacyBookmarks = mergeBookmarks(candidate.bookmarks, scratch->legacyBookmarks, true);
  }

  if (!hasCurrentFile && !hasLegacyFile) {
    candidate.storeFilePath = scratch->currentPath;
  } else {
    if (hasLegacyFile && (!hasCurrentFile || mergedLegacyBookmarks || legacyNeedsRewrite || needsRewrite)) {
      candidate.storeFilePath = scratch->currentPath;
      if (!candidate.writePdfTransaction(nullptr, std::numeric_limits<size_t>::max(), false)) {
        return false;
      }
    } else if (needsRewrite) {
      candidate.storeFilePath = scratch->currentPath;
      if (!candidate.writePdfTransaction(nullptr, std::numeric_limits<size_t>::max(), false)) {
        return false;
      }
    }

    if (hasLegacyFile && !deletePdfBookmarkStorePaths(scratch->legacyPath, scratch->legacyPath)) {
      return false;
    }
    candidate.storeFilePath = scratch->currentPath;
  }

  bookmarks = std::move(candidate.bookmarks);
  bookFilePath = std::move(candidate.bookFilePath);
  bookTitle = std::move(candidate.bookTitle);
  bookAuthor = std::move(candidate.bookAuthor);
  storeFilePath = std::move(candidate.storeFilePath);
  dirty = false;
  return true;
}

bool BookmarkStore::loadLegacyForBook(const std::string& filePath, const std::string& title, const std::string& author,
                                      const std::string& bookType) {
  bookFilePath = filePath;
  bookTitle = title;
  bookAuthor = author;
  dirty = false;
  bookmarks.clear();
  if (bookmarks.capacity() < INITIAL_BOOKMARK_RESERVE) {
    bookmarks.reserve(INITIAL_BOOKMARK_RESERVE);
  }

  storeFilePath = currentStoreFilePathForBook(filePath, bookType);
  if (bookType == "pdf" && !recoverPdfTransaction()) {
    LOG_ERR("BKS", "Failed to recover PDF bookmark transaction: %s", storeFilePath.c_str());
    return false;
  }
  const std::string legacyStoreFilePath = legacyStoreFilePathForBook(filePath, bookType);
  const bool hasCurrentFile = Storage.exists(storeFilePath.c_str());
  const bool hasLegacyFile = legacyStoreFilePath != storeFilePath && Storage.exists(legacyStoreFilePath.c_str());

  if (!hasCurrentFile && !hasLegacyFile) {
    if (bookType == "epub" && isInReadFolder(filePath) && Storage.exists(BOOKMARKS_DIR)) {
      for (const auto& name : Storage.listFiles(BOOKMARKS_DIR)) {
        BookmarkFileHeader header;
        const std::string fullPath = std::string(BOOKMARKS_DIR) + "/" + name.c_str();
        if (!readBookmarkFileHeader(fullPath, name.c_str(), header)) continue;
        if (header.bookType != bookType || header.count == 0 || Storage.exists(header.path.c_str())) continue;
        if (!title.empty() && !header.title.empty() && header.title != title) continue;
        if (!author.empty() && !header.author.empty() && header.author != author) continue;

        std::string resolvedMovedPath;
        if (!resolveMovedToReadDestinationPath(header.path, resolvedMovedPath) || resolvedMovedPath != filePath) {
          continue;
        }

        if (migrateForFilePath(header.path, filePath, title, author, bookType)) {
          return loadForBook(filePath, title, author, bookType);
        }
        break;
      }
    }

    return true;
  }

  bool loadedAny = false;
  bool needsRewrite = false;

  bool currentLoaded = false;
  if (hasCurrentFile) {
    bool currentNeedsRewrite = false;
    if (readFromFile(storeFilePath, bookmarks, currentNeedsRewrite)) {
      loadedAny = true;
      currentLoaded = true;
      needsRewrite = currentNeedsRewrite;
    } else {
      LOG_ERR("BKS", "Failed to load canonical bookmark file: %s", storeFilePath.c_str());
    }
  }

  if (hasLegacyFile) {
    bool legacyNeedsRewrite = false;
    std::vector<Bookmark> legacyBookmarks;
    if (readFromFile(legacyStoreFilePath, legacyBookmarks, legacyNeedsRewrite)) {
      const bool mergedLegacyBookmarks = mergeBookmarks(bookmarks, legacyBookmarks, bookType == "pdf");
      loadedAny = true;
      bool canDeleteLegacyFile = currentLoaded;

      if (!hasCurrentFile || mergedLegacyBookmarks || legacyNeedsRewrite || needsRewrite) {
        dirty = true;
        saveToFile();
        canDeleteLegacyFile = !dirty;
      }

      if (canDeleteLegacyFile) {
        if (deleteBookmarkStorePath(legacyStoreFilePath, "legacy")) {
          LOG_INF("BKS", "Migrated legacy bookmark store: %s -> %s", legacyStoreFilePath.c_str(),
                  storeFilePath.c_str());
        }
      }
    } else {
      LOG_ERR("BKS", "Failed to load legacy bookmark file: %s", legacyStoreFilePath.c_str());
    }
  }

  if (!loadedAny) {
    return false;
  }

  if (needsRewrite && !hasLegacyFile) {
    dirty = true;
    saveToFile();
  }

  return true;
}

bool BookmarkStore::loadForBook(const std::string& filePath, const std::string& title, const std::string& author,
                                const std::string& bookType) {
  if (bookType != "epub" && bookType != "pdf" && bookType != "xtc" && bookType != "txt") {
    LOG_ERR("BKS", "Unknown book type: %s", bookType.c_str());
    return false;
  }
  if (bookType == "pdf") {
    return loadPdfForBook(filePath, title, author);
  }
  return loadLegacyForBook(filePath, title, author, bookType);
}

bool deleteBookmarkStorePathNoPathAlloc(const char* const path, const char* const reasonTag) {
  if (!Storage.exists(path)) return true;
  if (!Storage.remove(path)) {
    LOG_ERR("BKS", "Failed to delete %s bookmark file: %s", reasonTag, path);
    return false;
  }
  return true;
}

bool mergeAuthoritativeBookmarks(std::vector<Bookmark>& destination, const std::vector<Bookmark>& source,
                                 const bool stablePdfIds) {
  for (const Bookmark& bookmark : source) {
    const auto found = std::find_if(destination.begin(), destination.end(), [&](const Bookmark& existing) {
      return bookmarksMatchIdentity(existing, bookmark, stablePdfIds);
    });
    if (found != destination.end()) {
      *found = bookmark;
      continue;
    }
    if (destination.size() >= MAX_BOOKMARKS) {
      LOG_ERR("BKS", "Bookmark limit (%u) reached while copying moved-book state", MAX_BOOKMARKS);
      return false;
    }
    destination.push_back(bookmark);
  }
  return true;
}

bool containsAllBookmarks(const std::vector<Bookmark>& destination, const std::vector<Bookmark>& source) {
  return std::all_of(source.begin(), source.end(), [&](const Bookmark& expected) {
    return std::any_of(destination.begin(), destination.end(),
                       [&](const Bookmark& actual) { return bookmarksMatchExactly(actual, expected); });
  });
}

bool BookmarkStore::reloadPdfFromDisk() {
  if (bookFilePath.empty() || !isPdfStorePath(storeFilePath)) {
    LOG_ERR("BKS", "Cannot reload a PDF bookmark store before it is loaded");
    return false;
  }
  return loadPdfForBook(bookFilePath, bookTitle, bookAuthor);
}

void BookmarkStore::unload() {
  if (dirty) saveToFile();
  bookmarks.clear();
  bookFilePath.clear();
  bookTitle.clear();
  bookAuthor.clear();
  storeFilePath.clear();
  dirty = false;
}

BookmarkStore::AddResult BookmarkStore::addBookmark(uint16_t spineIndex, float progress, int pageCount,
                                                    const char* chapterTitle, uint16_t paragraphIndex,
                                                    const char* snippet) {
  if (pageCount > 0) {
    const float pageSlice = 1.0f / static_cast<float>(pageCount);
    const float pageStart = progress;
    const float pageEnd = progress + pageSlice;
    std::erase_if(bookmarks, [&](const Bookmark& b) {
      return b.spineIndex == spineIndex && b.progress >= pageStart && b.progress < pageEnd;
    });
  }

  if (bookmarks.size() >= MAX_BOOKMARKS) {
    LOG_ERR("BKS", "Bookmark limit (%u) reached", MAX_BOOKMARKS);
    return AddResult::LimitReached;
  }

  Bookmark bm{};
  bm.spineIndex = spineIndex;
  bm.progress = progress;
  bm.timestamp = 0;  // ESP32-C3 has no battery-backed RTC; reserved for future use
  snprintf(bm.chapterTitle, sizeof(bm.chapterTitle), "%s", chapterTitle ? chapterTitle : "");
  bm.paragraphIndex = paragraphIndex;
  snprintf(bm.snippet, sizeof(bm.snippet), "%s", snippet ? snippet : "");

  bookmarks.push_back(bm);
  dirty = true;
  saveToFile();
  return AddResult::Added;
}

BookmarkStore::AddResult BookmarkStore::addPdfBookmark(const uint16_t spineIndex, const float progress,
                                                       const char* const chapterTitle, const uint16_t itemId,
                                                       const char* const snippet) {
  if (!isPdfStorePath(storeFilePath)) {
    return AddResult::SaveFailed;
  }
  if (itemId == 0 || itemId == UINT16_MAX ||
      std::any_of(bookmarks.begin(), bookmarks.end(),
                  [itemId](const Bookmark& bookmark) { return bookmark.paragraphIndex == itemId; })) {
    return AddResult::InvalidItemId;
  }
  if (bookmarks.size() >= PDF_BOOKMARK_MAX_PER_BOOK) {
    LOG_ERR("BKS", "PDF bookmark limit (%u) reached", PDF_BOOKMARK_MAX_PER_BOOK);
    return AddResult::LimitReached;
  }
  if (storeFilePath.empty()) {
    return AddResult::SaveFailed;
  }

  Bookmark bookmark{};
  bookmark.spineIndex = spineIndex;
  bookmark.progress = progress;
  bookmark.timestamp = 0;
  snprintf(bookmark.chapterTitle, sizeof(bookmark.chapterTitle), "%s", chapterTitle ? chapterTitle : "");
  bookmark.paragraphIndex = itemId;
  snprintf(bookmark.snippet, sizeof(bookmark.snippet), "%s", snippet ? snippet : "");

  if (bookmarks.size() == bookmarks.capacity()) {
    bookmarks.reserve(bookmarks.size() + 1);
  }
  if (!writePdfTransaction(&bookmark, std::numeric_limits<size_t>::max(), false)) {
    return AddResult::SaveFailed;
  }
  bookmarks.push_back(bookmark);
  dirty = false;
  return AddResult::Added;
}

bool BookmarkStore::removePdfBookmark(const uint16_t itemId) {
  if (!isPdfStorePath(storeFilePath)) return false;
  const auto found = std::find_if(bookmarks.begin(), bookmarks.end(),
                                  [itemId](const Bookmark& bookmark) { return bookmark.paragraphIndex == itemId; });
  if (found == bookmarks.end()) return false;

  const size_t removeIndex = static_cast<size_t>(found - bookmarks.begin());
  if (!writePdfTransaction(nullptr, removeIndex, false)) {
    return false;
  }
  bookmarks.erase(found);
  dirty = false;
  return true;
}

bool BookmarkStore::clearPdfBookmarks() {
  if (!isPdfStorePath(storeFilePath)) return false;
  std::string legacyPath;
  if (!pdfBookmarkStoreFilePathForBook(bookFilePath, true, legacyPath)) return false;
  if (!deletePdfBookmarkStorePaths(storeFilePath, legacyPath)) {
    return false;
  }
  bookmarks.clear();
  dirty = false;
  return true;
}

void BookmarkStore::removeBookmarkForPage(uint16_t spineIndex, float pageProgress, int pageCount) {
  if (pageCount <= 0) return;
  float pageSlice = 1.0f / static_cast<float>(pageCount);
  float pageStart = pageProgress;
  float pageEnd = pageProgress + pageSlice;

  auto it = std::find_if(bookmarks.begin(), bookmarks.end(), [&](const Bookmark& b) {
    return b.spineIndex == spineIndex && b.progress >= pageStart && b.progress < pageEnd;
  });
  if (it == bookmarks.end()) return;

  if (isPdfStorePath(storeFilePath)) {
    removePdfBookmark(it->paragraphIndex);
    return;
  }
  bookmarks.erase(it);
  dirty = true;
  saveToFile();
}

bool BookmarkStore::removeBookmarkAt(size_t index) {
  if (index >= bookmarks.size()) return false;
  if (isPdfStorePath(storeFilePath)) {
    return removePdfBookmark(bookmarks[index].paragraphIndex);
  }

  bookmarks.erase(bookmarks.begin() + index);
  dirty = true;
  saveToFile();
  return true;
}

bool BookmarkStore::hasBookmarkForPage(uint16_t spineIndex, float pageProgress, int pageCount) {
  if (pageCount <= 0) return false;
  float pageSlice = 1.0f / static_cast<float>(pageCount);
  float pageStart = pageProgress;
  float pageEnd = pageProgress + pageSlice;

  return std::any_of(bookmarks.begin(), bookmarks.end(), [&](const Bookmark& b) {
    return b.spineIndex == spineIndex && b.progress >= pageStart && b.progress < pageEnd;
  });
}

void BookmarkStore::saveToFile() {
  if (!dirty || storeFilePath.empty()) return;
  if (bookmarks.empty()) {
    if (Storage.exists(storeFilePath.c_str())) Storage.remove(storeFilePath.c_str());
    dirty = false;
    return;
  }
  if (writeToFile()) dirty = false;
}

void BookmarkStore::clearAll() {
  if (isPdfStorePath(storeFilePath)) {
    clearPdfBookmarks();
    return;
  }
  if (!storeFilePath.empty() && Storage.exists(storeFilePath.c_str())) {
    if (!Storage.remove(storeFilePath.c_str())) {
      LOG_ERR("BKS", "Failed to delete bookmark file");
      return;
    }
  }
  bookmarks.clear();
  dirty = false;
}

bool BookmarkStore::readFromFile() {
  bool needsRewrite = false;
  if (!readFromFile(storeFilePath, bookmarks, needsRewrite)) {
    return false;
  }

  if (needsRewrite) {
    dirty = true;
    saveToFile();
    LOG_DBG("BKS", "Migrated bookmark file to version %u", VERSION);
  }
  return true;
}

bool BookmarkStore::readFromFile(const std::string& path, std::vector<Bookmark>& out, bool& needsRewrite) const {
  needsRewrite = false;
  FsFile f;
  if (!Storage.openFileForRead("BKS", path, f)) {
    LOG_ERR("BKS", "Failed to open bookmark file for read: %s", path.c_str());
    return false;
  }

  uint8_t version = 0;
  serialization::readPod(f, version);
  if (version != LEGACY_VERSION && version != COUNT_U16_VERSION && version != PARAGRAPH_ANCHOR_VERSION &&
      version != VERSION) {
    LOG_ERR("BKS", "Unknown bookmark file version %u: %s", version, path.c_str());
    f.close();
    return false;
  }

  uint16_t count = 0;
  if (!readBookmarkCount(f, version, count)) {
    LOG_ERR("BKS", "Failed to read bookmark count for version %u: %s", version, path.c_str());
    f.close();
    return false;
  }
  if (count > MAX_BOOKMARKS) {
    LOG_ERR("BKS", "Bookmark count %u exceeds max, file may be corrupt: %s", count, path.c_str());
    f.close();
    return false;
  }

  std::string tmp;
  serialization::readString(f, tmp);  // title — not validated
  serialization::readString(f, tmp);  // author — not validated
  std::string storedPath;
  serialization::readString(f, storedPath);
  if (storedPath != bookFilePath) {
    LOG_ERR("BKS", "Bookmark file path mismatch, file may belong to a different book: %s", path.c_str());
    f.close();
    return false;
  }

  std::vector<Bookmark> loadedBookmarks;
  loadedBookmarks.reserve(count);
  for (uint16_t i = 0; i < count; i++) {
    Bookmark bm{};
    if (f.available() < static_cast<int>(sizeof(bm.spineIndex))) {
      LOG_ERR("BKS", "Bookmark file truncated at spineIndex, record %u: %s", i, path.c_str());
      f.close();
      return false;
    }
    serialization::readPod(f, bm.spineIndex);
    if (f.available() < static_cast<int>(sizeof(bm.progress))) {
      LOG_ERR("BKS", "Bookmark file truncated at progress, record %u: %s", i, path.c_str());
      f.close();
      return false;
    }
    serialization::readPod(f, bm.progress);
    if (f.available() < static_cast<int>(sizeof(bm.timestamp))) {
      LOG_ERR("BKS", "Bookmark file truncated at timestamp, record %u: %s", i, path.c_str());
      f.close();
      return false;
    }
    serialization::readPod(f, bm.timestamp);
    const int chRead = f.read(bm.chapterTitle, sizeof(bm.chapterTitle));
    bm.chapterTitle[sizeof(bm.chapterTitle) - 1] = '\0';
    if (chRead != static_cast<int>(sizeof(bm.chapterTitle))) {
      LOG_ERR("BKS", "Bookmark file truncated at chapterTitle, record %u: %s", i, path.c_str());
      f.close();
      return false;
    }
    if (version >= PARAGRAPH_ANCHOR_VERSION) {
      if (f.available() < static_cast<int>(sizeof(bm.paragraphIndex))) {
        LOG_ERR("BKS", "Bookmark file truncated at paragraphIndex, record %u: %s", i, path.c_str());
        f.close();
        return false;
      }
      serialization::readPod(f, bm.paragraphIndex);
    } else {
      bm.paragraphIndex = UINT16_MAX;
    }
    if (version >= VERSION) {
      const int snippetRead = f.read(bm.snippet, sizeof(bm.snippet));
      bm.snippet[sizeof(bm.snippet) - 1] = '\0';
      if (snippetRead != static_cast<int>(sizeof(bm.snippet))) {
        LOG_ERR("BKS", "Bookmark file truncated at snippet, record %u: %s", i, path.c_str());
        f.close();
        return false;
      }
    } else {
      bm.snippet[0] = '\0';
    }
    loadedBookmarks.push_back(bm);
  }

  const bool closed = f.close();
  if (!closed && isPdfStorePath(path)) {
    LOG_ERR("BKS", "Failed to close PDF bookmark file after read: %s", path.c_str());
    return false;
  }
  out = std::move(loadedBookmarks);
  needsRewrite = version != VERSION;
  return true;
}

bool BookmarkStore::writeToFile() const {
  Storage.mkdir(BOOKMARKS_DIR);

  FsFile f;
  if (!Storage.openFileForWrite("BKS", storeFilePath, f)) {
    LOG_ERR("BKS", "Failed to open bookmark file for write");
    return false;
  }

  const uint16_t count = static_cast<uint16_t>(bookmarks.size());
  serialization::writePod(f, VERSION);
  serialization::writePod(f, count);
  serialization::writeString(f, bookTitle);
  serialization::writeString(f, bookAuthor);
  serialization::writeString(f, bookFilePath);

  for (const auto& bm : bookmarks) {
    serialization::writePod(f, bm.spineIndex);
    serialization::writePod(f, bm.progress);
    serialization::writePod(f, bm.timestamp);
    f.write(reinterpret_cast<const uint8_t*>(bm.chapterTitle), sizeof(bm.chapterTitle));
    serialization::writePod(f, bm.paragraphIndex);
    f.write(reinterpret_cast<const uint8_t*>(bm.snippet), sizeof(bm.snippet));
  }

  f.close();
  return true;
}

bool BookmarkStore::writeMigrationPayload(void* const fileContext) const {
  if (fileContext == nullptr) return false;
  auto& file = *static_cast<FsFile*>(fileContext);
  if (bookmarks.size() > MAX_BOOKMARKS) return false;
  const uint16_t count = static_cast<uint16_t>(bookmarks.size());
  bool wrote = serialization::tryWritePod(file, VERSION) && serialization::tryWritePod(file, count) &&
               serialization::tryWriteString(file, bookTitle) && serialization::tryWriteString(file, bookAuthor) &&
               serialization::tryWriteString(file, bookFilePath);
  for (const Bookmark& bookmark : bookmarks) {
    wrote = wrote && serialization::tryWritePod(file, bookmark.spineIndex) &&
            serialization::tryWritePod(file, bookmark.progress) &&
            serialization::tryWritePod(file, bookmark.timestamp) &&
            file.write(reinterpret_cast<const uint8_t*>(bookmark.chapterTitle), sizeof(bookmark.chapterTitle)) ==
                sizeof(bookmark.chapterTitle) &&
            serialization::tryWritePod(file, bookmark.paragraphIndex) &&
            file.write(reinterpret_cast<const uint8_t*>(bookmark.snippet), sizeof(bookmark.snippet)) ==
                sizeof(bookmark.snippet);
    if (!wrote) return false;
  }
  return true;
}

bool BookmarkStore::writePdfTransaction(const Bookmark* const appended, const size_t removeIndex,
                                        const bool clear) const {
  if (storeFilePath.empty()) {
    LOG_ERR("BKS", "Cannot persist PDF bookmarks without a store path");
    return false;
  }
  if (!isPdfStorePath(storeFilePath)) {
    LOG_ERR("BKS", "Refusing PDF bookmark mutation for a non-PDF store");
    return false;
  }

  const std::string temporaryPath = storeFilePath + PDF_TRANSACTION_TEMP_SUFFIX;
  const std::string backupPath = storeFilePath + PDF_TRANSACTION_BACKUP_SUFFIX;
  if (!Storage.exists(storeFilePath.c_str()) && Storage.exists(backupPath.c_str()) &&
      !Storage.rename(backupPath.c_str(), storeFilePath.c_str())) {
    LOG_ERR("BKS", "Failed to restore PDF bookmark backup before mutation: %s", backupPath.c_str());
    return false;
  }
  if (Storage.exists(temporaryPath.c_str()) && !Storage.remove(temporaryPath.c_str())) {
    LOG_ERR("BKS", "Failed to remove stale PDF bookmark transaction: %s", temporaryPath.c_str());
    return false;
  }
  if (Storage.exists(backupPath.c_str()) && !Storage.remove(backupPath.c_str())) {
    LOG_ERR("BKS", "Failed to remove stale PDF bookmark backup: %s", backupPath.c_str());
    return false;
  }

  Storage.mkdir(BOOKMARKS_DIR);
  FsFile file;
  if (!Storage.openFileForWrite("BKS", temporaryPath, file)) {
    LOG_ERR("BKS", "Failed to open PDF bookmark transaction: %s", temporaryPath.c_str());
    return false;
  }

  const size_t logicalCount = pdfBookmarkCount(bookmarks, appended, removeIndex, clear);
  if (logicalCount > PDF_BOOKMARK_MAX_PER_BOOK) {
    LOG_ERR("BKS", "PDF bookmark transaction count exceeds limit: %u", static_cast<unsigned>(logicalCount));
    if (!file.close()) {
      LOG_ERR("BKS", "Failed to close rejected PDF bookmark transaction: %s", temporaryPath.c_str());
    }
    Storage.remove(temporaryPath.c_str());
    return false;
  }
  const uint16_t count = static_cast<uint16_t>(logicalCount);
  bool wrote = serialization::tryWritePod(file, VERSION) && serialization::tryWritePod(file, count) &&
               serialization::tryWriteString(file, bookTitle) && serialization::tryWriteString(file, bookAuthor) &&
               serialization::tryWriteString(file, bookFilePath);
  for (size_t index = 0; wrote && index < logicalCount; ++index) {
    const Bookmark* const bookmark = pdfBookmarkAt(bookmarks, appended, removeIndex, index);
    wrote = bookmark != nullptr && serialization::tryWritePod(file, bookmark->spineIndex) &&
            serialization::tryWritePod(file, bookmark->progress) &&
            serialization::tryWritePod(file, bookmark->timestamp) &&
            file.write(reinterpret_cast<const uint8_t*>(bookmark->chapterTitle), sizeof(bookmark->chapterTitle)) ==
                sizeof(bookmark->chapterTitle) &&
            serialization::tryWritePod(file, bookmark->paragraphIndex) &&
            file.write(reinterpret_cast<const uint8_t*>(bookmark->snippet), sizeof(bookmark->snippet)) ==
                sizeof(bookmark->snippet);
  }

  file.flush();
  const bool synced = file.sync();
  const bool closed = file.close();
  if (!wrote || !synced || !closed) {
    LOG_ERR("BKS", "Failed to durably write PDF bookmark transaction: %s", temporaryPath.c_str());
    Storage.remove(temporaryPath.c_str());
    return false;
  }
  if (!verifyPdfTransaction(temporaryPath, appended, removeIndex, clear)) {
    LOG_ERR("BKS", "PDF bookmark transaction readback failed: %s", temporaryPath.c_str());
    Storage.remove(temporaryPath.c_str());
    return false;
  }

  const bool hadCanonical = Storage.exists(storeFilePath.c_str());
  if (hadCanonical && !Storage.rename(storeFilePath.c_str(), backupPath.c_str())) {
    LOG_ERR("BKS", "Failed to back up PDF bookmark store: %s", storeFilePath.c_str());
    Storage.remove(temporaryPath.c_str());
    return false;
  }
  if (!Storage.rename(temporaryPath.c_str(), storeFilePath.c_str())) {
    LOG_ERR("BKS", "Failed to promote PDF bookmark transaction: %s", temporaryPath.c_str());
    if (hadCanonical && !Storage.rename(backupPath.c_str(), storeFilePath.c_str())) {
      LOG_ERR("BKS", "Failed to roll back PDF bookmark transaction: %s", backupPath.c_str());
    }
    return false;
  }

  if (hadCanonical && Storage.exists(backupPath.c_str()) && !Storage.remove(backupPath.c_str())) {
    // The new canonical file is already durable. Recovery treats it as authoritative
    // and removes this backup on the next load or mutation.
    LOG_ERR("BKS", "Failed to remove committed PDF bookmark backup: %s", backupPath.c_str());
  }
  return true;
}

bool BookmarkStore::verifyPdfTransaction(const std::string& path, const Bookmark* const appended,
                                         const size_t removeIndex, const bool clear) const {
  FsFile file;
  if (!Storage.openFileForRead("BKS", path, file)) {
    return false;
  }

  const size_t logicalCount = pdfBookmarkCount(bookmarks, appended, removeIndex, clear);
  const uint16_t count = static_cast<uint16_t>(logicalCount);
  bool matches = readExpectedPod(file, VERSION) && readExpectedPod(file, count) &&
                 readExpectedString(file, bookTitle) && readExpectedString(file, bookAuthor) &&
                 readExpectedString(file, bookFilePath);
  for (size_t index = 0; matches && index < logicalCount; ++index) {
    const Bookmark* const bookmark = pdfBookmarkAt(bookmarks, appended, removeIndex, index);
    matches = bookmark != nullptr && readExpectedPod(file, bookmark->spineIndex) &&
              readExpectedPod(file, bookmark->progress) && readExpectedPod(file, bookmark->timestamp) &&
              readExpectedBytes(file, bookmark->chapterTitle, sizeof(bookmark->chapterTitle)) &&
              readExpectedPod(file, bookmark->paragraphIndex) &&
              readExpectedBytes(file, bookmark->snippet, sizeof(bookmark->snippet));
  }
  matches = matches && file.available() == 0;
  const bool closed = file.close();
  return matches && closed;
}

bool BookmarkStore::recoverPdfTransaction() const {
  if (storeFilePath.empty()) return false;

  const std::string temporaryPath = storeFilePath + PDF_TRANSACTION_TEMP_SUFFIX;
  const std::string backupPath = storeFilePath + PDF_TRANSACTION_BACKUP_SUFFIX;
  if (Storage.exists(storeFilePath.c_str())) {
    bool recovered = true;
    if (Storage.exists(temporaryPath.c_str()) && !Storage.remove(temporaryPath.c_str())) {
      LOG_ERR("BKS", "Failed to clean stale PDF bookmark transaction: %s", temporaryPath.c_str());
      recovered = false;
    }
    if (Storage.exists(backupPath.c_str()) && !Storage.remove(backupPath.c_str())) {
      LOG_ERR("BKS", "Failed to clean stale PDF bookmark backup: %s", backupPath.c_str());
      recovered = false;
    }
    return recovered;
  }

  if (Storage.exists(backupPath.c_str())) {
    if (!Storage.rename(backupPath.c_str(), storeFilePath.c_str())) {
      LOG_ERR("BKS", "Failed to restore PDF bookmark backup: %s", backupPath.c_str());
      return false;
    }
    if (Storage.exists(temporaryPath.c_str()) && !Storage.remove(temporaryPath.c_str())) {
      LOG_ERR("BKS", "Failed to clean rolled-back PDF bookmark transaction: %s", temporaryPath.c_str());
      return false;
    }
    return true;
  }

  if (Storage.exists(temporaryPath.c_str()) && !Storage.remove(temporaryPath.c_str())) {
    LOG_ERR("BKS", "Failed to discard incomplete PDF bookmark transaction: %s", temporaryPath.c_str());
    return false;
  }
  return true;
}

bool BookmarkStore::deleteForFilePath(const std::string& filePath, const std::string& bookType) {
  if (bookType == "pdf") {
    std::string currentPath;
    std::string legacyPath;
    if (!pdfBookmarkStoreFilePathForBook(filePath, false, currentPath) ||
        !pdfBookmarkStoreFilePathForBook(filePath, true, legacyPath)) {
      return false;
    }
    const bool deleted = deletePdfBookmarkStorePaths(currentPath, legacyPath);
    if (deleted) {
      LOG_DBG("BKS", "Deleted PDF bookmark files for: %s", filePath.c_str());
    }
    return deleted;
  }

  const std::string currentPath = currentStoreFilePathForBook(filePath, bookType);
  const std::string legacyPath = legacyStoreFilePathForBook(filePath, bookType);
  bool deletedAny = false;
  bool success = true;

  if (Storage.exists(currentPath.c_str())) {
    const bool deleted = deleteBookmarkStorePath(currentPath, "canonical");
    deletedAny = deleted || deletedAny;
    success = success && deleted;
  }
  if (legacyPath != currentPath && Storage.exists(legacyPath.c_str())) {
    const bool deleted = deleteBookmarkStorePath(legacyPath, "legacy");
    deletedAny = deleted || deletedAny;
    success = success && deleted;
  }

  if (deletedAny) {
  }
  return success;
}

bool BookmarkStore::deleteLegacyForFilePathNoPathAlloc(const std::string_view filePath,
                                                       const std::string_view bookType) {
  if (filePath.size() > std::numeric_limits<unsigned int>::max() ||
      bookType.size() > static_cast<size_t>(std::numeric_limits<int>::max())) {
    return false;
  }

  char currentPath[64];
  char legacyPath[64];
  const uint32_t crc = uzlib_crc32(filePath.data(), static_cast<unsigned int>(filePath.size()), 0);
  const size_t legacyHash = std::hash<std::string_view>{}(filePath);
  const int currentWritten =
      snprintf(currentPath, sizeof(currentPath), "%s/%.*s_%lu.bin", BOOKMARKS_DIR, static_cast<int>(bookType.size()),
               bookType.data(), static_cast<unsigned long>(crc));
  const int legacyWritten =
      snprintf(legacyPath, sizeof(legacyPath), "%s/%.*s_%llu.bin", BOOKMARKS_DIR, static_cast<int>(bookType.size()),
               bookType.data(), static_cast<unsigned long long>(legacyHash));
  if (currentWritten < 0 || static_cast<size_t>(currentWritten) >= sizeof(currentPath) || legacyWritten < 0 ||
      static_cast<size_t>(legacyWritten) >= sizeof(legacyPath)) {
    LOG_ERR("BKS", "Legacy bookmark store path exceeds %u bytes", static_cast<unsigned>(sizeof(currentPath)));
    return false;
  }

  bool deletedAny = false;
  bool success = true;
  if (Storage.exists(currentPath)) {
    const bool deleted = deleteBookmarkStorePathNoPathAlloc(currentPath, "canonical");
    deletedAny = deleted || deletedAny;
    success = success && deleted;
  }
  if (std::strcmp(legacyPath, currentPath) != 0 && Storage.exists(legacyPath)) {
    const bool deleted = deleteBookmarkStorePathNoPathAlloc(legacyPath, "legacy");
    deletedAny = deleted || deletedAny;
    success = success && deleted;
  }
  if (deletedAny) {
    LOG_DBG("BKS", "Deleted bookmark file for: %.*s", static_cast<int>(filePath.size()), filePath.data());
  }
  return success;
}

bool BookmarkStore::deletePdfForFilePathNoPathAlloc(const std::string_view filePath) {
  if (filePath.size() > std::numeric_limits<unsigned int>::max()) return false;

  char currentPath[64];
  char legacyPath[64];
  char artifactPath[68];
  const uint32_t currentHash = uzlib_crc32(filePath.data(), static_cast<unsigned int>(filePath.size()), 0);
  const size_t legacyHash = std::hash<std::string_view>{}(filePath);
  const int currentWritten = snprintf(currentPath, sizeof(currentPath), "%s/pdf_%lu.bin", BOOKMARKS_DIR,
                                      static_cast<unsigned long>(currentHash));
  const int legacyWritten = snprintf(legacyPath, sizeof(legacyPath), "%s/pdf_%llu.bin", BOOKMARKS_DIR,
                                     static_cast<unsigned long long>(legacyHash));
  if (currentWritten < 0 || static_cast<size_t>(currentWritten) >= sizeof(currentPath) || legacyWritten < 0 ||
      static_cast<size_t>(legacyWritten) >= sizeof(legacyPath) ||
      static_cast<size_t>(currentWritten) + sizeof(PDF_TRANSACTION_BACKUP_SUFFIX) > sizeof(artifactPath) ||
      static_cast<size_t>(legacyWritten) + sizeof(PDF_TRANSACTION_BACKUP_SUFFIX) > sizeof(artifactPath)) {
    LOG_ERR("BKS", "PDF bookmark store path exceeds bounded delete buffers");
    return false;
  }

  const auto deleteArtifact = [&](const char* const base, const char* const suffix, const char* const description) {
    const int written = snprintf(artifactPath, sizeof(artifactPath), "%s%s", base, suffix);
    return written > 0 && static_cast<size_t>(written) < sizeof(artifactPath) &&
           deleteBookmarkStorePathNoPathAlloc(artifactPath, description);
  };

  // Match deletePdfBookmarkStorePaths exactly: rollback material first and
  // the authoritative canonical path last.
  if (!deleteArtifact(currentPath, PDF_TRANSACTION_BACKUP_SUFFIX, "PDF backup") ||
      !deleteArtifact(currentPath, PDF_TRANSACTION_TEMP_SUFFIX, "PDF temporary")) {
    return false;
  }
  if (std::strcmp(legacyPath, currentPath) != 0 &&
      (!deleteArtifact(legacyPath, PDF_TRANSACTION_BACKUP_SUFFIX, "legacy PDF backup") ||
       !deleteArtifact(legacyPath, PDF_TRANSACTION_TEMP_SUFFIX, "legacy PDF temporary") ||
       !deleteBookmarkStorePathNoPathAlloc(legacyPath, "legacy PDF canonical"))) {
    return false;
  }
  const bool deleted = deleteBookmarkStorePathNoPathAlloc(currentPath, "PDF canonical");
  if (deleted) {
    LOG_DBG("BKS", "Deleted PDF bookmark files for: %.*s", static_cast<int>(filePath.size()), filePath.data());
  }
  return deleted;
}

bool BookmarkStore::copyForFilePath(const std::string& oldFilePath, const std::string& newFilePath,
                                    const std::string& bookType) {
  if (bookType != "pdf" || oldFilePath.empty() || newFilePath.empty() || oldFilePath == newFilePath) {
    return oldFilePath == newFilePath && bookType == "pdf";
  }

  // Both record vectors plus Store/path controls are cold-path state. A single
  // fallible allocation avoids a multi-hundred-byte activity stack frame.
  auto scratch = makeUniqueNoThrow<PdfBookmarkMigrationScratch>();
  if (!scratch) {
    LOG_ERR("BKS", "Out of memory allocating bookmark copy scratch");
    return false;
  }

  if (!pdfBookmarkStoreFilePathForBook(oldFilePath, false, scratch->sourceCurrentPath) ||
      !pdfBookmarkStoreFilePathForBook(oldFilePath, true, scratch->sourceLegacyPath) ||
      !pdfBookmarkStoreFilePathForBook(newFilePath, false, scratch->destinationCurrentPath) ||
      !pdfBookmarkStoreFilePathForBook(newFilePath, true, scratch->destinationLegacyPath)) {
    return false;
  }
  if (scratch->sourceCurrentPath == scratch->destinationCurrentPath ||
      scratch->sourceLegacyPath == scratch->destinationCurrentPath) {
    LOG_ERR("BKS", "Refusing bookmark copy across a path-key collision");
    return false;
  }

  scratch->store.bookFilePath = oldFilePath;
  scratch->store.storeFilePath = scratch->sourceCurrentPath;
  if (!scratch->store.recoverPdfTransaction()) return false;
  if (scratch->sourceLegacyPath != scratch->sourceCurrentPath) {
    scratch->store.storeFilePath = scratch->sourceLegacyPath;
    if (!scratch->store.recoverPdfTransaction()) return false;
  }

  const bool hasCurrent = Storage.exists(scratch->sourceCurrentPath.c_str());
  const bool hasLegacy =
      scratch->sourceLegacyPath != scratch->sourceCurrentPath && Storage.exists(scratch->sourceLegacyPath.c_str());
  if (!hasCurrent && !hasLegacy) {
    return true;
  }

  BookmarkFileHeader header;
  const std::string& headerPath = hasCurrent ? scratch->sourceCurrentPath : scratch->sourceLegacyPath;
  const std::string headerName = fileNameFromPath(headerPath);
  if (!readBookmarkFileHeader(headerPath, headerName.c_str(), header) || header.path != oldFilePath) {
    LOG_ERR("BKS", "Failed to read source bookmark metadata before copy: %s", headerPath.c_str());
    return false;
  }

  bool needsRewrite = false;
  if (hasCurrent && !scratch->store.readFromFile(scratch->sourceCurrentPath, scratch->primary, needsRewrite)) {
    return false;
  }
  if (hasLegacy) {
    bool legacyNeedsRewrite = false;
    if (!scratch->store.readFromFile(scratch->sourceLegacyPath, scratch->secondary, legacyNeedsRewrite)) {
      return false;
    }
    // Match normal PDF load authority: the canonical current record wins a
    // stable-ID collision; legacy contributes only IDs absent from current.
    mergeBookmarks(scratch->primary, scratch->secondary, true);
  }

  scratch->secondary.clear();
  scratch->store.bookFilePath = newFilePath;
  scratch->store.storeFilePath = scratch->destinationCurrentPath;
  scratch->temporaryPath = scratch->destinationCurrentPath + PDF_TRANSACTION_TEMP_SUFFIX;
  scratch->backupPath = scratch->destinationCurrentPath + PDF_TRANSACTION_BACKUP_SUFFIX;
  const char* destinationSnapshot = nullptr;
  if (Storage.exists(scratch->destinationCurrentPath.c_str())) {
    destinationSnapshot = scratch->destinationCurrentPath.c_str();
  } else if (Storage.exists(scratch->backupPath.c_str())) {
    destinationSnapshot = scratch->backupPath.c_str();
  }
  if (destinationSnapshot != nullptr) {
    bool destinationNeedsRewrite = false;
    if (!scratch->store.readFromFile(destinationSnapshot, scratch->secondary, destinationNeedsRewrite)) {
      // The old-path snapshot is authoritative until activation. A torn
      // destination is replaced from it instead of blocking every retry.
      scratch->secondary.clear();
    }
  }
  if (!mergeAuthoritativeBookmarks(scratch->secondary, scratch->primary, true)) {
    return false;
  }

  scratch->store.bookTitle = std::move(header.title);
  scratch->store.bookAuthor = std::move(header.author);
  scratch->store.bookmarks = std::move(scratch->secondary);
  const BookMoveDurableFile::Payload payload{
      &scratch->store,
      [](void* context, void* fileContext) {
        return static_cast<BookmarkStore*>(context)->writeMigrationPayload(fileContext);
      },
      [](void* context, const char* path) {
        return static_cast<BookmarkStore*>(context)->verifyPdfTransaction(path, nullptr,
                                                                          std::numeric_limits<size_t>::max(), false);
      }};
  Storage.mkdir(BOOKMARKS_DIR);
  const bool wrote = BookMoveDurableFile::replace(scratch->destinationCurrentPath.c_str(),
                                                  scratch->temporaryPath.c_str(), scratch->backupPath.c_str(), payload);
  if (!wrote) {
    LOG_ERR("BKS", "Failed to copy bookmark state: %s -> %s", oldFilePath.c_str(), newFilePath.c_str());
  }
  return wrote;
}

bool BookmarkStore::verifyCopyForFilePath(const std::string& oldFilePath, const std::string& newFilePath,
                                          const std::string& bookType) {
  if (bookType != "pdf" || oldFilePath.empty() || newFilePath.empty() || oldFilePath == newFilePath) {
    return oldFilePath == newFilePath && bookType == "pdf";
  }

  auto scratch = makeUniqueNoThrow<PdfBookmarkMigrationScratch>();
  if (!scratch) {
    LOG_ERR("BKS", "Out of memory allocating bookmark verification scratch");
    return false;
  }
  if (!pdfBookmarkStoreFilePathForBook(oldFilePath, false, scratch->sourceCurrentPath) ||
      !pdfBookmarkStoreFilePathForBook(oldFilePath, true, scratch->sourceLegacyPath) ||
      !pdfBookmarkStoreFilePathForBook(newFilePath, false, scratch->destinationCurrentPath) ||
      !pdfBookmarkStoreFilePathForBook(newFilePath, true, scratch->destinationLegacyPath)) {
    return false;
  }

  scratch->store.bookFilePath = oldFilePath;
  bool needsRewrite = false;
  if (Storage.exists(scratch->sourceCurrentPath.c_str()) &&
      !scratch->store.readFromFile(scratch->sourceCurrentPath, scratch->primary, needsRewrite)) {
    return false;
  }
  if (scratch->sourceLegacyPath != scratch->sourceCurrentPath && Storage.exists(scratch->sourceLegacyPath.c_str())) {
    bool legacyNeedsRewrite = false;
    if (!scratch->store.readFromFile(scratch->sourceLegacyPath, scratch->secondary, legacyNeedsRewrite)) {
      return false;
    }
    // Verification must derive the same canonical-authoritative source view as
    // copyForFilePath, or both can agree on stale legacy payload.
    mergeBookmarks(scratch->primary, scratch->secondary, true);
  }
  if (scratch->primary.empty()) {
    return true;
  }

  scratch->secondary.clear();
  scratch->store.bookFilePath = newFilePath;
  if (!Storage.exists(scratch->destinationCurrentPath.c_str())) {
    return false;
  }
  bool destinationNeedsRewrite = false;
  return scratch->store.readFromFile(scratch->destinationCurrentPath, scratch->secondary, destinationNeedsRewrite) &&
         containsAllBookmarks(scratch->secondary, scratch->primary);
}

bool BookmarkStore::migratePdfForFilePath(const std::string& oldFilePath, const std::string& newFilePath,
                                          const std::string& title, const std::string& author) {
  // A rename is a cold operation. Keep its Store/vector/path control objects in
  // one fallible allocation instead of consuming most of the activity stack.
  auto scratch = makeUniqueNoThrow<PdfBookmarkMigrationScratch>();
  if (!scratch) {
    LOG_ERR("BKS", "Out of memory allocating PDF bookmark migration scratch");
    return false;
  }

  if (!pdfBookmarkStoreFilePathForBook(oldFilePath, false, scratch->sourceCurrentPath) ||
      !pdfBookmarkStoreFilePathForBook(oldFilePath, true, scratch->sourceLegacyPath) ||
      !pdfBookmarkStoreFilePathForBook(newFilePath, false, scratch->destinationCurrentPath) ||
      !pdfBookmarkStoreFilePathForBook(newFilePath, true, scratch->destinationLegacyPath)) {
    return false;
  }

  scratch->store.bookFilePath = oldFilePath;
  scratch->store.storeFilePath = scratch->sourceCurrentPath;
  if (!scratch->store.recoverPdfTransaction()) {
    LOG_ERR("BKS", "Failed to recover source PDF bookmarks before migration: %s", scratch->sourceCurrentPath.c_str());
    return false;
  }
  if (scratch->sourceLegacyPath != scratch->sourceCurrentPath) {
    scratch->store.storeFilePath = scratch->sourceLegacyPath;
    if (!scratch->store.recoverPdfTransaction()) {
      LOG_ERR("BKS", "Failed to recover source legacy PDF bookmarks before migration: %s",
              scratch->sourceLegacyPath.c_str());
      return false;
    }
  }

  const bool hasSourceCurrent = Storage.exists(scratch->sourceCurrentPath.c_str());
  const bool hasSourceLegacy =
      scratch->sourceLegacyPath != scratch->sourceCurrentPath && Storage.exists(scratch->sourceLegacyPath.c_str());
  if (!hasSourceCurrent && !hasSourceLegacy) {
    return true;
  }

  scratch->store.bookFilePath = newFilePath;
  scratch->store.storeFilePath = scratch->destinationCurrentPath;
  if (!scratch->store.recoverPdfTransaction()) {
    LOG_ERR("BKS", "Failed to recover destination PDF bookmarks before migration: %s",
            scratch->destinationCurrentPath.c_str());
    return false;
  }
  if (scratch->destinationLegacyPath != scratch->destinationCurrentPath) {
    scratch->store.storeFilePath = scratch->destinationLegacyPath;
    if (!scratch->store.recoverPdfTransaction()) {
      LOG_ERR("BKS", "Failed to recover destination legacy PDF bookmarks before migration: %s",
              scratch->destinationLegacyPath.c_str());
      return false;
    }
  }

  scratch->store.bookFilePath = oldFilePath;
  bool sourceNeedsRewrite = false;
  if (hasSourceCurrent &&
      !scratch->store.readFromFile(scratch->sourceCurrentPath, scratch->primary, sourceNeedsRewrite)) {
    LOG_ERR("BKS", "Failed to load source PDF bookmarks during migration: %s", scratch->sourceCurrentPath.c_str());
    return false;
  }
  if (hasSourceLegacy) {
    bool legacyNeedsRewrite = false;
    if (!scratch->store.readFromFile(scratch->sourceLegacyPath, scratch->secondary, legacyNeedsRewrite)) {
      LOG_ERR("BKS", "Failed to load source legacy PDF bookmarks during migration: %s",
              scratch->sourceLegacyPath.c_str());
      return false;
    }
    mergeBookmarks(scratch->primary, scratch->secondary, true);
    sourceNeedsRewrite = sourceNeedsRewrite || legacyNeedsRewrite;
  }

  scratch->store.bookFilePath = newFilePath;
  if (scratch->sourceCurrentPath == scratch->destinationCurrentPath) {
    if (scratch->destinationLegacyPath != scratch->destinationCurrentPath &&
        Storage.exists(scratch->destinationLegacyPath.c_str())) {
      bool destinationLegacyNeedsRewrite = false;
      if (!scratch->store.readFromFile(scratch->destinationLegacyPath, scratch->secondary,
                                       destinationLegacyNeedsRewrite)) {
        LOG_ERR("BKS", "Failed to load destination legacy PDF bookmarks during collision retag: %s",
                scratch->destinationLegacyPath.c_str());
        return false;
      }
      mergeBookmarks(scratch->secondary, scratch->primary, true);
      scratch->primary.swap(scratch->secondary);
    }

    scratch->store.bookTitle = title;
    scratch->store.bookAuthor = author;
    scratch->store.storeFilePath = scratch->destinationCurrentPath;
    scratch->store.bookmarks = std::move(scratch->primary);
    if (!scratch->store.writePdfTransaction(nullptr, std::numeric_limits<size_t>::max(), false)) {
      LOG_ERR("BKS", "Failed to durably retag colliding PDF bookmarks: %s", scratch->destinationCurrentPath.c_str());
      return false;
    }
    if (scratch->destinationLegacyPath != scratch->destinationCurrentPath &&
        !deletePdfBookmarkStorePaths(scratch->destinationLegacyPath, scratch->destinationLegacyPath)) {
      return false;
    }
    if (scratch->sourceLegacyPath != scratch->sourceCurrentPath &&
        scratch->sourceLegacyPath != scratch->destinationLegacyPath &&
        !deletePdfBookmarkStorePaths(scratch->sourceLegacyPath, scratch->sourceLegacyPath)) {
      return false;
    }
    LOG_INF("BKS", "Retagged colliding PDF bookmark path: %s -> %s (%u bookmark(s))", oldFilePath.c_str(),
            newFilePath.c_str(), static_cast<unsigned>(scratch->store.bookmarks.size()));
    return true;
  }

  bool hasDestinationBookmarks = false;
  if (Storage.exists(scratch->destinationCurrentPath.c_str())) {
    bool destinationNeedsRewrite = false;
    if (!scratch->store.readFromFile(scratch->destinationCurrentPath, scratch->secondary, destinationNeedsRewrite)) {
      LOG_ERR("BKS", "Failed to load destination PDF bookmarks during migration: %s",
              scratch->destinationCurrentPath.c_str());
      return false;
    }
    mergeBookmarks(scratch->secondary, scratch->primary, true);
    scratch->primary.swap(scratch->secondary);
    hasDestinationBookmarks = true;
  }
  if (scratch->destinationLegacyPath != scratch->destinationCurrentPath &&
      Storage.exists(scratch->destinationLegacyPath.c_str())) {
    bool destinationLegacyNeedsRewrite = false;
    if (!scratch->store.readFromFile(scratch->destinationLegacyPath, scratch->secondary,
                                     destinationLegacyNeedsRewrite)) {
      LOG_ERR("BKS", "Failed to load destination legacy PDF bookmarks during migration: %s",
              scratch->destinationLegacyPath.c_str());
      return false;
    }
    mergeBookmarks(scratch->secondary, scratch->primary, true);
    scratch->primary.swap(scratch->secondary);
    hasDestinationBookmarks = true;
  }

  scratch->store.bookTitle = title;
  scratch->store.bookAuthor = author;
  scratch->store.storeFilePath = scratch->destinationCurrentPath;
  scratch->store.bookmarks = std::move(scratch->primary);
  if (!scratch->store.writePdfTransaction(nullptr, std::numeric_limits<size_t>::max(), false)) {
    LOG_ERR("BKS", "Failed to durably write migrated PDF bookmarks: %s", scratch->destinationCurrentPath.c_str());
    return false;
  }

  if (scratch->destinationLegacyPath != scratch->destinationCurrentPath &&
      !deletePdfBookmarkStorePaths(scratch->destinationLegacyPath, scratch->destinationLegacyPath)) {
    return false;
  }
  if (scratch->sourceCurrentPath != scratch->destinationCurrentPath &&
      !deletePdfBookmarkStorePaths(scratch->sourceCurrentPath, scratch->sourceLegacyPath)) {
    return false;
  }

  LOG_INF("BKS", "Migrated PDF bookmark path: %s -> %s (%u bookmark(s)%s%s)", oldFilePath.c_str(), newFilePath.c_str(),
          static_cast<unsigned>(scratch->store.bookmarks.size()),
          hasDestinationBookmarks ? ", merged with destination" : "", sourceNeedsRewrite ? ", normalized format" : "");
  return true;
}

bool BookmarkStore::migrateLegacyForFilePath(const std::string& oldFilePath, const std::string& newFilePath,
                                             const std::string& title, const std::string& author,
                                             const std::string& bookType) {
  const std::string srcCurrentPath = currentStoreFilePathForBook(oldFilePath, bookType);
  const std::string srcLegacyPath = legacyStoreFilePathForBook(oldFilePath, bookType);
  const std::string dstCurrentPath = currentStoreFilePathForBook(newFilePath, bookType);
  const std::string dstLegacyPath = legacyStoreFilePathForBook(newFilePath, bookType);

  const bool hasSrcCurrent = Storage.exists(srcCurrentPath.c_str());
  const bool hasSrcLegacy = srcLegacyPath != srcCurrentPath && Storage.exists(srcLegacyPath.c_str());
  if (!hasSrcCurrent && !hasSrcLegacy) {
    return true;
  }

  BookmarkStore sourceReader;
  sourceReader.bookFilePath = oldFilePath;
  std::vector<Bookmark> migratedBookmarks;
  bool sourceNeedsRewrite = false;
  bool loadedSourceBookmarks = false;

  if (hasSrcCurrent) {
    bool currentNeedsRewrite = false;
    if (!sourceReader.readFromFile(srcCurrentPath, migratedBookmarks, currentNeedsRewrite)) {
      LOG_ERR("BKS", "Failed to load source bookmark file during path migration: %s", srcCurrentPath.c_str());
      return false;
    }
    sourceNeedsRewrite = currentNeedsRewrite;
    loadedSourceBookmarks = true;
  }

  if (hasSrcLegacy) {
    bool legacyNeedsRewrite = false;
    std::vector<Bookmark> legacyBookmarks;
    if (!sourceReader.readFromFile(srcLegacyPath, legacyBookmarks, legacyNeedsRewrite)) {
      LOG_ERR("BKS", "Failed to load source legacy bookmark file during path migration: %s", srcLegacyPath.c_str());
      return false;
    }
    mergeBookmarks(migratedBookmarks, legacyBookmarks, bookType == "pdf");
    sourceNeedsRewrite = sourceNeedsRewrite || legacyNeedsRewrite;
    loadedSourceBookmarks = true;
  }

  if (!loadedSourceBookmarks) {
    return true;
  }

  BookmarkStore destReader;
  destReader.bookFilePath = newFilePath;
  bool hasDestBookmarks = false;

  if (Storage.exists(dstCurrentPath.c_str())) {
    bool dstNeedsRewrite = false;
    std::vector<Bookmark> existingBookmarks;
    if (!destReader.readFromFile(dstCurrentPath, existingBookmarks, dstNeedsRewrite)) {
      LOG_ERR("BKS", "Failed to load destination bookmark file during path migration: %s", dstCurrentPath.c_str());
      return false;
    }
    mergeBookmarks(existingBookmarks, migratedBookmarks, bookType == "pdf");
    migratedBookmarks = std::move(existingBookmarks);
    hasDestBookmarks = true;
  }

  if (dstLegacyPath != dstCurrentPath && Storage.exists(dstLegacyPath.c_str())) {
    bool dstLegacyNeedsRewrite = false;
    std::vector<Bookmark> existingLegacyBookmarks;
    if (!destReader.readFromFile(dstLegacyPath, existingLegacyBookmarks, dstLegacyNeedsRewrite)) {
      LOG_ERR("BKS", "Failed to load destination legacy bookmark file during path migration: %s",
              dstLegacyPath.c_str());
      return false;
    }
    mergeBookmarks(existingLegacyBookmarks, migratedBookmarks, bookType == "pdf");
    migratedBookmarks = std::move(existingLegacyBookmarks);
    hasDestBookmarks = true;
  }

  BookmarkStore writer;
  writer.bookFilePath = newFilePath;
  writer.bookTitle = title;
  writer.bookAuthor = author;
  writer.storeFilePath = dstCurrentPath;
  writer.bookmarks = std::move(migratedBookmarks);

  if (!writer.bookmarks.empty()) {
    if (!writer.writeToFile()) {
      LOG_ERR("BKS", "Failed to write migrated bookmark file: %s", dstCurrentPath.c_str());
      return false;
    }
  } else if (Storage.exists(dstCurrentPath.c_str()) && !deleteBookmarkStorePath(dstCurrentPath, "empty migrated")) {
    return false;
  }

  if (!deleteBookmarkStorePath(srcCurrentPath, "source")) {
    return false;
  }
  if (srcLegacyPath != srcCurrentPath && !deleteBookmarkStorePath(srcLegacyPath, "source legacy")) {
    return false;
  }
  if (dstLegacyPath != dstCurrentPath && !deleteBookmarkStorePath(dstLegacyPath, "destination legacy")) {
    return false;
  }

  LOG_INF("BKS", "Migrated bookmark path: %s -> %s (%u bookmark(s)%s%s)", oldFilePath.c_str(), newFilePath.c_str(),
          static_cast<unsigned>(writer.bookmarks.size()), hasDestBookmarks ? ", merged with destination" : "",
          sourceNeedsRewrite ? ", normalized format" : "");
  return true;
}

bool BookmarkStore::migrateForFilePath(const std::string& oldFilePath, const std::string& newFilePath,
                                       const std::string& title, const std::string& author,
                                       const std::string& bookType) {
  if (bookType != "epub" && bookType != "pdf" && bookType != "xtc" && bookType != "txt") {
    LOG_ERR("BKS", "Unknown book type for bookmark migration: %s", bookType.c_str());
    return false;
  }
  if (oldFilePath.empty() || newFilePath.empty() || oldFilePath == newFilePath) {
    return true;
  }
  if (bookType == "pdf") {
    return migratePdfForFilePath(oldFilePath, newFilePath, title, author);
  }
  return migrateLegacyForFilePath(oldFilePath, newFilePath, title, author, bookType);
}

bool BookmarkStore::hasAnyBookmarks() {
  if (!Storage.exists(BOOKMARKS_DIR)) return false;
  const auto files = Storage.listFiles(BOOKMARKS_DIR);
  return std::any_of(files.cbegin(), files.cend(),
                     [](const auto& name) { return !isPdfTransactionArtifact(name.c_str()); });
}

bool BookmarkStore::getAllBookmarkedBooks(std::vector<BookmarkedBookEntry>& out) {
  if (!Storage.exists(BOOKMARKS_DIR)) return true;

  const auto files = Storage.listFiles(BOOKMARKS_DIR);
  for (const auto& name : files) {
    if (isPdfTransactionArtifact(name.c_str())) continue;
    const std::string fullPath = std::string(BOOKMARKS_DIR) + "/" + name.c_str();

    FsFile f;
    if (!Storage.openFileForRead("BKS", fullPath, f)) continue;

    if (f.available() < static_cast<int>(sizeof(uint8_t))) {
      f.close();
      continue;
    }
    uint8_t version = 0;
    serialization::readPod(f, version);
    if (version != LEGACY_VERSION && version != COUNT_U16_VERSION && version != PARAGRAPH_ANCHOR_VERSION &&
        version != VERSION) {
      LOG_DBG("BKS", "Skipping bookmark file with unknown version: %s", name.c_str());
      f.close();
      continue;
    }

    if (f.available() < static_cast<int>(version == LEGACY_VERSION ? sizeof(uint8_t) : sizeof(uint16_t))) {
      f.close();
      continue;
    }
    uint16_t count = 0;
    if (!readBookmarkCount(f, version, count)) {
      f.close();
      continue;
    }

    // Reads a length-prefixed string, returning false if the file is truncated.
    auto readCheckedString = [&f](std::string& s) -> bool {
      uint32_t len;
      if (f.available() < static_cast<int>(sizeof(len))) return false;
      serialization::readPod(f, len);
      if (f.available() < static_cast<int>(len)) return false;
      s.resize(len);
      f.read(reinterpret_cast<uint8_t*>(&s[0]), len);
      return true;
    };

    std::string title, author, path;
    if (!readCheckedString(title) || !readCheckedString(author) || !readCheckedString(path)) {
      f.close();
      continue;
    }
    f.close();

    std::string bookType = "epub";
    const std::string nameStr = name.c_str();
    size_t underscorePos = nameStr.find('_');
    if (underscorePos != std::string::npos) {
      bookType = nameStr.substr(0, underscorePos);
    }

    if (path.empty() || count == 0) continue;
    if (!Storage.exists(path.c_str())) {
      if (bookType != "epub") continue;

      std::string movedPath;
      if (!resolveMovedToReadDestinationPath(path, movedPath)) {
        continue;
      }
      if (!BookmarkStore::migrateForFilePath(path, movedPath, title, author, bookType)) {
        continue;
      }
      path = std::move(movedPath);
    }

    auto existing = std::find_if(out.begin(), out.end(), [&](const BookmarkedBookEntry& entry) {
      return entry.bookPath == path && entry.bookType == bookType;
    });
    if (existing != out.end()) {
      existing->count = std::max(existing->count, count);
      if (existing->bookTitle.empty()) existing->bookTitle = title;
      if (existing->bookAuthor.empty()) existing->bookAuthor = author;
      continue;
    }

    out.push_back({std::move(title), std::move(author), std::move(path), std::move(bookType), count});
  }

  return true;
}
