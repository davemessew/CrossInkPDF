import hashlib
import importlib.util
import json
from pathlib import Path
import tarfile
import tempfile
import unittest


REPO_ROOT = Path(__file__).resolve().parents[2]
INSTALLER = REPO_ROOT / "scripts" / "install_qemu_esp32c3.py"


class QemuInstallerTest(unittest.TestCase):
    def test_pinned_assets_match_espressif_release(self) -> None:
        module = self._load_installer()
        windows = module.select_asset("Windows", "AMD64")
        linux = module.select_asset("Linux", "x86_64")

        self.assertEqual(module.QEMU_VERSION, "esp_develop_9.2.2_20250817")
        self.assertEqual(
            windows.sha256,
            "9474015f24d27acb7516955ec932e5307226bd9d6652cdc870793ed36010ab73",
        )
        self.assertTrue(windows.url.endswith("x86_64-w64-mingw32.tar.xz"))
        self.assertEqual(
            linux.sha256,
            "373b37a68bae3ef441ead24a7bfc950fcbfc274cbdd2b628fc6915f179eb1d8e",
        )
        self.assertTrue(linux.url.endswith("x86_64-linux-gnu.tar.xz"))
        with self.assertRaisesRegex(RuntimeError, "unsupported"):
            module.select_asset("Darwin", "arm64")

    def test_checksum_mismatch_is_rejected_before_extraction(self) -> None:
        module = self._load_installer()
        with tempfile.TemporaryDirectory() as temporary_directory:
            archive = Path(temporary_directory) / "qemu.tar.xz"
            archive.write_bytes(b"not the pinned archive")
            with self.assertRaisesRegex(RuntimeError, "SHA-256"):
                module.install_from_archive(
                    archive,
                    Path(temporary_directory) / "install",
                    module.Asset(
                        url="https://example.invalid/qemu.tar.xz",
                        sha256="0" * 64,
                        executable_name="qemu-system-riscv32",
                        platform_key="test",
                    ),
                )

    def test_archive_is_safely_normalized_and_receipted(self) -> None:
        module = self._load_installer()
        with tempfile.TemporaryDirectory() as temporary_directory:
            directory = Path(temporary_directory)
            payload = directory / "payload"
            binary = payload / "package" / "qemu" / "bin" / "qemu-system-riscv32"
            binary.parent.mkdir(parents=True)
            binary.write_bytes(b"qemu")
            (binary.parent / "helper.dll").write_bytes(b"dll")
            archive = directory / "qemu.tar.xz"
            with tarfile.open(archive, "w:xz") as bundle:
                bundle.add(payload / "package", arcname="package")
            digest = hashlib.sha256(archive.read_bytes()).hexdigest()
            asset = module.Asset(
                url="https://example.invalid/qemu.tar.xz",
                sha256=digest,
                executable_name="qemu-system-riscv32",
                platform_key="test",
            )

            install_root = directory / "install"
            executable = module.install_from_archive(
                archive, install_root, asset
            )

            self.assertEqual(
                executable,
                install_root / "qemu" / "bin" / "qemu-system-riscv32",
            )
            self.assertEqual(executable.read_bytes(), b"qemu")
            self.assertEqual(
                (executable.parent / "helper.dll").read_bytes(), b"dll"
            )
            receipt = json.loads(
                (install_root / "install.json").read_text(encoding="utf-8")
            )
            self.assertEqual(receipt["version"], module.QEMU_VERSION)
            self.assertEqual(receipt["sha256"], digest)
            self.assertEqual(
                Path(receipt["executable"]), executable.resolve()
            )

    def test_archive_path_traversal_is_rejected(self) -> None:
        module = self._load_installer()
        with tempfile.TemporaryDirectory() as temporary_directory:
            directory = Path(temporary_directory)
            archive = directory / "qemu.tar.xz"
            malicious = tarfile.TarInfo("../outside")
            malicious.size = 0
            with tarfile.open(archive, "w:xz") as bundle:
                bundle.addfile(malicious)
            digest = hashlib.sha256(archive.read_bytes()).hexdigest()
            asset = module.Asset(
                url="https://example.invalid/qemu.tar.xz",
                sha256=digest,
                executable_name="qemu-system-riscv32",
                platform_key="test",
            )
            with self.assertRaisesRegex(RuntimeError, "unsafe archive"):
                module.install_from_archive(
                    archive, directory / "install", asset
                )

    @staticmethod
    def _load_installer():
        if not INSTALLER.is_file():
            raise AssertionError(f"missing required file: {INSTALLER}")
        spec = importlib.util.spec_from_file_location(
            "install_qemu_esp32c3", INSTALLER
        )
        if spec is None or spec.loader is None:
            raise AssertionError("cannot load QEMU installer module")
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        return module


if __name__ == "__main__":
    unittest.main()
