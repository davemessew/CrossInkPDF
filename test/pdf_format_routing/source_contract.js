"use strict";

const fs = require("fs");
const path = require("path");

function readUtf8(filePath) {
  return fs.readFileSync(filePath, "utf8");
}

function requireMatch(source, pattern, message) {
  if (!pattern.test(source)) {
    throw new Error(message);
  }
}

function extractBetween(source, start, end, label) {
  const startIndex = source.indexOf(start);
  if (startIndex < 0) {
    throw new Error(`cannot find ${label} start`);
  }
  const endIndex = source.indexOf(end, startIndex + start.length);
  if (endIndex < 0) {
    throw new Error(`cannot find ${label} end`);
  }
  return source.slice(startIndex, endIndex);
}

function loadRepository(repoRoot) {
  return {
    nextBookSource: readUtf8(path.join(repoRoot, "src/util/NextBookFinder.cpp")),
    screenshotHeader: readUtf8(path.join(repoRoot, "src/util/ScreenshotInfo.h")),
    epubReaderSource: readUtf8(path.join(repoRoot, "src/activities/reader/EpubReaderActivity.cpp")),
    txtReaderSource: readUtf8(path.join(repoRoot, "src/activities/reader/TxtReaderActivity.cpp")),
    xtcReaderSource: readUtf8(path.join(repoRoot, "src/activities/reader/XtcReaderActivity.cpp")),
    uiThemeSource: readUtf8(path.join(repoRoot, "src/components/UITheme.cpp")),
  };
}

function validateNextBookRouting(source) {
  const supported = extractBetween(source, "bool isSupportedBookFile", "}  // namespace", "supported-book predicate");
  for (const helper of [
    "hasEpubExtension",
    "hasPdfExtension",
    "hasXtcExtension",
    "hasTxtExtension",
    "hasMarkdownExtension",
  ]) {
    if (!supported.includes(`FsHelpers::${helper}(name)`)) {
      throw new Error(`next-book routing must retain ${helper}`);
    }
  }
  if (/has(?:Bmp|Png|Jpg|Gif)Extension/.test(supported)) {
    throw new Error("next-book routing must not promote image viewers to books");
  }

  for (const boundedFragment of [
    "result.reserve(maxCount + 1)",
    "result.size() >= maxCount",
    "result.size() > maxCount",
    "result.pop_back()",
  ]) {
    if (!source.includes(boundedFragment)) {
      throw new Error(`next-book routing lost its bounded-memory contract: ${boundedFragment}`);
    }
  }
}

function validateScreenshotRouting(header, epubSource, txtSource, xtcSource) {
  requireMatch(
    header,
    /enum class ReaderType\s*:\s*uint8_t\s*\{\s*None,\s*Epub,\s*Txt,\s*Xtc,\s*Pdf\s*\}/,
    "ScreenshotInfo::ReaderType must append Pdf without renumbering existing reader types",
  );

  const screenshotFunction = extractBetween(
    epubSource,
    "ScreenshotInfo EpubReaderActivity::getScreenshotInfo() const",
    "\n}",
    "EPUB screenshot-info function",
  );
  const titleIndex = screenshotFunction.indexOf("snprintf(info.title");
  if (titleIndex < 0) {
    throw new Error("EPUB screenshot-info function lost its title population");
  }
  const routingPrefix = screenshotFunction.slice(0, titleIndex);
  requireMatch(
    routingPrefix,
    /info\.readerType\s*=\s*ScreenshotInfo::ReaderType::Epub\s*;/,
    "EPUB screenshot-info must preserve Epub as its default",
  );
  requireMatch(
    routingPrefix,
    /document->getFormat\(\)\s*==\s*ReflowDocumentFormat::Pdf[\s\S]*info\.readerType\s*=\s*ScreenshotInfo::ReaderType::Pdf\s*;/,
    "EPUB screenshot-info must report Pdf only for a PDF reflow document",
  );
  requireMatch(
    txtSource,
    /info\.readerType\s*=\s*ScreenshotInfo::ReaderType::Txt\s*;/,
    "TXT screenshot routing must remain Txt",
  );
  requireMatch(
    xtcSource,
    /info\.readerType\s*=\s*ScreenshotInfo::ReaderType::Xtc\s*;/,
    "XTC screenshot routing must remain Xtc",
  );
}

function validateIconRouting(source) {
  const body = extractBetween(source, "UIIcon UITheme::getFileIcon", "int UITheme::getStatusBarHeight", "file-icon function");
  const bookEnd = body.indexOf("return Book;");
  const textEnd = body.indexOf("return Text;");
  const imageEnd = body.indexOf("return Image;");
  if (!(bookEnd >= 0 && textEnd > bookEnd && imageEnd > textEnd)) {
    throw new Error("file-icon branch order changed");
  }

  const bookBranch = body.slice(0, bookEnd);
  for (const helper of ["hasEpubExtension", "hasPdfExtension", "hasXtcExtension"]) {
    if (!bookBranch.includes(`FsHelpers::${helper}(filename)`)) {
      throw new Error(`book icon routing must include ${helper}`);
    }
  }
  if (/has(?:Txt|Markdown|Bmp)Extension/.test(bookBranch)) {
    throw new Error("book icon routing must not absorb text or image formats");
  }

  const textBranch = body.slice(bookEnd, textEnd);
  requireMatch(
    textBranch,
    /hasTxtExtension\(filename\)[\s\S]*hasMarkdownExtension\(filename\)/,
    "TXT and Markdown icon routing must remain Text",
  );
  const imageBranch = body.slice(textEnd, imageEnd);
  requireMatch(imageBranch, /hasBmpExtension\(filename\)/, "BMP icon routing must remain Image");
}

function validateSources(sources) {
  validateNextBookRouting(sources.nextBookSource);
  validateScreenshotRouting(
    sources.screenshotHeader,
    sources.epubReaderSource,
    sources.txtReaderSource,
    sources.xtcReaderSource,
  );
  validateIconRouting(sources.uiThemeSource);
}

module.exports = {
  loadRepository,
  validateSources,
};
