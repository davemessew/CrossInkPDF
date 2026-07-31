"use strict";

const fs = require("fs");
const path = require("path");
const { validatePathFences } = require("./path_fence_contract");

const root = path.resolve(__dirname, "..", "..");
const reader = fs.readFileSync(path.join(root, "src/activities/reader/ReaderActivity.cpp"), "utf8");
const main = fs.readFileSync(path.join(root, "src/main.cpp"), "utf8");

function expectRejected(name, mutatedReader, mutatedMain, expectedMessage) {
  if (mutatedReader === reader && mutatedMain === main) {
    throw new Error(`${name} negative control did not change source bytes`);
  }
  let errorMessage = "";
  try {
    validatePathFences(mutatedReader, mutatedMain);
  } catch (error) {
    errorMessage = error.message;
  }
  if (!errorMessage.includes(expectedMessage)) {
    throw new Error(`${name} negative control was not rejected as expected; got: ${errorMessage}`);
  }
}

expectRejected(
  "selected-PDF resolver removal",
  reader.replace("BookMoveUtils::migrationCacheHash(", "removedMigrationCacheHash("),
  main,
  "PDF reader must resolve and fence",
);

expectRejected(
  "selected-PDF read-only fence removal",
  reader.replace("if (!cacheResolved || readOnlyFallback)", "if (false)"),
  main,
  "PDF reader must resolve and fence",
);

expectRejected(
  "unrelated-PDF global reader fence reintroduction",
  reader.replace(
    "const uint64_t normalCacheHash",
    "BookMoveUtils::recoverPendingBookMove();\n  const uint64_t normalCacheHash",
  ),
  main,
  "PDF reader must not globally fence",
);

expectRejected(
  "boot selected-PDF resolver removal",
  reader,
  main.replace(
    "BookMoveUtils::migrationCacheHash(openBookPath",
    "removedMigrationCacheHash(openBookPath",
  ),
  "boot selected-PDF guard must contain ordered delete and move fences",
);

expectRejected(
  "boot non-PDF guard removal",
  reader,
  main.replace(
    "if (FsHelpers::hasPdfExtension(APP_STATE.openBookPath()))",
    "if (true)",
  ),
  "Missing function marker",
);

expectRejected(
  "boot delete fence removal",
  reader,
  main.replace(
    "PdfDeleteUtils::mutationFenceForPath(openBookPath)",
    "removedDeleteMutationFence(openBookPath)",
  ),
  "boot selected-PDF guard must contain ordered delete and move fences",
);

expectRejected(
  "boot read-only fence removal",
  reader,
  main.replace(
    "bookMoveResumeBlocked = !cacheResolved || readOnlyFallback;",
    "bookMoveResumeBlocked = false;",
  ),
  "boot selected-PDF guard must contain ordered delete and move fences",
);

process.stdout.write("BOOK_MOVE_PATH_FENCE_NEGATIVE_CONTROLS_PASS\n");
