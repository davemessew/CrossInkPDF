<h1 align="center">CrossPDF</h1>

<p align="center"><strong>Render PDFs like EPUBs.</strong></p>

<p align="center">
  On-device PDF reflow for CrossInk on Xteink. Use your reader font, size, spacing, margins, page turns, bookmarks, clippings, and reading progress—without converting files on a computer.
</p>

<p align="center">
  <img src="./docs/images/crosspdf-reader.jpg" width="680" alt="CrossPDF rendering a PDF as reflowed text on an Xteink e-reader">
</p>

<p align="center">
  <a href="https://github.com/davemessew/CrossPDF/releases/download/crosspdf-v1.5.0.19/firmware-tiny-crosspdf-v1.5.0.19.bin"><strong>Download Tiny</strong></a>
  &nbsp;&nbsp;·&nbsp;&nbsp;
  <a href="https://github.com/davemessew/CrossPDF/releases/download/crosspdf-v1.5.0.19/firmware-xlarge-crosspdf-v1.5.0.19.bin"><strong>Download XLarge</strong></a>
  &nbsp;&nbsp;·&nbsp;&nbsp;
  <a href="./docs/installation.md">Installation guide</a>
</p>

| **On-device reflow** | **Your reading settings** | **No conversion step** |
| :---: | :---: | :---: |
| Prepared directly on the Xteink | Uses CrossInk typography and controls | Copy the PDF to the SD card and open it |

Tiny includes 10, 12, 14, and 16 pt text. XLarge includes 16, 18, and 20 pt text. Both downloads contain the full CrossInk firmware, PDF support, emoji, and symbol support.

## PDF reflow

CrossPDF extracts the reading content from supported PDFs and passes it through CrossInk's existing reflow reader.

| A fixed-page PDF viewer | CrossPDF |
| --- | --- |
| Shrinks the original page | Repaginates the text for the display |
| Keeps the PDF's font sizes | Uses your CrossInk font and size |
| Needs zoom and pan | Uses normal page turns |
| Measures progress in PDF pages | Measures progress by words read |
| Reopens the original layout | Reuses prepared reading data from the SD card |

The PDF is still the source file. CrossPDF does not replace it or modify it.

## What is preserved

For supported PDFs, the reader keeps the parts that matter when reading:

- Document title and author metadata
- Chapters and table of contents
- Nested outline entries and document index entries
- Internal links and named destinations
- Publisher page labels
- Supported JPEG and raster images
- Reading position, calculated from words read across the book
- Resume position, bookmarks, and clippings

The PDF's visual font sizes and page dimensions are deliberately ignored. Your selected CrossInk typography is used instead.

## What happens when a PDF is opened

The first open prepares the book directly on the reader:

1. CrossPDF reads the PDF in small pieces.
2. Text, navigation, and supported images are written to reusable reading data on the SD card.
3. The normal reflow reader lays out the text with the current device settings.
4. Later opens and page turns reuse that saved data instead of parsing the PDF again.

The first open can take longer than opening an EPUB, especially for a large or complicated document. Once preparation is complete, normal reading does not keep reprocessing the source PDF. This keeps processor work and SD-card traffic down while reading.

Preparation is bounded for the ESP32-C3's limited memory. It does not borrow a second display framebuffer or try to hold the whole document in RAM.

## Which PDFs work

CrossPDF is intended for:

- Born-digital PDFs with selectable text
- Scanned PDFs that already contain a usable OCR text layer
- Documents with ordinary text columns, headings, tables, links, and supported images
- Passwordless RC4-encrypted PDFs that use the PDF Standard security handler

It is not intended for:

- Image-only scans without OCR text
- PDFs that require a password, AES encryption, or another unsupported security handler
- Comics, magazines, forms, or documents where the exact printed page is the content
- PDFs that depend on unsupported encodings, filters, or interactive features

Complex layouts are simplified into a reading order. Optional visual material that cannot be handled safely may be left out. See [PDF Support](./docs/pdf-support.md) for the detailed boundary.

## If a PDF cannot be prepared

A damaged or unsupported PDF should not take down the rest of the reader. If parsing fails, storage runs out, or the PDF needs more memory than the device can safely provide, preparation stops and the PDF stays closed. CrossPDF does not publish a half-built book cache.

The source PDF is left unchanged, so it can be removed, replaced, or tried again later.

## The rest of CrossInk is still here

PDF support is added to the existing CrossInk reading experience. EPUB behavior and its cache format remain separate.

This fork also includes:

- EPUB, TXT, and XTC reading
- Lexend Deca and Bitter reader fonts
- Inter for the interface
- Unicode emoji and miscellaneous symbol support
- Thicker underlines, strikethroughs, section breaks, and improved simple tables
- Minimal and Dashboard themes and sleep screens
- Bionic Reading, Guide Dots, redaction-style rendering, and forced paragraph indents
- Bookmarks and in-book reader settings
- Reader-specific front and side button remapping
- Automatic page turning
- Reading time, sessions, pages turned, finished-book tracking, and reading-stat sleep screens
- Nearby reading-position and all-time reading-stat sync
- Recent Books grid view and finished-book filing
- Local file transfer and the built-in EPUB optimizer

The full list of reading options is in [Reader Features](./docs/reader-features.md), and button actions are listed in [Controls](./docs/controls.md).

## Fonts and build variants

CrossInk has two firmware variants because the ESP32-C3 does not have enough flash to bundle every point size at once.

### `tiny`

The general-purpose build:

- 10, 12, 14, and 16 pt reader sizes
- Emoji and miscellaneous symbols
- Full on-device PDF support

### `xlarge`

For readers who only want larger text:

- 16, 18, and 20 pt reader sizes
- Emoji and miscellaneous symbols
- Full on-device PDF support

See [Font Build Variants](./docs/font-build-variants.md) for more detail.

## Installation

The fastest route is [Inky, CrossInk's web installer](https://inky.crossink.dev/#flash-tools):

1. Download the [`tiny` firmware](https://github.com/davemessew/CrossPDF/releases/download/crosspdf-v1.5.0.19/firmware-tiny-crosspdf-v1.5.0.19.bin) for standard text or the [`xlarge` firmware](https://github.com/davemessew/CrossPDF/releases/download/crosspdf-v1.5.0.19/firmware-xlarge-crosspdf-v1.5.0.19.bin) for large text.
2. Open the CrossInk web installer.
3. Select **Custom .bin**.
4. Choose the downloaded file and flash it.

Command-line installation and revert instructions are in the [Installation guide](./docs/installation.md).

## Tips for a smoother library

CrossInk runs on a single-core ESP32-C3 with limited RAM. A little organization makes a noticeable difference:

- Keep folders below about 200 files; 50–100 files per folder is more comfortable.
- A large library is fine when books are split into folders by author, series, genre, or read status.
- Avoid putting every book in the SD-card root.
- Text-first EPUBs and PDFs work best.
- For PDFs, check that text can be selected before copying the file to the reader.
- Image-only scans need OCR before they can be reflowed.
- Use a reliable SD card and leave free space for settings, progress, statistics, and prepared book data.

More cache and storage details are available in [Data Cache](./docs/data-cache.md).

## Documentation

- [User Guide](./USER_GUIDE.md)
- [PDF Support](./docs/pdf-support.md)
- [Installation](./docs/installation.md)
- [SD Card Fonts](./docs/sd-card-fonts.md)
- [Reader Features](./docs/reader-features.md)
- [Dictionary](./docs/dictionary.md)
- [Controls](./docs/controls.md)
- [Reading Stats Sync](./docs/reading-stats-sync.md)
- [Nearby Position Sync](./docs/nearby-position-sync.md)
- [Data Cache](./docs/data-cache.md)
- [Web server usage](./docs/webserver.md)
- [Common issues](./docs/troubleshooting.md)
- [Project scope](./SCOPE.md)
- [Development docs](./docs/development/README.md)

## Development

CrossInk uses PlatformIO for building and flashing firmware.

See [Getting Started](./docs/development/getting-started.md) for prerequisites, clone setup, and validation commands.

### Nix/NixOS

Nix/NixOS users can enter the development shell with either `nix develop` (flakes) or `nix-shell`:

```bash
nix develop -f nix
# or
nix-shell nix
```

To flash a connected ESP32-C3 device, enable PlatformIO's udev rules in your NixOS configuration:

```nix
services.udev.packages = with pkgs; [ platformio-core.udev ];
```

After rebuilding the system configuration, reconnect the device or reload udev rules.

### Build / flash / monitor

Connect your Xteink X4 or X3 via USB-C and run:

```sh
pio run -e tiny
```

Build the large-font variant with:

```sh
pio run -e xlarge
```

See [Testing and Debugging](./docs/development/testing-debugging.md) for serial logging, simulator checks, static analysis, and bug-report guidance.

## Project lineage

This project is based on [CrossInk](https://github.com/uxjulia/CrossInk), which is itself a personal fork of [CrossPoint Reader](https://github.com/crosspoint-reader/crosspoint-reader).

The current firmware base includes the official CrossInk 1.5.0 release.

CrossPDF keeps CrossInk's focused e-ink reading experience while adding on-device PDF reflow.

## Internals

The ESP32-C3 has about 380 KB of usable RAM, so CrossInk stores reusable book and device data on the SD card instead of rebuilding everything in memory.

See [Data Cache](./docs/data-cache.md) for the `.crosspoint` layout and [File Formats](./docs/file-formats.md) for binary cache details.
