from __future__ import annotations

import os
from pathlib import Path
import shutil
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[2]
TEST_DIR = Path(__file__).resolve().parent
SOURCE = ROOT / "src/activities/home/BookActions.cpp"


MUTATIONS = (
    (
        "cache-root creation and verification bypassed",
        "  if (!ensureVerifiedPdfCacheRoot(cachePath)) return false;\n",
        "  (void)&ensureVerifiedPdfCacheRoot;\n",
        "resolved PDF cache root must exist as a directory",
    ),
    (
        "cache-root directory verification bypassed",
        "  const bool validDirectory = root && root.isDirectory();\n",
        "  const bool validDirectory = true;\n",
        "PDF completion must fail when the created cache root cannot be verified",
    ),
    (
        "durable stats readback bypassed",
        "  if (!verifyPdfCompletionDurable(cachePath, nextCompleted)) return false;\n",
        "  (void)verifyPdfCompletionDurable(cachePath, nextCompleted);\n",
        "PDF completion must fail when production BookReadingStats cannot persist",
    ),
)


def compile_and_run(compiler: str, source: str, label: str, expected_failure: str) -> None:
    with tempfile.TemporaryDirectory(prefix="pdf-book-actions-durability-mutation-") as temporary:
        temp = Path(temporary)
        mutated_source = temp / "BookActionsUnderTest.cpp"
        adjacent_header = temp / "BookActions.h"
        production_stubs = temp / "production_stubs"
        executable = temp / (
            "PdfBookActionsDurabilityMutation.exe"
            if os.name == "nt"
            else "PdfBookActionsDurabilityMutation"
        )

        mutated_source.write_text(source, encoding="utf-8")
        adjacent_header.write_text(
            (TEST_DIR / "stubs/BookActions.h").read_text(encoding="utf-8"),
            encoding="utf-8",
        )
        shutil.copytree(TEST_DIR / "stubs", production_stubs)
        (production_stubs / "activities/reader/BookReadingStats.h").unlink()
        shutil.copy2(TEST_DIR / "production/HalStorage.h", production_stubs / "HalStorage.h")

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
                "-DPDF_BOOK_ACTIONS_PRODUCTION_STATS=1",
                f"-I{temp}",
                f"-I{production_stubs}",
                f"-I{ROOT / 'src'}",
                str(mutated_source),
                str(ROOT / "src/activities/reader/BookReadingStats.cpp"),
                str(TEST_DIR / "production/PdfBookActionsDurabilityTest.cpp"),
                str(TEST_DIR / "production/ProductionReadingStatsSupport.cpp"),
                str(TEST_DIR / "production/ProductionTestStorage.cpp"),
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
                f"{label} escaped the production durability witness\n"
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
    print("PDF_BOOK_ACTIONS_DURABILITY_MUTATIONS_PASS")


if __name__ == "__main__":
    main()
