"use strict";

const fs = require("fs");
const path = require("path");
const childProcess = require("child_process");
const { validatePathFences } = require("./path_fence_contract");

const root = path.resolve(__dirname, "..", "..");

function read(relative) {
  return fs.readFileSync(path.join(root, relative), "utf8");
}

function requireText(source, text, message) {
  if (!source.includes(text)) {
    throw new Error(message);
  }
}

function requireMatch(source, pattern, message) {
  if (!pattern.test(source)) {
    throw new Error(message);
  }
}

function extractFunction(source, marker) {
  const start = source.indexOf(marker);
  if (start < 0) {
    throw new Error(`Missing function marker: ${marker}`);
  }
  const openingBrace = source.indexOf("{", start);
  if (openingBrace < 0) {
    throw new Error(`Missing function body: ${marker}`);
  }
  let depth = 0;
  for (let index = openingBrace; index < source.length; ++index) {
    if (source[index] === "{") ++depth;
    if (source[index] === "}") {
      --depth;
      if (depth === 0) return source.slice(start, index + 1);
    }
  }
  throw new Error(`Unterminated function body: ${marker}`);
}

function normalizedTokens(source) {
  return source.replace(/\s+/g, " ").trim();
}

const moveHeader = read("src/util/BookMoveUtils.h");
const moveSource = read("src/util/BookMoveUtils.cpp");
const bookmarkHeader = read("src/BookmarkStore.h");
const bookmarkSource = read("src/BookmarkStore.cpp");
const clippingHeader = read("src/ClippingStore.h");
const clippingSource = read("src/ClippingStore.cpp");
const recentsHeader = read("src/RecentBooksStore.h");
const recentsSource = read("src/RecentBooksStore.cpp");
const stateHeader = read("src/CrossPointState.h");
const stateSource = read("src/CrossPointState.cpp");
const durableSource = read("src/util/BookMoveDurableFile.cpp");
const journalSource = read("lib/BookStateMigration/BookStateMigrationJournal.cpp");
const journalHeader = read("lib/BookStateMigration/BookStateMigrationJournal.h");
const reader = read("src/activities/reader/EpubReaderActivity.cpp");
const pdfReaderRoute = read("src/activities/reader/ReaderActivity.cpp");
const pdfHalDocumentHeader = read("lib/PdfReflow/PdfHalReflowDocument.h");
const pdfHalDocumentSource = read("lib/PdfReflow/PdfHalReflowDocument.cpp");
const bookCacheSource = read("src/util/BookCacheUtils.cpp");
const home = read("src/activities/home/BookActions.cpp");
const recentGrid = read("src/activities/home/RecentBooksGridActivity.cpp");
const web = read("src/network/CrossPointWebServer.cpp");
const main = read("src/main.cpp");
const headMoveSource = childProcess.execFileSync(
  "git",
  ["show", "HEAD:src/util/BookMoveUtils.cpp"],
  { cwd: root, encoding: "utf8" },
);

for (const api of ["moveBook", "recoverPendingBookMove", "migrationCacheHash"]) {
  requireText(moveHeader, api, `BookMoveUtils must expose ${api}`);
}
for (const api of ["copyForFilePath", "verifyCopyForFilePath"]) {
  requireText(bookmarkHeader, api, `BookmarkStore must expose ${api}`);
  requireText(clippingHeader, api, `ClippingStore must expose ${api}`);
}
for (const api of [
  "activatePathMigration",
  "verifyPathMigration",
  "verifyPersistedPathMigration",
]) {
  requireText(recentsHeader, api, `RecentBooksStore must expose ${api}`);
}

const recentMissing = extractFunction(recentsSource, "bool RecentBooksStore::isMissing");
requireText(recentMissing, "Storage.exists", "recent pruning must retain its backing-file positive control");
requireText(recentMissing, "FsHelpers::hasPdfExtension", "migration-aware recent pruning must remain PDF-only");
requireText(recentMissing, "pdfPathHash64", "missing PDFs must resolve their normal cache identity");
requireText(
  recentMissing,
  "BookMoveUtils::migrationCacheHash",
  "missing PDFs must consult the bounded migration cache resolver",
);
requireText(recentMissing, "readOnlyFallback", "pre-activation PDF recents must be preserved");
const gridLoad = extractFunction(recentGrid, "void RecentBooksGridActivity::loadRecentBooks");
requireText(gridLoad, "RecentBooksStore::isMissing", "recent grid must share migration-aware missing detection");
if (gridLoad.includes("Storage.exists(")) {
  throw new Error("recent grid must not bypass migration-aware missing detection");
}
for (const api of [
  "activateOpenPathMigration",
  "verifyPersistedOpenPathMigration",
]) {
  requireText(stateHeader, api, `CrossPointState must expose ${api}`);
}

requireText(moveSource, "BookStateMigration::Coordinator", "product integration must use the pure coordinator");
requireText(moveSource, "makeUniqueNoThrow", "journal/copy workspaces must be fallibly heap allocated");
requireMatch(
  moveSource,
  /BookFormat bookFormatForPath[\s\S]{0,180}hasPdfExtension[\s\S]{0,120}BookFormat::Unknown/,
  "the journaled move router must recognize PDF",
);
const formatRouter = extractFunction(moveSource, "BookFormat bookFormatForPath");
if (formatRouter.includes("hasEpubExtension") ||
    formatRouter.includes("BookFormat::Epub")) {
  throw new Error("the journaled move router must remain PDF-only");
}
requireMatch(
  moveSource,
  /initializeWorkspace[\s\S]{0,240}record\.format != BookFormat::Pdf/,
  "boot recovery must reject non-PDF journal records",
);
requireMatch(
  moveSource,
  /copyCache[\s\S]*verifyCache[\s\S]*copyBookmarks[\s\S]*verifyBookmarks[\s\S]*copyClippings[\s\S]*verifyClippings/,
  "product callbacks must copy then verify every path-keyed state class",
);
requireText(moveSource, "Storage.removeDir", "old cache cleanup must happen in the terminal old-state phase");
requireText(moveSource, "BookmarkStore::deleteForFilePath", "old bookmark cleanup must be terminal");
requireText(moveSource, "ClippingStore::deleteForFilePath", "old clipping cleanup must be terminal");
requireText(
  moveSource,
  "verifyPersistedPathMigration",
  "activation must reload recent-books state from storage",
);

requireText(
  pdfHalDocumentHeader,
  "const uint64_t* cacheHashOverride",
  "HAL PDF loads must accept an explicit migration cache identity",
);
requireText(
  pdfHalDocumentSource,
  "initialize(pdfHalCacheIo(ioContext_), sourcePath, cacheDirectory, cacheHashOverride)",
  "HAL PDF loads must pass the migration identity synchronously into the document",
);

validatePathFences(pdfReaderRoute, main);

const clearPdfCache = extractFunction(bookCacheSource, "[[gnu::noinline]] bool clearPdfDerivedCache(");
requireText(
  bookCacheSource,
  '#include "BookMoveUtils.h"',
  "PDF cache deletion must include the production-local move resolver header",
);
if (bookCacheSource.includes("#include <BookMoveUtils.h>")) {
  throw new Error("PDF cache deletion must not let test include paths mask the production move resolver header");
}
requireText(clearPdfCache, "BookMoveUtils::migrationCacheHash", "PDF cache deletion must resolve migration state first");
requireText(clearPdfCache, "readOnlyFallback", "PDF cache deletion must reject read-only fallback");
const cacheResolver = clearPdfCache.indexOf("BookMoveUtils::migrationCacheHash");
const cacheAllocation = clearPdfCache.indexOf("allocatePdfCacheClearWorkspace");
if (!(cacheResolver >= 0 && cacheAllocation > cacheResolver)) {
  throw new Error("PDF cache deletion must resolve migration state before allocating or deleting");
}
requireText(
  moveSource,
  "verifyPersistedOpenPathMigration",
  "activation must reload open-book state from storage",
);
requireText(
  journalSource,
  "static_cast<uint8_t>(record.recentsPolicy)",
  "the keep/remove policy must survive reboot in the journal",
);
if (journalHeader.includes("BookFormat::Epub") ||
    /enum class BookFormat[\s\S]{0,120}\bEpub\b/.test(journalHeader)) {
  throw new Error("the durable move journal must not expose an EPUB format");
}
requireMatch(
  journalSource,
  /bool validFormat[^{}]*\{ return format == BookFormat::Pdf; \}/,
  "the durable move journal must decode PDF records only",
);
for (const [source, name] of [
  [bookmarkSource, "bookmark"],
  [clippingSource, "clipping"],
  [recentsSource, "recent-books"],
  [stateSource, "open-state"],
]) {
  requireText(
    source,
    "BookMoveDurableFile::replace",
    `${name} migration must use verified atomic replacement`,
  );
}
for (const [source, name] of [
  [bookmarkSource, "bookmark"],
  [clippingSource, "clipping"],
]) {
  requireMatch(
    extractFunction(source, "copyForFilePath"),
    /bookType != "pdf"/,
    `${name} journal-copy API must reject legacy book types`,
  );
  requireMatch(
    extractFunction(source, "verifyCopyForFilePath"),
    /bookType != "pdf"/,
    `${name} journal-verify API must reject legacy book types`,
  );
}
for (const token of ["file.flush()", "file.sync()", "file.close()"]) {
  requireText(
    durableSource,
    token,
    `durable replacement must perform ${token}`,
  );
}
if (moveSource.includes("std::function")) {
  throw new Error("book move integration must use function pointers, not std::function");
}

requireMatch(
  reader,
  /document\.reset\(\)[\s\S]{0,180}if \(isPdf\)[\s\S]{0,160}moveFinishedPdfToReadFolder[\s\S]{0,160}else[\s\S]{0,160}moveFinishedBookToReadFolder/,
  "reader must release document handles, then route only PDF through the journal",
);
requireMatch(
  extractFunction(reader, "void moveFinishedPdfToReadFolder"),
  /BookMoveUtils::moveBook\([\s\S]{0,180}!SETTINGS\.removeReadBooksFromRecents/,
  "reader PDF move helper must persist the selected recents policy",
);
requireMatch(
  extractFunction(reader, "void moveFinishedBookToReadFolder"),
  /Storage\.rename\([\s\S]{0,600}BookMoveUtils::migrateMovedEpubState/,
  "reader EPUB move helper must retain legacy rename-then-migrate behavior",
);
requireMatch(
  home,
  /Storage\.rename\(fullPath\.c_str\(\), dstPath\.c_str\(\)\)[\s\S]{0,700}BookMoveUtils::migrateMovedEpubState/,
  "Home EPUB move-to-Read path must retain legacy rename-then-migrate behavior",
);
if (/Storage\.rename\(fullPath\.c_str\(\), dstPath\.c_str\(\)\)/.test(home)) {
  requireText(
    home,
    "BookMoveUtils::migrateMovedEpubState",
    "Home EPUB source rename must retain legacy state migration",
  );
}

for (const functionName of ["handleRename", "handleMove"]) {
  const start = web.indexOf(`void CrossPointWebServer::${functionName}`);
  const end = web.indexOf("\nvoid CrossPointWebServer::", start + 1);
  const body = web.slice(start, end < 0 ? web.length : end);
  requireMatch(
    body,
    /journaledPdf\s*=\s*FsHelpers::hasPdfExtension/,
    `${functionName} must journal PDF files only`,
  );
  if (body.includes("hasEpubExtension")) {
    throw new Error(`${functionName} must not route EPUB through the PDF journal`);
  }
  requireMatch(
    body,
    /if \(journaledPdf\) \{\s*file\.close\(\);\s*moveResult = BookMoveUtils::moveBook/,
    `${functionName} must close the PDF source handle before the journaled move`,
  );
  requireMatch(
    body,
    /else \{\s*clearBookCache\(itemPath\.c_str\(\)\);\s*success = file\.rename\(newPath\.c_str\(\)\);\s*file\.close\(\);/,
    `${functionName} must retain the legacy non-PDF file-handle rename path`,
  );
}

const legacyMarker = "bool migrateMovedEpubState";
if (
  normalizedTokens(extractFunction(moveSource, legacyMarker)) !==
  normalizedTokens(extractFunction(headMoveSource, legacyMarker))
) {
  throw new Error(
    "migrateMovedEpubState must remain token-equivalent to HEAD for EPUB",
  );
}
const destinationMarker = "std::string buildReadFolderDestination";
if (
  normalizedTokens(extractFunction(moveSource, destinationMarker)) !==
  normalizedTokens(extractFunction(headMoveSource, destinationMarker))
) {
  throw new Error(
    "buildReadFolderDestination must remain token-equivalent to HEAD",
  );
}

console.log("BOOK_MOVE_SOURCE_CONTRACT_PASS");
