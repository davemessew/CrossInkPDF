#include "RecentBooksStore.h"

#include <Epub.h>
#include <FsHelpers.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>
#include <Serialization.h>
#include <Xtc.h>

#include <algorithm>
#include <cstring>
#include <iterator>
#include <limits>
#include <utility>

#include "PdfSourceIdentity.h"
#include "util/BookMoveUtils.h"
#include "util/BookMoveDurableFile.h"

namespace {
constexpr uint8_t RECENT_BOOKS_FILE_VERSION = 3;
constexpr char RECENT_BOOKS_FILE_BIN[] = "/.crosspoint/recent.bin";
constexpr char RECENT_BOOKS_FILE_BAK[] = "/.crosspoint/recent.bin.bak";
constexpr char RECENT_BOOKS_MOVE_TEMP[] =
    "/.crosspoint/recent.json.move.tmp";
constexpr char RECENT_BOOKS_MOVE_BACKUP[] =
    "/.crosspoint/recent.json.move.bak";
constexpr char RECENT_BOOKS_DELETE_TEMP[] =
    "/.crosspoint/recent.json.delete.tmp";
constexpr char RECENT_BOOKS_DELETE_BACKUP[] =
    "/.crosspoint/recent.json.delete.bak";
constexpr size_t RECENT_BOOKS_DELETE_JSON_MAX_BYTES = 32U * 1024U;
constexpr size_t RECENT_BOOKS_DELETE_READ_CHUNK_BYTES = 128U;

struct RemovedRecentEntry {
  RecentBook book;
  size_t index = 0;
  bool present = false;
};

struct RecentActivationScratch {
  RemovedRecentEntry removed[2];
  String json;
  bool coverChanged = false;
};
static_assert(sizeof(RecentActivationScratch) <= 384);

struct RecentRemovalScratch {
  String json;
};
// The native simulator's pinned String wrapper is 40 bytes; Arduino String is
// smaller. This heap object contains only that bounded header, never the JSON
// payload inline.
static_assert(sizeof(RecentRemovalScratch) <= 48);

#if defined(PDF_RECENT_DELETE_TESTING)
size_t recentDeleteReserveCalls = 0;
size_t recentDeleteFailReserveCall = 0;
size_t recentDeleteSerializedByteLimit = std::numeric_limits<size_t>::max();
#endif

struct JsonBytes {
  const char* data = nullptr;
  size_t length = 0;
};

bool writeJsonBytes(void* const context, void* const fileContext) {
  if (context == nullptr || fileContext == nullptr) return false;
  const auto& payload = *static_cast<const JsonBytes*>(context);
  auto& file = *static_cast<FsFile*>(fileContext);
  return payload.data != nullptr &&
         file.write(reinterpret_cast<const uint8_t*>(payload.data),
                    payload.length) == payload.length;
}

bool verifyJsonBytes(void* const context, const char* const path) {
  if (context == nullptr || path == nullptr) return false;
  const auto& payload = *static_cast<const JsonBytes*>(context);
  FsFile file;
  if (payload.data == nullptr ||
      !Storage.openFileForRead("RBS", path, file)) {
    return false;
  }

  uint8_t actual[128];
  size_t offset = 0;
  bool matches = true;
  while (offset < payload.length) {
    const size_t chunk =
        std::min(sizeof(actual), payload.length - offset);
    const int count = file.read(actual, chunk);
    if (count != static_cast<int>(chunk) ||
        std::memcmp(actual, payload.data + offset, chunk) != 0) {
      matches = false;
      break;
    }
    offset += chunk;
  }
  matches = matches && file.available() == 0;
  const bool closed = file.close();
  return matches && closed;
}

bool cleanupRecentMoveArtifacts() {
  bool cleaned = true;
  if (Storage.exists(RECENT_BOOKS_MOVE_TEMP) &&
      !Storage.remove(RECENT_BOOKS_MOVE_TEMP)) {
    cleaned = false;
  }
  if (Storage.exists(RECENT_BOOKS_MOVE_BACKUP) &&
      !Storage.remove(RECENT_BOOKS_MOVE_BACKUP)) {
    cleaned = false;
  }
  return cleaned;
}

bool prepareRecentDeleteStateForRead() {
  if (BookMoveDurableFile::restoreCanonicalForRead(
      RecentBooksStore::getFilePath(), RECENT_BOOKS_DELETE_TEMP,
      RECENT_BOOKS_DELETE_BACKUP)) {
    return true;
  }
  LOG_ERR("RBS", "Failed to recover interrupted recent-book deletion");
  return false;
}

bool reserveRecentDeleteString(String& value, const size_t capacity) {
#if defined(PDF_RECENT_DELETE_TESTING)
  ++recentDeleteReserveCalls;
  if (recentDeleteFailReserveCall != 0 &&
      recentDeleteReserveCalls == recentDeleteFailReserveCall) {
    return false;
  }
#endif
#if defined(SIMULATOR)
  // The pinned native simulator String shim has no reserve() API. Production
  // firmware takes the checked Arduino String branch below.
  (void)value;
  (void)capacity;
  return true;
#else
  return capacity <= std::numeric_limits<unsigned int>::max() &&
         value.reserve(static_cast<unsigned int>(capacity));
#endif
}

// Keep the 128-byte SD read buffer out of the caller's durable-replacement
// frame on the ESP32-C3.
[[gnu::noinline]] bool readStrictRecentDeleteDocument(JsonDocument& doc, String& json) {
  FsFile file;
  if (!Storage.openFileForRead("RBS", RecentBooksStore::getFilePath(), file)) {
    return false;
  }

  const uint64_t fileSize = file.fileSize64();
  if (fileSize == 0 || fileSize > RECENT_BOOKS_DELETE_JSON_MAX_BYTES ||
      !reserveRecentDeleteString(json, static_cast<size_t>(fileSize))) {
    file.close();
    return false;
  }

  uint8_t chunk[RECENT_BOOKS_DELETE_READ_CHUNK_BYTES];
  size_t total = 0;
  bool readComplete = true;
  while (total < fileSize) {
    const size_t wanted = std::min(
        sizeof(chunk), static_cast<size_t>(fileSize - total));
    const int count = file.read(chunk, wanted);
    if (count <= 0 || static_cast<size_t>(count) > wanted) {
      readComplete = false;
      break;
    }
    const size_t before = json.length();
    if (!json.concat(reinterpret_cast<const char*>(chunk),
                     static_cast<unsigned int>(count)) ||
        json.length() != before + static_cast<size_t>(count)) {
      readComplete = false;
      break;
    }
    total += static_cast<size_t>(count);
  }
  const bool closed = file.close();
  if (!readComplete || !closed || total != fileSize ||
      json.length() != fileSize) {
    return false;
  }

  const DeserializationError error = deserializeJson(doc, json);
  if (error || doc.overflowed()) return false;
  const JsonVariantConst root = doc.as<JsonVariantConst>();
  if (!root.is<JsonObjectConst>()) return false;
  const JsonVariantConst books = root["books"];
  if (!books.is<JsonArrayConst>()) return false;
  for (JsonVariantConst entry : books.as<JsonArrayConst>()) {
    if (!entry.is<JsonObjectConst>()) return false;
    const JsonVariantConst path = entry["path"];
    if (!path.is<const char*>() || std::strlen(path.as<const char*>()) == 0) {
      return false;
    }
  }
  return true;
}

class RecentDeleteJsonWriter {
 public:
  explicit RecentDeleteJsonWriter(String& destination)
      : destination_(destination) {}

  size_t write(const uint8_t value) { return write(&value, 1); }

  size_t write(const uint8_t* const bytes, const size_t length) {
    if (bytes == nullptr || length == 0 || !complete_) return 0;
    size_t permitted = length;
#if defined(PDF_RECENT_DELETE_TESTING)
    if (destination_.length() >= recentDeleteSerializedByteLimit) {
      permitted = 0;
    } else {
      permitted = std::min(
          permitted, recentDeleteSerializedByteLimit - destination_.length());
    }
#endif
    const size_t before = destination_.length();
    if (permitted == 0 ||
        !destination_.concat(reinterpret_cast<const char*>(bytes),
                             static_cast<unsigned int>(permitted)) ||
        destination_.length() != before + permitted) {
      complete_ = false;
      return destination_.length() - before;
    }
    if (permitted != length) complete_ = false;
    return permitted;
  }

  bool complete() const { return complete_; }

 private:
  String& destination_;
  bool complete_ = true;
};

// ArduinoJson's writer state is bounded, but combining it with the SD read and
// replacement locals would exceed the firmware's 256-byte stack-frame budget.
[[gnu::noinline]] bool serializeRecentDeleteDocument(const JsonDocument& doc, String& json) {
  if (doc.overflowed()) return false;
  const size_t measured = measureJson(doc);
  if (measured == 0 || measured > RECENT_BOOKS_DELETE_JSON_MAX_BYTES) {
    return false;
  }

  json = static_cast<const char*>(nullptr);
  if (!reserveRecentDeleteString(json, measured)) return false;
  RecentDeleteJsonWriter writer(json);
  const size_t written = serializeJson(doc, writer);
  return writer.complete() && !doc.overflowed() && written == measured &&
         json.length() == measured;
}
}  // namespace

#if defined(PDF_RECENT_DELETE_TESTING)
namespace RecentBooksDeleteTesting {

void resetHooks() {
  recentDeleteReserveCalls = 0;
  recentDeleteFailReserveCall = 0;
  recentDeleteSerializedByteLimit = std::numeric_limits<size_t>::max();
}

void failReserveOnCall(const size_t call) {
  recentDeleteFailReserveCall = call;
}

void limitSerializedBytes(const size_t length) {
  recentDeleteSerializedByteLimit = length;
}

}  // namespace RecentBooksDeleteTesting
#endif

void RecentBooksStore::toJson(JsonDocument& doc) const {
  JsonArray arr = doc["books"].to<JsonArray>();
  for (const auto& book : recentBooks) {
    JsonObject obj = arr.add<JsonObject>();
    obj["path"] = book.path;
    obj["title"] = book.title;
    obj["author"] = book.author;
    obj["coverBmpPath"] = book.coverBmpPath;
  }
}

bool RecentBooksStore::fromJson(JsonVariantConst doc) {
  // Tolerate a missing/invalid 'books' key (treat as empty list); only a
  // JSON parse error is fatal. A null JsonArray iterates zero times.
  recentBooks.clear();
  JsonArrayConst arr = doc["books"].as<JsonArrayConst>();
  recentBooks.reserve(std::min(arr.size(), static_cast<size_t>(MAX_RECENT_BOOKS)));
  for (JsonObjectConst obj : arr) {
    if (getCount() >= MAX_RECENT_BOOKS) break;
    RecentBook book;
    book.path = obj["path"] | "";
    book.title = obj["title"] | "";
    book.author = obj["author"] | "";
    book.coverBmpPath = obj["coverBmpPath"] | "";
    recentBooks.push_back(book);
  }

  LOG_DBG("RBS", "Recent books loaded from file (%d entries)", getCount());
  return true;
}

void RecentBooksStore::addBook(const std::string& path, const std::string& title, const std::string& author,
                               const std::string& coverBmpPath) {
  addOrUpdateBook(path, title, author, coverBmpPath);
}

void RecentBooksStore::addOrUpdateBook(const std::string& path, const std::string& title, const std::string& author,
                                       const std::string& coverBmpPath) {
  // Drop stale entries first so a new add can't evict a valid book in their stead.
  pruneMissing();

  // Remove existing entry if present
  auto it =
      std::find_if(recentBooks.begin(), recentBooks.end(), [&](const RecentBook& book) { return book.path == path; });
  if (it != recentBooks.end()) {
    it->title = title;
    it->author = author;
    it->coverBmpPath = coverBmpPath;
    if (it != recentBooks.begin()) {
      RecentBook book = std::move(*it);
      recentBooks.erase(it);
      recentBooks.insert(recentBooks.begin(), std::move(book));
    }
  } else {
    recentBooks.insert(recentBooks.begin(), {path, title, author, coverBmpPath});
    if (recentBooks.size() > MAX_RECENT_BOOKS) {
      recentBooks.resize(MAX_RECENT_BOOKS);
    }
  }
  saveToFile();
}

bool RecentBooksStore::updateBook(const std::string& path, const std::string& title, const std::string& author,
                                  const std::string& coverBmpPath) {
  auto it =
      std::find_if(recentBooks.begin(), recentBooks.end(), [&](const RecentBook& book) { return book.path == path; });
  if (it == recentBooks.end()) {
    return false;
  }
  RecentBook& book = *it;
  book.title = title;
  book.author = author;
  book.coverBmpPath = coverBmpPath;
  saveToFile();
  return true;
}

bool RecentBooksStore::removeByPath(const std::string& path) {
  auto it =
      std::find_if(recentBooks.begin(), recentBooks.end(), [&](const RecentBook& book) { return book.path == path; });
  if (it == recentBooks.end()) {
    return false;
  }
  recentBooks.erase(it);
  if (!saveToFile()) {
    LOG_ERR("RBS", "Failed to persist removal of recent book: %s", path.c_str());
  }
  return true;
}

bool RecentBooksStore::removeByPathDurably(const std::string& path) {
  return removeByPathDurablyNoPathAlloc(path);
}

bool RecentBooksStore::removeByPathDurablyNoPathAlloc(
    const std::string_view path) {
  if (!prepareRecentDeleteStateForRead()) return false;

  auto scratch = makeUniqueNoThrow<RecentRemovalScratch>();
  if (!scratch) {
    LOG_ERR("RBS", "Out of memory removing recent book durably");
    return false;
  }

  JsonDocument doc;
  if (Storage.exists(getFilePath())) {
    if (!readStrictRecentDeleteDocument(doc, scratch->json)) {
      LOG_ERR("RBS", "Failed strict canonical recents load before deletion");
      return false;
    }
  } else {
    toJson(doc);
    if (doc.overflowed()) {
      LOG_ERR("RBS", "Recent-book deletion JSON allocation overflow");
      return false;
    }
  }

  JsonArray books = doc["books"].as<JsonArray>();
  for (size_t index = books.size(); index > 0; --index) {
    const char* const candidate = books[index - 1U]["path"] | "";
    const size_t candidateLength = std::strlen(candidate);
    if (path.size() == candidateLength &&
        std::memcmp(path.data(), candidate, candidateLength) == 0) {
      books.remove(index - 1U);
    }
  }

  bool persisted = false;
  if (serializeRecentDeleteDocument(doc, scratch->json)) {
    JsonBytes bytes{scratch->json.c_str(), scratch->json.length()};
    const BookMoveDurableFile::Payload payload{&bytes, &writeJsonBytes, &verifyJsonBytes};
    Storage.mkdir("/.crosspoint");
    persisted = BookMoveDurableFile::replace(getFilePath(), RECENT_BOOKS_DELETE_TEMP,
                                             RECENT_BOOKS_DELETE_BACKUP, payload);
  } else {
    LOG_ERR("RBS", "Failed to serialize recent-book deletion state");
  }
  if (!persisted) return false;

  const auto found = std::find_if(
      recentBooks.begin(), recentBooks.end(), [&](const RecentBook& book) {
        return book.path.size() == path.size() &&
               std::memcmp(book.path.data(), path.data(), path.size()) == 0;
      });
  if (found != recentBooks.end()) recentBooks.erase(found);
  return true;
}

void RecentBooksStore::updatePath(const std::string& oldPath, const std::string& newPath,
                                  const std::string& oldCachePath, const std::string& newCachePath) {
  auto it = std::find_if(recentBooks.begin(), recentBooks.end(),
                         [&](const RecentBook& book) { return book.path == oldPath; });
  if (it == recentBooks.end()) {
    return;
  }
  it->path = newPath;
  if (!oldCachePath.empty() && !it->coverBmpPath.empty() && it->coverBmpPath.rfind(oldCachePath, 0) == 0) {
    it->coverBmpPath = newCachePath + it->coverBmpPath.substr(oldCachePath.size());
  }
  saveToFile();
}

bool RecentBooksStore::activatePathMigration(
    const std::string& oldPath, const std::string& newPath,
    const std::string& oldCachePath, const std::string& newCachePath,
    const bool keepInRecents) {
  auto scratch = makeUniqueNoThrow<RecentActivationScratch>();
  if (!scratch) {
    LOG_ERR("RBS", "Out of memory activating moved recent book");
    return false;
  }

  auto oldEntry =
      std::find_if(recentBooks.begin(), recentBooks.end(), [&](const RecentBook& book) { return book.path == oldPath; });
  auto newEntry =
      std::find_if(recentBooks.begin(), recentBooks.end(), [&](const RecentBook& book) { return book.path == newPath; });

  auto removeInto = [&](const std::string& path, RemovedRecentEntry& removed) {
    const auto found = std::find_if(
        recentBooks.begin(), recentBooks.end(),
        [&](const RecentBook& book) { return book.path == path; });
    if (found == recentBooks.end()) return;
    removed.index = static_cast<size_t>(found - recentBooks.begin());
    removed.book = std::move(*found);
    removed.present = true;
    recentBooks.erase(found);
  };
  auto rollbackRemoved = [&]() {
    for (int index = 1; index >= 0; --index) {
      RemovedRecentEntry& removed = scratch->removed[index];
      if (!removed.present) continue;
      recentBooks.insert(
          recentBooks.begin() + static_cast<std::ptrdiff_t>(removed.index),
          std::move(removed.book));
    }
  };

  bool pathChanged = false;
  if (!keepInRecents) {
    removeInto(oldPath, scratch->removed[0]);
    removeInto(newPath, scratch->removed[1]);
  } else if (oldEntry != recentBooks.end() &&
             newEntry != recentBooks.end()) {
    removeInto(oldPath, scratch->removed[0]);
  } else if (oldEntry != recentBooks.end()) {
    oldEntry->path = newPath;
    pathChanged = true;
    if (!oldCachePath.empty() && !oldEntry->coverBmpPath.empty() &&
        oldEntry->coverBmpPath.rfind(oldCachePath, 0) == 0) {
      oldEntry->coverBmpPath.replace(0, oldCachePath.size(), newCachePath);
      scratch->coverChanged = true;
    }
  }

  JsonDocument doc;
  toJson(doc);
  bool persisted = false;
  if (serializeJson(doc, scratch->json) != 0) {
    JsonBytes bytes{scratch->json.c_str(), scratch->json.length()};
    const BookMoveDurableFile::Payload payload{
        &bytes, &writeJsonBytes, &verifyJsonBytes};
    Storage.mkdir("/.crosspoint");
    persisted = BookMoveDurableFile::replace(
        getFilePath(), RECENT_BOOKS_MOVE_TEMP, RECENT_BOOKS_MOVE_BACKUP,
        payload);
  } else {
    LOG_ERR("RBS", "Failed to serialize moved recent book state");
  }
  if (persisted) return true;

  if (pathChanged) {
    oldEntry = std::find_if(recentBooks.begin(), recentBooks.end(),
                            [&](const RecentBook& book) {
                              return book.path == newPath;
                            });
    if (oldEntry != recentBooks.end()) {
      oldEntry->path = oldPath;
      if (scratch->coverChanged &&
          oldEntry->coverBmpPath.rfind(newCachePath, 0) == 0) {
        oldEntry->coverBmpPath.replace(0, newCachePath.size(),
                                      oldCachePath);
      }
    }
  }
  rollbackRemoved();
  return false;
}

bool RecentBooksStore::verifyPathMigration(
    const std::string& oldPath, const std::string& newPath,
    const bool keepInRecents) const {
  const bool oldPresent =
      std::any_of(recentBooks.begin(), recentBooks.end(), [&](const RecentBook& book) { return book.path == oldPath; });
  if (oldPresent) return false;
  const size_t newCount =
      static_cast<size_t>(std::count_if(recentBooks.begin(), recentBooks.end(),
                                        [&](const RecentBook& book) { return book.path == newPath; }));
  return keepInRecents ? newCount <= 1 : newCount == 0;
}

bool RecentBooksStore::verifyPersistedPathMigration(
    const std::string& oldPath, const std::string& newPath,
    const bool keepInRecents) const {
  if (!Storage.exists(getFilePath())) return false;
  const String json = Storage.readFile(getFilePath());
  if (json.isEmpty()) return false;
  JsonDocument doc;
  if (deserializeJson(doc, json)) return false;

  size_t oldCount = 0;
  size_t newCount = 0;
  for (JsonObjectConst object : doc["books"].as<JsonArrayConst>()) {
    const char* const path = object["path"] | "";
    oldCount += static_cast<size_t>(oldPath == path);
    newCount += static_cast<size_t>(newPath == path);
  }
  return oldCount == 0 &&
         (keepInRecents ? newCount <= 1 : newCount == 0);
}

bool RecentBooksStore::isMissing(const RecentBook& book) {
  if (Storage.exists(book.path.c_str())) {
    return false;
  }
  if (!FsHelpers::hasPdfExtension(book.path)) {
    return true;
  }

  const uint64_t normalCacheHash = pdfPathHash64(book.path.c_str(), book.path.size());
  uint64_t resolvedCacheHash = normalCacheHash;
  bool readOnlyFallback = true;
  if (!BookMoveUtils::migrationCacheHash(book.path, normalCacheHash, &resolvedCacheHash, &readOnlyFallback)) {
    // An unreadable move journal is not proof that a recent PDF disappeared.
    return false;
  }
  (void)resolvedCacheHash;
  return !readOnlyFallback;
}

bool RecentBooksStore::pruneMissing() {
  const size_t before = recentBooks.size();
  recentBooks.erase(std::remove_if(recentBooks.begin(), recentBooks.end(), &isMissing), recentBooks.end());
  return recentBooks.size() != before;
}

RecentBook RecentBooksStore::getDataFromBook(std::string path) const {
  std::string lastBookFileName = "";
  const size_t lastSlash = path.find_last_of('/');
  if (lastSlash != std::string::npos) {
    lastBookFileName = path.substr(lastSlash + 1);
  }

  LOG_DBG("RBS", "Loading recent book: %s", path.c_str());

  // If epub, try to load the metadata for title/author and cover.
  // Use buildIfMissing=false to avoid heavy epub loading on boot; getTitle()/getAuthor() may be
  // blank until the book is opened, and entries with missing title are omitted from recent list.
  if (FsHelpers::hasEpubExtension(lastBookFileName)) {
    Epub epub(path, "/.crosspoint");
    epub.load(false, true);
    return RecentBook{path, epub.getTitle(), epub.getAuthor(), epub.getThumbBmpPath()};
  } else if (FsHelpers::hasXtcExtension(lastBookFileName)) {
    // Handle XTC file
    Xtc xtc(path, "/.crosspoint");
    if (xtc.load()) {
      return RecentBook{path, xtc.getTitle(), xtc.getAuthor(), xtc.getThumbBmpPath()};
    }
  } else if (FsHelpers::hasTxtExtension(lastBookFileName) || FsHelpers::hasMarkdownExtension(lastBookFileName)) {
    return RecentBook{path, lastBookFileName, "", ""};
  }
  return RecentBook{path, "", "", ""};
}

bool RecentBooksStore::loadFromFile() {
  if (!prepareRecentDeleteStateForRead()) return false;

  if (!Storage.exists(getFilePath())) {
    if (Storage.exists(RECENT_BOOKS_MOVE_BACKUP)) {
      if (!Storage.rename(RECENT_BOOKS_MOVE_BACKUP, getFilePath())) {
        LOG_ERR("RBS", "Failed to restore interrupted recent-books move");
        return false;
      }
      (void)Storage.remove(RECENT_BOOKS_MOVE_TEMP);
    } else if (Storage.exists(RECENT_BOOKS_MOVE_TEMP) &&
               !Storage.rename(RECENT_BOOKS_MOVE_TEMP, getFilePath())) {
      LOG_ERR("RBS", "Failed to promote interrupted recent-books move");
      return false;
    }
  }

  const bool hasStoreFile = Storage.exists(getFilePath());
  if (PersistableStore<RecentBooksStore>::loadFromFile()) {
    if (!cleanupRecentMoveArtifacts()) {
      LOG_ERR("RBS", "Failed to clean recent-books move artifacts");
    }
    return true;
  }
  if (hasStoreFile && Storage.exists(RECENT_BOOKS_MOVE_BACKUP)) {
    if (!Storage.remove(getFilePath()) ||
        !Storage.rename(RECENT_BOOKS_MOVE_BACKUP, getFilePath()) ||
        !cleanupRecentMoveArtifacts()) {
      LOG_ERR("RBS", "Failed to roll back interrupted recent-books move");
      return false;
    }
    return PersistableStore<RecentBooksStore>::loadFromFile();
  }
  if (hasStoreFile) {
    return false;
  }

  if (Storage.exists(RECENT_BOOKS_FILE_BIN)) {
    if (loadFromBinaryFile()) {
      saveToFile();
      Storage.rename(RECENT_BOOKS_FILE_BIN, RECENT_BOOKS_FILE_BAK);
      LOG_DBG("RBS", "Migrated recent.bin to recent.json");
      return true;
    }
  }

  return false;
}

bool RecentBooksStore::loadFromBinaryFile() {
  HalFile inputFile;
  if (!Storage.openFileForRead("RBS", RECENT_BOOKS_FILE_BIN, inputFile)) {
    return false;
  }

  uint8_t version;
  serialization::readPod(inputFile, version);
  if (version == 1 || version == 2) {
    // Old version, just read paths
    uint8_t count;
    serialization::readPod(inputFile, count);
    recentBooks.clear();
    recentBooks.reserve(count);
    for (uint8_t i = 0; i < count; i++) {
      std::string path;
      serialization::readString(inputFile, path);

      // load book to get missing data
      RecentBook book = getDataFromBook(path);
      if (book.title.empty() && book.author.empty() && version == 2) {
        // Fall back to loading what we can from the store
        std::string title, author;
        serialization::readString(inputFile, title);
        serialization::readString(inputFile, author);
        recentBooks.push_back({path, title, author, ""});
      } else {
        recentBooks.push_back(book);
      }
    }
  } else if (version == RECENT_BOOKS_FILE_VERSION) {
    uint8_t count;
    serialization::readPod(inputFile, count);

    recentBooks.clear();
    recentBooks.reserve(count);
    uint8_t omitted = 0;

    for (uint8_t i = 0; i < count; i++) {
      std::string path, title, author, coverBmpPath;
      serialization::readString(inputFile, path);
      serialization::readString(inputFile, title);
      serialization::readString(inputFile, author);
      serialization::readString(inputFile, coverBmpPath);

      // Omit books with missing title (e.g. saved before metadata was available)
      if (title.empty()) {
        omitted++;
        continue;
      }

      recentBooks.push_back({path, title, author, coverBmpPath});
    }

    if (omitted > 0) {
      LOG_DBG("RBS", "Omitted %u recent book(s) with missing title", omitted);
      return true;
    }
  } else {
    LOG_ERR("RBS", "Deserialization failed: Unknown version %u", version);
    return false;
  }

  LOG_DBG("RBS", "Recent books loaded from binary file (%d entries)", static_cast<int>(recentBooks.size()));
  return true;
}
