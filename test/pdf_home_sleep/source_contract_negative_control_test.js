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
  "path-and-hash PDF reuse removal",
  (sources) => {
    sources.progress = sources.progress.replace(
      "impl_->attempted && sourcePath == impl_->loadedPath",
      "false",
    );
  },
  "migration cache hash resolution must precede",
);

expectRejected(
  "migration cache resolution removal",
  (sources) => {
    sources.progress = sources.progress.replace("BookMoveUtils::migrationCacheHash(", "removedMigrationCacheHash(");
  },
  "migration cache hash resolution must precede",
);

expectRejected(
  "read-only stored metadata preservation removal",
  (sources) => {
    sources.progress = sources.progress.replace("cache.preservesStoredFallback()", "false");
  },
  "PDF hydration must preserve stored metadata",
);

expectRejected(
  "Home cold hydration removal",
  (sources) => {
    sources.home = sources.home.replace("cachedPdfProducts[i] = RecentBookProgress::hydratePdfBook(", "false && (");
  },
  "Home must cold-hydrate each visible PDF once",
);

expectRejected(
  "PDF cover preparation guard removal",
  (sources) => {
    sources.sleep = sources.sleep.replace("const bool mayPrepareCover = !isPdf", "const bool mayPrepareCover = true");
  },
  "PDF cover sleep must never run cover preparation",
);

expectRejected(
  "PDF snapshot-first removal",
  (sources) => {
    sources.sleep = sources.sleep.replace(
      "pdfSnapshotBeforeFallback(",
      "pdfFallbackBeforeSnapshot(",
    );
  },
  "Sleep onEnter must use the executable snapshot-first seam",
);

expectRejected(
  "PDF stats move-aware root removal",
  (sources) => {
    sources.sleep = sources.sleep.replaceAll(
      "isPdf ? pdfSleepProductCache.cacheRoot() : nullptr",
      "nullptr",
    );
  },
  "every PDF sleep stats renderer must use the memoized move-aware product root",
);

expectRejected(
  "Sleep zero-source loader removal",
  (sources) => {
    sources.sleepPage = sources.sleepPage.replace(
      "pdfLoadCachedProductStateForSleep(",
      "pdfLoadCachedProductState(",
    );
  },
  "zero-source-open loader",
);

expectRejected(
  "Sleep progress manifest binding removal",
  (sources) => {
    sources.cachedProduct = sources.cachedProduct.replace(
      "workspace->sourceIdentity = selected->manifest.source",
      "workspace->sourceIdentity = {}",
    );
  },
  "Sleep progress must bind to the selected manifest source",
);

expectRejected(
  "recent-progress header PDF gate removal",
  (sources) => {
    sources.progressHeader = sources.progressHeader.replace(
      "#if defined(CROSSINK_ENABLE_PDF) && CROSSINK_ENABLE_PDF",
      "#if 1",
    );
  },
  "recent-progress header PDF token must be compile-gated",
);

expectRejected(
  "recent-progress implementation PDF gate removal",
  (sources) => {
    sources.progress = sources.progress.replace(
      "#if defined(CROSSINK_ENABLE_PDF) && CROSSINK_ENABLE_PDF",
      "#if 1",
    );
  },
  "recent-progress implementation PDF token must be compile-gated",
);

expectRejected(
  "Home header PDF gate removal",
  (sources) => {
    sources.homeHeader = sources.homeHeader.replace(
      "#if defined(CROSSINK_ENABLE_PDF) && CROSSINK_ENABLE_PDF",
      "#if 1",
    );
  },
  "Home header PDF token must be compile-gated",
);

expectRejected(
  "Home implementation PDF gate removal",
  (sources) => {
    sources.home = sources.home.replace(
      "#if defined(CROSSINK_ENABLE_PDF) && CROSSINK_ENABLE_PDF",
      "#if 1",
    );
  },
  "Home implementation PDF token must be compile-gated",
);

expectRejected(
  "recent-list header PDF gate removal",
  (sources) => {
    sources.recentListHeader = sources.recentListHeader.replace(
      "#if defined(CROSSINK_ENABLE_PDF) && CROSSINK_ENABLE_PDF",
      "#if 1",
    );
  },
  "recent-list header PDF token must be compile-gated",
);

expectRejected(
  "recent-list implementation PDF gate removal",
  (sources) => {
    sources.recentList = sources.recentList.replace(
      "#if defined(CROSSINK_ENABLE_PDF) && CROSSINK_ENABLE_PDF",
      "#if 1",
    );
  },
  "recent-list implementation",
);

expectRejected(
  "recent-grid header PDF gate removal",
  (sources) => {
    sources.recentGridHeader = sources.recentGridHeader.replace(
      "#if defined(CROSSINK_ENABLE_PDF) && CROSSINK_ENABLE_PDF",
      "#if 1",
    );
  },
  "recent-grid header PDF token must be compile-gated",
);

expectRejected(
  "recent-grid implementation PDF gate removal",
  (sources) => {
    sources.recentGrid = sources.recentGrid.replace(
      "#if defined(CROSSINK_ENABLE_PDF) && CROSSINK_ENABLE_PDF",
      "#if 1",
    );
  },
  "recent-grid implementation PDF token must be compile-gated",
);

expectRejected(
  "Sleep header PDF gate removal",
  (sources) => {
    sources.sleepHeader = sources.sleepHeader.replace(
      "#if defined(CROSSINK_ENABLE_PDF) && CROSSINK_ENABLE_PDF",
      "#if 1",
    );
  },
  "Sleep header PDF token must be compile-gated",
);

expectRejected(
  "Sleep implementation PDF gate removal",
  (sources) => {
    sources.sleep = sources.sleep.replace(
      "#if defined(CROSSINK_ENABLE_PDF) && CROSSINK_ENABLE_PDF",
      "#if 1",
    );
  },
  "Sleep implementation PDF token must be compile-gated",
);

expectRejected(
  "PDF sidecar validation removal",
  (sources) => {
    sources.sleepPage = sources.sleepPage.replace("pdfInspectLayoutWordIndex(", "removedSidecarInspect(");
  },
  "PDF sleep sidecar must be fully validated",
);

expectRejected(
  "PDF sidecar metadata range binding removal",
  (sources) => {
    sources.sleepPage = sources.sleepPage.replace(
      "info.sectionWordCount != product.currentSectionWordCount()",
      "false",
    );
  },
  "PDF sleep sidecar must match completed metadata's section word count",
);

expectRejected(
  "PDF nested record preflight removal",
  (sources) => {
    sources.sleepPage = sources.sleepPage.replace(
      "preflightSerializedPage(pageRecord.get(), serializedBytes)",
      "true",
    );
  },
  "PDF sleep must close the section handle before in-memory page preflight",
);

expectRejected(
  "PDF text-only render removal",
  (sources) => {
    sources.sleepPage = sources.sleepPage.replace(
      "renderSerializedTextPage(impl_->pageRecord.get()",
      "renderSerializedImagesPage(impl_->pageRecord.get()",
    );
  },
  "PDF sleep fallback must render immutable text/rule views directly",
);

expectRejected(
  "PDF exact page-boundary validation removal",
  (sources) => {
    const signature = "bool preflightSerializedPage";
    const start = sources.sleepPage.indexOf(signature);
    const before = sources.sleepPage.slice(0, start);
    const body = sources.sleepPage.slice(start).replace("cursor.atEnd()", "true");
    sources.sleepPage = before + body;
  },
  "PDF sleep fallback must reject trailing or cross-page bytes",
);

expectRejected(
  "PDF page-buffer release removal",
  (sources) => {
    const release = sources.sleepPage.lastIndexOf("impl_->releasePageRecord();");
    sources.sleepPage =
      sources.sleepPage.slice(0, release) +
      "removedPageRecordRelease();" +
      sources.sleepPage.slice(release + "impl_->releasePageRecord();".length);
  },
  "PDF sleep page buffer must be released before overlay decoding",
);

expectRejected(
  "EPUB progress positive-control removal",
  (sources) => {
    sources.progress = sources.progress.replaceAll("loadEpubProgressPercent", "removedEpubProgress");
  },
  "non-PDF progress route lost",
);

process.stdout.write("PDF_HOME_SLEEP_NEGATIVE_CONTROLS_PASS\n");
