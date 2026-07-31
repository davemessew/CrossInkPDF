#include "CrossPointWebServer.h"

#include <ArduinoJson.h>
#ifdef SIMULATOR
#include <ArduinoJsonStringCompat.h>
#endif
#include <Epub.h>
#include <FsHelpers.h>
#include <HalGPIO.h>
#include <HalStorage.h>
#include <Logging.h>
#include <PdfDeleteJournal.h>
#include <WiFi.h>
#include <esp_efuse.h>
#include <esp_efuse_table.h>
#include <esp_task_wdt.h>

#include <algorithm>
#include <cctype>
#include <iterator>
#include <limits>

#include "AppVersion.h"
#include "CrossPointSettings.h"
#include "FontInstaller.h"
#include "OpdsServerStore.h"
#include "SdCardFontSystem.h"
#include "SettingsList.h"
#include "WebDAVHandler.h"
#include "WifiCredentialStore.h"
#include "html/FilesPageHtml.generated.h"
#include "html/FontsPageHtml.generated.h"
#include "html/HomePageHtml.generated.h"
#include "html/LogoPng.generated.h"
#include "html/SettingsPageHtml.generated.h"
#include "html/StyleCss.generated.h"
#include "html/js/jszip_minJs.generated.h"
#include "mbedtls/sha256.h"
#include "util/BookCacheUtils.h"
#include "util/BookMoveUtils.h"
#include "util/PdfDeleteUtils.h"
#include "util/StringUtils.h"

namespace {
static_assert(sizeof(mbedtls_sha256_context) <= BookUpload::kAtomicUploadSha256ContextCapacity,
              "mbedTLS SHA-256 context exceeded the fixed upload workspace");

// Folders/files to hide from the web interface file browser.
// Dot-prefixed items are hidden unless showHiddenFiles is enabled.
constexpr const char* HIDDEN_ITEMS[] = {"System Volume Information", "XTCache"};
constexpr uint16_t UDP_PORTS[] = {54982, 48123, 39001, 44044, 59678};
constexpr uint16_t LOCAL_UDP_PORT = 8134;

// Static pointer for WebSocket callback (WebSocketsServer requires C-style callback)
CrossPointWebServer* wsInstance = nullptr;

uint8_t enumDisplayIndexForRawValue(const SettingInfo& setting, uint8_t rawValue) {
  if (setting.enumRawValues.empty()) {
    return rawValue;
  }

  auto it = std::find(setting.enumRawValues.begin(), setting.enumRawValues.end(), rawValue);
  if (it == setting.enumRawValues.end()) {
    return 0;
  }
  return static_cast<uint8_t>(std::distance(setting.enumRawValues.begin(), it));
}

uint8_t enumRawValueForDisplayIndex(const SettingInfo& setting, uint8_t displayIndex) {
  if (setting.enumRawValues.empty()) {
    return displayIndex;
  }
  if (displayIndex >= setting.enumRawValues.size()) {
    return setting.enumRawValues.front();
  }
  return setting.enumRawValues[displayIndex];
}

// WebSocket upload metadata. The file handle and atomic transaction live in
// CrossPointWebServer::upload so HTTP and WS cannot write concurrently.
String wsUploadFileName;
String wsUploadPath;
size_t wsUploadSize = 0;
size_t wsUploadReceived = 0;
unsigned long wsUploadStartTime = 0;
bool wsUploadInProgress = false;
uint8_t wsUploadClientNum = 255;  // 255 = no active upload client
size_t wsLastProgressSent = 0;
String wsLastCompleteName;
size_t wsLastCompleteSize = 0;
unsigned long wsLastCompleteAt = 0;

bool atomicUploadExists(void*, const char* const path) { return Storage.exists(path); }

bool atomicUploadOpenWrite(void* const context, const char* const path) {
  return context != nullptr && Storage.openFileForWrite("WEB", path, *static_cast<HalFile*>(context));
}

size_t atomicUploadWrite(void* const context, const uint8_t* const data, const size_t length) {
  if (context == nullptr) return 0;
  esp_task_wdt_reset();
  const size_t written = static_cast<HalFile*>(context)->write(data, length);
  esp_task_wdt_reset();
  return written;
}

bool atomicUploadFlush(void* const context) {
  if (context == nullptr) return false;
  // HalFile::flush() has no status result. The following sync() and close()
  // remain the production durability/error gates; the pure fake can still
  // witness a reported flush failure.
  static_cast<HalFile*>(context)->flush();
  return true;
}

bool atomicUploadSync(void* const context) {
  if (context == nullptr) return false;
  esp_task_wdt_reset();
  const bool synced = static_cast<HalFile*>(context)->sync();
  esp_task_wdt_reset();
  return synced;
}

bool atomicUploadClose(void* const context) { return context != nullptr && static_cast<HalFile*>(context)->close(); }

bool atomicUploadOpenRead(void* const context, const char* const path) {
  return context != nullptr && Storage.openFileForRead("WEB", path, *static_cast<HalFile*>(context));
}

int atomicUploadRead(void* const context, uint8_t* const destination, const size_t capacity) {
  if (context == nullptr) return -1;
  esp_task_wdt_reset();
  const int read = static_cast<HalFile*>(context)->read(destination, capacity);
  esp_task_wdt_reset();
  return read;
}

bool atomicUploadRemove(void*, const char* const path) { return Storage.remove(path); }

bool atomicUploadRename(void*, const char* const source, const char* const destination) {
  return Storage.rename(source, destination);
}

bool atomicUploadSha256Start(void* const context) {
  if (context == nullptr) return false;
  auto* const sha256 = static_cast<mbedtls_sha256_context*>(context);
  mbedtls_sha256_init(sha256);
  if (mbedtls_sha256_starts(sha256, 0) == 0) return true;
  mbedtls_sha256_free(sha256);
  return false;
}

bool atomicUploadSha256Update(void* const context, const uint8_t* const data, const size_t length) {
  return context != nullptr && mbedtls_sha256_update(static_cast<mbedtls_sha256_context*>(context), data, length) == 0;
}

bool atomicUploadSha256Finish(void* const context, uint8_t digest[BookUpload::kAtomicUploadSha256Size]) {
  if (context == nullptr || digest == nullptr) return false;
  auto* const sha256 = static_cast<mbedtls_sha256_context*>(context);
  const bool finished = mbedtls_sha256_finish(sha256, digest) == 0;
  mbedtls_sha256_free(sha256);
  return finished;
}

void atomicUploadSha256Abort(void* const context) {
  if (context != nullptr) mbedtls_sha256_free(static_cast<mbedtls_sha256_context*>(context));
}

BookUpload::AtomicUploadIo atomicUploadIo(HalFile& file) {
  BookUpload::AtomicUploadIo io{};
  io.context = &file;
  io.exists = &atomicUploadExists;
  io.openWrite = &atomicUploadOpenWrite;
  io.write = &atomicUploadWrite;
  io.flush = &atomicUploadFlush;
  io.sync = &atomicUploadSync;
  io.close = &atomicUploadClose;
  io.openRead = &atomicUploadOpenRead;
  io.read = &atomicUploadRead;
  io.remove = &atomicUploadRemove;
  io.rename = &atomicUploadRename;
  io.sha256.contextSize = sizeof(mbedtls_sha256_context);
  io.sha256.start = &atomicUploadSha256Start;
  io.sha256.update = &atomicUploadSha256Update;
  io.sha256.finish = &atomicUploadSha256Finish;
  io.sha256.abort = &atomicUploadSha256Abort;
  return io;
}

bool invalidateUploadedBookCache(void*, const char* const targetPath) {
  return clearBookCachePreservingUserState(targetPath);
}

BookUpload::AtomicUploadCommitHook uploadCommitHook() { return {nullptr, &invalidateUploadedBookCache}; }

const char* atomicUploadStatusMessage(const BookUpload::AtomicUploadStatus status) {
  switch (status) {
    case BookUpload::AtomicUploadStatus::Ok:
      return "";
    case BookUpload::AtomicUploadStatus::Busy:
      return "Upload already in progress";
    case BookUpload::AtomicUploadStatus::PathTooLong:
      return "Upload path is too long";
    case BookUpload::AtomicUploadStatus::RecoveryFailed:
      return "Failed to recover an interrupted upload";
    case BookUpload::AtomicUploadStatus::OpenWriteFailed:
      return "Failed to create temporary file on SD card";
    case BookUpload::AtomicUploadStatus::SizeMismatch:
      return "Upload size mismatch";
    case BookUpload::AtomicUploadStatus::WriteFailed:
      return "Failed to write to SD card - disk may be full";
    case BookUpload::AtomicUploadStatus::FlushFailed:
      return "Failed to flush uploaded data";
    case BookUpload::AtomicUploadStatus::SyncFailed:
      return "Failed to sync uploaded data";
    case BookUpload::AtomicUploadStatus::CloseFailed:
      return "Failed to close uploaded file";
    case BookUpload::AtomicUploadStatus::OpenReadFailed:
      return "Failed to verify uploaded file";
    case BookUpload::AtomicUploadStatus::ReadFailed:
      return "Failed while verifying uploaded file";
    case BookUpload::AtomicUploadStatus::DigestFailed:
      return "Failed to calculate uploaded file checksum";
    case BookUpload::AtomicUploadStatus::VerificationFailed:
      return "Uploaded file verification failed";
    case BookUpload::AtomicUploadStatus::BackupFailed:
      return "Failed to preserve existing file";
    case BookUpload::AtomicUploadStatus::MarkerFailed:
      return "Failed to persist upload commit marker";
    case BookUpload::AtomicUploadStatus::PromotionFailed:
      return "Failed to activate uploaded file";
    case BookUpload::AtomicUploadStatus::CommitHookFailed:
      return "Failed to invalidate derived book cache";
    case BookUpload::AtomicUploadStatus::CleanupFailed:
      return "Upload cleanup failed; retry is safe";
    case BookUpload::AtomicUploadStatus::NotActive:
      return "No upload in progress";
    case BookUpload::AtomicUploadStatus::InvalidArgument:
    default:
      return "Invalid upload state";
  }
}

String normalizeWebPath(const String& inputPath) {
  if (inputPath.isEmpty() || inputPath == "/") {
    return "/";
  }
  std::string normalized = FsHelpers::normalisePath(inputPath.c_str());
  String result = normalized.c_str();
  if (result.isEmpty()) {
    return "/";
  }
  if (!result.startsWith("/")) {
    result = "/" + result;
  }
  if (result.length() > 1 && result.endsWith("/")) {
    result = result.substring(0, result.length() - 1);
  }
  return result;
}

bool parseUploadSizeToken(const String& token, size_t& size) {
  if (token.isEmpty()) return false;
  size_t index = 0;
  if (token[0] == '+') {
    if (token.length() == 1) return false;
    index = 1;
  }
  size_t value = 0;
  for (; index < token.length(); ++index) {
    const char ch = token[index];
    if (ch < '0' || ch > '9') return false;
    const size_t digit = static_cast<size_t>(ch - '0');
    if (value > (std::numeric_limits<size_t>::max() - digit) / 10) return false;
    value = value * 10 + digit;
  }
  size = value;
  return true;
}

bool isProtectedPath(const String& path) {
  // Check every segment of the path, not just the last one.
  // This prevents access to e.g. /.hidden/somefile or /System Volume Information/foo
  int start = 0;
  while (start < (int)path.length()) {
    if (path.charAt(start) == '/') {
      start++;
      continue;
    }
    int end = path.indexOf('/', start);
    if (end == -1) end = path.length();

    String segment = path.substring(start, end);

    if (!SETTINGS.showHiddenFiles && segment.startsWith(".")) return true;

    for (const auto* item : HIDDEN_ITEMS) {
      if (segment.equals(item)) return true;
    }

    start = end + 1;
  }

  return false;
}
}  // namespace

// File listing page template - now using generated headers:
// - HomePageHtml (from html/HomePage.html)
// - FilesPageHeaderHtml (from html/FilesPageHeader.html)
// - FilesPageFooterHtml (from html/FilesPageFooter.html)
CrossPointWebServer::CrossPointWebServer() {}

CrossPointWebServer::~CrossPointWebServer() { stop(); }

void CrossPointWebServer::begin() {
  if (running) {
    LOG_DBG("WEB", "Web server already running");
    return;
  }

  // Check if we have a valid network connection (either STA connected or AP mode)
  const wifi_mode_t wifiMode = WiFi.getMode();
  const bool isStaConnected = (wifiMode & WIFI_MODE_STA) && (WiFi.status() == WL_CONNECTED);
  const bool isInApMode = (wifiMode & WIFI_MODE_AP) && (WiFi.softAPgetStationNum() >= 0);  // AP is running

  if (!isStaConnected && !isInApMode) {
    LOG_DBG("WEB", "Cannot start webserver - no valid network (mode=%d, status=%d)", wifiMode, WiFi.status());
    return;
  }

  // Store AP mode flag for later use (e.g., in handleStatus)
  apMode = isInApMode;

  LOG_DBG("WEB", "[MEM] Free heap before begin: %d bytes", ESP.getFreeHeap());
  LOG_DBG("WEB", "Network mode: %s", apMode ? "AP" : "STA");

  LOG_DBG("WEB", "Creating web server on port %d...", port);
  server.reset(new WebServer(port));

  // Disable WiFi sleep to improve responsiveness and prevent 'unreachable' errors.
  // This is critical for reliable web server operation on ESP32.
  WiFi.setSleep(false);
  // Default varies by ESP32 core version. The activity's loss-recovery loop
  // relies on driver retries during transient disconnects.
  WiFi.setAutoReconnect(true);

  // Note: WebServer class doesn't have setNoDelay() in the standard ESP32 library.
  // We rely on disabling WiFi sleep for responsiveness.

  LOG_DBG("WEB", "[MEM] Free heap after WebServer allocation: %d bytes", ESP.getFreeHeap());

  if (!server) {
    LOG_ERR("WEB", "Failed to create WebServer!");
    return;
  }

  // Setup routes
  LOG_DBG("WEB", "Setting up routes...");
  server->on("/", HTTP_GET, [this] { handleRoot(); });
  server->on("/files", HTTP_GET, [this] { handleFileList(); });
  server->on("/js/jszip.min.js", HTTP_GET, [this] { handleJszip(); });
  server->on("/style.css", HTTP_GET, [this] { handleStyleCss(); });
  server->on("/logo.png", HTTP_GET, [this] { handleLogo(); });

  server->on("/api/status", HTTP_GET, [this] { handleStatus(); });
  server->on("/api/files", HTTP_GET, [this] { handleFileListData(); });
  server->on("/download", HTTP_GET, [this] { handleDownload(); });

  // Upload endpoint with special handling for multipart form data
  server->on("/upload", HTTP_POST, [this] { handleUploadPost(upload); }, [this] { handleUpload(upload); });

  // Create folder endpoint
  server->on("/mkdir", HTTP_POST, [this] { handleCreateFolder(); });

  // Rename file endpoint
  server->on("/rename", HTTP_POST, [this] { handleRename(); });

  // Move file endpoint
  server->on("/move", HTTP_POST, [this] { handleMove(); });

  // Delete file/folder endpoint
  server->on("/delete", HTTP_POST, [this] { handleDelete(); });

  // Settings endpoints
  server->on("/settings", HTTP_GET, [this] { handleSettingsPage(); });
  server->on("/api/settings", HTTP_GET, [this] { handleGetSettings(); });
  server->on("/api/settings", HTTP_POST, [this] { handlePostSettings(); });

  // Font management endpoints
  server->on("/fonts", HTTP_GET, [this] { handleFontsPage(); });
  server->on("/api/fonts", HTTP_GET, [this] { handleFontList(); });
  server->on("/api/fonts/upload", HTTP_POST, [this] { handleFontUpload(); }, [this] { handleFontUploadData(); });
  server->on("/api/fonts/delete", HTTP_POST, [this] { handleFontDelete(); });

  // OPDS server endpoints
  server->on("/api/opds", HTTP_GET, [this] { handleGetOpdsServers(); });
  server->on("/api/opds", HTTP_POST, [this] { handlePostOpdsServer(); });
  server->on("/api/opds/delete", HTTP_POST, [this] { handleDeleteOpdsServer(); });

  // Wi-Fi credential endpoints
  server->on("/api/wifi", HTTP_GET, [this] { handleGetWifiNetworks(); });
  server->on("/api/wifi", HTTP_POST, [this] { handlePostWifiNetwork(); });
  server->on("/api/wifi/delete", HTTP_POST, [this] { handleDeleteWifiNetwork(); });

  server->onNotFound([this] { handleNotFound(); });
  LOG_DBG("WEB", "[MEM] Free heap after route setup: %d bytes", ESP.getFreeHeap());

  // Collect WebDAV headers and register handler
  const char* davHeaders[] = {"Depth", "Destination", "Overwrite", "If", "Lock-Token", "Timeout"};
  server->collectHeaders(davHeaders, 6);
  server->addHandler(new WebDAVHandler());  // Note: WebDAVHandler will be deleted by WebServer when server is stopped
  LOG_DBG("WEB", "WebDAV handler initialized");

  server->begin();

  // Start WebSocket server for fast binary uploads
  LOG_DBG("WEB", "Starting WebSocket server on port %d...", wsPort);
  wsServer.reset(new WebSocketsServer(wsPort));
  wsInstance = const_cast<CrossPointWebServer*>(this);
  wsServer->begin();
  wsServer->onEvent(wsEventCallback);
  LOG_DBG("WEB", "WebSocket server started");

  udpActive = udp.begin(LOCAL_UDP_PORT);
  LOG_DBG("WEB", "Discovery UDP %s on port %d", udpActive ? "enabled" : "failed", LOCAL_UDP_PORT);

  running = true;

  LOG_DBG("WEB", "Web server started on port %d", port);
  // Show the correct IP based on network mode
  const String ipAddr = apMode ? WiFi.softAPIP().toString() : WiFi.localIP().toString();
  LOG_DBG("WEB", "Access at http://%s/", ipAddr.c_str());
  LOG_DBG("WEB", "WebSocket at ws://%s:%d/", ipAddr.c_str(), wsPort);
  LOG_DBG("WEB", "[MEM] Free heap after server.begin(): %d bytes", ESP.getFreeHeap());
}

void CrossPointWebServer::abortWsUpload(const char* tag) {
  const BookUpload::AtomicUploadIo io = atomicUploadIo(upload.file);
  const BookUpload::AtomicUploadStatus status = BookUpload::abort(upload.transaction, io);
  if (status != BookUpload::AtomicUploadStatus::Ok) {
    LOG_ERR(tag, "Failed to clean interrupted WS upload: %s", atomicUploadStatusMessage(status));
  }
  upload.owner = UploadState::Owner::None;
  wsUploadInProgress = false;
  wsUploadClientNum = 255;
  wsLastProgressSent = 0;
}

void CrossPointWebServer::stop() {
  if (!running || !server) {
    LOG_DBG("WEB", "stop() called but already stopped (running=%d, server=%p)", running, server.get());
    return;
  }

  LOG_DBG("WEB", "STOP INITIATED - setting running=false first");
  running = false;  // Set this FIRST to prevent handleClient from using server

  LOG_DBG("WEB", "[MEM] Free heap before stop: %d bytes", ESP.getFreeHeap());

  // Close any in-progress upload. The atomic helper removes only its hidden
  // sibling temp and leaves the canonical book untouched.
  if (BookUpload::isActive(upload.transaction)) {
    const BookUpload::AtomicUploadIo io = atomicUploadIo(upload.file);
    const BookUpload::AtomicUploadStatus status = BookUpload::abort(upload.transaction, io);
    if (status != BookUpload::AtomicUploadStatus::Ok) {
      LOG_ERR("WEB", "Failed to clean interrupted upload during stop: %s", atomicUploadStatusMessage(status));
    }
    upload.owner = UploadState::Owner::None;
    wsUploadInProgress = false;
    wsUploadClientNum = 255;
  }

  // Stop WebSocket server
  if (wsServer) {
    LOG_DBG("WEB", "Stopping WebSocket server...");
    wsServer->close();
    wsServer.reset();
    wsInstance = nullptr;
    LOG_DBG("WEB", "WebSocket server stopped");
  }

  if (udpActive) {
    udp.stop();
    udpActive = false;
  }

  // Brief delay to allow any in-flight handleClient() calls to complete
  delay(20);

  server->stop();
  LOG_DBG("WEB", "[MEM] Free heap after server->stop(): %d bytes", ESP.getFreeHeap());

  // Brief delay before deletion
  delay(10);

  server.reset();
  LOG_DBG("WEB", "Web server stopped and deleted");
  LOG_DBG("WEB", "[MEM] Free heap after delete server: %d bytes", ESP.getFreeHeap());

  // Note: Static upload variables (uploadFileName, uploadPath, uploadError) are declared
  // later in the file and will be cleared when they go out of scope or on next upload
  LOG_DBG("WEB", "[MEM] Free heap final: %d bytes", ESP.getFreeHeap());
}

void CrossPointWebServer::handleClient() {
  static unsigned long lastDebugPrint = 0;

  // Check running flag FIRST before accessing server
  if (!running) {
    return;
  }

  // Double-check server pointer is valid
  if (!server) {
    LOG_DBG("WEB", "WARNING: handleClient called with null server!");
    return;
  }

  // Print debug every 10 seconds to confirm handleClient is being called
  if (millis() - lastDebugPrint > 10000) {
    LOG_DBG("WEB", "handleClient active, server running on port %d", port);
    lastDebugPrint = millis();
  }

  server->handleClient();

  // Handle WebSocket events
  if (wsServer) {
    wsServer->loop();
  }

  // Respond to discovery broadcasts
  if (udpActive) {
    int packetSize = udp.parsePacket();
    if (packetSize > 0) {
      char buffer[16];
      int len = udp.read(buffer, sizeof(buffer) - 1);
      if (len > 0) {
        buffer[len] = '\0';
        if (strcmp(buffer, "hello") == 0) {
          String hostname = WiFi.getHostname();
          if (hostname.isEmpty()) {
            hostname = "crosspoint";
          }
          String message = "crosspoint (on " + hostname + ");" + String(wsPort);
          udp.beginPacket(udp.remoteIP(), udp.remotePort());
          udp.write(reinterpret_cast<const uint8_t*>(message.c_str()), message.length());
          udp.endPacket();
        }
      }
    }
  }
}

CrossPointWebServer::WsUploadStatus CrossPointWebServer::getWsUploadStatus() const {
  WsUploadStatus status;
  status.inProgress = wsUploadInProgress;
  status.received = wsUploadReceived;
  status.total = wsUploadSize;
  status.filename = wsUploadFileName.c_str();
  status.lastCompleteName = wsLastCompleteName.c_str();
  status.lastCompleteSize = wsLastCompleteSize;
  status.lastCompleteAt = wsLastCompleteAt;
  return status;
}

static void sendHtmlContent(WebServer* server, const char* data, size_t len) {
  server->sendHeader("Content-Encoding", "gzip");
  server->send_P(200, "text/html", data, len);
}

void CrossPointWebServer::handleRoot() const {
  sendHtmlContent(server.get(), HomePageHtml, sizeof(HomePageHtml));
  LOG_DBG("WEB", "Served root page");
}

void CrossPointWebServer::handleJszip() const {
  server->sendHeader("Content-Encoding", "gzip");
  server->send_P(200, "application/javascript", jszip_minJs, jszip_minJsCompressedSize);
  LOG_DBG("WEB", "Served jszip.min.js");
}

// Shared stylesheet and logo are referenced with a content-hashed ?v= query,
// so they can be cached aggressively: a new build changes the URL.
void CrossPointWebServer::handleStyleCss() const {
  server->sendHeader("Content-Encoding", "gzip");
  server->sendHeader("Cache-Control", "public, max-age=31536000, immutable");
  server->send_P(200, "text/css", StyleCss, StyleCssCompressedSize);
  LOG_DBG("WEB", "Served style.css");
}

void CrossPointWebServer::handleLogo() const {
  // Raw PNG (already compressed); no Content-Encoding.
  server->sendHeader("Cache-Control", "public, max-age=31536000, immutable");
  server->send_P(200, "image/png", LogoPng, LogoPngSize);
  LOG_DBG("WEB", "Served logo.png");
}

void CrossPointWebServer::handleNotFound() const {
  String message = "404 Not Found\n\n";
  message += "URI: " + server->uri() + "\n";
  server->send(404, "text/plain", message);
}

void CrossPointWebServer::handleStatus() const {
  // Get correct IP based on AP vs STA mode
  const String ipAddr = apMode ? WiFi.softAPIP().toString() : WiFi.localIP().toString();

  JsonDocument doc;
  doc["version"] = CROSSINK_VERSION;
  doc["ip"] = ipAddr;
  doc["mode"] = apMode ? "AP" : "STA";
  doc["rssi"] = apMode ? 0 : WiFi.RSSI();
  doc["freeHeap"] = ESP.getFreeHeap();
  doc["uptime"] = millis() / 1000;
  doc["device"] = gpio.deviceIsX3() ? "X3" : "X4";

  char snBuf[33] = {0};
  bool valid = false;
  if (esp_efuse_read_field_blob(ESP_EFUSE_USER_DATA, snBuf, 256) == ESP_OK) {
    valid = snBuf[0] != '\0' && snBuf[0] != (char)0xFF;
    for (int i = 0; i < 32 && snBuf[i] != '\0'; i++) {
      if (!std::isprint(static_cast<unsigned char>(snBuf[i]))) {
        valid = false;
        break;
      }
    }
  }

  if (valid) {
    doc["serial"] = snBuf;
  } else {
    doc["serial"] = "Not found";
  }

  String response;
  serializeJson(doc, response);
  server->send(200, "application/json", response);
}

void CrossPointWebServer::scanFiles(const char* path, const std::function<void(FileInfo)>& callback) const {
  HalFile root = Storage.open(path);
  if (!root) {
    LOG_DBG("WEB", "Failed to open directory: %s", path);
    return;
  }

  if (!root.isDirectory()) {
    LOG_DBG("WEB", "Not a directory: %s", path);
    root.close();
    return;
  }

  LOG_DBG("WEB", "Scanning files in: %s", path);

  HalFile file = root.openNextFile();
  char name[500];
  while (file) {
    file.getName(name, sizeof(name));
    auto fileName = String(name);

    // Skip hidden items (starting with ".")
    bool shouldHide = !SETTINGS.showHiddenFiles && fileName.startsWith(".");

    // Check against explicitly hidden items list
    if (!shouldHide) {
      for (const auto* item : HIDDEN_ITEMS) {
        if (fileName.equals(item)) {
          shouldHide = true;
          break;
        }
      }
    }

    if (!shouldHide) {
      FileInfo info;
      info.name = fileName;
      info.isDirectory = file.isDirectory();

      if (info.isDirectory) {
        info.size = 0;
        info.isEpub = false;
      } else {
        info.size = file.size();
        info.isEpub = isEpubFile(info.name);
      }

      callback(info);
    }

    file.close();
    yield();               // Yield to allow WiFi and other tasks to process during long scans
    esp_task_wdt_reset();  // Reset watchdog to prevent timeout on large directories
    file = root.openNextFile();
  }
  root.close();
}

bool CrossPointWebServer::isEpubFile(const String& filename) const { return FsHelpers::hasEpubExtension(filename); }

void CrossPointWebServer::handleFileList() const {
  sendHtmlContent(server.get(), FilesPageHtml, sizeof(FilesPageHtml));
}

void CrossPointWebServer::handleFileListData() const {
  // Get current path from query string (default to root)
  String currentPath = "/";
  if (server->hasArg("path")) {
    currentPath = normalizeWebPath(server->arg("path"));
  }

  if (isProtectedPath(currentPath)) {
    server->send(403, "application/json", "[]");
    return;
  }

  server->setContentLength(CONTENT_LENGTH_UNKNOWN);
  server->send(200, "application/json", "");
  server->sendContent("[");
  char output[512];
  constexpr size_t outputSize = sizeof(output);
  bool seenFirst = false;
  JsonDocument doc;

  scanFiles(currentPath.c_str(), [this, &output, &doc, seenFirst](const FileInfo& info) mutable {
    doc.clear();
    doc["name"] = info.name;
    doc["size"] = info.size;
    doc["isDirectory"] = info.isDirectory;
    doc["isEpub"] = info.isEpub;

    const size_t written = serializeJson(doc, output, outputSize);
    if (written >= outputSize) {
      // JSON output truncated; skip this entry to avoid sending malformed JSON
      LOG_DBG("WEB", "Skipping file entry with oversized JSON for name: %s", info.name.c_str());
      return;
    }

    if (seenFirst) {
      server->sendContent(",");
    } else {
      seenFirst = true;
    }
    server->sendContent(output);
  });
  server->sendContent("]");
  // End of streamed response, empty chunk to signal client
  server->sendContent("");
  LOG_DBG("WEB", "Served file listing page for path: %s", currentPath.c_str());
}

void CrossPointWebServer::handleDownload() const {
  if (!server->hasArg("path")) {
    server->send(400, "text/plain", "Missing path");
    return;
  }

  String itemPath = normalizeWebPath(server->arg("path"));
  if (itemPath.isEmpty() || itemPath == "/") {
    server->send(400, "text/plain", "Invalid path");
    return;
  }

  if (isProtectedPath(itemPath)) {
    server->send(403, "text/plain", "Access denied to protected path");
    return;
  }

  if (!Storage.exists(itemPath.c_str())) {
    server->send(404, "text/plain", "Item not found");
    return;
  }

  HalFile file = Storage.open(itemPath.c_str());
  if (!file) {
    server->send(500, "text/plain", "Failed to open file");
    return;
  }
  if (file.isDirectory()) {
    file.close();
    server->send(400, "text/plain", "Path is a directory");
    return;
  }

  String contentType = "application/octet-stream";
  if (isEpubFile(itemPath)) {
    contentType = "application/epub+zip";
  }

  char nameBuf[128] = {0};
  String filename = "download";
  if (file.getName(nameBuf, sizeof(nameBuf))) {
    filename = nameBuf;
  }

  server->setContentLength(file.size());
  server->sendHeader("Content-Disposition", "attachment; filename=\"" + filename + "\"");
  server->send(200, contentType.c_str(), "");

  NetworkClient client = server->client();
  const size_t chunkSize = 4096;
  uint8_t buffer[chunkSize];

  bool downloadOk = true;
  while (downloadOk && file.available()) {
    int result = file.read(buffer, chunkSize);
    if (result <= 0) break;
    size_t bytesRead = static_cast<size_t>(result);
    size_t totalWritten = 0;
    while (totalWritten < bytesRead) {
      esp_task_wdt_reset();
      size_t wrote = client.write(buffer + totalWritten, bytesRead - totalWritten);
      if (wrote == 0) {
        downloadOk = false;
        break;
      }
      totalWritten += wrote;
    }
  }
#ifndef SIMULATOR
  client.clear();
#endif
  file.close();
}

// Diagnostic counters for upload performance analysis
static unsigned long uploadStartTime = 0;
static unsigned long totalWriteTime = 0;
static size_t writeCount = 0;

static BookUpload::AtomicUploadStatus flushUploadBuffer(CrossPointWebServer::UploadState& state) {
  if (state.bufferPos == 0) return BookUpload::AtomicUploadStatus::Ok;
  const unsigned long writeStart = millis();
  const BookUpload::AtomicUploadIo io = atomicUploadIo(state.file);
  const BookUpload::AtomicUploadStatus status =
      BookUpload::write(state.transaction, io, state.buffer.data(), state.bufferPos);
  totalWriteTime += millis() - writeStart;
  writeCount++;
  state.bufferPos = 0;
  return status;
}

void CrossPointWebServer::handleUpload(UploadState& state) const {
  static size_t lastLoggedSize = 0;

  // Reset watchdog at start of every upload callback - HTTP parsing can be slow
  esp_task_wdt_reset();

  // Safety check: ensure server is still valid
  if (!running || !server) {
    LOG_DBG("WEB", "[UPLOAD] ERROR: handleUpload called but server not running!");
    return;
  }

  const HTTPUpload& upload = server->upload();

  if (upload.status == UPLOAD_FILE_START) {
    BookUpload::UploadTransportResponse response{state.success, !state.error.isEmpty()};
    const BookUpload::AtomicUploadStatus admission =
        BookUpload::admitTransportStart(BookUpload::isActive(state.transaction), response);
    state.success = response.success;
    state.fileName = "";
    state.error = "";
    if (admission != BookUpload::AtomicUploadStatus::Ok) {
      state.error = atomicUploadStatusMessage(admission);
      LOG_DBG("WEB", "[UPLOAD] START rejected: transaction already active");
      return;
    }

    // Reset watchdog - this is the critical 1% crash point
    esp_task_wdt_reset();

    state.fileName = StringUtils::sanitizeFilename(upload.filename.c_str()).c_str();
    state.size = 0;
    uploadStartTime = millis();
    lastLoggedSize = 0;
    state.bufferPos = 0;
    totalWriteTime = 0;
    writeCount = 0;

    // Get upload path from query parameter (defaults to root if not specified)
    // Note: We use query parameter instead of form data because multipart form
    // fields aren't available until after file upload completes
    if (server->hasArg("path")) {
      state.path = normalizeWebPath(server->arg("path"));
    } else {
      state.path = "/";
    }

    LOG_DBG("WEB", "[UPLOAD] START: %s to path: %s", state.fileName.c_str(), state.path.c_str());
    LOG_DBG("WEB", "[UPLOAD] Free heap: %d bytes", ESP.getFreeHeap());

    // Create file path
    String filePath = state.path;
    if (!filePath.endsWith("/")) filePath += "/";
    filePath += state.fileName;

    if (isProtectedPath(filePath)) {
      state.error = "Access denied to protected path";
      LOG_DBG("WEB", "[UPLOAD] FAILED: Access denied to protected path: %s", filePath.c_str());
      return;
    }

    const BookUpload::AtomicUploadIo io = atomicUploadIo(state.file);
    const BookUpload::AtomicUploadStatus status =
        BookUpload::begin(state.transaction, io, filePath.c_str(), BookUpload::kUnknownUploadSize);
    if (status != BookUpload::AtomicUploadStatus::Ok) {
      state.error = atomicUploadStatusMessage(status);
      LOG_ERR("WEB", "[UPLOAD] START failed for %s: %s", filePath.c_str(), state.error.c_str());
      return;
    }

    state.owner = UploadState::Owner::Http;
    LOG_DBG("WEB", "[UPLOAD] Hidden sibling temp opened for: %s", filePath.c_str());
  } else if (upload.status == UPLOAD_FILE_WRITE) {
    if (state.owner == UploadState::Owner::Http && BookUpload::isActive(state.transaction) && state.error.isEmpty()) {
      // Buffer incoming data and flush when buffer is full
      // This reduces SD card write operations and improves throughput
      const uint8_t* data = upload.buf;
      size_t remaining = upload.currentSize;

      while (remaining > 0) {
        const size_t space = UploadState::UPLOAD_BUFFER_SIZE - state.bufferPos;
        const size_t toCopy = (remaining < space) ? remaining : space;

        memcpy(state.buffer.data() + state.bufferPos, data, toCopy);
        state.bufferPos += toCopy;
        data += toCopy;
        remaining -= toCopy;

        // Flush buffer when full
        if (state.bufferPos >= UploadState::UPLOAD_BUFFER_SIZE) {
          const BookUpload::AtomicUploadStatus status = flushUploadBuffer(state);
          if (status != BookUpload::AtomicUploadStatus::Ok) {
            state.error = atomicUploadStatusMessage(status);
            return;
          }
        }
      }

      if (upload.currentSize > std::numeric_limits<size_t>::max() - state.size) {
        const BookUpload::AtomicUploadIo io = atomicUploadIo(state.file);
        BookUpload::abort(state.transaction, io);
        state.owner = UploadState::Owner::None;
        state.error = "Upload size overflow";
        state.bufferPos = 0;
        return;
      }
      state.size += upload.currentSize;

      // Log progress every 100KB
      if (state.size - lastLoggedSize >= 102400) {
        const unsigned long elapsed = millis() - uploadStartTime;
        const float kbps = (elapsed > 0) ? (state.size / 1024.0) / (elapsed / 1000.0) : 0;
        LOG_DBG("WEB", "[UPLOAD] %d bytes (%.1f KB), %.1f KB/s, %d writes", state.size, state.size / 1024.0, kbps,
                writeCount);
        lastLoggedSize = state.size;
      }
    }
  } else if (upload.status == UPLOAD_FILE_END) {
    if (state.owner == UploadState::Owner::Http) {
      // Flush any remaining buffered data
      if (state.error.isEmpty()) {
        const BookUpload::AtomicUploadStatus writeStatus = flushUploadBuffer(state);
        if (writeStatus != BookUpload::AtomicUploadStatus::Ok) {
          state.error = atomicUploadStatusMessage(writeStatus);
        }
      }

      if (state.error.isEmpty()) {
        const BookUpload::AtomicUploadIo io = atomicUploadIo(state.file);
        const BookUpload::AtomicUploadStatus finishStatus = BookUpload::finish(
            state.transaction, io, state.size, state.buffer.data(), state.buffer.size(), uploadCommitHook());
        if (finishStatus == BookUpload::AtomicUploadStatus::Ok) {
          state.success = true;
        } else {
          state.error = atomicUploadStatusMessage(finishStatus);
        }
      } else {
        const BookUpload::AtomicUploadIo io = atomicUploadIo(state.file);
        BookUpload::abort(state.transaction, io);
      }
      state.owner = UploadState::Owner::None;

      if (state.success) {
        const unsigned long elapsed = millis() - uploadStartTime;
        const float avgKbps = (elapsed > 0) ? (state.size / 1024.0) / (elapsed / 1000.0) : 0;
        const float writePercent = (elapsed > 0) ? (totalWriteTime * 100.0 / elapsed) : 0;
        LOG_DBG("WEB", "[UPLOAD] Complete: %s (%d bytes in %lu ms, avg %.1f KB/s)", state.fileName.c_str(), state.size,
                elapsed, avgKbps);
        LOG_DBG("WEB", "[UPLOAD] Diagnostics: %d writes, total write time: %lu ms (%.1f%%)", writeCount, totalWriteTime,
                writePercent);
      }
    }
  } else if (upload.status == UPLOAD_FILE_ABORTED) {
    state.bufferPos = 0;  // Discard buffered data
    if (state.owner == UploadState::Owner::Http) {
      const BookUpload::AtomicUploadIo io = atomicUploadIo(state.file);
      const BookUpload::AtomicUploadStatus status = BookUpload::abort(state.transaction, io);
      if (status != BookUpload::AtomicUploadStatus::Ok) {
        LOG_ERR("WEB", "[UPLOAD] Abort cleanup failed: %s", atomicUploadStatusMessage(status));
      }
      state.owner = UploadState::Owner::None;
    }
    state.error = "Upload aborted";
    LOG_DBG("WEB", "Upload aborted");
  }
}

void CrossPointWebServer::handleUploadPost(UploadState& state) const {
  const BookUpload::UploadTransportResponse response{state.success, !state.error.isEmpty()};
  if (BookUpload::httpResponseStatus(response) == 200) {
    server->send(200, "text/plain", "File uploaded successfully: " + state.fileName);
  } else {
    const String error = state.error.isEmpty() ? "Unknown error during upload" : state.error;
    server->send(400, "text/plain", error);
  }
}

void CrossPointWebServer::handleCreateFolder() const {
  // Get folder name from form data
  if (!server->hasArg("name")) {
    server->send(400, "text/plain", "Missing folder name");
    return;
  }

  const String folderName = StringUtils::sanitizeFilename(server->arg("name").c_str()).c_str();

  // Validate folder name
  if (folderName.isEmpty() || folderName == "book") {
    server->send(400, "text/plain", "Invalid folder name");
    return;
  }

  // Get parent path
  String parentPath = "/";
  if (server->hasArg("path")) {
    parentPath = normalizeWebPath(server->arg("path"));
  }

  // Build full folder path
  String folderPath = parentPath;
  if (!folderPath.endsWith("/")) folderPath += "/";
  folderPath += folderName;

  if (isProtectedPath(folderPath)) {
    server->send(403, "text/plain", "Access denied to protected path");
    return;
  }

  LOG_DBG("WEB", "Creating folder: %s", folderPath.c_str());

  // Check if already exists
  if (Storage.exists(folderPath.c_str())) {
    server->send(400, "text/plain", "Folder already exists");
    return;
  }

  // Create the folder
  if (Storage.mkdir(folderPath.c_str())) {
    LOG_DBG("WEB", "Folder created successfully: %s", folderPath.c_str());
    server->send(200, "text/plain", "Folder created: " + folderName);
  } else {
    LOG_DBG("WEB", "Failed to create folder: %s", folderPath.c_str());
    server->send(500, "text/plain", "Failed to create folder");
  }
}

void CrossPointWebServer::handleRename() const {
  if (!server->hasArg("path") || !server->hasArg("name")) {
    server->send(400, "text/plain", "Missing path or new name");
    return;
  }

  String itemPath = normalizeWebPath(server->arg("path"));
  String newName = StringUtils::sanitizeFilename(server->arg("name").c_str()).c_str();

  if (itemPath.isEmpty() || itemPath == "/") {
    server->send(400, "text/plain", "Invalid path");
    return;
  }
  if (newName.isEmpty()) {
    server->send(400, "text/plain", "New name cannot be empty");
    return;
  }
  if (isProtectedPath(itemPath)) {
    server->send(403, "text/plain", "Cannot rename protected item");
    return;
  }

  // Calculate new path to check if it's protected
  String parentPath = itemPath.substring(0, itemPath.lastIndexOf('/'));
  if (parentPath.isEmpty()) {
    parentPath = "/";
  }
  String newPath = parentPath;
  if (!newPath.endsWith("/")) {
    newPath += "/";
  }
  newPath += newName;

  if (isProtectedPath(newPath)) {
    server->send(403, "text/plain", "Cannot rename to protected path");
    return;
  }

  const String itemName = itemPath.substring(itemPath.lastIndexOf('/') + 1);
  if (newName == itemName) {
    server->send(200, "text/plain", "Name unchanged");
    return;
  }

  if (!Storage.exists(itemPath.c_str())) {
    server->send(404, "text/plain", "Item not found");
    return;
  }

  HalFile file = Storage.open(itemPath.c_str());
  if (!file) {
    server->send(500, "text/plain", "Failed to open file");
    return;
  }
  if (file.isDirectory()) {
    file.close();
    server->send(400, "text/plain", "Only files can be renamed");
    return;
  }

  if (Storage.exists(newPath.c_str())) {
    file.close();
    server->send(409, "text/plain", "Target already exists");
    return;
  }

  const bool journaledPdf = FsHelpers::hasPdfExtension(itemPath);
  bool success = false;
  BookMoveUtils::MoveResult moveResult = BookMoveUtils::MoveResult::Unsupported;
  if (journaledPdf) {
    file.close();
    moveResult = BookMoveUtils::moveBook(itemPath.c_str(), newPath.c_str());
    success = moveResult == BookMoveUtils::MoveResult::Complete;
  } else {
    clearBookCache(itemPath.c_str());
    success = file.rename(newPath.c_str());
    file.close();
  }

  if (success) {
    LOG_DBG("WEB", "Renamed file: %s -> %s", itemPath.c_str(), newPath.c_str());
    server->send(200, "text/plain", "Renamed successfully");
  } else if (journaledPdf && moveResult == BookMoveUtils::MoveResult::Conflict) {
    server->send(409, "text/plain", "A book move is already pending");
  } else if (journaledPdf && Storage.exists(newPath.c_str())) {
    server->send(503, "text/plain", "File moved, but its reading data could not be updated. Restart to retry.");
  } else {
    LOG_ERR("WEB", "Failed to rename file: %s -> %s", itemPath.c_str(), newPath.c_str());
    server->send(500, "text/plain", "Failed to rename file");
  }
}

void CrossPointWebServer::handleMove() const {
  if (!server->hasArg("path") || !server->hasArg("dest")) {
    server->send(400, "text/plain", "Missing path or destination");
    return;
  }

  String itemPath = normalizeWebPath(server->arg("path"));
  String destPath = normalizeWebPath(server->arg("dest"));

  if (itemPath.isEmpty() || itemPath == "/") {
    server->send(400, "text/plain", "Invalid path");
    return;
  }
  if (destPath.isEmpty()) {
    server->send(400, "text/plain", "Invalid destination");
    return;
  }

  if (isProtectedPath(itemPath)) {
    server->send(403, "text/plain", "Cannot move protected item");
    return;
  }
  if (isProtectedPath(destPath)) {
    server->send(403, "text/plain", "Cannot move into protected folder");
    return;
  }

  const String itemName = itemPath.substring(itemPath.lastIndexOf('/') + 1);

  if (!Storage.exists(itemPath.c_str())) {
    server->send(404, "text/plain", "Item not found");
    return;
  }

  HalFile file = Storage.open(itemPath.c_str());
  if (!file) {
    server->send(500, "text/plain", "Failed to open file");
    return;
  }
  if (file.isDirectory()) {
    file.close();
    server->send(400, "text/plain", "Only files can be moved");
    return;
  }

  if (!Storage.exists(destPath.c_str())) {
    file.close();
    server->send(404, "text/plain", "Destination not found");
    return;
  }
  HalFile destDir = Storage.open(destPath.c_str());
  if (!destDir || !destDir.isDirectory()) {
    if (destDir) {
      destDir.close();
    }
    file.close();
    server->send(400, "text/plain", "Destination is not a folder");
    return;
  }
  destDir.close();

  String newPath = destPath;
  if (!newPath.endsWith("/")) {
    newPath += "/";
  }
  newPath += itemName;

  if (newPath == itemPath) {
    file.close();
    server->send(200, "text/plain", "Already in destination");
    return;
  }
  if (Storage.exists(newPath.c_str())) {
    file.close();
    server->send(409, "text/plain", "Target already exists");
    return;
  }

  const bool journaledPdf = FsHelpers::hasPdfExtension(itemPath);
  bool success = false;
  BookMoveUtils::MoveResult moveResult = BookMoveUtils::MoveResult::Unsupported;
  if (journaledPdf) {
    file.close();
    moveResult = BookMoveUtils::moveBook(itemPath.c_str(), newPath.c_str());
    success = moveResult == BookMoveUtils::MoveResult::Complete;
  } else {
    clearBookCache(itemPath.c_str());
    success = file.rename(newPath.c_str());
    file.close();
  }

  if (success) {
    LOG_DBG("WEB", "Moved file: %s -> %s", itemPath.c_str(), newPath.c_str());
    server->send(200, "text/plain", "Moved successfully");
  } else if (journaledPdf && moveResult == BookMoveUtils::MoveResult::Conflict) {
    server->send(409, "text/plain", "A book move is already pending");
  } else if (journaledPdf && Storage.exists(newPath.c_str())) {
    server->send(503, "text/plain", "File moved, but its reading data could not be updated. Restart to retry.");
  } else {
    LOG_ERR("WEB", "Failed to move file: %s -> %s", itemPath.c_str(), newPath.c_str());
    server->send(500, "text/plain", "Failed to move file");
  }
}

void CrossPointWebServer::handleDelete() const {
  // To ensure backwards compatibility, plain `path` is mapped
  // to a single element JSON array.
  bool hasPathArg = server->hasArg("path");
  bool hasPathsArg = server->hasArg("paths");
  // Check 'paths' or `path` argument is provided
  if (!(hasPathArg || hasPathsArg)) {
    server->send(400, "text/plain", "Missing `path` or `paths` argument");
    return;
  }
  if (hasPathArg && hasPathsArg) {
    server->send(400, "text/plain", "Provide either 'path' or 'paths', not both");
    return;
  }

  // Parse paths
  String pathsArg;
  JsonDocument doc;
  DeserializationError error = DeserializationError(DeserializationError::Code::Ok);
  if (hasPathsArg) {
    pathsArg = server->arg("paths");
    error = deserializeJson(doc, pathsArg);
  } else {
    pathsArg = server->arg("path");
    doc.add(pathsArg);
  }
  if (error) {
    server->send(400, "text/plain", "Invalid paths format");
    return;
  }

  auto paths = doc.as<JsonArray>();
  if (paths.isNull() || paths.size() == 0) {
    server->send(400, "text/plain", "No paths provided");
    return;
  }

  // Iterate over paths and delete each item
  bool allSuccess = true;
  String failedItems;

  for (const auto& p : paths) {
    auto itemPath = p.as<String>();

    // Validate path
    if (itemPath.isEmpty() || itemPath == "/") {
      failedItems += itemPath + " (cannot delete root); ";
      allSuccess = false;
      continue;
    }

    // Ensure path starts with /
    if (!itemPath.startsWith("/")) {
      itemPath = "/" + itemPath;
    }

    if (itemPath.endsWith(PdfDelete::kTombstoneSuffix)) {
      failedItems += itemPath + " (reserved PDF deletion state); ";
      allSuccess = false;
      continue;
    }

    // Security check: prevent deletion of protected items
    if (isProtectedPath(itemPath)) {
      failedItems += itemPath + " (protected path); ";
      allSuccess = false;
      continue;
    }

    // Check if item exists
    if (!Storage.exists(itemPath.c_str())) {
      failedItems += itemPath + " (not found); ";
      allSuccess = false;
      continue;
    }

    // Decide whether it's a directory or file by opening it
    bool success = false;
    HalFile f = Storage.open(itemPath.c_str());
    if (f && f.isDirectory()) {
      // For folders, ensure empty before removing
      HalFile entry = f.openNextFile();
      if (entry) {
        entry.close();
        f.close();
        failedItems += itemPath + " (folder not empty); ";
        allSuccess = false;
        continue;
      }
      f.close();
      success = Storage.rmdir(itemPath.c_str());
    } else {
      // It's a file (or couldn't open as dir) — remove file
      if (f) f.close();
      // PDF_DELETE_ADAPTER_BEGIN
      if (FsHelpers::hasPdfExtension(itemPath)) {
        success = PdfDeleteUtils::deletePdfBook(itemPath.c_str()) == PdfDeleteUtils::Result::Complete;
      } else {
        success = Storage.remove(itemPath.c_str());
        clearBookCache(itemPath.c_str());
      }
      // PDF_DELETE_ADAPTER_END
    }

    if (!success) {
      failedItems += itemPath + " (deletion failed); ";
      allSuccess = false;
    }
  }

  if (allSuccess) {
    server->send(200, "text/plain", "All items deleted successfully");
  } else {
    server->send(500, "text/plain", "Failed to delete some items: " + failedItems);
  }
}

void CrossPointWebServer::handleSettingsPage() const {
  sendHtmlContent(server.get(), SettingsPageHtml, sizeof(SettingsPageHtml));
  LOG_DBG("WEB", "Served settings page");
}

void CrossPointWebServer::handleGetSettings() const {
  // Pass the SD font registry so the fontFamily setting's enumStringValues
  // includes SD-resident families — otherwise the web API only exposes the
  // three built-in fonts.
  const auto& settings = getSettingsList(&sdFontSystem.registry());

  server->setContentLength(CONTENT_LENGTH_UNKNOWN);
  server->send(200, "application/json", "");
  server->sendContent("[");

  char output[512];
  constexpr size_t outputSize = sizeof(output);
  bool seenFirst = false;
  JsonDocument doc;

  for (const auto& s : settings) {
    if (!s.key) continue;  // Skip ACTION-only entries

    doc.clear();
    doc["key"] = s.key;
    doc["name"] = I18N.get(s.nameId);
    doc["category"] = I18N.get(s.category);

    switch (s.type) {
      case SettingType::TOGGLE: {
        doc["type"] = "toggle";
        if (s.valuePtr) {
          doc["value"] = static_cast<int>(SETTINGS.*(s.valuePtr));
        }
        break;
      }
      case SettingType::ENUM: {
        doc["type"] = "enum";
        if (s.valuePtr) {
          doc["value"] = static_cast<int>(enumDisplayIndexForRawValue(s, SETTINGS.*(s.valuePtr)));
        } else if (s.valueGetter) {
          doc["value"] = static_cast<int>(s.valueGetter());
        }
        JsonArray options = doc["options"].to<JsonArray>();
        if (!s.enumStringValues.empty()) {
          for (const auto& opt : s.enumStringValues) {
            options.add(opt);
          }
        } else {
          for (const auto& opt : s.enumValues) {
            options.add(I18N.get(opt));
          }
        }
        break;
      }
      case SettingType::VALUE: {
        doc["type"] = "value";
        if (s.valuePtr) {
          doc["value"] = static_cast<int>(SETTINGS.*(s.valuePtr));
        }
        doc["min"] = s.valueRange.min;
        doc["max"] = s.valueRange.max;
        doc["step"] = s.valueRange.step;
        break;
      }
      case SettingType::STRING: {
        doc["type"] = "string";
        if (s.stringGetter) {
          doc["value"] = s.stringGetter();
        } else if (s.stringMaxLen > 0) {
          doc["value"] = reinterpret_cast<const char*>(&SETTINGS) + s.stringOffset;
        }
        break;
      }
      default:
        continue;
    }

    const size_t written = serializeJson(doc, output, outputSize);
    if (written >= outputSize) {
      LOG_DBG("WEB", "Skipping oversized setting JSON for: %s", s.key);
      continue;
    }

    if (seenFirst) {
      server->sendContent(",");
    } else {
      seenFirst = true;
    }
    server->sendContent(output);
  }

  server->sendContent("]");
  server->sendContent("");
  LOG_DBG("WEB", "Served settings API");
}

void CrossPointWebServer::handlePostSettings() {
  if (!server->hasArg("plain")) {
    server->send(400, "text/plain", "Missing JSON body");
    return;
  }

  const String body = server->arg("plain");
  JsonDocument doc;
  const DeserializationError err = deserializeJson(doc, body);
  if (err) {
    server->send(400, "text/plain", String("Invalid JSON: ") + err.c_str());
    return;
  }

  const auto& settings = getSettingsList(&sdFontSystem.registry());
  int applied = 0;

  for (const auto& s : settings) {
    if (!s.key) continue;
    if (!doc[s.key].is<JsonVariant>()) continue;

    switch (s.type) {
      case SettingType::TOGGLE: {
        const int val = doc[s.key].as<int>() ? 1 : 0;
        if (s.valuePtr) {
          SETTINGS.*(s.valuePtr) = val;
        }
        applied++;
        break;
      }
      case SettingType::ENUM: {
        const int val = doc[s.key].as<int>();
        const int maxVal = s.enumStringValues.empty() ? static_cast<int>(s.enumValues.size())
                                                      : static_cast<int>(s.enumStringValues.size());
        if (val >= 0 && val < maxVal) {
          if (s.valuePtr) {
            SETTINGS.*(s.valuePtr) = enumRawValueForDisplayIndex(s, static_cast<uint8_t>(val));
          } else if (s.valueSetter) {
            s.valueSetter(static_cast<uint8_t>(val));
          }
          applied++;
        }
        break;
      }
      case SettingType::VALUE: {
        const int val = doc[s.key].as<int>();
        if (val >= s.valueRange.min && val <= s.valueRange.max) {
          if (s.valuePtr) {
            SETTINGS.*(s.valuePtr) = static_cast<uint8_t>(val);
          }
          applied++;
        }
        break;
      }
      case SettingType::STRING: {
        const std::string val = doc[s.key].as<std::string>();
        if (std::strcmp(s.key, "deviceName") == 0 && (val.length() < CrossPointSettings::MIN_DEVICE_NAME_LENGTH ||
                                                      val.length() > CrossPointSettings::MAX_DEVICE_NAME_LENGTH)) {
          break;
        }
        if (s.stringSetter) {
          s.stringSetter(val);
        } else if (s.stringMaxLen > 0) {
          char* ptr = reinterpret_cast<char*>(&SETTINGS) + s.stringOffset;
          strncpy(ptr, val.c_str(), s.stringMaxLen - 1);
          ptr[s.stringMaxLen - 1] = '\0';
        }
        applied++;
        break;
      }
      default:
        break;
    }
  }

  SETTINGS.saveToFile();

  LOG_DBG("WEB", "Applied %d setting(s)", applied);
  server->send(200, "text/plain", String("Applied ") + String(applied) + " setting(s)");
}

// ---- OPDS Server API ----

void CrossPointWebServer::handleGetOpdsServers() const {
  const auto& servers = OPDS_STORE.getServers();

  // Stream JSON array incrementally to avoid allocating the full response in memory
  server->setContentLength(CONTENT_LENGTH_UNKNOWN);
  server->send(200, "application/json", "");
  server->sendContent("[");

  char output[512];
  constexpr size_t outputSize = sizeof(output);
  JsonDocument doc;

  for (size_t i = 0; i < servers.size(); i++) {
    doc.clear();
    doc["index"] = i;
    doc["name"] = servers[i].name;
    doc["url"] = servers[i].url;
    doc["username"] = servers[i].username;
    doc["filenameFormat"] = opdsFilenameFormatToJson(servers[i].filenameFormat);
    // Never expose passwords over the API — only indicate whether one is set
    doc["hasPassword"] = !servers[i].password.empty();

    const size_t written = serializeJson(doc, output, outputSize);
    if (written >= outputSize) continue;

    if (i > 0) server->sendContent(",");
    server->sendContent(output);
  }

  server->sendContent("]");
  server->sendContent("");
  LOG_DBG("WEB", "Served OPDS servers API (%zu servers)", servers.size());
}

void CrossPointWebServer::handlePostOpdsServer() {
  if (!server->hasArg("plain")) {
    server->send(400, "text/plain", "Missing JSON body");
    return;
  }

  const String body = server->arg("plain");
  JsonDocument doc;
  const DeserializationError err = deserializeJson(doc, body);
  if (err) {
    server->send(400, "text/plain", String("Invalid JSON: ") + err.c_str());
    return;
  }

  OpdsServer opdsServer;
  opdsServer.name = doc["name"] | std::string("");
  opdsServer.url = doc["url"] | std::string("");
  opdsServer.username = doc["username"] | std::string("");
  opdsServer.filenameFormat = opdsFilenameFormatFromJson(doc["filenameFormat"] | "");

  // The password field is optional in the JSON payload. When absent (vs. present but empty),
  // we preserve the existing password — the web UI omits it when the user hasn't changed it.
  bool hasPasswordField = doc["password"].is<const char*>() || doc["password"].is<std::string>();
  std::string password = doc["password"] | std::string("");
  const bool hasFilenameFormatField =
      doc["filenameFormat"].is<const char*>() || doc["filenameFormat"].is<std::string>();

  if (doc["index"].is<int>()) {
    int idx = doc["index"].as<int>();
    if (idx < 0 || idx >= static_cast<int>(OPDS_STORE.getCount())) {
      server->send(400, "text/plain", "Invalid server index");
      return;
    }
    const auto* existing = OPDS_STORE.getServer(static_cast<size_t>(idx));
    // Preserve existing values for fields older clients do not know how to send.
    if (existing && !hasPasswordField) {
      password = existing->password;
    }
    if (existing && !hasFilenameFormatField) {
      opdsServer.filenameFormat = existing->filenameFormat;
    }
    opdsServer.password = password;
    OPDS_STORE.updateServer(static_cast<size_t>(idx), opdsServer);
    LOG_DBG("WEB", "Updated OPDS server at index %d", idx);
  } else {
    opdsServer.password = password;
    if (!OPDS_STORE.addServer(opdsServer)) {
      server->send(400, "text/plain", "Cannot add server (limit reached)");
      return;
    }
    LOG_DBG("WEB", "Added new OPDS server: %s", opdsServer.name.c_str());
  }

  server->send(200, "text/plain", "OK");
}

// Uses POST (not HTTP DELETE) because ESP32 WebServer doesn't support DELETE with body.
void CrossPointWebServer::handleDeleteOpdsServer() {
  if (!server->hasArg("plain")) {
    server->send(400, "text/plain", "Missing JSON body");
    return;
  }

  const String body = server->arg("plain");
  JsonDocument doc;
  const DeserializationError err = deserializeJson(doc, body);
  if (err) {
    server->send(400, "text/plain", String("Invalid JSON: ") + err.c_str());
    return;
  }

  if (!doc["index"].is<int>()) {
    server->send(400, "text/plain", "Missing index");
    return;
  }

  int idx = doc["index"].as<int>();
  if (idx < 0 || idx >= static_cast<int>(OPDS_STORE.getCount())) {
    server->send(400, "text/plain", "Invalid server index");
    return;
  }

  OPDS_STORE.removeServer(static_cast<size_t>(idx));
  LOG_DBG("WEB", "Deleted OPDS server at index %d", idx);
  server->send(200, "text/plain", "OK");
}

// ---- Wi-Fi Credentials API ----

void CrossPointWebServer::handleGetWifiNetworks() const {
  const auto& credentials = WIFI_STORE.getCredentials();
  const std::string& lastConnectedSsid = WIFI_STORE.getLastConnectedSsid();

  // Stream JSON array incrementally to avoid allocating the full response in memory
  server->setContentLength(CONTENT_LENGTH_UNKNOWN);
  server->send(200, "application/json", "");
  server->sendContent("[");

  char output[320];
  constexpr size_t outputSize = sizeof(output);
  JsonDocument doc;

  for (size_t i = 0; i < credentials.size(); i++) {
    doc.clear();
    doc["index"] = i;
    doc["ssid"] = credentials[i].ssid;
    // Never expose Wi-Fi passwords over the API — only indicate whether one is set
    doc["hasPassword"] = !credentials[i].password.empty();
    doc["isLastConnected"] = credentials[i].ssid == lastConnectedSsid;

    const size_t written = serializeJson(doc, output, outputSize);
    if (written >= outputSize) continue;

    if (i > 0) server->sendContent(",");
    server->sendContent(output);
  }

  server->sendContent("]");
  server->sendContent("");
  LOG_DBG("WEB", "Served Wi-Fi credentials API (%zu network(s))", credentials.size());
}

void CrossPointWebServer::handlePostWifiNetwork() {
  if (!server->hasArg("plain")) {
    server->send(400, "text/plain", "Missing JSON body");
    return;
  }

  const String body = server->arg("plain");
  JsonDocument doc;
  const DeserializationError err = deserializeJson(doc, body);
  if (err) {
    server->send(400, "text/plain", String("Invalid JSON: ") + err.c_str());
    return;
  }

  std::string ssid = doc["ssid"] | std::string("");
  if (ssid.empty()) {
    server->send(400, "text/plain", "SSID is required");
    return;
  }

  // The password field is optional in the JSON payload. When absent (vs. present but empty),
  // preserve the existing password for updates. Empty passwords are valid for open networks.
  bool hasPasswordField = doc["password"].is<const char*>() || doc["password"].is<std::string>();
  std::string password = doc["password"] | std::string("");

  if (doc["index"].is<int>()) {
    int idx = doc["index"].as<int>();
    const auto& credentials = WIFI_STORE.getCredentials();
    if (idx < 0 || idx >= static_cast<int>(credentials.size())) {
      server->send(400, "text/plain", "Invalid network index");
      return;
    }

    const std::string oldSsid = credentials[static_cast<size_t>(idx)].ssid;
    if (!hasPasswordField) {
      password = credentials[static_cast<size_t>(idx)].password;
    }

    bool ok = true;
    if (oldSsid != ssid) {
      ok = WIFI_STORE.removeCredential(oldSsid) && WIFI_STORE.addCredential(ssid, password);
    } else {
      ok = WIFI_STORE.addCredential(ssid, password);
    }

    if (!ok) {
      server->send(400, "text/plain", "Failed to update Wi-Fi network");
      return;
    }

    LOG_DBG("WEB", "Updated Wi-Fi network at index %d (SSID: %s)", idx, ssid.c_str());
  } else {
    if (!WIFI_STORE.addCredential(ssid, password)) {
      server->send(400, "text/plain", "Cannot add network (limit reached)");
      return;
    }
    LOG_DBG("WEB", "Added Wi-Fi network: %s", ssid.c_str());
  }

  server->send(200, "text/plain", "OK");
}

// Uses POST (not HTTP DELETE) because ESP32 WebServer doesn't support DELETE with body.
void CrossPointWebServer::handleDeleteWifiNetwork() {
  if (!server->hasArg("plain")) {
    server->send(400, "text/plain", "Missing JSON body");
    return;
  }

  const String body = server->arg("plain");
  JsonDocument doc;
  const DeserializationError err = deserializeJson(doc, body);
  if (err) {
    server->send(400, "text/plain", String("Invalid JSON: ") + err.c_str());
    return;
  }

  if (!doc["index"].is<int>()) {
    server->send(400, "text/plain", "Missing index");
    return;
  }

  int idx = doc["index"].as<int>();
  const auto& credentials = WIFI_STORE.getCredentials();
  if (idx < 0 || idx >= static_cast<int>(credentials.size())) {
    server->send(400, "text/plain", "Invalid network index");
    return;
  }

  const std::string ssid = credentials[static_cast<size_t>(idx)].ssid;
  if (!WIFI_STORE.removeCredential(ssid)) {
    server->send(400, "text/plain", "Failed to delete Wi-Fi network");
    return;
  }

  LOG_DBG("WEB", "Deleted Wi-Fi network at index %d (SSID: %s)", idx, ssid.c_str());
  server->send(200, "text/plain", "OK");
}

// WebSocket callback trampoline
void CrossPointWebServer::wsEventCallback(uint8_t num, WStype_t type, uint8_t* payload, size_t length) {
  if (wsInstance) {
    wsInstance->onWebSocketEvent(num, type, payload, length);
  }
}

// WebSocket event handler for fast binary uploads
// Protocol:
//   1. Client sends TEXT message: "START:<filename>:<size>:<path>"
//   2. Client sends BINARY messages with file data chunks
//   3. Server sends TEXT "PROGRESS:<received>:<total>" after each chunk
//   4. Server sends TEXT "DONE" or "ERROR:<message>" when complete
void CrossPointWebServer::onWebSocketEvent(uint8_t num, WStype_t type, uint8_t* payload, size_t length) {
  switch (type) {
    case WStype_DISCONNECTED:
      LOG_DBG("WS", "Client %u disconnected", num);
      // Only clean up if this is the client that owns the active upload.
      // A new client may have already started a fresh upload before this
      // DISCONNECTED event fires (race condition on quick cancel + retry).
      if (num == wsUploadClientNum && wsUploadInProgress && upload.owner == UploadState::Owner::WebSocket &&
          BookUpload::isActive(upload.transaction)) {
        abortWsUpload("WS");
      }
      break;

    case WStype_CONNECTED: {
      LOG_DBG("WS", "Client %u connected", num);
      break;
    }

    case WStype_TEXT: {
      // Parse control messages
      String msg = String((char*)payload);
      LOG_DBG("WS", "Text from client %u: %s", num, msg.c_str());

      if (msg.startsWith("START:")) {
        // Reject any START while an upload is already active so both transports
        // keep exactly one transaction/file handle (owning re-START included).
        if (BookUpload::isActive(upload.transaction)) {
          wsServer->sendTXT(num, "ERROR:Upload already in progress");
          break;
        }

        // Parse: START:<filename>:<size>:<path>
        int firstColon = msg.indexOf(':', 6);
        int secondColon = msg.indexOf(':', firstColon + 1);

        if (firstColon > 0 && secondColon > 0) {
          wsUploadFileName = StringUtils::sanitizeFilename(msg.substring(6, firstColon).c_str()).c_str();
          String sizeToken = msg.substring(firstColon + 1, secondColon);
          if (!parseUploadSizeToken(sizeToken, wsUploadSize)) {
            LOG_DBG("WS", "START rejected: invalid size token '%s'", sizeToken.c_str());
            wsServer->sendTXT(num, "ERROR:Invalid START format");
            return;
          }
          wsUploadPath = normalizeWebPath(msg.substring(secondColon + 1));
          wsUploadReceived = 0;
          wsLastProgressSent = 0;
          wsUploadStartTime = millis();

          // Build file path
          String filePath = wsUploadPath;
          if (!filePath.endsWith("/")) filePath += "/";
          filePath += wsUploadFileName;

          if (isProtectedPath(filePath)) {
            wsServer->sendTXT(num, "ERROR:Access denied to protected path");
            wsUploadInProgress = false;
            wsUploadClientNum = 255;
            return;
          }

          LOG_DBG("WS", "Starting upload: %s (%d bytes) to %s", wsUploadFileName.c_str(), wsUploadSize,
                  filePath.c_str());

          const BookUpload::AtomicUploadIo io = atomicUploadIo(upload.file);
          const BookUpload::AtomicUploadStatus beginStatus =
              BookUpload::begin(upload.transaction, io, filePath.c_str(), wsUploadSize);
          if (beginStatus != BookUpload::AtomicUploadStatus::Ok) {
            wsServer->sendTXT(num, String("ERROR:") + atomicUploadStatusMessage(beginStatus));
            wsUploadInProgress = false;
            wsUploadClientNum = 255;
            return;
          }
          upload.owner = UploadState::Owner::WebSocket;

          // Zero-byte upload: complete immediately without waiting for BIN frames
          if (wsUploadSize == 0) {
            const BookUpload::AtomicUploadStatus finishStatus = BookUpload::finish(
                upload.transaction, io, 0, upload.buffer.data(), upload.buffer.size(), uploadCommitHook());
            upload.owner = UploadState::Owner::None;
            if (finishStatus != BookUpload::AtomicUploadStatus::Ok) {
              wsServer->sendTXT(num, String("ERROR:") + atomicUploadStatusMessage(finishStatus));
              wsLastProgressSent = 0;
              return;
            }
            wsLastCompleteName = wsUploadFileName;
            wsLastCompleteSize = 0;
            wsLastCompleteAt = millis();
            LOG_DBG("WS", "Zero-byte upload complete: %s", filePath.c_str());
            wsServer->sendTXT(num, "DONE");
            wsLastProgressSent = 0;
            break;
          }

          wsUploadClientNum = num;
          wsUploadInProgress = true;
          wsServer->sendTXT(num, "READY");
        } else {
          wsServer->sendTXT(num, "ERROR:Invalid START format");
        }
      }
      break;
    }

    case WStype_BIN: {
      if (!wsUploadInProgress || upload.owner != UploadState::Owner::WebSocket ||
          !BookUpload::isActive(upload.transaction) || num != wsUploadClientNum) {
        wsServer->sendTXT(num, "ERROR:No upload in progress");
        return;
      }

      // Write binary data directly to file
      size_t remaining = wsUploadSize - wsUploadReceived;
      if (length > remaining) {
        abortWsUpload("WS");
        wsServer->sendTXT(num, "ERROR:Upload overflow");
        return;
      }
      const BookUpload::AtomicUploadIo io = atomicUploadIo(upload.file);
      const BookUpload::AtomicUploadStatus writeStatus = BookUpload::write(upload.transaction, io, payload, length);
      if (writeStatus != BookUpload::AtomicUploadStatus::Ok) {
        abortWsUpload("WS");
        wsServer->sendTXT(num, String("ERROR:") + atomicUploadStatusMessage(writeStatus));
        return;
      }

      wsUploadReceived += length;

      // Send progress update (every 64KB or at end)
      if (wsUploadReceived - wsLastProgressSent >= 65536 || wsUploadReceived >= wsUploadSize) {
        String progress = "PROGRESS:" + String(wsUploadReceived) + ":" + String(wsUploadSize);
        wsServer->sendTXT(num, progress);
        wsLastProgressSent = wsUploadReceived;
      }

      // Check if upload complete
      if (wsUploadReceived >= wsUploadSize) {
        const BookUpload::AtomicUploadStatus finishStatus = BookUpload::finish(
            upload.transaction, io, wsUploadSize, upload.buffer.data(), upload.buffer.size(), uploadCommitHook());
        upload.owner = UploadState::Owner::None;
        wsUploadInProgress = false;
        wsUploadClientNum = 255;
        if (finishStatus != BookUpload::AtomicUploadStatus::Ok) {
          wsServer->sendTXT(num, String("ERROR:") + atomicUploadStatusMessage(finishStatus));
          wsLastProgressSent = 0;
          return;
        }

        wsLastCompleteName = wsUploadFileName;
        wsLastCompleteSize = wsUploadSize;
        wsLastCompleteAt = millis();

        unsigned long elapsed = millis() - wsUploadStartTime;
        float kbps = (elapsed > 0) ? (wsUploadSize / 1024.0) / (elapsed / 1000.0) : 0;

        LOG_DBG("WS", "Upload complete: %s (%d bytes in %lu ms, %.1f KB/s)", wsUploadFileName.c_str(), wsUploadSize,
                elapsed, kbps);

        wsServer->sendTXT(num, "DONE");
        wsLastProgressSent = 0;
      }
      break;
    }

    default:
      break;
  }
}

// --- Font management handlers ---

void CrossPointWebServer::handleFontsPage() const {
  sendHtmlContent(server.get(), FontsPageHtml, sizeof(FontsPageHtml));
  LOG_DBG("WEB", "Served fonts page");
}

void CrossPointWebServer::handleFontList() const {
  // Pick up any uploads/deletes that happened since the last reader load.
  const_cast<SdCardFontSystem&>(sdFontSystem).refreshIfDirty();
  const auto& families = sdFontSystem.registry().getFamilies();

  JsonDocument doc;
  JsonArray arr = doc["families"].to<JsonArray>();
  doc["maxFamilies"] = SdCardFontRegistry::MAX_SD_FAMILIES;

  for (const auto& family : families) {
    JsonObject fObj = arr.add<JsonObject>();
    fObj["name"] = family.name;

    JsonArray sizes = fObj["sizes"].to<JsonArray>();
    for (uint8_t s : family.availableSizes()) {
      sizes.add(s);
    }

    JsonArray files = fObj["files"].to<JsonArray>();
    for (const auto& file : family.files) {
      JsonObject fileObj = files.add<JsonObject>();
      // Extract filename from full path
      const char* name = strrchr(file.path.c_str(), '/');
      fileObj["name"] = name ? name + 1 : file.path.c_str();

      // Stat the file for size
      HalFile f;
      if (Storage.openFileForRead("WEB", file.path.c_str(), f)) {
        fileObj["size"] = static_cast<unsigned long>(f.size());
        f.close();
      } else {
        fileObj["size"] = 0;
      }
    }
  }

  String json;
  serializeJson(doc, json);
  server->send(200, "application/json", json);
}

void CrossPointWebServer::handleFontUploadData() {
  HTTPUpload& upload = server->upload();

  switch (upload.status) {
    case UPLOAD_FILE_START: {
      esp_task_wdt_reset();
      String family = server->arg("family");
      fontUpload.file = HalFile();
      fontUpload.familyName.clear();
      fontUpload.filePath.clear();
      fontUpload.valid = false;
      fontUpload.magicChecked = false;
      fontUpload.bytesWritten = 0;
      fontUpload.bufferPos = 0;

      if (!FontInstaller::isValidFamilyName(family.c_str())) {
        LOG_ERR("WEB", "Invalid font family name: %s", family.c_str());
        break;
      }

      String filename = upload.filename;
      filename.replace(' ', '_');
      // Validate filename: rejects path traversal (../, /, \) and enforces
      // a .cpfont basename of alphanumeric + hyphen + underscore. Without
      // this an attacker could supply "../../.crosspoint/settings.json" as
      // a "filename" and have it written outside the fonts directory.
      if (!FontInstaller::isValidCpfontFilename(filename.c_str())) {
        LOG_ERR("WEB", "Invalid font filename: %s", filename.c_str());
        break;
      }

      fontUpload.familyName = family.c_str();

      // Create a temporary FontInstaller for directory creation
      FontInstaller installer(sdFontSystem.registry());
      if (!installer.ensureFamilyDir(family.c_str())) {
        LOG_ERR("WEB", "Failed to create font family dir");
        break;
      }

      char path[192];
      FontInstaller::buildFontPath(family.c_str(), filename.c_str(), path, sizeof(path));
      fontUpload.filePath = path;

      if (!Storage.openFileForWrite("WEB", path, fontUpload.file)) {
        LOG_ERR("WEB", "Failed to open font file for write: %s", path);
        break;
      }

      fontUpload.valid = true;
      LOG_DBG("WEB", "Font upload started: %s -> %s", filename.c_str(), path);
      break;
    }

    case UPLOAD_FILE_WRITE: {
      if (!fontUpload.valid) break;
      esp_task_wdt_reset();

      // Validate magic bytes on first chunk only
      if (!fontUpload.magicChecked && upload.currentSize >= 8) {
        if (memcmp(upload.buf, "CPFONT\0\0", 8) != 0) {
          LOG_ERR("WEB", "Invalid .cpfont magic bytes");
          fontUpload.valid = false;
          break;
        }
        fontUpload.magicChecked = true;
      }

      // Buffer writes for efficiency
      size_t remaining = upload.currentSize;
      const uint8_t* src = upload.buf;
      while (remaining > 0) {
        size_t space = FontUploadState::BUFFER_SIZE - fontUpload.bufferPos;
        size_t chunk = (remaining < space) ? remaining : space;
        memcpy(fontUpload.buffer.data() + fontUpload.bufferPos, src, chunk);
        fontUpload.bufferPos += chunk;
        src += chunk;
        remaining -= chunk;

        if (fontUpload.bufferPos >= FontUploadState::BUFFER_SIZE) {
          fontUpload.file.write(fontUpload.buffer.data(), fontUpload.bufferPos);
          fontUpload.bytesWritten += fontUpload.bufferPos;
          fontUpload.bufferPos = 0;
          esp_task_wdt_reset();
        }
      }
      break;
    }

    case UPLOAD_FILE_END: {
      // Flush remaining buffer
      if (fontUpload.valid && fontUpload.bufferPos > 0) {
        fontUpload.file.write(fontUpload.buffer.data(), fontUpload.bufferPos);
        fontUpload.bytesWritten += fontUpload.bufferPos;
        fontUpload.bufferPos = 0;
      }
      if (fontUpload.file.isOpen()) {
        fontUpload.file.close();
      }

      if (!fontUpload.valid && !fontUpload.filePath.empty()) {
        Storage.remove(fontUpload.filePath.c_str());
      }

      LOG_DBG("WEB", "Font upload end: valid=%d, %zu bytes", fontUpload.valid, fontUpload.bytesWritten);
      break;
    }

    case UPLOAD_FILE_ABORTED: {
      if (fontUpload.file) {
        fontUpload.file.close();
      }
      if (!fontUpload.filePath.empty()) {
        Storage.remove(fontUpload.filePath.c_str());
      }
      fontUpload.valid = false;
      LOG_DBG("WEB", "Font upload aborted");
      break;
    }
  }
}

void CrossPointWebServer::handleFontUpload() {
  if (fontUpload.valid) {
    sdFontSystem.markRegistryDirty();
    server->send(200, "application/json", "{\"ok\":true}");
    LOG_DBG("WEB", "Font upload complete: %s", fontUpload.filePath.c_str());
  } else {
    server->send(400, "application/json", "{\"error\":\"Invalid .cpfont file\"}");
  }
}

void CrossPointWebServer::handleFontDelete() {
  String body = server->arg("plain");
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, body);

  if (err || !doc["family"].is<const char*>()) {
    server->send(400, "application/json", "{\"error\":\"Invalid request\"}");
    return;
  }

  const char* familyName = doc["family"];
  FontInstaller installer(sdFontSystem.registry());
  auto result = installer.deleteFamily(familyName);

  if (result == FontInstaller::Error::OK) {
    sdFontSystem.markRegistryDirty();
    server->send(200, "application/json", "{\"ok\":true}");
    LOG_DBG("WEB", "Deleted font family: %s", familyName);
  } else {
    server->send(500, "application/json", "{\"error\":\"Delete failed\"}");
    LOG_ERR("WEB", "Failed to delete font family: %s", familyName);
  }
}
