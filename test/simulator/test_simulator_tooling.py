import json
import importlib.util
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


REPO_ROOT = Path(__file__).resolve().parents[2]
CONTAINER_RUNNER = REPO_ROOT / "scripts" / "run_pdf_simulator_container.py"
DOCKERFILE = REPO_ROOT / "docker" / "pdf-simulator" / "Dockerfile"
SMOKE_RUNNER = REPO_ROOT / "scripts" / "run_simulator_smoke_test.py"
PLATFORMIO_CONFIG = REPO_ROOT / "platformio.ini"
ORACLE_SOURCE = (
    REPO_ROOT / "src" / "simulator" / "EpubReflowRegressionOracle.cpp"
)
SMOKE_SOURCE = REPO_ROOT / "src" / "simulator" / "SimulatorSmokeTest.cpp"


class SimulatorToolingTest(unittest.TestCase):
    def test_epub_oracle_is_wired_to_real_reader_contracts(self) -> None:
        oracle = ORACLE_SOURCE.read_text(encoding="utf-8")
        smoke = SMOKE_SOURCE.read_text(encoding="utf-8")

        for required in (
            "Section",
            "streamSection",
            "streamResource",
            "getCapabilities",
            "loadReadingPosition",
            "EpubReaderUtils::saveProgress",
            "EpubReaderUtils::loadProgress",
            "BookmarkStore",
            "getFrameBuffer",
            "SIM_REFLOW_UNCACHED_PASS",
            "SIM_REFLOW_CACHED_PASS",
        ):
            self.assertIn(required, oracle)
        self.assertIn("runEpubReflowRegressionOracle", smoke)

    def test_oracle_marker_parser_and_one_digit_positive_control(self) -> None:
        module = self._load_smoke_runner()
        expected = {
            "frames": {
                "first": {
                    "text": "Opening page",
                    "framebuffer_hash": "0123456789ABCDEF",
                }
            }
        }
        output = (
            "preparation animation\n"
            f"SIM_REFLOW_UNCACHED_PASS {json.dumps(expected)}\n"
            "Simulator smoke test passed\n"
        )
        parsed = module.parse_reflow_oracle_marker(output, "uncached")
        self.assertEqual(parsed, expected)
        module.verify_reflow_oracles(expected, [("uncached", parsed)])

        changed = json.loads(json.dumps(parsed))
        changed["frames"]["first"]["framebuffer_hash"] = (
            "0123456789ABCDE0"
        )
        with self.assertRaisesRegex(
            ValueError, r"frames[.]first[.]framebuffer_hash"
        ):
            module.verify_reflow_oracles(
                expected, [("uncached", changed)]
            )

    def test_single_pass_oracle_does_not_require_a_cached_pass(self) -> None:
        module = self._load_smoke_runner()
        oracle = {"framebuffer_hash": "0123456789ABCDEF"}

        module.verify_reflow_pass_consistency([("uncached", oracle)])

    def test_smoke_runner_exposes_two_pass_oracle_arguments(self) -> None:
        completed = subprocess.run(
            [sys.executable, str(SMOKE_RUNNER), "--help"],
            cwd=REPO_ROOT,
            capture_output=True,
            text=True,
            check=False,
        )
        self.assertEqual(completed.returncode, 0, completed.stderr)
        self.assertIn("--book", completed.stdout)
        self.assertIn("--passes", completed.stdout)
        self.assertIn("--page-turns", completed.stdout)
        self.assertIn("--reflow-oracle", completed.stdout)

    def test_container_is_pinned_and_self_tests_required_tools(self) -> None:
        source = DOCKERFILE.read_text(encoding="utf-8")
        platformio = PLATFORMIO_CONFIG.read_text(encoding="utf-8")
        self.assertIn(
            (
                "ubuntu:24.04@sha256:"
                "4fbb8e6a8395de5a7550b33509421a2bafbc0aab6c06ba2cef9ebffbc7092d90"
            ),
            source,
        )
        for required in (
            "libsdl2-dev",
            "libssl-dev",
            "build-essential",
            "cmake",
            "ninja-build",
            "clang-format-21",
            "fonts-dejavu-core",
            "fontconfig",
            "python3-venv",
            "platformio-core/archive/refs/tags/v6.1.19.zip",
            "crossink-simulator-self-test",
        ):
            self.assertIn(required, source)
        self.assertIn(
            "test -f /usr/include/openssl/md5.h",
            (
                REPO_ROOT
                / "docker"
                / "pdf-simulator"
                / "self-test.sh"
            ).read_text(encoding="utf-8"),
        )
        self_test = (
            REPO_ROOT / "docker" / "pdf-simulator" / "self-test.sh"
        ).read_text(encoding="utf-8")
        self.assertIn("cmake --version", self_test)
        self.assertIn("ninja --version", self_test)
        self.assertIn("-lssl", platformio)
        self.assertIn("-lcrypto", platformio)

    def test_container_runner_self_tests_then_forwards_without_devices(
        self,
    ) -> None:
        with tempfile.TemporaryDirectory() as temporary_directory:
            temporary = Path(temporary_directory)
            invocation_log = temporary / "docker-invocations.jsonl"
            fake_docker = temporary / "fake_docker.py"
            fake_docker.write_text(
                "\n".join(
                    (
                        "import json",
                        "import os",
                        "from pathlib import Path",
                        "import sys",
                        "with Path(os.environ['FAKE_DOCKER_LOG']).open(",
                        "    'a', encoding='utf-8'",
                        ") as output:",
                        "    output.write(json.dumps(sys.argv[1:]) + '\\n')",
                        "",
                    )
                ),
                encoding="utf-8",
            )
            environment = os.environ.copy()
            environment["FAKE_DOCKER_LOG"] = str(invocation_log)

            completed = subprocess.run(
                [
                    sys.executable,
                    str(CONTAINER_RUNNER),
                    "--docker",
                    str(fake_docker),
                    "--image",
                    "crossink-simulator-test",
                    "--build",
                    "--",
                    "python",
                    "scripts/run_simulator_smoke_test.py",
                    "--passes",
                    "2",
                ],
                cwd=REPO_ROOT,
                env=environment,
                capture_output=True,
                text=True,
                check=False,
            )
            self.assertEqual(completed.returncode, 0, completed.stderr)

            invocations = [
                json.loads(line)
                for line in invocation_log.read_text(
                    encoding="utf-8"
                ).splitlines()
            ]
            self.assertEqual(len(invocations), 3)
            self.assertEqual(invocations[0][0], "build")
            self.assertIn("crossink-simulator-self-test", invocations[1])
            self.assertEqual(
                invocations[2][-4:],
                [
                    "python",
                    "scripts/run_simulator_smoke_test.py",
                    "--passes",
                    "2",
                ],
            )
            for invocation in invocations:
                self.assertNotIn("--device", invocation)
                self.assertFalse(
                    any("ttyUSB" in argument or "ttyACM" in argument
                        for argument in invocation)
                )

    @staticmethod
    def _load_smoke_runner():
        spec = importlib.util.spec_from_file_location(
            "run_simulator_smoke_test", SMOKE_RUNNER
        )
        if spec is None or spec.loader is None:
            raise AssertionError("cannot load simulator smoke runner")
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        return module


if __name__ == "__main__":
    unittest.main()
