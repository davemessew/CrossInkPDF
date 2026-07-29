import argparse
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import platform
import shutil
import stat
import sys
import tarfile
import tempfile
from typing import NamedTuple
import urllib.request


QEMU_VERSION = "esp_develop_9.2.2_20250817"
RELEASE_TAG = "esp-develop-9.2.2-20250817"
RELEASE_ROOT = (
    f"https://github.com/espressif/qemu/releases/download/{RELEASE_TAG}"
)


class Asset(NamedTuple):
    url: str
    sha256: str
    executable_name: str
    platform_key: str


WINDOWS_ASSET = Asset(
    url=(
        f"{RELEASE_ROOT}/"
        "qemu-riscv32-softmmu-esp_develop_9.2.2_20250817-"
        "x86_64-w64-mingw32.tar.xz"
    ),
    sha256="9474015f24d27acb7516955ec932e5307226bd9d6652cdc870793ed36010ab73",
    executable_name="qemu-system-riscv32.exe",
    platform_key="windows-x86_64",
)
LINUX_ASSET = Asset(
    url=(
        f"{RELEASE_ROOT}/"
        "qemu-riscv32-softmmu-esp_develop_9.2.2_20250817-"
        "x86_64-linux-gnu.tar.xz"
    ),
    sha256="373b37a68bae3ef441ead24a7bfc950fcbfc274cbdd2b628fc6915f179eb1d8e",
    executable_name="qemu-system-riscv32",
    platform_key="linux-x86_64",
)


def select_asset(system: str, machine: str) -> Asset:
    normalized_system = system.lower()
    normalized_machine = machine.lower()
    is_x86_64 = normalized_machine in {"amd64", "x86_64"}
    if normalized_system == "windows" and is_x86_64:
        return WINDOWS_ASSET
    if normalized_system == "linux" and is_x86_64:
        return LINUX_ASSET
    raise RuntimeError(
        f"unsupported QEMU host: {system} {machine}"
    )


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        while chunk := source.read(1024 * 1024):
            digest.update(chunk)
    return digest.hexdigest()


def _validate_archive_members(members: list[tarfile.TarInfo]) -> None:
    for member in members:
        normalized_name = member.name.replace("\\", "/")
        path = PurePosixPath(normalized_name)
        if path.is_absolute() or ".." in path.parts:
            raise RuntimeError(
                f"unsafe archive path: {member.name}"
            )
        if member.issym() or member.islnk():
            link = PurePosixPath(member.linkname.replace("\\", "/"))
            if link.is_absolute() or ".." in link.parts:
                raise RuntimeError(
                    f"unsafe archive link: {member.linkname}"
                )
        if member.isdev():
            raise RuntimeError(
                f"unsafe archive device: {member.name}"
            )


def _locate_package_root(
    extracted: Path, executable_name: str
) -> tuple[Path, Path]:
    candidates = [
        path
        for path in extracted.rglob(executable_name)
        if path.is_file() and path.parent.name == "bin"
    ]
    if len(candidates) != 1:
        raise RuntimeError(
            f"archive contains {len(candidates)} matching QEMU executables"
        )
    executable = candidates[0]
    return executable.parent.parent, executable


def install_from_archive(
    archive: Path, install_root: Path, asset: Asset
) -> Path:
    actual_hash = _sha256(archive)
    if actual_hash != asset.sha256:
        raise RuntimeError(
            f"QEMU archive SHA-256 mismatch: {actual_hash}"
        )

    install_root = install_root.resolve()
    install_root.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(
        prefix="qemu-install-", dir=install_root.parent
    ) as temporary_directory:
        temporary = Path(temporary_directory)
        extracted = temporary / "extracted"
        extracted.mkdir()
        try:
            with tarfile.open(archive, "r:xz") as bundle:
                members = bundle.getmembers()
                _validate_archive_members(members)
                bundle.extractall(extracted, members=members, filter="data")
        except (OSError, tarfile.TarError) as error:
            raise RuntimeError(
                f"cannot extract QEMU archive: {error}"
            ) from error

        package_root, original_executable = _locate_package_root(
            extracted, asset.executable_name
        )
        staged_qemu = temporary / "qemu"
        shutil.copytree(package_root, staged_qemu)
        relative_executable = original_executable.relative_to(package_root)

        install_root.mkdir(parents=True, exist_ok=True)
        destination_qemu = install_root / "qemu"
        if destination_qemu.exists():
            shutil.rmtree(destination_qemu)
        shutil.move(str(staged_qemu), str(destination_qemu))

    executable = destination_qemu / relative_executable
    if os.name != "nt":
        executable.chmod(
            executable.stat().st_mode
            | stat.S_IXUSR
            | stat.S_IXGRP
            | stat.S_IXOTH
        )
    receipt = {
        "schema_version": 1,
        "version": QEMU_VERSION,
        "platform": asset.platform_key,
        "url": asset.url,
        "sha256": asset.sha256,
        "executable": str(executable.resolve()),
    }
    (install_root / "install.json").write_text(
        json.dumps(receipt, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )
    return executable


def _download(url: str, destination: Path) -> None:
    request = urllib.request.Request(
        url, headers={"User-Agent": "CrossInk-QEMU-installer"}
    )
    with urllib.request.urlopen(request, timeout=60) as response:
        with destination.open("wb") as output:
            shutil.copyfileobj(response, output, length=1024 * 1024)


def _default_install_root() -> Path:
    return (
        Path(__file__).resolve().parents[1]
        / ".tools"
        / "qemu-esp32c3"
    )


def _build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--install-root",
        type=Path,
        default=_default_install_root(),
    )
    parser.add_argument(
        "--archive",
        type=Path,
        help="Use an already-downloaded pinned archive",
    )
    return parser


def main() -> int:
    arguments = _build_parser().parse_args()
    try:
        asset = select_asset(platform.system(), platform.machine())
        if arguments.archive is not None:
            executable = install_from_archive(
                arguments.archive.resolve(),
                arguments.install_root,
                asset,
            )
        else:
            arguments.install_root.parent.mkdir(
                parents=True, exist_ok=True
            )
            with tempfile.TemporaryDirectory(
                prefix="qemu-download-",
                dir=arguments.install_root.parent,
            ) as temporary_directory:
                archive = Path(temporary_directory) / "qemu.tar.xz"
                _download(asset.url, archive)
                executable = install_from_archive(
                    archive, arguments.install_root, asset
                )
    except (OSError, RuntimeError) as error:
        sys.stderr.write(f"QEMU install failed: {error}\n")
        return 1
    sys.stdout.write(f"QEMU_INSTALL_PASS {executable}\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
