from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
SOURCE_PATH = ROOT / "src/activities/home/FileBrowserActivity.cpp"
SOURCE = SOURCE_PATH.read_text(encoding="utf-8")


def function_text(source: str, signature: str, next_signature: str) -> str:
    start = source.index(signature)
    end = source.index(next_signature, start)
    return source[start:end]


def require_in_order(text: str, needles: list[str], contract: str) -> None:
    cursor = 0
    for needle in needles:
        position = text.find(needle, cursor)
        if position < 0:
            raise AssertionError(f"{contract}: missing or out of order: {needle}")
        cursor = position + len(needle)


def validate(source: str) -> None:
    if "PdfDirectoryDeleteScan::containsPdfNoThrow" in source:
        raise AssertionError("FileBrowser reintroduced the separate PDF classification traversal")

    if "bool isPdfDirectoryDeleteEntry" not in source:
        raise AssertionError("FileBrowser is missing PDF tombstone classification")
    pdf_entry_classifier = function_text(
        source,
        "bool isPdfDirectoryDeleteEntry",
        "enum class MetadataDirectoryNextResult",
    )
    require_in_order(
        pdf_entry_classifier,
        [
            "FsHelpers::hasPdfExtension(name)",
            "PdfDelete::kTombstoneSuffix",
            "name.compare(name.size() - tombstoneSuffix.size(), tombstoneSuffix.size(), tombstoneSuffix) != 0",
            "name.remove_suffix(tombstoneSuffix.size())",
            "return FsHelpers::hasPdfExtension(name);",
        ],
        "PDF directory classification must strip only the exact tombstone suffix before checking PDF",
    )

    if "bool closeMetadataEntry" not in source:
        raise AssertionError("FileBrowser is missing reusable-entry close handling")
    close_entry = function_text(
        source,
        "bool closeMetadataEntry",
        "bool closeMetadataDirectory",
    )
    require_in_order(
        close_entry,
        ["!entry.isOpen()", "entry.close()"],
        "metadata traversal must treat an already-invalid reusable entry as closed",
    )

    next_entry = function_text(
        source,
        "MetadataDirectoryNextResult nextMetadataDirectoryEntry",
        "bool closeMetadataDirectory",
    )
    if "HalDirectoryNextStatus" not in next_entry or "MetadataDirectoryNextResult::Error" not in next_entry:
        raise AssertionError("metadata traversal no longer distinguishes EOF from iteration failure")

    collector = function_text(
        source,
        "DirectoryMetadataScanStatus collectMetadataPathsRecursively",
        "std::string normalizeDirectoryPath",
    )
    required_collector_fragments = [
        "nextMetadataDirectoryEntry(dir, file)",
        "DirectoryMetadataScanStatus::Failed",
        "isPdfDirectoryDeleteEntry(nameView)",
        "DirectoryMetadataScanStatus::PdfFound",
        "collectMetadataPathsRecursively(childPath, paths)",
        "paths.push_back(childPath)",
        "DirectoryMetadataScanStatus::Complete",
    ]
    for fragment in required_collector_fragments:
        if fragment not in collector:
            raise AssertionError(f"one-pass metadata/PDF traversal is missing: {fragment}")
    for forbidden in ["INDEX_THRESHOLD", "paths.size() >=", "paths.size() ==", "containsPdfNoThrow"]:
        if forbidden in collector:
            raise AssertionError(f"metadata/PDF traversal gained an entry-count cap or second scan: {forbidden}")

    directory_branch = collector[collector.index("if (isDirectory) {") :]
    require_in_order(
        directory_branch,
        [
            "if (!closeMetadataEntry(file))",
            "DirectoryMetadataScanStatus::Failed",
            "collectMetadataPathsRecursively(childPath, paths)",
        ],
        "directory entries must close successfully before the same child path is reopened",
    )

    prompt = function_text(
        source,
        "void FileBrowserActivity::promptDeleteDirectory",
        "void FileBrowserActivity::showDirectoryActionMenu",
    )
    if prompt.count("collectMetadataPathsRecursively(dirPath, metadataPaths)") != 1:
        raise AssertionError("directory routing must perform exactly one FileBrowser traversal")
    require_in_order(
        prompt,
        [
            "std::vector<std::string> metadataPaths;",
            "collectMetadataPathsRecursively(dirPath, metadataPaths)",
            "scanStatus == DirectoryMetadataScanStatus::Failed",
            "return;",
            "scanStatus == DirectoryMetadataScanStatus::PdfFound",
            "std::vector<std::string>().swap(metadataPaths);",
            "PdfDirectoryDeleteScan::deletePdfDirectoryNoThrow(dirPath, callbacks)",
        ],
        "PDF directory route must fail closed, release legacy paths, then journal",
    )

    pdf_branch_end = prompt.index("} else {", prompt.index("scanStatus == DirectoryMetadataScanStatus::PdfFound"))
    pdf_branch = prompt[prompt.index("scanStatus == DirectoryMetadataScanStatus::PdfFound") : pdf_branch_end]
    if "Storage.removeDir(dirPath.c_str())" in pdf_branch:
        raise AssertionError("PDF directory route bypassed journaled deletion")

    legacy_branch = prompt[pdf_branch_end:]
    require_in_order(
        legacy_branch,
        [
            "Storage.removeDir(dirPath.c_str())",
            'LOG_DBG("FileBrowser", "Deleted successfully")',
            "for (const auto& metadataPath : metadataPaths)",
            "BookActions::clearFileMetadata(metadataPath);",
        ],
        "PDF-free route must preserve upstream remove/log/metadata ordering",
    )
    first_remove = legacy_branch.index("Storage.removeDir(dirPath.c_str())")
    first_metadata_clear = legacy_branch.index("BookActions::clearFileMetadata")
    if first_metadata_clear < first_remove:
        raise AssertionError("PDF-free route cleared metadata before source directory removal")
    if "collectMetadataPathsRecursively" in legacy_branch:
        raise AssertionError("PDF-free branch reintroduced a second traversal")


def expect_rejected(label: str, old: str, new: str) -> None:
    if old not in SOURCE:
        raise AssertionError(f"negative control setup missing for {label}: {old}")
    mutated = SOURCE.replace(old, new, 1)
    try:
        validate(mutated)
    except (AssertionError, ValueError):
        return
    raise AssertionError(f"source contract accepted mutation: {label}")


def main() -> None:
    validate(SOURCE)
    expect_rejected(
        "separate PDF pre-scan",
        "std::vector<std::string> metadataPaths;",
        "bool containsPdf = false;\n    PdfDirectoryDeleteScan::containsPdfNoThrow(dirPath, &containsPdf);\n"
        "    std::vector<std::string> metadataPaths;",
    )
    expect_rejected(
        "ignored traversal failure",
        "scanStatus == DirectoryMetadataScanStatus::Failed",
        "false",
    )
    expect_rejected(
        "lost PDF detection",
        "isPdfDirectoryDeleteEntry(nameView)",
        "FsHelpers::hasPdfExtension(nameView)",
    )
    expect_rejected(
        "accepted an inexact tombstone suffix",
        "name.compare(name.size() - tombstoneSuffix.size(), tombstoneSuffix.size(), tombstoneSuffix) != 0",
        "false",
    )
    expect_rejected(
        "accepted a non-PDF tombstone",
        "return FsHelpers::hasPdfExtension(name);",
        "return true;",
    )
    expect_rejected(
        "reopened a child directory before closing its entry",
        "if (!closeMetadataEntry(file))",
        "if (false)",
    )
    expect_rejected(
        "retained collected metadata on strict PDF route",
        "std::vector<std::string>().swap(metadataPaths);",
        "metadataPaths.clear();",
    )
    expect_rejected(
        "capped traversal below large-directory coverage",
        "const MetadataDirectoryNextResult next = nextMetadataDirectoryEntry(dir, file);",
        "if (paths.size() >= 500U) return DirectoryMetadataScanStatus::Failed;\n"
        "    const MetadataDirectoryNextResult next = nextMetadataDirectoryEntry(dir, file);",
    )
    expect_rejected(
        "cleared metadata before source directory removal",
        "if (!Storage.removeDir(dirPath.c_str())) {",
        "BookActions::clearFileMetadata(metadataPaths.front());\n"
        "      if (!Storage.removeDir(dirPath.c_str())) {",
    )


if __name__ == "__main__":
    main()
