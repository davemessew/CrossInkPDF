#pragma once

#include <string>

class UITheme {
 public:
  static std::string getCoverThumbPath(const std::string& path, int) { return path; }
  static std::string getCoverThumbPath(const std::string& path, int, int) { return path; }
};
