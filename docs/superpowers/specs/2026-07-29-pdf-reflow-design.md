# On-Device Reflowable PDF Support

**Status:** Approved
**Date:** 2026-07-29
**Target:** CrossInk on Xteink X4 / ESP32-C3
**Source baseline:** `main` at `54ba245f`

## 1. Decision Summary

CrossInk will accept `.pdf` files and turn supported PDFs into a reflowable
reading stream on the ESP32-C3 itself. It will not render fixed PDF pages.
Absolute PDF typography and page geometry will not control the reading view.
CrossInk's selected device font, font size, margins, line spacing, orientation,
image settings, and other EPUB reader settings will.

The first open performs a bounded, resumable foreground extraction into an
SD-card cache. Once that cache is complete, the PDF uses the existing EPUB
reflow and reader facilities through a narrow shared reflow-document
abstraction. Page turns never reopen or parse the source PDF.

The feature must pass both:

1. the CrossInk native simulator for reader behavior and visual flow; and
2. an Espressif QEMU ESP32-C3 target build for real RISC-V execution, target
   allocation behavior, watchdog safety, and constrained-memory tests.

No physical device is flashed as part of implementation or acceptance.

## 2. Existing Constraints and Integration Evidence

The design follows these concrete repository constraints:

- The device has no PSRAM, about 380 KB usable RAM, a single-core ESP32-C3, and
  one 48,000-byte framebuffer (`AGENTS.md:11`, `AGENTS.md:40-43`).
- Local stack allocations above 256 bytes require justification, hot loops
  should avoid heap churn, and fallible ownership must use
  `makeUniqueNoThrow` rather than bare `new` (`AGENTS.md:47-57`).
- App code must use HAL file access, explicitly close files, and respect the
  real-storage single-reader constraint (`AGENTS.md:43`, `AGENTS.md:59-64`).
- PDF is currently excluded from browser metadata and supported-file routing
  (`src/activities/home/FileBrowserActivity.cpp:83-98`).
- Reader routing currently recognizes XTC and text explicitly and sends every
  remaining book path to the EPUB loader
  (`src/activities/reader/ReaderActivity.cpp:134-179`).
- EPUB pagination is already serialized to disk and records allocation and
  timing diagnostics while building pages
  (`lib/Epub/Epub/Section.cpp:88-125`).
- EPUB metadata already represents a hierarchical table of contents with
  title, destination, level, and spine index
  (`lib/Epub/Epub/BookMetadataCache.h:20-43`).
- Existing rendered pages can carry publisher page markers
  (`lib/Epub/Epub/Page.h:122-136`).
- Images already have a compact disk cache containing 2-bit grayscale pixels
  and are rendered in small row batches
  (`lib/Epub/Epub/blocks/ImageBlock.cpp:11-15`,
  `lib/Epub/Epub/blocks/ImageBlock.cpp:115-185`).
- The main loop already races active work at normal speed and returns to a
  10 MHz power-saving state after inactivity (`src/main.cpp:998-1013`,
  `lib/hal/HalPowerManager.h:32`).
- PlatformIO is the build source of truth. The current native simulator is the
  `simulator` environment (`AGENTS.md:103-115`, `platformio.ini:141-190`), but
  its documented setup is presently macOS/Apple-Silicon-specific
  (`docs/simulator.md:8-17`).
- CrossInk's current scope rejects fixed-page PDF rendering because panning and
  zooming make it unsuitable for the display (`SCOPE.md:46-50`). This design
  addresses that UX objection by extracting semantic reading content instead
  of adding a fixed-page renderer.

## 3. Goals

1. Open a supported PDF directly from the SD card with no desktop or upload
   conversion step.
2. Present the PDF as reflowable text using the same reader typography and
   layout controls as EPUB.
3. Preserve document order, meaningful inline images, outline chapters,
   internal links, contents/index material, original page labels, bookmarks,
   clippings where applicable, and reading position.
4. Linearize columns and flatten table-like material into a readable row-major
   stream.
5. Support born-digital PDFs and OCRed PDFs that contain a usable hidden text
   layer.
6. Keep first-open work bounded, interruptible, resumable, allocation-safe, and
   efficient on the ESP32-C3.
7. Count extracted words once and use reached words divided by total words for
   PDF reading progress.
8. Fail safely and clearly for image-only scans, encryption, unsupported text
   encodings, malformed data, decompression limits, and memory limits.
9. Prove target execution in ESP32-C3 QEMU without risking physical hardware.

## 4. Non-Goals

- Fixed-page PDF display, zoom, crop, or pan.
- Pixel-identical reproduction of PDF typography or geometry.
- On-device OCR.
- Password entry, decryption, signatures, forms, JavaScript, audio, video,
  attachments, editing, or annotation rendering.
- Exact reconstruction of arbitrary magazine layouts, equations, vector art,
  charts, or complex spanning tables.
- Bundling MuPDF, PDFium, Poppler, or another full page-rendering engine.
- Claiming physical battery-current savings from simulator timings.
- Flashing a physical X4 during development or acceptance.

## 5. Reader Experience

### 5.1 Opening a PDF

`.pdf` becomes a supported browser and recent-book extension. Selecting it
routes to an explicit PDF loader; it must never fall through to the EPUB
loader.

On an uncached first open, the display shows a translated, static preparation
message such as “Preparing PDF…”. The user may cancel between bounded work
slices. Cancellation closes all files and leaves a resumable cache checkpoint.

On a cache candidate, CrossInk opens the source exactly once, reads only the
bounded identity regions at the beginning and end, then closes it. A valid
cache opens through the normal reflow reader path with latency comparable to a
cached EPUB. No page turn opens the PDF.

### 5.2 Reflow Semantics

- PDF point sizes, page dimensions, coordinates, and font family names never
  become device font sizes or screen coordinates.
- Device reader settings determine the rendered typography.
- PDF styling may contribute only semantic hints such as heading, paragraph,
  emphasis, strong text, list item, caption, or code-like text.
- Original PDF page boundaries do not force reader page breaks.
- Original page labels are retained as publisher-page markers so contents and
  index references remain usable.
- Multi-column pages are emitted column-by-column in reading order.
- Table-like regions are emitted row-by-row. Cells are separated with a small
  stable delimiter or labelled blocks; the firmware does not attempt a
  horizontally scrolling table.
- Repeated headers, footers, running page numbers, watermarks, and decorative
  background material are suppressed when a conservative repeated-pattern
  test identifies them.

### 5.3 Unsupported Documents

User-visible failures use translated strings and preserve the original file:

- Image-only scan: “This PDF has no readable text. Add OCR text and try again.”
- Encrypted PDF: “Password-protected PDFs are not supported.”
- Unsupported text mapping: “This PDF’s text encoding is not supported.”
- Damaged or unsafe PDF: “This PDF could not be read safely.”
- Insufficient memory or storage: “Not enough free memory or storage to prepare
  this PDF.”

A mixed document may open when it has enough valid reading text. Unsupported
individual images or isolated undecodable text runs are omitted, logged, and
summarized once as “Some content could not be read.” The extractor must not
emit obvious binary or replacement-character garbage as book text.

## 6. Selected Architecture

```text
source.pdf
    |
    v
bounded PDF structure + text/image extractor
    |
    v
/.crosspoint/pdf_<path-hash>/gen_<sequence>/
  semantic sections + outline + links + word index + images
    |
    | CRC-protected two-slot manifest commit
    v
/.crosspoint/pdf_<path-hash>/manifest.{a,b}
    |
    v
shared ReflowDocument / ReflowSectionSource interfaces
    |
    v
existing HTML-to-Page section cache and EPUB reader UI
```

### 6.1 Why This Approach

The selected approach is a purpose-built, bounded semantic extractor plus
reuse of CrossInk's reflow renderer.

Rejected alternatives:

- **Fixed page rasterization:** directly contradicts the requested reading
  experience and the existing scope rationale.
- **Full PDF engine:** substantially exceeds the expected flash, heap, stack,
  and CPU budget; full engines also bring unsuitable rendering machinery and,
  in some cases, licensing constraints.
- **TXT-reader handoff:** loses the established EPUB chapter, link, image,
  bookmark, clipping, style, and relayout behavior.
- **Generate a complete EPUB ZIP on device:** feasible but adds a second
  container/indexing pass, extra storage traffic, and awkward original-PDF
  identity/progress mapping.
- **Duplicate `EpubReaderActivity`:** would fork a large reader surface and make
  future navigation and rendering fixes diverge.

### 6.2 Shared Reflow Interfaces

The EPUB-specific reader coupling will be narrowed rather than copied.

`ReflowDocument` supplies only what the reader needs:

- original document path and format;
- cache path and cache identity;
- title/author/language/cover where available;
- semantic section count and section metadata;
- hierarchical TOC entries and anchor resolution;
- access to bookmarks, clippings, progress, and publisher-page labels;
- access to a `ReflowSectionSource`.

`ReflowSectionSource` supplies:

- a bounded stream for one cached semantic XHTML section;
- resolution of a cached image or link target;
- stable semantic block anchors and their cumulative word offsets.

`Epub` adapts its existing ZIP-backed implementation to these interfaces.
`PdfReflowDocument` exposes only completed loose cache files. `Section` and
the reader depend on the narrow interfaces, not on PDF internals.

The abstraction must not make the EPUB hot path more allocation-heavy. Virtual
dispatch is acceptable at section-level boundaries, not per glyph or word.

### 6.3 PDF Extraction Components

Production code is separated into bounded components:

- **PDF structure reader:** header, trailers, cross-reference tables/streams,
  incremental updates, indirect objects, object streams, page tree, inherited
  resources, and bounded dictionary/array/token parsing.
- **Stream decoder:** unfiltered, Flate, ASCIIHex, and ASCII85 streams are
  required, including bounded ASCII-to-Flate chains. A chain contains at most
  four filters. Unsupported filters fail an affected required stream or omit an
  optional image.
- **Font mapper:** `/ToUnicode` CMaps and common simple-font encodings.
- **Content interpreter:** text state, text-showing operators, transforms,
  marked content, inline images, image XObjects, and link annotations.
- **Reading-order reducer:** page-local geometry to semantic blocks.
- **Outline/link resolver:** outline hierarchy, named destinations, explicit
  destinations, page labels, and internal links.
- **Cache writer:** semantic XHTML, metadata, word checkpoints, images, and
  resumable build state.

The parser executes no PDF actions, scripts, launch targets, or embedded files.
All counts, offsets, lengths, recursion, nesting, stream expansion, image
dimensions, and arithmetic are checked before allocation or seeking.

## 7. Supported PDF Boundary

### 7.1 Required Structural Support

- Common `%PDF-1.x` documents.
- Classic cross-reference tables.
- Cross-reference streams and compressed object streams.
- Incremental-update trailer chains, with a hard traversal cap and cycle
  detection.
- Page trees and inherited resources.
- Single or array-valued page content streams.
- Uncompressed, Flate, ASCIIHex, and ASCII85 required content, including
  bounded ASCII-to-Flate filter chains.
- Linearized PDFs treated as ordinary PDFs; linearization hints are ignored.

### 7.2 Required Text Support

The content interpreter handles at least:

- `BT`, `ET`;
- `Tf`, `Tm`, `Td`, `TD`, `T*`;
- `Tc`, `Tw`, `Tz`, `TL`, `Ts`, `Tr`;
- `Tj`, `TJ`, `'`, `"`;
- `cm`, `q`, `Q`;
- marked-content sequences including `/ActualText`;
- form XObjects used by page content, with bounded recursion;
- simple-font `/Widths` and `/FirstChar`, plus CID `/W` and `/DW`, as
  extraction-only geometry inputs;
- text clipping/render modes only to classify normal visible text and hidden
  OCR candidates, never to reproduce page appearance.

Mapping priority:

1. `/ActualText`;
2. `/ToUnicode`;
3. standard/simple font encoding with explicit differences;
4. conservative common Latin fallback.

Type0/CID fonts without a usable mapping are not guessed broadly. If they make
up the meaningful text, the document reports unsupported encoding.

OCRed PDFs work when their hidden layer supplies mappable text. Render-mode-3
text is retained as an OCR candidate only when it has a nonzero on-page
transform and plausibly overlaps page imagery. It is deduplicated against
visible text using normalized content and geometry. Off-page, zero-size, or
metadata-like hidden text is rejected. Image-only scans are detected after
bounded sampling and full extraction confirms no meaningful text. No OCR model
or image-to-text path is included.

### 7.3 Explicit Initial Exclusions

- Encrypted PDFs, including empty-password encryption.
- LZW-only required content unless a later measured fixture justifies the
  decoder.
- JPEG 2000, JBIG2, or CCITT-only images when no supported text alternative is
  present.
- Fonts that require executing arbitrary embedded font programs to infer text.
- Documents whose required object graph, expanded streams, or per-page
  complexity exceed configured safety caps.

The boundary is capability-based, not a promise that every syntactically valid
PDF will reflow correctly.

## 8. Reading Order and Semantic Reduction

Coordinates exist only during extraction.

For each page:

1. Decode visible text runs and qualified hidden-OCR candidates into bounded
   records containing text, baseline, bounding range, direction, style hints,
   source order, visibility class, and destination data.
2. Normalize rotation and text transforms.
3. Cluster runs into lines using baseline tolerance derived from that page's
   median glyph height.
4. Detect stable vertical whitespace bands with a fixed-size histogram.
5. If column confidence passes a conservative threshold, emit columns
   left-to-right and lines top-to-bottom. Otherwise retain a top-to-bottom,
   left-to-right ordering with source-order tie-breaking.
6. Detect table-like regions from repeated aligned x positions and short
   line fragments, then emit rows top-to-bottom and cells left-to-right.
7. Merge lines into paragraphs using gap, indentation, punctuation, and
   continuation cues.
8. Apply small semantic tags; never emit absolute CSS dimensions.
9. Attach images to the nearest following or containing semantic block.
10. Emit a publisher-page marker without forcing a section or page break.

The reducer uses fixed-capacity page-local structures. If a page exceeds the
in-memory run cap, run metadata and text spill sequentially to a bounded
temporary work file under the inactive generation directory. Before that spill
is read for ordering, the source PDF reader is closed; it is reopened and seeks
to the next verified object only after reduction finishes. It must never grow
an unbounded vector to match malicious or unusually dense page content.

Heading inference uses outline destinations first. In documents without an
outline, conservative relative cues may identify headings, but the original
point size is never rendered. If there is no reliable heading structure, the
document remains one chapter rather than inventing a chapter for every PDF
page.

## 9. Chapters, Contents, Index, and Links

- Preserve the PDF outline hierarchy, titles, levels, and destinations.
- Map named and explicit destinations to stable semantic block anchors.
- Use outline destinations to define semantic section boundaries without
  inserting a visible original-page break.
- Preserve clickable internal content and index links when their annotation or
  PDF destination is resolvable.
- Preserve printed contents and index text in the reading stream.
- Preserve original printed page labels as publisher-page markers. A plain
  index reference such as “42” remains useful through those markers even when
  it was not an interactive PDF link.
- Do not synthesize links from arbitrary numbers unless the match is
  unambiguous and covered by a test fixture.
- External URI handling follows the existing reader policy and is not expanded
  by this feature.

If no PDF outline exists, a conservative heading-derived TOC is used. If that
also yields no reliable entries, create one root entry using the document
title or filename.

## 10. Image Handling

Meaningful inline raster images are preserved; page backgrounds and decoration
are not.

- DCT/JPEG XObjects are copied byte-for-byte to the cache and use the existing
  JPEG-to-`.pxc` path when first displayed.
- Flate raster XObjects are decoded incrementally, scaled to the reader's useful
  maximum dimensions, and written directly as CrossInk's existing 2-bit `.pxc`
  row format. No PNG encoder and no full-size color framebuffer are introduced.
- Initial Flate raster support is deliberately bounded to:
  - `DeviceGray` at 1, 2, 4, or 8 bits per component;
  - `DeviceRGB` at 8 bits per component;
  - `Indexed` over `DeviceGray` or `DeviceRGB`, with at most 256 palette
    entries and 1, 2, 4, or 8-bit indices;
  - TIFF predictor 2 and PNG predictors 10 through 15;
  - normal or fully inverted decode arrays;
  - 1-bit image masks and same-dimension 8-bit `DeviceGray` soft masks.
- A Flate image is omitted if its source exceeds 16 megapixels, a decoded row
  exceeds 8 KiB, its dimensions overflow, or its color space, predictor, decode
  array, or mask falls outside that matrix. Readable surrounding text remains.
- Inline PDF images use the same bounded path.
- Supported masks and alpha are flattened to the device background while rows
  are produced.
- Duplicate image streams are identified with a streaming hash plus dimensions
  and decoded parameters.
- Tiny repeated icons, near-full-page backgrounds, low-information
  watermarks, and repeated header/footer images are suppressed conservatively.
- Vector art is not rasterized. Nearby captions and accessible replacement
  text remain in the flow.
- Image scaling preserves aspect ratio and follows existing EPUB reader image
  settings.

Image output is row-streamed. At no point does extraction allocate a full
source image plus the 48 KB display framebuffer.

## 11. Cache and Resume Model

### 11.1 Cache Identity

The directory remains path-hash-based to match CrossInk conventions:

```text
/.crosspoint/pdf_<path-hash>/
```

The manifest additionally records:

- PDF cache format version;
- original size and modification time;
- bounded fingerprints of the beginning and end of the source;
- extraction capability/version flags;
- completion state;
- total extracted words;
- warning flags.

Size and modification time make the common check cheap. Head/tail fingerprints
prevent stale reuse when timestamps are unreliable. A mismatch rebuilds the
cache.

### 11.2 Proposed Contents

```text
pdf_<hash>/
  manifest.a
  manifest.b
  build.a
  build.b
  gen_<sequence>/
    metadata.bin
    outline.bin
    anchors.bin
    words.bin
    sections/
      000000.xhtml
      ...
    images/
      <content-hash>.jpg
      <content-hash>.pxc
    layout/
      <existing section-page caches and PDF word sidecars>
```

Large lookup structures such as the xref index are fixed-record disk files
during build, not retained as one heap array.

### 11.3 Power-Loss-Safe Commit and Resumption

An incomplete build writes into a new, inactive `gen_<sequence>` directory. Its
two alternating `build.a`/`build.b` checkpoint slots each contain a monotonically
increasing sequence, verified source identity, generation name, completed
phase, last completed page/object boundary, emitted section/image state,
cumulative word count, and CRC. Startup selects the highest valid checkpoint.
Checkpoints are written only at safe boundaries and are debounced.

The source PDF has exactly one open read handle. Cache writers are short-lived
and explicitly closed at checkpoints. No component opens a second reader for
the source path.

On success:

1. close every source and cache handle;
2. validate every required cache file's recorded size and the checksums
   accumulated while it was streamed, without rereading whole files;
3. write the inactive `manifest.a` or `manifest.b` slot with a higher sequence,
   active-generation name, completed identity, file table, and CRC;
4. sync and close that file before considering the generation committed;
5. on the next cleanup opportunity, remove the older generation only after the
   new manifest slot has been reopened and validated.

The manifest slots are the sole commit marker; directory rename is never
treated as power-loss atomic. If a manifest write tears, startup retains the
older valid slot. If cleanup is interrupted, both generations may remain and
the highest valid manifest still identifies the active one. Fault-injection
tests cover every checkpoint, commit, validation, and old-generation cleanup
boundary.

An interrupted matching build resumes. A stale or invalid generation is
discarded only after validating that it belongs to the expected PDF cache root
and is not referenced by either valid manifest. The original PDF is never
modified.

## 12. Word Progress and Reading Position

PDF percentage is:

```text
words reached / total extracted words
```

The semantic extraction pass counts words once:

- contiguous letter/digit text is one word;
- apostrophes or hyphens internal to such a token do not split it;
- scripts normally written without spaces use a small deterministic Unicode
  range classifier to count readable character units without ICU;
- punctuation-only runs do not count.

The active manifest stores `totalWords`. Each semantic block stores its stable
anchor, cumulative word start, and word count. The initial implementation must
use a PDF-only layout sidecar mapping each generated reader page to its first
and last semantic word ordinal. It must not add fields to the shared serialized
`Page` format or invalidate existing EPUB page caches.

Consequences:

- Page turns read an already-serialized ordinal; they never rescan text.
- Changing font, size, margins, spacing, orientation, or hyphenation may rebuild
  layout pages, but it does not recount the document.
- Resume stores the original PDF path, semantic block anchor, in-block word
  offset, and global word ordinal.
- After relayout, CrossInk resolves the block and word offset, then selects the
  page containing that ordinal.
- The displayed percentage uses the last reached word on the current page and
  is clamped to 100% only at the document end.
- Persistent progress writes follow the existing debounce policy rather than
  writing on every page turn (`AGENTS.md:54`).

Bookmarks and clippings use the original `.pdf` path and semantic anchors, not
the internal cache filenames. Extension allowlists must be updated
deliberately; the cache must never make a PDF appear as a separate EPUB book.

## 13. CPU, Memory, Storage, and Energy Behavior

### 13.1 Execution Model

Extraction is a foreground state machine driven by the activity loop. It does
not create a persistent background worker.

- Work runs only while the preparation activity is active.
- A slice ends after 8 ms, 32 parser operations, or one 4 KiB
  storage/decompression chunk, whichever comes first.
- Long stream inflation checks the budget between input/output chunks.
- Between slices the app yields, checks cancellation and watchdog health, and
  updates its checkpoint only at safe phase boundaries.
- Normal CPU speed is used while useful work is queued so extraction finishes
  and returns to idle promptly.
- Once no immediate work remains, normal CrossInk delays and the existing
  `HalPowerManager` regain control.

This is race-to-idle, not continuous maximum-frequency polling.

### 13.2 Allocation Rules

- Reuse one input buffer, one output buffer, one lexer token buffer, and fixed
  page-reduction storage.
- Reuse the existing approximately 32 KB inflater dictionary where applicable.
- Put constant classification and encoding tables in flash.
- Do not allocate source-sized, page-sized, font-sized, object-count-sized, or
  image-sized heap blocks.
- Gate every meaningful allocation on both total free heap and maximum
  contiguous allocation.
- Use `makeUniqueNoThrow` for ownership and log before recoverable failure.
- Keep local stack objects under the repository's 256-byte review threshold
  unless a measured exception is documented.
- Record peak heap loss, lowest free heap, lowest maximum allocation, and task
  stack high-water mark in debug/QEMU acceptance logs.

Before parser expansion begins, the QEMU tracer records the no-PDF baseline.
The initial hard resource envelope is:

- release `.text + .rodata` growth of at most 256 KiB;
- static DRAM/BSS growth of at most 12 KiB;
- at most 80 KiB of additional live heap owned by PDF preparation, including
  the reusable inflater workspace;
- at least 64 KiB total free heap and a 48 KiB maximum contiguous allocation
  remaining after the display framebuffer exists;
- no individual new allocation above 32 KiB;
- at least 1 KiB stack high-water margin throughout extraction;
- no new persistent task or idle polling loop.

These are named, instrumented limits with forced-failure positive controls.
If the target baseline cannot retain them, support is narrowed and this design
is reviewed again; the limits are not raised merely to pass a fixture.

### 13.3 Storage and Display Work

- All cache writes are sequential where possible and batched to SD-friendly
  sizes.
- Xref/object lookup records use fixed-size disk entries.
- Image and semantic content are hashed while already streaming; no second
  full-file pass is added.
- Total cache output is byte-counted. The per-document ceiling is
  `min(max(4 MiB, 2 * sourceSize + 1 MiB), 64 MiB)`, calculated with checked
  arithmetic. Required text/metadata have priority; optional images are omitted
  before this ceiling is crossed.
- Where the storage HAL reports capacity, preparation also preserves at least
  the larger of 16 MiB or 5% of total card capacity as free space. Every short
  write or out-of-space result closes files, preserves only a valid resumable
  checkpoint when possible, and removes unreferenced partial-generation files.
- A routine checkpoint requires at least eight newly completed PDF pages or
  512 KiB of new output and at least five seconds since the prior checkpoint.
  Cancellation and terminal success/failure force one final safe checkpoint.
- Completed cache files are reused across boots.
- The preparation UI is static. A progress paint requires both at least
  10 percentage points of change and at least 15 seconds since the prior paint.
  It has no animation and is capped at ten intermediate paints for a full
  build.
- No PDF parser, CMap decoder, object lookup, or reading-order reducer runs
  during ordinary page turns.

Actual battery-current improvement is not claimed from QEMU. Simulator
acceptance can prove bounded CPU work, idle return, storage calls, and absence
of polling; physical current remains an optional later measurement.

## 14. Failure Containment and Security

The parser treats every byte as untrusted.

- Checked addition/multiplication for offsets, lengths, dimensions, and counts.
- File seeks remain inside source size.
- Bounded dictionary/array nesting, object recursion, form recursion, CMap
  ranges, xref sections, trailer chains, page count, per-page operators, and
  decompressed bytes.
- Initial document-wide caps are 100,000 indirect objects, 5,000 pages,
  250,000 content operators per page, 10,000,000 content operators total,
  16 nested form XObjects, 64 MiB of expanded required non-image streams, and a
  200:1 per-stream expansion ratio. Smaller structure-specific caps remain
  permitted.
- Cycle detection for object graphs and trailer/page-tree links.
- Per-stream expansion ratio and absolute output caps.
- Cache filenames are generated locally; embedded PDF names never become paths.
- No external file access, network fetch, action execution, or attachment
  extraction.
- Every failure closes files and releases buffers in reverse ownership order.
- OOM is recoverable and must not call `abort`, panic, or restart.
- A failed optional image does not invalidate already-safe text.
- A failed required structure never commits a partial generation.

## 15. ESP32-C3 QEMU Safety Gate

Espressif maintains an ESP32-C3 QEMU fork that emulates its RISC-V CPU, memory,
flash, and selected peripherals and supports booting a complete flash image
without flashing hardware:

https://docs.espressif.com/projects/esp-idf/en/stable/esp32c3/api-guides/tools/qemu.html

### 15.1 Target Environment

Add a PlatformIO `qemu-esp32c3` environment that:

- uses the same ESP32-C3 target platform, Arduino/ESP-IDF framework, compiler
  flags, exception policy, and release memory model as firmware;
- changes only board-peripheral HAL bindings and enables deterministic
  acceptance instrumentation;
- produces bootloader, partition table, application, and fixture/data
  partitions;
- merges them into a correctly sized QEMU flash image;
- runs Espressif's `qemu-system-riscv32` ESP32-C3 machine from a repository
  script with a pinned, documented QEMU version;
- defines a custom PlatformIO upload target that always exits nonzero with a
  “QEMU target cannot be flashed” message.

The QEMU runner accepts only build artifacts, copies the merged flash image to
a temporary working path, never enumerates serial ports, and rejects any
`write_flash` operation. Offline `esptool.py merge_bin` use is allowed solely
to assemble the flash image; it receives no port and performs no device I/O.

The first implementation tracer is a bootable QEMU build that reaches the
CrossInk event loop, mounts emulated test storage, draws a known frame, accepts
scripted input, reports heap/stack values, and emits a terminal serial marker.
The host runner then terminates QEMU and supplies the process exit status. PDF
implementation does not proceed past its minimal parser seam until this tracer
is green.

If the current pioarduino/ESP-IDF output does not boot with the chosen QEMU
release, the build/QEMU pairing must be made compatible. Replacing this gate
with a host-only parser test is not acceptable.

### 15.2 QEMU HAL Boundary

QEMU does not emulate the Xteink e-ink controller or the physical SD wiring.
The QEMU environment therefore substitutes only:

- storage media with a persistent emulated-flash filesystem or generated data
  partition containing PDF fixtures;
- e-ink transfer with a framebuffer sink that emits deterministic hashes and,
  where practical, mirrors to Espressif's virtual framebuffer;
- buttons with scripted logical `MappedInputManager` events;
- sleep/power peripherals with observable target-safe shims.

The production PDF parser, cache writer, reflow interfaces, section paginator,
reader activity, allocators, FreeRTOS scheduling, and target RISC-V code remain
the same.

### 15.3 Required QEMU Replays

The automated target run must:

1. boot the complete target image;
2. open a generated born-digital PDF;
3. build and validate its cache;
4. render deterministic first/middle/last reader pages;
5. open chapters, contents, and index links;
6. save progress, reboot, reopen from cache, and resume;
7. change device font settings and relayout without recounting words;
8. process an OCR-layer fixture;
9. process columns, a table, JPEG, and Flate raster fixtures;
10. reject scan-only, encrypted, malformed, expansion-bomb, and forced-OOM
    fixtures without panic;
11. perform at least 100 scripted page turns while checking heap and stack;
12. prove cache reopen performs exactly one bounded identity open/read, while
    subsequent page turns make zero source-PDF opens and zero PDF-parser calls;
13. return to the normal idle/power-saving path after extraction;
14. replay one uncached and one cached reference EPUB through the refactored
    shared reader path;
15. emit `QEMU_TEST_PASS` only after every oracle succeeds.

The host treats a missing terminal marker, `QEMU_TEST_FAIL`, panic, watchdog
reset, timeout, or unexpected QEMU exit as failure. It terminates QEMU itself
after the terminal marker. No serial port discovery or physical flashing
command is part of this gate.

## 16. Native Simulator Gate

The native CrossInk simulator remains the UI oracle:

- browser shows PDFs and routes them correctly;
- preparation, cancellation, error, and warning screens are readable;
- device typography, margins, orientations, chapters, links, publisher-page
  markers, progress, bookmarks, and resume behave like the EPUB reader;
- no clipped controls or accidental fixed-page canvas appears;
- deterministic image-cache fixtures verify placement even where native
  JPEG/PNG decoder stubs are intentionally limited.

Because the current documented simulator setup is macOS-specific, the
implementation plan must include a reproducible supported host route for the
current Windows workspace or a hermetic CI/container route. Simulator success
cannot be claimed from compile-only evidence.

## 17. Test Corpus and Acceptance Oracles

All committed PDFs are tiny, generated, license-safe fixtures. Each fixture has
an expected semantic transcript, outline/link map, word count, and selected
framebuffer hashes.

| Case | Required oracle |
|---|---|
| Same text at 6 pt and 72 pt | Identical semantic stream, total words, reader page count, and framebuffer hashes at identical device settings |
| Device font-size change | Different page layout/hash, proving the invariant test can detect typography changes |
| Two columns | Left column precedes right column without line interleaving |
| Table | Rows remain ordered and cells remain distinguishable |
| Outline hierarchy | Chapter selector preserves titles, levels, and destinations |
| Contents/index links | Links reach semantic anchors; printed page labels remain discoverable |
| Named destinations | Stable after cache reopen and relayout |
| JPEG image | Image retained near related text and cached once |
| Flate raster | Incremental `.pxc` output, bounded heap, correct dimensions/hash |
| OCR hidden text | Qualified render-mode-3 text opens once; visible duplicates and off-page hidden metadata do not |
| Image-only scan | Clear no-readable-text message |
| Encrypted PDF | Clear unsupported message; no partial generation is committed |
| Malformed offsets/lengths | Checked failure, no out-of-file seek or panic |
| Expansion bomb | Decoder cap trips before unsafe allocation/write |
| Forced allocation failure | Recoverable error and complete cleanup |
| Cache/storage ceiling | Optional images stop first; ENOSPC leaves no committed partial generation |
| Cancel/restart | Resume from last verified checkpoint |
| Word progress | Exact reached/total ratio at known anchors |
| Font/orientation relayout | Same semantic position and total words after relayout |
| 100 cached page turns | One bounded identity open before reading, then no PDF opens/parser calls, heap loss, or stack erosion |
| Single-source-reader fake | Test fails on a second concurrent source reader |
| QEMU upload target | Deliberately fails before serial discovery or device I/O |
| Existing EPUB | Cached and uncached page text, hashes, TOC, links, progress, and bookmarks match the baseline |

Tests are layered:

1. host unit tests for lexer, CMap, text operators, bounds, reading order, word
   counting, identity, and recovery state;
2. host integration tests for complete fixture-to-cache extraction;
3. native simulator reader replays and framebuffer comparisons;
4. QEMU ESP32-C3 target boot/replays with heap, stack, parser-call, file-open,
   and framebuffer-hash evidence;
5. cached and uncached EPUB before/after golden replays, including heap and
   firmware-size deltas for the shared-reader refactor;
6. PlatformIO simulator/qemu-esp32c3/default/tiny/xlarge builds and static
   analysis.

The original user-visible repro is always the final oracle: a PDF must open as
device-sized reflowed text rather than a miniature fixed page.

## 18. Documentation and Compatibility

- Add `.pdf` to user-facing supported-format documentation with the exact
  capability boundary.
- Document the PDF cache layout/version in `docs/file-formats.md`.
- Document simulator/QEMU setup and the no-hardware-flash acceptance workflow.
- Add all product strings to translation YAML and regenerate i18n outputs.
- Add a human-readable PDF feature entry to `CHANGELOG.md`.
- Bump every changed binary cache format version before writing the new layout.
- Existing EPUB/XTC/TXT behavior and caches must remain readable.
- Incomplete PDF cache directories are never shown as books.

## 19. Completion Criteria

The feature is complete only when:

- a supported PDF can be copied directly to SD and first-open extraction occurs
  entirely on the ESP32-C3 code path;
- its reading view uses device typography and has no original-page miniature,
  zoom, or pan behavior;
- chapters, contents/index material, internal destinations, images, page
  labels, progress, resume, bookmarks, and supported clippings pass their
  fixtures;
- total words are counted once and page turns do not parse or scan source text;
- cache build is cancel-safe, resumable, atomic, and allocation-failure-safe;
- cached reading is stable across reboot and relayout;
- unsupported scans/encryption/malformed inputs fail clearly;
- the native reader replay passes;
- the complete QEMU ESP32-C3 target replay passes without panic, watchdog reset,
  source-PDF access beyond bounded cache-identity validation, or monotonic
  resource loss;
- firmware builds and static checks pass;
- no physical device was flashed during acceptance.
