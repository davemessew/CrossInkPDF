# File Formats

These formats describe CrossInk's SD-card cache files, including the EPUB cache
under `/.crosspoint/epub_<hash>/` and the PDF reflow cache under
`/.crosspoint/pdf_<hash>/`. All POD fields are little-endian; strings are
length-prefixed UTF-8 unless a format notes a fixed-size char buffer.

## `book.bin`

### Version 10

`book.bin` stores EPUB metadata plus lookup tables for spine and TOC entries.
The current firmware writes this version from `BookMetadataCache`.
Version 10 stores book and TOC title strings NFC-composed so decomposed
diacritics render correctly with device fonts. It also rebuilds metadata after
the EPUB guide start-reference handling changed.

ImHex pattern:

```c++
import std.mem;
import std.string;
import std.core;

#define EXPECTED_VERSION 10
#define MAX_STRING_LENGTH 65535

struct String {
    u32 length [[hidden, comment("String byte length")]];
    if (length > MAX_STRING_LENGTH) {
        std::warning(std::format("Unusually large string length: {} bytes", length));
    }
    char data[length] [[comment("UTF-8 string data")]];
} [[sealed, format("format_string"), comment("Length-prefixed UTF-8 string")]];

fn format_string(String s) {
    return s.data;
};

struct Metadata {
    String title [[comment("Book title")]];
    String author [[comment("Book author")]];
    String language [[comment("Book language code")]];
    String coverItemHref [[comment("Path to cover image")]];
    String textReferenceHref [[comment("Path to guided first text reference")]];
};

struct SpineEntry {
    String href [[comment("Resource path")]];
    u32 cumulativeSize [[comment("Cumulative uncompressed spine size through this entry")]];
    s16 tocIndex [[comment("Index into TOC, or inherited/previous TOC index when no direct entry exists")]];
};

struct TocEntry {
    String title [[comment("Chapter/section title")]];
    String href [[comment("Resource path")]];
    String anchor [[comment("Fragment identifier")]];
    u8 level [[comment("Nesting level")]];
    s16 spineIndex [[comment("Index into spine (-1 if none)")]];
};

struct BookBin {
    u8 version;
    if (version != EXPECTED_VERSION) {
        std::error(std::format("Unsupported version: {} (expected {})", version, EXPECTED_VERSION));
    }

    u32 lutOffset [[comment("Offset to lookup tables")]];
    u16 spineCount;
    u16 tocCount;

    Metadata metadata;

    u32 currentOffset = $;
    if (currentOffset != lutOffset) {
        std::warning(std::format("LUT offset mismatch: expected 0x{:X}, got 0x{:X}", lutOffset, currentOffset));
    }

    u32 spineLut[spineCount] [[comment("Spine entry offsets")]];
    u32 tocLut[tocCount] [[comment("TOC entry offsets")]];

    SpineEntry spines[spineCount];
    TocEntry toc[tocCount];
};

BookBin book @ 0x00;

u32 fileSize = std::mem::size();
u32 parsedSize = $;
if (parsedSize != fileSize) {
    std::warning(std::format("Unparsed data detected: {} bytes remaining at offset 0x{:X}", fileSize - parsedSize, parsedSize));
}
```

## `reader_settings.bin`

### Version 4

Each EPUB cache directory may contain `reader_settings.bin`. Missing files mean
the book uses global Reader settings and the default auto-page-turn interval.

Version 1 stored only:

- `u8 version`
- `u16 autoPageTurnSeconds`

Version 2 stores flags before the full reader-settings snapshot. Version 3 adds
the EPUB word-spacing level to that snapshot. Version 4 adds the EPUB indexing
method (`0` = incremental, `1` = full section). This lets the
file preserve an auto-page-turn interval without forcing custom font/layout
settings for the book. It also stores a per-book EPUB render mode override,
which can be changed from book action menus before opening the book so a
problematic EPUB can be moved to Balanced or Light rendering without entering
the reader first. Safe Mode also uses this file to save Light rendering with
embedded styles, Bionic Reading, and Guide Dots disabled after that final
fallback successfully opens a difficult book.

```c++
struct ReaderSettingsBin {
    u8 version; // 4
    u8 flags;   // bit 0 = custom reader settings, bit 1 = custom auto-page-turn interval, bit 2 = render mode override
    u16 autoPageTurnSeconds;
    u8 renderMode; // 0 = CrossInk Default, 1 = Balanced, 2 = Light

    u8 fontFamily;
    u8 fontSize;
    u8 lineHeightPercent;
    u8 wordSpacing; // 0 = natural font spacing; 1-4 widen each gap by ~75% per level
    u8 orientation;
    u8 screenMargin;
    u8 publisherPageNumbers;
    u8 paragraphAlignment;
    u8 embeddedStyle;
    u8 hyphenationEnabled;
    u8 textAntiAliasing;
    u8 readerDarkMode;
    u8 imageRendering;
    u8 extraParagraphSpacing;
    u8 forceParagraphIndents;
    u8 bionicReadingEnabled;
    u8 guideReadingEnabled;
    u8 snapshotRenderMode;
    u8 indexingMethod; // 0 = incremental, 1 = full section
    char sdFontFamilyName[64];
};
```

## PDF reflow cache

PDF preparation is performed on the device. It creates a derived, EPUB-like
reading product under `/.crosspoint/pdf_<path-hash>/`; it never rewrites the
source PDF. The path hash is formatted as an unsigned decimal integer.

The cache root may contain:

- `manifest.a` and `manifest.b`: alternating committed-generation manifests.
- `build.a` and `build.b`: alternating preparation checkpoints.
- `progress.a` and `progress.b`: alternating reading-progress records.
- `saved_items.a` and `saved_items.b`: alternating bookmark and clipping
  records.
- `gen_<positive-decimal>/`: one prepared generation. A completed generation
  contains `sections/%06u.xhtml`, `metadata.bin`, `outline.bin`, `images/`,
  `cover.bmp`, and `thumb.bmp`.

An in-progress generation may also contain `resume.journal`, `resume.sections`,
and the `build.*` work files described below. They are resume inputs, not
committed-generation artifacts, and cleanup removes them after preparation is
committed or abandoned.

Preparation writes an inactive generation first. Only the newest CRC-valid,
completed manifest activates it. Each required-file record binds a relative
path to its exact size and CRC-32, and the manifest also binds the complete
record ledger. Cached reopen validates the source identity and required
artifacts before serving reflowed sections; it does not render or retain a PDF
page canvas.

The source identity consists of file size, optional modification time, and
fingerprints of the head and tail of the source file. A mismatched source
identity rejects the derived cache without changing the PDF.

### Two-slot sequence selection

All PDF cache-root A/B stores compare 32-bit sequences with wrap awareness. A
candidate is newer than a reference only when their unsigned difference is
nonzero and less than `0x80000000`. This preserves ordering across `UINT32_MAX`
wrap; an equal value or an exact half-range difference is not newer.

- PRMF manifests load the newest valid slot whose source identity matches and
  write the opposite slot; the first slot is A.
- PRCP checkpoints load the newest valid source-matching slot. Odd sequences
  are written to A and even sequences to B.
- PRPG progress records and PSIT saved-item stores load the newest valid slot
  and write the opposite one; the first slot is A. If the newest PSIT header is
  valid but its body fails validation, loading can fall back to the other valid
  slot.

An interrupted or uncertain write does not make an invalid slot authoritative.
Each reader validates magic, version, size, reserved fields, source binding,
and CRC before applying sequence ordering.

### `manifest.a` / `manifest.b`: PRMF codec 1, format 1, capability 2

The manifest codec uses:

- codec version `1`
- cache format version `1`
- cache capability version `2`
- at most `4,096` required files
- at most `512 KiB` per manifest slot
- required relative paths of 1 through 95 bytes

The fixed 84-byte header is:

- `[0-3]` magic `PRMF`
- `[4-5]` codec version (`1`)
- `[6-7]` cache format version (`1`)
- `[8-9]` cache capability version (`2`)
- `[10-11]` reserved zero
- `[12-15]` monotonic slot sequence (`uint32_t`)
- `[16]` completed flag (`0` or `1`)
- `[17]` source modification time known (`0` or `1`)
- `[18-19]` reserved zero
- `[20-23]` warning flags
- `[24-31]` source file size (`uint64_t`)
- `[32-39]` source modification time (`uint64_t`)
- `[40-47]` source head fingerprint (`uint64_t`)
- `[48-55]` source tail fingerprint (`uint64_t`)
- `[56-59]` generation number (`uint32_t`, nonzero when completed)
- `[60-63]` total semantic word count (`uint32_t`)
- `[64-67]` required-file record count (`uint32_t`)
- `[68-75]` aggregate required-file bytes (`uint64_t`)
- `[76-83]` FNV-1a required-file ledger (`uint64_t`)

Each variable-length required-file record is:

- `[0]` relative-path length (`uint8_t`)
- `[1-3]` reserved zero
- `[4-11]` exact file size (`uint64_t`)
- `[12-15]` file CRC-32
- `[16...]` raw relative-path bytes without a terminator

The final eight bytes are the total encoded length (`uint32_t`) followed by a
CRC-32 over every preceding byte, including the stored length. The minimum
manifest size is therefore 92 bytes. A manifest with `completed=1` is the only
generation commit marker; checkpoints and preparation spools cannot activate
partial output.

### `build.a` / `build.b`: PRCP codec 3

Preparation checkpoints use two alternating fixed 96-byte control records. The
newest CRC-valid sequence whose source identity matches the open PDF is the
only resume candidate:

- `[0-3]` magic `PRCP`
- `[4-5]` codec version (`3`)
- `[6-7]` reserved zero
- `[8-11]` monotonic slot sequence
- `[12-19]` source file size
- `[20]` source modification time known
- `[21-23]` committed resume-data byte count (unsigned 24-bit little-endian)
- `[24-31]` source modification time
- `[32-39]` source head fingerprint
- `[40-47]` source tail fingerprint
- `[48-51]` generation number
- `[52]` phase (`0=None`, `1=Discover`, `2=ParsePages`,
  `3=EmitSections`, `4=EmitImages`, `5=Finalize`, `6=Complete`,
  `7=Failed`, `8=Cancelled`)
- `[53]` resume phase (`0=None`, `1=CommitManifest`,
  `2=AfterEmitSections`, `3=AfterPage`, `4=AfterImage`,
  `5=AfterImageRepair`)
- `[54-55]` reserved zero
- `[56-59]` last verified source page
- `[60-63]` last verified source object
- `[64-67]` emitted section count
- `[68-71]` emitted image count
- `[72-75]` cumulative semantic word count
- `[76-83]` output bytes
- `[84-87]` warning flags
- `[88-91]` stored record length (`96`)
- `[92-95]` CRC-32 over bytes `[0-91]`

Routine checkpoint writes are debounced until at least five seconds have
elapsed and either eight more pages or 512 KiB more output has completed.
Forced terminal and cancellation checkpoints may bypass that gate. A
checkpoint records resumable work only; it never commits its generation.

For `AfterPage`, the three-byte resume-data count is the committed prefix of
`gen_<generation>/resume.journal`. For `AfterImage` and `AfterImageRepair`, it
records the encoded resume-ledger length. It is zero for checkpoints without
one of those durable data sets.

### `resume.sections`: PRES version 1

An in-progress generation stores a fixed 256-byte section/image resume control
at `gen_<generation>/resume.sections`. It is accepted only when its sequence,
generation, resume phase, section count, completed source-page count, and
retained-image count match the selected PRCP checkpoint.

- `[0-3]` magic `PRES`
- `[4-5]` version (`1`)
- `[6-7]` record size (`256`)
- `[8-11]` matching checkpoint sequence
- `[12-15]` generation
- `[16]` resume phase (`2=AfterEmitSections`, `4=AfterImage`,
  `5=AfterImageRepair`)
- `[17]` flags (`bit0=cover selected`, `bit1=cover record available`,
  `bit2=source cover is JPEG`)
- `[18]` deferred-image count (maximum `64`)
- `[19]` retained required-image-file count
- `[20-21]` emitted section count
- `[22-23]` completed source-page count
- `[24-31]` selected cover content hash
- `[32-35]` selected cover source CRC-32
- `[36]` cover relative-path length
- `[37-39]` reserved zero
- `[40-47]` cover file size
- `[48-51]` cover file CRC-32
- `[52-147]` fixed cover relative-path area; unused bytes are zero
- `[148-211]` 64 canonical deferred-image record indexes; entries after the
  deferred-image count must be `0xFF`
- `[212]` image index being resumed for `AfterImage`, otherwise zero
- `[213-251]` reserved zero
- `[252-255]` CRC-32 over bytes `[0-251]`

The file is published through `resume.sections.tmp` and cannot commit a
generation. Invalid control data rejects that resume boundary and preparation
falls back safely.

### `resume.journal`: discovery snapshot and page resume records

The single page-resume journal starts with one discovery snapshot:

- `PDRH` version 1, 192 bytes
- sorted 24-byte xref records, each with its own CRC-32
- 244-byte explicit page records, each with its own CRC-32
- `PDRT` version 1, 72 bytes

Each explicit page record is bound to the PDRH/PDRT version-1 envelope and has
this fixed PDRP layout:

- `[0-3]` magic `PDRP`
- `[4-5]` record size (`244`)
- `[6-7]` zero-based page ordinal
- `[8-13]` page object reference
- `[14-19]` resource-owner object reference
- `[20-25]` resource-dictionary object reference
- `[26-121]` 16 content-stream object references
- `[122-217]` 16 annotation object references
- `[218-221]` signed view-box minimum X coordinate bits
- `[222-225]` signed view-box minimum Y coordinate bits
- `[226-229]` zero-based page index
- `[230-231]` page width
- `[232-233]` page height
- `[234-235]` rotation (`0`, `90`, `180`, or `270`)
- `[236]` content-stream count
- `[237]` annotation count
- `[238]` resource flags (`bit0=resources present`,
  `bit1=resource dictionary is indirect`)
- `[239]` annotation-overflow marker (`0` means the fixed record is complete;
  `1` makes discovery resume restart because excess references are session-only)
- `[240-243]` record CRC-32 over bytes `[0-239]`

Every object reference occupies six bytes: a little-endian `uint32_t` object
number followed by a little-endian `uint16_t` generation.

The discovery snapshot length is exactly
`192 + xrefCount * 24 + pageCount * 244 + 72`. The header binds the source
identity, generation, xref and page counts, record sizes, catalog references,
and language. The trailer repeats the source size, head and tail fingerprints,
modification-time-known flag, generation, counts, and record sizes. It also
stores an aggregate CRC-32 and FNV-1a ledger over all xref and explicit-page
records. Xrefs must decode in strictly increasing object-number order.

After the discovery trailer, committed prepared pages append `PRJR` version 2, 512 bytes each.
A record binds its sequence, source identity, generation, completed page and
section counts, semantic word and anchor cursors, output byte totals, page
geometry, section path, section size and CRC-32, and ends with its own CRC-32.

Recovery reads only the checkpoint's `journalBytes` prefix; bytes appended
after that committed boundary are ignored. It validates both discovery passes,
every page record, the aggregate CRC-32 and FNV-1a ledger, source identity,
generation, monotonically increasing sequences, section files, and final
checkpoint totals before resuming. A missing, truncated, mismatched, or corrupt
journal closes the resume files and clears the in-memory resume state. The
reader falls back to a clean on-device preparation in a new generation. After
a replacement generation is committed, later cleanup removes the rejected
generation while preserving generations referenced by valid manifests.

### `metadata.bin`: XPMD codec 1

`metadata.bin` binds book metadata to the semantic section and word ledger. It
uses a 24-byte header, raw UTF-8 metadata fields, 24-byte section records, and a
final four-byte CRC-32 over all preceding bytes. It permits at most 256
sections and 256 outline entries; both counts must be nonzero. Stored title,
author, and language lengths must be less than 192, 128, and 24 bytes,
respectively.

The header is:

- `[0-3]` magic `XPMD`
- `[4-5]` codec version (`1`)
- `[6-7]` header size (`24`)
- `[8-9]` section count
- `[10-11]` outline count
- `[12-15]` total semantic word count
- `[16-17]` title byte length
- `[18-19]` author byte length
- `[20-21]` language byte length
- `[22-23]` reserved zero

The title, author, and language bytes immediately follow the header. Each
section record is:

- `[0-3]` section XHTML byte size
- `[4-7]` cumulative XHTML bytes through this section
- `[8-11]` first global word ordinal
- `[12-15]` section word count
- `[16-19]` first semantic-anchor ordinal
- `[20-21]` matching outline index (`int16_t`)
- `[22-23]` reserved zero

### `outline.bin`: XPOL codec 1

`outline.bin` stores the chapter tree, internal destinations, publisher page
labels, and synthetic index entries used by the reader. It supports 1 through
256 entries, titles shorter than 96 bytes, and nesting up to 16 levels.

The 16-byte header is:

- `[0-3]` magic `XPOL`
- `[4-5]` codec version (`1`)
- `[6-7]` record size (`128`)
- `[8-9]` record count
- `[10-11]` reserved zero
- `[12-15]` aggregate record bytes (`count * 128`)

Each 128-byte record is:

- `[0-3]` source object number
- `[4-5]` source object generation
- `[6-7]` parent index (`int16_t`, `-1` for a root)
- `[8-9]` semantic section index
- `[10]` nesting level
- `[11]` title byte length
- `[12-15]` semantic-anchor ordinal
- `[16-19]` zero-based source page index
- `[20-23]` reserved zero
- `[24...]` title bytes, with the unused remainder zero-filled

A final four-byte CRC-32 covers the header and every record. The XHTML anchor
is derived from the stored ordinal as `b` followed by eight lowercase
hexadecimal digits; that string is not duplicated in the outline record.

### `progress.a` / `progress.b`: PRPG version 1

PDF progress uses two alternating fixed 96-byte records. The newest valid
sequence whose source identity and total word count exactly match the active
book is selected.

- `[0-3]` magic `PRPG`
- `[4-5]` version (`1`)
- `[6-7]` record size (`96`)
- `[8-11]` monotonic slot sequence
- `[12-15]` flags (`bit0=page count`, `bit1=semantic position`,
  `bit2=word cursor`, `bit31=source modification time known`)
- `[16-23]` source file size
- `[24-31]` source modification time
- `[32-39]` source head fingerprint
- `[40-47]` source tail fingerprint
- `[48-51]` document total word count
- `[52-55]` section index (`int32_t`)
- `[56-59]` page number (`int32_t`)
- `[60-63]` page count (`int32_t`)
- `[64-67]` global word ordinal
- `[68-71]` word offset inside the semantic block
- `[72-81]` fixed semantic-block anchor (`char[10]`)
- `[82]` anchor byte length
- `[83]` reserved zero
- `[84-87]` word cursor
- `[88-91]` reserved zero
- `[92-95]` CRC-32 over bytes `[0-91]`

The word cursor is the count of semantic words reached and is valid from zero
through `totalWords`. Reading progress is `wordCursor / totalWords`. A global
word ordinal identifies one specific word and is zero-based, so it must be
less than `totalWords`. Resume resolves the semantic anchor and ordinal first;
the stored page tuple is only a fallback.

### PDF-only `.pwi` page-word sidecars: PWIH/PWIF version 3

Each PDF section layout may have a sidecar at
`<section-page-cache-path>.pwi`. These fixed-record indexes are PDF-only,
layout-dependent derived data. EPUB cache formats are unchanged. A sidecar can
be discarded and rebuilt after font, margin, orientation, or pagination
changes.

The 32-byte header is:

- `[0-3]` magic `PWIH`
- `[4-5]` version (`3`)
- `[6-7]` header size (`32`)
- `[8-9]` section index
- `[10-11]` record size (`40`)
- `[12-15]` first global word ordinal in the section
- `[16-19]` section word count
- `[20-23]` exact byte length of the unchanged version 44 section `.bin`
  prefix
- `[24-27]` nonzero pair token shared with the section `.bin` trailer
- `[28-31]` header CRC-32 over bytes `[0-27]`

Each rendered page has one 40-byte record. Bytes `[0-27]` retain the version 1
semantic payload exactly; versions 2 and 3 append the coordinates needed to
replay the unchanged section-cache lookup-table tails:

- `[0-3]` first global word ordinal, or the current cursor for an empty page
- `[4-7]` last global word ordinal, or the same cursor for an empty page
- `[8-11]` first word offset inside the semantic block
- `[12-21]` fixed semantic-block anchor (`char[10]`)
- `[22]` flags (`bit0=valid semantic range`)
- `[23]` anchor byte length
- `[24-27]` reserved zero
- `[28-31]` page data offset in the section `.bin`
- `[32-33]` paragraph index
- `[34-35]` list-item index
- `[36-39]` record CRC-32 over bytes `[0-35]`

For a valid range, the page word cursor decodes as
`lastGlobalWordOrdinal + 1`. A word split across adjacent rendered pages may
therefore repeat one ordinal. An invalid or empty record stores the current
cursor in both ordinal fields and has no anchor.

The 16-byte footer is:

- `[0-3]` magic `PWIF`
- `[4-5]` page count
- `[6-7]` footer size (`16`)
- `[8-11]` aggregate CRC-32 over the header and the first 36 bytes of every
  page record
- `[12-15]` footer CRC-32 over bytes `[0-11]`

For a normal PDF section, the corresponding `.bin` consists of the unchanged
version 44 section-cache prefix followed by one 16-byte binding trailer:

- `[0-3]` magic `PWIB`
- `[4-7]` exact version 44 prefix length
- `[8-11]` the same nonzero pair token stored in the PWIH header
- `[12-15]` CRC-32 over trailer bytes `[0-11]`

The writer derives the pair token incrementally from the PWI semantic data as
it is produced and maps a zero result to a nonzero token. It does not reread
the section `.bin` during the build. Reopen validation reads only the 16-byte
trailer at the end of the `.bin`, regardless of the prefix or file size, then
requires its prefix length and token to match the eagerly validated PWIH/PWIF
sidecar.

The sidecar maps stable semantic positions onto a particular pagination.
Resume and saved items use semantic positions across layout changes; a saved
page fallback is accepted only when its stored layout fingerprint exactly
matches the current layout.

Normal PDF pagination also uses the sidecar as an external fixed-record spool:
after validation, it replays file offsets, paragraph indexes, and list-item
indexes into the existing version 44 section `.bin` in four-record (32-byte)
windows. EPUB, PDF preview, and PDF footnote pagination keep the legacy
in-memory lookup-table path and do not create a `.pwi` or change their cache
bytes. A normal PDF section gains only its 16-byte `PWIB` trailer. PWI versions
1 and 2, a missing sidecar or trailer, a truncated or corrupt trailer, a zero
token, or a stale, count-mismatched, non-monotonic, CRC-invalid, or mismatched
sidecar invalidates both derived files and rebuilds them together.

Pairing and PWI record integrity are checked eagerly. The binding deliberately
does not perform a full eager rehash of the version 44 prefix, so an arbitrary
later bit flip inside that prefix is handled fail-safe when the existing
bounded section and page deserializers consume it. Publication removes the old
PWI first, removes the old `.bin` second, promotes the new `.bin`, and promotes
the new PWI last. Power loss at any remove, rename, or sync boundary therefore
leaves an acceptable old pair, an acceptable new pair, or a rejected cache,
never an accepted mixed pair.

Each external-LUT replay reads one bounded group of one to four 40-byte records
into a 160-byte stack buffer. Including the initial full sidecar validation,
three field replays over `N` pages perform exactly
`2 + 4 * ceil(N / 4)` PWI reads.

### Canonical PDF image resources

Prepared section XHTML refers to canonical resources as
`../images/<leaf-name>`. Two on-device representations are supported:

- `gen_<generation>/images/<16-hex-content-hash>-<8-hex-source-crc>-<16-hex-source-bytes>.jpg`
  stores a validated source JPEG stream without decoding and re-encoding it.
- `gen_<generation>/images/<16-hex-content-hash>-<8-hex-source-crc>.pxc`
  stores a decoded raster in the shared legacy pixel-cache layout.

PXC has no magic or internal version. Its first four bytes are little-endian
`uint16_t width` and `uint16_t height`. They are followed by
`ceil(width / 4) * height` bytes of row-major, two-bit grayscale pixels. Four
pixels occupy each byte, most-significant pair first. The active PRMF
capability version plus the required-file size and CRC protect this legacy
resource format.

Content-addressed identities are reused when the same resource occurs more
than once. Every canonical resource is listed in the completed manifest with
its exact size and CRC-32. Unsafe or unsupported optional artwork may be
omitted without removing the surrounding semantic text.

### Transient PDF preparation spools

The following files are implementation workspaces, not durable cache APIs.
They are never required-file records in a completed PRMF manifest and are
removed during preparation cleanup:

- `build.images`: `PIBS`/`PIBE` version 3, with at most 64 fixed 864-byte
  deferred-image records.
- `build.image-files` and `build.image-files.resume`: `PIFS`/`PIFE` version 3,
  with at most 64 fixed 116-byte required-file records. The `.resume` ledger is
  the separately published required-file snapshot used at image resume
  boundaries.
- `build.mask`: `PMSP`/`PMEN` version 2, with interleaved base/alpha payload
  streams and at most 64 fixed 60-byte records.
- `build.nav` and `build.inline-nav`: bounded raw phase workspaces whose
  contents are checked while preparation is running but have no public
  magic/version contract.
- `build.section-repair`, `images/build-jpeg.tmp`, and
  `images/build-inline-%02u.tmp`: temporary repair or atomic-write files.

The PIBS and PIFS files share this 16-byte header envelope:

- `[0-3]` start magic (`PIBS` or `PIFS`)
- `[4-5]` version (`3`)
- `[6]` maximum record count (`64`)
- `[7]` reserved zero
- `[8-9]` record size (`864` for PIBS, `116` for PIFS)
- `[10-11]` reserved zero
- `[12-15]` header CRC-32 over bytes `[0-11]`

Each 864-byte deferred-image record stores its own CRC-32 in `[860-863]`,
covering bytes `[0-859]`. Each 116-byte required-file record stores its own
CRC-32 in `[112-115]`, covering bytes `[0-111]`. The running aggregate CRC is
calculated over every complete encoded record, including those record-local
CRCs.

Their common 24-byte footer layout is:

- `[0-3]` end magic (`PIBE` or `PIFE`)
- `[4-5]` version (`3`)
- `[6]` encoded record count
- `[7]` reserved zero
- `[8-15]` first-record offset (`16`)
- `[16-19]` aggregate encoded-record CRC-32
- `[20-23]` footer CRC-32 over bytes `[0-19]`

The PMSP header is 12 bytes: magic `PMSP` at `[0-3]`, version `2` at
`[4-5]`, maximum record count `64` at `[6]`, zero at `[7]`, and a CRC-32 over
bytes `[0-7]` at `[8-11]`. Base and alpha payload streams are appended before
the record table; each record stores their offsets, lengths, and individual
CRCs together with the content hash, source CRC, width, and height. A 60-byte
record ends with a CRC-32 in `[56-59]` over bytes `[0-55]`.

The 24-byte PMEN footer stores magic `PMEN`, version `2`, record count, a zero
reserved byte, the 64-bit record-table offset, the aggregate CRC-32 of all
encoded records, and a final CRC-32 over the first 20 footer bytes. Validation
checks the header, local record CRCs, payload bounds and CRCs, aggregate record
CRC, footer, count, and exact file size before any spool is used for resume.

### PDF cache compatibility and versioning

A PRMF codec, format, or capability mismatch, invalid CRC, invalid
required-file ledger, or source-identity mismatch makes the derived
generation unusable and causes safe rebuild behavior. XPMD, XPOL, PRPG, and
PWIH/PWIF validate their own local versions and integrity fields. PXC has no
internal version, so incompatible changes to it require a PRMF capability
version bump.

Only a completed PRMF slot publishes a generation. PRCP checkpoints and
transient spools cannot expose partial output. Format changes must update the
corresponding source constant and this document together. PDF progress and
saved items live in the cache root rather than a generation so rebuilt
pagination can retain word-based progress, bookmarks, and clippings.

## Crash-safe PDF move and delete state

CrossInk keeps global recovery state under `/.crosspoint/` while a PDF move or
delete crosses the source file, path-derived cache, bookmarks, clippings, and
recent-book state. These records are operation journals, not reading caches.

### `book_move.a` / `book_move.b`: BMJ1 version 1

The two book-move slots contain a variable-length record with a 40-byte prefix,
two raw absolute paths of at most 1,023 bytes each, and a final four-byte CRC-32:

- `[0-3]` magic `BMJ1`
- `[4-5]` version (`1`)
- `[6-7]` prefix size (`40`)
- `[8-11]` monotonic sequence
- `[12]` phase (`1=Prepared`, `2=SourceMoved`, `3=CacheCopied`,
  `4=BookmarksCopied`, `5=ClippingsCopied`, `6=StateVerified`,
  `7=Activated`, `8=OldStateRemoved`, `9=Abandoned`)
- `[13]` book format (`2=PDF`)
- `[14]` committed marker (`0xA5`)
- `[15]` recents policy (`0=Keep`, `1=Remove`)
- `[16-23]` old path hash
- `[24-31]` new path hash
- `[32-33]` old path byte length
- `[34-35]` new path byte length
- `[36-37]` combined path payload length
- `[38-39]` reserved zero
- `[40...]` old path bytes followed immediately by new path bytes, without
  terminators
- final four bytes: CRC-32 over every preceding byte

A new move starts in slot A at sequence 1 and each legal phase transition
writes the opposite slot. Loading chooses the newest CRC-valid slot with the
same signed-delta wrap rule as the PDF cache stores; A wins an equal-sequence
tie. Both slots are removed only after `OldStateRemoved` or `Abandoned` is
durable.

### `pdf_delete.a` / `pdf_delete.b`: PDJ1 version 1

The PDF-delete journal has a 40-byte prefix, six raw absolute target paths of
at most 1,023 bytes each, and a final CRC-32:

- `[0-3]` magic `PDJ1`
- `[4-5]` version (`1`)
- `[6-7]` prefix size (`40`)
- `[8-11]` monotonic sequence
- `[12]` phase (`1=Prepared`, `2=SourceHidden`, `3=FullCachePurged`,
  `4=BookmarksPurged`, `5=ClippingsPurged`, `6=RecentsPurged`,
  `7=SourceRemoved`)
- `[13]` book format (`1=PDF`)
- `[14]` committed marker (`0xA5`)
- `[15]` reserved zero
- `[16-27]` six `uint16_t` path lengths in source, tombstone, cache,
  bookmarks, clippings, and recent-state order
- `[28-31]` combined path payload length
- `[32-35]` total encoded length, including the trailing CRC
- `[36-39]` reserved zero
- `[40...]` the six raw paths in the same order, without terminators
- final four bytes: CRC-32 over every preceding byte

Deletion begins in slot A at sequence 1 and advances through the opposite slot.
The newest valid wrap-aware sequence is selected; two valid slots with the same
sequence are treated as corrupt instead of guessing. Normal cleanup removes
both slots only after `SourceRemoved` is durable.

### `pdf-directory-delete.spool`: CPDFDSH1/CPDFDSF1 version 2

A directory deletion that finds PDFs first writes
`/.crosspoint/pdf-directory-delete.spool.tmp`, syncs and validates it, then
renames it to `/.crosspoint/pdf-directory-delete.spool` before replay. The
20-byte header is:

- `[0-7]` magic `CPDFDSH1`
- `[8]` version (`2`)
- `[9]` reserved zero
- `[10-11]` selected root-path byte length
- `[12-15]` root-path CRC-32
- `[16-19]` header CRC-32 over bytes `[0-15]`

Each record is a 12-byte header followed by raw path bytes:

- `[0-3]` path byte length
- `[4]` kind (`1=PDF`, `2=legacy metadata tree`)
- `[5-7]` reserved zero
- `[8-11]` path CRC-32
- `[12...]` raw path bytes without a terminator

The aggregate record CRC covers each complete record header and path in order.
The final 28-byte footer contains:

- `[0-7]` magic `CPDFDSF1`
- `[8-11]` record count
- `[12-15]` aggregate encoded record bytes
- `[16-19]` aggregate record CRC-32
- `[20-23]` maximum path byte length
- `[24-27]` footer CRC-32 over bytes `[0-23]`

Replay verifies the root binding, each record kind and path CRC, all reserved
bytes, aggregate sizes and CRC, maximum path length, footer, and exact file
size before deleting anything. The auxiliary
`pdf-directory-delete.legacy-a` and `.legacy-b` files are temporary sealed
workspaces for bounded legacy-metadata traversal. The spool and those
workspaces are removed during post-operation cleanup; replay never accepts
their paths unless the complete envelope and root binding validate first.

## PDF `saved_items.a` / `saved_items.b`

### PSIT Version 1

PDF bookmarks and clippings use two CRC-protected slots in the book's
`/.crosspoint/pdf_<hash>/` cache root. The slots are alternated by sequence
number so a failed or interrupted SD-card write cannot overwrite the last
confirmed state. A write whose final durability is uncertain remains
quarantined for that process and is retried in the same slot with the same
sequence; it is not deleted. After a reboot, a complete CRC-valid slot may be
accepted even if the preceding run did not observe a successful close.

The fixed 80-byte header is:

- `[0-3]` magic `PSIT`
- `[4-5]` version (`1`)
- `[6-7]` header size (`80`)
- `[8-9]` record size (`56`)
- `[10-11]` total record count (`uint16_t`, maximum `128`)
- `[12-13]` bookmark count (`uint16_t`, maximum `64`)
- `[14-15]` clipping count (`uint16_t`, maximum `64`)
- `[16-19]` monotonic sequence (`uint32_t`)
- `[20-23]` flags (`bit0=source modification time is known`)
- `[24-31]` source file size (`uint64_t`)
- `[32-39]` source modification time (`uint64_t`, used only when flag bit 0 is set)
- `[40-47]` source head fingerprint (`uint64_t`)
- `[48-55]` source tail fingerprint (`uint64_t`)
- `[56-59]` document total word count (`uint32_t`)
- `[60-63]` aggregate record CRC-32 (`uint32_t`, seed zero)
- `[64-67]` header CRC-32 over bytes `[0-63]`
- `[68-79]` reserved zero bytes

Each 56-byte record is:

- `[0-1]` stable item ID (`uint16_t`); IDs must be unique within a slot,
  use the range `1` through `65534`, and `0` and `65535` are invalid
- `[2]` kind (`1=bookmark`, `2=clipping`)
- `[3]` flags (`bit0=start semantic position`, `bit1=end semantic position`,
  `bit2=fallback pages`)
- `[4-7]` timestamp (`uint32_t`)
- `[8-11]` start global word ordinal (`uint32_t`)
- `[12-15]` end global word ordinal (`uint32_t`; zero for bookmarks)
- `[16-19]` start in-block word offset (`uint32_t`)
- `[20-23]` end in-block word offset (`uint32_t`; zero for bookmarks)
- `[24-25]` semantic section index (`uint16_t`)
- `[26-27]` fallback start page (`uint16_t`)
- `[28-29]` fallback end page (`uint16_t`)
- `[30-31]` fallback page count (`uint16_t`)
- `[32-41]` fixed start-block anchor (`char[10]`, NUL-terminated)
- `[42-51]` fixed end-block anchor (`char[10]`, NUL-terminated; empty for bookmarks)
- `[52-55]` exact layout fingerprint (`uint32_t`)

Fallback pages are valid only when flag bit 2 is set, the stored page count
matches, and the nonzero layout fingerprint exactly matches the current
layout. Otherwise CrossInk resolves the semantic anchors and word ordinals or
fails safely instead of jumping to a similarly sized but different layout.
The exact file size is `80 + recordCount * 56`, with a maximum of 7,248 bytes.

PSIT is the canonical semantic saved-item state for a PDF. To keep the shared
Bookmarks and Clippings screens compatible with existing formats, CrossInk
also maintains `/.crosspoint/bookmarks/pdf_<crc32(path)>.bin` using bookmark
format version 5 and `/.crosspoint/clippings/pdf_<crc32(path)>.bin` using the
clipping format described below. The bookmark reader accepts versions 2 through
5 for migration. These compatibility files do not select or validate a PDF
generation; the BMJ1 and PDJ1 journals move or remove them alongside the
canonical PSIT slots.

## `/.crosspoint/clippings/<bookType>_<crc32(path)>.bin`

### Version 1

Clipping files store the per-book EPUB clipping list and the PDF compatibility
list used by shared reader screens. A saved clipping is also what CrossInk
renders as an in-reader highlight; there is no separate highlight file. The
file lives in `/.crosspoint/clippings/` instead of a render-cache directory so
clearing or rebuilding layout cache does not delete user clippings.

The current implementation writes EPUB clipping files and PDF saved-item
compatibility files, so `bookType` is `epub` or `pdf`. The numeric suffix is
`uzlib_crc32()` of the book's SD-card path, for example:

```text
/.crosspoint/clippings/epub_1234567890.bin
```

Binary layout:

- `[0]` version (`1`)
- `[1-2]` clipping count (`uint16_t` LE, maximum `64`)
- book title (`String`)
- book author (`String`)
- book path (`String`)
- repeated clipping records:
  - `spineIndex` (`uint16_t` LE)
  - `startPage` (`uint16_t` LE)
  - `endPage` (`uint16_t` LE)
  - `pageCount` (`uint16_t` LE, at least `1`)
  - `startWordIndex` (`uint16_t` LE)
  - `endWordIndex` (`uint16_t` LE)
  - `wordCount` (`uint16_t` LE)
  - `paragraphIndex` (`uint16_t` LE, `UINT16_MAX` when unavailable)
  - `timestamp` (`uint32_t` LE, seconds since firmware boot when saved)
  - `chapterTitle` (`char[48]`, null-terminated/truncated)
  - selected text (`String`, truncated to `512` bytes for the in-app store)

CrossInk uses the stored spine/page/paragraph fields as anchors, then searches
near that location for the stored clipping text after relayout. This is similar
to keeping both a DOM position and a text quote in a web app: the numeric
position gives a fast starting point, while the text makes jumps and highlights
survive font, layout, or page-count changes when possible.

Creating a clipping also appends a Kindle-style export entry to
`/My Clippings.txt` on the SD-card root. That text export can keep up to `2000`
bytes of the selected text and is append-only. Removing a clipping from the
reader deletes or rewrites only the binary clipping file; it does not remove
previous entries from `/My Clippings.txt`.

When CrossInk moves an EPUB or PDF through its built-in move-to-Read flow, it
rewrites the clipping file under the new path-derived name and removes the old
one. If a book is renamed or moved outside CrossInk, the path hash changes, so
the old clipping file may no longer be associated with the book until the file
is moved back or the clipping store is migrated.

## `stats_v5.bin`

### Version 5

`stats_v5.bin` stores per-book reading statistics for stats schema version 5.
Versioned filenames let firmware branches with different stats schemas keep
their own per-book stats files without overwriting each other. Version 5 extends
version 4 with a cached live reader book time-left estimate so Home and Reading
Stats can show the same estimate the reader last computed.

When `stats_v5.bin` is missing, CrossInk can read the previous versioned stats
filename (`stats_v4.bin` for version 5, `stats_v5.bin` after a future version 6
bump) before falling back to legacy `stats.bin` files with compatible stats
payloads. Future changes are always saved to the current versioned filename.

Binary layout:

- `[0]` version (`5`)
- `[1-2]` `sessionCount` (`uint16_t` LE)
- `[3-6]` `totalReadingSeconds` (`uint32_t` LE)
- `[7-10]` `totalPagesTurned` (`uint32_t` LE)
- `[11]` `isCompleted` (`uint8_t`)
- `[12-13]` `avgSecondsPerForwardPage` (`uint16_t` LE)
- `[14-15]` `paceSampleCount` (`uint16_t` LE)
- `[16]` flags (`bit0=startDateManual`, `bit1=finishedDateManual`)
- `[17-20]` `startDate` (`year uint16_t` LE, `month uint8_t`, `day uint8_t`)
- `[21-24]` `finishedDate` (`year uint16_t` LE, `month uint8_t`, `day uint8_t`)
- `[25-40]` `timeOfDaySeconds[4]` (`uint32_t` LE each)
- `[41-68]` `dayOfWeekSeconds[7]` (`uint32_t` LE each)
- `[69-72]` `estimatedTimeLeftSeconds` (`uint32_t` LE, `0` means unavailable)

## `section.bin`

### Version 56

Each file in `sections/*.bin` stores one laid-out spine section. The header is
also the cache-busting key: if any layout-affecting setting differs from the
current reader settings, the section is discarded and rebuilt.

Version 56 changes `<br>` layout: a line break after text no longer reapplies
the containing block's top or bottom spacing, while an empty `<br>` block keeps
the existing scene-break gap. Full and suspended partial section caches rebuild
together. Version 55 assigns compact IDs to internal EPUB links. The ID is
stored in the existing per-word flags byte and in each page's footnote entry so
touch devices can map tapped text to the existing fragment-navigation path
without retaining another per-word data structure. Version 54 adds compact
ruby-text annotations to serialized text blocks. Only words that begin a ruby
group store annotation text; continuation words use a dedicated style bit. This
keeps books without ruby markup unchanged apart from the cache version while
avoiding an empty string allocation for every word.
Version 53 stores each image's EPUB-internal source path so section indexing can
read only its header and defer full extraction until the page is shown. Version
52 keeps Guide Dots centered when extra word spacing is enabled. Version 51
preserves continuation state for oversized CJK word fragments. Version 50
paginates chapter-heading image runs within the reader viewport so they do not
overflow into the reserved status-bar area. Version 49 stores Bionic Reading
split-run offsets in visual order so RTL word prefixes render on the right.
Version 48 changed Arabic contextual shaping and text measurement, so cached
word positions from version 47 no longer match what `drawText` renders.

Version 48 makes the EPUB word-spacing level widen the natural inter-word gap
(each level adds 10 pixels), which changes laid-out word positions, so
older sections must rebuild. Version 46 added the EPUB word-spacing level to the
cache-busting header. It retains the flat `TextBlock` arena and chapter-opener
anchor behavior introduced in version 45. It includes:

- cache-busting fields for font, line compression, extra paragraph spacing,
  forced paragraph indents, paragraph alignment, viewport size, hyphenation,
  embedded CSS, image rendering mode, Bionic Reading, Guide Dots, word spacing,
  and EPUB render mode
- page offset LUT
- anchor-to-page map for fragment and footnote navigation
- paragraph and list-item LUTs used by KOReader sync page refinement
- optional per-word Bionic Reading split metadata
- optional per-word Guide Dot x-offset metadata
- optional per-word text flags for CSS backgrounds, layout-inserted hyphens,
  and internal-link IDs
- reading-aid layout that stores Bionic Reading and Guide Dots as per-word metadata instead of temporary layout words
- publisher CSS page-break handling and adjusted justification spacing baked into page layout
- table fragments
- per-page footnote entries
- per-page publisher page markers
- serialized word style bits for underline, strikethrough, superscript, and
  subscript
- flat TextBlock word storage: per-word arrays plus one shared NUL-terminated
  text blob, replacing length-prefixed word strings and parallel vectors. The
  on-disk order mirrors the in-RAM arena so the firmware reads a whole block
  payload with a single allocation and a single SD read

ImHex pattern:

```c++
import std.mem;
import std.string;
import std.core;

#define EXPECTED_VERSION 55
#define MAX_STRING_LENGTH 65535
#define FOOTNOTE_NUMBER_LEN 32
#define FOOTNOTE_HREF_LEN 96

struct String {
    u32 length [[hidden, comment("String byte length")]];
    if (length > MAX_STRING_LENGTH) {
        std::warning(std::format("Unusually large string length: {} bytes", length));
    }
    char data[length] [[comment("UTF-8 string data")]];
} [[sealed, format("format_string"), comment("Length-prefixed UTF-8 string")]];

fn format_string(String s) {
    return s.data;
};

enum PageElementTag : u8 {
    TAG_PageLine = 1,
    TAG_PageImage = 2,
    TAG_PageTableFragment = 3,
    TAG_PageHorizontalRule = 4
};

enum WordStyle : u8 {
    REGULAR = 0,
    BOLD = 1,
    ITALIC = 2,
    BOLD_ITALIC = 3,
    UNDERLINE = 4,
    STRIKETHROUGH = 8,
    SUP = 16,
    SUB = 32
};

enum TextAlign : u8 {
    JUSTIFIED = 0,
    LEFT_ALIGN = 1,
    CENTER_ALIGN = 2,
    RIGHT_ALIGN = 3,
    NONE = 4
};

struct BlockStyle {
    TextAlign alignment;
    bool textAlignDefined;
    s16 marginTop;
    s16 marginBottom;
    s16 marginLeft;
    s16 marginRight;
    s16 paddingTop;
    s16 paddingBottom;
    s16 paddingLeft;
    s16 paddingRight;
    s16 textIndent;
    bool textIndentDefined;
    bool isRtl;
    bool directionDefined;
};

struct TextBlock {
    u16 wordCount;
    u8 hasBionic;
    u8 hasGuideDots;
    u8 hasWordFlags;
    u16 textBytes [[comment("Total size of text[], including one NUL per word")]];

    if (wordCount > 0) {
        u16 textOff[wordCount] [[comment("Byte offset of word i's text within text[]")]];
        s16 wordXPos[wordCount];
        if (hasBionic != 0) {
            u16 wordBionicSuffixX[wordCount] [[comment("Suffix x offset from word start")]];
        }
        if (hasGuideDots != 0) {
            u16 wordGuideDotXOffset[wordCount] [[comment("Guide dot x offset from word start; 0 means no dot")]];
        }
        WordStyle wordStyle[wordCount];
        if (hasBionic != 0) {
            u8 wordBionicBoundary[wordCount] [[comment("UTF-8 byte boundary between bold prefix and suffix")]];
        }
        if (hasWordFlags != 0) {
            u8 wordFlags[wordCount] [[comment("bit 0 = black background, bit 1 = layout-inserted trailing hyphen")]];
        }
        char text[textBytes] [[comment("All words back to back, each NUL-terminated")]];
    }

    BlockStyle blockStyle;
};

struct ImageBlock {
    String imagePath;
    String sourcePath;
    s16 width;
    s16 height;
};

struct PageLine {
    s16 xPos;
    s16 yPos;
    TextBlock block;
};

struct PageImage {
    s16 xPos;
    s16 yPos;
    ImageBlock image;
};

struct PageHorizontalRule {
    s16 xPos;
    s16 yPos;
    u16 width;
    u8 thickness;
};

struct TableFragmentCell {
    bool isHeader;
    u8 lineCount;
    TextBlock lines[lineCount];
};

struct TableFragmentRow {
    u16 height;
    bool headerSeparator;
    u8 cellCount;
    TableFragmentCell cells[cellCount];
};

struct PageTableFragment {
    s16 xPos;
    s16 yPos;
    u16 width;
    u8 columnCount;
    u8 cellPadding;
    u16 lineHeight;
    u8 rowCount;
    TableFragmentRow rows[rowCount];
};

struct PageElement {
    PageElementTag pageElementType;
    if (pageElementType == TAG_PageLine) {
        PageLine pageLine [[inline]];
    } else if (pageElementType == TAG_PageImage) {
        PageImage pageImage [[inline]];
    } else if (pageElementType == TAG_PageTableFragment) {
        PageTableFragment tableFragment [[inline]];
    } else if (pageElementType == TAG_PageHorizontalRule) {
        PageHorizontalRule horizontalRule [[inline]];
    } else {
        std::error(std::format("Unknown page element type: {}", pageElementType));
    }
};

struct FootnoteEntry {
    char number[FOOTNOTE_NUMBER_LEN];
    char href[FOOTNOTE_HREF_LEN];
    u8 linkId;
};

struct PublisherPageMarker {
    s16 yPos;
    char label[16];
};

struct Page {
    u16 elementCount;
    PageElement elements[elementCount] [[inline]];

    u16 footnoteCount;
    FootnoteEntry footnotes[footnoteCount];

    u8 publisherPageMarkerCount;
    PublisherPageMarker publisherPageMarkers[publisherPageMarkerCount];
};

struct AnchorEntry {
    String anchor;
    u16 page;
};

struct AnchorMap {
    u16 count;
    AnchorEntry entries[count];
};

struct ParagraphLut {
    u16 count;
    u16 paragraphIndex[count];
};

struct SectionBin {
    u8 version;
    if (version != EXPECTED_VERSION) {
        std::error(std::format("Unsupported version: {} (expected {})", version, EXPECTED_VERSION));
    }

    s32 fontId;
    float lineCompression;
    bool extraParagraphSpacing;
    bool forceParagraphIndents;
    u8 paragraphAlignment;
    u16 viewportWidth;
    u16 viewportHeight;
    bool hyphenationEnabled;
    bool embeddedStyle;
    u8 imageRendering;
    bool bionicReadingEnabled;
    bool guideReadingEnabled;
    u8 wordSpacing;
    u8 renderMode; // 0 = CrossInk Default, 1 = Balanced, 2 = Light

    u16 pageCount;
    u32 pageLutOffset;
    u32 anchorMapOffset;
    u32 paragraphLutOffset;
    u32 listItemLutOffset;

    Page pages[pageCount];

    u32 currentOffset = $;
    if (currentOffset != pageLutOffset) {
        std::warning(std::format("Page LUT offset mismatch: expected 0x{:X}, got 0x{:X}", pageLutOffset, currentOffset));
    }

    u32 pageLut[pageCount] [[comment("Page data offsets")]];

    if (anchorMapOffset != 0) {
        AnchorMap anchorMap @ anchorMapOffset;
    }

    if (paragraphLutOffset != 0) {
        ParagraphLut paragraphLut @ paragraphLutOffset;
    }

    if (listItemLutOffset != 0 && paragraphLutOffset != 0) {
        u16 listItemIndex[paragraphLut.count] @ listItemLutOffset;
    }
};

SectionBin section @ 0x00;

u32 fileSize = std::mem::size();
u32 parsedSize = $;
if (parsedSize != fileSize) {
    std::warning(std::format("Unparsed data detected: {} bytes remaining at offset 0x{:X}", fileSize - parsedSize, parsedSize));
}
```
