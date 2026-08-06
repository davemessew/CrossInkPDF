import os
from pathlib import Path
import re
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[2]
TEST_DIR = Path(__file__).resolve().parent


def main() -> None:
    source = (ROOT / "src/util/NextBookFinder.cpp").read_text(encoding="utf-8")
    mutated, replacements = re.subn(
        r"\s*\|\|\s*FsHelpers::hasPdfExtension\(name\)",
        "",
        source,
        count=1,
    )
    if replacements != 1:
        raise RuntimeError("could not create the missing-PDF behavior mutation")

    with tempfile.TemporaryDirectory(prefix="pdf-format-routing-negative-") as temp:
        temp_dir = Path(temp)
        mutated_source = temp_dir / "NextBookFinder.cpp"
        executable = temp_dir / "PdfFormatRoutingNegative"
        mutated_source.write_text(mutated, encoding="utf-8")

        compiler = os.environ.get("CXX", "c++")
        compile_result = subprocess.run(
            [
                compiler,
                "-std=c++20",
                "-Os",
                "-Wall",
                "-Wextra",
                "-Werror",
                "-pedantic",
                "-fno-exceptions",
                "-fno-rtti",
                f"-I{TEST_DIR / 'stubs'}",
                f"-I{ROOT / 'lib/FsHelpers'}",
                f"-I{ROOT / 'lib/Memory'}",
                f"-I{ROOT / 'src/util'}",
                str(ROOT / "lib/FsHelpers/FsHelpers.cpp"),
                str(ROOT / "lib/FsHelpers/NaturalSort.cpp"),
                str(mutated_source),
                str(TEST_DIR / "NextBookFinderTest.cpp"),
                "-o",
                str(executable),
            ],
            check=False,
            capture_output=True,
            text=True,
        )
        if compile_result.returncode != 0:
            raise RuntimeError(f"negative-control compile failed:\n{compile_result.stderr}")

        run_result = subprocess.run(
            [str(executable)],
            check=False,
            capture_output=True,
            text=True,
        )
        expected_failure = "PDF must join existing book formats in natural order"
        if run_result.returncode == 0 or expected_failure not in run_result.stderr:
            raise RuntimeError(
                "behavior negative control did not catch missing PDF routing:\n"
                f"stdout:\n{run_result.stdout}\nstderr:\n{run_result.stderr}"
            )

    print("PDF_FORMAT_ROUTING_BEHAVIOR_NEGATIVE_CONTROL_PASS")


if __name__ == "__main__":
    main()
