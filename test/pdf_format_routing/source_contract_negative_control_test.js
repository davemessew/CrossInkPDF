"use strict";

const path = require("path");
const { loadRepository, validateSources } = require("./source_contract");

const repoRoot = path.resolve(__dirname, "..", "..");

function expectRejected(name, mutate, expectedMessage) {
  const sources = loadRepository(repoRoot);
  mutate(sources);
  let errorMessage = "";
  try {
    validateSources(sources);
  } catch (error) {
    errorMessage = error.message;
  }
  if (!errorMessage.includes(expectedMessage)) {
    throw new Error(`${name} negative control was not rejected as expected; got: ${errorMessage}`);
  }
}

expectRejected(
  "next-book PDF removal",
  (sources) => {
    sources.nextBookSource = sources.nextBookSource.replace(
      /\s*\|\|\s*FsHelpers::hasPdfExtension\(name\)/,
      "",
    );
  },
  "next-book routing must retain hasPdfExtension",
);

expectRejected(
  "screenshot PDF misclassification",
  (sources) => {
    sources.epubReaderSource = sources.epubReaderSource.replace(
      "info.readerType = ScreenshotInfo::ReaderType::Pdf;",
      "info.readerType = ScreenshotInfo::ReaderType::Epub;",
    );
  },
  "EPUB screenshot-info must report Pdf only",
);

expectRejected(
  "screenshot enum renumbering",
  (sources) => {
    sources.screenshotHeader = sources.screenshotHeader.replace(
      "None, Epub, Txt, Xtc, Pdf",
      "None, Epub, Pdf, Txt, Xtc",
    );
  },
  "must append Pdf without renumbering",
);

expectRejected(
  "PDF icon removal",
  (sources) => {
    sources.uiThemeSource = sources.uiThemeSource.replace(
      /\s*\|\|\s*FsHelpers::hasPdfExtension\(filename\)/,
      "",
    );
  },
  "book icon routing must include hasPdfExtension",
);

process.stdout.write("PDF_FORMAT_ROUTING_NEGATIVE_CONTROLS_PASS\n");
