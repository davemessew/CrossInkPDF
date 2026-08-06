"use strict";

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

function requireText(source, text, message) {
  if (!source.includes(text)) {
    throw new Error(message);
  }
}

function validatePathFences(readerSource, mainSource) {
  const openPdfRoute = extractFunction(readerSource, "bool ReaderActivity::openPdfRoute");
  const readerResolver = openPdfRoute.indexOf("BookMoveUtils::migrationCacheHash(");
  const readerFence = openPdfRoute.indexOf("if (!cacheResolved || readOnlyFallback)");
  const readerExists = openPdfRoute.indexOf("Storage.exists(initialBookPath.c_str())");
  const readerLoad = openPdfRoute.indexOf("loadPdfHalReflowDocumentNoThrow(");
  const readerPreparation = openPdfRoute.indexOf("makeUniqueNoThrow<PdfPrepareActivity>");
  if (!(readerResolver >= 0 && readerFence > readerResolver && readerExists > readerFence &&
        readerLoad > readerExists && readerPreparation > readerLoad)) {
    throw new Error(
      "PDF reader must resolve and fence the selected path before source access, cache load, or preparation",
    );
  }
  if (openPdfRoute.includes("BookMoveUtils::recoverPendingBookMove()") ||
      openPdfRoute.includes("MoveResult::Pending") ||
      openPdfRoute.includes("MoveResult::Conflict") ||
      openPdfRoute.includes("MoveResult::Invalid")) {
    throw new Error("PDF reader must not globally fence an unrelated PDF because another path has a pending move");
  }
  requireText(openPdfRoute, "readOnlyFallback", "PDF reader must never cross a read-only migration fence");
  requireText(openPdfRoute, "cacheHashOverride", "PDF reader must pass the resolved cache identity explicitly");

  const appLoad = mainSource.indexOf("APP_STATE.loadFromFile()");
  const recentLoad = mainSource.indexOf("RECENT_BOOKS.loadFromFile()");
  const recovery = mainSource.indexOf("BookMoveUtils::recoverPendingBookMove()");
  const resumeFenceState = mainSource.indexOf("bool bookMoveResumeBlocked = false;");
  const bootFence = mainSource.indexOf("else if (bookMoveResumeBlocked)");
  const resume = mainSource.indexOf("activityManager.goToReader(APP_STATE.openBookPath())");
  if (!(appLoad >= 0 && recentLoad > appLoad && recovery > recentLoad &&
        resumeFenceState > recovery && bootFence > resumeFenceState && resume > bootFence)) {
    throw new Error("boot recovery must resolve only the selected PDF before fencing reader resume");
  }

  const selectedPdfGuard =
    "if (FsHelpers::hasPdfExtension(APP_STATE.openBookPath()))";
  const selectedPdfFence = extractFunction(
    mainSource.slice(resumeFenceState, bootFence),
    selectedPdfGuard,
  );
  const deleteFence = selectedPdfFence.indexOf(
    "PdfDeleteUtils::mutationFenceForPath(openBookPath)",
  );
  const matchingDelete = selectedPdfFence.indexOf(
    "BookMutationFence::MatchingPending",
  );
  const indeterminateDelete = selectedPdfFence.indexOf(
    "BookMutationFence::Indeterminate",
  );
  const moveFence = selectedPdfFence.indexOf(
    "if (!bookMoveResumeBlocked && bookMoveRecoveryBlocked)",
  );
  const resumeResolver = selectedPdfFence.indexOf(
    "BookMoveUtils::migrationCacheHash(",
  );
  const readOnlyFence = selectedPdfFence.indexOf(
    "bookMoveResumeBlocked = !cacheResolved || readOnlyFallback;",
  );
  if (!(deleteFence >= 0 && matchingDelete > deleteFence &&
        indeterminateDelete > matchingDelete && moveFence > indeterminateDelete &&
        resumeResolver > moveFence && readOnlyFence > resumeResolver)) {
    throw new Error(
      "boot selected-PDF guard must contain ordered delete and move fences and fail closed on unresolved or read-only state",
    );
  }
}

module.exports = { validatePathFences };
