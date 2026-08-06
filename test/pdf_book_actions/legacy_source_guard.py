from __future__ import annotations

import hashlib
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / "src/activities/home/BookActions.cpp"
BASELINE_SHA256 = "c4d9f19f8f96ae9e09170ae51539bcf949e35f71efd7e716bc13a32443003db7"
EXPECTED_BLOCKS = 10


def strip_pdf_parity_blocks(source: str) -> str:
    output: list[str] = []
    active_label: str | None = None
    completed = 0
    for line in source.splitlines(keepends=True):
        if "PDF_BOOK_ACTIONS_PARITY_BEGIN:" in line:
            if active_label is not None:
                raise RuntimeError("nested PDF parity marker")
            active_label = line.split("PDF_BOOK_ACTIONS_PARITY_BEGIN:", 1)[1].strip()
            continue
        if "PDF_BOOK_ACTIONS_PARITY_END:" in line:
            label = line.split("PDF_BOOK_ACTIONS_PARITY_END:", 1)[1].strip()
            if active_label is None or label != active_label:
                raise RuntimeError("unbalanced PDF parity marker")
            active_label = None
            completed += 1
            continue
        if active_label is None:
            output.append(line)
    if active_label is not None:
        raise RuntimeError("unterminated PDF parity marker")
    if completed != EXPECTED_BLOCKS:
        raise RuntimeError(f"expected {EXPECTED_BLOCKS} PDF parity blocks, found {completed}")
    return "".join(output)


def baseline_matches(source: str) -> bool:
    stripped = strip_pdf_parity_blocks(source)
    return hashlib.sha256(stripped.encode("utf-8")).hexdigest() == BASELINE_SHA256


def main() -> None:
    source = SOURCE.read_text(encoding="utf-8")
    if not baseline_matches(source):
        raise RuntimeError("legacy BookActions bytes changed outside marked PDF-only blocks")

    mutated = source.replace(
        "return FsHelpers::hasEpubExtension(path) || FsHelpers::hasXtcExtension(path);",
        "return FsHelpers::hasEpubExtension(path);",
        1,
    )
    if mutated == source or baseline_matches(mutated):
        raise RuntimeError("legacy source guard failed its mutation witness")

    print("PDF_BOOK_ACTIONS_LEGACY_SOURCE_GUARD_PASS")


if __name__ == "__main__":
    main()
