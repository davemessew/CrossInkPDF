# PDF Overflow And Compatibility Design

## Goal

Make on-device PDF preparation accept valid documents without failing because a fixed in-memory table, counter, or temporary-file ceiling was exceeded. Keep preparation fast on ordinary files, preserve EPUB-like reflow, and remain stable on the ESP32-C3.

## Constraints

- Keep the existing 63,488-byte PDF preparation workspace.
- Do not allocate another framebuffer or borrow the display framebuffer.
- Do not add unbounded heap allocation or per-page heap churn.
- Keep one SD reader open at a time.
- Preserve text order, content-stream order, chapters, links, page labels, and word-count progress where the PDF provides them.
- Cancellation and resume must remain valid across overflow paths.
- PDF failures must close the document and return to the library; they must not crash or restart the device.
- Do not change EPUB or other non-PDF reader behavior.

## Decision

Use fixed RAM as a fast path and fixed-record SD spools as the overflow path. Ordinary PDFs retain the current low-I/O path. A valid document is not rejected merely because it exceeds an in-memory table. Optional content that cannot be decoded safely is omitted locally with a warning instead of aborting the book.

We explicitly reject two alternatives:

1. Raising constants only postpones the next failure and wastes scarce RAM.
2. A generic disk-backed PDF object model would make every file slower and substantially increase code size and maintenance cost.

## Hard Limits That Remain

The following are correctness or physical-resource guards rather than arbitrary compatibility limits:

- checked integer arithmetic and source-range validation;
- the PDF object-number and generation-number domains;
- page-tree, xref, name-tree, and Form cycle detection;
- bounded parser and Form recursion with local omission on overflow;
- decompression-bomb protection and per-slice CPU/time/cancellation gates;
- the single-reader SD rule;
- actual free-space exhaustion with a recovery reserve;
- the supported passwordless Standard RC4 security profile; unsupported encryption closes the PDF safely.

## Data Flow

### Fixed RAM, SD overflow

Small arrays remain the normal path. When one fills, additional fixed-size records are appended to a PDF build spool. Consumers read those records in batches through the existing workspace. Records carry stable ordinals so sorting or batching cannot change source order.

This pattern applies to:

- page `/Contents` references;
- xref `/Index` pairs and `/Prev` history;
- outline traversal, outline entries, named destinations, and page-label ranges;
- used font and XObject resource aliases;
- parser values, dictionary entries, arrays, and relevant long strings;
- extracted page text/runs when the resident page model fills.

Every spool is covered by the existing build cleanup and cancellation paths. Persistent spool formats are versioned and documented when changed.

### Xref and stream decoding

Xref capacity is derived from the PDF object-number domain and available SD space, not a document-wide record constant. Wide `W` fields discard only leading zero bytes and reject non-zero numeric overflow. PNG predictor filters 0 through 4 accept the actual xref row width. Extra filter stages are processed sequentially through alternating temporary stores when they cannot share the resident decoder workspace.

Decompression remains bounded by the output sink and available storage. Xref streams have a checked expected output size. Temporary decoded-stream work is bounded per live stream rather than by an arbitrary cumulative lifetime total.

### Page content and resources

The first content/resource records stay inline. Overflow references are spooled and consumed in original order. Resource discovery prioritizes aliases actually used by `Do` and `Tf`, avoiding work for unused dictionary entries.

Unsupported or excessively complex optional images and Forms emit a warning and are skipped locally. A malformed or over-budget page is skipped without poisoning later pages. Text-bearing resources use the spool path instead of silent truncation wherever the source remains readable.

### Navigation and sections

Outline and name-tree traversal uses SD-backed pending and result records. Excess depth is flattened for display while keeping the title and target. Long display titles are truncated at a valid UTF-8 boundary, but their target records remain intact.

Physical XHTML files roll at page boundaries before storage or field-size exhaustion. Logical chapters and anchors remain independent of physical file boundaries, so more chapters or long chapters do not abort preparation. If a navigation item itself is malformed, it is omitted while text preparation continues.

### Security and reader ownership

The current passwordless RC4 path remains fixed-state and heap-free. Trailer IDs are retained without assuming a 16-byte length, and the derived key is never persisted. Any transition from the PDF or object-stream reader to an SD spool explicitly releases the active resolver reader; reopening happens on demand.

## Error Handling

- Corrupt mandatory structure: close the PDF and report a preparation error.
- Unsupported encryption: close the PDF and report it as unsupported.
- Optional image, Form, link, label, or outline failure: warn and omit only that item.
- Page-local operator, CMap, glyph, or recursion budget: use fallback text where possible, otherwise skip only that page or resource.
- SD full: stop cleanly, remove partial build files, retain a valid resume point when possible, and return to the library.
- Cancellation: close every handle and resume from the last committed record/page without restarting completed discovery.

## Performance And Energy

- Ordinary PDFs stay on the fixed-RAM fast path.
- Overflow records are written and read in batches, not one handle operation per record.
- Durability syncs occur at committed batch boundaries, not after every record.
- Existing xref lookup windows, section batching, font batching, and page-tree handoff optimizations remain.
- No new work is performed for EPUBs or other formats.

## Verification

Verification is proportional but strict because this is a crash-prone parser and persisted-cache change:

1. Focused contracts for each new overflow path, one-reader transitions, cleanup, cancellation, and format versions.
2. Exact preparation replay of every supplied PDF, including Atomic Habits and the Sony manual, with non-zero text/chapter output and terminal completion.
3. Cancel/resume replay during discovery and finalization.
4. Resource assertions: 63,488-byte preparation workspace, no framebuffer borrowing, no new unbounded heap allocation, and one-reader enforcement.
5. Sequential Tiny and XLarge PlatformIO builds, binary size/hash capture, release upload, and download verification.

## Completion Criteria

- Every supplied PDF prepares or, for genuinely unsupported encryption/structure, exits cleanly with a specific non-crashing error.
- The Sony manual completes after the reader-handoff and overflow work.
- Atomic Habits retains its word count and shows no material preparation-speed regression.
- Chapters, links, index/navigation, and word-count reading progress are present where available.
- Tiny and XLarge binaries build, are committed, published in the replacement GitHub release, and the unusable prior release is removed only after the replacement assets are verified.
