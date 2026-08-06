#pragma once

#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

#include <Memory.h>

class RecentBooksStore {
 public:
  inline static std::unordered_set<std::string> paths;
  inline static std::unordered_set<std::string> persistedPaths;
  inline static std::vector<std::string> events;
  inline static bool failNextDurableRemoval = false;
  inline static bool canonicalExists = true;
  inline static bool failNextCanonicalRead = false;
  inline static bool failNextCanonicalParse = false;
  inline static std::string canonicalBytes;
  inline static bool exerciseCheckedScratchAllocation = false;

  static RecentBooksStore& getInstance() {
    static RecentBooksStore store;
    return store;
  }

  static const char* getFilePath() { return "/.crosspoint/recent.json"; }

  bool removeByPathDurably(const std::string& path) {
    events.emplace_back("recents:" + path);
    if (canonicalExists) {
      if (failNextCanonicalRead) {
        failNextCanonicalRead = false;
        return false;
      }
      if (failNextCanonicalParse) {
        failNextCanonicalParse = false;
        return false;
      }
      paths = persistedPaths;
    }
    const bool present = paths.contains(path);
    if (present) paths.erase(path);
    if (failNextDurableRemoval) {
      failNextDurableRemoval = false;
      if (present) paths.insert(path);
      return false;
    }
    persistedPaths = paths;
    canonicalBytes = "persisted-after-delete";
    return true;
  }

  bool removeByPathDurablyNoPathAlloc(const std::string_view path) {
    if (exerciseCheckedScratchAllocation) {
      exerciseCheckedScratchAllocation = false;
      auto scratch = makeUniqueNoThrow<uint8_t>();
      if (!scratch) return false;
    }
    return removeByPathDurably(std::string(path));
  }
};

#define RECENT_BOOKS RecentBooksStore::getInstance()
