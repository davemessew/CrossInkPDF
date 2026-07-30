"use strict";

const fs = require("fs");
const path = require("path");

const repoRoot = path.resolve(__dirname, "..", "..");
const serverHeader = fs.readFileSync(path.join(repoRoot, "src", "network", "CrossPointWebServer.h"), "utf8");
const serverSource = fs.readFileSync(path.join(repoRoot, "src", "network", "CrossPointWebServer.cpp"), "utf8");
const filesPage = fs.readFileSync(path.join(repoRoot, "web", "pages", "files.js"), "utf8");
const rootTestCmake = fs.readFileSync(path.join(repoRoot, "test", "CMakeLists.txt"), "utf8");

function requireMatch(source, pattern, message) {
  if (!pattern.test(source)) throw new Error(message);
}

requireMatch(
  serverHeader,
  /BookUpload\/AtomicBookUpload\.h/,
  "CrossPointWebServer must own the bounded atomic upload state",
);
requireMatch(
  serverHeader,
  /BookUpload::AtomicUploadState\s+transaction/,
  "HTTP and WS must share the same long-lived transaction",
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
  "HTTP START must use the executable shared admission seam",
);
requireMatch(
  serverSource,
  /admitTransportStart\([\s\S]{0,300}state\.success = response\.success;[\s\S]{0,120}state\.fileName = "";[\s\S]{0,120}state\.error = "";[\s\S]{0,120}if \(admission/,
  "HTTP response success, filename, and error must reset before the Busy return",
);
requireMatch(
  serverSource,
  /httpResponseStatus\(/,
  "HTTP response selection must give errors precedence over stale success",
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
if (/Storage\.remove\(filePath\.c_str\(\)\)/.test(transportHandlers)) {
  throw new Error("transport handlers must never delete the canonical target at upload START or abort");
}
if (/openFileForWrite\([^;]*filePath/.test(transportHandlers)) {
  throw new Error("transport handlers must never open the canonical target for direct writes");
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
