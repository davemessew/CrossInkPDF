#pragma once

#include <string>

#include "TestState.h"

struct FakeRecentBooksStore {
  bool removeByPath(const std::string& path) {
    TEST_STATE.recentRemovals.push_back(path);
    return true;
  }

  void addOrUpdateBook(const std::string& path, const std::string& title, const std::string& author,
                       const std::string& thumbnail) {
    TEST_STATE.recentAdds.push_back({path, title, author, thumbnail});
  }
};

extern FakeRecentBooksStore TEST_RECENT_BOOKS;
#define RECENT_BOOKS TEST_RECENT_BOOKS
