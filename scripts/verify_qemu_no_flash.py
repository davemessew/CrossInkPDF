import argparse
from pathlib import Path
import re
import subprocess
import sys


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
    for target in PROHIBITED_TARGETS:
        completed = subprocess.run(
            _pio_command(arguments.pio)
            + ["run", "-e", "qemu-esp32c3", "-t", target],
            capture_output=True,
            text=True,
            check=False,
        )
        combined_output = completed.stdout + completed.stderr
        accessed_device = any(
            pattern.search(combined_output) for pattern in DEVICE_ACCESS_PATTERNS
        )
        if (
            completed.returncode != 2
            or completed.stderr != EXPECTED_REFUSAL
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
