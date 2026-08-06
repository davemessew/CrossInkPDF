---
title: Data Cache
nav_order: 16
---

# Data Cache

CrossInk caches data aggressively on the SD card to minimize RAM use. The ESP32-C3 has about 380 KB of usable RAM, so rebuilding every book structure in memory on every open would be too expensive.

The main data directory is `.crosspoint` on the SD card. It stores render caches and persistent user/device data.

## Directory Layout

```text
.crosspoint/
├── global_stats.bin        # All-time reading stats, including total books read
├── global_stats.bin.bak    # Backup used if the main global stats file is corrupt
├── synced_stats/           # Stats snapshots received from other readers
├── crossink-settings.json  # CrossInk device settings
├── settings.json           # Legacy settings fallback, if present
├── settings.bin.bak        # Legacy binary settings file after migration, if present
├── state.json              # Last-opened book and sleep/session state
├── state.bin.bak           # Legacy binary state file after migration, if present
├── recent.json             # Recent books list
├── recent.bin.bak          # Legacy binary recent-books file after migration, if present
├── wifi.json               # Saved Wi-Fi networks
├── opds.json               # Saved OPDS servers
├── koreader.json           # KOReader sync credentials
├── bookmarks/              # EPUB and PDF bookmark compatibility files
├── clippings/              # EPUB and PDF clipping/highlight compatibility files
├── book_move.a/.b          # Temporary two-slot book-move journal
├── pdf_delete.a/.b         # Temporary two-slot PDF-delete journal
├── pdf-directory-delete.spool      # Temporary validated PDF directory-delete plan
├── pdf-directory-delete.spool.tmp  # Incomplete plan before atomic publication
├── home_carousel_cache.bin # Lyra Carousel home-screen snapshot cache
├── sleep_frame.bin         # Temporary sleep overlay framebuffer, when used
├── epub_12471232/          # Each EPUB is cached to epub_<hash>
│   ├── progress.bin        # Reading position (chapter, page, etc.)
│   ├── stats.bin           # Legacy per-book reading stats
│   ├── stats_v5.bin        # Version 5 per-book reading stats
│   ├── reader_settings.bin # Per-book reader settings, render mode, and auto-page-turn interval
│   ├── cover.bmp           # Book cover image, once generated
│   ├── thumb_*.bmp         # Home/recent-books thumbnail images
│   ├── book.bin            # Book metadata, spine, table of contents, etc.
│   ├── css_rules.cache     # Parsed CSS rules
│   └── sections/           # Pre-rendered chapter/page layout data
│       ├── 0.bin
│       ├── 1.bin
│       └── ...
├── pdf_12471232/           # Each PDF is cached to pdf_<hash>
│   ├── manifest.a/.b       # Alternating committed-generation manifests
│   ├── build.a/.b          # Alternating preparation checkpoints
│   ├── progress.a/.b       # Word-based reading position
│   ├── saved_items.a/.b    # Canonical PDF bookmarks and clippings
│   └── gen_1/              # One prepared, inactive or committed generation
│       ├── metadata.bin    # Semantic section and word ledger
│       ├── outline.bin     # Outline, destinations, and page labels
│       ├── sections/       # Reflowable XHTML plus derived page caches
│       ├── images/         # Validated JPEG or grayscale PXC resources
│       ├── cover.bmp
│       └── thumb.bmp
├── xtc_12471232/           # XTC progress and generated cover/thumb images
└── txt_12471232/           # TXT progress, page index, and generated cover image
```

## Clearing Cache Data

Deleting the entire `.crosspoint` directory resets caches, settings, saved network/server data, bookmarks, recent books, reading progress, and reading stats.

To clear supported book render caches from the device UI without deleting
settings or global stats, use:

**Settings > System > Files & Cache > Clear Reading Cache**

For PDFs, this action removes prepared generations, manifests, checkpoints,
and temporary build files while preserving the book's progress, saved items,
reading stats, and reader settings. The next open prepares the PDF again. Use
this action instead of deleting the whole `.crosspoint` directory when only a
PDF cache is stale or damaged.

## Book Moves And Cache Identity

Cache folders are path-based. Moving a book file can create a new cache
directory, so the moved copy may start with fresh reading progress unless the
firmware migrates the cache for that move. CrossInk's built-in move-to-Read and
file-browser move flows migrate supported cache and user-state data. For PDFs,
that includes prepared content, semantic progress, bookmarks, and clippings;
the move is journaled so an interrupted operation can resume safely.

EPUB reader font, page layout, styling, and reading-aid settings normally come from the global Reader settings. If those settings are changed from inside an EPUB, CrossInk stores a per-book override in that book's `reader_settings.bin`; books without that override continue to follow the global defaults. EPUB render mode is also stored per book so a problematic title can be switched to Balanced or Light rendering from the File Browser or Recent Books long-press menus before opening it.

EPUB and PDF clipping compatibility files live in `/.crosspoint/clippings/`.
Each book gets a binary file named from the book type and the CRC32 of the book
path. A PDF's canonical semantic bookmark and clipping records also live in its
`saved_items.a` and `saved_items.b` slots so they survive repagination. The
shared compatibility files keep the existing bookmark and clipping lists and
exports working across formats. CrossInk also appends a Kindle-style text
export to `/My Clippings.txt` on the SD-card root. That export is human-readable
and append-only, so deleting a clipping in the UI does not rewrite older text
already exported there.

Cache data is cleared by supported CrossInk delete/move flows. PDF deletes use
a two-slot recovery journal, while bulk directory deletes first publish and
validate a bounded deletion spool. If you remove or rename books outside
CrossInk by editing the SD card directly, old cache folders and path-derived
bookmark or clipping files may remain until you clear reading cache or restore
the original path.

All-time reading stats can also be backed up outside `.crosspoint` in:

```text
/.crossink-stats-backup/
```

For user-facing PDF behavior, see [PDF Support](./pdf-support.md). For binary
file layout details, see [File Formats](./file-formats.md).
