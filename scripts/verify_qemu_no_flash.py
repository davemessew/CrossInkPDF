import argparse
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile


EXPECTED_REFUSAL = "QEMU target cannot be flashed\n"
PROHIBITED_TARGETS = (
    "upload",
    "uploadfs",
    "uploadfsota",
    "erase",
    "erase_upload",
    "download_fs",
)
DEVICE_ACCESS_PATTERNS = (
    re.compile(r"\bCOM\d+\b", re.IGNORECASE),
    re.compile(r"/dev/tty", re.IGNORECASE),
    re.compile(r"\bserial\s+port\b", re.IGNORECASE),
    re.compile(r"\bwrite_flash\b", re.IGNORECASE),
)


def _pio_command(executable: Path) -> list[str]:
    if executable.suffix.lower() == ".py":
        return [sys.executable, str(executable)]
    return [str(executable)]


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--pio", type=Path, required=True)
    arguments = parser.parse_args()

    failures: list[str] = []
    with tempfile.TemporaryDirectory(
        prefix="crossink-qemu-no-flash-"
    ) as temporary_directory:
        isolation_root = Path(temporary_directory)
        for target in PROHIBITED_TARGETS:
            environment = os.environ.copy()
            environment["PLATFORMIO_BUILD_DIR"] = str(
                (isolation_root / target / "build").resolve()
            )
            completed = subprocess.run(
                _pio_command(arguments.pio)
                + [
                    "run",
                    "--disable-auto-clean",
                    "-e",
                    "qemu-esp32c3",
                    "-t",
                    target,
                ],
                capture_output=True,
                text=True,
                check=False,
                env=environment,
            )
            combined_output = completed.stdout + completed.stderr
            output_lines = (
                completed.stdout.splitlines() + completed.stderr.splitlines()
            )
            refused_safely = (
                EXPECTED_REFUSAL.rstrip("\n") in output_lines
            )
            accessed_device = any(
                pattern.search(combined_output)
                for pattern in DEVICE_ACCESS_PATTERNS
            )
            if (
                completed.returncode == 0
                or not refused_safely
                or accessed_device
            ):
                failures.append(target)

    if failures:
        for target in failures:
            sys.stderr.write(
                f"unsafe target {target} was not refused safely\n"
            )
        return 1

    sys.stdout.write("QEMU_NO_FLASH_PASS\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
