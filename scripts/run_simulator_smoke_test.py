#!/usr/bin/env python3
"""Build and run the simulator smoke test against an isolated fs_ directory."""

from __future__ import annotations

import argparse
import json
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
PROGRAM = ROOT / ".pio" / "build" / "simulator" / "program"
DEFAULT_BOOK = ROOT / "test" / "epubs" / "test_reader_rendering_matrix.epub"
CRASH_PATTERNS = (
    "std::bad_alloc",
    "terminating due to uncaught exception",
    "Assertion failed",
    "Segmentation fault",
    "AddressSanitizer",
    "UndefinedBehaviorSanitizer",
)
THEMES = {
    "classic": 0,
    "lyra": 1,
    "lyra-extended": 2,
    "lyra_extended": 2,
    "lyra3": 2,
    "lyra-3-covers": 2,
    "roundedraff": 3,
    "rounded-raff": 3,
    "lyra-carousel": 4,
    "lyra_carousel": 4,
    "carousel": 4,
}
REFLOW_MARKERS = {
    "uncached": "SIM_REFLOW_UNCACHED_PASS ",
    "cached": "SIM_REFLOW_CACHED_PASS ",
}
REFLOW_MARKER_BYTES = {
    name: marker.encode("ascii") for name, marker in REFLOW_MARKERS.items()
}


def build_simulator() -> None:
    print("Building simulator...", flush=True)
    proc = subprocess.run(["pio", "run", "-e", "simulator"], cwd=ROOT)
    if proc.returncode != 0:
        raise SystemExit(proc.returncode)


def prepare_fs(temp_root: Path, book: Path) -> str:
    books_dir = temp_root / "fs_" / "books"
    books_dir.mkdir(parents=True, exist_ok=True)

    target = books_dir / book.name
    shutil.copy2(book, target)
    return f"/books/{book.name}"


def retain_only_generated_book_cache(temp_root: Path) -> bool:
    filesystem = temp_root / "fs_"
    cache_root = filesystem / ".crosspoint"
    book_caches = (
        [
            entry
            for entry in cache_root.iterdir()
            if entry.is_dir() and entry.name.startswith("epub_")
        ]
        if cache_root.is_dir()
        else []
    )
    if not book_caches:
        return False

    for entry in filesystem.iterdir():
        if entry.name in {"books", ".crosspoint"}:
            continue
        if entry.is_dir():
            shutil.rmtree(entry)
        else:
            entry.unlink()

    for entry in cache_root.iterdir():
        if entry in book_caches:
            continue
        if entry.is_dir():
            shutil.rmtree(entry)
        else:
            entry.unlink()
    return True


def parse_reflow_oracle_marker(output: bytes, pass_name: str) -> dict:
    marker = REFLOW_MARKER_BYTES[pass_name]
    payloads = [
        line.split(marker, 1)[1]
        for line in output.splitlines()
        if marker in line
    ]
    if len(payloads) != 1:
        raise ValueError(
            f"expected exactly one {REFLOW_MARKERS[pass_name].strip()} "
            f"marker, found "
            f"{len(payloads)}"
        )
    try:
        payload = payloads[0].decode("utf-8", errors="strict")
    except UnicodeDecodeError as error:
        raise ValueError(
            f"{REFLOW_MARKERS[pass_name].strip()} contains invalid UTF-8: "
            f"{error}"
        ) from error
    try:
        value = json.loads(payload)
    except json.JSONDecodeError as error:
        raise ValueError(
            f"{REFLOW_MARKERS[pass_name].strip()} contains invalid JSON: "
            f"{error}"
        ) from error
    if not isinstance(value, dict):
        raise ValueError(
            f"{REFLOW_MARKERS[pass_name].strip()} must contain a JSON object"
        )
    return value


def decode_log_output(output: bytes) -> str:
    return output.decode("utf-8", errors="backslashreplace")


def _first_difference(expected: object, actual: object, path: str = "") -> str:
    if type(expected) is not type(actual):
        return path or "<root>"
    if isinstance(expected, dict):
        expected_keys = set(expected)
        actual_keys = set(actual)
        if expected_keys != actual_keys:
            differing = sorted(expected_keys.symmetric_difference(actual_keys))
            return ".".join(filter(None, (path, differing[0])))
        for key in sorted(expected):
            difference = _first_difference(
                expected[key],
                actual[key],
                ".".join(filter(None, (path, key))),
            )
            if difference:
                return difference
        return ""
    if isinstance(expected, list):
        if len(expected) != len(actual):
            return path or "<root>"
        for index, (expected_item, actual_item) in enumerate(
            zip(expected, actual, strict=True)
        ):
            difference = _first_difference(
                expected_item,
                actual_item,
                f"{path}[{index}]",
            )
            if difference:
                return difference
        return ""
    return "" if expected == actual else (path or "<root>")


def verify_reflow_oracles(
    expected: dict, actuals: list[tuple[str, dict]]
) -> None:
    if not actuals:
        raise ValueError("no reflow oracle output was captured")
    for pass_name, actual in actuals:
        difference = _first_difference(expected, actual)
        if difference:
            raise ValueError(
                f"{pass_name} reflow oracle mismatch at {difference}"
            )


def verify_reflow_pass_consistency(
    actuals: list[tuple[str, dict]],
) -> None:
    if len(actuals) > 1:
        verify_reflow_oracles(actuals[0][1], actuals[1:])


def _load_reflow_oracle(path: Path, book: Path) -> dict:
    try:
        document = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise ValueError(f"cannot read reflow oracle {path}: {error}") from error
    if not isinstance(document, dict) or document.get("schema_version") != 1:
        raise ValueError("reflow oracle must use schema_version 1")
    if document.get("fixture") != book.name:
        raise ValueError(
            f"reflow oracle fixture is not {book.name}"
        )
    oracle = document.get("oracle")
    if not isinstance(oracle, dict):
        raise ValueError("reflow oracle has no oracle object")
    return oracle


def _validate_process_output(
    process: subprocess.CompletedProcess[bytes],
) -> int:
    if process.returncode != 0:
        print(
            f"Simulator smoke test failed with exit code "
            f"{process.returncode}",
            file=sys.stderr,
        )
        return process.returncode

    for pattern in CRASH_PATTERNS:
        if pattern.encode("ascii") in process.stdout:
            print(
                f"Simulator smoke test output contained crash pattern: "
                f"{pattern}",
                file=sys.stderr,
            )
            return 2

    if b"Simulator smoke test passed" not in process.stdout:
        print(
            "Simulator smoke test did not print its success marker",
            file=sys.stderr,
        )
        return 2
    return 0


def run_smoke(args: argparse.Namespace) -> int:
    book = Path(args.book).resolve()
    program = Path(args.program).resolve()
    if not book.exists():
        print(f"Smoke test book not found: {book}", file=sys.stderr)
        return 2

    if args.build:
        build_simulator()

    if not program.exists():
        print(f"Simulator binary not found: {program}", file=sys.stderr)
        print("Run: pio run -e simulator", file=sys.stderr)
        return 2

    oracle_path = (
        Path(args.reflow_oracle).resolve()
        if args.reflow_oracle is not None
        else None
    )
    if args.update_reflow_oracle and oracle_path is None:
        print(
            "--update-reflow-oracle requires --reflow-oracle",
            file=sys.stderr,
        )
        return 2
    expected_oracle = None
    if oracle_path is not None and not args.update_reflow_oracle:
        try:
            expected_oracle = _load_reflow_oracle(oracle_path, book)
        except ValueError as error:
            print(error, file=sys.stderr)
            return 2

    actual_oracles: list[tuple[str, dict]] = []
    with tempfile.TemporaryDirectory(prefix="crossink-sim-smoke-") as temp_dir_name:
        temp_root = Path(temp_dir_name)
        simulator_book_path = prepare_fs(temp_root, book)

        for pass_index in range(args.passes):
            pass_name = "uncached" if pass_index == 0 else "cached"
            if pass_index > 0 and not retain_only_generated_book_cache(
                temp_root
            ):
                print(
                    "Simulator uncached pass produced no EPUB cache",
                    file=sys.stderr,
                )
                return 2

            env = os.environ.copy()
            env["CROSSINK_SIMULATOR_SMOKE_TEST"] = "1"
            env["CROSSINK_SIMULATOR_SMOKE_BOOK"] = simulator_book_path
            env["CROSSINK_SIMULATOR_SMOKE_PAGE_TURNS"] = str(
                args.page_turns
            )
            if oracle_path is not None:
                env["CROSSINK_SIMULATOR_REFLOW_ORACLE"] = "1"
                env["CROSSINK_SIMULATOR_REFLOW_PASS"] = pass_name
                if args.embedded_style is not None:
                    env["CROSSINK_SIMULATOR_REFLOW_EMBEDDED_STYLE"] = str(
                        args.embedded_style
                    )
                if args.bookmark_snippet is not None:
                    env["CROSSINK_SIMULATOR_REFLOW_BOOKMARK_SNIPPET"] = (
                        args.bookmark_snippet
                    )
            if args.theme:
                env["CROSSINK_SIMULATOR_SMOKE_THEME"] = str(
                    THEMES[args.theme]
                )
            if args.headless:
                env.setdefault("SDL_VIDEODRIVER", "dummy")

            print(
                f"Running {pass_name} simulator smoke test with isolated "
                f"fs_: {temp_root / 'fs_'}",
                flush=True,
            )
            process = subprocess.run(
                [str(program)],
                cwd=temp_root,
                env=env,
                text=False,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                timeout=args.timeout,
            )
            sys.stdout.write(decode_log_output(process.stdout))
            result = _validate_process_output(process)
            if result != 0:
                return result
            if oracle_path is not None:
                try:
                    actual_oracles.append(
                        (
                            pass_name,
                            parse_reflow_oracle_marker(
                                process.stdout, pass_name
                            ),
                        )
                    )
                except ValueError as error:
                    print(error, file=sys.stderr)
                    return 2

    if actual_oracles:
        try:
            verify_reflow_pass_consistency(actual_oracles)
            if expected_oracle is not None:
                verify_reflow_oracles(expected_oracle, actual_oracles)
        except ValueError as error:
            print(error, file=sys.stderr)
            return 2
        if args.update_reflow_oracle:
            document = {
                "schema_version": 1,
                "fixture": book.name,
                "oracle": actual_oracles[0][1],
            }
            oracle_path.parent.mkdir(parents=True, exist_ok=True)
            oracle_path.write_text(
                json.dumps(document, indent=2, sort_keys=True) + "\n",
                encoding="utf-8",
            )
            print(f"Updated reflow oracle: {oracle_path}")

    return 0


def ascii_bookmark_snippet(value: str) -> str:
    try:
        encoded = value.encode("ascii")
    except UnicodeEncodeError as error:
        raise argparse.ArgumentTypeError(
            "bookmark snippet must contain ASCII characters only"
        ) from error
    if not 1 <= len(encoded) <= 63:
        raise argparse.ArgumentTypeError(
            "bookmark snippet must be between 1 and 63 ASCII bytes"
        )
    return value


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--program", default=str(PROGRAM), help="Native simulator executable")
    parser.add_argument(
        "--book",
        default=str(DEFAULT_BOOK),
        help="Book fixture to copy into the isolated simulator fs_",
    )
    parser.add_argument("--timeout", type=int, default=45, help="Seconds before the simulator run is treated as hung")
    parser.add_argument(
        "--passes",
        type=int,
        choices=(1, 2),
        default=1,
        help="Run once uncached or twice against the same generated cache",
    )
    parser.add_argument(
        "--page-turns",
        type=int,
        default=2,
        help="Number of reader page-forward taps to run",
    )
    parser.add_argument(
        "--reflow-oracle",
        help="Expected deterministic reflow oracle JSON",
    )
    parser.add_argument(
        "--update-reflow-oracle",
        action="store_true",
        help="Replace the selected oracle with the observed locked output",
    )
    parser.add_argument(
        "--embedded-style",
        type=int,
        choices=(0, 1),
        default=None,
        help=(
            "Override embedded styles for oracle derivation; omitted keeps "
            "the locked default enabled"
        ),
    )
    parser.add_argument(
        "--bookmark-snippet",
        type=ascii_bookmark_snippet,
        default=None,
        help=(
            "Use an explicit ASCII bookmark snippet for candidate oracle "
            "derivation (1-63 bytes)"
        ),
    )
    parser.add_argument("--theme", choices=sorted(THEMES), help="UI theme to use during the smoke test")
    parser.add_argument("--no-build", dest="build", action="store_false", help="Run the existing simulator binary")
    parser.add_argument("--window", dest="headless", action="store_false", help="Show the SDL window instead of using dummy video")
    parser.set_defaults(build=True, headless=True)
    return parser.parse_args()


def main() -> int:
    return run_smoke(parse_args())


if __name__ == "__main__":
    raise SystemExit(main())
