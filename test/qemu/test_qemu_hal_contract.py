import json
from pathlib import Path
import re
import unittest


REPO_ROOT = Path(__file__).resolve().parents[2]
PRODUCTION_HAL = REPO_ROOT / "lib" / "hal"
QEMU_HAL = REPO_ROOT / "test" / "qemu" / "hal"
QEMU_SRC = QEMU_HAL / "src"
PUBLIC_HEADERS = (
    "HalClock.h",
    "HalDisplay.h",
    "HalGPIO.h",
    "HalPowerManager.h",
    "HalSpiBus.h",
    "HalStorage.h",
    "HalSystem.h",
    "HalTiltSensor.h",
)
PHYSICAL_SDK_TOKENS = (
    "BatteryMonitor.h",
    "EInkDisplay.h",
    "InputManager.h",
    "SDCardManager.h",
    "SdFat.h",
    "esp_sleep.h",
)
CONTROL_FUNCTIONS = {
    "frameCrc32",
    "storageOpenCount",
    "storageCloseCount",
    "setStorageQuota",
    "storageQuota",
    "storageCapacity",
    "storageFree",
    "powerSavingEnabled",
}


class QemuHalContractTest(unittest.TestCase):
    def test_qemu_hal_mirrors_the_live_public_contract(self) -> None:
        missing = [
            header for header in PUBLIC_HEADERS if not (QEMU_SRC / header).is_file()
        ]
        self.assertEqual(missing, [], f"missing mirrored headers: {missing}")

        for header in PUBLIC_HEADERS:
            with self.subTest(header=header):
                production_api = self._api_contract(
                    (PRODUCTION_HAL / header).read_text(encoding="utf-8")
                )
                qemu_api = self._api_contract(
                    (QEMU_SRC / header).read_text(encoding="utf-8")
                )
                self.assertTrue(
                    production_api.issubset(qemu_api),
                    f"missing API tokens: {sorted(production_api - qemu_api)}",
                )

    def test_qemu_hal_has_one_static_framebuffer_and_no_physical_tasks(self) -> None:
        sources = self._all_qemu_source()
        framebuffer_declarations = re.findall(
            r"\b(?:inline\s+)?static\s+uint8_t\s+framebuffer"
            r"\s*\[\s*48000\s*\]",
            sources,
        )
        self.assertEqual(len(framebuffer_declarations), 1)
        self.assertNotRegex(sources, r"\bxTaskCreate(?:PinnedToCore)?\b")
        self.assertNotRegex(sources, r"\bmalloc\s*\(")
        self.assertNotRegex(sources, r"\bnew(?:\s*\[|\s+[A-Za-z_(])")
        for token in PHYSICAL_SDK_TOKENS:
            self.assertNotIn(token, sources)

    def test_storage_display_power_and_control_contracts_are_bounded(self) -> None:
        storage = self._read_required(QEMU_SRC / "HalStorage.cpp")
        display = self._read_required(QEMU_SRC / "HalDisplay.cpp")
        control = self._read_required(QEMU_SRC / "QemuHalControl.h")

        self.assertRegex(
            storage,
            r"LittleFS\.begin\([^;]*\"spiffs\"\)",
        )
        self.assertIn("64ULL * 1024ULL * 1024ULL", storage)
        self.assertIn("32ULL * 1024ULL * 1024ULL", storage)
        self.assertIn("seek64", storage)
        self.assertIn("UINT32_MAX", storage)
        self.assertIn("storageQuota", storage)
        self.assertIn("storageOpenCount", storage)
        self.assertIn("storageCloseCount", storage)
        self.assertIn("frameCrc32", display)
        self.assertIn("powerSavingEnabled", self._all_qemu_source())

        declared_controls = set(
            re.findall(r"\b(?:bool|void|uint32_t|uint64_t)\s+(\w+)\s*\(", control)
        )
        self.assertEqual(declared_controls, CONTROL_FUNCTIONS)

    def test_library_manifest_and_sentinel_are_deterministic(self) -> None:
        manifest = json.loads(self._read_required(QEMU_HAL / "library.json"))
        self.assertEqual(manifest["name"], "qemu-hal")
        self.assertEqual(manifest["frameworks"], "arduino")
        self.assertEqual(manifest["platforms"], "espressif32")
        self.assertTrue((QEMU_SRC / "common" / "FsApiConstants.h").is_file())
        sentinel = (
            REPO_ROOT / "test" / "qemu" / "data" / "qemu" / "sentinel.txt"
        )
        self.assertTrue(sentinel.is_file(), f"missing required file: {sentinel}")
        self.assertEqual(
            sentinel.read_bytes(),
            b"crossink-qemu-sentinel-v1\n",
        )

    @staticmethod
    def _api_contract(source: str) -> set[str]:
        without_comments = re.sub(r"/\*.*?\*/|//[^\n]*", "", source, flags=re.S)
        types = {
            " ".join(match.split())
            for match in re.findall(
                r"\b(?:class|struct|namespace|enum(?:\s+class)?)\s+[A-Za-z_]\w*",
                without_comments,
            )
        }
        callables = set()
        declaration_pattern = re.compile(
            r"(?m)^[ \t]*(?:inline\s+)?(?:explicit\s+)?"
            r"(?:[A-Za-z_][\w:<>]*[\s*&]+)*"
            r"(?:~?[A-Za-z_]\w*|operator(?:=| bool))"
            r"\s*\([^;{}]*?\)\s*(?:const\s*)?(?:override\s*)?"
            r"(?:=\s*(?:delete|default)\s*)?(?:;|\{)"
        )
        for match in declaration_pattern.finditer(without_comments):
            signature = match.group(0).rstrip("{;")
            callables.add(" ".join(signature.split()))
        return types | callables

    def _read_required(self, path: Path) -> str:
        self.assertTrue(path.is_file(), f"missing required file: {path}")
        return path.read_text(encoding="utf-8")

    @staticmethod
    def _all_qemu_source() -> str:
        source_files = sorted(QEMU_SRC.rglob("*.h")) + sorted(
            QEMU_SRC.rglob("*.cpp")
        )
        return "\n".join(
            source_file.read_text(encoding="utf-8") for source_file in source_files
        )


if __name__ == "__main__":
    unittest.main()
