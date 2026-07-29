#!/usr/bin/env python3
"""Build and run the pinned Linux SDL simulator container."""

from __future__ import annotations

import argparse
from pathlib import Path
import subprocess
import sys
import tempfile


ROOT = Path(__file__).resolve().parents[1]
DOCKERFILE = ROOT / "docker" / "pdf-simulator" / "Dockerfile"
DEFAULT_IMAGE = "crossink-pdf-simulator:ubuntu24.04-sdl2-v1"
CACHE_DIR = ROOT / ".tools" / "pdf-simulator-platformio"


def _docker_command(executable: str) -> list[str]:
    path = Path(executable)
    if path.suffix.lower() == ".py" and path.is_file():
        return [sys.executable, str(path)]
    return [executable]


def _run(command: list[str]) -> int:
    completed = subprocess.run(command, cwd=ROOT, check=False)
    return completed.returncode


def _container_command(
    docker: list[str],
    image: str,
    test_filesystem: Path,
    command: list[str],
) -> list[str]:
    return docker + [
        "run",
        "--rm",
        "--mount",
        f"type=bind,source={ROOT.resolve()},target=/workspace",
        "--mount",
        (
            "type=bind,"
            f"source={CACHE_DIR.resolve()},"
            "target=/home/ubuntu/.platformio"
        ),
        "--mount",
        (
            "type=bind,"
            f"source={test_filesystem.resolve()},"
            "target=/crossink-test-fs"
        ),
        "--workdir",
        "/workspace",
        "--env",
        "CROSSINK_SIMULATOR_TEST_FS=/crossink-test-fs",
        image,
    ] + command


def run(arguments: argparse.Namespace) -> int:
    docker = _docker_command(arguments.docker)
    if arguments.build:
        result = _run(
            docker
            + [
                "build",
                "--file",
                str(DOCKERFILE),
                "--tag",
                arguments.image,
                str(ROOT),
            ]
        )
        if result != 0:
            return result

    CACHE_DIR.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(
        prefix="crossink-simulator-mount-test-"
    ) as temporary_directory:
        test_filesystem = Path(temporary_directory)
        result = _run(
            _container_command(
                docker,
                arguments.image,
                test_filesystem,
                ["crossink-simulator-self-test"],
            )
        )
        if result != 0 or not arguments.command:
            return result
        return _run(
            _container_command(
                docker,
                arguments.image,
                test_filesystem,
                arguments.command,
            )
        )


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", action="store_true")
    parser.add_argument("--docker", default="docker")
    parser.add_argument("--image", default=DEFAULT_IMAGE)
    parser.add_argument("command", nargs=argparse.REMAINDER)
    arguments = parser.parse_args()
    if arguments.command[:1] == ["--"]:
        arguments.command = arguments.command[1:]
    return arguments


def main() -> int:
    return run(parse_args())


if __name__ == "__main__":
    raise SystemExit(main())
