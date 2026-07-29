import argparse
import json
from pathlib import Path
import queue
import re
import shutil
import subprocess
import sys
import tempfile
import threading
import time
from typing import TextIO


FAIL_MARKER = re.compile(r"^QEMU_[A-Z0-9_]*FAIL(?:\s|$)")
BOOT_MARKER = re.compile(r"^QEMU_BOOT seq=(\d+)$")
EXPECT_RESET_MARKER = re.compile(r"^QEMU_EXPECT_RESET seq=(\d+)$")
MAX_EXPECTED_RESETS = 8
OUTPUT_FAILURES = (
    (re.compile(r"\bpanic\b", re.IGNORECASE), "panic"),
    (re.compile(r"Guru Meditation", re.IGNORECASE), "Guru Meditation"),
    (re.compile(r"\babort(?:\(\))?\b", re.IGNORECASE), "abort"),
    (re.compile(r"\bwatchdog\b", re.IGNORECASE), "watchdog"),
)
TRACER_EXPECTED_MARKER = "QEMU_TRACER_PASS"
TRACER_MARKERS = (
    re.compile(r"^QEMU_STORAGE_PASS path=/qemu/sentinel\.txt bytes=26$"),
    re.compile(r"^QEMU_FRAME_PASS bytes=48000 crc32=0F7C8C45$"),
    re.compile(r"^QEMU_INPUT_PASS button=DOWN press=1 release=1$"),
    re.compile(r"^QEMU_POWER_PASS idle_ms=3000 saving=1$"),
    re.compile(
        r"^QEMU_RUNTIME heap_start=\d+ min_free=\d+ "
        r"min_max_alloc=\d+ max_alloc=\d+ stack_margin=\d+$"
    ),
)


class OutputGuard:
    def __init__(self, expected_marker: str) -> None:
        self.expected_marker = expected_marker
        self.last_boot: int | None = None
        self.pending_reset: int | None = None
        self.reset_count = 0
        self.tracer_marker_index = 0

    def inspect(self, line: str) -> tuple[str | None, bool]:
        if FAIL_MARKER.match(line):
            return f"failure marker observed: {line}", False
        for pattern, label in OUTPUT_FAILURES:
            if pattern.search(line):
                return f"{label} observed in QEMU output", False

        boot_match = BOOT_MARKER.match(line)
        if boot_match:
            error = self._accept_boot(int(boot_match.group(1)))
            if error:
                return error, False
            self.tracer_marker_index = 0

        reset_match = EXPECT_RESET_MARKER.match(line)
        if reset_match:
            error = self._arm_reset(int(reset_match.group(1)))
            if error:
                return error, False

        tracer_error = self._accept_tracer_marker(line)
        if tracer_error:
            return tracer_error, False

        if line == self.expected_marker:
            if self.pending_reset is not None:
                return "terminal marker arrived before armed reset", False
            if (
                self.expected_marker == TRACER_EXPECTED_MARKER
                and self.tracer_marker_index != len(TRACER_MARKERS)
            ):
                return (
                    "missing required tracer marker before terminal marker "
                    f"(next index {self.tracer_marker_index})"
                ), False
            return None, True
        return None, False

    def _accept_tracer_marker(self, line: str) -> str | None:
        if self.expected_marker != TRACER_EXPECTED_MARKER:
            return None

        matching_index = next(
            (
                index
                for index, pattern in enumerate(TRACER_MARKERS)
                if pattern.fullmatch(line)
            ),
            None,
        )
        if matching_index is None:
            return None
        if self.last_boot is None:
            return "tracer marker arrived before initial QEMU boot"
        if matching_index != self.tracer_marker_index:
            return (
                "out-of-order tracer marker "
                f"(expected index {self.tracer_marker_index}, "
                f"received {matching_index})"
            )
        self.tracer_marker_index += 1
        return None

    def _accept_boot(self, sequence: int) -> str | None:
        if self.last_boot is None:
            if sequence != 0:
                return f"first QEMU boot has invalid sequence {sequence}"
            self.last_boot = sequence
            return None

        if self.pending_reset is None:
            if sequence == self.last_boot:
                return f"restart loop detected at boot sequence {sequence}"
            return f"unarmed reset detected at boot sequence {sequence}"

        expected_boot = self.pending_reset + 1
        if sequence != expected_boot:
            return (
                f"armed reset expected boot sequence {expected_boot}, "
                f"received {sequence}"
            )
        self.last_boot = sequence
        self.pending_reset = None
        return None

    def _arm_reset(self, sequence: int) -> str | None:
        if self.last_boot is None:
            return "reset armed before initial QEMU boot"
        if self.pending_reset is not None:
            return "repeated reset arm without an intervening boot"
        if sequence != self.last_boot:
            return (
                f"reset arm sequence {sequence} does not match "
                f"boot sequence {self.last_boot}"
            )
        if self.reset_count >= MAX_EXPECTED_RESETS:
            return "expected reset limit exceeded"
        self.pending_reset = sequence
        self.reset_count += 1
        return None


def _pump_output(stream: TextIO, output_queue: queue.Queue[str | None]) -> None:
    try:
        for line in stream:
            output_queue.put(line)
    finally:
        output_queue.put(None)


def _terminate(process: subprocess.Popen[str]) -> None:
    if process.poll() is not None:
        return
    process.terminate()
    try:
        process.wait(timeout=1.0)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait(timeout=1.0)


def _command(qemu: Path) -> list[str]:
    if qemu.suffix.lower() == ".py":
        return [sys.executable, str(qemu)]
    return [str(qemu)]


def _monitor(command: list[str], arguments: argparse.Namespace) -> int:
    if arguments.timeout <= 0:
        sys.stderr.write("QEMU timeout must be greater than zero\n")
        return 1

    arguments.log.parent.mkdir(parents=True, exist_ok=True)
    try:
        process = subprocess.Popen(
            command,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            bufsize=1,
        )
    except OSError as error:
        sys.stderr.write(f"cannot start QEMU: {error}\n")
        return 1

    assert process.stdout is not None
    output_queue: queue.Queue[str | None] = queue.Queue()
    reader = threading.Thread(
        target=_pump_output,
        args=(process.stdout, output_queue),
        daemon=True,
    )
    reader.start()

    guard = OutputGuard(arguments.expect)
    deadline = time.monotonic() + arguments.timeout
    end_of_output = False
    failure: str | None = None
    passed = False

    try:
        with arguments.log.open("w", encoding="utf-8", newline="\n") as log:
            while True:
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    failure = f"QEMU timed out after {arguments.timeout:g} seconds"
                    break
                try:
                    item = output_queue.get(timeout=min(0.05, remaining))
                except queue.Empty:
                    item = ""

                if item is None:
                    end_of_output = True
                elif item:
                    line = item.rstrip("\r\n")
                    error, terminal = guard.inspect(line)
                    log.write(item)
                    log.flush()
                    if not (error and line == arguments.expect):
                        sys.stdout.write(item)
                        sys.stdout.flush()
                    if error:
                        failure = error
                        break
                    if terminal:
                        passed = True
                        break

                exit_code = process.poll()
                if exit_code is not None and end_of_output:
                    if exit_code == 0:
                        failure = "QEMU exited with missing terminal marker"
                    else:
                        failure = f"QEMU unexpected exit with code {exit_code}"
                    break
    except OSError as error:
        failure = f"cannot write QEMU log: {error}"
    finally:
        _terminate(process)
        process.stdout.close()
        reader.join(timeout=1.0)

    if passed:
        return 0
    sys.stderr.write((failure or "QEMU runner failed") + "\n")
    return 1


def _load_json(path: Path) -> dict:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise RuntimeError(f"cannot read {path}: {error}") from error
    if not isinstance(value, dict):
        raise RuntimeError(f"{path} must contain a JSON object")
    return value


def _default_install_receipt() -> Path:
    return (
        Path(__file__).resolve().parents[1]
        / ".tools"
        / "qemu-esp32c3"
        / "install.json"
    )


def _default_manifest() -> Path:
    return (
        Path(__file__).resolve().parents[1]
        / ".pio"
        / "build"
        / "qemu-esp32c3"
        / "qemu_manifest.json"
    )


def _installed_qemu(receipt_path: Path) -> Path:
    receipt = _load_json(receipt_path)
    raw_executable = receipt.get("executable")
    if not isinstance(raw_executable, str):
        raise RuntimeError("QEMU install receipt has no executable")
    executable = Path(raw_executable)
    if not executable.is_absolute():
        raise RuntimeError("QEMU install receipt executable must be absolute")
    if not executable.is_file():
        raise RuntimeError(f"QEMU executable does not exist: {executable}")
    return executable


def _manifest_images(manifest_path: Path) -> tuple[Path, Path]:
    manifest = _load_json(manifest_path)
    images = manifest.get("images")
    if not isinstance(images, dict):
        raise RuntimeError("QEMU manifest has no images object")
    resolved: list[Path] = []
    for name in ("flash", "efuse"):
        entry = images.get(name)
        raw_path = entry.get("path") if isinstance(entry, dict) else None
        if not isinstance(raw_path, str):
            raise RuntimeError(f"QEMU manifest has no {name} image path")
        path = Path(raw_path)
        if not path.is_absolute():
            raise RuntimeError(f"QEMU {name} image path must be absolute")
        if not path.is_file():
            raise RuntimeError(f"QEMU {name} image does not exist: {path}")
        resolved.append(path)
    return resolved[0], resolved[1]


def _qemu_machine_command(
    executable: Path, flash_image: Path, efuse_image: Path
) -> list[str]:
    return _command(executable) + [
        "-M",
        "esp32c3",
        "-icount",
        "shift=3,sleep=off",
        "-drive",
        f"file={flash_image},if=mtd,format=raw",
        "-drive",
        f"file={efuse_image},if=none,format=raw,id=efuse",
        "-global",
        "driver=nvram.esp32c3.efuse,property=drive,value=efuse",
        "-nic",
        "none",
        "-nographic",
        "-serial",
        "mon:stdio",
    ]


def _run(arguments: argparse.Namespace) -> int:
    if arguments.qemu is not None and arguments.manifest is None:
        if not arguments.qemu.is_file():
            sys.stderr.write(
                f"QEMU executable does not exist: {arguments.qemu}\n"
            )
            return 1
        return _monitor(_command(arguments.qemu), arguments)

    try:
        executable = (
            arguments.qemu
            if arguments.qemu is not None
            else _installed_qemu(
                arguments.install or _default_install_receipt()
            )
        )
        if not executable.is_file():
            raise RuntimeError(
                f"QEMU executable does not exist: {executable}"
            )
        flash_source, efuse_source = _manifest_images(
            arguments.manifest or _default_manifest()
        )
        with tempfile.TemporaryDirectory(
            prefix="crossink-qemu-run-"
        ) as temporary_directory:
            temporary = Path(temporary_directory)
            flash_copy = temporary / "qemu_flash.bin"
            efuse_copy = temporary / "qemu_efuse.bin"
            shutil.copy2(flash_source, flash_copy)
            shutil.copy2(efuse_source, efuse_copy)
            return _monitor(
                _qemu_machine_command(
                    executable, flash_copy, efuse_copy
                ),
                arguments,
            )
    except (OSError, RuntimeError) as error:
        sys.stderr.write(f"QEMU runner setup failed: {error}\n")
        return 1


def _build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser()
    parser.add_argument("--qemu", type=Path)
    parser.add_argument("--install", type=Path)
    parser.add_argument("--manifest", type=Path)
    parser.add_argument("--expect", required=True)
    parser.add_argument("--timeout", type=float, default=60.0)
    parser.add_argument("--log", type=Path, required=True)
    return parser


def main() -> int:
    return _run(_build_parser().parse_args())


if __name__ == "__main__":
    raise SystemExit(main())
