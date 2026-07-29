import configparser
import hashlib
import importlib.util
from pathlib import Path
import tempfile
import unittest


REPO_ROOT = Path(__file__).resolve().parents[2]
PLATFORMIO_INI = REPO_ROOT / "platformio.ini"
BUILD_SCRIPT = REPO_ROOT / "scripts" / "qemu_build.py"
NO_FLASH_SCRIPT = REPO_ROOT / "scripts" / "qemu_no_flash.py"
EXPECTED_OFFSETS = {
    "bootloader": 0x0,
    "partitions": 0x8000,
    "otadata": 0xE000,
    "application": 0x10000,
    "filesystem": 0xC90000,
}


class QemuBuildContractTest(unittest.TestCase):
    def test_platformio_environment_isolated_from_physical_hardware(self) -> None:
        parser = configparser.RawConfigParser(interpolation=None, strict=False)
        parser.read(PLATFORMIO_INI, encoding="utf-8")
        self.assertTrue(parser.has_section("env:qemu-esp32c3"))
        section = parser["env:qemu-esp32c3"]

        self.assertEqual(section["extends"].strip(), "base")
        self.assertEqual(section["upload_protocol"].strip(), "custom")
        self.assertEqual(section["board_build.filesystem"].strip(), "littlefs")
        self.assertEqual(section["lib_ignore"].strip(), "hal")

        flags = section["build_flags"]
        self.assertIn("${base.build_flags}", flags)
        self.assertIn("-DCROSSINK_QEMU=1", flags)
        self.assertIn('CROSSPOINT_FIRMWARE_VARIANT=\\"tiny\\"', flags)
        for omission in (
            "OMIT_TEENSY_FONT",
            "OMIT_ITTY_BITTY_FONT",
            "OMIT_XLARGE_FONT",
            "OMIT_HUGE_FONT",
        ):
            self.assertIn(omission, flags)
        self.assertNotIn("-DSIMULATOR", flags)

        unflags = section["build_unflags"]
        self.assertIn("${base.build_unflags}", unflags)
        self.assertIn("-DARDUINO_USB_MODE=1", unflags)
        self.assertIn("-DARDUINO_USB_CDC_ON_BOOT=1", unflags)

        dependencies = section["lib_deps"]
        self.assertIn("qemu-hal=symlink://test/qemu/hal", dependencies)
        for physical_dependency in (
            "BatteryMonitor",
            "InputManager",
            "EInkDisplay",
            "SDCardManager",
            "BoardConfig",
            "XteinkDetect",
            "PowerManager",
        ):
            self.assertNotIn(physical_dependency, dependencies)

        scripts = [
            line.strip()
            for line in section["extra_scripts"].splitlines()
            if line.strip()
        ]
        self.assertEqual(scripts[0], "pre:scripts/qemu_no_flash.py")
        self.assertIn("post:scripts/qemu_build.py", scripts)

    def test_no_flash_hook_guards_all_unsafe_targets_before_upload(self) -> None:
        self.assertTrue(
            NO_FLASH_SCRIPT.is_file(),
            f"missing required file: {NO_FLASH_SCRIPT}",
        )
        source = NO_FLASH_SCRIPT.read_text(encoding="utf-8")
        for target in (
            "upload",
            "uploadfs",
            "uploadfsota",
            "erase",
            "erase_upload",
            "download_fs",
        ):
            self.assertIn(f'"{target}"', source)
        self.assertIn("COMMAND_LINE_TARGETS", source)
        self.assertIn("refuse_qemu_flash.py", source)
        self.assertIn("sys.executable", source)
        for forbidden in ("serial", "esptool", "write_flash", "/dev/tty"):
            self.assertNotIn(forbidden, source.lower())

    def test_image_layout_efuse_and_offline_merge_are_exact(self) -> None:
        module = self._load_build_module()
        with tempfile.TemporaryDirectory() as temporary_directory:
            directory = Path(temporary_directory)
            entries = {
                name: (offset, directory / f"{name}.bin")
                for name, offset in EXPECTED_OFFSETS.items()
            }
            for _, image in entries.values():
                image.write_bytes(b"image")

            validated = module.validate_flash_layout(entries)
            self.assertEqual(
                {name: offset for name, (offset, _) in validated.items()},
                EXPECTED_OFFSETS,
            )

            efuse = directory / "qemu_efuse.bin"
            module.generate_esp32c3_efuse(efuse)
            efuse_bytes = efuse.read_bytes()
            self.assertEqual(len(efuse_bytes), 1024)
            self.assertEqual(
                hashlib.sha256(efuse_bytes).hexdigest(),
                "2054600a17c72426ac024ae851e7ea26f9cf612f31140b445ff713ba15ac09c8",
            )
            self.assertEqual(
                [(index, value) for index, value in enumerate(efuse_bytes) if value],
                [(38, 0x0C)],
            )

            command = module.build_merge_command(
                Path("python"),
                Path("esptool.py"),
                directory / "qemu_flash.bin",
                entries,
            )
            self.assertIn("merge_bin", command)
            self.assertNotIn("write_flash", command)
            self.assertNotIn("--port", command)
            for offset in EXPECTED_OFFSETS.values():
                self.assertIn(hex(offset), command)

    def test_layout_and_fixture_headroom_reject_one_byte_overflow(self) -> None:
        module = self._load_build_module()
        with tempfile.TemporaryDirectory() as temporary_directory:
            directory = Path(temporary_directory)
            entries = {
                name: (offset, directory / f"{name}.bin")
                for name, offset in EXPECTED_OFFSETS.items()
            }
            for _, image in entries.values():
                image.write_bytes(b"image")
            entries["filesystem"] = (0xC90001, entries["filesystem"][1])
            with self.assertRaisesRegex(ValueError, "filesystem"):
                module.validate_flash_layout(entries)

            fixtures = directory / "fixtures"
            fixtures.mkdir()
            partition_size = 0x360000
            headroom = module.MIN_WRITABLE_HEADROOM
            (fixtures / "fits.bin").write_bytes(
                b"x" * (partition_size - headroom)
            )
            self.assertEqual(
                module.verify_fixture_headroom(fixtures),
                partition_size - headroom,
            )
            (fixtures / "one-more-byte.bin").write_bytes(b"x")
            with self.assertRaisesRegex(ValueError, "headroom"):
                module.verify_fixture_headroom(fixtures)

    def test_scons_registration_uses_platform_builders_and_manifest(self) -> None:
        self.assertTrue(
            BUILD_SCRIPT.is_file(), f"missing required file: {BUILD_SCRIPT}"
        )
        source = BUILD_SCRIPT.read_text(encoding="utf-8")
        self.assertIn('env.DataToBin("$BUILD_DIR/qemu-data"', source)
        self.assertIn('"$PROJECT_DIR/test/qemu/data"', source)
        self.assertIn("$BUILD_DIR/${PROGNAME}.bin", source)
        self.assertIn('name="qemu-image"', source)
        self.assertIn("setup_python_env(env)", source)
        self.assertIn("FLASH_EXTRA_IMAGES", source)
        self.assertIn("qemu_manifest.json", source)
        self.assertIn("riscv32-esp-elf-size", source)

    @staticmethod
    def _load_build_module():
        if not BUILD_SCRIPT.is_file():
            raise AssertionError(f"missing required file: {BUILD_SCRIPT}")
        spec = importlib.util.spec_from_file_location("qemu_build", BUILD_SCRIPT)
        if spec is None or spec.loader is None:
            raise AssertionError("cannot load qemu_build module")
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        return module


if __name__ == "__main__":
    unittest.main()
