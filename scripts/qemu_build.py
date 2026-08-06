import configparser
import hashlib
import json
from pathlib import Path
import shlex
import subprocess
import sys
from typing import Any


FLASH_SIZE = 16 * 1024 * 1024
FILESYSTEM_SIZE = 0x360000
MIN_WRITABLE_HEADROOM = 512 * 1024
EXPECTED_OFFSETS = {
    "bootloader": 0x0,
    "partitions": 0x8000,
    "otadata": 0xE000,
    "application": 0x10000,
    "filesystem": 0xC90000,
}
IMAGE_ORDER = tuple(EXPECTED_OFFSETS)
EFUSE_SIZE = 1024
EFUSE_REVISION_BYTE_OFFSET = 38
EFUSE_REVISION_BYTE_VALUE = 0x0C
ESP_IDF_EFUSE_SOURCE = "esp-idf-v5.5.2/tools/idf_py_actions/qemu_ext.py"
PDF_FIXTURE_RELATIVE_PATH = Path("qemu") / "classic_text.pdf"
PDF_NAVIGATION_FIXTURE_RELATIVE_PATH = Path("qemu") / "navigation_outline.pdf"
PDF_POSITIVE_FIXTURE_RELATIVE_PATHS = tuple(
    Path("qemu") / name
    for name in ("hidden_ocr.pdf", "columns_table.pdf", "jpeg_caption.pdf")
)


def validate_flash_layout(
    entries: dict[str, tuple[int, Path]],
) -> dict[str, tuple[int, Path]]:
    missing = set(EXPECTED_OFFSETS).difference(entries)
    extra = set(entries).difference(EXPECTED_OFFSETS)
    if missing or extra:
        raise ValueError(
            f"flash image set differs: missing={sorted(missing)} extra={sorted(extra)}"
        )
    validated: dict[str, tuple[int, Path]] = {}
    for name in IMAGE_ORDER:
        offset, image = entries[name]
        if offset != EXPECTED_OFFSETS[name]:
            raise ValueError(
                f"{name} offset is {hex(offset)}, expected {hex(EXPECTED_OFFSETS[name])}"
            )
        path = Path(image)
        if not path.is_file():
            raise ValueError(f"{name} image does not exist: {path}")
        if offset + path.stat().st_size > FLASH_SIZE:
            raise ValueError(f"{name} image exceeds the 16 MiB flash")
        validated[name] = (offset, path.resolve())
    return validated


def verify_fixture_headroom(fixtures: Path) -> int:
    total = sum(
        path.stat().st_size
        for path in fixtures.rglob("*")
        if path.is_file()
    )
    maximum_fixture_bytes = FILESYSTEM_SIZE - MIN_WRITABLE_HEADROOM
    if total > maximum_fixture_bytes:
        raise ValueError(
            "QEMU fixtures leave less than the required writable headroom"
        )
    return total


def verify_pdf_fixture(canonical: Path, staged: Path) -> str:
    if not canonical.is_file():
        raise ValueError(f"canonical PDF fixture does not exist: {canonical}")
    if not staged.is_file():
        raise ValueError(f"staged QEMU PDF fixture does not exist: {staged}")
    canonical_hash = _sha256_file(canonical)
    staged_hash = _sha256_file(staged)
    if staged_hash != canonical_hash:
        raise ValueError("staged QEMU PDF fixture differs from the generated corpus")
    return canonical_hash


def generate_esp32c3_efuse(output: Path) -> None:
    efuse = bytearray(EFUSE_SIZE)
    efuse[EFUSE_REVISION_BYTE_OFFSET] = EFUSE_REVISION_BYTE_VALUE
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_bytes(efuse)


def build_merge_command(
    python_executable: Path,
    esptool: Path,
    output: Path,
    entries: dict[str, tuple[int, Path]],
) -> list[str]:
    validated = validate_flash_layout(entries)
    command = [
        str(python_executable),
        str(esptool),
        "--chip",
        "esp32c3",
        "merge_bin",
        "-o",
        str(output),
        "--fill-flash-size",
        "16MB",
    ]
    for name in IMAGE_ORDER:
        offset, image = validated[name]
        command.extend([hex(offset), str(image)])
    return command


def _sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        while chunk := source.read(64 * 1024):
            digest.update(chunk)
    return digest.hexdigest()


def _sha256_tree(root: Path) -> str:
    digest = hashlib.sha256()
    for path in sorted(candidate for candidate in root.rglob("*") if candidate.is_file()):
        relative = path.relative_to(root).as_posix().encode("utf-8")
        digest.update(len(relative).to_bytes(4, "big"))
        digest.update(relative)
        digest.update(bytes.fromhex(_sha256_file(path)))
    return digest.hexdigest()


def _sha256_ini_sections(path: Path, section_names: tuple[str, ...]) -> str:
    parser = configparser.RawConfigParser(interpolation=None, strict=False)
    if not parser.read(path, encoding="utf-8"):
        raise RuntimeError(f"cannot read PlatformIO config: {path}")

    digest = hashlib.sha256()

    def update_field(value: str) -> None:
        encoded = value.encode("utf-8")
        digest.update(len(encoded).to_bytes(4, "big"))
        digest.update(encoded)

    for section_name in section_names:
        if not parser.has_section(section_name):
            raise RuntimeError(
                f"PlatformIO config has no [{section_name}] section"
            )
        update_field(section_name)
        for key, value in sorted(parser.items(section_name, raw=True)):
            normalized_value = "\n".join(
                " ".join(line.split())
                for line in value.splitlines()
                if line.strip()
            )
            update_field(key)
            update_field(normalized_value)
    return digest.hexdigest()


def _package_version(package_dir: Path) -> str:
    for metadata_name in ("package.json", ".piopm"):
        metadata_path = package_dir / metadata_name
        if not metadata_path.is_file():
            continue
        try:
            metadata = json.loads(metadata_path.read_text(encoding="utf-8"))
        except (OSError, json.JSONDecodeError):
            continue
        version = metadata.get("version")
        if isinstance(version, str):
            return version
        spec = metadata.get("spec")
        if isinstance(spec, dict) and isinstance(spec.get("version"), str):
            return spec["version"]
    raise RuntimeError(f"cannot determine package version for {package_dir}")


def _normalized_flags(raw_flags: Any) -> list[str]:
    if isinstance(raw_flags, str):
        flags = shlex.split(raw_flags, posix=True)
    elif isinstance(raw_flags, (list, tuple)):
        flags = [str(flag) for flag in raw_flags]
    else:
        raise RuntimeError("BUILD_FLAGS has an unsupported type")
    return sorted({" ".join(flag.split()) for flag in flags})


def _parse_offset(value: Any) -> int:
    if isinstance(value, int):
        return value
    return int(str(value), 0)


def _flash_entries_from_environment(
    env: Any, firmware: Path, filesystem: Path
) -> dict[str, tuple[int, Path]]:
    by_offset: dict[int, Path] = {}
    for raw_offset, raw_path in env.get("FLASH_EXTRA_IMAGES", []):
        offset = _parse_offset(env.subst(str(raw_offset)))
        by_offset[offset] = Path(env.subst(str(raw_path)))

    entries: dict[str, tuple[int, Path]] = {}
    for name in ("bootloader", "partitions", "otadata"):
        offset = EXPECTED_OFFSETS[name]
        if offset not in by_offset:
            raise RuntimeError(
                f"PlatformIO FLASH_EXTRA_IMAGES has no {name} at {hex(offset)}"
            )
        entries[name] = (offset, by_offset[offset])

    entries["application"] = (
        _parse_offset(env.subst("$ESP32_APP_OFFSET")),
        firmware,
    )
    if "FS_START" not in env:
        raise RuntimeError("PlatformIO did not generate FS_START")
    entries["filesystem"] = (_parse_offset(env["FS_START"]), filesystem)
    return validate_flash_layout(entries)


def _size_tool(platform: Any) -> tuple[Path, str]:
    toolchain_dir_raw = platform.get_package_dir("toolchain-riscv32-esp")
    if not toolchain_dir_raw:
        raise RuntimeError("PlatformIO RISC-V toolchain package is unavailable")
    toolchain_dir = Path(toolchain_dir_raw)
    suffix = ".exe" if sys.platform == "win32" else ""
    size_tool = (
        toolchain_dir / "bin" / f"riscv32-esp-elf-size{suffix}"
    ).resolve()
    if not size_tool.is_file():
        raise RuntimeError(f"RISC-V size tool does not exist: {size_tool}")
    return size_tool, _package_version(toolchain_dir)


def _resource_fingerprint(env: Any, project_dir: Path) -> tuple[dict[str, Any], dict[str, Any]]:
    platform = env.PioPlatform()
    framework_dir_raw = platform.get_package_dir(
        "framework-arduinoespressif32"
    )
    if not framework_dir_raw:
        raise RuntimeError("PlatformIO Arduino framework package is unavailable")
    size_tool, toolchain_version = _size_tool(platform)
    partitions = Path(env.subst("$PARTITIONS_TABLE_CSV")).resolve()
    qemu_hal = project_dir / "test" / "qemu" / "hal"
    platformio_ini = project_dir / "platformio.ini"
    fingerprint = {
        "toolchain": toolchain_version,
        "platform": str(platform.version),
        "framework": _package_version(Path(framework_dir_raw)),
        "build_flags": _normalized_flags(env.get("BUILD_FLAGS", [])),
        "partition_sha256": _sha256_file(partitions),
        "qemu_hal_sha256": _sha256_tree(qemu_hal),
        "qemu_config_sha256": _sha256_ini_sections(
            platformio_ini, ("base", "env:qemu-esp32c3")
        ),
    }
    tools = {
        "size": {
            "path": str(size_tool),
            "version": toolchain_version,
        }
    }
    return fingerprint, tools


def _write_manifest(
    manifest_path: Path,
    entries: dict[str, tuple[int, Path]],
    flash_image: Path,
    efuse_image: Path,
    env: Any,
    fixture_bytes: int,
    pdf_fixture_sha256: str,
    pdf_navigation_fixture_sha256: str,
    positive_pdf_fixture_sha256: dict[str, str],
    elf: Path,
) -> None:
    project_dir = Path(env.subst("$PROJECT_DIR")).resolve()
    fingerprint, tools = _resource_fingerprint(env, project_dir)
    images = {
        name: {
            "offset": offset,
            "path": str(path),
            "sha256": _sha256_file(path),
            "size": path.stat().st_size,
        }
        for name, (offset, path) in entries.items()
    }
    images["flash"] = {
        "path": str(flash_image.resolve()),
        "sha256": _sha256_file(flash_image),
        "size": flash_image.stat().st_size,
    }
    images["efuse"] = {
        "path": str(efuse_image.resolve()),
        "sha256": _sha256_file(efuse_image),
        "size": efuse_image.stat().st_size,
        "source": ESP_IDF_EFUSE_SOURCE,
    }
    manifest = {
        "schema_version": 1,
        "environment": "qemu-esp32c3",
        "flash_size": FLASH_SIZE,
        "filesystem": {
            "offset": EXPECTED_OFFSETS["filesystem"],
            "size": FILESYSTEM_SIZE,
            "page_size": 256,
            "block_size": 4096,
            "fixture_bytes": fixture_bytes,
            "writable_headroom": MIN_WRITABLE_HEADROOM,
            "pdf_fixture_sha256": pdf_fixture_sha256,
            "pdf_navigation_fixture_sha256": pdf_navigation_fixture_sha256,
            "positive_pdf_fixture_sha256": positive_pdf_fixture_sha256,
        },
        "images": images,
        "artifacts": {
            "elf": {
                "path": str(elf.resolve()),
                "sha256": _sha256_file(elf),
                "size": elf.stat().st_size,
            }
        },
        "resource_fingerprint": fingerprint,
        "tools": tools,
    }
    manifest_path.write_text(
        json.dumps(manifest, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )


def _build_qemu_images(target: Any, source: Any, env: Any) -> int:
    flash_image = Path(str(target[0])).resolve()
    efuse_image = Path(str(target[1])).resolve()
    manifest_path = Path(str(target[2])).resolve()
    firmware = Path(str(source[0])).resolve()
    filesystem = Path(str(source[1])).resolve()
    elf = Path(str(source[2])).resolve()
    fixtures = Path(env.subst("$PROJECT_DIR/test/qemu/data")).resolve()
    canonical_pdf = Path(
        env.subst(
            "$PROJECT_DIR/test/pdf_reflow_core/fixtures/classic_text.pdf"
        )
    ).resolve()
    canonical_navigation_pdf = Path(
        env.subst(
            "$PROJECT_DIR/test/pdf_reflow_core/fixtures/navigation_outline.pdf"
        )
    ).resolve()
    canonical_pdf_fixtures = Path(
        env.subst("$PROJECT_DIR/test/pdf_reflow_core/fixtures")
    ).resolve()

    try:
        pdf_fixture_sha256 = verify_pdf_fixture(
            canonical_pdf, fixtures / PDF_FIXTURE_RELATIVE_PATH
        )
        pdf_navigation_fixture_sha256 = verify_pdf_fixture(
            canonical_navigation_pdf,
            fixtures / PDF_NAVIGATION_FIXTURE_RELATIVE_PATH,
        )
        positive_pdf_fixture_sha256: dict[str, str] = {}
        for relative_path in PDF_POSITIVE_FIXTURE_RELATIVE_PATHS:
            positive_pdf_fixture_sha256[relative_path.name] = verify_pdf_fixture(
                canonical_pdf_fixtures / relative_path.name,
                fixtures / relative_path,
            )
        fixture_bytes = verify_fixture_headroom(fixtures)
        entries = _flash_entries_from_environment(
            env, firmware, filesystem
        )
        python_executable, esptool = env.PioPlatform().setup_python_env(env)
        if not python_executable or not esptool:
            raise RuntimeError("PlatformIO did not provide its esptool runtime")
        flash_image.parent.mkdir(parents=True, exist_ok=True)
        completed = subprocess.run(
            build_merge_command(
                Path(python_executable),
                Path(esptool),
                flash_image,
                entries,
            ),
            capture_output=True,
            text=True,
            check=False,
        )
        if completed.returncode != 0:
            raise RuntimeError(
                f"offline merge_bin failed: {completed.stderr.strip()}"
            )
        if flash_image.stat().st_size != FLASH_SIZE:
            raise RuntimeError("qemu_flash.bin is not exactly 16 MiB")
        generate_esp32c3_efuse(efuse_image)
        _write_manifest(
            manifest_path,
            entries,
            flash_image,
            efuse_image,
            env,
            fixture_bytes,
            pdf_fixture_sha256,
            pdf_navigation_fixture_sha256,
            positive_pdf_fixture_sha256,
            elf,
        )
    except (OSError, RuntimeError, ValueError) as error:
        sys.stderr.write(f"QEMU image build failed: {error}\n")
        return 1
    return 0


def register_qemu_image_target(env: Any) -> None:
    firmware = env.File("$BUILD_DIR/${PROGNAME}.bin")
    elf = env.File("$BUILD_DIR/${PROGNAME}.elf")
    filesystem = env.DataToBin("$BUILD_DIR/qemu-data", "$PROJECT_DIR/test/qemu/data")
    outputs = env.Command(
        [
            "$BUILD_DIR/qemu_flash.bin",
            "$BUILD_DIR/qemu_efuse.bin",
            "$BUILD_DIR/qemu_manifest.json",
        ],
        [firmware, filesystem, elf],
        env.VerboseAction(_build_qemu_images, "Building ESP32-C3 QEMU image"),
    )
    env.AddCustomTarget(
        name="qemu-image",
        dependencies=outputs,
        actions=None,
        title="ESP32-C3 QEMU Image",
        description="Build a no-hardware ESP32-C3 QEMU flash image",
    )


if "Import" in globals():
    Import("env")
    register_qemu_image_target(env)
