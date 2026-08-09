# PDF Open Speed Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Cut first-open PDF preparation time and SD energy use without adding RAM, borrowing the framebuffer, or weakening resume safety.

**Architecture:** Coalesce tiny fixed-record I/O, reuse phase-dead navigation memory for sequential read windows, and retain the tracked XHTML writer across consecutive pages. Apply the larger compact font snapshot only after the zero-RAM changes are proven and keep the existing disk path as fallback.

**Tech Stack:** C++17, PlatformIO/Arduino-ESP32, SdFat-backed `PdfCacheIo`, host PDF contract binaries, constrained QEMU.

---

### Task 1: Batch xref and sequential navigation I/O

**Files:**
- Modify: `lib/PdfReflow/PdfTypes.h`
- Modify: `lib/PdfReflow/PdfFixedRecordSpool.cpp`
- Modify: `lib/PdfReflow/PdfXref.h`
- Modify: `lib/PdfReflow/PdfXref.cpp`
- Modify: `lib/PdfReflow/PdfPreparation.h`
- Modify: `lib/PdfReflow/PdfPreparation.cpp`
- Test: `test/pdf_document_contract/PdfLookupContractTest.cpp`
- Test: `test/pdf_extraction/PdfPreparationTest.cpp`

- [ ] **Step 1: Add a batching witness**

Add a counting fixed-record store and assert that appending 49 records uses three physical writes when the batch capacity is 24 while record order remains exact.

- [ ] **Step 2: Verify the witness fails**

Run the focused xref contract filter and confirm it reports 49 writes rather than 3.

- [ ] **Step 3: Implement optional batch storage and bounded read windows**

Add an optional trailing callback equivalent to:

```cpp
using WriteManyFn = PdfStatus (*)(void*, uint32_t, const void*, uint32_t, size_t);
```

Flush the xref batch before finalize, adoption, compaction, or any operation that reads records. Batch sequential page records and retain future-page resolved-link records instead of discarding them.

- [ ] **Step 4: Run focused contracts and exact replay**

Build the affected host targets, run the xref/page/link filters, and replay Atomic Habits. Require identical words, sections, images, and stored-byte oracle with fewer physical opens/reads/writes.

### Task 2: Retain the section writer through the 86% page loop

**Files:**
- Modify: `lib/PdfReflow/PdfPreparation.h`
- Modify: `lib/PdfReflow/PdfPreparation.cpp`
- Test: `test/pdf_extraction/PdfPreparationTest.cpp`
- Test: `test/pdf_extraction/PdfCacheRecoveryTest.cpp`

- [ ] **Step 1: Add section durability witnesses**

Cover consecutive pages in one section, a real section boundary, Cancel with an open writer, and replay from a checkpoint whose file has a removable uncommitted tail.

- [ ] **Step 2: Verify at least the consecutive-page witness fails**

Confirm the baseline closes/syncs once per page.

- [ ] **Step 3: Retain and safely synchronize the writer**

Update the in-memory file record after each page, reserve only newly added bytes, leave the handle open for the same section, and sync at a checkpoint/boundary/Cancel. Reopen only after resume or section change.

- [ ] **Step 4: Verify recovery and performance**

Run focused recovery tests and exact Atomic normal/cancel-resume replays. Require exact output and a large reduction in phase 41-44 opens/closes/syncs.

### Task 3: Compact the font-navigation snapshot

**Files:**
- Modify: `lib/PdfReflow/PdfPreparation.h`
- Modify: `lib/PdfReflow/PdfPreparation.cpp`
- Test: `test/pdf_extraction/PdfPreparationProductGapRedTest.cpp`

- [ ] **Step 1: Prove the live-field snapshot contract**

Add compile-time capacity checks and a dense-journal fallback witness. Preserve section boundaries, page labels, links, XObject/image candidates, and image-cache state.

- [ ] **Step 2: Implement compact save and restore**

Copy only post-font live fields into phase-dead run-record, operand, and page-text tails. Reconstruct scratch/default fields before interpretation. Route pages that do not fit through the existing full SD snapshot.

- [ ] **Step 3: Apply the retention gate**

Replay Atomic and retain this task only if it improves total measured I/O or wall time by at least another 10% with exact output and no resource-peak increase.

### Task 4: Device-constrained verification and release

**Files:**
- Modify: `CHANGELOG.md`
- Modify: `platformio.ini`
- Update: `release/crossink-tiny.bin`
- Update: `release/crossink-xlarge.bin`

- [ ] **Step 1: Run the real corpus**

Replay Atomic Habits first, then the supplied manuals and papers. Record terminal phase, page/word totals, peak owned RAM, cache bytes, and preparation wall time.

- [ ] **Step 2: Run constrained QEMU**

Use the existing ESP32-C3 RAM, flash, SD-capacity, and CPU-speed limits. Require completion without watchdog, reader-overlap, or allocation failure.

- [ ] **Step 3: Build release firmware**

Run production Tiny and XLarge PlatformIO builds, verify flash/RAM reports and artifact hashes, and copy only the finished firmware binaries into `release/`.

- [ ] **Step 4: Commit, push, and replace release assets**

Stage only PDF implementation, tests/docs, version/changelog, and verified binaries. Push `main`, publish the replacement release, verify both downloads, then remove the superseded release.
