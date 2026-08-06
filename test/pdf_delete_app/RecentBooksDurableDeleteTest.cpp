#include <ArduinoJson.h>
#include <HalStorage.h>
#include <Memory.h>

#include <iostream>
#include <string>
#include <vector>

#include "../../src/RecentBooksStore.h"

namespace RecentBooksDeleteTesting {
void resetHooks();
void failReserveOnCall(size_t call);
void limitSerializedBytes(size_t length);
}  // namespace RecentBooksDeleteTesting

namespace {

constexpr char kDeletePath[] = "/Books/delete.pdf";
constexpr char kKeepPath[] = "/Books/keep.epub";

int failures = 0;

void expect(const bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
  }
}

void loadInMemoryFixture() {
  JsonDocument doc;
  JsonArray books = doc["books"].to<JsonArray>();
  JsonObject removed = books.add<JsonObject>();
  removed["path"] = kDeletePath;
  removed["title"] = "Delete";
  removed["author"] = "Author A";
  removed["coverBmpPath"] = "/cache/delete.bmp";
  JsonObject kept = books.add<JsonObject>();
  kept["path"] = kKeepPath;
  kept["title"] = "Keep";
  kept["author"] = "Author B";
  kept["coverBmpPath"] = "/cache/keep.bmp";
  expect(RECENT_BOOKS.fromJson(doc.as<JsonVariantConst>()), "fixture must load into production store");
}

void reset(const std::string& canonical) {
  Storage.reset();
  Storage.mkdir("/.crosspoint");
  TestMemory::reset();
  RecentBooksDeleteTesting::resetHooks();
  loadInMemoryFixture();
  Storage.putText(RecentBooksStore::getFilePath(), canonical);
}

bool memoryFixtureUnchanged() {
  const auto& books = RECENT_BOOKS.getBooks();
  return books.size() == 2 && books[0].path == kDeletePath && books[0].title == "Delete" &&
         books[1].path == kKeepPath && books[1].title == "Keep" &&
         books[1].author == "Author B" && books[1].coverBmpPath == "/cache/keep.bmp";
}

void expectRejectedWithoutMutation(const std::string& canonical, const std::string& context) {
  reset(canonical);
  const std::string exactBefore = Storage.text(RecentBooksStore::getFilePath());
  expect(!RECENT_BOOKS.removeByPathDurably(kDeletePath), context + " must fail closed");
  expect(Storage.text(RecentBooksStore::getFilePath()) == exactBefore,
         context + " must preserve canonical bytes exactly");
  expect(memoryFixtureUnchanged(), context + " must preserve in-memory unrelated entries and ordering");
}

void testStrictCanonicalShapeAndReadFailures() {
  expectRejectedWithoutMutation("{}", "missing books array");
  expectRejectedWithoutMutation(R"({"books":{}})", "non-array books member");
  expectRejectedWithoutMutation(
      R"({"books":[{"path":"/Books/keep.epub","title":"Keep"}])",
      "malformed canonical JSON");

  const std::string canonical =
      R"({"books":[{"path":"/Books/delete.pdf","title":"Delete"},{"path":"/Books/keep.epub","title":"Keep"}]})";
  reset(canonical);
  Storage.failNextReadOf(RecentBooksStore::getFilePath());
  expect(!RECENT_BOOKS.removeByPathDurably(kDeletePath), "transient canonical read failure must fail closed");
  expect(Storage.text(RecentBooksStore::getFilePath()) == canonical,
         "transient read failure must preserve exact canonical bytes");
  expect(memoryFixtureUnchanged(), "transient read failure must preserve in-memory state");
}

void testLegacyTolerantParserRemainsTolerant() {
  JsonDocument missingBooks;
  expect(RECENT_BOOKS.fromJson(missingBooks.as<JsonVariantConst>()),
         "legacy display parser must continue accepting a missing books member");
  expect(RECENT_BOOKS.getBooks().empty(), "legacy missing-books behavior must remain an empty list");

  JsonDocument wrongBooks;
  wrongBooks["books"].to<JsonObject>();
  expect(RECENT_BOOKS.fromJson(wrongBooks.as<JsonVariantConst>()),
         "legacy display parser must continue accepting a non-array books member");
  expect(RECENT_BOOKS.getBooks().empty(), "legacy non-array behavior must remain an empty list");
}

void testSerializationFailuresPreserveCanonicalAndMemory() {
  const std::string canonical =
      R"({"books":[{"path":"/Books/delete.pdf","title":"Delete"},{"path":"/Books/keep.epub","title":"Keep"}]})";

  reset(canonical);
  TestMemory::failNextAllocation = true;
  expect(!RECENT_BOOKS.removeByPathDurably(kDeletePath),
         "scratch allocation failure must fail before reading or durable replacement");
  expect(Storage.text(RecentBooksStore::getFilePath()) == canonical && memoryFixtureUnchanged(),
         "scratch allocation failure must preserve exact bytes and in-memory state");

  reset(canonical);
  RecentBooksDeleteTesting::failReserveOnCall(1);
  expect(!RECENT_BOOKS.removeByPathDurably(kDeletePath),
         "input String reserve failure must fail before canonical parsing");
  expect(Storage.text(RecentBooksStore::getFilePath()) == canonical && memoryFixtureUnchanged(),
         "input reserve failure must preserve exact bytes and in-memory state");

  reset(canonical);
  RecentBooksDeleteTesting::failReserveOnCall(2);
  expect(!RECENT_BOOKS.removeByPathDurably(kDeletePath),
         "output String reserve failure must fail before durable replacement");
  expect(Storage.text(RecentBooksStore::getFilePath()) == canonical && memoryFixtureUnchanged(),
         "output reserve failure must preserve exact bytes and in-memory state");

  reset(canonical);
  RecentBooksDeleteTesting::limitSerializedBytes(7);
  expect(!RECENT_BOOKS.removeByPathDurably(kDeletePath),
         "partial String concat must fail before durable replacement");
  expect(Storage.text(RecentBooksStore::getFilePath()) == canonical && memoryFixtureUnchanged(),
         "partial concat must preserve exact bytes and in-memory state");

  reset(canonical + std::string(33U * 1024U, ' '));
  const std::string oversized = Storage.text(RecentBooksStore::getFilePath());
  expect(!RECENT_BOOKS.removeByPathDurably(kDeletePath), "oversized canonical JSON must fail its RAM cap");
  expect(Storage.text(RecentBooksStore::getFilePath()) == oversized && memoryFixtureUnchanged(),
         "oversized canonical rejection must preserve exact bytes and in-memory state");
}

void testSuccessfulStrictRemovalPreservesUnrelatedEntry() {
  const std::string canonical =
      R"({"books":[{"path":"/Books/delete.pdf","title":"Delete","author":"A"},{"path":"/Books/keep.epub","title":"Keep","author":"B","coverBmpPath":"/keep.bmp"}]})";
  reset(canonical);
  expect(RECENT_BOOKS.removeByPathDurably(kDeletePath),
         "strict canonical removal must durably complete for a valid store");
  const std::string persisted = Storage.text(RecentBooksStore::getFilePath());
  expect(persisted.find(kDeletePath) == std::string::npos &&
             persisted.find(kKeepPath) != std::string::npos &&
             persisted.find("\"author\":\"B\"") != std::string::npos,
         "successful deletion must preserve the unrelated canonical entry");
  const auto& books = RECENT_BOOKS.getBooks();
  expect(books.size() == 1 && books[0].path == kKeepPath && books[0].title == "Keep",
         "successful deletion must remove only the matching in-memory entry");
  expect(Storage.maximumFileHandles() <= 1,
         "strict load and durable replacement must retain one-reader ordering");
}

}  // namespace

int main() {
  testStrictCanonicalShapeAndReadFailures();
  testLegacyTolerantParserRemainsTolerant();
  testSerializationFailuresPreserveCanonicalAndMemory();
  testSuccessfulStrictRemovalPreservesUnrelatedEntry();
  if (failures != 0) return 1;
  std::cout << "RECENT_BOOKS_DURABLE_DELETE_PASS\n";
  return 0;
}
