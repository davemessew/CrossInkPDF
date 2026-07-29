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
terminal marker. A successful base tracer emits this ordered sequence:

```text
QEMU_BOOT seq=0
QEMU_STORAGE_PASS path=/qemu/sentinel.txt bytes=26
QEMU_FRAME_PASS bytes=48000 crc32=0F7C8C45
QEMU_INPUT_PASS button=DOWN press=1 release=1
QEMU_POWER_PASS idle_ms=3000 saving=1
QEMU_RUNTIME heap_start=... min_free=... min_max_alloc=... max_alloc=... stack_margin=...
QEMU_TRACER_PASS
```

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
```

This command builds and links the hardware target. Do not append an upload
target as part of QEMU acceptance.

## Troubleshooting target startup

If the log ends immediately after the ROM prints an entry address, the
application has not emitted `QEMU_BOOT` and the target gate is not green.
Keep the log, verify the manifest/image hashes, and diagnose the bootloader or
emulator state before adding PDF code. Host-only tests do not replace this
target-runtime gate.

With the pinned pioarduino/ESP-IDF libraries, ESP-IDF also runs ADC2 hardware
calibration from a global constructor before `setup()`. The ESP32-C3 QEMU model
does not complete that ADC event, so the QEMU environment links a target-only,
nonblocking calibration wrapper. The hardware environment does not use this
wrapper.

The current unattended Gate A replay proceeds through boot, storage,
framebuffer, and logical input, then hits an emulated Timer Group interrupt
watchdog while the FreeRTOS idle task is in `esp_cpu_wait_for_intr`. Espressif
documents a `wdt_disable` machine property, and it is useful for diagnosis, but
it is deliberately absent from the acceptance runner: disabling the watchdog
would remove the gate, and the non-interactive process still needs an external
monitor event to leave the emulated wait-for-interrupt state. Do not treat an
interactive diagnostic pass as `QEMU_TRACER_PASS` acceptance.
