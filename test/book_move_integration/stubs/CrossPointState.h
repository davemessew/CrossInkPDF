#pragma once

#include <string>
#include <vector>

class CrossPointState {
 public:
  inline static std::vector<std::string> events;
  inline static std::string persistedPath;
  inline static bool dropNextPersistence = false;

  static CrossPointState& getInstance() {
    static CrossPointState instance;
    return instance;
  }
  std::string& openBookPath() { return path_; }
  const std::string& openBookPath() const { return path_; }
  bool saveToFile() {
    events.emplace_back("save-open");
    if (dropNextPersistence) {
      dropNextPersistence = false;
    } else {
      persistedPath = path_;
    }
    return true;
  }
  bool activateOpenPathMigration(const std::string& oldPath, const std::string& newPath) {
    events.emplace_back("activate-open");
    if (path_ != oldPath) return true;
    path_ = newPath;
    if (saveToFile()) return true;
    path_ = oldPath;
    return false;
  }
  bool verifyPersistedOpenPathMigration(const std::string& oldPath, const std::string& newPath) const {
    events.emplace_back("verify-persisted-open");
    if (path_ == oldPath) return false;
    if (path_ == newPath) return persistedPath == newPath;
    return persistedPath == path_;
  }

 private:
  std::string path_;
};

#define APP_STATE CrossPointState::getInstance()
