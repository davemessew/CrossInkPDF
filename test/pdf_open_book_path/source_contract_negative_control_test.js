"use strict";

const path = require("path");
const { loadRepository, validateSources } = require("./source_contract");

const repoRoot = path.resolve(__dirname, "..", "..");
const sources = loadRepository(repoRoot);
sources.semanticSources.push({
  path: "src/InjectedLegacyConsumer.cpp",
  source: "const auto& path = APP_STATE.openEpubPath;\n",
});

let rejected = false;
try {
  validateSources(sources);
} catch (error) {
  rejected = /InjectedLegacyConsumer\.cpp must use openBookPath\(\)/.test(error.message);
}

if (!rejected) {
  throw new Error("negative control failed to reject a semantic legacy-member consumer");
}

process.stdout.write("PDF_OPEN_BOOK_PATH_NEGATIVE_CONTROL_PASS\n");
