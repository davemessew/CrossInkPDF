from pathlib import Path
import re
import unittest


REPO_ROOT = Path(__file__).resolve().parents[2]
HAL_SOURCE = REPO_ROOT / "test" / "qemu" / "hal" / "src" / "HalStorage.cpp"
HAL_HEADER = REPO_ROOT / "test" / "qemu" / "hal" / "src" / "HalStorage.h"
PRODUCTION_HAL_SOURCE = REPO_ROOT / "lib" / "hal" / "HalStorage.cpp"
PRODUCTION_HAL_HEADER = REPO_ROOT / "lib" / "hal" / "HalStorage.h"
PDF_CACHE_IO_HEADER = REPO_ROOT / "lib" / "PdfReflow" / "PdfCacheIo.h"
PDF_HAL_CACHE_IO_SOURCE = REPO_ROOT / "lib" / "PdfReflow" / "PdfHalCacheIo.cpp"
CAPACITY_HEADER = REPO_ROOT / "test" / "qemu" / "hal" / "src" / "QemuStorageCapacityCache.h"
QEMU_SOURCE = REPO_ROOT / "src" / "qemu" / "QemuAcceptance.cpp"


def function_body(source: str, signature: str) -> str:
    start = source.index(signature)
    opening = source.index("{", start)
    depth = 0
    for offset in range(opening, len(source)):
        if source[offset] == "{":
            depth += 1
        elif source[offset] == "}":
            depth -= 1
            if depth == 0:
                return source[opening + 1 : offset]
    raise AssertionError(f"unterminated function {signature}")


def hal_open_failures(source: str) -> list[str]:
    failures: list[str] = []
    mapper = function_body(source, "fs::File openWithFlags")
    storage_open = function_body(source, "HalFile HalStorage::open")
    if "LittleFS.exists(path)" in mapper or "LittleFS.exists(path)" in storage_open:
        failures.append("unconditional existence lookup")
    for token in (
        "LittleFS.open(path, FILE_READ, false)",
        'LittleFS.open(path, "r+", false)',
        'opened = LittleFS.open(path, readWrite ? "w+" : FILE_WRITE, false)',
        "(flags & O_TRUNC) != 0",
        "(flags & O_CREAT) != 0",
        "if (!opened && create)",
    ):
        if token not in mapper:
            failures.append(f"lazy mode mapping: {token}")
    if "LittleFS.open(path, mode, false)" not in mapper:
        failures.append("direct truncate open")
    if "LittleFS.open(path, mode, true)" in mapper or "LittleFS.mkdir" in mapper:
        failures.append("implicit parent creation")
    if "openWithFlags(path, oflag)" not in storage_open:
        failures.append("HalStorage open mapping")
    return failures


def capacity_cache_failures(storage: str, cache: str) -> list[str]:
    failures: list[str] = []
    begin = function_body(storage, "bool HalStorage::begin")
    can_write = function_body(storage, "bool canWrite")
    account_write = function_body(storage, "void accountWrite")
    file_write = function_body(storage, "size_t HalFile::write(const void*")

    if storage.count("LittleFS.totalBytes()") != 1 or "LittleFS.totalBytes()" not in begin:
        failures.append("begin-only totalBytes probe")
    if storage.count("LittleFS.usedBytes()") != 1 or "LittleFS.usedBytes()" not in begin:
        failures.append("begin-only usedBytes probe")
    if "std::min<uint64_t>(LITTLEFS_CAPACITY_BYTES, LittleFS.totalBytes())" not in begin:
        failures.append("physical capacity cap")
    if "qemuPhysicalCapacity.refresh(physicalCapacity, physicalUsed)" not in begin:
        failures.append("begin refresh")
    if "qemuPhysicalCapacity.canWrite(count)" not in can_write:
        failures.append("cached physical write check")
    if "static_cast<uint64_t>(count) > qemuStorageQuota" not in can_write:
        failures.append("fault-injection quota check")
    if "LittleFS.usedBytes()" in can_write or "LittleFS.totalBytes()" in can_write:
        failures.append("per-write filesystem capacity probe")
    if "qemuPhysicalCapacity.charge(count)" not in account_write:
        failures.append("actual-byte physical charge")
    if "qemuStorageQuota -= std::min<uint64_t>(qemuStorageQuota, count)" not in account_write:
        failures.append("actual-byte quota charge")
    if "accountWrite(written)" not in file_write or "accountWrite(count)" in file_write:
        failures.append("short-write accounting")

    for signature in (
        "fs::File openWithFlags",
        "bool removeDirectoryTree",
        "bool HalStorage::remove(",
        "bool HalStorage::rename(",
        "bool HalStorage::rmdir(",
        "bool HalFile::rename(",
    ):
        body = function_body(storage, signature)
        if "qemuPhysicalCapacity." in body:
            failures.append(f"capacity credit from {signature}")

    for token in (
        "uint64_t capacityBytes_ = 0;",
        "uint64_t accountedBytes_ = 0;",
        "static_assert(sizeof(QemuStorageCapacityCache) == 2 * sizeof(uint64_t))",
        "accountedBytes_ <= capacityBytes_",
        "capacityBytes_ - accountedBytes_",
        "chargedBytes <= remainingBytes ? chargedBytes : remainingBytes",
    ):
        if token not in cache:
            failures.append(f"fixed saturating cache: {token}")
    for token in ("new ", "malloc(", "std::vector", "std::string"):
        if token in cache:
            failures.append(f"allocation or overflow risk: {token}")
    if re.search(r"accountedBytes_\s*\+(?!=)", cache):
        failures.append("allocation or overflow risk: unchecked accounted-byte addition")
    return failures


def truncate_contract_failures(storage: str, acceptance: str, cache: str) -> list[str]:
    failures: list[str] = []
    truncate = function_body(storage, "bool HalFile::truncate64")
    close_at = truncate.find("close();")
    path_truncate_at = truncate.find("::truncate")
    reopen_at = truncate.find("openWithFlags(reopenPath, O_RDWR)")
    if close_at < 0 or path_truncate_at < 0 or reopen_at < 0 or not close_at < path_truncate_at < reopen_at:
        failures.append("single-descriptor close-truncate-reopen handoff")
    for token in (
        "const uint32_t originalPosition = file.position();",
        "const bool truncated = ::truncate",
        "countedOpen = true;",
        "writable = true;",
        "++qemuStorageOpenCount;",
        "truncated ? static_cast<uint32_t>(length) : originalPosition",
        "return truncated && positioned;",
    ):
        if token not in truncate:
            failures.append(f"caller-visible truncate state: {token}")

    parity = function_body(acceptance, "bool checkStorageOpenParity")
    for token in (
        "prefixTruncate.truncate64(2U) || prefixTruncate.fileSize64() != 2U",
        "prefixTruncate.position() != 2U) {",
        "std::memcmp(prefix, seed, sizeof(prefix)) != 0",
        '"truncate_read_only"',
        '"truncate_oversize"',
        '"QEMU_STORAGE_TRUNCATE_PASS size=2 position=2 prefix=ab\\n"',
    ):
        if token not in parity:
            failures.append(f"runtime truncate receipt: {token}")

    for token in (
        "static_assert(sizeof(PdfCacheIo) == 48U",
        "static_assert(offsetof(PdfCacheIo, metadata) == 44U",
    ):
        if token not in cache:
            failures.append(f"RV32 cache ABI guard: {token}")
    return failures


class QemuHalStorageContractTest(unittest.TestCase):
    def test_pdf_prefix_truncate_is_wired_without_growing_cache_io(self) -> None:
        production_header = PRODUCTION_HAL_HEADER.read_text(encoding="utf-8")
        production_source = PRODUCTION_HAL_SOURCE.read_text(encoding="utf-8")
        qemu_header = HAL_HEADER.read_text(encoding="utf-8")
        qemu_source = HAL_SOURCE.read_text(encoding="utf-8")
        cache_header = PDF_CACHE_IO_HEADER.read_text(encoding="utf-8")
        cache_adapter = PDF_HAL_CACHE_IO_SOURCE.read_text(encoding="utf-8")

        self.assertIn("bool truncate64(uint64_t length);", production_header)
        self.assertIn("bool truncate64(uint64_t length);", qemu_header)
        self.assertIn(
            "HAL_FILE_WRAPPED_CALL(truncate, length)",
            function_body(production_source, "bool HalFile::truncate64"),
        )
        qemu_truncate = function_body(qemu_source, "bool HalFile::truncate64")
        self.assertIn("LittleFS.mountpoint()", qemu_truncate)
        self.assertIn("::truncate", qemu_truncate)
        self.assertIn("PdfCacheMetadataOperation::Truncate", cache_header)
        self.assertIn("pdfCacheTruncate", cache_header)
        self.assertIn("PdfCacheMetadataOperation::Truncate", cache_adapter)
        self.assertIn("file.truncate64(length)", cache_adapter)
        self.assertNotIn("TruncateFn", cache_header)

    def test_truncate_handoff_has_runtime_receipt_and_rv32_abi_guards(self) -> None:
        storage = HAL_SOURCE.read_text(encoding="utf-8")
        acceptance = QEMU_SOURCE.read_text(encoding="utf-8")
        cache = PDF_CACHE_IO_HEADER.read_text(encoding="utf-8")
        self.assertEqual(truncate_contract_failures(storage, acceptance, cache), [])

    def test_truncate_contract_mutation_controls(self) -> None:
        storage = HAL_SOURCE.read_text(encoding="utf-8")
        acceptance = QEMU_SOURCE.read_text(encoding="utf-8")
        cache = PDF_CACHE_IO_HEADER.read_text(encoding="utf-8")
        self.assertEqual(truncate_contract_failures(storage, acceptance, cache), [])
        mutations = {
            "keep descriptor open": (
                storage.replace("  file.flush();\n  close();\n", "  file.flush();\n", 1),
                acceptance,
                cache,
            ),
            "skip reopen": (
                storage.replace("openWithFlags(reopenPath, O_RDWR)", "fs::File{}", 1),
                acceptance,
                cache,
            ),
            "stale immediate size": (
                storage,
                acceptance.replace(
                    "prefixTruncate.truncate64(2U) || prefixTruncate.fileSize64() != 2U",
                    "prefixTruncate.truncate64(2U) || prefixTruncate.fileSize64() != 3U",
                    1,
                ),
                cache,
            ),
            "grow RV32 callback table": (
                storage,
                acceptance,
                cache.replace("sizeof(PdfCacheIo) == 48U", "sizeof(PdfCacheIo) == 52U", 1),
            ),
        }
        for name, (mutated_storage, mutated_acceptance, mutated_cache) in mutations.items():
            with self.subTest(mutation=name):
                self.assertTrue(
                    mutated_storage != storage or mutated_acceptance != acceptance or mutated_cache != cache
                )
                self.assertNotEqual(
                    truncate_contract_failures(mutated_storage, mutated_acceptance, mutated_cache), []
                )

    def test_open_mapping_matches_production_storage_semantics(self) -> None:
        source = HAL_SOURCE.read_text(encoding="utf-8")
        self.assertEqual(hal_open_failures(source), [])

    def test_open_mapping_mutation_controls(self) -> None:
        source = HAL_SOURCE.read_text(encoding="utf-8")
        self.assertEqual(hal_open_failures(source), [])
        mutations = {
            "unconditional exists": source.replace(
                "fs::File opened = openWithFlags(path, oflag);",
                "(void)LittleFS.exists(path);\n  fs::File opened = openWithFlags(path, oflag);",
                1,
            ),
            "auto parent create": source.replace(
                "LittleFS.open(path, mode, false)",
                "LittleFS.open(path, mode, true)",
                1,
            ),
            "missing readwrite fallback": source.replace(
                "if (!opened && create)",
                "if (false)",
                1,
            ),
            "bypass lazy mapper": source.replace(
                "openWithFlags(path, oflag)",
                "LittleFS.open(path, FILE_READ, false)",
                1,
            ),
        }
        for name, mutation in mutations.items():
            with self.subTest(mutation=name):
                self.assertNotEqual(mutation, source)
                self.assertNotEqual(hal_open_failures(mutation), [])

    def test_runtime_acceptance_covers_open_mode_semantics(self) -> None:
        source = QEMU_SOURCE.read_text(encoding="utf-8")
        body = function_body(source, "bool checkStorageOpenParity")
        for token in (
            "O_RDONLY",
            "O_WRONLY | O_CREAT | O_TRUNC",
            "O_RDWR | O_CREAT",
            '"missing_read"',
            '"preserve_existing"',
            '"missing_readwrite"',
            '"implicit_parent"',
        ):
            with self.subTest(token=token):
                self.assertIn(token, body)
        self.assertIn("checkStorageOpenParity()", function_body(source, "bool checkStorage()"))

    def test_capacity_is_snapshotted_once_and_conservatively_charged(self) -> None:
        storage = HAL_SOURCE.read_text(encoding="utf-8")
        cache = CAPACITY_HEADER.read_text(encoding="utf-8")
        self.assertEqual(capacity_cache_failures(storage, cache), [])

    def test_capacity_contract_mutation_controls(self) -> None:
        storage = HAL_SOURCE.read_text(encoding="utf-8")
        cache = CAPACITY_HEADER.read_text(encoding="utf-8")
        self.assertEqual(capacity_cache_failures(storage, cache), [])
        mutations = {
            "restore per-write probe": (
                storage.replace(
                    "return qemuPhysicalCapacity.canWrite(count);",
                    "(void)LittleFS.usedBytes();\n  return qemuPhysicalCapacity.canWrite(count);",
                    1,
                ),
                cache,
            ),
            "remove physical charge": (
                storage.replace("  qemuPhysicalCapacity.charge(count);\n", "", 1),
                cache,
            ),
            "credit delete": (
                storage.replace(
                    "bool HalStorage::remove(const char* path) {",
                    "bool HalStorage::remove(const char* path) {\n"
                    "  qemuPhysicalCapacity.refresh(LITTLEFS_CAPACITY_BYTES, 0);",
                    1,
                ),
                cache,
            ),
            "credit truncate": (
                storage.replace(
                    "if (truncate) {",
                    "if (truncate) {\n"
                    "    qemuPhysicalCapacity.refresh(LITTLEFS_CAPACITY_BYTES, 0);",
                    1,
                ),
                cache,
            ),
            "overflowing charge": (
                storage,
                cache.replace(
                    "accountedBytes_ += chargedBytes <= remainingBytes ? chargedBytes : remainingBytes;",
                    "accountedBytes_ += chargedBytes;",
                    1,
                ),
            ),
            "remove quota check": (
                storage.replace(
                    "if (static_cast<uint64_t>(count) > qemuStorageQuota)",
                    "if (false)",
                    1,
                ),
                cache,
            ),
            "charge requested bytes after short write": (
                storage.replace("accountWrite(written);", "accountWrite(count);", 1),
                cache,
            ),
            "remove re-begin refresh": (
                storage.replace(
                    "qemuPhysicalCapacity.refresh(physicalCapacity, physicalUsed);",
                    "(void)physicalCapacity;\n  (void)physicalUsed;",
                    1,
                ),
                cache,
            ),
        }
        for name, (mutated_storage, mutated_cache) in mutations.items():
            with self.subTest(mutation=name):
                self.assertTrue(mutated_storage != storage or mutated_cache != cache)
                self.assertNotEqual(capacity_cache_failures(mutated_storage, mutated_cache), [])


if __name__ == "__main__":
    unittest.main()
