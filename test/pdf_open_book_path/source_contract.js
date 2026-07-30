"use strict";

const fs = require("fs");
const path = require("path");

function readUtf8(filePath) {
  return fs.readFileSync(filePath, "utf8");
}

function listSourceFiles(directory) {
  const files = [];
  for (const entry of fs.readdirSync(directory, { withFileTypes: true })) {
    const entryPath = path.join(directory, entry.name);
    if (entry.isDirectory()) {
      files.push(...listSourceFiles(entryPath));
    } else if (/\.(?:cpp|h)$/.test(entry.name) && !entry.name.endsWith(".generated.h")) {
      files.push(entryPath);
    }
  }
  return files;
}

function requireMatch(source, pattern, message) {
  if (!pattern.test(source)) {
    throw new Error(message);
  }
}

function countMatches(source, pattern) {
  return [...source.matchAll(pattern)].length;
}

function loadRepository(repoRoot) {
  const srcRoot = path.join(repoRoot, "src");
  const exceptionPaths = new Set([
    path.join(srcRoot, "CrossPointState.h"),
    path.join(srcRoot, "CrossPointState.cpp"),
    path.join(srcRoot, "JsonSettingsIO.cpp"),
  ]);

  return {
    stateHeader: readUtf8(path.join(srcRoot, "CrossPointState.h")),
    stateSource: readUtf8(path.join(srcRoot, "CrossPointState.cpp")),
    jsonSource: readUtf8(path.join(srcRoot, "JsonSettingsIO.cpp")),
    mainSource: readUtf8(path.join(srcRoot, "main.cpp")),
    semanticSources: listSourceFiles(srcRoot)
      .filter((filePath) => !exceptionPaths.has(filePath))
      .map((filePath) => ({
        path: path.relative(repoRoot, filePath),
        source: readUtf8(filePath),
      })),
  };
}

function validateSources(sources) {
  requireMatch(
    sources.stateHeader,
    /std::string&\s+openBookPath\(\)\s+noexcept\s*\{\s*return openEpubPath;\s*\}/,
    "CrossPointState must expose a non-const generic openBookPath alias",
  );
  requireMatch(
    sources.stateHeader,
    /const std::string&\s+openBookPath\(\)\s+const noexcept\s*\{\s*return openEpubPath;\s*\}/,
    "CrossPointState must expose a const generic openBookPath alias",
  );
  requireMatch(
    sources.stateHeader,
    /std::string\s+openEpubPath;/,
    "the persisted openEpubPath member must retain its name and type",
  );
  if (countMatches(sources.stateHeader, /\bopenEpubPath\b/g) !== 3) {
    throw new Error("CrossPointState.h may use openEpubPath only for the persisted member and its two aliases");
  }

  requireMatch(
    sources.stateSource,
    /Legacy binary compatibility:[^\n]*\n\s*serialization::readString\(inputFile, openEpubPath\);/,
    "binary migration must document and retain the legacy member/layout",
  );
  if (countMatches(sources.stateSource, /\bopenEpubPath\b/g) !== 1) {
    throw new Error("CrossPointState.cpp may use openEpubPath only in the documented legacy binary loader");
  }

  requireMatch(
    sources.jsonSource,
    /Persisted-state compatibility:[^\n]*\n\s*doc\["openEpubPath"\]\s*=\s*s\.openEpubPath;/,
    "JSON save must document and retain the legacy member/key",
  );
  requireMatch(
    sources.jsonSource,
    /Persisted-state compatibility:[^\n]*\n\s*s\.openEpubPath\s*=\s*doc\["openEpubPath"\]/,
    "JSON load must document and retain the legacy member/key",
  );
  if (countMatches(sources.jsonSource, /\bopenEpubPath\b/g) !== 4) {
    throw new Error("JsonSettingsIO.cpp may use openEpubPath only for the documented legacy JSON save/load pairs");
  }
  if (/doc\["openBookPath"\]/.test(sources.jsonSource)) {
    throw new Error("the persisted JSON key must not be renamed to openBookPath");
  }

  requireMatch(
    sources.mainSource,
    /const std::string epubPath\s*=\s*APP_STATE\.openBookPath\(\);[\s\S]{0,180}!FsHelpers::hasEpubExtension\(epubPath\)/,
    "KOReader sync must read the generic path while retaining its EPUB-only extension guard",
  );

  for (const file of sources.semanticSources) {
    if (/\bopenEpubPath\b/.test(file.source)) {
      throw new Error(`${file.path} must use openBookPath() for semantic access`);
    }
  }
}

module.exports = {
  loadRepository,
  validateSources,
};
