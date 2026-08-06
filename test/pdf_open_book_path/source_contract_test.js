"use strict";

const path = require("path");
const { loadRepository, validateSources } = require("./source_contract");

const repoRoot = path.resolve(__dirname, "..", "..");
validateSources(loadRepository(repoRoot));

process.stdout.write("PDF_OPEN_BOOK_PATH_SOURCE_CONTRACT_PASS\n");
