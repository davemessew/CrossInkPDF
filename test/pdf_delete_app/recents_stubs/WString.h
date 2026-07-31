#pragma once

#ifndef ARDUINOJSON_ENABLE_ARDUINO_STRING
#define ARDUINOJSON_ENABLE_ARDUINO_STRING 1
#endif

#include <cstdint>
#include <cstring>
#include <string>

class String {
 public:
  String() = default;
  String(const char* value) : value_(value == nullptr ? "" : value) {}
  explicit String(const std::string& value) : value_(value) {}

  String& operator=(const char* value) {
    value_ = value == nullptr ? "" : value;
    return *this;
  }

  unsigned char reserve(const size_t capacity) {
    value_.reserve(capacity);
    return value_.capacity() >= capacity;
  }

  unsigned char concat(const char* value) {
    if (value == nullptr) return 0;
    value_ += value;
    return 1;
  }

  unsigned char concat(const char* value, const unsigned int length) {
    if (value == nullptr) return 0;
    value_.append(value, length);
    return 1;
  }

  size_t write(const uint8_t value) {
    value_.push_back(static_cast<char>(value));
    return 1;
  }

  size_t write(const uint8_t* value, const size_t length) {
    if (value == nullptr) return 0;
    value_.append(reinterpret_cast<const char*>(value), length);
    return length;
  }

  bool isEmpty() const { return value_.empty(); }
  size_t length() const { return value_.size(); }
  const char* c_str() const { return value_.c_str(); }

 private:
  std::string value_;
};
