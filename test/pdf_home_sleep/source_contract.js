"use strict";

const fs = require("fs");
const path = require("path");

function readUtf8(filePath) {
  return fs.readFileSync(filePath, "utf8");
}

function requireContains(source, fragment, message) {
  if (!source.includes(fragment)) {
    throw new Error(message);
  }
}

function extractFunction(source, signature, label) {
  const signatureIndex = source.indexOf(signature);
  if (signatureIndex < 0) {
    throw new Error(`cannot find ${label}`);
  }
  const openBrace = source.indexOf("{", signatureIndex + signature.length);
  if (openBrace < 0) {
    throw new Error(`cannot find ${label} body`);
  }

  let depth = 0;
  for (let index = openBrace; index < source.length; ++index) {
    if (source[index] === "{") {
      ++depth;
    } else if (source[index] === "}") {
      --depth;
      if (depth === 0) {
        return source.slice(signatureIndex, index + 1);
      }
    }
  }
  throw new Error(`cannot find ${label} end`);
}

function countOccurrences(source, fragment) {
  return source.split(fragment).length - 1;
}

function indexIsInsidePdfGate(source, index) {
  const prefix = source.slice(0, index);
  const stack = [];
  const directive = /^[ \t]*#(if|ifdef|ifndef|elif|else|endif)\b([^\r\n]*)/gm;
  let match;
  while ((match = directive.exec(prefix)) !== null) {
    if (match[1] === "if" || match[1] === "ifdef" || match[1] === "ifndef") {
      stack.push(match[0].includes("CROSSINK_ENABLE_PDF"));
    } else if (match[1] === "endif") {
      stack.pop();
    }
  }
  return stack.some(Boolean);
}

function requireEveryOccurrenceInsidePdfGate(source, needle, message) {
  let found = false;
  let offset = 0;
  while (true) {
    const index = source.indexOf(needle, offset);
    if (index < 0) {
      break;
    }
    found = true;
    if (!indexIsInsidePdfGate(source, index)) {
      throw new Error(message);
    }
    offset = index + needle.length;
  }
  if (!found) {
    throw new Error(`${message}: missing ${needle}`);
  }
}

function loadRepository(repoRoot) {
  const homeRoot = path.join(repoRoot, "src", "activities", "home");
  const sleepRoot = path.join(repoRoot, "src", "activities", "boot_sleep");
  return {
    progress: readUtf8(path.join(homeRoot, "RecentBookProgress.cpp")),
    progressHeader: readUtf8(path.join(homeRoot, "RecentBookProgress.h")),
    home: readUtf8(path.join(homeRoot, "HomeActivity.cpp")),
    homeHeader: readUtf8(path.join(homeRoot, "HomeActivity.h")),
    recentList: readUtf8(path.join(homeRoot, "RecentBooksActivity.cpp")),
    recentListHeader: readUtf8(path.join(homeRoot, "RecentBooksActivity.h")),
    recentGrid: readUtf8(path.join(homeRoot, "RecentBooksGridActivity.cpp")),
    recentGridHeader: readUtf8(path.join(homeRoot, "RecentBooksGridActivity.h")),
    sleepAssets: readUtf8(path.join(sleepRoot, "SleepCoverAssets.cpp")),
    sleep: readUtf8(path.join(sleepRoot, "SleepActivity.cpp")),
    sleepHeader: readUtf8(path.join(sleepRoot, "SleepActivity.h")),
    sleepPage: readUtf8(path.join(sleepRoot, "PdfSleepPageCache.cpp")),
    sleepPageHeader: readUtf8(path.join(sleepRoot, "PdfSleepPageCache.h")),
    cachedProduct: readUtf8(path.join(repoRoot, "lib", "PdfReflow", "PdfCachedProductState.cpp")),
  };
}

function validateProductCache(progressSource) {
  if (countOccurrences(progressSource, "pdfLoadCachedProductState(") !== 1) {
    throw new Error("the Home/Sleep product seam must have exactly one cached-state loader call site");
  }

  const load = extractFunction(
    progressSource,
    "bool RecentBookProgress::PdfProductCache::load",
    "PDF product-cache load",
  );
  const resolverIndex = load.indexOf("BookMoveUtils::migrationCacheHash(");
  const reuseIndex = load.indexOf("impl_->attempted && sourcePath == impl_->loadedPath");
  const loaderIndex = load.indexOf("pdfLoadCachedProductState(");
  if (resolverIndex < 0 || reuseIndex < 0 || loaderIndex < 0 || resolverIndex > reuseIndex || reuseIndex > loaderIndex) {
    throw new Error("migration cache hash resolution must precede path-and-hash reuse and the cold cached-state loader");
  }
  requireContains(load, "readOnlyFallback", "PDF product cache must retain the migration read-only fence");
  requireContains(load, "cacheHashOverride", "PDF product cache must pass the resolved migration hash explicitly");
  requireContains(load, "pdfFormatCacheRootForHash(",
                  "PDF product cache must retain the explicitly resolved move-aware cache root");

  const hydrate = extractFunction(
    progressSource,
    "bool RecentBookProgress::hydratePdfBook",
    "PDF recent-book hydration",
  );
  const fallbackIndex = hydrate.indexOf("applyPdfFallback(");
  const cacheIndex = hydrate.indexOf("cache.load(");
  const preserveIndex = hydrate.indexOf("cache.preservesStoredFallback()");
  if (fallbackIndex < 0 || cacheIndex < 0 || preserveIndex < 0 || cacheIndex > preserveIndex ||
      preserveIndex > fallbackIndex) {
    throw new Error("PDF hydration must preserve stored metadata before applying the normal strict fallback");
  }

  for (const positiveControl of [
    "loadEpubProgressPercent",
    "loadXtcProgressPercent",
    "loadTxtProgressPercent",
  ]) {
    requireContains(progressSource, positiveControl, `non-PDF progress route lost: ${positiveControl}`);
  }

  for (const getterSignature of [
    "const char* RecentBookProgress::PdfProductCache::cacheRoot() const",
    "uint16_t RecentBookProgress::PdfProductCache::currentSection() const",
    "uint32_t RecentBookProgress::PdfProductCache::currentWord() const",
    "uint32_t RecentBookProgress::PdfProductCache::totalWords() const",
    "uint32_t RecentBookProgress::PdfProductCache::currentSectionFirstWordOrdinal() const",
    "uint32_t RecentBookProgress::PdfProductCache::currentSectionWordCount() const",
  ]) {
    const getter = extractFunction(progressSource, getterSignature, getterSignature);
    for (const forbidden of ["Storage.", "pdfLoad", "migrationCacheHash(", ".load("]) {
      if (getter.includes(forbidden)) {
        throw new Error("PDF product getters must remain const memoized reads with zero I/O");
      }
    }
  }
}

function validateHomeLifecycle(homeSource, recentListSource, recentGridSource) {
  const homeLoad = extractFunction(homeSource, "void HomeActivity::loadPdfRecentProducts", "Home PDF product load");
  requireContains(homeLoad, "hydratePdfBook(", "Home must cold-hydrate each visible PDF once");

  const homeEnter = extractFunction(homeSource, "void HomeActivity::onEnter", "Home onEnter");
  requireContains(homeEnter, "loadPdfRecentProducts();", "Home onEnter must own the PDF hydration lifecycle");

  const homeCoverLoad = extractFunction(homeSource, "void HomeActivity::loadRecentCovers", "Home cover load");
  requireContains(
    homeCoverLoad,
    "if (FsHelpers::hasPdfExtension(book.path))",
    "Home cover loading must skip PDF extraction/generation",
  );

  const listLoad = extractFunction(
    recentListSource,
    "void RecentBooksActivity::loadRecentBooks",
    "recent-list cold load",
  );
  requireContains(listLoad, "hydratePdfBook(", "recent-list cold load must hydrate PDF display metadata");

  const gridLoad = extractFunction(
    recentGridSource,
    "void RecentBooksGridActivity::loadRecentBooks",
    "recent-grid cold load",
  );
  requireContains(gridLoad, "hydratePdfBook(", "recent-grid cold load must hydrate PDF display metadata");
  requireContains(gridLoad, "pdfHydrated", "recent-grid PDF progress must be marked cold-loaded");

  const gridCovers = extractFunction(
    recentGridSource,
    "void RecentBooksGridActivity::loadPageCovers",
    "recent-grid cover load",
  );
  if (countOccurrences(gridCovers, "FsHelpers::hasPdfExtension(book.path)") < 2) {
    throw new Error("recent-grid cover probing and generation must both skip PDFs");
  }
}

function validateSleepLifecycle(sleepSource, sleepAssetsSource) {
  const sleepLoad = extractFunction(
    sleepSource,
    "void SleepActivity::loadPdfSleepProducts",
    "Sleep transient PDF product load",
  );
  if (sleepLoad.includes("hydratePdfBook(") || sleepLoad.includes("pdfComputeSourceIdentity(")) {
    throw new Error("Sleep must never invoke the live-source Home hydration loader");
  }
  requireContains(sleepLoad, "pdfSleepProductCache.load(",
                  "Sleep must use its path-hash-only transient product loader");

  const onEnter = extractFunction(sleepSource, "void SleepActivity::onEnter", "Sleep onEnter");
  if (onEnter.includes("hydratePdfBook(")) {
    throw new Error("Sleep must never invoke the live-source Home hydration loader");
  }
  requireContains(onEnter, "loadPdfSleepProducts(",
                  "Sleep onEnter must route through its transient product loader");
  const pdfOverlayBranch = extractFunction(onEnter, "if (isPdfOverlay)", "PDF overlay setup branch");
  for (const forbidden of ["storeBwBuffer(", "restoreBwBuffer(", "pdfSnapshotBeforeFallback("]) {
    if (pdfOverlayBranch.includes(forbidden)) {
      throw new Error("PDF overlay path must not snapshot or restore framebuffer clones");
    }
  }
  const productIndex = pdfOverlayBranch.indexOf("loadPdfSleepProducts(");
  const pageIndex = pdfOverlayBranch.indexOf("pdfSleepPageCache.load(");
  if (productIndex < 0 || pageIndex < 0 || productIndex > pageIndex) {
    throw new Error("PDF overlay must load its persisted page before the sleep popup");
  }

  const cover = extractFunction(
    sleepSource,
    "void SleepActivity::renderCoverSleepScreen",
    "cover sleep renderer",
  );
  requireContains(
    cover,
    "isPdf ? pdfSleepProductCache.coverPath()",
    "PDF cover sleep must use the already-hydrated full cover",
  );

  const statsRoot = extractFunction(sleepSource, "std::string bookStatsCachePathFor", "sleep stats cache route");
  requireContains(statsRoot, "FsHelpers::hasPdfExtension(path)", "sleep stats must recognize PDF cache roots");
  requireContains(statsRoot, "pdfCacheRoot != nullptr ? pdfCacheRoot",
                  "PDF sleep stats must reuse the memoized move-aware product root");
  if (countOccurrences(sleepSource, "isPdf ? pdfSleepProductCache.cacheRoot() : nullptr") !== 4) {
    throw new Error("every PDF sleep stats renderer must use the memoized move-aware product root");
  }

  const overlay = extractFunction(sleepSource, "void SleepActivity::renderOverlaySleepScreen", "overlay renderer");
  requireContains(overlay, "pdfSleepPageCache.renderTextAndRelease(renderer)",
                   "PDF overlay fallback must consume the retained text-only page");
  const pdfRebuildIndex = overlay.indexOf("if (isPdfReaderPage)");
  const restoreIndex = overlay.indexOf("renderer.restoreBwBuffer()");
  if (pdfRebuildIndex < 0 || restoreIndex < 0 || pdfRebuildIndex > restoreIndex) {
    throw new Error("PDF overlay must rebuild its persisted page before the non-PDF snapshot restore path");
  }
  const pdfRebuildBranch = extractFunction(overlay, "if (isPdfReaderPage)", "PDF overlay rebuild branch");
  if (pdfRebuildBranch.includes("restoreBwBuffer(")) {
    throw new Error("PDF overlay path must not snapshot or restore framebuffer clones");
  }
  const grayscaleDeclaration = overlay.indexOf("const bool backgroundSupportsGrayscale");
  const grayscaleDeclarationEnd = overlay.indexOf(";", grayscaleDeclaration);
  const grayscaleExpression = overlay.slice(grayscaleDeclaration, grayscaleDeclarationEnd);
  if (grayscaleDeclaration < 0 || grayscaleDeclarationEnd < 0 ||
      grayscaleExpression.includes("hasPdfExtension") || grayscaleExpression.includes(".pdf")) {
    throw new Error("PDF overlay must remain excluded from the snapshot-based grayscale pass");
  }
  requireContains(overlay, "shouldUseReaderPageBackground && backgroundSupportsGrayscale",
                  "overlay grayscale pass must remain gated by the non-PDF background capability");
  for (const positiveControl of [
    "XtcReaderActivity::drawCurrentPageToBuffer",
    "TxtReaderActivity::drawCurrentPageToBuffer",
    "EpubReaderActivity::drawCurrentPageToBuffer",
  ]) {
    requireContains(overlay, positiveControl, `non-PDF overlay route lost: ${positiveControl}`);
  }
  requireContains(cover, "const bool mayPrepareCover = !isPdf",
                  "PDF cover sleep must never run cover preparation");

  for (const [signature, prepareCall] of [
    ["void SleepActivity::renderMinimalSleepScreen", "prepareMinimalCoverForPath"],
    ["void SleepActivity::renderMinimalStatsSleepScreen", "prepareMinimalCoverForPath"],
    ["void SleepActivity::renderDashboardSleepScreen", "prepareDashboardCoverForPath"],
  ]) {
    const renderer = extractFunction(sleepSource, signature, signature);
    requireContains(renderer, "const bool isPdf =", `${signature} must select the cached PDF branch`);
    requireContains(renderer, "!isPdf &&", `${signature} must guard preparation away from PDFs`);
    requireContains(renderer, prepareCall, `${signature} lost its non-PDF preparation positive control`);
    requireContains(
      renderer,
      "pdfSleepProductCache.thumbnailPath()",
      `${signature} must use the already-hydrated PDF thumbnail`,
    );
    requireContains(renderer, "pdfCachedProgress", `${signature} must use cached PDF word progress`);
  }

  const dashboard = extractFunction(
    sleepSource,
    "void SleepActivity::renderDashboardSleepScreen",
    "dashboard sleep renderer",
  );
  requireContains(dashboard, "pdfCachedChapter", "dashboard sleep must use the cached PDF chapter");
  requireContains(
    dashboard,
    "if (!isPdf && book.coverBmpPath.empty())",
    "dashboard sleep must not restore an unvalidated persisted PDF cover",
  );

  for (const positiveControl of [
    "Epub epub(bookPath",
    "Xtc xtc(bookPath",
    "Txt txt(bookPath",
  ]) {
    requireContains(sleepAssetsSource, positiveControl, `non-PDF sleep-cover route lost: ${positiveControl}`);
  }
}

function validatePdfSleepPageCache(pageSource, pageHeaderSource) {
  for (const forbidden of ["storeBwBuffer(", "restoreBwBuffer(", "pdfSnapshotBeforeFallback("]) {
    if (pageSource.includes(forbidden) || pageHeaderSource.includes(forbidden)) {
      throw new Error("PDF sleep cache must not own framebuffer snapshot behavior");
    }
  }

  requireContains(pageHeaderSource, "MAX_SERIALIZED_PAGE_BYTES = 64U * 1024U",
                  "PDF sleep page decoding must have a fixed serialized-byte ceiling");
  requireContains(pageHeaderSource, "MAX_LAYOUT_PAGES = 4096",
                   "PDF sleep word-index scanning must have a fixed page ceiling");
  requireContains(pageHeaderSource, "MAX_PAGE_ELEMENTS = 256",
                   "PDF sleep page parsing must have a fixed outer-element ceiling");
  for (const budget of [
    "MAX_TOTAL_TEXT_BLOCKS",
    "MAX_TOTAL_WORDS",
    "MAX_TOTAL_TEXT_BYTES",
    "MAX_TOTAL_TABLE_ROWS",
    "MAX_TOTAL_TABLE_CELLS",
    "MAX_DECODED_WORK_UNITS",
  ]) {
    requireContains(pageHeaderSource, budget, `PDF sleep parser is missing global budget ${budget}`);
  }

  const select = extractFunction(pageSource, "bool selectPage", "PDF sleep word-index selection");
  requireContains(select, "pdfInspectLayoutWordIndex(", "PDF sleep sidecar must be fully validated");
  requireContains(select, "pdfFindLayoutCursor(", "PDF sleep progress must map word cursor to rendered page");
  requireContains(select, "info.firstGlobalWordOrdinal != product.currentSectionFirstWordOrdinal()",
                  "PDF sleep sidecar must match completed metadata's section start");
  requireContains(select, "info.sectionWordCount != product.currentSectionWordCount()",
                  "PDF sleep sidecar must match completed metadata's section word count");
  requireContains(select, "closeWordIndex(io, source)", "PDF sleep sidecar must close before section I/O");

  const load = extractFunction(pageSource, "bool loadPage", "PDF sleep persisted-page load");
  if (countOccurrences(load, "Storage.openFileForRead(") !== 1) {
    throw new Error("PDF sleep section layout must use exactly one open");
  }
  requireContains(load, "makeUniqueNoThrow<uint8_t[]>(serializedBytes)",
                   "PDF sleep fallback must use one checked bounded page-buffer allocation");
  requireContains(load, "serializedBytes <= MAX_SERIALIZED_PAGE_BYTES",
                   "PDF sleep fallback must reject oversized serialized pages before allocation");
  const readIndex = load.indexOf("readExact(file,");
  const closeIndex = load.indexOf("file.close()");
  const preflightIndex = load.indexOf("preflightSerializedPage(");
  if (readIndex < 0 || closeIndex < 0 || preflightIndex < 0 ||
      readIndex > closeIndex || closeIndex > preflightIndex) {
    throw new Error("PDF sleep must close the section handle before in-memory page preflight");
  }
  requireContains(load, "preflightSerializedPage(",
                   "PDF sleep fallback must bound the complete in-memory production record");
  for (const forbidden of ["Page::deserialize(", "TextBlock::deserialize(", "std::vector", "std::string path"]) {
    if (load.includes(forbidden)) {
      throw new Error("PDF sleep fallback must not allocate parser objects or strings");
    }
  }
  const preflight = extractFunction(
    pageSource,
    "bool preflightSerializedPage",
    "PDF sleep in-memory page preflight",
  );
  requireContains(preflight, "DecodeBudget budget",
                  "PDF sleep preflight must enforce aggregate decoded-work budgets");
  requireContains(preflight, "cursor.atEnd()",
                  "PDF sleep fallback must reject trailing or cross-page bytes");
  requireContains(pageSource, "checkedMultiply(",
                  "PDF sleep parser must check serialized-record multiplication");
  requireContains(pageSource, "checkedAdd(",
                  "PDF sleep parser must check serialized-record addition");

  const render = extractFunction(
    pageSource,
    "bool PdfSleepPageCache::renderTextAndRelease",
    "PDF sleep text-only render",
  );
  requireContains(render, "renderSerializedTextPage(",
                   "PDF sleep fallback must render immutable text/rule views directly");
  requireContains(render, "releasePageRecord()",
                   "PDF sleep page buffer must be released before overlay decoding");
  for (const forbidden of ["Page::", "page->", "renderImages(", "ImageBlock"]) {
    if (render.includes(forbidden)) {
      throw new Error("PDF sleep fallback must never instantiate or render retained PDF images");
    }
  }

  for (const forbidden of [
    "PdfPreparation",
    "PdfPrepareActivity",
    "PdfHalReflowDocument",
    "EpubReaderActivity",
    "streamSection",
    "resolveResource",
    "<Epub/Page.h>",
  ]) {
    if (pageSource.includes(forbidden)) {
      throw new Error(`PDF sleep persisted-page path must not depend on ${forbidden}`);
    }
  }
}

function validateSleepProductCache(pageSource, cachedProductSource) {
  const productLoad = extractFunction(
    pageSource,
    "bool PdfSleepProductCache::load",
    "Sleep transient PDF product load",
  );
  requireContains(productLoad, "pdfPathHash64(", "Sleep product cache must derive identity from the source path");
  requireContains(productLoad, "BookMoveUtils::migrationCacheHash(",
                  "Sleep product cache must honor a resolved move alias");
  requireContains(productLoad, "pdfLoadCachedProductStateForSleep(",
                  "Sleep product cache must call only the zero-source-open loader");
  for (const forbidden of ["pdfComputeSourceIdentity(", "pdfLoadCachedProductState(", "hydratePdfBook("]) {
    if (productLoad.includes(forbidden)) {
      throw new Error("Sleep product cache must never open or hydrate the PDF source");
    }
  }

  const workspaceLoad = extractFunction(
    cachedProductSource,
    "loadForSleepWithWorkspace",
    "zero-source sleep product workspace load",
  );
  requireContains(workspaceLoad, "selectCompletedManifest(io, workspace, nullptr",
                  "Sleep must select completed manifests without opening the source");
  requireContains(workspaceLoad, "workspace->sourceIdentity = selected->manifest.source",
                  "Sleep progress must bind to the selected manifest source");
  if (workspaceLoad.includes("pdfComputeSourceIdentity(")) {
    throw new Error("Sleep product cache must never open or hydrate the PDF source");
  }
}

function validatePdfCompileGates(sources) {
  for (const token of [
    "#include <memory>",
    "class PdfProductCache",
    "hydratePdfBook",
  ]) {
    requireEveryOccurrenceInsidePdfGate(
      sources.progressHeader,
      token,
      `recent-progress header PDF token must be compile-gated: ${token}`,
    );
  }
  for (const token of [
    "#include <Memory.h>",
    "#include <cstddef>",
    "#include <cstring>",
    "#include <new>",
    '#include "PdfCachedProductState.h"',
    '#include "PdfHalCacheIo.h"',
    '#include "PdfSourceIdentity.h"',
    '#include "util/BookMoveUtils.h"',
    "PDF_CACHE_DIRECTORY",
    "emptyProductValue",
    "applyPdfFallback",
    "clearPdfHydrationOutputs",
    "PdfProductCache",
    "hydratePdfBook",
    "pdfPathHash64",
    "pdfFormatCacheRootForHash",
    "pdfLoadCachedProductState",
    "BookMoveUtils::migrationCacheHash",
    "FsHelpers::hasPdfExtension",
  ]) {
    requireEveryOccurrenceInsidePdfGate(
      sources.progress,
      token,
      `recent-progress implementation PDF token must be compile-gated: ${token}`,
    );
  }
  for (const token of [
    '#include "RecentBookProgress.h"',
    "cachedBookChapters",
    "cachedPdfProducts",
    "PdfProductCache",
    "loadPdfRecentProducts",
  ]) {
    requireEveryOccurrenceInsidePdfGate(
      sources.homeHeader,
      token,
      `Home header PDF token must be compile-gated: ${token}`,
    );
  }
  for (const token of [
    "cachedPdfProducts",
    "pdfProductCache",
    "loadPdfRecentProducts",
    "hydratePdfBook",
    "FsHelpers::hasPdfExtension",
  ]) {
    requireEveryOccurrenceInsidePdfGate(
      sources.home,
      token,
      `Home implementation PDF token must be compile-gated: ${token}`,
    );
  }
  for (const [source, label] of [
    [sources.recentListHeader, "recent-list header"],
    [sources.recentGridHeader, "recent-grid header"],
  ]) {
    for (const token of [
      '#include "RecentBookProgress.h"',
      "PdfProductCache",
    ]) {
      requireEveryOccurrenceInsidePdfGate(
        source,
        token,
        `${label} PDF token must be compile-gated: ${token}`,
      );
    }
  }
  requireEveryOccurrenceInsidePdfGate(
    sources.recentList,
    "#include <FsHelpers.h>",
    "recent-list implementation PDF-only FsHelpers include must be compile-gated",
  );
  for (const [source, label] of [
    [sources.recentList, "recent-list implementation"],
    [sources.recentGrid, "recent-grid implementation"],
  ]) {
    for (const token of [
      "pdfProductCache",
      "hydratePdfBook",
      "FsHelpers::hasPdfExtension",
      "BookActions::deletePdfBook",
    ]) {
      requireEveryOccurrenceInsidePdfGate(
        source,
        token,
        `${label} PDF token must be compile-gated: ${token}`,
      );
    }
  }
  for (const token of [
    "PdfSleepPageCache.h",
    "capturePdfSleepPageLayoutForSleep",
    "loadPdfSleepProducts",
    "pdfOverlayLayout",
    "pdfSleepPageCache",
    "pdfSleepProductCache",
    "pdfCachedBook",
    "pdfCachedChapter",
    "pdfCachedProgress",
    "pdfBookHydrated",
  ]) {
    requireEveryOccurrenceInsidePdfGate(
      sources.sleepHeader,
      token,
      `Sleep header PDF token must be compile-gated: ${token}`,
    );
  }
  for (const token of [
    "PDF_PUBLISHER_PAGE_NUMBER_LEFT_MARGIN_MIN",
    "capturePdfSleepPageLayoutForSleep",
    "loadPdfSleepProducts",
    "pdfOverlayLayout",
    "pdfSleepPageCache",
    "pdfSleepProductCache",
    "pdfCachedBook",
    "pdfCachedChapter",
    "pdfCachedProgress",
    "pdfBookHydrated",
    "FsHelpers::hasPdfExtension",
  ]) {
    requireEveryOccurrenceInsidePdfGate(
      sources.sleep,
      token,
      `Sleep implementation PDF token must be compile-gated: ${token}`,
    );
  }
  requireEveryOccurrenceInsidePdfGate(
    sources.sleepPageHeader,
    "class PdfSleepProductCache",
    "PDF sleep helper declarations must be compile-gated",
  );
  requireEveryOccurrenceInsidePdfGate(
    sources.sleepPage,
    "struct PdfSleepProductCache::Impl",
    "PDF sleep helper implementation must be compile-gated",
  );
}

function validateNoPreparationDependencies(sources) {
  const combined = Object.values(sources).join("\n");
  for (const forbidden of [
    "PdfPreparation",
    "PdfPrepareActivity",
    "PdfHalReflowDocument",
    "pdfPrepare",
  ]) {
    if (combined.includes(forbidden)) {
      throw new Error(`Home/Sleep cached-product lane must not depend on ${forbidden}`);
    }
  }
}

function validateSources(sources) {
  validateProductCache(sources.progress);
  validateHomeLifecycle(sources.home, sources.recentList, sources.recentGrid);
  validateSleepLifecycle(sources.sleep, sources.sleepAssets);
  validateSleepProductCache(sources.sleepPage, sources.cachedProduct);
  validatePdfSleepPageCache(sources.sleepPage, sources.sleepPageHeader);
  validatePdfCompileGates(sources);
  validateNoPreparationDependencies(sources);
}

module.exports = {
  loadRepository,
  validateSources,
};
