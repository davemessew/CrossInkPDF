#pragma once

#include <cstddef>

class JsonArray;
class JsonArrayConst;
class JsonObject;
class JsonObjectConst;
class JsonVariantConst {};

class JsonVariant {
 public:
  template <typename T>
  T to() {
    return {};
  }
  template <typename T>
  JsonVariant& operator=(T&&) {
    return *this;
  }
  template <typename T>
  T operator|(T fallback) const {
    return fallback;
  }
  operator JsonArrayConst() const;
};

class JsonObject {
 public:
  JsonVariant operator[](const char*) { return {}; }
};

class JsonObjectConst {
 public:
  JsonVariant operator[](const char*) const { return {}; }
};

class JsonArray {
 public:
  template <typename T>
  T add() {
    return {};
  }
};

class JsonArrayConst {
 public:
  class Iterator {
   public:
    bool operator!=(const Iterator&) const { return false; }
    Iterator& operator++() { return *this; }
    JsonObjectConst operator*() const { return {}; }
  };

  bool isNull() const { return true; }
  Iterator begin() const { return {}; }
  Iterator end() const { return {}; }
};

inline JsonVariant::operator JsonArrayConst() const { return {}; }

class JsonDocument {
 public:
  template <typename T>
  T to() {
    return {};
  }
  template <typename T>
  T as() const {
    return {};
  }
  JsonVariant operator[](const char*) { return {}; }
};

class DeserializationError {
 public:
  explicit operator bool() const { return true; }
  const char* c_str() const { return "fixture JSON disabled"; }
};

namespace DeserializationOption {
struct FilterOption {};
inline FilterOption Filter(JsonVariantConst) { return {}; }
}  // namespace DeserializationOption

inline DeserializationError deserializeJson(JsonDocument&, const char*, size_t,
                                            DeserializationOption::FilterOption) {
  return {};
}
