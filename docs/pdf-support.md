---
title: PDF Support
nav_order: 19
---

# PDF Support

CrossInk can turn supported PDF files into reflowable books directly on the
reader. It extracts the document's reading content and lays it out with the
same device font, font size, margins, line spacing, orientation, hyphenation,
and image preference used for other reflowable books.

This is not a miniature PDF page viewer. PDF page dimensions, coordinates,
font names, and point sizes do not control the reading view, and there is no
zoom-and-pan mode.

## Supported Documents

The best results come from born-digital PDFs with selectable text. OCRed PDFs
also work when they contain a usable hidden text layer. CrossInk does not run
OCR itself.

Supported PDFs may include:

- ordinary PDF 1.x structures, including classic or stream-based cross-reference data;
- readable text with usable Unicode, standard-font, or common Latin mappings;
- multi-column pages and simple table-like regions, which are placed into a stable reading order;
- document outlines, internal destinations, contents and index text, and publisher page labels;
- meaningful JPEG images and supported Flate-compressed raster images.

PDF is a flexible container, so a `.pdf` extension alone does not guarantee
that a document can be reflowed. CrossInk accepts the capabilities it can
process safely within the X3/X4 memory limits.

## First Open And Preparation

Open a PDF from **Browse Files** in the same way as another book. The first open
shows **Preparing PDF** while CrossInk builds a reading cache on the SD card.
All preparation happens on the reader; the source PDF is never rewritten.

You can cancel preparation and return later. CrossInk saves work only at safe
boundaries and resumes a matching, valid checkpoint on the next open. A
completed cache normally reopens without extracting the PDF again, and page
turns use the prepared reading data rather than reopening the source file.

If the source file changes, CrossInk rejects its old prepared cache and builds
a new one.

## Reading, Navigation, And Progress

The reading view follows the selected device typography. Changing the font,
font size, margins, line spacing, orientation, or hyphenation can rebuild page
layout without changing the extracted document or losing the reading place.

CrossInk preserves usable PDF navigation as reading-oriented navigation:

- outline entries appear as chapters;
- resolvable contents and index links jump to their semantic destinations;
- printed contents and index text remain in the reading stream;
- original page labels remain available as publisher-page markers.

PDF progress is based on words reached divided by the total extracted words,
not on the number of original PDF pages. Resume positions, bookmarks, and
clippings use semantic text locations so they can survive a typography change
or rebuilt page layout. Moves performed inside CrossInk migrate the associated
PDF reading state to the new path.

## Images And Complex Layouts

CrossInk keeps meaningful supported raster images near their related text when
memory and storage limits allow. Decorative backgrounds, repeated watermarks,
tiny repeated icons, and unsupported or oversized images may be omitted while
the surrounding readable text remains available. Vector artwork is not
rasterized.

Complex magazine layouts, equations, charts, and spanning tables are reduced
to a reading order rather than reproduced pixel-for-pixel. This can differ
from the printed page while remaining readable on the small e-ink display.

## Unsupported PDFs

CrossInk does not support:

- image-only scans without an OCR text layer;
- encrypted or password-protected PDFs, including files with an empty password;
- fixed-page display, zoom, crop, or pan;
- on-device OCR;
- PDF forms, JavaScript, signatures, attachments, audio, or video;
- documents whose required text encoding, compression, structure, or expanded data exceeds the reader's safety limits.

An unsupported optional image can be skipped without losing otherwise readable
text. If required text or document structure cannot be read safely, CrossInk
stops without publishing a partial book and leaves the original PDF unchanged.

## Errors And Recovery

CrossInk shows a specific message when it can identify the problem:

| Message | What to do |
| --- | --- |
| **No readable text was found in this PDF** | Add an OCR text layer on a computer, then copy the new file to the reader. |
| **Password-protected PDFs are not supported** | Use an unencrypted copy that you are allowed to read. |
| **This PDF uses unsupported compression** | Create a compatible copy with standard PDF compression. |
| **This PDF uses unsupported text encoding** | Create a new PDF with embedded, Unicode-mappable text. |
| **This PDF is damaged or cannot be read safely** | Replace the file with a known-good copy. |
| **Not enough memory to prepare this PDF** | Restart the reader and try a smaller or simpler copy. |
| **Not enough storage to prepare this PDF** | Free SD-card space and try again. |

## Cache And Clear Reading Cache

Prepared PDF data lives under `/.crosspoint/pdf_<hash>/`. **Delete Book Cache**
and **Clear Reading Cache** remove prepared generations and page-layout data so
the PDF is prepared again on its next open. They preserve PDF word progress,
bookmarks, clippings, and other user state.

Deleting the entire `/.crosspoint/` directory is different: it removes reading
state and device data as well as render caches. See [Data Cache](./data-cache.md)
for the storage layout and safe clearing behavior.
