#include "CrossPointState.h"

#include <ArduinoJson.h>
#include <HalStorage.h>
#include <Logging.h>
#include <PersistableStore.h>
#include <Serialization.h>

#include <algorithm>
#include <mutex>

#include "JsonSettingsIO.h"
#include "util/BookMoveDurableFile.h"

namespace {
constexpr uint8_t STATE_FILE_VERSION = 5;
constexpr char STATE_FILE_BIN[] = "/.crosspoint/state.bin";
constexpr char STATE_FILE_JSON[] = "/.crosspoint/state.json";
constexpr char STATE_FILE_BAK[] = "/.crosspoint/state.bin.bak";
constexpr char STATE_MOVE_TEMP[] = "/.crosspoint/state.json.move.tmp";
constexpr char STATE_MOVE_BACKUP[] = "/.crosspoint/state.json.move.bak";

bool writeStatePayload(void* const context, void* const fileContext) {
  if (context == nullptr || fileContext == nullptr) return false;
  auto& state = *static_cast<CrossPointState*>(context);
  auto& file = *static_cast<FsFile*>(fileContext);
  return JsonSettingsIO::writeState(state, file);
}

bool verifyStatePayload(void* const context, const char* const path) {
  if (context == nullptr || path == nullptr || !Storage.exists(path)) {
    return false;
  }
  const auto& state = *static_cast<const CrossPointState*>(context);
  const String json = Storage.readFile(path);
  if (json.isEmpty()) return false;
  JsonDocument doc;
  if (deserializeJson(doc, json)) return false;
  const char* const persisted = doc["openEpubPath"] | "";
  return state.openBookPath() == persisted;
}

bool cleanupStateMoveArtifacts() {
  bool cleaned = true;
  if (Storage.exists(STATE_MOVE_TEMP) && !Storage.remove(STATE_MOVE_TEMP)) {
    cleaned = false;
  }
  if (Storage.exists(STATE_MOVE_BACKUP) &&
      !Storage.remove(STATE_MOVE_BACKUP)) {
    cleaned = false;
  }
  return cleaned;
}
}  // namespace

bool CrossPointState::isRecentSleep(uint16_t idx, uint8_t checkCount) const {
  const uint8_t effectiveCount = std::min(checkCount, recentSleepFill);
  for (uint8_t i = 0; i < effectiveCount; i++) {
    const uint8_t slot = (recentSleepPos + SLEEP_RECENT_COUNT - 1 - i) % SLEEP_RECENT_COUNT;
    if (recentSleepImages[slot] == idx) return true;
  }
  return false;
}

void CrossPointState::pushRecentSleep(uint16_t idx) {
  recentSleepImages[recentSleepPos] = idx;
  recentSleepPos = (recentSleepPos + 1) % SLEEP_RECENT_COUNT;
  if (recentSleepFill < SLEEP_RECENT_COUNT) recentSleepFill++;
}

void CrossPointState::clearRecentSleepHistory() {
  std::fill_n(recentSleepImages, SLEEP_RECENT_COUNT, static_cast<uint16_t>(0));
  recentSleepPos = 0;
  recentSleepFill = 0;
}

bool CrossPointState::saveToFile() const {
  std::lock_guard<std::mutex> storeLock(storeMutex);
  std::lock_guard<std::mutex> stateLock(_mutex);
  JsonDocument doc;
  toJson(doc);
  return PersistableStoreBase::writeDocToFile(STATE_FILE_JSON, doc);
}

bool CrossPointState::activateOpenPathMigration(
    const std::string& oldPath, const std::string& newPath) {
  std::lock_guard<std::mutex> lock(_mutex);
  const bool changed = openEpubPath == oldPath;
  if (changed) openEpubPath = newPath;

  const BookMoveDurableFile::Payload payload{
      this, &writeStatePayload, &verifyStatePayload};
  Storage.mkdir("/.crosspoint");
  if (BookMoveDurableFile::replace(STATE_FILE_JSON, STATE_MOVE_TEMP,
                                   STATE_MOVE_BACKUP, payload)) {
    return true;
  }
  if (changed) openEpubPath = oldPath;
  return false;
}

bool CrossPointState::verifyPersistedOpenPathMigration(
    const std::string& oldPath, const std::string& newPath) const {
  std::string expectedPath;
  {
    std::lock_guard<std::mutex> lock(_mutex);
    expectedPath = openEpubPath;
  }
  if (!Storage.exists(STATE_FILE_JSON)) return false;
  const String json = Storage.readFile(STATE_FILE_JSON);
  if (json.isEmpty()) return false;
  JsonDocument doc;
  if (deserializeJson(doc, json)) return false;
  const char* const persistedValue = doc["openEpubPath"] | "";
  const std::string persisted = persistedValue;
  if (expectedPath == oldPath) return false;
  if (expectedPath == newPath) return persisted == newPath;
  return persisted == expectedPath;
}

bool CrossPointState::loadFromFile() {
  if (!Storage.exists(STATE_FILE_JSON)) {
    if (Storage.exists(STATE_MOVE_BACKUP)) {
      if (!Storage.rename(STATE_MOVE_BACKUP, STATE_FILE_JSON)) {
        LOG_ERR("CPS", "Failed to restore interrupted state move");
        return false;
      }
      (void)Storage.remove(STATE_MOVE_TEMP);
    } else if (Storage.exists(STATE_MOVE_TEMP) &&
               !Storage.rename(STATE_MOVE_TEMP, STATE_FILE_JSON)) {
      LOG_ERR("CPS", "Failed to promote interrupted state move");
      return false;
    }
  }

  // Try JSON first
  if (Storage.exists(STATE_FILE_JSON)) {
    String json = Storage.readFile(STATE_FILE_JSON);
    if (!json.isEmpty()) {
      std::lock_guard<std::mutex> lock(_mutex);
      if (JsonSettingsIO::loadState(*this, json.c_str())) {
        if (!cleanupStateMoveArtifacts()) {
          LOG_ERR("CPS", "Failed to clean state move artifacts");
        }
        return true;
      }
    }
    if (Storage.exists(STATE_MOVE_BACKUP)) {
      if (!Storage.remove(STATE_FILE_JSON) ||
          !Storage.rename(STATE_MOVE_BACKUP, STATE_FILE_JSON) ||
          !cleanupStateMoveArtifacts()) {
        LOG_ERR("CPS", "Failed to roll back interrupted state move");
        return false;
      }
      json = Storage.readFile(STATE_FILE_JSON);
      if (!json.isEmpty()) {
        std::lock_guard<std::mutex> lock(_mutex);
        return JsonSettingsIO::loadState(*this, json.c_str());
      }
    }
    return false;
  }

  // Fall back to binary migration
  if (Storage.exists(STATE_FILE_BIN)) {
    if (loadFromBinaryFile()) {
      if (saveToFile()) {
        Storage.rename(STATE_FILE_BIN, STATE_FILE_BAK);
        LOG_DBG("CPS", "Migrated state.bin to state.json");
        return true;
      } else {
        LOG_ERR("CPS", "Failed to save state during migration");
        return false;
      }
    }
  }

  return false;
}

void CrossPointState::toJson(JsonDocument& doc) const {
  doc["openEpubPath"] = openEpubPath;
  doc["favoriteSleepImagePath"] = favoriteSleepImagePath;
  doc["preferredSleepFolderPath"] = preferredSleepFolderPath;
  JsonArray recentArr = doc["recentSleepImages"].to<JsonArray>();
  for (int i = 0; i < SLEEP_RECENT_COUNT; i++) recentArr.add(recentSleepImages[i]);
  doc["recentSleepPos"] = recentSleepPos;
  doc["recentSleepFill"] = recentSleepFill;
  doc["readerActivityLoadCount"] = readerActivityLoadCount;
  doc["lastSleepFromReader"] = lastSleepFromReader;
  doc["pendingBookmarkSpine"] = pendingBookmarkSpine;
  doc["pendingBookmarkProgress"] = pendingBookmarkProgress;
  doc["pendingBookmarkParagraphIndex"] = pendingBookmarkParagraphIndex;
  doc["pendingClippingIndex"] = pendingClippingIndex;
  doc["showBootScreen"] = showBootScreen;
}

bool CrossPointState::fromJson(JsonVariantConst doc) {
  openEpubPath = doc["openEpubPath"] | "";
  favoriteSleepImagePath = doc["favoriteSleepImagePath"] | "";
  preferredSleepFolderPath = doc["preferredSleepFolderPath"] | "";
  std::fill_n(recentSleepImages, SLEEP_RECENT_COUNT, static_cast<uint16_t>(0));
  JsonArrayConst recentArr = doc["recentSleepImages"];
  const int actualCount =
      recentArr.isNull() ? 0 : std::min(static_cast<int>(recentArr.size()), static_cast<int>(SLEEP_RECENT_COUNT));
  for (int i = 0; i < actualCount; i++) recentSleepImages[i] = recentArr[i] | static_cast<uint16_t>(0);
  recentSleepPos = doc["recentSleepPos"] | static_cast<uint8_t>(0);
  if (recentSleepPos >= SLEEP_RECENT_COUNT) {
    recentSleepPos = actualCount > 0 ? recentSleepPos % SLEEP_RECENT_COUNT : 0;
  }
  recentSleepFill = doc["recentSleepFill"] | static_cast<uint8_t>(0);
  recentSleepFill = static_cast<uint8_t>(std::min(static_cast<int>(recentSleepFill), actualCount));
  if (recentSleepFill == 0 && !doc["lastSleepImage"].isNull()) {
    const uint8_t legacy = doc["lastSleepImage"] | static_cast<uint8_t>(UINT8_MAX);
    if (legacy != UINT8_MAX) pushRecentSleep(static_cast<uint16_t>(legacy));
  }
  readerActivityLoadCount = doc["readerActivityLoadCount"] | static_cast<uint8_t>(0);
  lastSleepFromReader = doc["lastSleepFromReader"] | false;
  pendingBookmarkSpine = doc["pendingBookmarkSpine"] | static_cast<uint16_t>(UINT16_MAX);
  pendingBookmarkProgress = doc["pendingBookmarkProgress"] | static_cast<float>(-1.0f);
  pendingBookmarkParagraphIndex = doc["pendingBookmarkParagraphIndex"] | static_cast<uint16_t>(UINT16_MAX);
  pendingClippingIndex = doc["pendingClippingIndex"] | static_cast<uint16_t>(UINT16_MAX);
  showBootScreen = doc["showBootScreen"] | true;
  return true;
}

bool CrossPointState::loadFromBinaryFile() {
  HalFile inputFile;
  if (!Storage.openFileForRead("CPS", STATE_FILE_BIN, inputFile)) {
    return false;
  }
  std::lock_guard<std::mutex> lock(_mutex);

  uint8_t version;
  serialization::readPod(inputFile, version);
  if (version > STATE_FILE_VERSION) {
    LOG_ERR("CPS", "Deserialization failed: Unknown version %u", version);
    return false;
  }

  // Legacy binary compatibility: retain the persisted member and field order.
  serialization::readString(inputFile, openEpubPath);
  if (version >= 2) {
    uint8_t legacyLastSleep = UINT8_MAX;
    serialization::readPod(inputFile, legacyLastSleep);
    if (legacyLastSleep != UINT8_MAX) {
      pushRecentSleep(static_cast<uint16_t>(legacyLastSleep));
    }
  }

  if (version >= 3) {
    serialization::readPod(inputFile, readerActivityLoadCount);
  }

  if (version >= 4) {
    serialization::readPod(inputFile, lastSleepFromReader);
  } else {
    lastSleepFromReader = false;
  }

  if (version >= 5) {
    serialization::readPod(inputFile, pendingBookmarkSpine);
    serialization::readPod(inputFile, pendingBookmarkProgress);
    pendingBookmarkParagraphIndex = UINT16_MAX;
    pendingClippingIndex = UINT16_MAX;
  } else {
    pendingBookmarkSpine = UINT16_MAX;
    pendingBookmarkProgress = -1.0f;
    pendingBookmarkParagraphIndex = UINT16_MAX;
    pendingClippingIndex = UINT16_MAX;
  }

  return true;
}
