#include "ClippingStore.h"

#include <Arduino.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>
#include <Serialization.h>
#include <uzlib.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <functional>
#include <limits>

#include "util/BookMoveDurableFile.h"

namespace {
constexpr uint8_t LEGACY_VERSION = 1;
constexpr uint8_t VERSION = 2;
constexpr size_t INITIAL_CLIPPING_RESERVE = 4;
constexpr char CLIPPINGS_DIR[] = "/.crosspoint/clippings";
constexpr char PDF_TRANSACTION_TEMP_SUFFIX[] = ".tmp";
constexpr char PDF_TRANSACTION_BACKUP_SUFFIX[] = ".bak";
constexpr char PDF_STORE_PREFIX[] = "/.crosspoint/clippings/pdf_";

struct ClippingFileHeader {
  std::string title;
  std::string author;
  std::string path;
  std::string bookType;
  uint16_t count = 0;
};

struct PdfClippingMigrationScratch {
  ClippingStore store;
  std::vector<Clipping> source;
  std::vector<Clipping> destination;
  std::string sourcePath;
  std::string destinationPath;
  std::string temporaryPath;
  std::string backupPath;
};
static_assert(sizeof(PdfClippingMigrationScratch) <= 512);

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

const Clipping* pdfClippingAt(const std::vector<Clipping>& clippings, const Clipping* const appended,
                              const size_t removeIndex, const size_t logicalIndex) {
  size_t candidate = 0;
  for (size_t index = 0; index < clippings.size(); ++index) {
    if (index == removeIndex) continue;
    if (candidate++ == logicalIndex) return &clippings[index];
  }
  return appended != nullptr && candidate == logicalIndex ? appended : nullptr;
}

size_t pdfClippingCount(const std::vector<Clipping>& clippings, const Clipping* const appended,
                        const size_t removeIndex, const bool clear) {
  if (clear) return 0;
  return clippings.size() - static_cast<size_t>(removeIndex < clippings.size()) +
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

bool deleteStorePath(const std::string& path, const char* const description) {
  if (!Storage.exists(path.c_str())) return true;
  if (!Storage.remove(path.c_str())) {
    LOG_ERR("CLIP", "Failed to delete %s: %s", description, path.c_str());
    return false;
  }
  return true;
}

bool deletePdfClippingStorePaths(const std::string& canonicalPath) {
  const std::string backupPath = canonicalPath + PDF_TRANSACTION_BACKUP_SUFFIX;
  const std::string temporaryPath = canonicalPath + PDF_TRANSACTION_TEMP_SUFFIX;
  return deleteStorePath(backupPath, "PDF clipping backup") &&
         deleteStorePath(temporaryPath, "PDF clipping temporary") &&
         deleteStorePath(canonicalPath, "PDF clipping canonical");
}

bool mergePdfClippings(std::vector<Clipping>& destination, const std::vector<Clipping>& source) {
  for (const Clipping& clipping : source) {
    const bool duplicate = std::any_of(destination.begin(), destination.end(), [&](const Clipping& existing) {
      return clipping.paragraphIndex != 0 && clipping.paragraphIndex != UINT16_MAX &&
             existing.paragraphIndex == clipping.paragraphIndex;
    });
    if (duplicate) continue;
    if (destination.size() >= CLIPPING_MAX_PER_BOOK) {
      LOG_ERR("CLIP", "PDF clipping limit reached while merging migration");
      return false;
    }
    destination.push_back(clipping);
  }
  return true;
}

std::string storeFilePathForBook(const std::string& filePath, const std::string& bookType) {
  const uint32_t crc = uzlib_crc32(filePath.data(), static_cast<unsigned int>(filePath.size()), 0);
  return std::string(CLIPPINGS_DIR) + "/" + bookType + "_" + std::to_string(crc) + ".bin";
}

bool pdfClippingStoreFilePathForBook(const std::string& filePath, std::string& output) {
  constexpr size_t PATH_CAPACITY = 64;
  char path[PATH_CAPACITY];
  const uint32_t crc = uzlib_crc32(filePath.data(), static_cast<unsigned int>(filePath.size()), 0);
  const int written = snprintf(path, sizeof(path), "%s/pdf_%lu.bin", CLIPPINGS_DIR, static_cast<unsigned long>(crc));
  if (written < 0 || static_cast<size_t>(written) >= sizeof(path)) {
    LOG_ERR("CLIP", "PDF clipping store path exceeds %u bytes", static_cast<unsigned>(sizeof(path)));
    return false;
  }
  output.assign(path, static_cast<size_t>(written));
  return true;
}

void copyBounded(char* dst, const size_t dstSize, const char* src) {
  if (dstSize == 0) return;
  if (!src) src = "";
  snprintf(dst, dstSize, "%s", src);
}

bool readClippingFileHeader(const std::string& fullPath, const char* name, ClippingFileHeader& header) {
  FsFile f;
  if (!Storage.openFileForRead("CLIP", fullPath, f)) {
    return false;
  }

  uint8_t version = 0;
  uint16_t count = 0;
  if (!serialization::tryReadPod(f, version) || (version != LEGACY_VERSION && version != VERSION) ||
      !serialization::tryReadPod(f, count) || !serialization::tryReadString(f, header.title) ||
      !serialization::tryReadString(f, header.author) || !serialization::tryReadString(f, header.path)) {
    f.close();
    return false;
  }
  f.close();

  if (count > CLIPPING_MAX_PER_BOOK) {
    return false;
  }
  header.count = count;
  header.bookType = "epub";
  const std::string nameStr = name ? name : "";
  const size_t underscorePos = nameStr.find('_');
  if (underscorePos != std::string::npos) {
    header.bookType = nameStr.substr(0, underscorePos);
  }
  return true;
}

bool copyBytes(FsFile& in, FsFile& out, uint16_t length) {
  std::array<uint8_t, TEXT_COPY_BUFFER_SIZE> buffer{};
  while (length > 0) {
    const size_t chunk = std::min<size_t>(length, buffer.size());
    if (in.read(buffer.data(), chunk) != static_cast<int>(chunk)) {
      return false;
    }
    if (out.write(buffer.data(), chunk) != chunk) {
      return false;
    }
    length = static_cast<uint16_t>(length - chunk);
  }
  return true;
}
}  // namespace

ClippingStore ClippingStore::instance;

bool ClippingStore::loadForBook(const std::string& filePath, const std::string& title, const std::string& author,
                                const std::string& bookType) {
  if (bookType != "epub" && bookType != "pdf") {
    LOG_ERR("CLIP", "Unknown clipping book type: %s", bookType.c_str());
    return false;
  }

  bookFilePath = filePath;
  bookTitle = title;
  bookAuthor = author;
  dirty = false;
  clippings.clear();
  if (clippings.capacity() < INITIAL_CLIPPING_RESERVE) {
    clippings.reserve(INITIAL_CLIPPING_RESERVE);
  }

  if (bookType == "pdf") {
    if (!pdfClippingStoreFilePathForBook(filePath, storeFilePath)) return false;
  } else {
    storeFilePath = storeFilePathForBook(filePath, bookType);
  }
  if (bookType == "pdf" && !recoverPdfTransaction()) {
    LOG_ERR("CLIP", "Failed to recover PDF clipping transaction: %s", storeFilePath.c_str());
    return false;
  }
  if (!Storage.exists(storeFilePath.c_str())) {
    return true;
  }

  return readFromFile();
}

bool deleteStorePathNoPathAlloc(const char* const path, const char* const description) {
  if (!Storage.exists(path)) return true;
  if (!Storage.remove(path)) {
    LOG_ERR("CLIP", "Failed to delete %s: %s", description, path);
    return false;
  }
  return true;
}

bool clippingsMatchExactly(const Clipping& left, const Clipping& right) {
  return left.spineIndex == right.spineIndex && left.startPage == right.startPage && left.endPage == right.endPage &&
         left.pageCount == right.pageCount && left.startWordIndex == right.startWordIndex &&
         left.endWordIndex == right.endWordIndex && left.wordCount == right.wordCount &&
         left.paragraphIndex == right.paragraphIndex && left.timestamp == right.timestamp &&
         std::memcmp(left.chapterTitle, right.chapterTitle, sizeof(left.chapterTitle)) == 0 && left.text == right.text;
}

bool mergeAuthoritativeClippings(std::vector<Clipping>& destination, const std::vector<Clipping>& source,
                                 const bool stablePdfIds) {
  for (const Clipping& clipping : source) {
    auto found = destination.end();
    if (stablePdfIds && clipping.paragraphIndex != 0 && clipping.paragraphIndex != UINT16_MAX) {
      found = std::find_if(destination.begin(), destination.end(), [&](const Clipping& existing) {
        return existing.paragraphIndex == clipping.paragraphIndex;
      });
    } else {
      found = std::find_if(destination.begin(), destination.end(),
                           [&](const Clipping& existing) { return clippingsMatchExactly(existing, clipping); });
    }
    if (found != destination.end()) {
      *found = clipping;
      continue;
    }
    if (destination.size() >= CLIPPING_MAX_PER_BOOK) {
      LOG_ERR("CLIP", "Clipping limit reached while copying moved-book state");
      return false;
    }
    destination.push_back(clipping);
  }
  return true;
}

bool containsAllClippings(const std::vector<Clipping>& destination, const std::vector<Clipping>& source) {
  return std::all_of(source.begin(), source.end(), [&](const Clipping& expected) {
    return std::any_of(destination.begin(), destination.end(),
                       [&](const Clipping& actual) { return clippingsMatchExactly(actual, expected); });
  });
}

bool ClippingStore::reloadPdfFromDisk() {
  if (bookFilePath.empty() || !isPdfStorePath(storeFilePath)) {
    LOG_ERR("CLIP", "Cannot reload a PDF clipping store before it is loaded");
    return false;
  }
  return loadForBook(bookFilePath, bookTitle, bookAuthor, "pdf");
}

void ClippingStore::unload() {
  if (dirty) saveToFile();
  clippings.clear();
  bookFilePath.clear();
  bookTitle.clear();
  bookAuthor.clear();
  storeFilePath.clear();
  dirty = false;
}

ClippingStore::AddResult ClippingStore::addClipping(const uint16_t spineIndex, const uint16_t startPage,
                                                    const uint16_t endPage, const uint16_t pageCount,
                                                    const uint16_t startWordIndex, const uint16_t endWordIndex,
                                                    const uint16_t wordCount, const char* chapterTitle,
                                                    const uint16_t paragraphIndex, const std::string& text) {
  if (clippings.size() >= CLIPPING_MAX_PER_BOOK) {
    LOG_ERR("CLIP", "Clipping limit (%u) reached", CLIPPING_MAX_PER_BOOK);
    return AddResult::LimitReached;
  }

  Clipping clipping;
  clipping.spineIndex = spineIndex;
  clipping.startPage = startPage;
  clipping.endPage = endPage;
  clipping.pageCount = std::max<uint16_t>(1, pageCount);
  clipping.startWordIndex = startWordIndex;
  clipping.endWordIndex = endWordIndex;
  clipping.wordCount = wordCount;
  clipping.paragraphIndex = paragraphIndex;
  clipping.timestamp = static_cast<uint32_t>(millis() / 1000UL);
  copyBounded(clipping.chapterTitle, sizeof(clipping.chapterTitle), chapterTitle);
  clipping.textLength = static_cast<uint16_t>(std::min(text.size(), CLIPPING_TEXT_MAX));

  clippings.push_back(std::move(clipping));
  dirty = true;
  if (!writeToFile(&text, clippings.size() - 1)) {
    clippings.pop_back();
    dirty = true;
    return AddResult::SaveFailed;
  }
  dirty = false;
  return AddResult::Added;
}

ClippingStore::AddResult ClippingStore::addPdfClipping(const uint16_t spineIndex, const uint16_t startPage,
                                                       const uint16_t endPage, const uint16_t pageCount,
                                                       const uint16_t startWordIndex, const uint16_t endWordIndex,
                                                       const uint16_t wordCount, const char* const chapterTitle,
                                                       const uint16_t itemId, const std::string& text) {
  if (!isPdfStorePath(storeFilePath)) {
    return AddResult::SaveFailed;
  }
  if (itemId == 0 || itemId == UINT16_MAX ||
      std::any_of(clippings.begin(), clippings.end(),
                  [itemId](const Clipping& clipping) { return clipping.paragraphIndex == itemId; })) {
    return AddResult::SaveFailed;
  }
  if (clippings.size() >= CLIPPING_MAX_PER_BOOK) {
    LOG_ERR("CLIP", "PDF clipping limit (%u) reached", CLIPPING_MAX_PER_BOOK);
    return AddResult::LimitReached;
  }
  if (storeFilePath.empty()) {
    return AddResult::SaveFailed;
  }

  Clipping clipping;
  clipping.spineIndex = spineIndex;
  clipping.startPage = startPage;
  clipping.endPage = endPage;
  clipping.pageCount = std::max<uint16_t>(1, pageCount);
  clipping.startWordIndex = startWordIndex;
  clipping.endWordIndex = endWordIndex;
  clipping.wordCount = wordCount;
  clipping.paragraphIndex = itemId;
  clipping.timestamp = static_cast<uint32_t>(millis() / 1000UL);
  copyBounded(clipping.chapterTitle, sizeof(clipping.chapterTitle), chapterTitle);
  clipping.text.assign(text.data(), std::min(text.size(), CLIPPING_TEXT_MAX));

  if (clippings.size() == clippings.capacity()) {
    clippings.reserve(clippings.size() + 1);
  }
  if (!writePdfTransaction(&clipping, std::numeric_limits<size_t>::max(), false)) {
    return AddResult::SaveFailed;
  }
  clippings.push_back(std::move(clipping));
  dirty = false;
  return AddResult::Added;
}

bool ClippingStore::removePdfClipping(const uint16_t itemId) {
  if (!isPdfStorePath(storeFilePath)) return false;
  const auto found = std::find_if(clippings.begin(), clippings.end(),
                                  [itemId](const Clipping& clipping) { return clipping.paragraphIndex == itemId; });
  if (found == clippings.end()) return false;

  const size_t removeIndex = static_cast<size_t>(found - clippings.begin());
  if (!writePdfTransaction(nullptr, removeIndex, false)) {
    return false;
  }
  clippings.erase(found);
  dirty = false;
  return true;
}

bool ClippingStore::clearPdfClippings() {
  if (!isPdfStorePath(storeFilePath)) return false;
  if (!deletePdfClippingStorePaths(storeFilePath)) {
    return false;
  }
  clippings.clear();
  dirty = false;
  return true;
}

bool ClippingStore::removeClippingAt(const size_t index) {
  if (index >= clippings.size()) return false;
  if (isPdfStorePath(storeFilePath)) {
    return removePdfClipping(clippings[index].paragraphIndex);
  }
  Clipping clipping = std::move(clippings[index]);
  clippings.erase(clippings.begin() + index);
  dirty = true;
  if (!saveToFile()) {
    clippings.insert(clippings.begin() + index, std::move(clipping));
    dirty = true;
    return false;
  }
  return true;
}

bool ClippingStore::hasClippingForPage(const uint16_t spineIndex, const uint16_t page) const {
  return std::any_of(clippings.begin(), clippings.end(), [&](const Clipping& clipping) {
    return clipping.spineIndex == spineIndex && page >= clipping.startPage && page <= clipping.endPage;
  });
}

const Clipping* ClippingStore::clippingAt(const size_t index) const {
  if (index >= clippings.size()) return nullptr;
  return &clippings[index];
}

bool ClippingStore::readClippingText(const size_t index, std::string& out) const {
  const Clipping* clipping = clippingAt(index);
  if (!clipping) return false;
  return readClippingText(*clipping, out);
}

bool ClippingStore::readClippingText(const Clipping& clipping, std::string& out) const {
  out.clear();
  if (clipping.textLength == 0) return true;
  if (storeFilePath.empty()) return false;

  FsFile f;
  if (!Storage.openFileForRead("CLIP", storeFilePath, f)) {
    return false;
  }
  if (!f.seek(clipping.textOffset)) {
    f.close();
    LOG_ERR("CLIP", "Failed to seek clipping text at %u: %s", clipping.textOffset, storeFilePath.c_str());
    return false;
  }
  out.resize(clipping.textLength);
  const int expected = static_cast<int>(clipping.textLength);
  const bool ok = f.read(&out[0], clipping.textLength) == expected;
  f.close();
  if (!ok) {
    out.clear();
    LOG_ERR("CLIP", "Failed to read clipping text at %u: %s", clipping.textOffset, storeFilePath.c_str());
  }
  return ok;
}

bool ClippingStore::saveToFile() {
  if (!dirty) return true;
  if (writeToFile()) {
    dirty = false;
    return true;
  }
  return false;
}

void ClippingStore::clearAll() {
  if (isPdfStorePath(storeFilePath)) {
    clearPdfClippings();
    return;
  }
  clippings.clear();
  dirty = false;
  if (!storeFilePath.empty() && Storage.exists(storeFilePath.c_str())) {
    Storage.remove(storeFilePath.c_str());
  }
}

bool ClippingStore::readFromFile() { return readFromFile(storeFilePath, clippings); }

bool ClippingStore::readFromFile(const std::string& path, std::vector<Clipping>& out) const {
  out.clear();
  FsFile f;
  if (!Storage.openFileForRead("CLIP", path, f)) {
    return false;
  }

  uint8_t version = 0;
  uint16_t count = 0;
  std::string title;
  std::string author;
  std::string storedPath;
  if (!serialization::tryReadPod(f, version) || (version != LEGACY_VERSION && version != VERSION) ||
      !serialization::tryReadPod(f, count) || !serialization::tryReadString(f, title) ||
      !serialization::tryReadString(f, author) || !serialization::tryReadString(f, storedPath)) {
    f.close();
    LOG_ERR("CLIP", "Failed to read clipping header: %s", path.c_str());
    return false;
  }
  if (isPdfStorePath(path) && storedPath != bookFilePath) {
    f.close();
    LOG_ERR("CLIP", "PDF clipping file path mismatch: %s", path.c_str());
    return false;
  }

  if (count > CLIPPING_MAX_PER_BOOK) {
    LOG_ERR("CLIP", "Clipping count %u exceeds max, file may be corrupt: %s", count, path.c_str());
    f.close();
    return false;
  }

  out.reserve(count);
  for (uint16_t i = 0; i < count; ++i) {
    Clipping clipping;
    if (!serialization::tryReadPod(f, clipping.spineIndex) || !serialization::tryReadPod(f, clipping.startPage) ||
        !serialization::tryReadPod(f, clipping.endPage) || !serialization::tryReadPod(f, clipping.pageCount) ||
        !serialization::tryReadPod(f, clipping.startWordIndex) ||
        !serialization::tryReadPod(f, clipping.endWordIndex) || !serialization::tryReadPod(f, clipping.wordCount) ||
        !serialization::tryReadPod(f, clipping.paragraphIndex) || !serialization::tryReadPod(f, clipping.timestamp)) {
      f.close();
      LOG_ERR("CLIP", "Clipping file truncated at record %u: %s", i, path.c_str());
      return false;
    }
    if (f.read(reinterpret_cast<uint8_t*>(clipping.chapterTitle), sizeof(clipping.chapterTitle)) !=
        sizeof(clipping.chapterTitle)) {
      f.close();
      LOG_ERR("CLIP", "Clipping file truncated at chapter title, record %u: %s", i, path.c_str());
      return false;
    }
    clipping.chapterTitle[sizeof(clipping.chapterTitle) - 1] = '\0';
    if (version == LEGACY_VERSION) {
      uint32_t textLen = 0;
      if (!serialization::tryReadPod(f, textLen)) {
        f.close();
        LOG_ERR("CLIP", "Clipping file truncated at text length, record %u: %s", i, path.c_str());
        return false;
      }
      clipping.textOffset = static_cast<uint32_t>(f.position());
      clipping.textLength = static_cast<uint16_t>(std::min<uint32_t>(textLen, CLIPPING_TEXT_MAX));
      if (textLen > 0 && !f.seekCur(textLen)) {
        f.close();
        LOG_ERR("CLIP", "Clipping file truncated at text, record %u: %s", i, path.c_str());
        return false;
      }
    } else {
      if (!serialization::tryReadPod(f, clipping.textLength)) {
        f.close();
        LOG_ERR("CLIP", "Clipping file truncated at text length, record %u: %s", i, path.c_str());
        return false;
      }
      if (clipping.textLength > CLIPPING_TEXT_MAX) {
        f.close();
        LOG_ERR("CLIP", "Clipping text length %u exceeds max, record %u: %s", clipping.textLength, i, path.c_str());
        return false;
      }
      clipping.textOffset = static_cast<uint32_t>(f.position());
      if (clipping.textLength > 0 && !f.seekCur(clipping.textLength)) {
        f.close();
        LOG_ERR("CLIP", "Clipping file truncated at text, record %u: %s", i, path.c_str());
        return false;
      }
    }
    out.push_back(std::move(clipping));
  }

  const bool closed = f.close();
  if (!closed && isPdfStorePath(path)) {
    LOG_ERR("CLIP", "Failed to close PDF clipping file after read: %s", path.c_str());
    return false;
  }
  return true;
}

bool ClippingStore::writeToFile(const std::string* replacementText, const size_t replacementIndex) {
  Storage.mkdir("/.crosspoint");
  Storage.mkdir(CLIPPINGS_DIR);

  const std::string tmpPath = storeFilePath + ".tmp";
  const std::string backupPath = storeFilePath + ".bak";
  if (!Storage.exists(storeFilePath.c_str()) && Storage.exists(backupPath.c_str())) {
    if (!Storage.rename(backupPath.c_str(), storeFilePath.c_str())) {
      LOG_ERR("CLIP", "Failed to recover clipping backup: %s", backupPath.c_str());
      return false;
    }
    LOG_INF("CLIP", "Recovered clipping backup: %s", storeFilePath.c_str());
  }
  if (Storage.exists(tmpPath.c_str())) Storage.remove(tmpPath.c_str());
  if (Storage.exists(backupPath.c_str()) && Storage.exists(storeFilePath.c_str())) Storage.remove(backupPath.c_str());

  FsFile source;
  const bool hasSource = Storage.exists(storeFilePath.c_str());
  if (hasSource && !Storage.openFileForRead("CLIP", storeFilePath, source)) {
    LOG_ERR("CLIP", "Failed to open clipping source for rewrite: %s", storeFilePath.c_str());
    return false;
  }

  FsFile f = Storage.open(tmpPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC);
  if (!f) {
    if (source) source.close();
    LOG_ERR("CLIP", "Failed to open clipping temp file for write: %s", tmpPath.c_str());
    return false;
  }

  const uint16_t count = static_cast<uint16_t>(std::min<size_t>(clippings.size(), CLIPPING_MAX_PER_BOOK));
  std::vector<uint32_t> newTextOffsets;
  newTextOffsets.reserve(count);
  std::vector<uint16_t> newTextLengths;
  newTextLengths.reserve(count);
  if (!serialization::tryWritePod(f, VERSION) || !serialization::tryWritePod(f, count) ||
      !serialization::tryWriteString(f, bookTitle) || !serialization::tryWriteString(f, bookAuthor) ||
      !serialization::tryWriteString(f, bookFilePath)) {
    LOG_ERR("CLIP", "Failed to write clipping header: %s", tmpPath.c_str());
    f.close();
    if (source) source.close();
    Storage.remove(tmpPath.c_str());
    return false;
  }

  for (uint16_t i = 0; i < count; ++i) {
    const Clipping& clipping = clippings[i];
    if (!serialization::tryWritePod(f, clipping.spineIndex) || !serialization::tryWritePod(f, clipping.startPage) ||
        !serialization::tryWritePod(f, clipping.endPage) || !serialization::tryWritePod(f, clipping.pageCount) ||
        !serialization::tryWritePod(f, clipping.startWordIndex) ||
        !serialization::tryWritePod(f, clipping.endWordIndex) || !serialization::tryWritePod(f, clipping.wordCount) ||
        !serialization::tryWritePod(f, clipping.paragraphIndex) || !serialization::tryWritePod(f, clipping.timestamp) ||
        f.write(reinterpret_cast<const uint8_t*>(clipping.chapterTitle), sizeof(clipping.chapterTitle)) !=
            sizeof(clipping.chapterTitle)) {
      LOG_ERR("CLIP", "Failed to write clipping record %u: %s", i, storeFilePath.c_str());
      f.close();
      if (source) source.close();
      Storage.remove(tmpPath.c_str());
      return false;
    }

    const bool useReplacement = replacementText && i == replacementIndex;
    const uint16_t textLen = useReplacement
                                 ? static_cast<uint16_t>(std::min(replacementText->size(), CLIPPING_TEXT_MAX))
                                 : clipping.textLength;
    if (!serialization::tryWritePod(f, textLen)) {
      LOG_ERR("CLIP", "Failed to write clipping text length %u: %s", i, tmpPath.c_str());
      f.close();
      if (source) source.close();
      Storage.remove(tmpPath.c_str());
      return false;
    }

    const uint32_t newTextOffset = static_cast<uint32_t>(f.position());
    bool wroteText = true;
    if (textLen > 0 && useReplacement) {
      wroteText = f.write(reinterpret_cast<const uint8_t*>(replacementText->data()), textLen) == textLen;
    } else if (textLen > 0) {
      wroteText = source && source.seek(clipping.textOffset) && copyBytes(source, f, textLen);
    }
    if (!wroteText) {
      LOG_ERR("CLIP", "Failed to write clipping text %u: %s", i, tmpPath.c_str());
      f.close();
      if (source) source.close();
      Storage.remove(tmpPath.c_str());
      return false;
    }
    newTextOffsets.push_back(newTextOffset);
    newTextLengths.push_back(textLen);
  }

  if (!f.sync()) {
    LOG_ERR("CLIP", "Failed to sync clipping file: %s", tmpPath.c_str());
    f.close();
    if (source) source.close();
    Storage.remove(tmpPath.c_str());
    return false;
  }
  f.close();
  if (source) source.close();

  if (hasSource && !Storage.rename(storeFilePath.c_str(), backupPath.c_str())) {
    LOG_ERR("CLIP", "Failed to back up clipping file: %s", storeFilePath.c_str());
    Storage.remove(tmpPath.c_str());
    return false;
  }
  if (!Storage.rename(tmpPath.c_str(), storeFilePath.c_str())) {
    LOG_ERR("CLIP", "Failed to replace clipping file: %s", storeFilePath.c_str());
    Storage.remove(tmpPath.c_str());
    if (hasSource) Storage.rename(backupPath.c_str(), storeFilePath.c_str());
    return false;
  }
  if (hasSource && Storage.exists(backupPath.c_str())) {
    Storage.remove(backupPath.c_str());
  }
  for (uint16_t i = 0; i < count; ++i) {
    clippings[i].textOffset = newTextOffsets[i];
    clippings[i].textLength = newTextLengths[i];
  }
  return true;
}

bool ClippingStore::writeMigrationPayload(void* const fileContext) const {
  if (fileContext == nullptr) return false;
  auto& file = *static_cast<FsFile*>(fileContext);
  const uint16_t count = static_cast<uint16_t>(std::min<size_t>(clippings.size(), CLIPPING_MAX_PER_BOOK));
  bool wrote = serialization::tryWritePod(file, VERSION) && serialization::tryWritePod(file, count) &&
               serialization::tryWriteString(file, bookTitle) && serialization::tryWriteString(file, bookAuthor) &&
               serialization::tryWriteString(file, bookFilePath);
  for (uint16_t index = 0; wrote && index < count; ++index) {
    const Clipping& clipping = clippings[index];
    wrote =
        serialization::tryWritePod(file, clipping.spineIndex) && serialization::tryWritePod(file, clipping.startPage) &&
        serialization::tryWritePod(file, clipping.endPage) && serialization::tryWritePod(file, clipping.pageCount) &&
        serialization::tryWritePod(file, clipping.startWordIndex) &&
        serialization::tryWritePod(file, clipping.endWordIndex) &&
        serialization::tryWritePod(file, clipping.wordCount) &&
        serialization::tryWritePod(file, clipping.paragraphIndex) &&
        serialization::tryWritePod(file, clipping.timestamp) &&
        file.write(reinterpret_cast<const uint8_t*>(clipping.chapterTitle), sizeof(clipping.chapterTitle)) ==
            sizeof(clipping.chapterTitle) &&
        serialization::tryWriteString(file, clipping.text);
  }
  return wrote;
}

bool ClippingStore::writePdfTransaction(const Clipping* const appended, const size_t removeIndex,
                                        const bool clear) const {
  if (storeFilePath.empty()) {
    LOG_ERR("CLIP", "Cannot persist PDF clippings without a store path");
    return false;
  }
  if (!isPdfStorePath(storeFilePath)) {
    LOG_ERR("CLIP", "Refusing PDF clipping mutation for a non-PDF store");
    return false;
  }

  const std::string temporaryPath = storeFilePath + PDF_TRANSACTION_TEMP_SUFFIX;
  const std::string backupPath = storeFilePath + PDF_TRANSACTION_BACKUP_SUFFIX;
  if (!Storage.exists(storeFilePath.c_str()) && Storage.exists(backupPath.c_str()) &&
      !Storage.rename(backupPath.c_str(), storeFilePath.c_str())) {
    LOG_ERR("CLIP", "Failed to restore PDF clipping backup before mutation: %s", backupPath.c_str());
    return false;
  }
  if (Storage.exists(temporaryPath.c_str()) && !Storage.remove(temporaryPath.c_str())) {
    LOG_ERR("CLIP", "Failed to remove stale PDF clipping transaction: %s", temporaryPath.c_str());
    return false;
  }
  if (Storage.exists(backupPath.c_str()) && !Storage.remove(backupPath.c_str())) {
    LOG_ERR("CLIP", "Failed to remove stale PDF clipping backup: %s", backupPath.c_str());
    return false;
  }

  Storage.mkdir("/.crosspoint");
  Storage.mkdir(CLIPPINGS_DIR);
  FsFile file = Storage.open(temporaryPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC);
  if (!file) {
    LOG_ERR("CLIP", "Failed to open PDF clipping transaction: %s", temporaryPath.c_str());
    return false;
  }

  const size_t logicalCount = pdfClippingCount(clippings, appended, removeIndex, clear);
  if (logicalCount > CLIPPING_MAX_PER_BOOK) {
    LOG_ERR("CLIP", "PDF clipping transaction count exceeds limit: %u", static_cast<unsigned>(logicalCount));
    if (!file.close()) {
      LOG_ERR("CLIP", "Failed to close rejected PDF clipping transaction: %s", temporaryPath.c_str());
    }
    Storage.remove(temporaryPath.c_str());
    return false;
  }
  const uint16_t count = static_cast<uint16_t>(logicalCount);
  bool wrote = serialization::tryWritePod(file, VERSION) && serialization::tryWritePod(file, count) &&
               serialization::tryWriteString(file, bookTitle) && serialization::tryWriteString(file, bookAuthor) &&
               serialization::tryWriteString(file, bookFilePath);
  for (size_t index = 0; wrote && index < logicalCount; ++index) {
    const Clipping* const clipping = pdfClippingAt(clippings, appended, removeIndex, index);
    wrote = clipping != nullptr && serialization::tryWritePod(file, clipping->spineIndex) &&
            serialization::tryWritePod(file, clipping->startPage) &&
            serialization::tryWritePod(file, clipping->endPage) &&
            serialization::tryWritePod(file, clipping->pageCount) &&
            serialization::tryWritePod(file, clipping->startWordIndex) &&
            serialization::tryWritePod(file, clipping->endWordIndex) &&
            serialization::tryWritePod(file, clipping->wordCount) &&
            serialization::tryWritePod(file, clipping->paragraphIndex) &&
            serialization::tryWritePod(file, clipping->timestamp) &&
            file.write(reinterpret_cast<const uint8_t*>(clipping->chapterTitle), sizeof(clipping->chapterTitle)) ==
                sizeof(clipping->chapterTitle) &&
            serialization::tryWriteString(file, clipping->text);
  }

  file.flush();
  const bool synced = file.sync();
  const bool closed = file.close();
  if (!wrote || !synced || !closed) {
    LOG_ERR("CLIP", "Failed to durably write PDF clipping transaction: %s", temporaryPath.c_str());
    Storage.remove(temporaryPath.c_str());
    return false;
  }
  if (!verifyPdfTransaction(temporaryPath, appended, removeIndex, clear)) {
    LOG_ERR("CLIP", "PDF clipping transaction readback failed: %s", temporaryPath.c_str());
    Storage.remove(temporaryPath.c_str());
    return false;
  }

  const bool hadCanonical = Storage.exists(storeFilePath.c_str());
  if (hadCanonical && !Storage.rename(storeFilePath.c_str(), backupPath.c_str())) {
    LOG_ERR("CLIP", "Failed to back up PDF clipping store: %s", storeFilePath.c_str());
    Storage.remove(temporaryPath.c_str());
    return false;
  }
  if (!Storage.rename(temporaryPath.c_str(), storeFilePath.c_str())) {
    LOG_ERR("CLIP", "Failed to promote PDF clipping transaction: %s", temporaryPath.c_str());
    if (hadCanonical && !Storage.rename(backupPath.c_str(), storeFilePath.c_str())) {
      LOG_ERR("CLIP", "Failed to roll back PDF clipping transaction: %s", backupPath.c_str());
    }
    return false;
  }

  if (hadCanonical && Storage.exists(backupPath.c_str()) && !Storage.remove(backupPath.c_str())) {
    LOG_ERR("CLIP", "Failed to remove committed PDF clipping backup: %s", backupPath.c_str());
  }
  return true;
}

bool ClippingStore::verifyPdfTransaction(const std::string& path, const Clipping* const appended,
                                         const size_t removeIndex, const bool clear) const {
  FsFile file;
  if (!Storage.openFileForRead("CLIP", path, file)) {
    return false;
  }

  const size_t logicalCount = pdfClippingCount(clippings, appended, removeIndex, clear);
  const uint16_t count = static_cast<uint16_t>(logicalCount);
  bool matches = readExpectedPod(file, VERSION) && readExpectedPod(file, count) &&
                 readExpectedString(file, bookTitle) && readExpectedString(file, bookAuthor) &&
                 readExpectedString(file, bookFilePath);
  for (size_t index = 0; matches && index < logicalCount; ++index) {
    const Clipping* const clipping = pdfClippingAt(clippings, appended, removeIndex, index);
    matches = clipping != nullptr && readExpectedPod(file, clipping->spineIndex) &&
              readExpectedPod(file, clipping->startPage) && readExpectedPod(file, clipping->endPage) &&
              readExpectedPod(file, clipping->pageCount) && readExpectedPod(file, clipping->startWordIndex) &&
              readExpectedPod(file, clipping->endWordIndex) && readExpectedPod(file, clipping->wordCount) &&
              readExpectedPod(file, clipping->paragraphIndex) && readExpectedPod(file, clipping->timestamp) &&
              readExpectedBytes(file, clipping->chapterTitle, sizeof(clipping->chapterTitle)) &&
              readExpectedString(file, clipping->text);
  }
  matches = matches && file.available() == 0;
  const bool closed = file.close();
  return matches && closed;
}

bool ClippingStore::recoverPdfTransaction() const {
  if (storeFilePath.empty()) return false;

  const std::string temporaryPath = storeFilePath + PDF_TRANSACTION_TEMP_SUFFIX;
  const std::string backupPath = storeFilePath + PDF_TRANSACTION_BACKUP_SUFFIX;
  if (Storage.exists(storeFilePath.c_str())) {
    bool recovered = true;
    if (Storage.exists(temporaryPath.c_str()) && !Storage.remove(temporaryPath.c_str())) {
      LOG_ERR("CLIP", "Failed to clean stale PDF clipping transaction: %s", temporaryPath.c_str());
      recovered = false;
    }
    if (Storage.exists(backupPath.c_str()) && !Storage.remove(backupPath.c_str())) {
      LOG_ERR("CLIP", "Failed to clean stale PDF clipping backup: %s", backupPath.c_str());
      recovered = false;
    }
    return recovered;
  }

  if (Storage.exists(backupPath.c_str())) {
    if (!Storage.rename(backupPath.c_str(), storeFilePath.c_str())) {
      LOG_ERR("CLIP", "Failed to restore PDF clipping backup: %s", backupPath.c_str());
      return false;
    }
    if (Storage.exists(temporaryPath.c_str()) && !Storage.remove(temporaryPath.c_str())) {
      LOG_ERR("CLIP", "Failed to clean rolled-back PDF clipping transaction: %s", temporaryPath.c_str());
      return false;
    }
    return true;
  }

  if (Storage.exists(temporaryPath.c_str()) && !Storage.remove(temporaryPath.c_str())) {
    LOG_ERR("CLIP", "Failed to discard incomplete PDF clipping transaction: %s", temporaryPath.c_str());
    return false;
  }
  return true;
}

bool ClippingStore::hasAnyClippings() {
  if (!Storage.exists(CLIPPINGS_DIR)) return false;
  const auto files = Storage.listFiles(CLIPPINGS_DIR);
  return std::any_of(files.cbegin(), files.cend(),
                     [](const auto& name) { return !isPdfTransactionArtifact(name.c_str()); });
}

bool ClippingStore::getAllClippedBooks(std::vector<ClippedBookEntry>& out) {
  if (!Storage.exists(CLIPPINGS_DIR)) return true;

  const auto files = Storage.listFiles(CLIPPINGS_DIR);
  for (const auto& name : files) {
    if (isPdfTransactionArtifact(name.c_str())) continue;
    ClippingFileHeader header;
    const std::string fullPath = std::string(CLIPPINGS_DIR) + "/" + name.c_str();
    if (!readClippingFileHeader(fullPath, name.c_str(), header)) continue;
    if (header.path.empty() || header.count == 0 || !Storage.exists(header.path.c_str())) continue;

    auto existing = std::find_if(out.begin(), out.end(), [&](const ClippedBookEntry& entry) {
      return entry.bookPath == header.path && entry.bookType == header.bookType;
    });
    if (existing != out.end()) {
      existing->count = std::max(existing->count, header.count);
      continue;
    }
    out.push_back({std::move(header.title), std::move(header.author), std::move(header.path),
                   std::move(header.bookType), header.count});
  }
  return true;
}

bool ClippingStore::deleteForFilePath(const std::string& filePath, const std::string& bookType) {
  if (bookType == "pdf") {
    std::string path;
    if (!pdfClippingStoreFilePathForBook(filePath, path)) return false;
    return deletePdfClippingStorePaths(path);
  }
  const std::string path = storeFilePathForBook(filePath, bookType);
  return deleteStorePath(path, "clipping canonical");
}

bool ClippingStore::deleteLegacyForFilePathNoPathAlloc(const std::string_view filePath,
                                                       const std::string_view bookType) {
  if (filePath.size() > std::numeric_limits<unsigned int>::max() ||
      bookType.size() > static_cast<size_t>(std::numeric_limits<int>::max())) {
    return false;
  }

  char path[64];
  const uint32_t crc = uzlib_crc32(filePath.data(), static_cast<unsigned int>(filePath.size()), 0);
  const int written = snprintf(path, sizeof(path), "%s/%.*s_%lu.bin", CLIPPINGS_DIR, static_cast<int>(bookType.size()),
                               bookType.data(), static_cast<unsigned long>(crc));
  if (written < 0 || static_cast<size_t>(written) >= sizeof(path)) {
    LOG_ERR("CLIP", "Legacy clipping store path exceeds %u bytes", static_cast<unsigned>(sizeof(path)));
    return false;
  }
  return deleteStorePathNoPathAlloc(path, "clipping canonical");
}

bool ClippingStore::deletePdfForFilePathNoPathAlloc(const std::string_view filePath) {
  if (filePath.size() > std::numeric_limits<unsigned int>::max()) return false;

  char canonicalPath[64];
  char artifactPath[68];
  const uint32_t crc = uzlib_crc32(filePath.data(), static_cast<unsigned int>(filePath.size()), 0);
  const int canonicalWritten =
      snprintf(canonicalPath, sizeof(canonicalPath), "%s/pdf_%lu.bin", CLIPPINGS_DIR, static_cast<unsigned long>(crc));
  if (canonicalWritten < 0 || static_cast<size_t>(canonicalWritten) >= sizeof(canonicalPath) ||
      static_cast<size_t>(canonicalWritten) + sizeof(PDF_TRANSACTION_BACKUP_SUFFIX) > sizeof(artifactPath)) {
    LOG_ERR("CLIP", "PDF clipping store path exceeds bounded delete buffers");
    return false;
  }

  const auto deleteArtifact = [&](const char* const suffix, const char* const description) {
    const int written = snprintf(artifactPath, sizeof(artifactPath), "%s%s", canonicalPath, suffix);
    return written > 0 && static_cast<size_t>(written) < sizeof(artifactPath) &&
           deleteStorePathNoPathAlloc(artifactPath, description);
  };

  return deleteArtifact(PDF_TRANSACTION_BACKUP_SUFFIX, "PDF clipping backup") &&
         deleteArtifact(PDF_TRANSACTION_TEMP_SUFFIX, "PDF clipping temporary") &&
         deleteStorePathNoPathAlloc(canonicalPath, "PDF clipping canonical");
}

bool ClippingStore::copyForFilePath(const std::string& oldFilePath, const std::string& newFilePath,
                                    const std::string& bookType) {
  if (bookType != "pdf" || oldFilePath.empty() || newFilePath.empty() || oldFilePath == newFilePath) {
    return oldFilePath == newFilePath && bookType == "pdf";
  }

  // Clipping text makes the vectors dynamic; retain both vectors and their
  // Store/path controls in one fallible cold-path allocation.
  auto scratch = makeUniqueNoThrow<PdfClippingMigrationScratch>();
  if (!scratch) {
    LOG_ERR("CLIP", "Out of memory allocating clipping copy scratch");
    return false;
  }
  if (!pdfClippingStoreFilePathForBook(oldFilePath, scratch->sourcePath) ||
      !pdfClippingStoreFilePathForBook(newFilePath, scratch->destinationPath)) {
    return false;
  }
  if (scratch->sourcePath == scratch->destinationPath) {
    LOG_ERR("CLIP", "Refusing clipping copy across a path-key collision");
    return false;
  }

  scratch->store.bookFilePath = oldFilePath;
  scratch->store.storeFilePath = scratch->sourcePath;
  if (!scratch->store.recoverPdfTransaction()) return false;
  if (!Storage.exists(scratch->sourcePath.c_str())) return true;

  const std::string sourceName = scratch->sourcePath.substr(
      scratch->sourcePath.rfind('/') == std::string::npos ? 0 : scratch->sourcePath.rfind('/') + 1U);
  ClippingFileHeader header;
  if (!readClippingFileHeader(scratch->sourcePath, sourceName.c_str(), header) || header.path != oldFilePath ||
      !scratch->store.readFromFile(scratch->sourcePath, scratch->source)) {
    return false;
  }

  scratch->store.bookFilePath = newFilePath;
  scratch->store.storeFilePath = scratch->destinationPath;
  scratch->temporaryPath = scratch->destinationPath + PDF_TRANSACTION_TEMP_SUFFIX;
  scratch->backupPath = scratch->destinationPath + PDF_TRANSACTION_BACKUP_SUFFIX;
  const char* destinationSnapshot = nullptr;
  if (Storage.exists(scratch->destinationPath.c_str())) {
    destinationSnapshot = scratch->destinationPath.c_str();
  } else if (Storage.exists(scratch->backupPath.c_str())) {
    destinationSnapshot = scratch->backupPath.c_str();
  }
  if (destinationSnapshot != nullptr && !scratch->store.readFromFile(destinationSnapshot, scratch->destination)) {
    // Retry from the still-authoritative source instead of treating a torn
    // destination as permanent.
    scratch->destination.clear();
  }
  if (!mergeAuthoritativeClippings(scratch->destination, scratch->source, true)) {
    return false;
  }

  scratch->store.bookTitle = std::move(header.title);
  scratch->store.bookAuthor = std::move(header.author);
  scratch->store.clippings = std::move(scratch->destination);
  const BookMoveDurableFile::Payload payload{
      &scratch->store,
      [](void* context, void* fileContext) {
        return static_cast<ClippingStore*>(context)->writeMigrationPayload(fileContext);
      },
      [](void* context, const char* path) {
        return static_cast<ClippingStore*>(context)->verifyPdfTransaction(path, nullptr,
                                                                          std::numeric_limits<size_t>::max(), false);
      }};
  Storage.mkdir("/.crosspoint");
  Storage.mkdir(CLIPPINGS_DIR);
  const bool wrote = BookMoveDurableFile::replace(scratch->destinationPath.c_str(), scratch->temporaryPath.c_str(),
                                                  scratch->backupPath.c_str(), payload);
  if (!wrote) {
    LOG_ERR("CLIP", "Failed to copy clipping state: %s -> %s", oldFilePath.c_str(), newFilePath.c_str());
  }
  return wrote;
}

bool ClippingStore::verifyCopyForFilePath(const std::string& oldFilePath, const std::string& newFilePath,
                                          const std::string& bookType) {
  if (bookType != "pdf" || oldFilePath.empty() || newFilePath.empty() || oldFilePath == newFilePath) {
    return oldFilePath == newFilePath && bookType == "pdf";
  }

  auto scratch = makeUniqueNoThrow<PdfClippingMigrationScratch>();
  if (!scratch) {
    LOG_ERR("CLIP", "Out of memory allocating clipping verification scratch");
    return false;
  }
  if (!pdfClippingStoreFilePathForBook(oldFilePath, scratch->sourcePath) ||
      !pdfClippingStoreFilePathForBook(newFilePath, scratch->destinationPath)) {
    return false;
  }
  if (!Storage.exists(scratch->sourcePath.c_str())) return true;

  scratch->store.bookFilePath = oldFilePath;
  if (!scratch->store.readFromFile(scratch->sourcePath, scratch->source)) return false;
  if (scratch->source.empty()) return true;
  if (!Storage.exists(scratch->destinationPath.c_str())) return false;

  scratch->store.bookFilePath = newFilePath;
  return scratch->store.readFromFile(scratch->destinationPath, scratch->destination) &&
         containsAllClippings(scratch->destination, scratch->source);
}

bool ClippingStore::migratePdfForFilePath(const std::string& oldFilePath, const std::string& newFilePath,
                                          const std::string& title, const std::string& author) {
  // A rename is a cold operation. Keep its Store/vector/path control objects in
  // one fallible allocation instead of consuming most of the activity stack.
  auto scratch = makeUniqueNoThrow<PdfClippingMigrationScratch>();
  if (!scratch) {
    LOG_ERR("CLIP", "Out of memory allocating PDF clipping migration scratch");
    return false;
  }

  if (!pdfClippingStoreFilePathForBook(oldFilePath, scratch->sourcePath) ||
      !pdfClippingStoreFilePathForBook(newFilePath, scratch->destinationPath)) {
    return false;
  }

  scratch->store.bookFilePath = oldFilePath;
  scratch->store.storeFilePath = scratch->sourcePath;
  if (!scratch->store.recoverPdfTransaction()) {
    LOG_ERR("CLIP", "Failed to recover source PDF clippings before migration: %s", scratch->sourcePath.c_str());
    return false;
  }
  if (!Storage.exists(scratch->sourcePath.c_str())) {
    return true;
  }

  scratch->store.bookFilePath = newFilePath;
  scratch->store.storeFilePath = scratch->destinationPath;
  if (!scratch->store.recoverPdfTransaction()) {
    LOG_ERR("CLIP", "Failed to recover destination PDF clippings before migration: %s",
            scratch->destinationPath.c_str());
    return false;
  }

  scratch->store.bookFilePath = oldFilePath;
  if (!scratch->store.readFromFile(scratch->sourcePath, scratch->source)) {
    return false;
  }
  if (scratch->sourcePath == scratch->destinationPath) {
    scratch->store.bookFilePath = newFilePath;
    scratch->store.bookTitle = title;
    scratch->store.bookAuthor = author;
    scratch->store.clippings = std::move(scratch->source);
    if (!scratch->store.writePdfTransaction(nullptr, std::numeric_limits<size_t>::max(), false)) {
      LOG_ERR("CLIP", "Failed to durably retag colliding PDF clippings: %s", scratch->destinationPath.c_str());
      return false;
    }
    LOG_INF("CLIP", "Retagged colliding PDF clipping path: %s -> %s (%u clipping(s))", oldFilePath.c_str(),
            newFilePath.c_str(), static_cast<unsigned>(scratch->store.clippings.size()));
    return true;
  }

  scratch->store.bookFilePath = newFilePath;
  const bool hasDestinationClippings = Storage.exists(scratch->destinationPath.c_str());
  if (hasDestinationClippings && !scratch->store.readFromFile(scratch->destinationPath, scratch->destination)) {
    return false;
  }
  if (!mergePdfClippings(scratch->destination, scratch->source)) {
    return false;
  }

  scratch->store.bookTitle = title;
  scratch->store.bookAuthor = author;
  scratch->store.clippings = std::move(scratch->destination);
  if (!scratch->store.writePdfTransaction(nullptr, std::numeric_limits<size_t>::max(), false)) {
    LOG_ERR("CLIP", "Failed to durably write migrated PDF clippings: %s", scratch->destinationPath.c_str());
    return false;
  }
  if (scratch->sourcePath != scratch->destinationPath && !deletePdfClippingStorePaths(scratch->sourcePath)) {
    return false;
  }

  LOG_INF("CLIP", "Migrated PDF clipping path: %s -> %s (%u clipping(s)%s)", oldFilePath.c_str(), newFilePath.c_str(),
          static_cast<unsigned>(scratch->store.clippings.size()),
          hasDestinationClippings ? ", merged with destination" : "");
  return true;
}

bool ClippingStore::migrateLegacyForFilePath(const std::string& oldFilePath, const std::string& newFilePath,
                                             const std::string& title, const std::string& author,
                                             const std::string& bookType) {
  const std::string oldStorePath = storeFilePathForBook(oldFilePath, bookType);
  const std::string newStorePath = storeFilePathForBook(newFilePath, bookType);

  if (!Storage.exists(oldStorePath.c_str())) {
    return true;
  }

  ClippingStore reader;
  std::vector<Clipping> migratedClippings;
  if (!reader.readFromFile(oldStorePath, migratedClippings)) {
    return false;
  }

  ClippingStore writer;
  writer.bookFilePath = newFilePath;
  writer.bookTitle = title;
  writer.bookAuthor = author;
  writer.storeFilePath = newStorePath;
  writer.clippings = std::move(migratedClippings);
  if (!writer.writeToFile()) {
    return false;
  }

  const std::string newStorePath = storeFilePathForBook(newFilePath, bookType);
  if (oldStorePath == newStorePath) {
    return true;
  }

  const std::string backupPath = newStorePath + ".bak";
  const bool hasDestination = Storage.exists(newStorePath.c_str());
  if (hasDestination) {
    if (Storage.exists(backupPath.c_str()) && !Storage.remove(backupPath.c_str())) {
      LOG_ERR("CLIP", "Failed to remove stale clipping migration backup: %s", backupPath.c_str());
      return false;
    }
    if (!Storage.rename(newStorePath.c_str(), backupPath.c_str())) {
      LOG_ERR("CLIP", "Failed to back up destination clippings: %s", newStorePath.c_str());
      return false;
    }
  }
  if (!Storage.rename(oldStorePath.c_str(), newStorePath.c_str())) {
    LOG_ERR("CLIP", "Failed to rename migrated clippings: %s -> %s", oldStorePath.c_str(), newStorePath.c_str());
    if (hasDestination && !Storage.rename(backupPath.c_str(), newStorePath.c_str())) {
      LOG_ERR("CLIP", "Failed to restore destination clipping backup: %s", backupPath.c_str());
    }
    return false;
  }
  if (hasDestination && Storage.exists(backupPath.c_str())) {
    Storage.remove(backupPath.c_str());
  }
  return true;
}

bool ClippingStore::migrateForFilePath(const std::string& oldFilePath, const std::string& newFilePath,
                                       const std::string& title, const std::string& author,
                                       const std::string& bookType) {
  if (bookType != "epub" && bookType != "pdf") {
    LOG_ERR("CLIP", "Unknown clipping book type for migration: %s", bookType.c_str());
    return false;
  }
  if (bookType == "pdf") {
    return migratePdfForFilePath(oldFilePath, newFilePath, title, author);
  }
  return migrateLegacyForFilePath(oldFilePath, newFilePath, title, author, bookType);
}
