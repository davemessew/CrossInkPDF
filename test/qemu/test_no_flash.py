import ast
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


REPO_ROOT = Path(__file__).resolve().parents[2]
REFUSAL_SCRIPT = REPO_ROOT / "scripts" / "refuse_qemu_flash.py"
VERIFIER_SCRIPT = REPO_ROOT / "scripts" / "verify_qemu_no_flash.py"
EXPECTED_REFUSAL = "QEMU target cannot be flashed\n"
PROHIBITED_TARGETS = (
    "upload",
    "uploadfs",
    "uploadfsota",
    "erase",
    "erase_upload",
    "download_fs",
)


class NoFlashWitnessTest(unittest.TestCase):
    def test_refusal_happens_without_flash_or_serial_code(self) -> None:
        completed = subprocess.run(
            [sys.executable, str(REFUSAL_SCRIPT)],
            cwd=REPO_ROOT,
            capture_output=True,
            text=True,
            check=False,
        )

        self.assertEqual(completed.returncode, 2)
        self.assertEqual(completed.stdout, "")
        self.assertEqual(completed.stderr, EXPECTED_REFUSAL)

        source = REFUSAL_SCRIPT.read_text(encoding="utf-8")
        source_lower = source.lower()
        for prohibited in ("com", "/dev/tty", "serial", "write_flash", "esptool"):
            self.assertNotIn(prohibited, source_lower)

        tree = ast.parse(source)
        imported_modules = {
            alias.name.split(".", 1)[0]
            for node in ast.walk(tree)
            if isinstance(node, (ast.Import, ast.ImportFrom))
            for alias in node.names
        }
        self.assertTrue(imported_modules.isdisjoint({"serial", "esptool"}))

    def test_verifier_checks_every_unsafe_platformio_target(self) -> None:
        with tempfile.TemporaryDirectory() as temporary_directory:
            temporary_path = Path(temporary_directory)
            fake_pio = self._write_fake_pio(temporary_path)
            invocation_log = temporary_path / "invocations.txt"
            environment = os.environ.copy()
            environment["FAKE_PIO_LOG"] = str(invocation_log)
            environment["FAKE_PIO_MODE"] = "refuse"

            completed = subprocess.run(
                [
                    sys.executable,
                    str(VERIFIER_SCRIPT),
                    "--pio",
                    str(fake_pio),
                ],
                cwd=REPO_ROOT,
                capture_output=True,
                text=True,
                check=False,
                env=environment,
            )

            self.assertEqual(completed.returncode, 0)
            self.assertEqual(completed.stdout, "QEMU_NO_FLASH_PASS\n")
            self.assertEqual(completed.stderr, "")
            self.assertEqual(
                invocation_log.read_text(encoding="utf-8").splitlines(),
                list(PROHIBITED_TARGETS),
            )

    def test_verifier_isolates_existing_build_artifacts_per_target(
        self,
    ) -> None:
        with tempfile.TemporaryDirectory() as temporary_directory:
            temporary_path = Path(temporary_directory)
            fake_pio = self._write_fake_pio(temporary_path)
            invocation_log = temporary_path / "invocations.txt"
            build_log = temporary_path / "build-directories.jsonl"
            existing_build = temporary_path / "existing-build"
            existing_build.mkdir()
            sentinel = existing_build / "qemu_flash.bin"
            sentinel_bytes = b"existing-container-qemu-artifact"
            sentinel.write_bytes(sentinel_bytes)

            environment = os.environ.copy()
            environment["FAKE_PIO_LOG"] = str(invocation_log)
            environment["FAKE_PIO_BUILD_LOG"] = str(build_log)
            environment["FAKE_PIO_MODE"] = "autoclean_refuse"
            environment["PLATFORMIO_BUILD_DIR"] = str(
                existing_build.resolve()
            )

            completed = subprocess.run(
                [
                    sys.executable,
                    str(VERIFIER_SCRIPT),
                    "--pio",
                    str(fake_pio),
                ],
                cwd=REPO_ROOT,
                capture_output=True,
                text=True,
                check=False,
                env=environment,
            )

            self.assertEqual(completed.returncode, 0, completed.stderr)
            self.assertEqual(completed.stdout, "QEMU_NO_FLASH_PASS\n")
            self.assertEqual(completed.stderr, "")
            self.assertTrue(
                sentinel.is_file(),
                "the verifier deleted a pre-existing build artifact",
            )
            self.assertEqual(sentinel.read_bytes(), sentinel_bytes)

            records = [
                json.loads(line)
                for line in build_log.read_text(
                    encoding="utf-8"
                ).splitlines()
            ]
            self.assertEqual(
                [record["target"] for record in records],
                list(PROHIBITED_TARGETS),
            )
            build_directories = [
                Path(record["build_dir"]) for record in records
            ]
            self.assertEqual(len(set(build_directories)), len(PROHIBITED_TARGETS))
            self.assertTrue(
                all(directory.is_absolute() for directory in build_directories)
            )
            self.assertNotIn(existing_build.resolve(), build_directories)
            self.assertTrue(
                all(not directory.exists() for directory in build_directories)
            )
            for target, record in zip(PROHIBITED_TARGETS, records):
                self.assertEqual(
                    record["arguments"],
                    [
                        "run",
                        "--disable-auto-clean",
                        "-e",
                        "qemu-esp32c3",
                        "-t",
                        target,
                    ],
                )

    def test_verifier_rejects_device_enumeration_output(self) -> None:
        with tempfile.TemporaryDirectory() as temporary_directory:
            temporary_path = Path(temporary_directory)
            fake_pio = self._write_fake_pio(temporary_path)
            environment = os.environ.copy()
            environment["FAKE_PIO_LOG"] = str(temporary_path / "invocations.txt")
            environment["FAKE_PIO_MODE"] = "enumerate"

            completed = subprocess.run(
                [
                    sys.executable,
                    str(VERIFIER_SCRIPT),
                    "--pio",
                    str(fake_pio),
                ],
                cwd=REPO_ROOT,
                capture_output=True,
                text=True,
                check=False,
                env=environment,
            )

            self.assertEqual(completed.returncode, 1)
            self.assertEqual(completed.stdout, "")
            self.assertIn("unsafe target upload was not refused safely", completed.stderr)

    def test_verifier_accepts_platformio_wrapped_refusal(self) -> None:
        with tempfile.TemporaryDirectory() as temporary_directory:
            temporary_path = Path(temporary_directory)
            fake_pio = self._write_fake_pio(temporary_path)
            invocation_log = temporary_path / "invocations.txt"
            environment = os.environ.copy()
            environment["FAKE_PIO_LOG"] = str(invocation_log)
            environment["FAKE_PIO_MODE"] = "platformio_refuse"

            completed = subprocess.run(
                [
                    sys.executable,
                    str(VERIFIER_SCRIPT),
                    "--pio",
                    str(fake_pio),
                ],
                cwd=REPO_ROOT,
                capture_output=True,
                text=True,
                check=False,
                env=environment,
            )

            self.assertEqual(completed.returncode, 0)
            self.assertEqual(completed.stdout, "QEMU_NO_FLASH_PASS\n")
            self.assertEqual(completed.stderr, "")
            self.assertEqual(
                invocation_log.read_text(encoding="utf-8").splitlines(),
                list(PROHIBITED_TARGETS),
            )

    @staticmethod
    def _write_fake_pio(directory: Path) -> Path:
        fake_pio = directory / "fake_pio.py"
        fake_pio.write_text(
            "\n".join(
                (
                    "import json",
                    "import os",
                    "from pathlib import Path",
                    "import shutil",
                    "import sys",
                    "",
                    "target = sys.argv[-1]",
                    "log_path = Path(os.environ['FAKE_PIO_LOG'])",
                    "with log_path.open('a', encoding='utf-8') as log:",
                    "    log.write(target + '\\n')",
                    "mode = os.environ['FAKE_PIO_MODE']",
                    "if mode == 'autoclean_refuse':",
                    "    build_dir = Path(os.environ['PLATFORMIO_BUILD_DIR'])",
                    "    if '--disable-auto-clean' not in sys.argv[1:]:",
                    "        shutil.rmtree(build_dir, ignore_errors=True)",
                    "    record = {",
                    "        'target': target,",
                    "        'arguments': sys.argv[1:],",
                    "        'build_dir': str(build_dir),",
                    "    }",
                    "    with Path(os.environ['FAKE_PIO_BUILD_LOG']).open(",
                    "        'a', encoding='utf-8'",
                    "    ) as build_log:",
                    "        build_log.write(json.dumps(record) + '\\n')",
                    "if mode == 'enumerate':",
                    "    sys.stdout.write('Auto-detected: COM3\\n')",
                    "sys.stderr.write('QEMU target cannot be flashed\\n')",
                    "if mode == 'platformio_refuse':",
                    "    sys.stderr.write('=== [FAILED] Took 0.01 seconds ===\\n')",
                    "    raise SystemExit(1)",
                    "raise SystemExit(2)",
                    "",
                )
            ),
            encoding="utf-8",
        )
        return fake_pio


if __name__ == "__main__":
    unittest.main()
