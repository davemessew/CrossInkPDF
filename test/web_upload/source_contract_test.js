"use strict";

const fs = require("fs");
const path = require("path");

const repoRoot = path.resolve(__dirname, "..", "..");
const serverHeader = fs.readFileSync(path.join(repoRoot, "src", "network", "CrossPointWebServer.h"), "utf8");
const serverSource = fs.readFileSync(path.join(repoRoot, "src", "network", "CrossPointWebServer.cpp"), "utf8");
const filesPage = fs.readFileSync(path.join(repoRoot, "web", "pages", "files.js"), "utf8");
const rootTestCmake = fs.readFileSync(path.join(repoRoot, "test", "CMakeLists.txt"), "utf8");
const traceHarness = fs.readFileSync(path.join(repoRoot, "test", "web_upload", "legacy_transport_trace_test.js"), "utf8");
const baselineFixture = fs.readFileSync(
  path.join(repoRoot, "test", "web_upload", "fixtures", "legacy_upload_baseline.cpp.txt"),
  "utf8",
);

function requireMatch(source, pattern, message) {
  if (!pattern.test(source)) throw new Error(message);
}

function functionBody(source, signature) {
  const start = source.indexOf(signature);
  if (start < 0) throw new Error(`missing function: ${signature}`);
  const opening = source.indexOf("{", start);
  let depth = 0;
  for (let index = opening; index < source.length; index += 1) {
    if (source[index] === "{") depth += 1;
    if (source[index] === "}") {
      depth -= 1;
      if (depth === 0) return source.slice(start, index + 1);
    }
  }
  throw new Error(`unterminated function: ${signature}`);
}

requireMatch(
  traceHarness,
  /baselinePath = path\.join\(__dirname, "fixtures\/legacy_upload_baseline\.cpp\.txt"\)/,
  "legacy executable oracle must use the bounded checked-in baseline fixture",
);
if (/runProcess\("git"|git\s+show|0e05e8b5\^/.test(traceHarness)) {
  throw new Error("legacy executable oracle must not require repository history at runtime");
}
requireMatch(
  baselineFixture,
  /Source: 3849685de92d90a7f3e4177e643ef43a4d45113c:src\/network\/CrossPointWebServer\.cpp/,
  "legacy baseline fixture must record its exact historical source",
);
for (const historicalHandler of [
  "flushUploadBuffer(",
  "void CrossPointWebServer::abortWsUpload(",
  "void CrossPointWebServer::stop(",
  "void CrossPointWebServer::handleUpload(",
  "void CrossPointWebServer::handleUploadPost(",
  "void CrossPointWebServer::onWebSocketEvent(",
]) {
  if (!baselineFixture.includes(historicalHandler)) {
    throw new Error(`legacy baseline fixture is missing ${historicalHandler}`);
  }
}

requireMatch(
  serverHeader,
  /BookUpload\/AtomicBookUpload\.h/,
  "CrossPointWebServer must own the bounded atomic upload state",
);
requireMatch(
  serverHeader,
  /BookUpload::AtomicUploadState\s+transaction/,
  "PDF HTTP and WS must share the same long-lived transaction",
);
requireMatch(
  serverHeader,
  /enum class HttpPostStatus[\s\S]{0,120}UploadResult[\s\S]{0,80}Busy[\s\S]{0,240}HttpPostStatus httpPostStatus/,
  "HTTP POST admission outcome must be request-scoped separately from active upload state",
);
requireMatch(
  serverSource,
  /BookUpload::begin\(/,
  "uploads must start through the atomic helper",
);
requireMatch(
  serverSource,
  /BookUpload::finish\(/,
  "uploads must verify and promote through the atomic helper",
);
requireMatch(
  serverSource,
  /clearBookCachePreservingUserState\(targetPath\)/,
  "committed replacement must invalidate only derived cache",
);
requireMatch(
  serverSource,
  /return clearBookCachePreservingUserState\(targetPath\);/,
  "cache invalidation failure must propagate through the fallible commit hook",
);
requireMatch(
  serverSource,
  /mbedtls_sha256_(starts|update|finish)/,
  "production upload verification must use streaming mbedTLS SHA-256",
);
requireMatch(
  serverSource,
  /admitTransportStart\(/,
  "PDF HTTP START must use the executable shared admission seam",
);

const uploadHandlerStart = serverSource.indexOf("void CrossPointWebServer::handleUpload(");
const uploadHandlerEnd = serverSource.indexOf("void CrossPointWebServer::handleUploadPost(", uploadHandlerStart);
const wsHandlerStart = serverSource.indexOf("void CrossPointWebServer::onWebSocketEvent(");
const wsHandlerEnd = serverSource.indexOf("// --- Font management handlers ---", wsHandlerStart);
if (uploadHandlerStart < 0 || uploadHandlerEnd < 0 || wsHandlerStart < 0 || wsHandlerEnd < 0) {
  throw new Error("could not locate upload handler source ranges");
}
const transportHandlers =
  serverSource.slice(uploadHandlerStart, uploadHandlerEnd) + serverSource.slice(wsHandlerStart, wsHandlerEnd);
const uploadHandler = serverSource.slice(uploadHandlerStart, uploadHandlerEnd);
const wsHandler = serverSource.slice(wsHandlerStart, wsHandlerEnd);
const flushHandler = functionBody(serverSource, "flushUploadBuffer(");
const abortHandler = functionBody(serverSource, "void CrossPointWebServer::abortWsUpload(");
const stopHandler = functionBody(serverSource, "void CrossPointWebServer::stop(");
const postHandler = functionBody(serverSource, "void CrossPointWebServer::handleUploadPost(");

const httpCandidateAt = uploadHandler.indexOf("const String startFileName");
const httpAdmissionAt = uploadHandler.indexOf("BookUpload::admitTransportStart");
const httpPublishAt = uploadHandler.indexOf("state.fileName = startFileName");
if (!(httpCandidateAt >= 0 && httpCandidateAt < httpAdmissionAt && httpAdmissionAt < httpPublishAt)) {
  throw new Error("HTTP START must parse candidate metadata, admit the atomic target, then publish state");
}

const wsCandidateAt = wsHandler.indexOf("String startValue = normalizeWebPath");
const wsProtectedAt = wsHandler.indexOf("if (isProtectedPath(startValue))");
const wsBusyAt = wsHandler.indexOf("if (wsUploadInProgress || sharedPdfActive");
const wsPublishAt = wsHandler.indexOf("wsUploadFileName = StringUtils::sanitizeFilename");
if (!(wsCandidateAt >= 0 && wsCandidateAt < wsProtectedAt && wsProtectedAt < wsBusyAt && wsBusyAt < wsPublishAt)) {
  throw new Error("WebSocket START must validate the candidate and conflicts before publishing state");
}
const wsPrePublish = wsHandler.slice(wsCandidateAt, wsPublishAt);
if (/wsUpload(?:FileName|Path|Size|Received|InProgress|ClientNum)\s*=(?!=)|upload\.owner\s*=(?!=)|BookUpload::(?:begin|write|finish|abort)\s*\(|wsUploadFile\.(?:write|close)\s*\(/.test(wsPrePublish)) {
  throw new Error("WebSocket START rejection paths must not mutate active upload state before admission");
}

requireMatch(
  serverSource,
  /bool isPdfUploadTarget\(const String& filePath\)[\s\S]{0,160}FsHelpers::hasPdfExtension\(filePath\)/,
  "atomic upload routing must select the real Arduino String overload",
);
requireMatch(
  uploadHandler,
  /const bool pdfUpload = isPdfUploadTarget\(filePath\);[\s\S]*?if \(pdfUpload\)[\s\S]*?admitTransportStart\([\s\S]*?BookUpload::begin\([\s\S]*?\} else \{[\s\S]*?Storage\.remove\(filePath\.c_str\(\)\)[\s\S]*?openFileForWrite\("WEB", filePath, state\.file\)/,
  "HTTP must reserve atomic begin/promotion for PDFs and retain direct legacy overwrite for other formats",
);
requireMatch(
  uploadHandler,
  /const String startFileName[\s\S]*?const bool pdfUpload[\s\S]*?if \(state\.owner != UploadState::Owner::None[\s\S]*?return;[\s\S]*?state\.fileName = startFileName/,
  "HTTP START must parse a candidate before admission and publish it only after the active upload is preserved",
);
requireMatch(
  uploadHandler,
  /if \(state\.owner != UploadState::Owner::None[\s\S]{0,280}state\.httpPostStatus = UploadState::HttpPostStatus::Busy;[\s\S]{0,180}return;[\s\S]{0,100}state\.httpPostStatus = UploadState::HttpPostStatus::UploadResult;/,
  "Busy HTTP START must publish a request result without replacing active upload metadata",
);
requireMatch(
  flushHandler,
  /const bool pdfUpload = isPdfUploadTarget\(state\.fileName\);[\s\S]*?if \(pdfUpload\)[\s\S]*?BookUpload::write\([\s\S]*?\} else \{[\s\S]*?state\.file\.write\(/,
  "HTTP buffered writes must keep the legacy direct-write transport beside PDF atomic writes",
);
requireMatch(
  uploadHandler,
  /UPLOAD_FILE_ABORTED[\s\S]*?const bool pdfUpload = isPdfUploadTarget\(state\.fileName\);[\s\S]*?if \(pdfUpload\)[\s\S]*?BookUpload::abort\([\s\S]*?\} else if \(state\.file\) \{[\s\S]*?state\.file\.close\(\)[\s\S]*?Storage\.remove\(filePath\.c_str\(\)\)/,
  "HTTP abort must preserve the legacy close-and-remove behavior for non-PDF uploads",
);
requireMatch(
  postHandler,
  /if \(isPdfUploadTarget\(state\.fileName\)\)[\s\S]*?httpResponseStatus\([\s\S]*?return;[\s\S]*?if \(state\.success\)/,
  "HTTP POST must apply strict error precedence only to PDFs and retain legacy success status for other books",
);
requireMatch(
  postHandler,
  /if \(state\.httpPostStatus == UploadState::HttpPostStatus::Busy\)[\s\S]{0,180}server->send\(400, "text\/plain", "Upload already in progress"\);[\s\S]{0,80}return;[\s\S]*?if \(isPdfUploadTarget/,
  "HTTP POST must return the current Busy request before consulting an older upload result",
);
requireMatch(
  wsHandler,
  /pdfUpload = isPdfUploadTarget\(startValue\);[\s\S]*?if \(pdfUpload\)[\s\S]*?BookUpload::begin\([\s\S]*?\} else \{[\s\S]*?Storage\.remove\(startValue\.c_str\(\)\)[\s\S]*?openFileForWrite\("WS", startValue, wsUploadFile\)/,
  "WebSocket START must reserve atomic begin for PDFs and directly overwrite legacy formats",
);
requireMatch(
  wsHandler,
  /String startValue = normalizeWebPath[\s\S]*?startValue \+= StringUtils::sanitizeFilename[\s\S]*?pdfUpload = isPdfUploadTarget[\s\S]*?if \(wsUploadInProgress[\s\S]*?break;[\s\S]*?wsUploadFileName = StringUtils::sanitizeFilename/,
  "WebSocket START must parse a candidate before admission and publish it only after the active upload is preserved",
);
requireMatch(
  wsHandler,
  /case WStype_BIN[\s\S]*?const bool pdfUpload = isPdfUploadTarget\(wsUploadFileName\);[\s\S]*?if \(pdfUpload\)[\s\S]*?BookUpload::write\([\s\S]*?\} else \{[\s\S]*?wsUploadFile\.write\(payload, length\)/,
  "WebSocket binary frames must retain direct legacy writes for non-PDF uploads",
);
requireMatch(
  abortHandler,
  /const bool pdfUpload = isPdfUploadTarget\(wsUploadFileName\);[\s\S]*?if \(pdfUpload\)[\s\S]*?BookUpload::abort\([\s\S]*?\} else \{[\s\S]*?wsUploadFile\.close\(\)[\s\S]*?Storage\.remove\(filePath\.c_str\(\)\)/,
  "WebSocket abort must leave PDF canonical bytes untouched but remove partial legacy uploads",
);
requireMatch(
  stopHandler,
  /if \(upload\.owner == UploadState::Owner::Http\)[\s\S]*?if \(isPdfUploadTarget\(upload\.fileName\)\)[\s\S]*?BookUpload::abort\([\s\S]*?\} else \{[\s\S]*?upload\.file\.close\(\)[\s\S]*?Storage\.remove\(filePath\.c_str\(\)\)/,
  "stop must abort atomic PDFs but close and remove partial direct legacy HTTP uploads",
);
for (const reset of [
  "upload.owner = UploadState::Owner::None",
  "upload.httpPostStatus = UploadState::HttpPostStatus::UploadResult",
  'upload.fileName = ""',
  'upload.path = "/"',
  "upload.size = 0",
  "upload.success = false",
  'upload.error = ""',
  "upload.bufferPos = 0",
]) {
  if (!stopHandler.includes(reset)) throw new Error(`stop must reset reusable HTTP state: ${reset}`);
}
for (const legacyExtension of [".epub", ".xtc", ".xtch", ".txt", ".md", ".markdown"]) {
  if (legacyExtension === ".pdf") {
    throw new Error("legacy transport extension set must not contain PDF");
  }
}
if (!transportHandlers.includes("clearBookCachePreservingUserState")) {
  throw new Error("both committed PDF promotion and completed legacy overwrite must invalidate derived cache");
}

requireMatch(
  filesPage,
  /const needsConversion = isEpub && convertEnabled;/,
  "stale optimize state must not convert a PDF",
);
requireMatch(
  filesPage,
  /files\.filter\(\(f\) => f\.name\.toLowerCase\(\)\.endsWith\("\.epub"\) && convertEnabled\)/,
  "batch conversion must remain EPUB-only",
);
requireMatch(
  rootTestCmake,
  /add_subdirectory\(web_upload\)/,
  "the atomic upload suite must be registered in the root host test graph",
);

process.stdout.write("WEB_UPLOAD_SOURCE_CONTRACT_PASS\n");
