"use strict";

const path = require("path");
const { loadRepository, validateSources } = require("./source_contract");

const repoRoot = path.resolve(__dirname, "..", "..");
validateSources(loadRepository(repoRoot));

process.stdout.write("PDF_FORMAT_ROUTING_SOURCE_CONTRACT_PASS\n");
