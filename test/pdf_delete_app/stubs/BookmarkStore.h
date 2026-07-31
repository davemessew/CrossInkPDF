#pragma once

#include <string>
#include <unordered_set>
#include <vector>

class BookmarkStore {
 public:
  inline static std::unordered_set<std::string> books;
  inline static std::vector<std::string> events;
  inline static bool failNextDelete = false;

  static bool deleteForFilePath(const std::string& path, const std::string& type) {
    events.emplace_back("bookmarks:" + path + ":" + type);
    if (failNextDelete) {
      failNextDelete = false;
      return false;
    }
    books.erase(path);
    return true;
  }
};
