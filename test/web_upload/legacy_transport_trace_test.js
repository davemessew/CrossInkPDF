"use strict";

const childProcess = require("child_process");
const fs = require("fs");
const os = require("os");
const path = require("path");

const repoRoot = path.resolve(__dirname, "../..");
const serverPath = path.join(repoRoot, "src/network/CrossPointWebServer.cpp");
const baselinePath = path.join(__dirname, "fixtures/legacy_upload_baseline.cpp.txt");
const sanitize = process.env.WEB_UPLOAD_HARNESS_SANITIZERS === "1";
const forbidGit = process.env.WEB_UPLOAD_HARNESS_FORBID_GIT === "1";
const legacyExtensions = [".epub", ".xtc", ".xtch", ".txt", ".md", ".markdown"];
const legacyScenarios = [
  "http-success",
  "http-final-short",
  "http-abort",
  "http-stale-post",
  "ws-success",
  "ws-short",
  "ws-overflow",
  "ws-disconnect",
  "ws-zero",
  "ws-to-int",
];

function normalized(source) {
  return source.replace(/\r\n/g, "\n");
}

function functionBody(source, marker, optional = false) {
  const markerAt = source.indexOf(marker);
  if (markerAt < 0) {
    if (optional) return "";
    throw new Error(`missing function marker: ${marker}`);
  }
  const start = source.lastIndexOf("\n", markerAt) + 1;
  const opening = source.indexOf("{", markerAt);
  if (opening < 0) throw new Error(`missing function body: ${marker}`);
  let depth = 0;
  let quote = "";
  let lineComment = false;
  let blockComment = false;
  for (let index = opening; index < source.length; index += 1) {
    const ch = source[index];
    const next = source[index + 1];
    if (lineComment) {
      if (ch === "\n") lineComment = false;
      continue;
    }
    if (blockComment) {
      if (ch === "*" && next === "/") {
        blockComment = false;
        index += 1;
      }
      continue;
    }
    if (quote) {
      if (ch === "\\") {
        index += 1;
      } else if (ch === quote) {
        quote = "";
      }
      continue;
    }
    if (ch === "/" && next === "/") {
      lineComment = true;
      index += 1;
      continue;
    }
    if (ch === "/" && next === "*") {
      blockComment = true;
      index += 1;
      continue;
    }
    if (ch === '"' || ch === "'") {
      quote = ch;
      continue;
    }
    if (ch === "{") depth += 1;
    if (ch === "}") {
      depth -= 1;
      if (depth === 0) return source.slice(start, index + 1);
    }
  }
  throw new Error(`unterminated function: ${marker}`);
}

function replaceInFunction(source, marker, before, after) {
  const body = functionBody(source, marker);
  if (!body.includes(before)) throw new Error(`${marker}: mutation source not found`);
  return source.replace(body, body.replace(before, after));
}

function generatedHarness(source, { wideIntegers = false } = {}) {
  const rawParseSize =
    functionBody(source, "parseUploadSizeToken(", true) ||
    "bool parseUploadSizeToken(const String&, size_t&) { return false; }";
  const parseSize = wideIntegers ? rawParseSize : rawParseSize.replace(/\bsize_t\b/g, "uint32_t");
  const uploadSizeType = wideIntegers ? "size_t" : "uint32_t";
  const toIntImplementation = wideIntegers
    ? String.raw`long toInt() const {
    errno = 0;
    return std::strtol(value_.c_str(), nullptr, 10);
  }`
    : String.raw`int32_t toInt() const {
    errno = 0;
    const long long parsed = std::strtoll(value_.c_str(), nullptr, 10);
    if (errno == ERANGE || parsed > INT32_MAX) return INT32_MAX;
    if (parsed < INT32_MIN) return INT32_MIN;
    return static_cast<int32_t>(parsed);
  }`;
  const pdfGate =
    functionBody(source, "isPdfUploadTarget(", true) ||
    "bool isPdfUploadTarget(const String&) { return false; }";
  const protectedGate =
    functionBody(source, "isProtectedPath(", true) ||
    "bool isProtectedPath(const String&) { return false; }";
  const flush = functionBody(source, "flushUploadBuffer(");
  const abortWs = functionBody(source, "void CrossPointWebServer::abortWsUpload(");
  const stop = functionBody(source, "void CrossPointWebServer::stop(");
  const http = functionBody(source, "void CrossPointWebServer::handleUpload(");
  const post = functionBody(source, "void CrossPointWebServer::handleUploadPost(");
  const ws = functionBody(source, "void CrossPointWebServer::onWebSocketEvent(");

  return String.raw`
#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

static std::vector<std::string> sinkTrace;
static unsigned long fakeMillis = 0;

static void record(const std::string& value) { sinkTrace.push_back(value); }

template <typename... Args>
static void logNoop(Args&&...) {}

#define LOG_DBG(...) logNoop(__VA_ARGS__)
#define LOG_ERR(...) logNoop(__VA_ARGS__)

static void esp_task_wdt_reset() {}
static void delay(unsigned long) {}
static unsigned long millis() { return ++fakeMillis; }

class String {
 public:
  String() = default;
  String(const char* value) : value_(value == nullptr ? "" : value) {}
  String(char* value) : String(static_cast<const char*>(value)) {}
  String(const std::string& value) : value_(value) {}
  String(const size_t value) : value_(std::to_string(value)) {}
  String(const uint32_t value) : value_(std::to_string(value)) {}
  String(const long value) : value_(std::to_string(value)) {}

  const char* c_str() const { return value_.c_str(); }
  bool isEmpty() const { return value_.empty(); }
  size_t length() const { return value_.size(); }
  char operator[](const size_t index) const { return value_[index]; }
  char charAt(const int index) const { return value_[static_cast<size_t>(index)]; }
  void clear() { value_.clear(); }
  bool startsWith(const char* prefix) const { return value_.rfind(prefix, 0) == 0; }
  bool equals(const char* other) const { return value_ == other; }
  bool endsWith(const char* suffix) const {
    const std::string wanted(suffix);
    return value_.size() >= wanted.size() &&
           value_.compare(value_.size() - wanted.size(), wanted.size(), wanted) == 0;
  }
  int indexOf(const char needle, const int from = 0) const {
    if (from < 0) return -1;
    const size_t found = value_.find(needle, static_cast<size_t>(from));
    return found == std::string::npos ? -1 : static_cast<int>(found);
  }
  String substring(const int start) const {
    return substring(start, static_cast<int>(value_.size()));
  }
  String substring(const int start, const int end) const {
    if (start < 0 || end < start || static_cast<size_t>(start) >= value_.size()) return String();
    return String(value_.substr(static_cast<size_t>(start), static_cast<size_t>(end - start)));
  }
  ${toIntImplementation}
  String& operator+=(const String& other) {
    value_ += other.value_;
    return *this;
  }
  String& operator+=(const char* other) {
    value_ += other;
    return *this;
  }
  bool operator==(const char* other) const { return value_ == other; }
  const std::string& stdString() const { return value_; }

 private:
  std::string value_;
};

static String operator+(const String& left, const String& right) {
  return String(left.stdString() + right.stdString());
}
static String operator+(const char* left, const String& right) {
  return String(std::string(left) + right.stdString());
}
static String operator+(const String& left, const char* right) {
  return String(left.stdString() + std::string(right));
}

struct FakeStorage;

struct HalFile {
  FakeStorage* storage = nullptr;
  std::string path;
  bool open = false;

  explicit operator bool() const { return open; }
  size_t write(const uint8_t* data, size_t length);
  void close();
};

struct FakeStorage {
  std::map<std::string, std::vector<uint8_t>> files;
  size_t nextWriteLimit = std::numeric_limits<size_t>::max();

  bool exists(const char* path) {
    record(std::string("exists:") + path);
    return files.count(path) != 0;
  }
  bool remove(const char* path) {
    record(std::string("remove:") + path);
    return files.erase(path) != 0;
  }
  bool openFileForWrite(const char* tag, const String& path, HalFile& file) {
    return openFileForWrite(tag, path.c_str(), file);
  }
  bool openFileForWrite(const char* tag, const char* path, HalFile& file) {
    record(std::string("open:") + tag + ":" + path);
    files[path].clear();
    file.storage = this;
    file.path = path;
    file.open = true;
    return true;
  }
};

static FakeStorage Storage;

size_t HalFile::write(const uint8_t* data, const size_t length) {
  if (!open || storage == nullptr) return 0;
  const size_t written = std::min(length, storage->nextWriteLimit);
  storage->nextWriteLimit = std::numeric_limits<size_t>::max();
  auto& bytes = storage->files[path];
  bytes.insert(bytes.end(), data, data + written);
  record("write:" + path + ":" + std::to_string(length) + ":" + std::to_string(written));
  return written;
}

void HalFile::close() {
  if (!open) return;
  record("close:" + path);
  open = false;
}

namespace BookUpload {
enum class AtomicUploadStatus {
  Ok,
  Busy,
  PathTooLong,
  RecoveryFailed,
  OpenWriteFailed,
  SizeMismatch,
  WriteFailed,
  FlushFailed,
  SyncFailed,
  CloseFailed,
  OpenReadFailed,
  ReadFailed,
  DigestFailed,
  VerificationFailed,
  BackupFailed,
  MarkerFailed,
  PromotionFailed,
  CommitHookFailed,
  CleanupFailed,
  NotActive,
  InvalidArgument,
};

struct AtomicUploadState { bool active = false; };
struct AtomicUploadIo { HalFile* file = nullptr; };
struct AtomicUploadCommitHook {
  void* context = nullptr;
  bool (*run)(void*, const char*) = nullptr;
};
struct UploadTransportResponse { bool success; bool hasError; };
constexpr size_t kUnknownUploadSize = std::numeric_limits<size_t>::max();

static bool isActive(const AtomicUploadState& state) { return state.active; }
static AtomicUploadStatus admitTransportStart(const bool active, UploadTransportResponse& response) {
  if (!active) return AtomicUploadStatus::Ok;
  response.success = false;
  return AtomicUploadStatus::Busy;
}
static int httpResponseStatus(const UploadTransportResponse& response) {
  return response.success && !response.hasError ? 200 : 400;
}
static AtomicUploadStatus begin(AtomicUploadState& state, const AtomicUploadIo& io,
                                const char* path, const size_t expected) {
  record(std::string("atomic.begin:") + path + ":" + std::to_string(expected));
  state.active = true;
  if (io.file != nullptr) {
    io.file->storage = &Storage;
    io.file->path = std::string(path) + ".atomic";
    io.file->open = true;
  }
  return AtomicUploadStatus::Ok;
}
static AtomicUploadStatus write(AtomicUploadState&, const AtomicUploadIo&, const uint8_t*,
                                const size_t length) {
  record("atomic.write:" + std::to_string(length));
  return AtomicUploadStatus::Ok;
}
static AtomicUploadStatus abort(AtomicUploadState& state, const AtomicUploadIo& io) {
  record("atomic.abort");
  state.active = false;
  if (io.file != nullptr) io.file->open = false;
  return AtomicUploadStatus::Ok;
}
static AtomicUploadStatus finish(AtomicUploadState& state, const AtomicUploadIo& io,
                                 const size_t expected, uint8_t*, size_t,
                                 const AtomicUploadCommitHook hook) {
  record("atomic.finish:" + std::to_string(expected));
  state.active = false;
  if (io.file != nullptr) io.file->open = false;
  const std::string target = io.file == nullptr ? "" : io.file->path.substr(0, io.file->path.size() - 7);
  if (hook.run != nullptr && !hook.run(hook.context, target.c_str())) {
    return AtomicUploadStatus::CommitHookFailed;
  }
  return AtomicUploadStatus::Ok;
}
}  // namespace BookUpload

struct FakeEsp {
  int getFreeHeap() const { return 300000; }
};
static FakeEsp ESP;

enum HTTPUploadStatus {
  UPLOAD_FILE_START,
  UPLOAD_FILE_WRITE,
  UPLOAD_FILE_END,
  UPLOAD_FILE_ABORTED,
};
struct HTTPUpload {
  HTTPUploadStatus status = UPLOAD_FILE_START;
  String filename;
  uint8_t* buf = nullptr;
  size_t currentSize = 0;
};

struct FakeHttpServer {
  HTTPUpload request;
  std::map<std::string, String> args;

  HTTPUpload& upload() { return request; }
  bool hasArg(const char* name) const { return args.count(name) != 0; }
  String arg(const char* name) const {
    const auto found = args.find(name);
    return found == args.end() ? String() : found->second;
  }
  void send(const int status, const char* contentType, const String& body) {
    record("http:" + std::to_string(status) + ":" + contentType + ":" + body.stdString());
  }
  void stop() { record("http.stop"); }
};

struct FakeWsServer {
  void sendTXT(const uint8_t client, const char* message) {
    record("ws:" + std::to_string(client) + ":" + message);
  }
  void sendTXT(const uint8_t client, const String& message) {
    sendTXT(client, message.c_str());
  }
  void close() { record("ws.close"); }
};

struct FakeUdp { void stop() { record("udp.stop"); } };

enum WStype_t { WStype_DISCONNECTED, WStype_CONNECTED, WStype_TEXT, WStype_BIN };

class CrossPointWebServer {
 public:
  struct UploadState {
    enum class Owner : uint8_t { None, Http, WebSocket };
    enum class HttpPostStatus : uint8_t { UploadResult, Busy };
    HalFile file;
    BookUpload::AtomicUploadState transaction;
    Owner owner = Owner::None;
    HttpPostStatus httpPostStatus = HttpPostStatus::UploadResult;
    String fileName;
    String path = "/";
    size_t size = 0;
    bool success = false;
    String error;
    static constexpr size_t UPLOAD_BUFFER_SIZE = 4096;
    std::vector<uint8_t> buffer = std::vector<uint8_t>(UPLOAD_BUFFER_SIZE);
    size_t bufferPos = 0;
  } upload;

  std::shared_ptr<FakeHttpServer> server = std::make_shared<FakeHttpServer>();
  std::shared_ptr<FakeWsServer> wsServer = std::make_shared<FakeWsServer>();
  bool running = true;
  FakeUdp udp;
  bool udpActive = false;

  void abortWsUpload(const char* tag);
  void stop();
  void handleUpload(UploadState& state) const;
  void handleUploadPost(UploadState& state) const;
  void onWebSocketEvent(uint8_t num, WStype_t type, uint8_t* payload, size_t length);
};

static CrossPointWebServer* wsInstance = nullptr;
static HalFile wsUploadFile;
static String wsUploadFileName;
static String wsUploadPath;
static ${uploadSizeType} wsUploadSize = 0;
static ${uploadSizeType} wsUploadReceived = 0;
static unsigned long wsUploadStartTime = 0;
static bool wsUploadInProgress = false;
static uint8_t wsUploadClientNum = 255;
static ${uploadSizeType} wsLastProgressSent = 0;
static String wsLastCompleteName;
static ${uploadSizeType} wsLastCompleteSize = 0;
static unsigned long wsLastCompleteAt = 0;
static unsigned long uploadStartTime = 0;
static unsigned long totalWriteTime = 0;
static size_t writeCount = 0;

namespace StringUtils {
static std::string sanitizeFilename(const char* input) {
  const std::string value(input == nullptr ? "" : input);
  const size_t slash = value.find_last_of("/\\");
  return slash == std::string::npos ? value : value.substr(slash + 1);
}
}  // namespace StringUtils

namespace FsHelpers {
static bool hasPdfExtension(const std::string_view input) {
  std::string value(input);
  std::transform(value.begin(), value.end(), value.begin(),
                 [](const unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
  return value.size() >= 4 && value.compare(value.size() - 4, 4, ".pdf") == 0;
}
static bool hasPdfExtension(const String& input) {
  return hasPdfExtension(std::string_view{input.c_str(), input.length()});
}
}  // namespace FsHelpers

static String normalizeWebPath(const String& input) {
  if (input.isEmpty() || input == "/") return String("/");
  String result = input;
  if (!result.startsWith("/")) result = String("/") + result;
  if (result.length() > 1 && result.endsWith("/")) {
    result = result.substring(0, static_cast<int>(result.length() - 1));
  }
  return result;
}
struct FakeSettings { bool showHiddenFiles = false; };
static FakeSettings SETTINGS;
static constexpr const char* HIDDEN_ITEMS[] = {"System Volume Information", "XTCache"};
${protectedGate}
static bool clearBookCachePreservingUserState(const char* path) {
  record(std::string("cache:") + path);
  return true;
}

static BookUpload::AtomicUploadIo atomicUploadIo(HalFile& file) { return {&file}; }
static bool invalidateUploadedBookCache(void*, const char* path) {
  return clearBookCachePreservingUserState(path);
}
static BookUpload::AtomicUploadCommitHook uploadCommitHook() {
  return {nullptr, &invalidateUploadedBookCache};
}
static const char* atomicUploadStatusMessage(const BookUpload::AtomicUploadStatus status) {
  switch (status) {
    case BookUpload::AtomicUploadStatus::Ok: return "";
    case BookUpload::AtomicUploadStatus::Busy: return "Upload already in progress";
    case BookUpload::AtomicUploadStatus::WriteFailed: return "Failed to write to SD card - disk may be full";
    case BookUpload::AtomicUploadStatus::CommitHookFailed: return "Failed to invalidate derived book cache";
    default: return "Invalid upload state";
  }
}

${parseSize}

${pdfGate}

${flush}

${abortWs}

${stop}

${http}

${post}

${ws}

static void resetHarness() {
  sinkTrace.clear();
  fakeMillis = 0;
  Storage = FakeStorage();
  wsUploadFile = HalFile();
  wsUploadFileName = String();
  wsUploadPath = String();
  wsUploadSize = 0;
  wsUploadReceived = 0;
  wsUploadStartTime = 0;
  wsUploadInProgress = false;
  wsUploadClientNum = 255;
  wsLastProgressSent = 0;
  wsLastCompleteName = String();
  wsLastCompleteSize = 0;
  wsLastCompleteAt = 0;
  uploadStartTime = 0;
  totalWriteTime = 0;
  writeCount = 0;
}

static std::string joinedTrace() {
  std::ostringstream output;
  for (size_t index = 0; index < sinkTrace.size(); ++index) {
    if (index != 0) output << '~';
    output << sinkTrace[index];
  }
  return output.str();
}

static size_t storedSize(const std::string& path) {
  const auto found = Storage.files.find(path);
  return found == Storage.files.end() ? 0 : found->second.size();
}

static bool storedEquals(const std::string& path, const std::vector<uint8_t>& expected) {
  const auto found = Storage.files.find(path);
  return found != Storage.files.end() && found->second == expected;
}

static void emit(const std::string& name, const CrossPointWebServer* web = nullptr,
                 const CrossPointWebServer::UploadState* state = nullptr,
                 const std::string& path = "") {
  std::cout << name << '|' << joinedTrace();
  if (state != nullptr) {
    std::cout << "|success=" << (state->success ? 1 : 0) << "|error=" << state->error.c_str()
              << "|size=" << state->size;
  }
  if (web != nullptr) {
    std::cout << "|wsSize=" << wsUploadSize << "|received=" << wsUploadReceived
              << "|inProgress=" << (wsUploadInProgress ? 1 : 0);
  }
  if (!path.empty()) std::cout << "|stored=" << storedSize(path);
  std::cout << '\n';
}

static std::string bookPath(const std::string& extension) {
  return "/books/book" + extension;
}

static void httpEvent(CrossPointWebServer& web, CrossPointWebServer::UploadState& state,
                      const HTTPUploadStatus status, const std::string& filename,
                      std::vector<uint8_t>& bytes) {
  web.server->request.status = status;
  web.server->request.filename = filename.c_str();
  web.server->request.buf = bytes.empty() ? nullptr : bytes.data();
  web.server->request.currentSize = bytes.size();
  web.handleUpload(state);
}

static void wsText(CrossPointWebServer& web, const std::string& message) {
  std::vector<uint8_t> bytes(message.begin(), message.end());
  bytes.push_back(0);
  web.onWebSocketEvent(7, WStype_TEXT, bytes.data(), message.size());
}

static void wsBinary(CrossPointWebServer& web, std::vector<uint8_t>& bytes) {
  web.onWebSocketEvent(7, WStype_BIN, bytes.data(), bytes.size());
}

static void runLegacyExtension(const std::string& extension) {
  const std::string filename = "book" + extension;
  const std::string path = bookPath(extension);
  std::vector<uint8_t> bytes{1, 2, 3};
  std::vector<uint8_t> httpBytes(4099, 7);

  resetHarness();
  Storage.files[path] = {9};
  CrossPointWebServer httpSuccess;
  httpSuccess.server->args["path"] = "/books";
  httpEvent(httpSuccess, httpSuccess.upload, UPLOAD_FILE_START, filename, httpBytes);
  httpEvent(httpSuccess, httpSuccess.upload, UPLOAD_FILE_WRITE, filename, httpBytes);
  std::vector<uint8_t> empty;
  httpEvent(httpSuccess, httpSuccess.upload, UPLOAD_FILE_END, filename, empty);
  httpSuccess.handleUploadPost(httpSuccess.upload);
  emit("legacy/http-success/" + extension, &httpSuccess, &httpSuccess.upload, path);

  resetHarness();
  Storage.files[path] = {9};
  CrossPointWebServer httpShort;
  httpShort.server->args["path"] = "/books";
  httpEvent(httpShort, httpShort.upload, UPLOAD_FILE_START, filename, bytes);
  httpEvent(httpShort, httpShort.upload, UPLOAD_FILE_WRITE, filename, bytes);
  Storage.nextWriteLimit = 2;
  httpEvent(httpShort, httpShort.upload, UPLOAD_FILE_END, filename, empty);
  httpShort.handleUploadPost(httpShort.upload);
  emit("legacy/http-final-short/" + extension, &httpShort, &httpShort.upload, path);

  resetHarness();
  Storage.files[path] = {9};
  CrossPointWebServer httpAbort;
  httpAbort.server->args["path"] = "/books";
  httpEvent(httpAbort, httpAbort.upload, UPLOAD_FILE_START, filename, bytes);
  httpEvent(httpAbort, httpAbort.upload, UPLOAD_FILE_WRITE, filename, bytes);
  httpEvent(httpAbort, httpAbort.upload, UPLOAD_FILE_ABORTED, filename, empty);
  httpAbort.handleUploadPost(httpAbort.upload);
  emit("legacy/http-abort/" + extension, &httpAbort, &httpAbort.upload, path);

  resetHarness();
  CrossPointWebServer stalePost;
  stalePost.upload.fileName = filename.c_str();
  stalePost.upload.success = true;
  stalePost.upload.error = "stale error";
  stalePost.handleUploadPost(stalePost.upload);
  emit("legacy/http-stale-post/" + extension, &stalePost, &stalePost.upload);

  resetHarness();
  Storage.files[path] = {9};
  CrossPointWebServer wsSuccess;
  wsText(wsSuccess, "START:" + filename + ":3:/books");
  wsBinary(wsSuccess, bytes);
  emit("legacy/ws-success/" + extension, &wsSuccess, nullptr, path);

  resetHarness();
  Storage.files[path] = {9};
  CrossPointWebServer wsShort;
  wsText(wsShort, "START:" + filename + ":3:/books");
  Storage.nextWriteLimit = 2;
  wsBinary(wsShort, bytes);
  emit("legacy/ws-short/" + extension, &wsShort, nullptr, path);

  resetHarness();
  Storage.files[path] = {9};
  CrossPointWebServer wsOverflow;
  wsText(wsOverflow, "START:" + filename + ":3:/books");
  std::vector<uint8_t> overflow{1, 2, 3, 4};
  wsBinary(wsOverflow, overflow);
  emit("legacy/ws-overflow/" + extension, &wsOverflow, nullptr, path);

  resetHarness();
  Storage.files[path] = {9};
  CrossPointWebServer wsDisconnect;
  wsText(wsDisconnect, "START:" + filename + ":3:/books");
  wsDisconnect.onWebSocketEvent(7, WStype_DISCONNECTED, nullptr, 0);
  emit("legacy/ws-disconnect/" + extension, &wsDisconnect, nullptr, path);

  resetHarness();
  Storage.files[path] = {9};
  CrossPointWebServer wsZero;
  wsText(wsZero, "START:" + filename + ":0:/books");
  emit("legacy/ws-zero/" + extension, &wsZero, nullptr, path);

  std::ostringstream integerBoundaries;
  for (const char* token : {"2147483647", "2147483648", "4294967295", "4294967296"}) {
    resetHarness();
    CrossPointWebServer boundary;
    wsText(boundary, "START:" + filename + ":" + token + ":/books");
    if (integerBoundaries.tellp() > 0) integerBoundaries << "~~";
    integerBoundaries << token << '{' << joinedTrace() << "|wsSize=" << wsUploadSize << '}';
  }
  std::cout << "legacy/ws-to-int/" << extension << '|' << integerBoundaries.str() << '\n';
}

static void runPdfPolicy() {
  const std::string path = "/books/book.pdf";
  std::vector<uint8_t> bytes{1, 2, 3};

  resetHarness();
  Storage.files[path] = {9};
  CrossPointWebServer ws;
  wsText(ws, "START:book.pdf:3:/books");
  wsBinary(ws, bytes);
  emit("pdf/ws-success", &ws, nullptr, path);

  for (const char* token : {"2147483647", "2147483648", "4294967295", "4294967296"}) {
    resetHarness();
    CrossPointWebServer boundary;
    wsText(boundary, std::string("START:book.pdf:") + token + ":/books");
    emit(std::string("pdf/ws-size-") + token, &boundary, nullptr, path);
  }

  resetHarness();
  Storage.files[path] = {9};
  CrossPointWebServer http;
  http.server->args["path"] = "/books";
  httpEvent(http, http.upload, UPLOAD_FILE_START, "book.pdf", bytes);
  httpEvent(http, http.upload, UPLOAD_FILE_WRITE, "book.pdf", bytes);
  std::vector<uint8_t> empty;
  httpEvent(http, http.upload, UPLOAD_FILE_END, "book.pdf", empty);
  http.handleUploadPost(http.upload);
  emit("pdf/http-success", &http, &http.upload, path);
}

static void runUploadConcurrencyPolicy() {
  resetHarness();
  CrossPointWebServer concurrent;
  concurrent.server->args["path"] = "/http-books";
  std::vector<uint8_t> httpBytes{1, 2, 3, 4};
  std::vector<uint8_t> wsBytes{5, 6, 7};
  std::vector<uint8_t> empty;
  httpEvent(concurrent, concurrent.upload, UPLOAD_FILE_START, "http.epub", httpBytes);
  httpEvent(concurrent, concurrent.upload, UPLOAD_FILE_WRITE, "http.epub", httpBytes);
  wsText(concurrent, "START:socket.txt:3:/ws-books");
  wsBinary(concurrent, wsBytes);
  httpEvent(concurrent, concurrent.upload, UPLOAD_FILE_END, "http.epub", empty);
  std::cout << "concurrency/legacy-http-ws|" << joinedTrace()
            << "|httpExact=" << (storedEquals("/http-books/http.epub", httpBytes) ? 1 : 0)
            << "|wsExact=" << (storedEquals("/ws-books/socket.txt", wsBytes) ? 1 : 0)
            << "|httpName=" << concurrent.upload.fileName.c_str()
            << "|httpPath=" << concurrent.upload.path.c_str()
            << "|httpSize=" << concurrent.upload.size
            << "|httpSuccess=" << (concurrent.upload.success ? 1 : 0) << '\n';

  resetHarness();
  CrossPointWebServer restart;
  restart.server->args["path"] = "/original";
  std::vector<uint8_t> pdfBytes{9, 8, 7};
  httpEvent(restart, restart.upload, UPLOAD_FILE_START, "active.pdf", pdfBytes);
  restart.server->args["path"] = "/replacement";
  httpEvent(restart, restart.upload, UPLOAD_FILE_START, "replacement.epub", pdfBytes);
  restart.server->args["path"] = "/original";
  httpEvent(restart, restart.upload, UPLOAD_FILE_WRITE, "active.pdf", pdfBytes);
  httpEvent(restart, restart.upload, UPLOAD_FILE_END, "active.pdf", empty);
  std::cout << "concurrency/http-restart-preserves-active-pdf|" << joinedTrace()
            << "|name=" << restart.upload.fileName.c_str()
            << "|path=" << restart.upload.path.c_str()
            << "|size=" << restart.upload.size
            << "|success=" << (restart.upload.success ? 1 : 0)
            << "|error=" << restart.upload.error.c_str()
            << "|owner=" << static_cast<int>(restart.upload.owner)
            << "|atomicActive=" << (BookUpload::isActive(restart.upload.transaction) ? 1 : 0) << '\n';

  resetHarness();
  CrossPointWebServer busyPost;
  busyPost.server->args["path"] = "/prior";
  httpEvent(busyPost, busyPost.upload, UPLOAD_FILE_START, "prior.pdf", pdfBytes);
  httpEvent(busyPost, busyPost.upload, UPLOAD_FILE_WRITE, "prior.pdf", pdfBytes);
  httpEvent(busyPost, busyPost.upload, UPLOAD_FILE_END, "prior.pdf", empty);
  wsText(busyPost, "START:active.pdf:3:/pdf-ws");
  busyPost.server->args["path"] = "/rejected";
  httpEvent(busyPost, busyPost.upload, UPLOAD_FILE_START, "rejected.pdf", pdfBytes);
  busyPost.handleUploadPost(busyPost.upload);
  const String busyActiveName = wsUploadFileName;
  const String busyActivePath = wsUploadPath;
  const auto busyActiveSize = wsUploadSize;
  const bool busyActiveInProgress = wsUploadInProgress;
  const uint8_t busyActiveClient = wsUploadClientNum;
  const int busyActiveOwner = static_cast<int>(busyPost.upload.owner);
  const bool busyActiveAtomic = BookUpload::isActive(busyPost.upload.transaction);
  wsBinary(busyPost, pdfBytes);
  std::cout << "concurrency/http-busy-post-is-request-scoped|" << joinedTrace()
            << "|activeName=" << busyActiveName.c_str()
            << "|activePath=" << busyActivePath.c_str()
            << "|activeSize=" << busyActiveSize
            << "|activeInProgress=" << (busyActiveInProgress ? 1 : 0)
            << "|activeClient=" << static_cast<int>(busyActiveClient)
            << "|activeOwner=" << busyActiveOwner
            << "|activeAtomic=" << (busyActiveAtomic ? 1 : 0)
            << "|afterOwner=" << static_cast<int>(busyPost.upload.owner)
            << "|afterAtomic=" << (BookUpload::isActive(busyPost.upload.transaction) ? 1 : 0) << '\n';

  resetHarness();
  CrossPointWebServer httpPdf;
  httpPdf.server->args["path"] = "/pdf-http";
  wsUploadFileName = "prior.txt";
  wsUploadPath = "/prior-ws";
  wsUploadSize = 11;
  wsUploadReceived = 4;
  httpEvent(httpPdf, httpPdf.upload, UPLOAD_FILE_START, "active.pdf", pdfBytes);
  wsText(httpPdf, "START:blocked.txt:3:/blocked-ws");
  httpEvent(httpPdf, httpPdf.upload, UPLOAD_FILE_WRITE, "active.pdf", pdfBytes);
  httpEvent(httpPdf, httpPdf.upload, UPLOAD_FILE_END, "active.pdf", empty);
  std::cout << "concurrency/http-pdf-blocks-ws-legacy|" << joinedTrace()
            << "|httpName=" << httpPdf.upload.fileName.c_str()
            << "|httpPath=" << httpPdf.upload.path.c_str()
            << "|httpSize=" << httpPdf.upload.size
            << "|httpSuccess=" << (httpPdf.upload.success ? 1 : 0)
            << "|wsName=" << wsUploadFileName.c_str()
            << "|wsPath=" << wsUploadPath.c_str()
            << "|wsSize=" << wsUploadSize
            << "|wsReceived=" << wsUploadReceived
            << "|atomicActive=" << (BookUpload::isActive(httpPdf.upload.transaction) ? 1 : 0) << '\n';

  resetHarness();
  CrossPointWebServer wsPdf;
  wsPdf.upload.fileName = "prior-http.txt";
  wsPdf.upload.path = "/prior-http";
  wsPdf.upload.size = 17;
  wsPdf.upload.success = true;
  wsText(wsPdf, "START:active.pdf:3:/pdf-ws");
  wsPdf.server->args["path"] = "/blocked-http";
  httpEvent(wsPdf, wsPdf.upload, UPLOAD_FILE_START, "blocked.epub", pdfBytes);
  wsBinary(wsPdf, pdfBytes);
  std::cout << "concurrency/ws-pdf-blocks-http-legacy|" << joinedTrace()
            << "|httpName=" << wsPdf.upload.fileName.c_str()
            << "|httpPath=" << wsPdf.upload.path.c_str()
            << "|httpSize=" << wsPdf.upload.size
            << "|httpSuccess=" << (wsPdf.upload.success ? 1 : 0)
            << "|httpError=" << wsPdf.upload.error.c_str()
            << "|wsName=" << wsUploadFileName.c_str()
            << "|wsPath=" << wsUploadPath.c_str()
            << "|wsSize=" << wsUploadSize
            << "|atomicActive=" << (BookUpload::isActive(wsPdf.upload.transaction) ? 1 : 0) << '\n';

  resetHarness();
  CrossPointWebServer protectedRestart;
  wsText(protectedRestart, "START:active.pdf:3:/pdf-ws");
  wsText(protectedRestart, "START:replacement.epub:2:/.crosspoint");
  const String continuedName = wsUploadFileName;
  const String continuedPath = wsUploadPath;
  const auto continuedSize = wsUploadSize;
  const auto continuedReceived = wsUploadReceived;
  const bool continuedInProgress = wsUploadInProgress;
  const uint8_t continuedClient = wsUploadClientNum;
  const int continuedOwner = static_cast<int>(protectedRestart.upload.owner);
  const bool continuedAtomic = BookUpload::isActive(protectedRestart.upload.transaction);
  wsBinary(protectedRestart, pdfBytes);
  std::cout << "concurrency/ws-pdf-protected-restart-continues|" << joinedTrace()
            << "|beforeName=" << continuedName.c_str()
            << "|beforePath=" << continuedPath.c_str()
            << "|beforeSize=" << continuedSize
            << "|beforeReceived=" << continuedReceived
            << "|beforeInProgress=" << (continuedInProgress ? 1 : 0)
            << "|beforeClient=" << static_cast<int>(continuedClient)
            << "|beforeOwner=" << continuedOwner
            << "|beforeAtomic=" << (continuedAtomic ? 1 : 0)
            << "|afterName=" << wsUploadFileName.c_str()
            << "|afterPath=" << wsUploadPath.c_str()
            << "|afterSize=" << wsUploadSize
            << "|afterReceived=" << wsUploadReceived
            << "|afterInProgress=" << (wsUploadInProgress ? 1 : 0)
            << "|afterClient=" << static_cast<int>(wsUploadClientNum)
            << "|afterOwner=" << static_cast<int>(protectedRestart.upload.owner)
            << "|afterAtomic=" << (BookUpload::isActive(protectedRestart.upload.transaction) ? 1 : 0) << '\n';

  resetHarness();
  CrossPointWebServer protectedAbort;
  wsText(protectedAbort, "START:active.pdf:3:/pdf-ws");
  wsText(protectedAbort, "START:replacement.epub:2:/.crosspoint");
  protectedAbort.onWebSocketEvent(7, WStype_DISCONNECTED, nullptr, 0);
  std::cout << "concurrency/ws-pdf-protected-restart-aborts|" << joinedTrace()
            << "|name=" << wsUploadFileName.c_str()
            << "|path=" << wsUploadPath.c_str()
            << "|size=" << wsUploadSize
            << "|received=" << wsUploadReceived
            << "|inProgress=" << (wsUploadInProgress ? 1 : 0)
            << "|client=" << static_cast<int>(wsUploadClientNum)
            << "|owner=" << static_cast<int>(protectedAbort.upload.owner)
            << "|atomic=" << (BookUpload::isActive(protectedAbort.upload.transaction) ? 1 : 0) << '\n';

  resetHarness();
  CrossPointWebServer stopLegacy;
  stopLegacy.server->args["path"] = "/stop-books";
  std::vector<uint8_t> stopBytes{4, 5, 6};
  Storage.files["/stop-books/partial.epub"] = {9};
  httpEvent(stopLegacy, stopLegacy.upload, UPLOAD_FILE_START, "partial.epub", stopBytes);
  httpEvent(stopLegacy, stopLegacy.upload, UPLOAD_FILE_WRITE, "partial.epub", stopBytes);
  stopLegacy.stop();
  const bool stoppedTargetRemoved = Storage.files.count("/stop-books/partial.epub") == 0;
  const bool stoppedFileClosed = !static_cast<bool>(stopLegacy.upload.file);
  const auto stoppedOwner = static_cast<int>(stopLegacy.upload.owner);
  const size_t stoppedBufferPos = stopLegacy.upload.bufferPos;
  const String stoppedName = stopLegacy.upload.fileName;
  const String stoppedPath = stopLegacy.upload.path;
  const size_t stoppedSize = stopLegacy.upload.size;
  const bool stoppedSuccess = stopLegacy.upload.success;
  const String stoppedError = stopLegacy.upload.error;
  stopLegacy.server = std::make_shared<FakeHttpServer>();
  stopLegacy.wsServer = std::make_shared<FakeWsServer>();
  stopLegacy.running = true;
  stopLegacy.server->args["path"] = "/stop-books";
  httpEvent(stopLegacy, stopLegacy.upload, UPLOAD_FILE_START, "partial.epub", stopBytes);
  httpEvent(stopLegacy, stopLegacy.upload, UPLOAD_FILE_WRITE, "partial.epub", stopBytes);
  httpEvent(stopLegacy, stopLegacy.upload, UPLOAD_FILE_END, "partial.epub", empty);
  stopLegacy.handleUploadPost(stopLegacy.upload);
  std::cout << "concurrency/stop-cleans-legacy-http-and-reuses|" << joinedTrace()
            << "|stoppedRemoved=" << (stoppedTargetRemoved ? 1 : 0)
            << "|stoppedClosed=" << (stoppedFileClosed ? 1 : 0)
            << "|stoppedOwner=" << stoppedOwner
            << "|stoppedBuffer=" << stoppedBufferPos
            << "|stoppedName=" << stoppedName.c_str()
            << "|stoppedPath=" << stoppedPath.c_str()
            << "|stoppedSize=" << stoppedSize
            << "|stoppedSuccess=" << (stoppedSuccess ? 1 : 0)
            << "|stoppedError=" << stoppedError.c_str()
            << "|reusedExact=" << (storedEquals("/stop-books/partial.epub", stopBytes) ? 1 : 0)
            << "|reusedOwner=" << static_cast<int>(stopLegacy.upload.owner)
            << "|reusedSuccess=" << (stopLegacy.upload.success ? 1 : 0) << '\n';
}

int main() {
  for (const char* extension : {".epub", ".xtc", ".xtch", ".txt", ".md", ".markdown"}) {
    runLegacyExtension(extension);
  }
  runPdfPolicy();
  runUploadConcurrencyPolicy();
  return 0;
}
`;
}

function runProcess(command, args, options = {}) {
  if (forbidGit && path.basename(command).toLowerCase().startsWith("git")) {
    throw new Error(`git invocation forbidden by executable-oracle test: ${command}`);
  }
  const result = childProcess.spawnSync(command, args, {
    cwd: repoRoot,
    encoding: "utf8",
    maxBuffer: 16 * 1024 * 1024,
    ...options,
  });
  if (result.error) throw result.error;
  return result;
}

function compilerInvocation(tempDir, outputName) {
  const flags = [
    "-std=c++17",
    "-Wall",
    "-Wextra",
    "-pedantic",
    "-O1",
    "/work/harness.cpp",
    "-o",
    `/work/${outputName}`,
  ];
  if (sanitize) flags.splice(4, 0, "-fsanitize=address,undefined", "-fno-omit-frame-pointer");
  if (process.platform === "win32") {
    return {
      command: "docker",
      args: [
        "run",
        "--rm",
        "--mount",
        `type=bind,source=${tempDir},target=/work`,
        "gcc:14-bookworm",
        "g++",
        ...flags,
      ],
    };
  }
  return { command: process.env.CXX || "c++", args: flags.map((flag) => flag.replace("/work/", `${tempDir}/`)) };
}

function executableInvocation(tempDir, outputName) {
  const sanitizerEnvironment = sanitize
    ? ["-e", "ASAN_OPTIONS=detect_leaks=1:halt_on_error=1", "-e", "UBSAN_OPTIONS=halt_on_error=1"]
    : [];
  if (process.platform === "win32") {
    return {
      command: "docker",
      args: [
        "run",
        "--rm",
        ...sanitizerEnvironment,
        "--mount",
        `type=bind,source=${tempDir},target=/work`,
        "gcc:14-bookworm",
        `/work/${outputName}`,
      ],
    };
  }
  return {
    command: path.join(tempDir, outputName),
    args: [],
    options: {
      env: {
        ...process.env,
        ...(sanitize
          ? { ASAN_OPTIONS: "detect_leaks=1:halt_on_error=1", UBSAN_OPTIONS: "halt_on_error=1" }
          : {}),
      },
    },
  };
}

function compileAndRun(source, label, harnessOptions = {}) {
  const tempDir = fs.mkdtempSync(path.join(os.tmpdir(), `crossink-upload-${label}-`));
  try {
    fs.writeFileSync(path.join(tempDir, "harness.cpp"), generatedHarness(source, harnessOptions));
    const outputName = "harness";
    const compiler = compilerInvocation(tempDir, outputName);
    const compiled = runProcess(compiler.command, compiler.args);
    if (compiled.status !== 0) {
      throw new Error(`${label}: extracted handler harness did not compile\n${compiled.stdout}\n${compiled.stderr}`);
    }
    const executable = executableInvocation(tempDir, outputName);
    const ran = runProcess(executable.command, executable.args, executable.options);
    if (ran.status !== 0) {
      throw new Error(`${label}: extracted handler harness failed\n${ran.stdout}\n${ran.stderr}`);
    }
    return normalized(ran.stdout).trim().split("\n");
  } finally {
    fs.rmSync(tempDir, { recursive: true, force: true });
  }
}

function compileFsHelpersBoundary(source, label) {
  const tempDir = fs.mkdtempSync(path.join(os.tmpdir(), `crossink-fshelpers-${label}-`));
  try {
    fs.copyFileSync(path.join(repoRoot, "lib/FsHelpers/FsHelpers.h"), path.join(tempDir, "FsHelpers.h"));
    fs.copyFileSync(path.join(repoRoot, "lib/FsHelpers/NaturalSort.h"), path.join(tempDir, "NaturalSort.h"));
    fs.writeFileSync(
      path.join(tempDir, "WString.h"),
      String.raw`#pragma once
#include <cstddef>
class String {
 public:
  String(const char* value);
  const char* c_str() const;
  std::size_t length() const;
};
`,
    );
    fs.writeFileSync(
      path.join(tempDir, "probe.cpp"),
      `#include "FsHelpers.h"\n\n${functionBody(source, "isPdfUploadTarget(")}\n`,
    );
    const flags = [
      "-std=c++17",
      "-Wall",
      "-Wextra",
      "-Werror",
      "-pedantic",
      "-I/work",
      "-c",
      "/work/probe.cpp",
      "-o",
      "/work/probe.o",
    ];
    if (process.platform === "win32") {
      return runProcess("docker", [
        "run",
        "--rm",
        "--mount",
        `type=bind,source=${tempDir},target=/work`,
        "gcc:14-bookworm",
        "g++",
        ...flags,
      ]);
    }
    return runProcess(
      process.env.CXX || "c++",
      flags.map((flag) => (flag === "-I/work" ? `-I${tempDir}` : flag.replace("/work/", `${tempDir}/`))),
    );
  } finally {
    fs.rmSync(tempDir, { recursive: true, force: true });
  }
}

function scenarioMap(lines) {
  return new Map(lines.map((line) => [line.slice(0, line.indexOf("|")), line]));
}

function legacyLines(lines) {
  return lines.filter((line) => line.startsWith("legacy/"));
}

function assertLegacyCoverage(lines) {
  const scenarios = scenarioMap(lines);
  const expected = legacyExtensions.length * legacyScenarios.length;
  if (lines.length !== expected) {
    throw new Error(`legacy trace coverage expected ${expected}, observed ${lines.length}`);
  }
  for (const extension of legacyExtensions) {
    for (const scenario of legacyScenarios) {
      const name = `legacy/${scenario}/${extension}`;
      if (!scenarios.has(name)) throw new Error(`legacy trace coverage missing ${name}`);
    }
  }
}

function assertEsp32ToIntBoundaries(lines) {
  const scenarios = scenarioMap(lines);
  for (const extension of legacyExtensions) {
    const trace = scenarios.get(`legacy/ws-to-int/${extension}`) || "";
    const segments = trace.split("|").slice(1).join("|").split("~~");
    const expectedTokens = ["2147483647", "2147483648", "4294967295", "4294967296"];
    if (segments.length !== expectedTokens.length) {
      throw new Error(`ESP32-C3 toInt trace has wrong boundary count for ${extension}\n${trace}`);
    }
    expectedTokens.forEach((token, index) => {
      const segment = segments[index];
      if (
        !segment.startsWith(`${token}{`) ||
        !segment.includes("ws:7:READY") ||
        !segment.includes("|wsSize=2147483647}")
      ) {
        throw new Error(`ESP32-C3 toInt boundary ${token} is not signed-32-bit for ${extension}\n${segment}`);
      }
    });
  }
}

function assertPdfPolicy(lines) {
  const scenarios = scenarioMap(lines);
  const success = scenarios.get("pdf/ws-success") || "";
  for (const expected of [
    "atomic.begin:/books/book.pdf:3",
    "ws:7:READY",
    "atomic.write:3",
    "ws:7:PROGRESS:3:3",
    "atomic.finish:3",
    "cache:/books/book.pdf",
    "ws:7:DONE",
  ]) {
    if (!success.includes(expected)) throw new Error(`PDF WS policy missing ${expected}\n${success}`);
  }

  for (const forbidden of ["remove:/books/book.pdf", "open:WS:/books/book.pdf", "write:/books/book.pdf"] ) {
    if (success.includes(forbidden)) throw new Error(`PDF WS used legacy canonical sink ${forbidden}\n${success}`);
  }

  for (const token of ["2147483647", "2147483648", "4294967295"]) {
    const boundary = scenarios.get(`pdf/ws-size-${token}`) || "";
    if (!boundary.includes(`atomic.begin:/books/book.pdf:${token}`) || !boundary.includes("ws:7:READY")) {
      throw new Error(`PDF checked size parser rejected valid uint32 boundary ${token}\n${boundary}`);
    }
  }
  const overflow = scenarios.get("pdf/ws-size-4294967296") || "";
  if (!overflow.includes("ws:7:ERROR:Invalid START format") || overflow.includes("atomic.begin:")) {
    throw new Error(`PDF checked size parser accepted uint32 overflow\n${overflow}`);
  }

  const http = scenarios.get("pdf/http-success") || "";
  for (const expected of ["atomic.begin:/books/book.pdf:", "atomic.write:3", "atomic.finish:3", "http:200:"]) {
    if (!http.includes(expected)) throw new Error(`PDF HTTP policy missing ${expected}\n${http}`);
  }
  for (const forbidden of ["remove:/books/book.pdf", "open:WEB:/books/book.pdf", "write:/books/book.pdf"]) {
    if (http.includes(forbidden)) throw new Error(`PDF HTTP used legacy canonical sink ${forbidden}\n${http}`);
  }
}

function assertUploadConcurrencyPolicy(lines) {
  const scenarios = scenarioMap(lines);
  const failures = [];
  const legacy = scenarios.get("concurrency/legacy-http-ws") || "";
  for (const expected of [
    "open:WEB:/http-books/http.epub",
    "open:WS:/ws-books/socket.txt",
    "write:/ws-books/socket.txt:3:3",
    "cache:/ws-books/socket.txt",
    "write:/http-books/http.epub:4:4",
    "cache:/http-books/http.epub",
    "|httpExact=1|wsExact=1|httpName=http.epub|httpPath=/http-books|httpSize=4|httpSuccess=1",
  ]) {
    if (!legacy.includes(expected)) failures.push(`independent legacy transports missing ${expected}\n${legacy}`);
  }
  for (const forbidden of ["ERROR:Upload already in progress", "atomic.begin:", "atomic.write:"]) {
    if (legacy.includes(forbidden)) failures.push(`independent legacy transports used shared PDF state ${forbidden}\n${legacy}`);
  }

  const restart = scenarios.get("concurrency/http-restart-preserves-active-pdf") || "";
  for (const expected of [
    "atomic.begin:/original/active.pdf:",
    "atomic.write:3",
    "atomic.finish:3",
    "cache:/original/active.pdf",
    "|name=active.pdf|path=/original|size=3|success=1|error=|owner=0|atomicActive=0",
  ]) {
    if (!restart.includes(expected)) failures.push(`HTTP re-START corrupted active PDF state at ${expected}\n${restart}`);
  }
  for (const forbidden of ["/replacement/replacement.epub", "open:WEB:/replacement", "remove:/replacement"]) {
    if (restart.includes(forbidden)) failures.push(`rejected HTTP re-START mutated filesystem target ${forbidden}\n${restart}`);
  }

  const busyPost = scenarios.get("concurrency/http-busy-post-is-request-scoped") || "";
  for (const expected of [
    "atomic.begin:/prior/prior.pdf:",
    "atomic.finish:3",
    "atomic.begin:/pdf-ws/active.pdf:3",
    "http:400:text/plain:Upload already in progress",
    "atomic.write:3",
    "cache:/pdf-ws/active.pdf",
    "|activeName=active.pdf|activePath=/pdf-ws|activeSize=3|activeInProgress=1|activeClient=7|activeOwner=2|activeAtomic=1|afterOwner=0|afterAtomic=0",
  ]) {
    if (!busyPost.includes(expected)) {
      failures.push(`Busy HTTP POST leaked stale success or damaged active WS PDF at ${expected}\n${busyPost}`);
    }
  }
  if (busyPost.includes("http:200:")) {
    failures.push(`Busy HTTP POST returned stale prior success\n${busyPost}`);
  }

  const httpPdf = scenarios.get("concurrency/http-pdf-blocks-ws-legacy") || "";
  for (const expected of [
    "atomic.begin:/pdf-http/active.pdf:",
    "ws:7:ERROR:Upload already in progress",
    "atomic.write:3",
    "atomic.finish:3",
    "cache:/pdf-http/active.pdf",
    "|httpName=active.pdf|httpPath=/pdf-http|httpSize=3|httpSuccess=1|wsName=prior.txt|wsPath=/prior-ws|wsSize=11|wsReceived=4|atomicActive=0",
  ]) {
    if (!httpPdf.includes(expected)) failures.push(`active HTTP PDF conflict lost state at ${expected}\n${httpPdf}`);
  }
  for (const forbidden of ["/blocked-ws/blocked.txt", "open:WS:/blocked-ws"]) {
    if (httpPdf.includes(forbidden)) failures.push(`active HTTP PDF conflict mutated WS target ${forbidden}\n${httpPdf}`);
  }

  const wsPdf = scenarios.get("concurrency/ws-pdf-blocks-http-legacy") || "";
  for (const expected of [
    "atomic.begin:/pdf-ws/active.pdf:3",
    "atomic.write:3",
    "atomic.finish:3",
    "cache:/pdf-ws/active.pdf",
    "|httpName=prior-http.txt|httpPath=/prior-http|httpSize=17|httpSuccess=1|httpError=|wsName=active.pdf|wsPath=/pdf-ws|wsSize=3|atomicActive=0",
  ]) {
    if (!wsPdf.includes(expected)) failures.push(`active WS PDF conflict lost state at ${expected}\n${wsPdf}`);
  }
  for (const forbidden of ["/blocked-http/blocked.epub", "open:WEB:/blocked-http"]) {
    if (wsPdf.includes(forbidden)) failures.push(`active WS PDF conflict mutated HTTP target ${forbidden}\n${wsPdf}`);
  }

  const protectedContinue = scenarios.get("concurrency/ws-pdf-protected-restart-continues") || "";
  for (const expected of [
    "atomic.begin:/pdf-ws/active.pdf:3",
    "ws:7:ERROR:Access denied to protected path",
    "atomic.write:3",
    "atomic.finish:3",
    "cache:/pdf-ws/active.pdf",
    "|beforeName=active.pdf|beforePath=/pdf-ws|beforeSize=3|beforeReceived=0|beforeInProgress=1|beforeClient=7|beforeOwner=2|beforeAtomic=1",
    "|afterName=active.pdf|afterPath=/pdf-ws|afterSize=3|afterReceived=3|afterInProgress=0|afterClient=255|afterOwner=0|afterAtomic=0",
  ]) {
    if (!protectedContinue.includes(expected)) {
      failures.push(`protected WS re-START stranded the continuable PDF at ${expected}\n${protectedContinue}`);
    }
  }
  for (const forbidden of ["/.crosspoint/replacement.epub", "open:WS:/.crosspoint", "atomic.begin:/.crosspoint"]) {
    if (protectedContinue.includes(forbidden)) {
      failures.push(`protected WS re-START published or opened candidate ${forbidden}\n${protectedContinue}`);
    }
  }

  const protectedAbort = scenarios.get("concurrency/ws-pdf-protected-restart-aborts") || "";
  for (const expected of [
    "atomic.begin:/pdf-ws/active.pdf:3",
    "ws:7:ERROR:Access denied to protected path",
    "atomic.abort",
    "|name=active.pdf|path=/pdf-ws|size=3|received=0|inProgress=0|client=255|owner=0|atomic=0",
  ]) {
    if (!protectedAbort.includes(expected)) {
      failures.push(`protected WS re-START stranded the abort path at ${expected}\n${protectedAbort}`);
    }
  }

  const stopLegacy = scenarios.get("concurrency/stop-cleans-legacy-http-and-reuses") || "";
  for (const expected of [
    "open:WEB:/stop-books/partial.epub",
    "close:/stop-books/partial.epub",
    "remove:/stop-books/partial.epub",
    "http.stop",
    "write:/stop-books/partial.epub:3:3",
    "cache:/stop-books/partial.epub",
    "http:200:text/plain:File uploaded successfully: partial.epub",
    "|stoppedRemoved=1|stoppedClosed=1|stoppedOwner=0|stoppedBuffer=0|stoppedName=|stoppedPath=/|stoppedSize=0|stoppedSuccess=0|stoppedError=|reusedExact=1|reusedOwner=0|reusedSuccess=1",
  ]) {
    if (!stopLegacy.includes(expected)) {
      failures.push(`stop stranded or reused a partial legacy HTTP upload at ${expected}\n${stopLegacy}`);
    }
  }
  if (failures.length > 0) throw new Error(failures.join("\n\n"));
}

function mutationSources(source) {
  const successAfterCache = replaceInFunction(
    source,
    "void CrossPointWebServer::handleUpload(",
    "clearBookCachePreservingUserState(filePath.c_str());\n        }",
    "clearBookCachePreservingUserState(filePath.c_str());\n          state.success = false;\n        }",
  );
  const forcedPdf = replaceInFunction(
    source,
    "void CrossPointWebServer::handleUpload(",
    "const bool pdfUpload = isPdfUploadTarget(filePath);",
    "const bool pdfUpload = true;",
  );
  const changedMessage = replaceInFunction(
    source,
    "void CrossPointWebServer::handleUpload(",
    '"Failed to write final data to SD card"',
    '"Final write failed"',
  );
  const changedStatus = replaceInFunction(
    source,
    "void CrossPointWebServer::handleUploadPost(",
    'if (state.success) {\n    server->send(200, "text/plain", "File uploaded successfully: " + state.fileName);',
    'if (state.success) {\n    server->send(201, "text/plain", "File uploaded successfully: " + state.fileName);',
  );
  const upload = functionBody(source, "void CrossPointWebServer::handleUpload(");
  const abortedAt = upload.indexOf("UPLOAD_FILE_ABORTED");
  const removeAt = upload.indexOf("Storage.remove(filePath.c_str());", abortedAt);
  if (abortedAt < 0 || removeAt < 0) throw new Error("abort-removal mutation source not found");
  const mutatedUpload =
    upload.slice(0, removeAt) +
    "Storage.exists(filePath.c_str());" +
    upload.slice(removeAt + "Storage.remove(filePath.c_str());".length);
  const abortRemoval = source.replace(upload, mutatedUpload);
  const checkedBeforeGate = replaceInFunction(
    source,
    "void CrossPointWebServer::onWebSocketEvent(",
    "if (pdfUpload && !parseUploadSizeToken(startValue, startSize))",
    "if (!parseUploadSizeToken(startValue, startSize))",
  );
  return new Map([
    ["success-after-cache", successAfterCache],
    ["forced-pdf-gate", forcedPdf],
    ["changed-final-message", changedMessage],
    ["changed-post-status", changedStatus],
    ["abort-removal", abortRemoval],
    ["checked-parser-before-gate", checkedBeforeGate],
  ]);
}

function concurrencyMutationSources(source) {
  const protectedBody = functionBody(source, "isProtectedPath(", true);
  const bypassProtectedPath = source.replace(
    protectedBody,
    "bool isProtectedPath(const String&) { return false; }",
  );
  const clobberedProtectedRestart = replaceInFunction(
    source,
    "void CrossPointWebServer::onWebSocketEvent(",
    'wsServer->sendTXT(num, "ERROR:Access denied to protected path");\n            return;',
    'wsServer->sendTXT(num, "ERROR:Access denied to protected path");\n            wsUploadInProgress = false;\n            wsUploadClientNum = 255;\n            return;',
  );
  const staleBusyPost = replaceInFunction(
    source,
    "void CrossPointWebServer::handleUploadPost(",
    "if (state.httpPostStatus == UploadState::HttpPostStatus::Busy)",
    "if (false)",
  );
  const stopWithoutLegacyRemoval = replaceInFunction(
    source,
    "void CrossPointWebServer::stop(",
    "Storage.remove(filePath.c_str())",
    "Storage.exists(filePath.c_str())",
  );
  const stopWithoutOwnerReset = replaceInFunction(
    source,
    "void CrossPointWebServer::stop(",
    "upload.owner = UploadState::Owner::None;",
    "upload.owner = UploadState::Owner::Http;",
  );
  return new Map([
    ["protected-ws-restart-gate", bypassProtectedPath],
    ["protected-ws-restart-state-clobber", clobberedProtectedRestart],
    ["http-busy-post-stale-success", staleBusyPost],
    ["stop-legacy-removal", stopWithoutLegacyRemoval],
    ["stop-http-owner-reset", stopWithoutOwnerReset],
  ]);
}

const source = normalized(fs.readFileSync(serverPath, "utf8"));
const baseline = normalized(fs.readFileSync(baselinePath, "utf8"));
const overloadProbe = compileFsHelpersBoundary(source, "current");
if (overloadProbe.status !== 0) {
  throw new Error(
    `production PDF gate did not compile against the real FsHelpers overloads\n${overloadProbe.stdout}\n${overloadProbe.stderr}`,
  );
}
const baselineTrace = legacyLines(compileAndRun(baseline, "baseline"));
const currentTrace = compileAndRun(source, "current");
const currentLegacy = legacyLines(currentTrace);
assertLegacyCoverage(baselineTrace);
assertLegacyCoverage(currentLegacy);
assertEsp32ToIntBoundaries(baselineTrace);
assertEsp32ToIntBoundaries(currentLegacy);
if (JSON.stringify(currentLegacy) !== JSON.stringify(baselineTrace)) {
  const mismatch = baselineTrace.findIndex((line, index) => line !== currentLegacy[index]);
  throw new Error(
    `legacy upload behavior diverged at trace ${mismatch}\n` +
      `baseline: ${baselineTrace[mismatch]}\ncurrent:  ${currentLegacy[mismatch]}`,
  );
}
assertPdfPolicy(currentTrace);
assertUploadConcurrencyPolicy(currentTrace);

const wideIntegerTrace = legacyLines(compileAndRun(baseline, "baseline-wide-integers", { wideIntegers: true }));
try {
  assertEsp32ToIntBoundaries(wideIntegerTrace);
  throw new Error("host 64-bit integer model unexpectedly passed the ESP32-C3 boundary oracle");
} catch (error) {
  if (error.message === "host 64-bit integer model unexpectedly passed the ESP32-C3 boundary oracle") throw error;
  console.log("WEB_UPLOAD_HARNESS_MUTATION_REJECTED host-64-bit-integer-model");
}

try {
  assertLegacyCoverage(baselineTrace.filter((line) => !line.endsWith("/.xtch") && !line.includes("/.xtch|")));
  throw new Error("xtch coverage omission was accepted");
} catch (error) {
  if (error.message === "xtch coverage omission was accepted") throw error;
  console.log("WEB_UPLOAD_HARNESS_MUTATION_REJECTED xtch-coverage-omission");
}

const ambiguousOverload = replaceInFunction(
  source,
  "isPdfUploadTarget(",
  "FsHelpers::hasPdfExtension(filePath)",
  "FsHelpers::hasPdfExtension(filePath.c_str())",
);
const ambiguousProbe = compileFsHelpersBoundary(ambiguousOverload, "mutation-c-string-overload");
if (ambiguousProbe.status === 0 || !/ambiguous/i.test(ambiguousProbe.stderr)) {
  throw new Error(
    `c_str overload mutation was not rejected as ambiguous\n${ambiguousProbe.stdout}\n${ambiguousProbe.stderr}`,
  );
}
console.log("WEB_UPLOAD_HARNESS_MUTATION_REJECTED c-string-overload-ambiguity");

for (const [name, mutated] of mutationSources(source)) {
  const mutationTrace = legacyLines(compileAndRun(mutated, `mutation-${name}`));
  if (JSON.stringify(mutationTrace) === JSON.stringify(baselineTrace)) {
    throw new Error(`${name}: compiled behavioral harness accepted mutation`);
  }
  console.log(`WEB_UPLOAD_BEHAVIOR_MUTATION_REJECTED ${name}`);
}

for (const [name, mutated] of concurrencyMutationSources(source)) {
  const mutationTrace = compileAndRun(mutated, `mutation-${name}`);
  let rejected = false;
  try {
    assertUploadConcurrencyPolicy(mutationTrace);
  } catch {
    rejected = true;
  }
  if (!rejected) throw new Error(`${name}: compiled concurrency harness accepted mutation`);
  console.log(`WEB_UPLOAD_CONCURRENCY_MUTATION_REJECTED ${name}`);
}

console.log(
  `WEB_UPLOAD_LEGACY_BEHAVIOR_PASS traces=${baselineTrace.length} mode=${sanitize ? "asan-ubsan" : "normal"}`,
);
