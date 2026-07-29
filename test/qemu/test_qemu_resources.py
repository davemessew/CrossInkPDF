import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


REPO_ROOT = Path(__file__).resolve().parents[2]
CHECKER = REPO_ROOT / "scripts" / "check_qemu_resources.py"
PASS_MARKER = "QEMU_RESOURCE_PASS\n"


class QemuResourceCheckTest(unittest.TestCase):
    def test_capture_and_verify_use_worst_boot_measurements(self) -> None:
        with tempfile.TemporaryDirectory() as temporary_directory:
            paths = self._create_inputs(Path(temporary_directory))
            paths["runtime"].write_text(
                "\n".join(
                    (
                        "QEMU_BOOT seq=0",
                        (
                            "QEMU_RUNTIME heap_start=100000 min_free=70000 "
                            "min_max_alloc=50000 max_alloc=2000 stack_margin=1500"
                        ),
                        "QEMU_BOOT seq=1",
                        (
                            "QEMU_RUNTIME heap_start=100000 min_free=90000 "
                            "min_max_alloc=80000 max_alloc=1000 stack_margin=3000"
                        ),
                        "",
                    )
                ),
                encoding="utf-8",
            )
            environment = self._size_environment(text=1000, static_dram=100)

            captured = self._run_capture(paths, environment)
            self.assertEqual(captured.returncode, 0)
            self.assertEqual(captured.stdout, PASS_MARKER)
            self.assertEqual(captured.stderr, "")

            baseline = json.loads(paths["baseline"].read_text(encoding="utf-8"))
            self.assertEqual(
                baseline["measurements"],
                {
                    "code_rodata": 1000,
                    "static_dram": 100,
                    "peak_heap": 30000,
                    "min_free_heap": 70000,
                    "min_largest_block": 50000,
                    "max_allocation": 2000,
                    "min_stack_margin": 1500,
                },
            )

            verified = self._run_verify(paths, environment)
            self.assertEqual(verified.returncode, 0)
            self.assertEqual(verified.stdout, PASS_MARKER)
            self.assertEqual(verified.stderr, "")

    def test_verify_rejects_environment_fingerprint_drift(self) -> None:
        with tempfile.TemporaryDirectory() as temporary_directory:
            paths = self._create_inputs(Path(temporary_directory))
            environment = self._size_environment(text=1000, static_dram=100)
            self.assertEqual(self._run_capture(paths, environment).returncode, 0)

            manifest = json.loads(paths["manifest"].read_text(encoding="utf-8"))
            manifest["resource_fingerprint"]["framework"] = "changed"
            paths["manifest"].write_text(
                json.dumps(manifest), encoding="utf-8"
            )

            verified = self._run_verify(paths, environment)
            self.assertEqual(verified.returncode, 1)
            self.assertEqual(verified.stdout, "")
            self.assertIn("resource fingerprint differs", verified.stderr)

    def test_verify_rejects_each_one_byte_boundary_violation(self) -> None:
        with tempfile.TemporaryDirectory() as temporary_directory:
            paths = self._create_inputs(Path(temporary_directory))
            baseline_environment = self._size_environment(
                text=1000, static_dram=100
            )
            self.assertEqual(
                self._run_capture(paths, baseline_environment).returncode, 0
            )

            violations = (
                {
                    "name": "text",
                    "text": 1000 + 262145,
                    "static_dram": 100,
                    "runtime": self._runtime_line(),
                },
                {
                    "name": "static_dram",
                    "text": 1000,
                    "static_dram": 100 + 12289,
                    "runtime": self._runtime_line(),
                },
                {
                    "name": "pdf_heap",
                    "text": 1000,
                    "static_dram": 100,
                    "runtime": self._runtime_line(
                        heap_start=152921, min_free=70000
                    ),
                },
                {
                    "name": "free_heap",
                    "text": 1000,
                    "static_dram": 100,
                    "runtime": self._runtime_line(
                        heap_start=66535, min_free=65535
                    ),
                },
                {
                    "name": "largest_block",
                    "text": 1000,
                    "static_dram": 100,
                    "runtime": self._runtime_line(min_max_alloc=49151),
                },
                {
                    "name": "allocation",
                    "text": 1000,
                    "static_dram": 100,
                    "runtime": self._runtime_line(max_alloc=32769),
                },
                {
                    "name": "stack_margin",
                    "text": 1000,
                    "static_dram": 100,
                    "runtime": self._runtime_line(stack_margin=1023),
                },
            )

            for violation in violations:
                with self.subTest(boundary=violation["name"]):
                    paths["runtime"].write_text(
                        "QEMU_BOOT seq=0\n" + violation["runtime"] + "\n",
                        encoding="utf-8",
                    )
                    environment = self._size_environment(
                        text=violation["text"],
                        static_dram=violation["static_dram"],
                    )
                    verified = self._run_verify(paths, environment)
                    self.assertEqual(verified.returncode, 1)
                    self.assertEqual(verified.stdout, "")
                    self.assertIn(violation["name"], verified.stderr)

    def test_capture_rejects_relative_size_tool_path(self) -> None:
        with tempfile.TemporaryDirectory() as temporary_directory:
            paths = self._create_inputs(Path(temporary_directory))
            manifest = json.loads(paths["manifest"].read_text(encoding="utf-8"))
            manifest["tools"]["size"]["path"] = "fake_size.py"
            paths["manifest"].write_text(
                json.dumps(manifest), encoding="utf-8"
            )

            captured = self._run_capture(
                paths, self._size_environment(text=1000, static_dram=100)
            )
            self.assertEqual(captured.returncode, 1)
            self.assertEqual(captured.stdout, "")
            self.assertIn("absolute", captured.stderr)

    def _create_inputs(self, directory: Path) -> dict[str, Path]:
        fake_size = directory / "fake_size.py"
        fake_size.write_text(
            "\n".join(
                (
                    "import os",
                    "import sys",
                    "",
                    "if len(sys.argv) != 3 or sys.argv[1] != '-A':",
                    "    raise SystemExit(9)",
                    "text = int(os.environ['QEMU_TEST_TEXT_SIZE'])",
                    "dram = int(os.environ['QEMU_TEST_DRAM_SIZE'])",
                    "print(f'{sys.argv[2]} :')",
                    "print('section size addr')",
                    "print('.iram0.text 0 0x0')",
                    "print('.iram0.vectors 0 0x0')",
                    "print(f'.flash.text {text} 0x0')",
                    "print('.flash.rodata 0 0x0')",
                    "print(f'.dram0.data {dram} 0x0')",
                    "print('.dram0.bss 0 0x0')",
                    "print('.noinit 0 0x0')",
                    "print('.ignored 999999 0x0')",
                    "",
                )
            ),
            encoding="utf-8",
        )
        manifest = directory / "qemu_manifest.json"
        manifest.write_text(
            json.dumps(
                {
                    "resource_fingerprint": {
                        "toolchain": "riscv32-esp-elf-14.2.0",
                        "platform": "55.03.37",
                        "framework": "arduino-3.3.7",
                        "build_flags": [
                            " -fno-exceptions ",
                            "-std=gnu++20",
                        ],
                        "partition_sha256": "partition-hash",
                        "qemu_hal_sha256": "hal-hash",
                        "qemu_config_sha256": "config-hash",
                    },
                    "tools": {
                        "size": {
                            "path": str(fake_size.resolve()),
                            "version": "fake-size-1",
                        }
                    },
                }
            ),
            encoding="utf-8",
        )
        elf = directory / "firmware.elf"
        elf.write_bytes(b"ELF")
        runtime = directory / "runtime.log"
        runtime.write_text(
            "QEMU_BOOT seq=0\n" + self._runtime_line() + "\n",
            encoding="utf-8",
        )
        return {
            "manifest": manifest,
            "elf": elf,
            "runtime": runtime,
            "baseline": directory / "baseline.json",
        }

    @staticmethod
    def _runtime_line(
        *,
        heap_start: int = 100000,
        min_free: int = 99000,
        min_max_alloc: int = 60000,
        max_alloc: int = 1000,
        stack_margin: int = 2000,
    ) -> str:
        return (
            f"QEMU_RUNTIME heap_start={heap_start} min_free={min_free} "
            f"min_max_alloc={min_max_alloc} max_alloc={max_alloc} "
            f"stack_margin={stack_margin}"
        )

    @staticmethod
    def _size_environment(*, text: int, static_dram: int) -> dict[str, str]:
        environment = os.environ.copy()
        environment["QEMU_TEST_TEXT_SIZE"] = str(text)
        environment["QEMU_TEST_DRAM_SIZE"] = str(static_dram)
        return environment

    @staticmethod
    def _run_capture(
        paths: dict[str, Path], environment: dict[str, str]
    ) -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            [
                sys.executable,
                str(CHECKER),
                "capture",
                "--manifest",
                str(paths["manifest"]),
                "--elf",
                str(paths["elf"]),
                "--runtime-log",
                str(paths["runtime"]),
                "--out",
                str(paths["baseline"]),
            ],
            cwd=REPO_ROOT,
            capture_output=True,
            text=True,
            check=False,
            env=environment,
        )

    @staticmethod
    def _run_verify(
        paths: dict[str, Path], environment: dict[str, str]
    ) -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            [
                sys.executable,
                str(CHECKER),
                "verify",
                "--baseline",
                str(paths["baseline"]),
                "--manifest",
                str(paths["manifest"]),
                "--elf",
                str(paths["elf"]),
                "--runtime-log",
                str(paths["runtime"]),
            ],
            cwd=REPO_ROOT,
            capture_output=True,
            text=True,
            check=False,
            env=environment,
        )


if __name__ == "__main__":
    unittest.main()
