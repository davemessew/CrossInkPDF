---
title: Simulator
nav_order: 15
---

# Development Device Simulator

CrossInk can run in the [CrossPoint simulator](https://github.com/uxjulia/crosspoint-simulator), which renders the e-ink display in an SDL2 window. Use it for quick sanity checks without flashing firmware every time.

## Platform Support

The simulator is currently configured for macOS on Apple Silicon.

The `platformio.ini` `[env:simulator]` section contains hardcoded `-arch arm64` and Homebrew paths under `/opt/homebrew`.

- Intel Mac users need to remove `-arch arm64` and change Homebrew paths to `/usr/local`.
- Linux requires similar path changes plus a replacement for `lib/simulator_mock/src/MD5Builder.h`, which uses the macOS-only `CommonCrypto` API.
- The interactive SDL simulator is not configured for native Windows. Use WSL
  and follow the Linux adjustments, or use the containerized PDF acceptance
  replay described below.

## Prerequisites

```sh
# macOS
brew install sdl2

# Linux (Debian/Ubuntu)
sudo apt install libsdl2-dev
```

## Setup

Place EPUB or PDF books in `./fs_/books/` relative to the project root. That
maps to the SD-card `/books/` path on device.

## Build And Run

```sh
pio run -e simulator
.pio/build/simulator/program
```

## Keyboard Controls

| Key | Action |
| --- | --- |
| Up / Down | Page back / forward (side buttons) |
| Left / Right | Left / right front buttons |
| Return | Confirm / Select |
| Escape | Back |
| P | Power |

## PDF Acceptance Replay

The repository includes a deterministic, headless PDF acceptance runner. Its
container mode supplies the pinned Ubuntu and SDL environment, so it is also
the preferred PDF simulator gate on Windows:

```powershell
python scripts/run_pdf_simulator_acceptance.py --container --headless
```

The runner builds the simulator, stages its own PDF fixtures in a temporary SD
card image, and exercises cancelled preparation, resumed preparation, a clean
uncached open, and a cached reopen. It checks semantic reflow output, progress
and navigation behavior, images, cache artifacts, negative fixtures, and an
EPUB regression fixture against the checked-in oracle. It prints
`PDF_SIMULATOR_ACCEPTANCE_PASS` only after every phase and oracle check succeeds.

The runner owns its temporary filesystem; it does not need books from
`./fs_/books/`. Use `--update-oracle` only when an intentional behavior change
has been reviewed, because that option replaces the expected output instead of
testing against it.

## Cache Note

On first open of an EPUB, an **Indexing...** popup appears while the section
cache is built in `.crosspoint/`. On first open of a supported PDF, a
**Preparing PDF** popup reports preparation progress and allows
cancellation; opening the book again resumes from a valid checkpoint when one
is available.

If rendering looks stale after a code change, clear the affected book cache in
the simulated UI. Deleting `./fs_/.crosspoint/` is a full simulator reset and
also removes progress, bookmarks, settings, and other user state.
