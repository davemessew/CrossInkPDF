#pragma once

#include <string>

class Xtc {
 public:
  Xtc(const std::string&, const std::string&) {}
  bool load() { return false; }
  std::string getTitle() const { return {}; }
  std::string getAuthor() const { return {}; }
  std::string getThumbBmpPath() const { return {}; }
};
