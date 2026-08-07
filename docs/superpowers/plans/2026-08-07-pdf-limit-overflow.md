# PDF Limit Overflow Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace arbitrary PDF preparation failures with fixed-RAM fast paths, batched SD overflow, or page/resource-local fallback while preserving ESP32-C3 stability and EPUB-like reading.

**Architecture:** Keep existing resident arrays for ordinary PDFs. Route valid excess records through `PdfFixedRecordSpool`-style stores, consume them cooperatively in batches, and scope optional-resource failures to the resource or page. Keep one reader, the 63,488-byte workspace, cancellation, and cache cleanup invariants.

**Tech Stack:** C++17, PlatformIO/Arduino-ESP32, SdFat HAL, fixed-record PDF cache spools, CMake/GoogleTest host contracts, Docker PDF replay harness, GitHub CLI releases.

---

### Task 1: Fix reader ownership before any overflow work

**Files:**
- Modify: `lib/PdfReflow/PdfPreparation.cpp:4331-4368`
- Test: `test/pdf_document_contract/PdfDocumentContractTest.cpp`

- [ ] **Step 1: Add a one-reader regression witness**

Build a navigation fixture whose annotation lives in an object stream, resolve it, then load the next page record while the object-store reader is active. The constrained cache I/O must reject a second reader before the fix.

- [ ] **Step 2: Release resolver access before page-spool access**

At the start of `loadPageRecord()`, capture the active resolver reader, switch to a closed/no-reader state through the existing access callback, read `build.pages`, then restore the prior reader on demand. Do not close a writer or change resolver lookup state.

```cpp
const PdfObjectResolverReader previous = resolverReader_;
PdfStatus status = releaseResolverReaderForSpool();
if (status) {
  status = readPreparedPageRecord(index, navigation_->pageScratch);
}
if (status) {
  status = restoreResolverReader(previous);
}
```

- [ ] **Step 3: Verify the focused contract and exact Sony replay**

Run the one-reader contract, then `AtomicPdfRepro` on `50310861M.pdf`. Expected: the former `LimitExceeded(0)` page-3 handoff disappears and the replay reaches a later honest boundary or completes.

### Task 2: Generalize xref records, revision history, widths, and predictors

**Files:**
- Modify: `lib/PdfReflow/PdfLimits.h`
- Modify: `lib/PdfReflow/PdfXref.h`
- Modify: `lib/PdfReflow/PdfXref.cpp`
- Modify: `lib/PdfReflow/PdfStreamDecoder.h`
- Modify: `lib/PdfReflow/PdfStreamDecoder.cpp`
- Modify: `lib/PdfReflow/PdfObjectResolver.cpp`
- Test: `test/pdf_reflow_core/PdfXrefResolverTest.cpp`
- Test: `test/pdf_reflow_core/PdfStreamDecoderTest.cpp`

- [ ] **Step 1: Replace fixed xref document counts with checked storage capacity**

Keep `MaxObjectNumber` as the PDF-domain guard. Calculate the maximum record count from object domain, record size, checked `uint64_t` arithmetic, configured cache budget, and reported free SD space. Object-stream lookup scans only as far as the requested member and its following offset.

```cpp
const uint64_t bytes = uint64_t(recordCount) * sizeof(PdfXrefEntry);
if (recordCount > PdfLimits::MaxObjectNumber + 1U || bytes > availableBuildBytes) {
  return PdfStatus::failure(PdfError::InsufficientStorage, bytes);
}
```

- [ ] **Step 2: Spool `/Prev` offsets and `/Index` pairs**

Replace the 32-offset and 64-pair document ceilings with fixed records. Detect repeated `/Prev` offsets before following them. Iterate `/Index` pairs cooperatively and preserve their declared order.

- [ ] **Step 3: Accept wide zero-prefixed `W` fields**

For each xref field, discard leading zero bytes beyond the destination integer width. Return numeric overflow only if a discarded byte is non-zero.

- [ ] **Step 4: Implement PNG predictor filters 0 through 4 for the actual row width**

Use the existing source-buffer tail when the previous row fits; use a temporary spool only for exceptional row widths. Implement None, Sub, Up, Average, and Paeth byte transforms with checked row boundaries. Keep `sizeof(PdfStreamDecoder)` unchanged for normal rows.

- [ ] **Step 5: Run focused xref/decoder contracts and Sony**

Cover multi-revision chains, more than 64 `/Index` pairs, zero-prefixed wide fields, all five predictor filters, Columns other than 5/6, malformed overflow, cancellation, and allocation counters. Re-run Sony and record xref count, pages walked, terminal phase, and peak workspace.

### Task 3: Spool excess page content streams in source order

**Files:**
- Modify: `lib/PdfReflow/PdfPageTree.h`
- Modify: `lib/PdfReflow/PdfPageTree.cpp`
- Modify: `lib/PdfReflow/PdfPreparation.h`
- Modify: `lib/PdfReflow/PdfPreparation.cpp`
- Modify: `lib/PdfReflow/PdfPreparedContent.h`
- Modify: `lib/PdfReflow/PdfPreparedContent.cpp`
- Test: `test/pdf_reflow_core/PdfPageTreeTest.cpp`
- Test: `test/pdf_extraction/PdfPreparedContentTest.cpp`

- [ ] **Step 1: Reuse the annotation-overflow record pattern for `/Contents`**

Keep the current inline references in `PdfPageInfo`; add an overflow ordinal/count for later references. Append fixed `PdfObjectReference` records during page-tree walking, with checked page ownership and source ordinal.

```cpp
struct PdfPageContentOverflowRecord {
  uint32_t pageIndex;
  uint16_t ordinal;
  uint16_t reserved;
  PdfObjectReference reference;
};
```

- [ ] **Step 2: Decode inline and overflow references sequentially**

The prepared-content iterator must yield all references in original array order and preserve stream separators. Batch spool reads while respecting one-reader transitions and the existing byte/time budget.

- [ ] **Step 3: Persist resume metadata and bump the discovery format**

Record overflow counts/ordinals in the discovery page record, update version assertions and documentation data, and make cancellation resume at the next content ordinal rather than redoing the page.

- [ ] **Step 4: Verify 17+, 64+, empty, indirect-length, and cancel/resume cases**

Expected: identical concatenated text order, no document-level `LimitExceeded`, single-reader peak of one, and exact resume output.

### Task 4: Make navigation and physical sections disk-backed

**Files:**
- Modify: `lib/PdfReflow/PdfOutline.h`
- Modify: `lib/PdfReflow/PdfOutline.cpp`
- Modify: `lib/PdfReflow/PdfMetadataStore.h`
- Modify: `lib/PdfReflow/PdfPreparation.h`
- Modify: `lib/PdfReflow/PdfPreparation.cpp`
- Test: `test/pdf_navigation/*`
- Test: `test/pdf_section_contract/*`

- [ ] **Step 1: Traverse destination and page-label trees through `/Kids`**

Use an SD pending-node spool with visited-reference cycle detection. Store destination names and page-label ranges as length-prefixed/fixed-header records rather than limiting them to 16 resident entries.

- [ ] **Step 2: Continue outlines beyond resident entry/depth/title limits**

Spool outline entries and pending nodes. Flatten display depth beyond the resident UI depth without losing title/target. Truncate display titles only at a valid UTF-8 boundary; retain the destination record separately. A malformed optional item logs and is skipped.

- [ ] **Step 3: Decouple logical chapters from physical XHTML files**

Remove the 1 MiB per-section fatal check. Before appending a page, roll to a new physical file if required by checked file fields or remaining cache storage. Keep logical chapter anchors mapped to physical file plus offset. When the resident metadata section array fills, continue via fixed records rather than aborting.

- [ ] **Step 4: Verify large/deep navigation and long/no-outline documents**

Cover 257+ outlines, 17+ destinations, `/Kids` trees, deep outlines, long UTF-8 titles, 257+ logical chapters, a section beyond 1 MiB, invalid optional nodes, and cancel/resume. Expected: text always prepares; valid navigation is preserved; invalid optional records are omitted only.

### Task 5: Scope resource, parser, glyph, and operator overflow locally

**Files:**
- Modify: `lib/PdfReflow/PdfPreparedContent.h`
- Modify: `lib/PdfReflow/PdfPreparedContent.cpp`
- Modify: `lib/PdfReflow/PdfContentInterpreter.cpp`
- Modify: `lib/PdfReflow/PdfObjectParser.cpp`
- Modify: `lib/PdfReflow/PdfLexer.cpp`
- Modify: `lib/PdfReflow/PdfTypes.h`
- Modify: `lib/PdfReflow/PdfCMap.cpp`
- Modify: `lib/PdfReflow/PdfPreparation.cpp`
- Test: `test/pdf_extraction/*`
- Test: `test/pdf_reflow_core/*`

- [ ] **Step 1: Resolve resource aliases that are actually used**

Collect `Do` and `Tf` names while decoding content, retain inline entries first, and spool used overflow aliases. Permit PDF names through the 127-byte format bound. Missing/unsupported optional XObjects produce `VectorArtOmitted` or image omission and continue the page.

- [ ] **Step 2: Stop silent long-string and page-text truncation**

Honor the lexer's external-string truncation flag. Skip irrelevant dictionary values before materialization; spool relevant long strings and excess parser entries. When the resident page run/text model fills, emit ordered continuation blocks to SD and merge them during XHTML emission.

- [ ] **Step 3: Localize complexity budgets**

Keep per-page operator, Form recursion, CMap, and glyph work guards. On exhaustion, finish known mappings, use U+FFFD/default width for unknown glyphs, omit the offending Form, or skip only the page. Remove the cumulative document operator poison state.

- [ ] **Step 4: Verify fallback output and later-page recovery**

Cover more than 16 used resources, long names/strings, more than 256 glyphs/runs, large CMaps, recursive/deep Forms, and an over-budget page followed by a normal page. Expected: no crash/document abort, later page text present, and warnings scoped to affected items.

### Task 6: Make storage and security metadata length-driven

**Files:**
- Modify: `lib/PdfReflow/PdfSecurity.h`
- Modify: `lib/PdfReflow/PdfSecurity.cpp`
- Modify: `lib/PdfReflow/PdfXref.cpp`
- Modify: `lib/PdfReflow/PdfPreparation.cpp`
- Modify: `lib/PdfReflow/PdfCacheFormat.h`
- Test: `test/pdf_reflow_core/PdfSecurityTest.cpp`
- Test: `test/pdf_cache_recovery/*`

- [ ] **Step 1: Retain the complete first trailer ID**

Hash the full ID during initial parse and retain/reparse the bytes needed by Standard security setup. Resume stores the Encrypt reference and ID metadata or spool reference, never the derived file key.

- [ ] **Step 2: Replace cumulative temporary-work ceilings**

Bound each live decoded stream by its checked sink size and actual available build storage. Xref streams use `entryCount * rowWidth`; optional streams that exceed policy are omitted locally. Keep expansion-bomb detection where output size is not otherwise known.

- [ ] **Step 3: Verify security vectors, cleanup, storage failure, and resume**

Cover variable-length trailer IDs, the Sony empty-password RC4 dictionary, unsupported AES/non-empty password, injected SD full, cancel/reopen, and absence of file-key bytes in persistent records.

### Task 7: Final integration, documentation, firmware, and release

**Files:**
- Modify: `platformio.ini`
- Modify: `CHANGELOG.md`
- Modify: `README.md`
- Modify: `docs/pdf-support.md`
- Modify: `docs/file-formats.md`
- Replace: `release/firmware-tiny-pdf-reflow-v1.5.0.3.bin`
- Replace: `release/firmware-xlarge-pdf-reflow-v1.5.0.3.bin`

- [ ] **Step 1: Run focused suites and supplied-PDF corpus**

Rebuild the host harness, run focused changed-area contracts, then every supplied PDF. Record pages, words, sections, terminal status, workspace peak, reader peak, and preparation counters. Atomic Habits must retain 75,007 words and no material performance regression; Sony must complete with non-zero text.

- [ ] **Step 2: Run cancellation/resume and cleanup replays**

Cancel Atomic and Sony during discovery and finalization, then resume. Expected: completed output exactly once, no leaked handle, no restart from zero after a committed checkpoint, and partial files removed on injected failure.

- [ ] **Step 3: Update public and format documentation**

Set version `1.5.0.3`; document passwordless RC4 scope, overflow behavior, current cache/discovery/resume/image-spool versions and record sizes, and user-facing fixes. Keep README prose in the existing CrossInk style and the release download prominent.

- [ ] **Step 4: Build Tiny and XLarge sequentially**

Run `.venv\Scripts\pio.exe run -e tiny -j 8`, then `xlarge`. Copy `firmware.bin` to the versioned release paths. Capture SHA-256, exact byte sizes, map/DRAM usage, and confirm no framebuffer symbol is referenced by PDF preparation.

- [ ] **Step 5: Review, commit, push, and publish replacement release**

Review the complete PDF-only diff against the design, stage only intended tracked files and the two binaries, commit without AI attribution, and push `main`. Create `pdf-reflow-v1.5.0.3`, verify both assets download and match local hashes, then delete release/tag `pdf-reflow-v1.5.0.2`.
