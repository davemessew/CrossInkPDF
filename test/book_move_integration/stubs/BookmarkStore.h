#pragma once

#include <string>
#include <unordered_map>
#include <vector>

class BookmarkStore {
 public:
  inline static std::unordered_map<std::string, std::string> stores;
  inline static bool failCopy = false;
  inline static std::vector<std::string> events;

  static bool copyForFilePath(const std::string& oldPath, const std::string& newPath, const std::string&) {
    events.emplace_back("copy-bookmarks");
    if (failCopy) {
      failCopy = false;
      return false;
    }
    const auto found = stores.find(oldPath);
    if (found != stores.end()) stores[newPath] = found->second;
    return true;
  }
  static bool verifyCopyForFilePath(const std::string& oldPath, const std::string& newPath, const std::string&) {
    events.emplace_back("verify-bookmarks");
    const auto old = stores.find(oldPath);
    if (old == stores.end()) return true;
    const auto moved = stores.find(newPath);
    return moved != stores.end() && moved->second == old->second;
  }
  static bool deleteForFilePath(const std::string& path, const std::string&) {
    events.emplace_back("delete-bookmarks");
    stores.erase(path);
    return true;
  }
  static bool migrateForFilePath(const std::string& oldPath, const std::string& newPath, const std::string&,
                                 const std::string&, const std::string&) {
    const auto found = stores.find(oldPath);
    if (found != stores.end()) {
      stores[newPath] = found->second;
      stores.erase(found);
    }
    return true;
  }
};
