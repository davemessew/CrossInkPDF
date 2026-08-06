#pragma once

#include <string>
#include <unordered_set>
#include <vector>

class RecentBooksStore {
 public:
  inline static std::unordered_set<std::string> paths;
  inline static std::unordered_set<std::string> persistedPaths;
  inline static std::vector<std::string> events;
  inline static bool dropNextPersistence = false;

  static RecentBooksStore& getInstance() {
    static RecentBooksStore instance;
    return instance;
  }
  bool activatePathMigration(const std::string& oldPath, const std::string& newPath, const std::string&,
                             const std::string&, const bool keepInRecents = true) {
    events.emplace_back("activate-recent");
    if (keepInRecents) {
      if (paths.erase(oldPath) != 0) paths.insert(newPath);
    } else {
      paths.erase(oldPath);
      paths.erase(newPath);
    }
    if (dropNextPersistence) {
      dropNextPersistence = false;
    } else {
      persistedPaths = paths;
    }
    return true;
  }
  bool verifyPathMigration(const std::string& oldPath, const std::string&) const {
    events.emplace_back("verify-recent");
    return !paths.contains(oldPath);
  }
  bool verifyPersistedPathMigration(const std::string& oldPath, const std::string& newPath,
                                    const bool keepInRecents) const {
    events.emplace_back("verify-persisted-recent");
    if (!keepInRecents) {
      return !persistedPaths.contains(oldPath) && !persistedPaths.contains(newPath);
    }
    return !persistedPaths.contains(oldPath) && persistedPaths.contains(newPath);
  }
  void updatePath(const std::string& oldPath, const std::string& newPath, const std::string&, const std::string&) {
    if (paths.erase(oldPath) != 0) paths.insert(newPath);
    persistedPaths = paths;
  }
  bool removeByPath(const std::string& path) {
    const bool removed = paths.erase(path) != 0;
    persistedPaths = paths;
    return removed;
  }
};

#define RECENT_BOOKS RecentBooksStore::getInstance()
