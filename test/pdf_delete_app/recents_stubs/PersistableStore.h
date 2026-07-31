#pragma once

#include <ArduinoJson.h>
#include <HalStorage.h>

class PersistableStoreBase {
 protected:
  static bool writeDocToFile(const char* path, const JsonDocument& doc) {
    String json;
    serializeJson(doc, json);
    return Storage.writeFile(path, json);
  }

  static bool readDocFromFile(const char* path, JsonDocument& doc) {
    if (!Storage.exists(path)) return false;
    const String json = Storage.readFile(path);
    return !json.isEmpty() && !deserializeJson(doc, json);
  }
};

template <typename T>
class PersistableStore : public PersistableStoreBase {
 public:
  static T& getInstance() {
    static T instance;
    return instance;
  }

  bool saveToFile() const {
    JsonDocument doc;
    static_cast<const T*>(this)->toJson(doc);
    return writeDocToFile(T::getFilePath(), doc);
  }

  bool loadFromFile() {
    JsonDocument doc;
    if (!readDocFromFile(T::getFilePath(), doc)) return false;
    return static_cast<T*>(this)->fromJson(doc.as<JsonVariantConst>());
  }
};
