# On-Device Reflowable PDF Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add safe, entirely on-device PDF extraction to CrossInk so supported PDFs read like EPUBs with device typography, preserved semantic navigation and images, word-based progress, resumable low-energy preparation, and no physical-device flashing during development or acceptance.

**Architecture:** A bounded, callback-driven PDF core incrementally extracts semantic XHTML, navigation metadata, word ordinals, and selected images into a two-slot power-loss-safe cache. `PdfReflowDocument` and `Epub` implement one section-level `ReflowDocument` seam consumed by the existing paginator and reader. ESP32-C3 QEMU is established before PDF parsing and remains the mandatory target-runtime safety gate; the native simulator is the UI oracle.

**Tech Stack:** C++20 without exceptions, Arduino/pioarduino for ESP32-C3, PlatformIO, FreeRTOS, existing HAL/SdFat and EPUB paginator, uzlib with caller-owned dictionary, CMake/GoogleTest host tests, Python standard-library fixture/runner scripts, Espressif QEMU `esp_develop_9.2.2_20250817`, SDL native simulator.

---

## Execution Contract

The approved design is
`docs/superpowers/specs/2026-07-29-pdf-reflow-design.md`. If this plan and that
specification differ, the specification wins.

- Do not flash, probe, enumerate, or connect to a physical ESP32-C3. The
  `qemu-esp32c3` upload target must fail before serial discovery.
- Do not stage, commit, push, or create a PR unless the user gives separate
  explicit authorization. Every task below ends with a non-commit checkpoint.
- Preserve the existing dirty state. At plan creation, `docs/superpowers/` is
  untracked and all other tracked files match `main` at `54ba245f`.
- Initialize the existing `freeink-sdk` submodule before target builds; do not
  replace its pinned revision.
- Follow red-green-refactor. A red witness must fail for the behavior being
  added, not merely because a test cannot compile.
- Keep exactly one caller-owned source-PDF read handle. Every cache file is
  short-lived, synchronized where required, and explicitly closed.
- Before reading any xref/CMap/font/run spill, close the source at a verified
  continuation boundary; close the spill before reopening/seeking the source.
  The single-reader fake must exercise every spill phase.
- The PDF hot core contains no `new`, `malloc`, `std::vector`, `std::string`, or
  `std::function`. Host test doubles may use them.
- Do not change EPUB section cache version 44, the serialized `Page` format,
  EPUB `progress.bin`, or existing EPUB cache paths.
- Add user-visible strings only to `lib/I18n/translations/english.yaml`, then run
  the generator. Do not hand-edit generated translations.
- Do not claim battery-current savings from QEMU. Prove bounded work,
  race-to-idle, no polling task, and return to the existing power-saving path.

## Phase and Gate Order

| Gate | Work unlocked when green |
|---|---|
| A. QEMU tracer | Minimal PDF tracer may begin |
| B. EPUB reflow regression | Shared seam and PDF document adapter may land |
| C. `QEMU_PDF_CORE_PASS` | Full PDF structural/text core may expand |
| D. `QEMU_PDF_TRACER_PASS` | Navigation, progress, images, and product integration |
| E. Cache and reader integration | Full native/QEMU acceptance |
| F. Native + `QEMU_TEST_PASS` | Documentation and completion report |

Never substitute a host-only pass for a QEMU gate. If a gate fails, stop at the
smallest failing hypothesis; do not layer speculative production changes.

## Phase I — Safe ESP32-C3 QEMU Foundation

### Task 1: Add host-side no-flash and resource-check witnesses

**Files:**

- Create: `test/qemu/test_no_flash.py`
- Create: `test/qemu/test_qemu_runner.py`
- Create: `test/qemu/test_qemu_resources.py`
- Create: `scripts/refuse_qemu_flash.py`
- Create: `scripts/verify_qemu_no_flash.py`
- Create: `scripts/check_qemu_resources.py`
- Create: `scripts/run_qemu_esp32c3.py` (host process monitor; Task 3 adds target image wiring)
- Modify: `test/README`

- [x] Add `test_no_flash.py` with a subprocess witness that executes
  `scripts/refuse_qemu_flash.py`, expects exit code `2`, expects exactly
  `QEMU target cannot be flashed` on stderr, and rejects `COM`, `/dev/tty`,
  `serial`, `write_flash`, and imports of `serial` or `esptool`.
- [x] Add a verifier which runs each prohibited PlatformIO target and exits zero
  only when every command refuses with the exact message before enumeration:
  `upload`, `uploadfs`, `uploadfsota`, `erase`, `erase_upload`, and
  `download_fs`.
- [x] Run the no-flash test before the script exists:

  ```powershell
  python -m unittest discover -s test/qemu -p "test_no_flash.py" -v
  ```

  Expected: FAIL because the refusal script is absent.

- [x] Implement `refuse_qemu_flash.py` using only `sys.stderr.write(...)` and
  `raise SystemExit(2)`.
- [x] Add resource-check tests for every approved boundary. Each positive
  control changes one value by one byte:
  `text=262145`, `data+bss=12289`, PDF heap `81921`, free heap `65535`,
  largest block `49151`, allocation `32769`, and stack margin `1023`.
- [x] Implement `check_qemu_resources.py` with `capture` and `verify` commands.
  It must reject a baseline whose platform/framework/build fingerprint differs,
  use the PlatformIO RISC-V `size` executable recorded in the manifest, and
  print only one terminal `QEMU_RESOURCE_PASS` on success.
- [x] Parse `riscv32-esp-elf-size -A` and sum exactly:
  `.iram0.text`, `.iram0.vectors`, `.flash.text`, `.flash.rodata` for
  code/rodata; `.dram0.data`, `.dram0.bss`, `.noinit` for static DRAM. Resolve
  the absolute tool path from the PlatformIO toolchain manifest. Across multiple
  boots, use worst free-heap/largest-block/stack values, never only the final
  boot.
- [x] Add runner unit tests with a fake QEMU executable for pass marker, fail
  marker, panic, Guru Meditation, abort, watchdog reset, restart loop, timeout,
  unexpected exit, and missing terminal marker. The pass fake must remain alive
  after printing the marker so the test proves the host terminates it. Add one
  valid armed-reset sequence and negative unarmed/repeated-reset sequences.
- [x] Re-run:

  ```powershell
  python -m unittest discover -s test/qemu -p "test_*.py" -v
  ```

  Expected: all host safety tests PASS.

- [x] Record a non-commit checkpoint:

  ```powershell
  git diff -- test/qemu scripts/refuse_qemu_flash.py scripts/check_qemu_resources.py test/README
  git status --short
  ```

### Task 2: Add the isolated QEMU HAL

**Files:**

- Create: `test/qemu/hal/library.json`
- Create: `test/qemu/hal/src/HalClock.h`
- Create: `test/qemu/hal/src/HalClock.cpp`
- Create: `test/qemu/hal/src/HalDisplay.h`
- Create: `test/qemu/hal/src/HalDisplay.cpp`
- Create: `test/qemu/hal/src/HalGPIO.h`
- Create: `test/qemu/hal/src/HalGPIO.cpp`
- Create: `test/qemu/hal/src/HalPowerManager.h`
- Create: `test/qemu/hal/src/HalPowerManager.cpp`
- Create: `test/qemu/hal/src/HalSpiBus.h`
- Create: `test/qemu/hal/src/HalSpiBus.cpp`
- Create: `test/qemu/hal/src/HalStorage.h`
- Create: `test/qemu/hal/src/HalStorage.cpp`
- Create: `test/qemu/hal/src/HalSystem.h`
- Create: `test/qemu/hal/src/HalSystem.cpp`
- Create: `test/qemu/hal/src/HalTiltSensor.h`
- Create: `test/qemu/hal/src/HalTiltSensor.cpp`
- Create: `test/qemu/hal/src/common/FsApiConstants.h`
- Create: `test/qemu/hal/src/QemuHalControl.h`
- Create: `test/qemu/data/qemu/sentinel.txt`
- Test: `test/qemu/test_qemu_hal_contract.py`

- [x] Add a source-level contract test which compares the public types and
  required method signatures of `lib/hal` with `test/qemu/hal/src`; also assert
  that the QEMU HAL has one and only one 48,000-byte framebuffer and no
  persistent task creation.
- [x] Run it before creating the HAL:

  ```powershell
  python -m unittest discover -s test/qemu -p "test_qemu_hal_contract.py" -v
  ```

  Expected: FAIL listing the missing mirrored headers.

- [x] Implement the HAL with these fixed behaviors:

  - `HalDisplay`: one static `uint8_t framebuffer[48000]`, deterministic CRC32,
    no display-sized heap allocation.
  - `HalStorage`: mount Arduino LittleFS on partition label `spiffs`; adapt
    `fs::File` to the move-only `HalFile` API with 64-bit checked seek/size and
    explicit close.
  - `HalGPIO`: target-safe physical-state shim only; logical scripted input is
    injected through `MappedInputManager` in Task 4.
  - `HalPowerManager`: record power-saving transitions without touching
    unsupported clock/sleep peripherals.
  - `QemuHalControl`: expose only `frameCrc32`, storage/open counters,
    injectable storage quota, capacity model, and `powerSavingEnabled`.
  - Capacity model: report a separate virtual SD of 64 MiB total and 32 MiB
    initially free even though LittleFS backs it. Enforce actual fixture/cache
    bytes against the `0x360000` partition and use the quota for ENOSPC tests.

- [x] Put exactly `crossink-qemu-sentinel-v1` followed by one LF in
  `test/qemu/data/qemu/sentinel.txt`.
- [x] Re-run the contract and inspect forbidden allocations:

  ```powershell
  python -m unittest discover -s test/qemu -p "test_qemu_hal_contract.py" -v
  rg -n "\bnew\b|malloc|xTaskCreate|std::vector" test/qemu/hal
  ```

  Expected: contract PASS; `rg` has no production QEMU HAL hit requiring an
  unbounded or persistent allocation.

- [x] Record the diff and status without staging.

### Task 3: Add the PlatformIO QEMU environment and offline image builder

**Files:**

- Modify: `platformio.ini`
- Create: `scripts/qemu_build.py`
- Create: `scripts/qemu_no_flash.py`
- Create: `scripts/install_qemu_esp32c3.py`
- Modify: `scripts/run_qemu_esp32c3.py`
- Create: `test/qemu/test_qemu_build.py`
- Create: `test/qemu/test_qemu_install.py`
- Modify: `.gitignore`

- [x] Add failing parser tests which require a `qemu-esp32c3` environment to:

  - extend the pinned ESP32-C3 base;
  - retain C++20, `-fno-exceptions`, linker wrappers, and 16 MiB layout;
  - remove USB-on-boot flags and define `CROSSINK_QEMU=1`;
  - override physical `lib_deps`, use `lib_ignore = hal`, and add only
    `qemu-hal=symlink://test/qemu/hal` plus non-hardware dependencies;
  - set `upload_protocol = custom`;
  - invoke the refusal script without serial discovery for all six unsafe
    targets;
  - use LittleFS and the tiny release font omissions;
  - register `qemu-image`.

- [x] Run:

  ```powershell
  python -m unittest discover -s test/qemu -p "test_qemu_build.py" -v
  ```

  Expected: FAIL because the environment does not exist.

- [x] Add `[env:qemu-esp32c3]` to `platformio.ini`. Keep the base target,
  framework, compiler, partition table, image size, exception policy, and
  release memory model. Override `lib_deps` so physical Battery/Input/EInk/SD/
  Power SDK libraries are absent; assert the resolved dependency graph contains
  none. Copy the `tiny` font-omission flags, keep
  `CROSSPOINT_FIRMWARE_VARIANT="tiny"`, define `CROSSINK_QEMU=1`, set
  `board_build.filesystem = littlefs`, and do not define `SIMULATOR`.
- [x] In `scripts/qemu_no_flash.py`, inspect `COMMAND_LINE_TARGETS` before the
  platform's `BeforeUpload` hooks. Immediately refuse `upload`, `uploadfs`,
  `uploadfsota`, `erase`, `erase_upload`, and `download_fs` using an absolute,
  quoted `sys.executable` path. Test every target.
- [x] In `scripts/qemu_build.py`, register `qemu-image` and:

  1. register after the platform builder and depend on both
     `$BUILD_DIR/${PROGNAME}.bin` and
     `env.DataToBin("$BUILD_DIR/qemu-data", "$PROJECT_DIR/test/qemu/data")`;
  2. create LittleFS with page 256, block 4096, and size `0x360000`, failing
     before QEMU if fixtures plus required writable headroom do not fit;
  3. parse PlatformIO-generated flash arguments rather than inventing offsets;
  4. assert bootloader `0x0`, partition table `0x8000`, OTA selector
     `0xe000`, app `0x10000`, and fixture FS `0xc90000`;
  5. get the platform-managed esptool via
     `env.PioPlatform().setup_python_env(env)` and invoke `merge_bin` only;
  6. generate the ESP32-C3 revision-0.3 `qemu_efuse.bin` from pinned ESP-IDF
     5.5.2 data;
  7. emit `qemu_flash.bin` exactly 16 MiB plus `qemu_manifest.json` containing
     hashes, offsets, tool versions, and build flags.

- [x] Pin Espressif QEMU `esp_develop_9.2.2_20250817` in
  `install_qemu_esp32c3.py`. For Windows verify SHA-256
  `9474015f24d27acb7516955ec932e5307226bd9d6652cdc870793ed36010ab73`
  and for Linux x64 verify
  `373b37a68bae3ef441ead24a7bfc950fcbfc274cbdd2b628fc6915f179eb1d8e`.
  Write `install.json` and normalize the executable to
  `.tools/qemu-esp32c3/qemu/bin/qemu-system-riscv32[.exe]`; the runner reads
  this file and never relies on `PATH`. Add `/.tools/` to `.gitignore`.
- [x] Implement the runner command:

  ```text
  qemu-system-riscv32 -M esp32c3
    -drive file=<temporary-flash>,if=mtd,format=raw
    -drive file=<temporary-efuse>,if=none,format=raw,id=efuse
    -global driver=nvram.esp32c3.efuse,property=drive,value=efuse
    -nic none
    -nographic -serial mon:stdio
  ```

  It must copy both mutable images to a temporary directory, never pass
  `wdt_disable`, stream logs, enforce a timeout, and terminate only after the
  requested terminal marker. Support an armed reset protocol:
  `QEMU_EXPECT_RESET seq=N` followed by exactly one `QEMU_BOOT seq=N+1`;
  reject unannounced/repeated resets and cap expected sequences.

- [x] Re-run all QEMU Python tests. Expected: PASS.
- [x] Record the diff and status without staging.

### Task 4: Boot the real ESP32-C3 QEMU tracer

**Files:**

- Create: `src/qemu/QemuAcceptance.h`
- Create: `src/qemu/QemuAcceptance.cpp`
- Create: `src/qemu/QemuPlatformStubs.cpp`
- Modify: `src/main.cpp`
- Modify: `lib/Logging/Logging.h`
- Modify: `src/MappedInputManager.h`
- Modify: `src/MappedInputManager.cpp`
- Create: `docs/qemu.md`

- [x] Add a marker-sequence test requiring:

  ```text
  QEMU_BOOT seq=0
  QEMU_STORAGE_PASS path=/qemu/sentinel.txt bytes=26
  QEMU_FRAME_PASS bytes=48000 crc32=0F7C8C45
  QEMU_INPUT_PASS button=DOWN press=1 release=1
  QEMU_POWER_PASS idle_ms=3000 saving=1
  QEMU_RUNTIME heap_start=... min_free=... min_max_alloc=... stack_margin=...
  QEMU_TRACER_PASS
  ```

- [x] Under `CROSSINK_QEMU`, bind logging to UART `Serial0` instead of `HWCDC`;
  skip the USB-enumeration delay and USB-specific `setTxTimeoutMs()` call.
  Outside that define, preserve the hardware path exactly.
- [x] Add `qemuAcceptanceBegin(...)` after normal setup and
  `qemuAcceptanceTick()` inside the real event loop. Use `esp_rom_printf` for
  acceptance markers so success does not depend on normal logging.
- [x] The tracer must read the sentinel through `HalStorage`, render the fixed
  framebuffer pattern (all 48,000 bytes `0xFF`, then byte zero `0x7F`; standard
  CRC32 `0F7C8C45`), inject and consume Down press/release, wait through the
  real 3000 ms idle threshold, observe power saving, and sample:
  `ESP.getFreeHeap()`, `ESP.getMinFreeHeap()`, `ESP.getMaxAllocHeap()`, and
  `uxTaskGetStackHighWaterMark(nullptr) * sizeof(StackType_t)`.
- [x] Broaden the existing logical injection seam in
  `MappedInputManager.h/.cpp` to `SIMULATOR || CROSSINK_QEMU`. Inject
  `MappedInputManager::Button::Down`; do not inject a raw physical GPIO index.
- [x] Install a repository-local Python 3.13 runtime/venv because the pinned
  pioarduino platform rejects this host's Python 3.14. Initialize the pinned
  submodule:

  ```powershell
  py install --target=.tools\python313 3.13
  .\.tools\python313\python.exe -m venv .venv
  .\.venv\Scripts\python.exe -m pip install -U https://github.com/pioarduino/platformio-core/archive/refs/tags/v6.1.19.zip
  git submodule update --init --recursive
  python scripts/install_qemu_esp32c3.py
  ```

  All later `pio` commands in this plan mean the repository-local
  `.\.venv\Scripts\pio.exe` (or `.venv/bin/pio` in Linux/container). CI must use
  Python 3.13.

- [x] Build and run the tracer:

  ```powershell
  .\.venv\Scripts\pio.exe run -e qemu-esp32c3 -t qemu-image
  python scripts/run_qemu_esp32c3.py --expect QEMU_TRACER_PASS --log .pio/build/qemu-esp32c3/qemu-tracer.log
  ```

  Expected: all component markers followed by `QEMU_TRACER_PASS`; no panic,
  abort, watchdog reset, or restart loop.

- [x] Prove all unsafe target guards using the zero-on-expected-refusal wrapper:

  ```powershell
  python scripts/verify_qemu_no_flash.py --pio .venv/Scripts/pio.exe
  ```

  Expected: wrapper exits zero only after all six targets fail with
  `QEMU target cannot be flashed`; logs contain no port enumeration and no
  device-writing command.

- [x] Build the unchanged hardware environment without uploading:

  ```powershell
  .\.venv\Scripts\pio.exe run -e default
  ```

  Expected: firmware links successfully.

- [x] Document the exact pinned QEMU install, build, run, marker, and refusal
  commands in `docs/qemu.md`.
- [x] Stop here if QEMU does not boot. A host test is not a substitute.

  **2026-07-29 completion:** the QEMU image and unchanged hardware environment
  build successfully, and `QEMU_NO_FLASH_PASS` is green. A QEMU-only linker
  wrapper bypasses the ESP-IDF ADC2 global calibration constructor because the
  emulator never completes its ADC event. The unattended runner uses
  `-icount shift=3,sleep=off`, which advances virtual time to the next timer
  while FreeRTOS is in `esp_cpu_wait_for_intr`; the Timer Group watchdog remains
  enabled. The ordered boot, storage, framebuffer, input, power, and runtime
  markers now end in `QEMU_TRACER_PASS`. Gate A is green.

### Task 5: Capture the pre-PDF target resource baseline

**Files:**

- Create: `test/qemu/baselines/esp32c3-55.03.37-arduino-3.3.7.json`
- Modify: `scripts/check_qemu_resources.py`
- Modify: `.github/workflows/ci.yml`

- [x] Capture only after Task 4 is green:

  ```powershell
  python scripts/check_qemu_resources.py capture `
    --manifest .pio/build/qemu-esp32c3/qemu_manifest.json `
    --elf .pio/build/qemu-esp32c3/firmware.elf `
    --runtime-log .pio/build/qemu-esp32c3/qemu-tracer.log `
    --out test/qemu/baselines/esp32c3-55.03.37-arduino-3.3.7.json
  ```

- [x] Verify the same build against the captured baseline and run every
  one-byte positive control:

  ```powershell
  python scripts/check_qemu_resources.py verify `
    --baseline test/qemu/baselines/esp32c3-55.03.37-arduino-3.3.7.json `
    --manifest .pio/build/qemu-esp32c3/qemu_manifest.json `
    --elf .pio/build/qemu-esp32c3/firmware.elf `
    --runtime-log .pio/build/qemu-esp32c3/qemu-tracer.log
  python -m unittest discover -s test/qemu -p "test_qemu_resources.py" -v
  ```

  Expected: `QEMU_RESOURCE_PASS`; every deliberate one-byte violation fails.

- [x] Add a `windows-latest` `qemu-tracer` CI job which installs the pinned
  Python 3.13, PlatformIO, and QEMU versions, initializes submodules, builds
  `qemu-image`, and runs the tracer. Add it to `test-status.needs` only after
  the local tracer is green.
- [x] Define the comparison fingerprint as toolchain/platform/framework
  versions, normalized flags, partition hash, and QEMU HAL/config hashes. The
  baseline source commit is informational; PDF source changes are allowed and
  measured as size deltas rather than rejected as environment drift.
- [x] Record the diff/status. Do not commit.

## Phase II — Shared Reflow Reader Without EPUB Regression

### Task 6: Capture cached and uncached EPUB behavior before refactoring

**Files:**

- Create: `src/simulator/EpubReflowRegressionOracle.h`
- Create: `src/simulator/EpubReflowRegressionOracle.cpp`
- Create: `test/epubs/test_reader_rendering_matrix.oracle.json`
- Create: `docker/pdf-simulator/Dockerfile`
- Create: `scripts/run_pdf_simulator_container.py`
- Modify: `src/simulator/SimulatorSmokeTest.cpp`
- Modify: `scripts/run_simulator_smoke_test.py`

- [x] Add a pinned Ubuntu/SDL container route before capturing the baseline.
  Its self-test must verify SDL2, compiler, clang-format 21, PlatformIO, Python,
  fonts, and a read/write mounted test filesystem. The container may build and
  run the native simulator but may not expose host serial devices.
- [x] Extend the simulator runner with format-neutral `--book`, `--passes`,
  `--page-turns`, and `--reflow-oracle` arguments.
- [x] Pin reader settings, device dimensions, orientation, font, margins,
  status-clock inputs, input timing, semantic page targets, and initial
  progress/bookmark state. Reset the fixture filesystem before the uncached
  pass, then retain only the generated book cache for the cached pass. Compare
  reader-content frames, not preparation/status animations.
- [x] Capture exact oracles for section count, selected TOC tuples, href
  resolution, streamed XHTML hash, first/middle/last page text and framebuffer
  hashes, section-cache hash, progress, bookmark, and resume.
- [x] Run once uncached and once cached against the same temporary filesystem:

  ```powershell
  python scripts/run_pdf_simulator_container.py --build
  python scripts/run_pdf_simulator_container.py -- `
    python scripts/run_simulator_smoke_test.py `
      --book test/epubs/test_reader_rendering_matrix.epub `
      --passes 2 --page-turns 10 `
      --reflow-oracle test/epubs/test_reader_rendering_matrix.oracle.json
  ```

  Expected: distinct uncached/cached markers and identical locked outputs.

- [x] Change one expected framebuffer hash by one digit, rerun, and confirm the
  runner fails. Restore the correct value and confirm PASS. This is the oracle's
  positive control.
- [x] Record the generated oracle and evidence log without staging.

### Task 7: Introduce `ReflowDocument` and its host contract

**Files:**

- Create: `lib/Reflow/ReflowDocument.h`
- Create: `test/reflow_document/CMakeLists.txt`
- Create: `test/reflow_document/ReflowDocumentContractTest.cpp`
- Modify: `test/CMakeLists.txt`

- [x] Scaffold only the compiling declarations, then write a behavior-red
  contract for capability-bit composition, borrowed-resource immutability, and
  virtual section/resource dispatch. The initial scaffold must compile and fail
  on wrong values/behavior; a missing header is not the red witness.
- [x] Run:

  ```powershell
  cmake -S test -B build/test -G Ninja -DCMAKE_BUILD_TYPE=Debug
  cmake --build build/test --target reflow_document_tests
  ctest --test-dir build/test -R ReflowDocument --output-on-failure
  ```

  Expected RED: exact capability/resource behavior mismatch.

- [x] Define:

  ```cpp
  enum class ReflowDocumentFormat : uint8_t { Epub, Pdf };

  enum class ReflowCapability : uint16_t {
    ExternalProgressSync = 1u << 0,
    NearbyProgressSync = 1u << 1,
    PublisherRenderModes = 1u << 2,
    EmbeddedStyles = 1u << 3,
    SavedItems = 1u << 4,
  };

  enum class ReflowResourceKind : uint8_t {
    Streamed,
    BorrowedLocalFile,
  };

  enum class ReflowImageKind : uint8_t {
    EncodedImage,
    PixelCache,
  };

  struct ReflowResource {
    ReflowResourceKind kind = ReflowResourceKind::Streamed;
    ReflowImageKind imageKind = ReflowImageKind::EncodedImage;
    std::string localPath;
    uint16_t width = 0;
    uint16_t height = 0;
    bool paginatorMayDelete = false;
  };

  struct ReflowReadingPosition {
    int sectionIndex = 0;
    int pageNumber = 0;
    int pageCount = 0;
    bool hasPageCount = false;
    bool hasSemanticPosition = false;
    uint32_t globalWordOrdinal = 0;
    uint32_t blockWordOffset = 0;
    std::string blockAnchor;
  };
  ```

  Add `ReflowSectionInfo`, hierarchical `ReflowTocEntry`, and define the seam
  and inheritance explicitly:

  ```cpp
  class ReflowSectionSource {
   public:
    virtual ~ReflowSectionSource() = default;
    virtual bool getLocalSectionPath(
        int sectionIndex, ReflowResource& out) const = 0;
    virtual bool streamSection(
        int sectionIndex, Print& out, size_t chunkSize) const = 0;
    virtual bool resolveResource(
        int sectionIndex, const std::string& href,
        ReflowResource& out) const = 0;
    virtual bool streamResource(
        int sectionIndex, const std::string& href,
        Print& out, size_t chunkSize) const = 0;
    virtual bool getResourceSize(
        int sectionIndex, const std::string& href,
        size_t* size) const = 0;
    virtual CssParser* getCssParser() const = 0;
  };

  class ReflowDocument : public ReflowSectionSource {
    // Document metadata, navigation, progress, and cover methods below.
  };
  ```

  A borrowed local section/image is immutable generation-owned data;
  `paginatorMayDelete` is always false for PDF cache resources. EPUB temporary
  extraction explicitly returns true only for files the current paginator
  already owns.

- [x] Add exact document methods for path/cache identity; title/author/language;
  cover/adaptive-thumbnail paths and generation (forward-declare
  `GfxRenderer`); section count/info/size; TOC hierarchy; href resolution;
  progress/percentage/reference pages; and position persistence.

- [x] Keep Arduino types out of the interface by forward-declaring `Print` and
  `CssParser`. Virtual dispatch is permitted only at section/resource/document
  boundaries.
- [x] Test virtual destruction, format/store keys, render/sync capability bits,
  metadata and cover methods, TOC hierarchy, href resolution, ownership for
  local and streamed sections/resources, and semantic-position round trip using
  one fake implementation.
- [x] Re-run the contract. Expected: PASS.

### Task 8: Adapt `Epub` while preserving every serialized byte

**Files:**

- Create: `lib/Reflow/LegacyPageProgressStore.h`
- Create: `lib/Reflow/LegacyPageProgressStore.cpp`
- Modify: `lib/Epub/Epub.h`
- Modify: `lib/Epub/Epub.cpp`
- Modify: `src/activities/reader/EpubReaderUtils.h`
- Modify: `test/reflow_document/CMakeLists.txt`
- Test: `test/reflow_document/LegacyPageProgressStoreTest.cpp`

- [x] Keep host coverage to the pure progress store: legacy four-byte reads,
  current exact six-byte reads/writes, `.bak` recovery/rotation, invalid
  lengths, and uint16 bounds. Exercise real `Epub` spine/TOC,
  streamSection/streamResource, cache path, reference pages, and capability
  behavior through the Task 6 simulator oracle; the lightweight host harness
  does not pretend to supply Arduino/HAL/ZIP/rendering.
- [x] Move the legacy page-tuple codec below the activity layer into
  `LegacyPageProgressStore`. Leave `EpubReaderUtils` as compatibility wrappers;
  `Epub` must not depend on a `src/activities` header.
- [x] Make `Epub : public ReflowDocument` with no adapter allocation.
  Preserve existing public EPUB methods for non-reader consumers.
- [x] Delegate generic section calls to existing spine methods and generic TOC
  calls to existing TOC methods. Both local-path methods return `false`; the
  existing ZIP extraction path remains active.
- [x] Set both sync capabilities, `PublisherRenderModes`, `EmbeddedStyles`,
  `SavedItems`, and store key `"epub"`.
- [x] Move the existing progress logic behind `loadReadingPosition` and
  `saveReadingPosition`; compare resulting files byte-for-byte to the baseline.
- [x] Run host progress-store tests and the Task 6 two-pass simulator oracle.
  Expected: no changed EPUB hash, cache file, or behavior.

### Task 9: Migrate `Section` and the XHTML parser to the narrow source

**Files:**

- Modify: `lib/Epub/Epub/Section.h`
- Modify: `lib/Epub/Epub/Section.cpp`
- Modify: `lib/Epub/Epub/parsers/ChapterHtmlSlimParser.h`
- Modify: `lib/Epub/Epub/parsers/ChapterHtmlSlimParser.cpp`
- Modify: `test/reflow_document/CMakeLists.txt`
- Test: `test/reflow_document/ReflowSectionSourceTest.cpp`

- [x] Add pure fake-source contract tests plus simulator oracles with one
  ZIP-like streamed source and one loose local XHTML source. For the local
  source, assert no second copy is written before parsing.
- [x] Change `Section` ownership from `shared_ptr<Epub>` to
  `shared_ptr<ReflowDocument>`.
- [x] Change `ChapterHtmlSlimParser` to hold `ReflowSectionSource&` plus section
  index rather than a copied EPUB `shared_ptr`.
- [x] Resolve images through a resource result containing local path,
  ownership kind, encoded/pixel-cache kind, dimensions, and delete permission;
  preserve the current EPUB stream path when no local resource is exposed.
- [x] Treat borrowed PDF XHTML/images as immutable. Parser promotion, retry,
  low-memory cleanup, decode failure, cancellation, and OOM must never rename,
  truncate, or remove a borrowed generation file. Add a simulator fault witness
  for every cleanup exit.
- [x] Make `Section::hasHtmlCache()` recognize a borrowed local XHTML source
  without classifying it as an EPUB-owned `cache/html/...` file. EPUB must
  retain its existing HTML promotion/retry path exactly.
- [x] Keep `SECTION_FILE_VERSION = 44`, all header/LUT offsets, Page
  serialization, HTML cache names, image names, CSS behavior, TOC anchor
  insertion, and retries unchanged.
- [x] Test that EPUB never enters the direct-local branches and PDF borrowed
  resources never report `paginatorMayDelete=true`.
- [x] Run host tests and both EPUB simulator passes. Expected: byte-identical.

### Task 10: Migrate supporting reader consumers and capability gates

**Files:**

- Modify: `src/activities/reader/EpubReaderChapterSelectionActivity.h`
- Modify: `src/activities/reader/EpubReaderChapterSelectionActivity.cpp`
- Modify: `lib/KOReaderSync/ProgressMapper.h`
- Modify: `lib/KOReaderSync/ProgressMapper.cpp`
- Modify: `lib/KOReaderSync/ChapterXPathResolver.h`
- Modify: `lib/KOReaderSync/ChapterXPathResolver.cpp`
- Modify: `src/activities/reader/NearbyBookPositionSyncActivity.h`
- Modify: `src/activities/reader/NearbyBookPositionSyncActivity.cpp`
- Modify: `src/activities/reader/EpubReaderMenuActivity.h`
- Modify: `src/activities/reader/EpubReaderMenuActivity.cpp`
- Modify: `test/reflow_document/CMakeLists.txt`
- Test: `test/reflow_document/ReflowCapabilityTest.cpp`

- [x] Add a fake EPUB-capable document and PDF-like no-sync document. Red tests
  require both sync actions and quick actions for the first, none for the
  second.
- [x] Retarget chapter selection and section/TOC mapping to
  `ReflowDocument`.
- [x] Retarget `ProgressMapper`, `ChapterXPathResolver`, and Nearby sync inputs
  to `ReflowDocument`; their section streams, sizes, progress, and TOC needs are
  in the shared seam. Capability-gate invocation for PDF. Use no RTTI,
  downcast, or attempt to construct `Epub` from a PDF path.
- [x] Gate Nearby sync and configured quick actions using capability bits.
- [x] Run host tests and the EPUB oracle. Expected: existing EPUB menus and sync
  remain visible and unchanged.

### Task 11: Migrate the main EPUB reader activity without renaming it

**Files:**

- Modify: `src/activities/reader/EpubReaderActivity.h`
- Modify: `src/activities/reader/EpubReaderActivity.cpp`
- Modify: `test/reflow_document/CMakeLists.txt`
- Test: `test/reflow_document/ReflowReaderProgressTest.cpp`

- [x] Keep host tests at the pure fake-document/interface level. Exercise real
  `Epub`, `Section`, parser, and activity behavior through the Task 6 simulator
  oracle unless this task also adds complete Arduino/HAL/rendering host stubs.
  Test section loading, TOC jump, internal href, progress load/save, relayout,
  menu store key, and capability policy.
- [x] Replace the owned `shared_ptr<Epub>` with
  `shared_ptr<ReflowDocument>`, retaining class/file names to keep the diff
  bounded.
- [x] Replace spine calls with section calls, and use document-owned progress.
  Use `getStoreFormatKey()` for bookmarks/clippings.
- [x] Apply publisher render modes and embedded styles only when the document
  advertises those capabilities. EPUB retains current settings exactly; PDF
  is forced to device typography/light semantic markup even if global
  `SETTINGS.epubRenderMode` requests another mode.
- [x] Load bookmark/clipping stores only when `SavedItems` is advertised.
  Otherwise explicitly unload/clear prior global saved-item state before the
  document becomes interactive. EPUB remains enabled; the minimal PDF stays
  disabled until Task 21.
- [x] Keep the static sleep-page loader EPUB-only in this task; its
  format-aware factory arrives with `PdfReflowDocument`.
- [x] Run:

  ```powershell
  ctest --test-dir build/test --output-on-failure
  python scripts/run_pdf_simulator_container.py -- `
    python scripts/run_simulator_smoke_test.py `
      --book test/epubs/test_reader_rendering_matrix.epub `
      --passes 2 --page-turns 10 `
      --reflow-oracle test/epubs/test_reader_rendering_matrix.oracle.json
  .\.venv\Scripts\pio.exe run -e default
  .\.venv\Scripts\pio.exe check -e default --fail-on-defect low --fail-on-defect medium --fail-on-defect high
  ```

  Expected: all host tests, cached/uncached EPUB oracles, target link, and
  static analysis PASS.

- [x] Re-run QEMU tracer/resource verification. Expected: tracer PASS,
  environment fingerprint unchanged, and the intentional seam size delta
  reported against the original baseline. Stop if EPUB output changes.

  **2026-07-29 completion:** all 121 host tests, both EPUB simulator
  oracle passes, the default ESP32-C3 link, and cppcheck pass. QEMU emits
  `QEMU_TRACER_PASS` and `QEMU_RESOURCE_PASS`. Cumulative shared-reflow work
  adds 5,970 bytes of code/rodata and 0 bytes of static DRAM against the
  original pre-PDF baseline; all runtime resource measurements are unchanged.
  The QEMU config fingerprint was narrowed to the effective `[base]` and
  `[env:qemu-esp32c3]` sections after its positive control exposed that native
  simulator-only flags incorrectly invalidated the target-runtime baseline.

## Phase III — Bounded PDF Core

### Task 12: Create deterministic PDF fixtures and allocation-free core seams

**Files:**

- Create: `scripts/generate_pdf_reflow_fixtures.py`
- Create: `test/pdf_reflow_core/CMakeLists.txt`
- Create: `test/pdf_reflow_core/PdfTestIo.h`
- Create: `test/pdf_reflow_core/PdfTestIo.cpp`
- Create: `test/pdf_reflow_core/PdfByteSourceTest.cpp`
- Create: `test/pdf_reflow_core/PdfAllocationTest.cpp`
- Create: `test/pdf_reflow_core/fixtures/`
- Modify: `test/CMakeLists.txt`
- Create: `lib/PdfReflow/PdfTypes.h`
- Create: `lib/PdfReflow/PdfLimits.h`
- Create: `lib/PdfReflow/PdfCheckedMath.h`
- Create: `lib/PdfReflow/PdfIo.h`
- Create: `lib/PdfReflow/PdfIo.cpp`
- Create: `lib/PdfReflow/PdfHalIo.h`
- Create: `lib/PdfReflow/PdfHalIo.cpp`
- Create: `lib/PdfReflow/PdfWorkBudget.h`

- [x] Change the host CMake project from `CXX` to `C CXX`; the PDF test target
  must compile the real uzlib C sources, not host zlib.
- [x] Write the standard-library-only generator for the approved tiny,
  license-safe corpus. It must calculate xref offsets and SHA-256 hashes rather
  than embed hand-maintained offsets.
- [x] Generate at least:
  `classic_text`, `incremental_update`, `xref_stream_objstm`, `filter_matrix`,
  `tounicode_simple_and_cid`, `operators_actualtext_forms`, `hidden_ocr`,
  `hidden_ocr_visible_duplicate`, `scan_only`, `columns_table`,
  `repeated_bands`, `dense_spill`, `vector_caption`, `font_size_6`,
  `font_size_72`, `linearized_hint`, `lzw_required`, `bad_startxref`,
  `xref_prev_cycle`, `oversized_length`, `flate_bomb`, and `encrypted`.
  Each `.expected.json` contains transcript, word count, warning/error,
  geometry order, outline/link map, and relevant image hashes.
- [x] Add `--check`, which regenerates into a temporary directory and
  byte-compares every fixture, expected file, and `SHA256SUMS`.
- [x] In `test/pdf_reflow_core/CMakeLists.txt`, use
  `find_package(Python3 REQUIRED COMPONENTS Interpreter)` and register the
  generator `--check` command directly with `add_test`. The final CTest run
  must execute fixture determinism rather than relying on a C++ proxy.
- [x] Run before accepting/checkpointing generated outputs:

  ```powershell
  python scripts/generate_pdf_reflow_fixtures.py --check
  ```

  Expected RED: missing or differing outputs; after generation, PASS.

- [x] Define callback-only `ByteSource`, `ByteSink`, `FixedRecordStore`,
  `WorkBudget`, `Status`, `Error`, `StepState`, and `StepResult`. `PdfHalIo`
  adapts a caller-owned `HalFile`; it never opens, owns, or closes the PDF.
- [x] Before implementing `PdfIo`, add behavior-red byte-source tests for short
  reads, out-of-range slices, checked addition overflow, exact
  `UnexpectedEof`/`InvalidOffset` statuses, and budget-one output equivalence.
  The red witness must call a compiling stub and fail on the wrong status, not
  only on a missing header.
- [x] Add checked offset/range/multiplication and fixed 16.16 matrix helpers.
- [x] Put approved production defaults in `PdfLimits.h`: 100,000 indirect
  objects; 5,000 pages; 250,000 content operators/page; 10,000,000
  operators/document; 16 nested form XObjects; 64 MiB expanded required
  non-image streams; 200:1 expansion ratio; four filters; plus the approved
  bounded nesting/trailer/page-tree/CMap/image limits. Tests separately assert
  these constants.
- [x] Allocate workspaces once outside the hot core:

  | Workspace | Bytes |
  |---|---:|
  | uzlib dictionary | 32,768 |
  | source buffer | 4,096 |
  | decoder output | 4,096 |
  | page text | 8,192 |
  | 256 run records | at most 12,288 |
  | operand/order/histogram | at most 2,048 |

  Total must be at most 63,488 bytes; no individual allocation exceeds
  32,768 bytes.

- [x] Add `static_assert(sizeof(PdfTextRun) <= 48)` when the type appears and
  `static_assert(sizeof(PdfToken) <= 128)`.
- [x] Add allocation interception tests proving zero hot-core heap calls after
  initialization, including failure when the interceptor is armed.
- [x] Run:

  ```powershell
  cmake -S test -B build/test -G Ninja -DCMAKE_BUILD_TYPE=Release
  cmake --build build/test --target PdfReflowCoreTest
  ctest --test-dir build/test --output-on-failure -R "^Pdf"
  rg -n "\bnew\b|malloc|std::vector|std::string|std::function" lib/PdfReflow
  ```

  Expected: fixture/core tests PASS; forbidden-container scan has no hot-core
  use.

### Task 13: Implement checked lexing and classic-xref text tracer

**Files:**

- Create: `lib/PdfReflow/PdfLexer.h`
- Create: `lib/PdfReflow/PdfLexer.cpp`
- Create: `lib/PdfReflow/PdfObjectParser.h`
- Create: `lib/PdfReflow/PdfObjectParser.cpp`
- Create: `lib/PdfReflow/PdfXref.h`
- Create: `lib/PdfReflow/PdfXref.cpp`
- Create: `lib/PdfReflow/PdfObjectResolver.h`
- Create: `lib/PdfReflow/PdfObjectResolver.cpp`
- Create: `lib/PdfReflow/PdfPageTree.h`
- Create: `lib/PdfReflow/PdfPageTree.cpp`
- Modify: `src/qemu/QemuAcceptance.cpp`
- Modify: `scripts/qemu_build.py`
- Test: `test/pdf_reflow_core/PdfByteSourceTest.cpp`
- Test: `test/pdf_reflow_core/PdfLexerObjectTest.cpp`
- Test: `test/pdf_reflow_core/PdfXrefResolverTest.cpp`
- Test: `test/pdf_reflow_core/PdfPageTreeTest.cpp`
- Test: `test/pdf_reflow_core/PdfFixtureIntegrationTest.cpp`

- [x] Add red tests for every 4 KiB split, short read, checked-add overflow,
  EOF, names with `#xx`, comments, nested literal strings, hex padding,
  references, malformed nesting, token limits, and one-operation work budgets.
- [x] Implement resumable lexing/object parsing with caller-owned token storage.
  Every long method returns `StepResult`. The pure core decrements operation and
  4 KiB byte budgets and calls the supplied stop callback only at bounded
  boundaries; `PdfPreparation` later supplies the 8 ms deadline and
  cancellation callback. Assert byte-identical output with operation budget 1
  and the normal budget 32.
- [x] Add red end-to-end test: resolve catalog → page tree → content in
  `classic_text.pdf` and emit exactly `Hello PDF`.
- [x] Implement `startxref`, classic xref, trailers, newest incremental revision
  precedence, and `/Prev` cap/cycle detection. Reject `/Encrypt`, including an
  empty-password dictionary, before content extraction. Treat a linearization
  dictionary as an ordinary indirect object and ignore its hints.
- [x] Add `PdfPageTree.h/.cpp` and `PdfPageTreeTest.cpp` for inherited
  resources, single/array `/Contents`, cycles, depth, checked page count, and
  the 5,000-page production cap. Keep page traversal separate from object
  resolution.
- [x] Reject bad offsets before seeking. Test every truncation of the tiny
  classic fixture and `/Prev` cycles.
- [x] Run `ctest -R "^Pdf"`. Expected: PASS, including `PdfPageTree`, with
  stable error classes and no crash/allocation.
- [x] Stage `classic_text.pdf` into the generated QEMU fixture filesystem and
  extend `src/qemu/QemuAcceptance.cpp` with a target-core tracer. It must read
  the file through QEMU `HalStorage`, run the same parser code, compare exactly
  `Hello PDF`, and emit `QEMU_PDF_CORE_PASS`.
- [x] Run the QEMU core tracer before expanding the parser:

  ```powershell
  Remove-Item -LiteralPath .pio\build\qemu-esp32c3\qemu-pdf-core.log -ErrorAction SilentlyContinue
  .\.venv\Scripts\pio.exe run -e qemu-esp32c3 -t qemu-image
  python scripts/run_qemu_esp32c3.py `
    --expect QEMU_PDF_CORE_PASS `
    --log .pio/build/qemu-esp32c3/qemu-pdf-core.log
  python scripts/check_qemu_resources.py verify `
    --baseline test/qemu/baselines/esp32c3-55.03.37-arduino-3.3.7.json `
    --manifest .pio/build/qemu-esp32c3/qemu_manifest.json `
    --elf .pio/build/qemu-esp32c3/firmware.elf `
    --runtime-log .pio/build/qemu-esp32c3/qemu-pdf-core.log
  ```

  Stop here if the target core tracer is not green. Host-only parsing does not
  unlock Tasks 14–17.

### Task 14: Add bounded filters, xref streams, and object streams

**Files:**

- Create: `lib/PdfReflow/PdfStreamDecoder.h`
- Create: `lib/PdfReflow/PdfStreamDecoder.cpp`
- Modify: `lib/PdfReflow/PdfXref.h`
- Modify: `lib/PdfReflow/PdfXref.cpp`
- Modify: `lib/PdfReflow/PdfObjectResolver.h`
- Modify: `lib/PdfReflow/PdfObjectResolver.cpp`
- Test: `test/pdf_reflow_core/PdfStreamDecoderTest.cpp`
- Test: `test/pdf_reflow_core/PdfXrefResolverTest.cpp`

- [ ] Add red tests for raw, ASCIIHex, ASCII85, Flate, ASCIIHex→Flate,
  ASCII85→Flate, one supported four-stage chain, and fifth-filter rejection.
  Cover ASCII85 `z`, `~>`, and whitespace; ASCIIHex odd nibble and terminator;
  every input/output split; truncated zlib/Adler data; expanded-byte cap; and
  200:1 expansion-ratio cap.
- [ ] Assert a required LZW-only content stream returns `UnsupportedFilter`
  without committing output; an optional LZW image may be omitted with warning.
- [ ] Use `InflateReader::initWithExternalDictionary` with the caller-owned
  32 KiB dictionary. A test must fail if the hidden-allocating `init(true)` path
  is used.
- [ ] Add `/W`, sparse `/Index`, xref stream, object stream, and compressed
  object tests. For each test-only limit, the same small fixture passes at `N`
  and fails at `N-1`; separately assert approved production constants.
- [ ] Reuse the fixed-record 64-entry external merge pattern from
  `lib/FileIndex/FileIndex.cpp`; do not retain object-count-sized arrays.
- [ ] Xref merge writes may be sequential while parsing, but close the source
  at a verified boundary before reading/merging spill runs; close all spill
  readers before reopening/seeking the source. Exercise this phase with the
  single-reader fake.
- [ ] Implement newest-revision wins and safe object-stream resolution with
  recursion/cycle checks.
- [ ] Run:

  ```powershell
  cmake --build build/test --target PdfReflowCoreTest
  ctest --test-dir build/test --output-on-failure -R "^Pdf(Stream|Xref)"
  ```

  Expected: PASS; malformed widths/indexes fail before unsafe read/allocation.

### Task 15: Add Unicode, CMaps, encodings, fonts, and text operators

**Files:**

- Create: `lib/PdfReflow/PdfUnicode.h`
- Create: `lib/PdfReflow/PdfUnicode.cpp`
- Create: `lib/PdfReflow/PdfCMap.h`
- Create: `lib/PdfReflow/PdfCMap.cpp`
- Create: `lib/PdfReflow/PdfEncoding.h`
- Create: `lib/PdfReflow/PdfEncoding.cpp`
- Create: `lib/PdfReflow/PdfFontMap.h`
- Create: `lib/PdfReflow/PdfFontMap.cpp`
- Create: `lib/PdfReflow/PdfPageModel.h`
- Create: `lib/PdfReflow/PdfContentInterpreter.h`
- Create: `lib/PdfReflow/PdfContentInterpreter.cpp`
- Test: `test/pdf_reflow_core/PdfCMapFontMapTest.cpp`
- Test: `test/pdf_reflow_core/PdfContentInterpreterTest.cpp`

- [ ] Add red CMap tests for 1–4 byte codespaces, `bfchar`, scalar/array
  `bfrange`, UTF-16BE, surrogate pairs, `/Differences`, and mapping priority:
  `/ActualText` → `/ToUnicode` → simple encoding → conservative common Latin.
- [ ] Keep Standard, WinAnsi, MacRoman, PDFDoc, and Unicode classification
  tables in flash. Spill over-cap mappings to fixed records. Close the source
  before reading CMap/font-map spills, close them before reopening the source,
  and exercise both spill types with the single-reader fake.
- [ ] Return `UnsupportedEncoding` when an unmapped CID font supplies the
  document's meaningful text; do not guess broadly.
- [ ] Add red operator tests for:
  `BT ET Tf Tm Td TD T* Tc Tw Tz TL Ts Tr Tj TJ ' " cm q Q`,
  marked content/`ActualText`, single/array page content, `Do` image/form
  XObjects, resource lookup, `BI/ID/EI` inline-image token boundaries, simple
  `/Widths`/`FirstChar`, CID `/W`/`DW`, and bounded graphics/form stacks.
- [ ] Add a vector/path-painting fixture. Consume and bounded-skip ordinary
  path/paint/clipping/color operands (`m l c v y h re S s f F f* B B* b b*
  n W W*` and related color operators) without rasterizing vector art, leaking
  operand state, or losing captions/text before or after it.
- [ ] Implement 16.16 text/graphics matrices and extraction-only glyph geometry.
  Never carry PDF font family or point size into semantic XHTML styles.
- [ ] Record bounded image placements and references in `PdfPageModel`; defer
  decoding/caching to Task 22. Form recursion and inline image data must share
  the same work/decompression limits and never obscure following text tokens.
- [ ] Prove `font_size_6.pdf` and `font_size_72.pdf` produce byte-identical
  semantic text and word inputs.
- [ ] Run `ctest -R "^Pdf(CMap|Content)"`. Expected: PASS.

### Task 16: Add hidden OCR qualification and reading-order reduction

**Files:**

- Create: `lib/PdfReflow/PdfHiddenText.h`
- Create: `lib/PdfReflow/PdfHiddenText.cpp`
- Create: `lib/PdfReflow/PdfReadingOrder.h`
- Create: `lib/PdfReflow/PdfReadingOrder.cpp`
- Create: `lib/PdfReflow/PdfRunStore.h`
- Create: `lib/PdfReflow/PdfRunStore.cpp`
- Create: `lib/PdfReflow/PdfDocumentTextClassifier.h`
- Create: `lib/PdfReflow/PdfDocumentTextClassifier.cpp`
- Test: `test/pdf_reflow_core/PdfHiddenTextTest.cpp`
- Test: `test/pdf_reflow_core/PdfReadingOrderTest.cpp`
- Test: `test/pdf_reflow_core/PdfDocumentTextClassifierTest.cpp`

- [ ] Add red hidden-text fixtures requiring render-mode-3 text to have a
  nonzero on-page transform and plausible image overlap. Reject off-page,
  zero-size, metadata-like, and unmappable hidden text.
- [ ] Deduplicate qualified hidden OCR against visible normalized text using
  content plus geometry. Fix all thresholds as named constants with positive
  and negative controls.
- [ ] Add red geometry tests for single-column lines, two/three columns,
  headings, paragraphs, row-major tables, repeated headers/footers, rotated
  noise, and ambiguous layouts.
- [ ] Implement conservative whitespace-histogram columns, line clustering,
  table rows/cells, paragraph grouping, and document-scoped repeated-band
  suppression. A three-page fixture must remove a repeated header/footer while
  retaining a one-off heading.
  Ambiguity must degrade to a deterministic readable stream, never fixed-page
  rendering.
- [ ] Define `PdfRunStore`, HAL fixed-record spill, and host fault-injection
  implementations. Store fixed geometry/order metadata with checked offset and
  length into a paired sequential variable-text file. Run a dense fixture that
  exceeds both 256 runs and the 8 KiB page-text workspace through a real disk
  spill, with no truncation. Required oracle: byte-identical semantic output
  versus the roomy in-memory path.
- [ ] Add a single-reader fake that fails on any second source-PDF handle.
  At a verified page boundary, close the source before opening/reading a spill
  reduction file; close the spill; then reopen the source once and seek to the
  checked continuation offset. Add a negative witness that intentionally reads
  spill while the source remains open and must fail.
- [ ] Sample early pages only for UI classification, but return
  `NoReadableText` only after full extraction confirms zero meaningful mapped
  text. `scan_only.pdf` must reach that result; unsupported encoding/filter and
  malformed input retain their distinct errors.
- [ ] Run `ctest -R "^Pdf"`. Expected: PASS, including
  `PdfDocumentTextClassifier`.

### Task 17: Count words once and emit semantic XHTML blocks

**Files:**

- Create: `lib/PdfReflow/PdfWordCounter.h`
- Create: `lib/PdfReflow/PdfWordCounter.cpp`
- Create: `lib/PdfReflow/PdfSemanticWriter.h`
- Create: `lib/PdfReflow/PdfSemanticWriter.cpp`
- Test: `test/pdf_reflow_core/PdfWordCounterTest.cpp`
- Test: `test/pdf_reflow_core/PdfSemanticWriterTest.cpp`

- [ ] Add red streaming tests for Latin/digits, internal apostrophes/hyphens,
  CJK character units, punctuation-only runs, malformed UTF-8, and every
  multibyte split.
- [ ] Count each extraction token once. At block completion emit checked stable
  32-bit `b%08lx` anchor, cumulative word start, and count. Test maximum value,
  rollover rejection, and collision detection.
- [ ] Emit minimal well-formed XHTML using semantic headings, paragraphs,
  row-major table fragments, internal `<a href>`, and
  `<span role="doc-pagebreak" aria-label="…">`; do not emit PDF font sizes,
  families, absolute coordinates, or page-sized containers.
- [ ] Escape all source text/attributes and validate deterministic publisher
  label truncation against `Page`'s fixed 16-byte field.
- [ ] Assert the 6 pt and 72 pt fixtures have identical transcript, XHTML,
  anchors, and total words. Add a separate positive control showing a device
  font-size change later alters paginator output.
- [ ] Run `ctest -R "^Pdf(Word|Semantic)"`. Expected: PASS.

## Phase IV — Durable Cache, Navigation, Progress, and Images

### Task 18: Implement source identity and two-slot resumable cache

**Files:**

- Create: `lib/PdfReflow/PdfCacheFormat.h`
- Create: `lib/PdfReflow/PdfSourceIdentity.h`
- Create: `lib/PdfReflow/PdfSourceIdentity.cpp`
- Create: `lib/PdfReflow/PdfCacheManifest.h`
- Create: `lib/PdfReflow/PdfCacheManifest.cpp`
- Create: `lib/PdfReflow/PdfBuildCheckpoint.h`
- Create: `lib/PdfReflow/PdfBuildCheckpoint.cpp`
- Create: `lib/PdfReflow/PdfCacheIo.h`
- Create: `lib/PdfReflow/PdfHalCacheIo.h`
- Create: `lib/PdfReflow/PdfHalCacheIo.cpp`
- Create: `lib/PdfReflow/PdfCacheStore.h`
- Create: `lib/PdfReflow/PdfCacheStore.cpp`
- Modify: `lib/hal/HalStorage.h`
- Modify: `lib/hal/HalStorage.cpp`
- Modify: `test/qemu/hal/src/HalStorage.h`
- Modify: `test/qemu/hal/src/HalStorage.cpp`
- Create: `test/pdf_cache_recovery/CMakeLists.txt`
- Create: `test/pdf_cache_recovery/PdfTestCacheIo.h`
- Create: `test/pdf_cache_recovery/PdfTestCacheIo.cpp`
- Test: `test/pdf_cache_recovery/PdfCacheRecoveryTest.cpp`
- Test: `test/pdf_cache_recovery/PdfCacheFaultInjectionTest.cpp`
- Modify: `test/CMakeLists.txt`

- [ ] Add red explicit-codec tests for magic/version/length/sequence/CRC,
  highest-valid slot, identity mismatch, sequence wrap, corrupt/truncated slots,
  and every torn-write prefix. Never serialize raw POD.
- [ ] Define a host-testable `PdfCacheIo` seam for
  open/read/write/flush/sync/close/remove/mkdir/list/capacity/file metadata.
  Implement HAL and fault-injection test adapters; do not hide storage calls
  behind host filesystem APIs in recovery tests.
- [ ] Use stable FNV64 path hashing matching `Epub::cachePathForFilePath`; never
  use `std::hash`.
- [ ] Lock source fingerprints to domain-separated FNV-1a 64-bit over at most
  the first and last 4,096 bytes plus their absolute offsets/lengths and source
  size. Use one source handle and at most two reads; a file ≤4,096 bytes is read
  once and feeds both domains. Test 0-byte, 1-byte, 4,096-byte, overlapping
  4,097–8,191-byte, and larger files.
- [ ] Add optional HAL results for FAT modification time and total/free
  capacity. Encode `known=false` when the production or emulated filesystem
  cannot supply a value; never invent one. Cache byte ceilings remain mandatory
  even when capacity is unknown.
- [ ] Encode manifest fields for format/capability version, sequence, completed
  state, warning flags, source identity, active generation, total words, and a
  bounded/streamed required-file table of relative path, size, and accumulated
  CRC.
- [ ] Encode checkpoint fields for sequence, verified source identity,
  generation, phase, last verified page/object boundary, emitted
  section/image state, cumulative words, output bytes, warning flags, and CRC.
- [ ] Implement inactive `gen_<sequence>` builds plus alternating
  `manifest.a/b` and `build.a/b`. Manifest is the only commit marker; never
  assume directory rename is power-loss atomic.
- [ ] Accumulate file sizes/CRCs while streaming. Finalization validates the
  recorded writer state and closed handles without rereading complete cache
  files; reopen only the new manifest slot to prove its commit record.
- [ ] Checkpoint only after at least eight completed PDF pages or 512 KiB of new
  output and at least five seconds, except forced cancellation/terminal state.
- [ ] Add fault injection at open, write, short write, flush, sync, close,
  validation, manifest commit, reopen, and cleanup. The older valid slot must
  survive every inactive-slot tear.
- [ ] Validate cleanup targets as local `gen_<digits>` entries unreferenced by
  either valid manifest. Reject traversal, symlink-like, root, and foreign
  paths.
- [ ] Enforce checked cache cap
  `min(max(4 MiB, 2 * sourceSize + 1 MiB), 64 MiB)`. When capacity is exposed,
  also preserve `max(16 MiB, 5% total)`. Omit optional images before required
  text/metadata.
- [ ] Run:

  ```powershell
  cmake --build build/test
  ctest --test-dir build/test --output-on-failure -R "^PdfCache"
  ```

  Expected: all recovery/fault tests PASS; no partial generation becomes
  committed.

### Task 19: Add the foreground preparation state machine and minimal PDF route

**Files:**

- Create: `lib/PdfReflow/PdfPreparation.h`
- Create: `lib/PdfReflow/PdfPreparation.cpp`
- Create: `lib/PdfReflow/PdfResourceTracker.h`
- Create: `lib/PdfReflow/PdfResourceTracker.cpp`
- Create: `src/activities/reader/PdfPrepareActivity.h`
- Create: `src/activities/reader/PdfPrepareActivity.cpp`
- Create: `src/activities/reader/ReaderRoute.h`
- Create: `src/activities/reader/ReaderRoute.cpp`
- Create: `lib/PdfReflow/PdfReflowDocument.h`
- Create: `lib/PdfReflow/PdfReflowDocument.cpp`
- Create: `test/pdf_extraction/CMakeLists.txt`
- Create: `test/pdf_product_integration/CMakeLists.txt`
- Modify: `lib/FsHelpers/FsHelpers.h`
- Modify: `lib/FsHelpers/FsHelpers.cpp`
- Modify: `src/activities/home/FileBrowserActivity.cpp`
- Modify: `src/activities/reader/ReaderActivity.h`
- Modify: `src/activities/reader/ReaderActivity.cpp`
- Modify: `lib/I18n/translations/english.yaml`
- Modify generated i18n outputs with: `python scripts/gen_i18n.py`
- Modify: `test/CMakeLists.txt`
- Test: `test/pdf_product_integration/PdfRoutingTest.cpp`
- Test: `test/pdf_extraction/PdfPreparationTest.cpp`

- [ ] Extract a pure `ReaderRoute` selector and add red tests for `.pdf`,
  `.PDF`, and mixed case. A route spy must prove PDFs never call `loadEpub()`.
- [ ] Add case-insensitive `hasPdfExtension` and browser visibility.
- [ ] Under a temporary compile-time `CROSSINK_ENABLE_PDF` gate, route a valid
  completed cache to `PdfReflowDocument` and missing/stale cache to
  `PdfPrepareActivity`; keep other routes unchanged. Enable the gate only for
  QEMU/simulator acceptance until Task 23 makes all product actions
  format-aware, then enable it for normal firmware and remove the temporary
  limitation.
- [ ] `PdfPreparation::step()` owns the one source `HalFile`, fixed workspaces,
  parser components, short-lived writers, resource tracker, and cancellation.
  End each `loop()` slice at 8 ms, 32 operations, or one 4 KiB I/O/decode chunk.
- [ ] Before parsing, gate both total free heap and largest contiguous block.
  Allocate the six Task 12 workspaces once with `makeUniqueNoThrow`, log/account
  each allocation in `PdfResourceTracker`, reuse them without per-step
  allocation, and release them in reverse order. Never fall back to
  `InflateReader::init(true)`.
- [ ] Use race-to-idle at normal CPU frequency only while work is queued. Create
  no task, timer poller, or background worker.
- [ ] Repaint the static preparation UI only after both 10 percentage points
  and 15 seconds, at most ten intermediate paints.
- [ ] `PdfReflowDocument::loadCompletedCache()` performs one bounded identity
  transaction, closes the source immediately, then opens/validates the selected
  manifest and required cache files before exposing loose sections/resources.
  Page turns must not parse or reopen the PDF. The single-reader fake covers
  this source-to-cache handoff.
- [ ] Until Task 21 supplies `PdfProgressStore`, the QEMU-only minimal document
  returns `false` from position load/save without surfacing a save-error toast;
  the minimal tracer does not exercise persistence. Normal firmware routing
  remains disabled until real semantic progress exists.
- [ ] Add translated outcomes for preparing, cancel/resume, no readable text,
  encrypted, unsupported encoding/filter, damaged/unsafe, insufficient memory
  or storage, and optional content skipped.
- [ ] Run `python scripts/gen_i18n.py` in this task so every new `tr(STR_*)`
  identifier exists before compiling the activity.
- [ ] Run the minimal born-digital fixture through preparation into one XHTML
  section, then through the shared reader in QEMU. Add a temporary terminal
  `QEMU_PDF_TRACER_PASS` only after rendered text is `Hello PDF`.
- [ ] Verify target size and runtime resources against the Task 5 baseline
  immediately after this tracer; stop if any final hard envelope is already
  exceeded. Remove any prior log, run the tracer with
  `--log .pio/build/qemu-esp32c3/qemu-pdf-tracer.log`, and pass that exact log
  to `check_qemu_resources.py verify`.

  ```powershell
  Remove-Item -LiteralPath .pio\build\qemu-esp32c3\qemu-pdf-tracer.log -ErrorAction SilentlyContinue
  .\.venv\Scripts\pio.exe run -e qemu-esp32c3 -t qemu-image
  python scripts/run_qemu_esp32c3.py `
    --expect QEMU_PDF_TRACER_PASS `
    --log .pio/build/qemu-esp32c3/qemu-pdf-tracer.log
  python scripts/check_qemu_resources.py verify `
    --baseline test/qemu/baselines/esp32c3-55.03.37-arduino-3.3.7.json `
    --manifest .pio/build/qemu-esp32c3/qemu_manifest.json `
    --elf .pio/build/qemu-esp32c3/firmware.elf `
    --runtime-log .pio/build/qemu-esp32c3/qemu-pdf-tracer.log
  ```
- [ ] Stop expansion until the QEMU minimal tracer is green.

### Task 20: Preserve outline chapters, contents/index links, and page labels

**Files:**

- Create: `lib/PdfReflow/PdfOutline.h`
- Create: `lib/PdfReflow/PdfOutline.cpp`
- Create: `lib/PdfReflow/PdfMetadataStore.h`
- Create: `lib/PdfReflow/PdfMetadataStore.cpp`
- Create: `test/pdf_navigation/CMakeLists.txt`
- Modify: `scripts/generate_pdf_reflow_fixtures.py`
- Modify generated: `test/pdf_reflow_core/fixtures/`
- Modify: `lib/PdfReflow/PdfSemanticWriter.cpp`
- Modify: `lib/PdfReflow/PdfReflowDocument.cpp`
- Modify: `src/activities/reader/EpubReaderChapterSelectionActivity.cpp`
- Modify: `test/CMakeLists.txt`
- Test: `test/pdf_navigation/PdfNavigationTest.cpp`

- [ ] Add fixtures for PDF `/Info`, XMP/catalog language, hierarchical
  outlines, named destinations, explicit
  destinations, same/cross-section links, contents/index text, nonnumeric page
  labels, no-outline headings, no-outline/no-heading fallback, malformed
  cycles, and unresolved external/actions.
- [ ] Populate metadata deterministically: title from XMP then `/Info` then
  filename; author from XMP then `/Info`; language from Catalog `/Lang` then
  XMP. Store bounded UTF-8 values in `metadata.bin`.
- [ ] Parse outline hierarchy with hard depth/count/cycle limits. Preserve
  title, level, and destination; map destinations to stable semantic anchors.
- [ ] Make navigation discovery an explicit preparation phase before final
  section emission. Outline destinations define semantic section boundaries
  but never force an original-PDF page break. If there is no usable outline,
  derive conservative entries from heading blocks; if there are no headings,
  emit one root entry from title/filename.
- [ ] Resolve named and explicit internal destinations. Ignore URI, launch,
  JavaScript, attachment, and external-file actions without executing/fetching.
- [ ] Preserve resolvable link annotations as internal XHTML anchors. Do not
  synthesize links from arbitrary printed numbers.
- [ ] Parse `/PageLabels` number trees with bounded recursion/ranges and emit
  publisher-page markers without forcing reader page breaks. Keep
  contents and index text in the semantic stream even when some printed
  references cannot resolve.
- [ ] Record bounded early-content cover candidate references in build state,
  but do not mutate `metadata.bin` or an active generation after manifest
  commit. Task 22 performs final image classification and cover creation inside
  the inactive generation before its file table/CRC is finalized.
- [ ] Run chapter selector/link/page-label host tests, then QEMU scripted
  navigation. Expected: correct hierarchical titles and anchor landings before
  `QEMU_PDF_NAV_PASS`.
- [ ] Run `python scripts/generate_pdf_reflow_fixtures.py --check`; all new
  navigation expectations and hashes must be deterministic.

### Task 21: Add word-ordinal progress, relayout resume, bookmarks, and clippings

**Files:**

- Create: `lib/PdfReflow/PdfProgressStore.h`
- Create: `lib/PdfReflow/PdfProgressStore.cpp`
- Create: `lib/PdfReflow/PdfLayoutWordIndex.h`
- Create: `lib/PdfReflow/PdfLayoutWordIndex.cpp`
- Create: `test/pdf_word_progress/CMakeLists.txt`
- Create: `test/pdf_saved_items/CMakeLists.txt`
- Modify: `lib/Epub/Epub/parsers/ChapterHtmlSlimParser.h`
- Modify: `lib/Epub/Epub/parsers/ChapterHtmlSlimParser.cpp`
- Modify: `lib/Epub/Epub/Section.h`
- Modify: `lib/Epub/Epub/Section.cpp`
- Modify: `src/activities/reader/EpubReaderActivity.cpp`
- Modify: `src/BookmarkStore.h`
- Modify: `src/BookmarkStore.cpp`
- Modify: `src/ClippingStore.h`
- Modify: `src/ClippingStore.cpp`
- Modify: `src/activities/ActivityResult.h`
- Modify: `src/activities/reader/EpubReaderBookmarkListActivity.cpp`
- Modify: `src/activities/reader/EpubReaderClippingListActivity.cpp`
- Modify: `src/activities/home/SavedItemsHomeActivity.cpp`
- Modify: `src/activities/home/BookmarksHomeActivity.cpp`
- Modify: `src/CrossPointState.h`
- Modify: `src/JsonSettingsIO.cpp`
- Modify: `test/CMakeLists.txt`
- Test: `test/pdf_word_progress/PdfWordProgressTest.cpp`
- Test: `test/pdf_saved_items/PdfSavedItemsTest.cpp`

- [ ] Add red progress tests with exact known block ordinals and expected
  `lastReachedWord / totalWords`; punctuation-only text must not change totals.
- [ ] Extend page-completion callbacks with a small semantic-position record and
  write one PDF-only fixed record per generated reader page: first/last global
  ordinal, anchor ID, and in-block offset. Do not alter shared `Page` or section
  v44.
- [ ] Make that callback optional and allocation-free when disabled. Assert
  EPUB produces no PDF sidecar, its section-v44 bytes are unchanged, and both
  cached/uncached EPUB hashes remain locked.
- [ ] Persist root-level PDF progress as anchor, in-block word offset, global
  ordinal, and fallback section/page tuple. Keep existing debounced writes; do
  not write on every page turn.
- [ ] On relayout, resolve anchor+offset and select the page whose sidecar range
  contains the ordinal. Device font/orientation changes must alter page hashes
  while total words and semantic position stay unchanged.
- [ ] Calculate displayed progress from the last reached ordinal read directly
  from the sidecar, never by rescanning XHTML. Clamp 100% only at actual
  document end. Define/test start, middle, final word, empty/error document,
  zero-word block, and out-of-range/corrupt sidecar behavior.
- [ ] Add `"pdf"` to Bookmark/Clipping allowlists. Keep existing EPUB v5/v1
  writers byte-identical; write v6/v2 only for PDF semantic records, and make
  readers accept both old/new records with safe defaults.
- [ ] Advertise `SavedItems` from `PdfReflowDocument` only after those PDF
  readers/writers and jump tests are green; before that point the Task 11
  clearing path remains active.
- [ ] Extend ActivityResult and saved-items pending state with absent-field-safe
  semantic anchor/ordinal fields. Test launch from bookmark and clipping after
  reboot and relayout.
- [ ] Run exact progress, legacy-format, saved-item, and two-pass EPUB tests.
  Expected: PDF ratios/resume PASS and existing EPUB bytes/behavior unchanged.

### Task 22: Retain bounded JPEG and Flate images

**Files:**

- Create: `lib/PixelCache/PixelCache.h`
- Create: `lib/PixelCache/PixelCache.cpp`
- Create: `lib/PdfReflow/PdfImageExtractor.h`
- Create: `lib/PdfReflow/PdfImageExtractor.cpp`
- Create: `lib/PdfReflow/PdfPixelCacheWriter.h`
- Create: `lib/PdfReflow/PdfPixelCacheWriter.cpp`
- Create: `test/pdf_pixel_cache/CMakeLists.txt`
- Modify: `scripts/generate_pdf_reflow_fixtures.py`
- Modify generated: `test/pdf_reflow_core/fixtures/`
- Modify: `lib/Epub/Epub/blocks/ImageBlock.cpp`
- Modify: `lib/Epub/Epub/parsers/ChapterHtmlSlimParser.cpp`
- Modify: `lib/PdfReflow/PdfSemanticWriter.cpp`
- Modify: `test/CMakeLists.txt`
- Test: `test/pdf_pixel_cache/PdfPixelCacheTest.cpp`
- Test: `test/pdf_pixel_cache/PdfImageExtractorTest.cpp`

- [ ] Factor existing `.pxc` header/read logic into `PixelCache` without
  changing the legacy two-byte width, two-byte height, or packed 2-bit row
  format. Prove existing EPUB `.pxc` fixtures remain byte-identical.
- [ ] Add red JPEG tests for stream copy, content-hash dedupe, placement near
  related text, manifest CRC, and one-copy cache behavior.
- [ ] Add red Flate raster tests for Gray 1/2/4/8 bpc, RGB8, Indexed Gray/RGB
  with at most 256 entries, TIFF predictor 2, PNG predictors 10–15, inversion
  decode arrays, 1-bit masks, same-size Gray8 soft masks, inline images,
  dimensions/row hashes, 16 MP image cap, 8 KiB source-row cap, output cap,
  short writes, and forced OOM.
- [ ] Stream JPEG once to content-hash `.jpg`. Convert supported Flate rasters
  row-by-row directly to `.pxc` with at most 4 KiB image workspace; stream
  downscale to useful reader dimensions and never hold a full source image
  alongside the framebuffer. JPEG bytes must remain byte-identical.
- [ ] For an 8 KiB decoded source-row cap, phase-reuse the existing 8 KiB page
  text workspace as the prior-row predictor buffer while image decoding is
  active; use the 4 KiB decoder output for current-row chunks. Account the
  overlap in `PdfResourceTracker` and prove peak live heap does not increase.
- [ ] Classify meaningful images by bounded dimensions, repeated/background
  coverage, placement, and nearby semantic content. Suppress page backgrounds,
  rules, tiny decorations, and repeated logos; retain figures/covers near the
  corresponding block. Make `PdfSemanticWriter` emit the retained image at that
  block anchor.
- [ ] Before manifest finalization, choose the first retained meaningful
  early-content image as cover; otherwise generate a title/author cover with
  device typography. Write cover/thumbnail and final `metadata.bin` into the
  inactive generation, accumulate their size/CRC in the file table, close them,
  and only then commit the manifest. Never modify an active generation.
- [ ] Make `ReflowSectionSource::resolveResource` return `PixelCache` and
  bypass decoder probing for a completed PDF `.pxc`; preserve the EPUB
  extraction/decoder path.
- [ ] Hash during the original stream and omit unsupported/over-budget optional
  JPX/JBIG2/CCITT images with a warning while surrounding text survives.
  Unsupported required text remains a hard failure.
- [ ] Run image tests, existing EPUB image tests/oracle, and QEMU JPEG/Flate
  fixtures. Expected: bounded allocations and deterministic image/frame hashes.
- [ ] Run `python scripts/generate_pdf_reflow_fixtures.py --check`; all image
  expectations and hashes must be deterministic.

### Task 23: Complete home, recents, cache, move/delete, sleep, and upload flows

**Files:**

- Modify: `src/RecentBooksStore.cpp`
- Modify: `src/activities/home/RecentBookProgress.h`
- Modify: `src/activities/home/RecentBookProgress.cpp`
- Modify: `src/activities/home/HomeActivity.cpp`
- Modify: `src/activities/home/RecentBooksGridActivity.cpp`
- Modify: `src/components/themes/dashboard/DashboardTheme.cpp`
- Modify: `src/components/themes/minimal/MinimalTheme.cpp`
- Modify: `src/activities/boot_sleep/SleepCoverAssets.cpp`
- Modify: `src/activities/boot_sleep/SleepActivity.cpp`
- Modify: `src/activities/home/BookActions.cpp`
- Modify: `src/activities/settings/ClearCacheActivity.h`
- Modify: `src/activities/settings/ClearCacheActivity.cpp`
- Modify: `src/util/BookCacheUtils.h`
- Modify: `src/util/BookCacheUtils.cpp`
- Modify: `src/util/BookMoveUtils.h`
- Modify: `src/util/BookMoveUtils.cpp`
- Create: `src/util/BookStateMigrationJournal.h`
- Create: `src/util/BookStateMigrationJournal.cpp`
- Modify: `src/util/NextBookFinder.cpp`
- Modify: `src/components/UITheme.cpp`
- Modify: `src/util/ScreenshotInfo.h`
- Modify: `src/network/CrossPointWebServer.cpp`
- Modify: `web/pages/files.js`
- Modify: `platformio.ini`
- Modify: `test/pdf_product_integration/CMakeLists.txt`
- Modify: `test/CMakeLists.txt`
- Test: `test/pdf_product_integration/PdfProductIntegrationTest.cpp`

- [ ] Add red tests proving Home/recents read completed metadata only and never
  start extraction; absent cache falls back to filename/placeholder.
- [ ] Add cached-only word progress, cover, current chapter, sleep overlay, next
  book, stats, completed state, move-to-Read, and screenshot metadata.
- [ ] Hide EPUB render-mode and external-sync actions for PDF; device typography
  is always used.
- [ ] Recognize `pdf_` caches. “Clear reading cache” removes generations/layout
  but preserves root progress/settings/stats/bookmarks/clippings; deleting the
  book removes all owned metadata.
- [ ] Generalize moved-book state migration with a two-slot CRC journal. Before
  renaming, persist old/new paths, old/new hashes, and phase. After the source
  move, copy/rekey cache/progress/recents/bookmarks/clippings, verify new state,
  atomically advance the journal, then remove old state and journal. On boot or
  next access, a journal whose source is at the new path exposes the old-hash
  state as fallback and retries migration; a pre-rename failure clears safely.
  Inject failure at journal write/sync, source rename, every state copy/verify,
  activation, old-state cleanup, and journal cleanup.
- [ ] Test browser upload of a PDF byte-for-byte. It must never enter EPUB
  optimization/conversion. Test overwrite invalidation and Web rename/move
  state preservation.
- [ ] Keep KOReader sync in `src/main.cpp` EPUB-only. Do not reload PDF as EPUB.
- [ ] Audit every reader-exit/action path before enabling normal firmware:
  render-mode switching, sync, cache clearing, move-to-Read, completed state,
  sleep snapshot, stats, and `APP_STATE.openEpubPath`. Add a generic
  open-book-path accessor over the legacy-named persisted field and prove no
  consumer assumes EPUB from the stored extension.
- [ ] After those tests are green, enable `CROSSINK_ENABLE_PDF` in normal
  default/tiny/xlarge environments. QEMU/simulator-only routing must not be the
  final implementation.
- [ ] Run product integration tests plus cached/uncached EPUB simulator oracle.

## Phase V — Simulator, QEMU, Fault, and Release Acceptance

### Task 24: Add a reproducible native-simulator UI route

**Files:**

- Create: `scripts/run_pdf_simulator_acceptance.py`
- Create: `src/simulator/PdfSimulatorAcceptance.h`
- Create: `src/simulator/PdfSimulatorAcceptance.cpp`
- Modify: `docker/pdf-simulator/Dockerfile`
- Modify: `scripts/run_pdf_simulator_container.py`
- Modify: `scripts/run_simulator_smoke_test.py`
- Modify: `src/simulator/SimulatorSmokeTest.cpp`
- Modify: `.github/workflows/ci.yml`

- [ ] Re-run the Task 6 container self-test and keep its base/packages pinned.
- [ ] Build the native simulator in the container and run headlessly with a
  deterministic virtual display:

  ```powershell
  python scripts/run_pdf_simulator_container.py --build
  python scripts/run_pdf_simulator_acceptance.py --container --headless
  ```

- [ ] Script visible flows for PDF browser routing, preparation, cancellation,
  resume, warnings/errors, first/middle/last pages, typography/margins,
  orientation, chapters, internal links, publisher labels, bookmarks,
  clippings, progress, reboot resume, and cached reopen.
- [ ] Include 6 pt and 72 pt PDFs at identical device settings. Required:
  identical page text, count, and framebuffer hashes. Change the device font
  size as a positive control; hashes/page layout must change.
- [ ] Assert no screen contains a full fixed-page canvas or miniature PDF page.
- [ ] Preseed deterministic completed `.pxc` cache fixtures for native image
  placement/frame hashes; native JPEG/PNG decoding is not a prerequisite for
  this UI oracle.
- [ ] Assert one bounded identity transaction at cache open (one handle, at
  most two 4 KiB reads) and zero source-PDF opens/parser calls during 100 cached
  page turns.
- [ ] Replay cached and uncached reference EPUB in the same runner.
- [ ] Add an Ubuntu CI simulator acceptance job only after the local/container
  run is green. Compile-only is not acceptance.

### Task 25: Expand the ESP32-C3 QEMU acceptance replay

**Files:**

- Modify: `src/qemu/QemuAcceptance.cpp`
- Modify: `scripts/run_qemu_esp32c3.py`
- Create: `test/qemu/data/books/` through the fixture-image build
- Modify: `scripts/qemu_build.py`
- Modify: `.github/workflows/ci.yml`

- [ ] Add a deterministic scripted state machine which boots the complete app
  and:

  1. opens a born-digital PDF and completes cache build;
  2. renders deterministic first/middle/last pages;
  3. opens chapters, contents, index, named destinations, and page labels;
  4. saves progress, reboots, reopens from cache, and resumes;
  5. changes device font/orientation and relayouts without recounting;
  6. accepts qualified OCR hidden text;
  7. handles columns, table, JPEG, and Flate raster;
  8. rejects scan-only, encrypted, malformed, expansion-bomb, and forced-OOM
     fixtures without panic;
  9. performs 100 cached page turns;
  10. replays one uncached and one cached EPUB.

- [ ] Arm the deliberate reboot with `QEMU_EXPECT_RESET seq=N` and require the
  next `QEMU_BOOT seq=N+1`; reject any unarmed/repeated reset and aggregate
  worst runtime resource values across boots.

- [ ] Instrument source opens, parser entry calls, PDF-owned allocations,
  minimum free heap, minimum largest block, stack high-water margin, work-slice
  duration/ops/bytes, UI paints, checkpoints, and power-saving transition.
- [ ] Required page-turn oracle: one bounded identity transaction before
  reading (one source open and at most two 4 KiB reads); then zero PDF source
  opens and zero parser calls for all 100 turns, with no heap loss or stack
  erosion.
- [ ] Emit `QEMU_TEST_FAIL <stable-code>` immediately on any oracle failure.
  Emit `QEMU_TEST_PASS` only after every step and final idle transition.
- [ ] Before building, remove any prior acceptance log. Enforce fixture bytes
  plus writable headroom below `0x360000`; use injected quota for ENOSPC rather
  than filling the normal image. Build/run:

  ```powershell
  Remove-Item -LiteralPath .pio\build\qemu-esp32c3\pdf-acceptance.log -ErrorAction SilentlyContinue
  .\.venv\Scripts\pio.exe run -e qemu-esp32c3 -t qemu-image
  python scripts/run_qemu_esp32c3.py `
    --expect QEMU_TEST_PASS `
    --log .pio/build/qemu-esp32c3/pdf-acceptance.log
  python scripts/check_qemu_resources.py verify `
    --baseline test/qemu/baselines/esp32c3-55.03.37-arduino-3.3.7.json `
    --manifest .pio/build/qemu-esp32c3/qemu_manifest.json `
    --elf .pio/build/qemu-esp32c3/firmware.elf `
    --runtime-log .pio/build/qemu-esp32c3/pdf-acceptance.log
  ```

  Expected: `QEMU_TEST_PASS` and `QEMU_RESOURCE_PASS`.

- [ ] Re-run `verify_qemu_no_flash.py`. Expected: all unsafe targets refuse
  before any device I/O and the verifier exits zero.
- [ ] Upgrade the CI QEMU job from tracer-only to this full
  `QEMU_TEST_PASS` replay plus resource verification.

### Task 26: Run adversarial, power-loss, ENOSPC, and energy-efficiency gates

**Files:**

- Create: `test/pdf_extraction/PdfAdversarialTest.cpp`
- Modify: `test/pdf_extraction/CMakeLists.txt`
- Modify: `test/CMakeLists.txt`
- Modify: `test/pdf_cache_recovery/PdfCacheFaultInjectionTest.cpp`
- Modify: `src/qemu/QemuAcceptance.cpp`

- [ ] Test every truncation point of the smallest valid fixture and
  deterministic fixed-seed byte mutations. Assert stable error class, checked
  seeks, no panic, and no source-sized allocation.
- [ ] Assert all approved production defaults, then use small test-only limits
  for positive controls: a fixture passes at `N` and fails at `N-1`. Cover
  objects, pages, operators/page and /document, forms, expanded bytes, filters,
  trailer/page-tree depth, CMap ranges, image dimensions, and cache bytes
  without manufacturing enormous fixtures.
- [ ] Inject allocation failure at every PDF-owned allocation. Required:
  translated recoverable error, handles closed in reverse ownership order, no
  abort/restart, and no committed partial generation.
- [ ] Inject ENOSPC/short write before and after checkpoints/manifest commit.
  Optional images stop first; text/metadata either commit completely or the
  older manifest remains active.
- [ ] Exhaustively enumerate manifest/checkpoint truncation bytes in host tests.
  In QEMU, reboot at the bounded commit transitions: before/after checkpoint,
  manifest write, sync/close, validation, slot activation, and old-generation
  cleanup. Matching builds resume; stale identity rebuilds.
- [ ] In QEMU assert every step respects 8 ms/32 ops/4 KiB, no new task exists,
  preparation paints at most ten times, routine checkpoints obey debounce, and
  the existing 3000 ms idle/power-saving path is reached after extraction.
- [ ] Run all host and QEMU fault gates. Stop on the first failed externally
  visible oracle; revert or isolate the hypothesis before another production
  change.

### Task 27: Documentation, translations, changelog, and final matrix

**Files:**

- Modify: `lib/I18n/translations/english.yaml`
- Modify generated i18n outputs via: `python scripts/gen_i18n.py`
- Modify: `README.md`
- Modify: `USER_GUIDE.md`
- Modify: `SCOPE.md`
- Modify: `CHANGELOG.md`
- Modify: `docs/index.md`
- Modify: `docs/reader-features.md`
- Modify: `docs/data-cache.md`
- Modify: `docs/file-formats.md`
- Modify: `docs/simulator.md`
- Create: `docs/pdf-support.md`
- Modify: `docs/qemu.md`

- [ ] Document supported born-digital/OCR-layer boundary, excluded encryption
  and scan-only OCR, on-device preparation, semantic reflow, device typography,
  image limits, navigation, word progress, cache storage, clear-cache behavior,
  and user-visible error recovery.
- [ ] Document every binary cache/store version, magic, endianness, CRC,
  slot-selection rule, generation layout, progress/sidecar records, and
  backward-compatibility behavior.
- [ ] Add clear public copy. Do not expose internal QA/process language in
  user-facing screens/docs.
- [ ] Add a human-readable changelog entry and update scope/supported formats.
- [ ] Generate translations:

  ```powershell
  python scripts/gen_i18n.py
  ```

- [ ] Format only touched/new C/C++ files with clang-format 21, then run
  `git diff --check`; do not format unrelated tracked files:

  ```powershell
  $changedCpp = @(
    git diff --name-only --diff-filter=ACMR -- '*.c' '*.cpp' '*.h' '*.hpp'
    git ls-files --others --exclude-standard -- '*.c' '*.cpp' '*.h' '*.hpp'
  ) | Sort-Object -Unique
  if ($changedCpp.Count -gt 0) {
    python scripts/run_pdf_simulator_container.py -- clang-format-21 -i -- $changedCpp
  }
  git diff --check
  ```

- [ ] Run the full non-hardware matrix:

  ```powershell
  python scripts/generate_pdf_reflow_fixtures.py --check
  cmake -S test -B build/test -G Ninja -DCMAKE_BUILD_TYPE=Release
  cmake --build build/test
  ctest --test-dir build/test --output-on-failure -j
  python -m unittest discover -s test/qemu -p "test_*.py" -v
  python scripts/run_pdf_simulator_acceptance.py --container --headless
  Remove-Item -LiteralPath .pio\build\qemu-esp32c3\pdf-acceptance.log -ErrorAction SilentlyContinue
  .\.venv\Scripts\pio.exe run -e qemu-esp32c3 -t qemu-image
  python scripts/run_qemu_esp32c3.py `
    --expect QEMU_TEST_PASS `
    --log .pio/build/qemu-esp32c3/pdf-acceptance.log
  python scripts/check_qemu_resources.py verify `
    --baseline test/qemu/baselines/esp32c3-55.03.37-arduino-3.3.7.json `
    --manifest .pio/build/qemu-esp32c3/qemu_manifest.json `
    --elf .pio/build/qemu-esp32c3/firmware.elf `
    --runtime-log .pio/build/qemu-esp32c3/pdf-acceptance.log
  .\.venv\Scripts\pio.exe run -e default
  .\.venv\Scripts\pio.exe run -e tiny
  .\.venv\Scripts\pio.exe run -e xlarge
  .\.venv\Scripts\pio.exe check -e default --fail-on-defect low --fail-on-defect medium --fail-on-defect high
  python scripts/verify_qemu_no_flash.py --pio .venv/Scripts/pio.exe
  ```

  Expected: every matrix command exits zero. The no-flash verifier internally
  requires all six PlatformIO targets to fail with the exact refusal message
  and no device I/O.

- [ ] Compare size/runtime evidence to the pre-PDF baseline:

  - `.text + .rodata` growth ≤ 256 KiB;
  - static DRAM/BSS growth ≤ 12 KiB;
  - PDF-owned live heap ≤ 80 KiB;
  - free heap ≥ 64 KiB with framebuffer present;
  - largest block ≥ 48 KiB;
  - no PDF allocation > 32 KiB;
  - stack margin ≥ 1 KiB;
  - no persistent task or idle poller.

- [ ] Inspect final status/diff and verify there are no accidental binaries,
  secrets, physical-flash commands, debug-only bypasses, generated junk, or
  unrelated user-file edits:

  ```powershell
  git status --short
  git diff --check
  git diff --stat
  python scripts/verify_qemu_no_flash.py --pio .venv/Scripts/pio.exe
  ```

- [ ] Prepare a completion report that distinguishes host, native simulator,
  ESP32-C3 QEMU, build/static-analysis, and deliberately unperformed physical
  hardware evidence. Do not stage or commit.

## Final Acceptance Checklist

- [ ] Directly copied supported PDF opens after entirely on-device extraction.
- [ ] Reading uses device font, size, margins, spacing, orientation, and
  hyphenation; no fixed full-page PDF rendering exists.
- [ ] 6 pt and 72 pt source PDFs render identically at equal device settings.
- [ ] Multi-column text and tables read in deterministic semantic order.
- [ ] Meaningful JPEG/Flate images are retained within memory/storage limits.
- [ ] Outline hierarchy, contents/index text, internal destinations, and page
  labels survive reflow.
- [ ] Progress is reached words / total extracted words counted once.
- [ ] Resume/bookmarks/clippings survive relayout, reboot, cache rebuild, and
  moved paths.
- [ ] Cached page turns do no PDF parsing and no source-PDF I/O after the one
  bounded identity transaction (one open, at most two 4 KiB reads).
- [ ] Scan-only/encrypted/unsupported/damaged/resource failures are clear,
  recoverable, translated, and never commit partial output.
- [ ] Existing cached and uncached EPUB behavior remains byte/visually
  equivalent.
- [ ] Native simulator UI acceptance and complete ESP32-C3 QEMU replay pass.
- [ ] Resource, fault, watchdog, race-to-idle, and no-flash gates pass.
- [ ] No physical hardware was flashed or probed.
