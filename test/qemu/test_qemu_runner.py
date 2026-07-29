import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time
import unittest


REPO_ROOT = Path(__file__).resolve().parents[2]
RUNNER = REPO_ROOT / "scripts" / "run_qemu_esp32c3.py"
EXPECTED_MARKER = "QEMU_TRACER_PASS"


class QemuRunnerTest(unittest.TestCase):
    def test_pass_marker_terminates_still_running_qemu(self) -> None:
        with tempfile.TemporaryDirectory() as temporary_directory:
            paths = self._create_fake_qemu(Path(temporary_directory))
            started = time.monotonic()
            completed = self._run(paths, "pass", timeout=2.0)
            elapsed = time.monotonic() - started

            self.assertEqual(completed.returncode, 0)
            self.assertEqual(completed.stderr, "")
            self.assertIn(EXPECTED_MARKER, completed.stdout)
            self.assertLess(elapsed, 2.0)
            self.assertEqual(
                paths["log"].read_text(encoding="utf-8"),
                "\n".join(
                    (
                        "QEMU_BOOT seq=0",
                        (
                            "QEMU_STORAGE_PASS "
                            "path=/qemu/sentinel.txt bytes=26"
                        ),
                        (
                            "QEMU_FRAME_PASS "
                            "bytes=48000 crc32=0F7C8C45"
                        ),
                        (
                            "QEMU_INPUT_PASS "
                            "button=DOWN press=1 release=1"
                        ),
                        "QEMU_POWER_PASS idle_ms=3000 saving=1",
                        (
                            "QEMU_RUNTIME heap_start=100000 "
                            "min_free=99000 min_max_alloc=60000 "
                            "max_alloc=1000 stack_margin=2000"
                        ),
                        EXPECTED_MARKER,
                        "",
                    )
                ),
            )

    def test_valid_armed_reset_sequence_passes(self) -> None:
        with tempfile.TemporaryDirectory() as temporary_directory:
            paths = self._create_fake_qemu(Path(temporary_directory))
            completed = self._run(paths, "armed_reset", timeout=2.0)

            self.assertEqual(completed.returncode, 0)
            self.assertEqual(completed.stderr, "")
            self.assertIn("QEMU_EXPECT_RESET seq=0", completed.stdout)
            self.assertIn("QEMU_BOOT seq=1", completed.stdout)

    def test_real_command_uses_private_image_copies_and_no_network(self) -> None:
        with tempfile.TemporaryDirectory() as temporary_directory:
            directory = Path(temporary_directory)
            paths = self._create_fake_qemu(directory)
            flash = directory / "qemu_flash.bin"
            efuse = directory / "qemu_efuse.bin"
            flash.write_bytes(b"flash")
            efuse.write_bytes(b"efuse")
            install = directory / "install.json"
            install.write_text(
                json.dumps(
                    {
                        "version": "test",
                        "executable": str(paths["qemu"].resolve()),
                    }
                ),
                encoding="utf-8",
            )
            manifest = directory / "qemu_manifest.json"
            manifest.write_text(
                json.dumps(
                    {
                        "images": {
                            "flash": {"path": str(flash.resolve())},
                            "efuse": {"path": str(efuse.resolve())},
                        }
                    }
                ),
                encoding="utf-8",
            )
            arguments_log = directory / "arguments.json"
            environment = os.environ.copy()
            environment["FAKE_QEMU_MODE"] = "real_command"
            environment["FAKE_QEMU_ARGS"] = str(arguments_log)

            completed = subprocess.run(
                [
                    sys.executable,
                    str(RUNNER),
                    "--install",
                    str(install),
                    "--manifest",
                    str(manifest),
                    "--expect",
                    EXPECTED_MARKER,
                    "--timeout",
                    "2",
                    "--log",
                    str(paths["log"]),
                ],
                cwd=REPO_ROOT,
                capture_output=True,
                text=True,
                check=False,
                env=environment,
                timeout=4.0,
            )

            self.assertEqual(completed.returncode, 0)
            arguments = json.loads(arguments_log.read_text(encoding="utf-8"))
            self.assertEqual(arguments[0:2], ["-M", "esp32c3"])
            self.assertIn("-icount", arguments)
            self.assertEqual(
                arguments[arguments.index("-icount") + 1],
                "shift=3,sleep=off",
            )
            self.assertIn("-nic", arguments)
            self.assertIn("none", arguments)
            self.assertIn("-nographic", arguments)
            self.assertEqual(arguments[-2:], ["-serial", "mon:stdio"])
            self.assertFalse(
                any("wdt_disable" in argument for argument in arguments)
            )
            drives = [
                argument
                for argument in arguments
                if argument.startswith("file=")
            ]
            self.assertEqual(len(drives), 2)
            copied_paths = [
                Path(argument.split(",", 1)[0].removeprefix("file="))
                for argument in drives
            ]
            self.assertNotIn(flash, copied_paths)
            self.assertNotIn(efuse, copied_paths)
            self.assertTrue(all(not path.exists() for path in copied_paths))
            self.assertEqual(flash.read_bytes(), b"flash")
            self.assertEqual(efuse.read_bytes(), b"efuse")

    def test_failure_modes_are_rejected(self) -> None:
        cases = (
            ("fail_marker", "failure marker"),
            ("panic", "panic"),
            ("guru", "Guru Meditation"),
            ("abort", "abort"),
            ("watchdog", "watchdog"),
            ("restart_loop", "restart loop"),
            ("timeout", "timed out"),
            ("unexpected_exit", "unexpected exit"),
            ("missing_marker", "missing terminal marker"),
            ("unarmed_reset", "unarmed reset"),
            ("repeated_reset", "unarmed reset"),
            ("missing_stage", "missing required tracer marker"),
            ("out_of_order", "out-of-order tracer marker"),
        )
        for mode, expected_error in cases:
            with self.subTest(mode=mode):
                with tempfile.TemporaryDirectory() as temporary_directory:
                    paths = self._create_fake_qemu(Path(temporary_directory))
                    completed = self._run(paths, mode, timeout=0.3)

                    self.assertEqual(completed.returncode, 1)
                    self.assertEqual(completed.stdout.count(EXPECTED_MARKER), 0)
                    self.assertIn(expected_error, completed.stderr)

    def _run(
        self, paths: dict[str, Path], mode: str, *, timeout: float
    ) -> subprocess.CompletedProcess[str]:
        environment = os.environ.copy()
        environment["FAKE_QEMU_MODE"] = mode
        return subprocess.run(
            [
                sys.executable,
                str(RUNNER),
                "--qemu",
                str(paths["qemu"]),
                "--expect",
                EXPECTED_MARKER,
                "--timeout",
                str(timeout),
                "--log",
                str(paths["log"]),
            ],
            cwd=REPO_ROOT,
            capture_output=True,
            text=True,
            check=False,
            env=environment,
            timeout=4.0,
        )

    @staticmethod
    def _create_fake_qemu(directory: Path) -> dict[str, Path]:
        fake_qemu = directory / "fake_qemu.py"
        fake_qemu.write_text(
            "\n".join(
                (
                    "import os",
                    "import sys",
                    "import time",
                    "",
                    "mode = os.environ['FAKE_QEMU_MODE']",
                    "",
                    "def emit(line):",
                    "    print(line, flush=True)",
                    "",
                    "emit('QEMU_BOOT seq=0')",
                    "if mode == 'pass':",
                    (
                        "    emit('QEMU_STORAGE_PASS "
                        "path=/qemu/sentinel.txt bytes=26')"
                    ),
                    (
                        "    emit('QEMU_FRAME_PASS "
                        "bytes=48000 crc32=0F7C8C45')"
                    ),
                    (
                        "    emit('QEMU_INPUT_PASS "
                        "button=DOWN press=1 release=1')"
                    ),
                    (
                        "    emit('QEMU_POWER_PASS "
                        "idle_ms=3000 saving=1')"
                    ),
                    (
                        "    emit('QEMU_RUNTIME heap_start=100000 "
                        "min_free=99000 min_max_alloc=60000 "
                        "max_alloc=1000 stack_margin=2000')"
                    ),
                    "    emit('QEMU_TRACER_PASS')",
                    "    time.sleep(5)",
                    "elif mode == 'armed_reset':",
                    "    emit('QEMU_EXPECT_RESET seq=0')",
                    "    emit('QEMU_BOOT seq=1')",
                    (
                        "    emit('QEMU_STORAGE_PASS "
                        "path=/qemu/sentinel.txt bytes=26')"
                    ),
                    (
                        "    emit('QEMU_FRAME_PASS "
                        "bytes=48000 crc32=0F7C8C45')"
                    ),
                    (
                        "    emit('QEMU_INPUT_PASS "
                        "button=DOWN press=1 release=1')"
                    ),
                    (
                        "    emit('QEMU_POWER_PASS "
                        "idle_ms=3000 saving=1')"
                    ),
                    (
                        "    emit('QEMU_RUNTIME heap_start=100000 "
                        "min_free=99000 min_max_alloc=60000 "
                        "max_alloc=1000 stack_margin=2000')"
                    ),
                    "    emit('QEMU_TRACER_PASS')",
                    "    time.sleep(5)",
                    "elif mode == 'fail_marker':",
                    "    emit('QEMU_STORAGE_FAIL reason=missing')",
                    "    time.sleep(5)",
                    "elif mode == 'panic':",
                    "    emit('panic: simulated fault')",
                    "    time.sleep(5)",
                    "elif mode == 'guru':",
                    "    emit('Guru Meditation Error: simulated fault')",
                    "    time.sleep(5)",
                    "elif mode == 'abort':",
                    "    emit('abort() was called')",
                    "    time.sleep(5)",
                    "elif mode == 'watchdog':",
                    "    emit('Task watchdog got triggered')",
                    "    time.sleep(5)",
                    "elif mode == 'restart_loop':",
                    "    emit('QEMU_BOOT seq=0')",
                    "    time.sleep(5)",
                    "elif mode == 'timeout':",
                    "    time.sleep(5)",
                    "elif mode == 'unexpected_exit':",
                    "    raise SystemExit(7)",
                    "elif mode == 'missing_marker':",
                    "    raise SystemExit(0)",
                    "elif mode == 'unarmed_reset':",
                    "    emit('QEMU_BOOT seq=1')",
                    "    time.sleep(5)",
                    "elif mode == 'repeated_reset':",
                    "    emit('QEMU_EXPECT_RESET seq=0')",
                    "    emit('QEMU_BOOT seq=1')",
                    "    emit('QEMU_BOOT seq=2')",
                    "    time.sleep(5)",
                    "elif mode == 'missing_stage':",
                    (
                        "    emit('QEMU_STORAGE_PASS "
                        "path=/qemu/sentinel.txt bytes=26')"
                    ),
                    (
                        "    emit('QEMU_FRAME_PASS "
                        "bytes=48000 crc32=0F7C8C45')"
                    ),
                    (
                        "    emit('QEMU_INPUT_PASS "
                        "button=DOWN press=1 release=1')"
                    ),
                    "    emit('QEMU_TRACER_PASS')",
                    "    time.sleep(5)",
                    "elif mode == 'out_of_order':",
                    (
                        "    emit('QEMU_FRAME_PASS "
                        "bytes=48000 crc32=0F7C8C45')"
                    ),
                    (
                        "    emit('QEMU_STORAGE_PASS "
                        "path=/qemu/sentinel.txt bytes=26')"
                    ),
                    (
                        "    emit('QEMU_INPUT_PASS "
                        "button=DOWN press=1 release=1')"
                    ),
                    (
                        "    emit('QEMU_POWER_PASS "
                        "idle_ms=3000 saving=1')"
                    ),
                    (
                        "    emit('QEMU_RUNTIME heap_start=100000 "
                        "min_free=99000 min_max_alloc=60000 "
                        "max_alloc=1000 stack_margin=2000')"
                    ),
                    "    emit('QEMU_TRACER_PASS')",
                    "    time.sleep(5)",
                    "elif mode == 'real_command':",
                    "    import json",
                    "    from pathlib import Path",
                    "    Path(os.environ['FAKE_QEMU_ARGS']).write_text(",
                    "        json.dumps(sys.argv[1:]), encoding='utf-8'",
                    "    )",
                    "    for argument in sys.argv[1:]:",
                    "        if argument.startswith('file='):",
                    "            image = Path(argument.split(',', 1)[0][5:])",
                    "            with image.open('ab') as output:",
                    "                output.write(b'changed')",
                    (
                        "    emit('QEMU_STORAGE_PASS "
                        "path=/qemu/sentinel.txt bytes=26')"
                    ),
                    (
                        "    emit('QEMU_FRAME_PASS "
                        "bytes=48000 crc32=0F7C8C45')"
                    ),
                    (
                        "    emit('QEMU_INPUT_PASS "
                        "button=DOWN press=1 release=1')"
                    ),
                    (
                        "    emit('QEMU_POWER_PASS "
                        "idle_ms=3000 saving=1')"
                    ),
                    (
                        "    emit('QEMU_RUNTIME heap_start=100000 "
                        "min_free=99000 min_max_alloc=60000 "
                        "max_alloc=1000 stack_margin=2000')"
                    ),
                    "    emit('QEMU_TRACER_PASS')",
                    "    time.sleep(5)",
                    "else:",
                    "    sys.stderr.write('unknown fake mode\\n')",
                    "    raise SystemExit(8)",
                    "",
                )
            ),
            encoding="utf-8",
        )
        return {"qemu": fake_qemu, "log": directory / "qemu.log"}


if __name__ == "__main__":
    unittest.main()
