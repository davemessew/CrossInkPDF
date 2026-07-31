from __future__ import annotations

import os
from pathlib import Path
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[2]
TEST_DIR = Path(__file__).resolve().parent
SOURCE = ROOT / "src/activities/home/BookActions.cpp"


MUTATIONS = (
    (
        "missing PDF Delete Stats menu entry",
        "    items.push_back({FileBrowserAction::DeleteStats, StrId::STR_DELETE_BOOK_STATS});\n",
        "",
        "PDF menu must expose Delete, Clear Cache, Delete Stats, Toggle Completed in order",
    ),
    (
        "read-only migration fence removed",
        " || readOnlyFallback) {",
        ") {",
        "read-only fallback must refuse PDF stats deletion",
    ),
    (
        "cached product availability inverted",
        "  if (!loaded.available()) return;\n",
        "  if (loaded.available()) return;\n",
        "PDF recent metadata must come only from validated cached product state",
    ),
    (
        "PDF move recents policy forced to keep",
        "BookMoveUtils::moveBook(fullPath, destination, !SETTINGS.removeReadBooksFromRecents);",
        "BookMoveUtils::moveBook(fullPath, destination, true);",
        "PDF move must preserve configured keep/remove recents policy",
    ),
)


def compile_and_run(compiler: str, source: str, label: str, expected_failure: str) -> None:
    with tempfile.TemporaryDirectory(prefix="pdf-book-actions-mutation-") as temp:
        temp_dir = Path(temp)
        mutated_source = temp_dir / "BookActionsUnderTest.cpp"
        adjacent_header = temp_dir / "BookActions.h"
        executable = temp_dir / ("PdfBookActionsMutation.exe" if os.name == "nt" else "PdfBookActionsMutation")
        mutated_source.write_text(source, encoding="utf-8")
        adjacent_header.write_text((TEST_DIR / "stubs/BookActions.h").read_text(encoding="utf-8"), encoding="utf-8")

        compile_result = subprocess.run(
            [
                compiler,
                "-std=c++20",
                "-Wall",
                "-Wextra",
                "-Werror",
                "-pedantic",
                "-fno-exceptions",
                "-fno-rtti",
                "-Os",
                f"-I{temp_dir}",
                f"-I{TEST_DIR / 'stubs'}",
                str(mutated_source),
                str(TEST_DIR / "PdfBookActionsTest.cpp"),
                str(TEST_DIR / "stubs/TestState.cpp"),
                "-o",
                str(executable),
            ],
            check=False,
            capture_output=True,
            text=True,
        )
        if compile_result.returncode != 0:
            raise RuntimeError(f"{label} did not compile:\n{compile_result.stderr}")

        run_result = subprocess.run([str(executable)], check=False, capture_output=True, text=True)
        if run_result.returncode == 0 or expected_failure not in run_result.stderr:
            raise RuntimeError(
                f"{label} escaped the behavior witness\n"
                f"stdout:\n{run_result.stdout}\nstderr:\n{run_result.stderr}"
            )


def main() -> None:
    original = SOURCE.read_text(encoding="utf-8")
    compiler = os.environ.get("CXX", "c++")
    for label, target, replacement, expected_failure in MUTATIONS:
        mutated = original.replace(target, replacement, 1)
        if mutated == original:
            raise RuntimeError(f"could not create mutation: {label}")
        compile_and_run(compiler, mutated, label, expected_failure)
    print("PDF_BOOK_ACTIONS_BEHAVIOR_MUTATIONS_PASS")


if __name__ == "__main__":
    main()
