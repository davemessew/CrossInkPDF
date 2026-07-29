import argparse
import json
from pathlib import Path
import subprocess
import sys
from typing import Any


PASS_MARKER = "QEMU_RESOURCE_PASS\n"
CODE_SECTIONS = (
    ".iram0.text",
    ".iram0.vectors",
    ".flash.text",
    ".flash.rodata",
)
STATIC_DRAM_SECTIONS = (
    ".dram0.data",
    ".dram0.bss",
    ".noinit",
)
FINGERPRINT_FIELDS = (
    "toolchain",
    "platform",
    "framework",
    "build_flags",
    "partition_sha256",
    "qemu_hal_sha256",
    "qemu_config_sha256",
)
LIMITS = {
    "text": 262144,
    "static_dram": 12288,
    "pdf_heap": 81920,
    "free_heap": 65536,
    "largest_block": 49152,
    "allocation": 32768,
    "stack_margin": 1024,
}
RUNTIME_KEYS = (
    "heap_start",
    "min_free",
    "min_max_alloc",
    "stack_margin",
)


class ResourceCheckError(Exception):
    pass


def _load_json(path: Path) -> dict[str, Any]:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise ResourceCheckError(f"cannot read {path}: {error}") from error
    if not isinstance(value, dict):
        raise ResourceCheckError(f"{path} must contain a JSON object")
    return value


def _normalized_fingerprint(manifest: dict[str, Any]) -> dict[str, Any]:
    fingerprint = manifest.get("resource_fingerprint")
    if not isinstance(fingerprint, dict):
        raise ResourceCheckError("manifest has no resource_fingerprint object")

    missing = [field for field in FINGERPRINT_FIELDS if field not in fingerprint]
    if missing:
        raise ResourceCheckError(
            "resource fingerprint is missing " + ", ".join(missing)
        )

    flags = fingerprint["build_flags"]
    if not isinstance(flags, list) or not all(
        isinstance(flag, str) for flag in flags
    ):
        raise ResourceCheckError("resource fingerprint build_flags must be a list")

    normalized = {
        field: fingerprint[field]
        for field in FINGERPRINT_FIELDS
        if field != "build_flags"
    }
    normalized["build_flags"] = sorted(
        {" ".join(flag.split()) for flag in flags}
    )
    return normalized


def _size_tool_from_manifest(manifest: dict[str, Any]) -> Path:
    tools = manifest.get("tools")
    size = tools.get("size") if isinstance(tools, dict) else None
    raw_path = size.get("path") if isinstance(size, dict) else None
    if not isinstance(raw_path, str):
        raise ResourceCheckError("manifest has no tools.size.path")

    path = Path(raw_path)
    if not path.is_absolute():
        raise ResourceCheckError("manifest size tool path must be absolute")
    if not path.is_file():
        raise ResourceCheckError(f"manifest size tool does not exist: {path}")
    return path


def _read_elf_sections(
    manifest: dict[str, Any], elf_path: Path
) -> dict[str, int]:
    if not elf_path.is_file():
        raise ResourceCheckError(f"ELF does not exist: {elf_path}")

    size_tool = _size_tool_from_manifest(manifest)
    command = [str(size_tool), "-A", str(elf_path)]
    if size_tool.suffix.lower() == ".py":
        command.insert(0, sys.executable)
    try:
        completed = subprocess.run(
            command,
            capture_output=True,
            text=True,
            check=False,
        )
    except OSError as error:
        raise ResourceCheckError(f"cannot execute size tool: {error}") from error
    if completed.returncode != 0:
        raise ResourceCheckError(
            f"size tool failed with exit code {completed.returncode}"
        )

    requested_sections = set(CODE_SECTIONS + STATIC_DRAM_SECTIONS)
    section_sizes: dict[str, int] = {}
    for line in completed.stdout.splitlines():
        fields = line.split()
        if len(fields) < 2 or fields[0] not in requested_sections:
            continue
        section = fields[0]
        if section in section_sizes:
            raise ResourceCheckError(f"duplicate ELF section: {section}")
        try:
            size = int(fields[1], 0)
        except ValueError as error:
            raise ResourceCheckError(
                f"invalid size for ELF section {section}"
            ) from error
        if size < 0:
            raise ResourceCheckError(f"negative size for ELF section {section}")
        section_sizes[section] = size

    missing = requested_sections.difference(section_sizes)
    if missing:
        raise ResourceCheckError(
            "size output is missing sections " + ", ".join(sorted(missing))
        )
    return section_sizes


def _read_runtime_measurements(runtime_log: Path) -> dict[str, int]:
    try:
        lines = runtime_log.read_text(encoding="utf-8").splitlines()
    except OSError as error:
        raise ResourceCheckError(
            f"cannot read runtime log {runtime_log}: {error}"
        ) from error

    samples: list[dict[str, int]] = []
    for line in lines:
        if not line.startswith("QEMU_RUNTIME "):
            continue
        fields: dict[str, int] = {}
        for token in line.removeprefix("QEMU_RUNTIME ").split():
            key, separator, raw_value = token.partition("=")
            if not separator:
                raise ResourceCheckError("malformed QEMU_RUNTIME token")
            try:
                fields[key] = int(raw_value, 10)
            except ValueError as error:
                raise ResourceCheckError(
                    f"invalid QEMU_RUNTIME value for {key}"
                ) from error
        missing = [key for key in RUNTIME_KEYS if key not in fields]
        if missing:
            raise ResourceCheckError(
                "QEMU_RUNTIME is missing " + ", ".join(missing)
            )
        fields.setdefault("max_alloc", 0)
        if fields["heap_start"] < fields["min_free"]:
            raise ResourceCheckError("QEMU_RUNTIME min_free exceeds heap_start")
        if any(value < 0 for value in fields.values()):
            raise ResourceCheckError("QEMU_RUNTIME values must be non-negative")
        samples.append(fields)

    if not samples:
        raise ResourceCheckError("runtime log has no QEMU_RUNTIME samples")

    return {
        "peak_heap": max(
            sample["heap_start"] - sample["min_free"] for sample in samples
        ),
        "min_free_heap": min(sample["min_free"] for sample in samples),
        "min_largest_block": min(
            sample["min_max_alloc"] for sample in samples
        ),
        "max_allocation": max(sample["max_alloc"] for sample in samples),
        "min_stack_margin": min(
            sample["stack_margin"] for sample in samples
        ),
    }


def _measure(
    manifest: dict[str, Any], elf_path: Path, runtime_log: Path
) -> dict[str, int]:
    sections = _read_elf_sections(manifest, elf_path)
    measurements = {
        "code_rodata": sum(sections[name] for name in CODE_SECTIONS),
        "static_dram": sum(
            sections[name] for name in STATIC_DRAM_SECTIONS
        ),
    }
    measurements.update(_read_runtime_measurements(runtime_log))
    return measurements


def _violations(
    baseline: dict[str, int], current: dict[str, int]
) -> list[str]:
    values = {
        "text": current["code_rodata"] - baseline["code_rodata"],
        "static_dram": current["static_dram"] - baseline["static_dram"],
        "pdf_heap": current["peak_heap"] - baseline["peak_heap"],
        "free_heap": current["min_free_heap"],
        "largest_block": current["min_largest_block"],
        "allocation": current["max_allocation"],
        "stack_margin": current["min_stack_margin"],
    }
    failures: list[str] = []
    for name in ("text", "static_dram", "pdf_heap", "allocation"):
        if values[name] > LIMITS[name]:
            failures.append(name)
    for name in ("free_heap", "largest_block", "stack_margin"):
        if values[name] < LIMITS[name]:
            failures.append(name)
    return failures


def _capture(arguments: argparse.Namespace) -> None:
    manifest = _load_json(arguments.manifest)
    fingerprint = _normalized_fingerprint(manifest)
    measurements = _measure(manifest, arguments.elf, arguments.runtime_log)
    failures = _violations(measurements, measurements)
    if failures:
        raise ResourceCheckError(
            "baseline violates resource limits: " + ", ".join(failures)
        )

    baseline = {
        "schema_version": 1,
        "resource_fingerprint": fingerprint,
        "measurements": measurements,
        "limits": LIMITS,
    }
    try:
        arguments.out.parent.mkdir(parents=True, exist_ok=True)
        arguments.out.write_text(
            json.dumps(baseline, indent=2, sort_keys=True) + "\n",
            encoding="utf-8",
        )
    except OSError as error:
        raise ResourceCheckError(
            f"cannot write baseline {arguments.out}: {error}"
        ) from error


def _verify(arguments: argparse.Namespace) -> None:
    baseline = _load_json(arguments.baseline)
    manifest = _load_json(arguments.manifest)
    current_fingerprint = _normalized_fingerprint(manifest)
    if baseline.get("resource_fingerprint") != current_fingerprint:
        raise ResourceCheckError("resource fingerprint differs from baseline")

    baseline_measurements = baseline.get("measurements")
    if not isinstance(baseline_measurements, dict):
        raise ResourceCheckError("baseline has no measurements object")
    required_measurements = {
        "code_rodata",
        "static_dram",
        "peak_heap",
        "min_free_heap",
        "min_largest_block",
        "max_allocation",
        "min_stack_margin",
    }
    if (
        set(baseline_measurements) != required_measurements
        or not all(
            isinstance(value, int) for value in baseline_measurements.values()
        )
    ):
        raise ResourceCheckError("baseline measurements are invalid")

    current = _measure(manifest, arguments.elf, arguments.runtime_log)
    failures = _violations(baseline_measurements, current)
    if failures:
        raise ResourceCheckError(
            "resource limits exceeded: " + ", ".join(failures)
        )


def _build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser()
    subparsers = parser.add_subparsers(dest="command", required=True)

    capture = subparsers.add_parser("capture")
    capture.add_argument("--manifest", type=Path, required=True)
    capture.add_argument("--elf", type=Path, required=True)
    capture.add_argument("--runtime-log", type=Path, required=True)
    capture.add_argument("--out", type=Path, required=True)

    verify = subparsers.add_parser("verify")
    verify.add_argument("--baseline", type=Path, required=True)
    verify.add_argument("--manifest", type=Path, required=True)
    verify.add_argument("--elf", type=Path, required=True)
    verify.add_argument("--runtime-log", type=Path, required=True)
    return parser


def main() -> int:
    arguments = _build_parser().parse_args()
    try:
        if arguments.command == "capture":
            _capture(arguments)
        else:
            _verify(arguments)
    except ResourceCheckError as error:
        sys.stderr.write(f"QEMU_RESOURCE_FAIL: {error}\n")
        return 1
    sys.stdout.write(PASS_MARKER)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
