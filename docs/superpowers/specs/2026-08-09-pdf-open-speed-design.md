# PDF preparation speed design

## Goal

Make first-open PDF preparation materially faster on the ESP32-C3, especially the long visible 86% interval, without borrowing the framebuffer, adding heap allocations, weakening Cancel, or changing EPUB behavior.

## Measured causes

The current Atomic Habits replay completes correctly but performs avoidable SD-card metadata and durability work:

- 5,334 individual 24-byte xref writes.
- One page-record reader handoff per source page.
- Resolved-link records are reread after a future-page record was already fetched.
- The active XHTML section file is closed and synced after nearly every page, then reopened for the next page.
- Font preparation writes and rereads a 14,592-byte navigation snapshot on most pages.

The displayed 86% spans most of the remaining page loop because the paint gate never shows a lower percentage. The optimization target is the real work behind it, not the progress label.

## Design

1. Batch xref appends through an optional fixed-record-store batch callback. Reuse phase-dead storage already owned by `PdfXrefTable`; fall back to the existing one-record callback for tests and non-spool stores.
2. Read sequential page records and resolved links in bounded batches using phase-dead navigation arrays. Keep the existing one-reader invariant and the 3 KiB cooperative byte slice.
3. Keep the tracked XHTML writer open while consecutive pages belong to the same section. Reserve newly written bytes per page, sync only at the existing durable checkpoint or a real section boundary, and close immediately on Cancel. Power loss still rolls back to the last validated checkpoint.
4. Replace the full font-navigation disk round trip with a compact snapshot in phase-dead workspaces. If a page's observed-glyph journal occupies that storage, automatically retain the existing SD fallback.

## Resource and safety contract

- No framebuffer use.
- No new heap allocation or repeated allocation in a loop.
- No additional readable file handle; the hardware one-reader rule remains enforced.
- No persistent PDF cache-format change unless verification proves one is required.
- Cancellation closes and syncs the active section before publishing its resume state.
- Corrupt or incomplete checkpoints continue to rebuild rather than being trusted.
- All changes remain under `lib/PdfReflow` and PDF tests/docs; EPUB and unrelated firmware stay untouched.

## Retention gates

Each optimization must preserve exact Atomic Habits output and pass its focused contracts. The compact font snapshot is retained only if it removes at least 10% additional measured preparation I/O or wall time after the lower-risk changes. Final acceptance includes constrained ESP32-C3 QEMU and Tiny/XLarge production builds.
