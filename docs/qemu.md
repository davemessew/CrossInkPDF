# ESP32-C3 QEMU

The `qemu-esp32c3` PlatformIO environment runs CrossInk against an isolated
ESP32-C3 HAL. It never discovers, opens, erases, or flashes a physical device.
The emulated target uses one 48,000-byte framebuffer and a generated LittleFS
image containing the fixtures under `test/qemu/data`.

## Prerequisites

- Git submodules initialized, including the pinned `freeink-sdk` revision.
- Python 3.13. The pinned pioarduino platform does not support Python 3.14.
- A repository-local virtual environment with PlatformIO Core 6.1.19:

  ```powershell
  py -3.13 -m venv .venv
  .\.venv\Scripts\python.exe -m pip install pioarduino-core==6.1.19
  ```

## Install the pinned emulator

Install Espressif QEMU `esp_develop_9.2.2_20250817` into the ignored
`.tools/qemu-esp32c3` directory:

```powershell
python scripts/install_qemu_esp32c3.py
```

The installer verifies the release archive SHA-256 before extracting it and
writes `.tools/qemu-esp32c3/install.json`. On Windows, it also gives the
emulator a private `libiconv-2.dll`, records that DLL's hash, and smoke-tests
the executable. It searches a Git for Windows installation by default. An
explicit runtime directory can be supplied when needed:

```powershell
python scripts/install_qemu_esp32c3.py `
  --windows-runtime-dir "C:\Program Files\Git\mingw64\bin"
```

## Build the target image

On Windows, force UTF-8 output so PlatformIO can render dependency metadata:

```powershell
$env:PYTHONUTF8 = "1"
.\.venv\Scripts\pio.exe run -e qemu-esp32c3 -t qemu-image
```

The build produces a 16 MiB flash image and an eFuse image under
`.pio/build/qemu-esp32c3`, plus `qemu_manifest.json` containing their paths,
hashes, offsets, and build metadata.

## Run the boot tracer

Remove an old log before each acceptance replay:

```powershell
Remove-Item -LiteralPath `
  .pio\build\qemu-esp32c3\qemu-tracer.log `
  -ErrorAction SilentlyContinue
python scripts/run_qemu_esp32c3.py `
  --expect QEMU_TRACER_PASS `
  --log .pio/build/qemu-esp32c3/qemu-tracer.log
```

The runner copies the mutable flash and eFuse images to a temporary directory,
starts the ESP32-C3 machine with deterministic instruction counting, rejects
panic/reset/watchdog output, and terminates QEMU only after the requested
terminal marker. It uses `-icount shift=3,sleep=off`: when FreeRTOS executes
`WFI`, QEMU advances virtual time directly to the next timer deadline instead
of waiting for a host console event. The emulated Timer Group watchdog remains
enabled. A successful base tracer emits this ordered sequence:

```text
QEMU_BOOT seq=0
QEMU_STORAGE_PASS path=/qemu/sentinel.txt bytes=26
QEMU_FRAME_PASS bytes=48000 crc32=0F7C8C45
QEMU_INPUT_PASS button=DOWN press=1 release=1
QEMU_POWER_PASS idle_ms=3000 saving=1
QEMU_RUNTIME heap_start=... min_free=... min_max_alloc=... max_alloc=... stack_margin=...
QEMU_TRACER_PASS
```

## Run the PDF target acceptance gate

The boot tracer proves the emulated board, storage, display, input, power, and
runtime probes are alive. The full target replay adds the PDF behavior gate.
Build a fresh image, remove the previous log, and request its terminal marker:

```powershell
$env:PYTHONUTF8 = "1"
.\.venv\Scripts\pio.exe run -e qemu-esp32c3 -t qemu-image
Remove-Item -LiteralPath `
  .pio\build\qemu-esp32c3\pdf-acceptance.log `
  -ErrorAction SilentlyContinue
python scripts/run_qemu_esp32c3.py `
  --expect QEMU_TEST_PASS `
  --log .pio/build/qemu-esp32c3/pdf-acceptance.log
```

This replay requires the base markers followed by the PDF acceptance markers,
and rejects panic, reset, and watchdog output. The PDF checks cover semantic
extraction and reflow, cache publication and validation, cancellation and
resume, device typography, navigation, images, word-based progress, cached
reopen, rejected-input behavior, and the EPUB compatibility oracle. Reaching
`QEMU_TEST_PASS` proves the functional target replay only.

Run the resource verifier separately against the same firmware and runtime log:

```powershell
python scripts/check_qemu_resources.py verify `
  --baseline test/qemu/baselines/esp32c3-55.03.37-arduino-3.3.7.json `
  --manifest .pio/build/qemu-esp32c3/qemu_manifest.json `
  --elf .pio/build/qemu-esp32c3/firmware.elf `
  --runtime-log .pio/build/qemu-esp32c3/pdf-acceptance.log
```

The resource command must independently exit with status zero against the
pinned baseline. It gates static DRAM/BSS growth at 12 KiB, additional
PDF-owned heap at 80 KiB, free heap at 44 KiB, largest free block at 40 KiB,
the largest PDF allocation at 32 KiB, and stack margin at 1 KiB. Exact-boundary
positive controls and one-byte failures lock those comparisons.

The verifier still records QEMU code/rodata for diagnosis, but does not treat
acceptance-harness growth as a release flash-capacity failure. QEMU includes
target-only fixtures, oracles, instrumentation, and HAL code. The authoritative
flash gate is the real `default`, `tiny`, and `xlarge` production partition-fit
check described below. A functional `QEMU_TEST_PASS`, a QEMU resource result,
and production partition fit are separate evidence and none substitutes for
the others.

The 2026-08-03 measured amendment replaced the former 64 KiB/48 KiB QEMU
low-water floors. The current replay measured 49,612 bytes free and a
45,044-byte largest block; an older paired replay measured 47,052/42,996. The
maximum PDF allocation was 32,768 bytes, peak PDF-owned heap was 65,240 bytes,
stack margin was 9,412 bytes, the framebuffer was present, steady-state heap
recovered, and 100 cached page turns showed no heap erosion. This is emulator
acceptance evidence under the no-hardware constraint, not physical X4 memory,
timing, or battery validation.

For cancellation timing, the emulator ceiling is 8 ms of cooperative CPU time,
32 operations, and a 4 KiB request. Cooperative time is total step time minus
time inside separately instrumented synchronous HAL callbacks; no
uninstrumented time is subtracted. A wall-time overrun is accepted only for one
non-recursive callback from this allowlist:

- write: 1-1,024 requested bytes and at most 30 ms;
- rename: no payload and at most 24 ms;
- read-only open: no payload and at most 12 ms.

Each exceptional slice permits at most 500 us of non-callback work. The complete
cancellation replay permits at most 26 exceptions: 22 writes, two renames, and
two read-only opens, with aggregate ceilings of 3,072 requested bytes, 550,000
us callback time, and 5,000 us non-callback time. Cancellation is checked as
soon as a synchronous callback returns; the callback itself is not preemptible.

## Prove that QEMU cannot flash hardware

Run the wrapper below after changing PlatformIO targets, upload hooks, or the
QEMU environment:

```powershell
python scripts/verify_qemu_no_flash.py `
  --pio .venv/Scripts/pio.exe
```

It invokes `upload`, `uploadfs`, `uploadfsota`, `erase`, `erase_upload`, and
`download_fs`. Each command must print `QEMU target cannot be flashed` before
PlatformIO's own failure summary, return nonzero, and contain no serial-port or
flash-tool output. The wrapper prints `QEMU_NO_FLASH_PASS` only when all six
targets are safely refused.

## Compile the physical firmware without uploading

The QEMU HAL and logging seams must remain compile-time isolated from the real
firmware:

```powershell
$env:PYTHONUTF8 = "1"
.\.venv\Scripts\pio.exe run -e default -j 4
.\.venv\Scripts\pio.exe run -e tiny -j 4
.\.venv\Scripts\pio.exe run -e xlarge -j 4
```

Each build runs `scripts/check_firmware_size.py`, which compares the actual
`firmware.bin` with the smallest application partition in `partitions.csv`.
Both OTA slots are currently `0x640000` (6,553,600 bytes), so all three
production variants must fit independently. Recorded 2026-08-03 artifacts were
5,901,392 bytes (`default`), 5,901,328 bytes (`tiny`), and 5,764,240 bytes
(`xlarge`), leaving 652,208, 652,272, and 789,360 bytes respectively. Rebuild
for release evidence; these recorded sizes do not replace a fresh build.

These commands build and link the hardware targets without installing them. Do
not append an upload target as part of QEMU acceptance.

## Troubleshooting target startup

If the log ends immediately after the ROM prints an entry address, the
application has not emitted `QEMU_BOOT` and the target gate is not green.
Keep the log, verify the manifest/image hashes, and diagnose the bootloader or
emulator state before changing target or acceptance code. Host-only tests do
not replace this target-runtime gate.

With the pinned pioarduino/ESP-IDF libraries, ESP-IDF runs ADC2 hardware
calibration from a global constructor before `setup()`. The ESP32-C3 QEMU model
does not complete that ADC event, so the QEMU environment links a target-only,
nonblocking calibration wrapper. The hardware environment does not use this
wrapper.

Do not add Espressif's diagnostic `wdt_disable` machine property to the
acceptance runner. The `sleep=off` instruction-counting mode makes unattended
idle/timer progress correctly while preserving watchdog coverage.
