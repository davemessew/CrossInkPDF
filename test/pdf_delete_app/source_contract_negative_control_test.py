import source_contract_test


def expect_rejected(
    name: str, path: str, original: str, replacement: str
) -> None:
    texts = source_contract_test.sources()
    if original not in texts[path]:
        raise AssertionError(f"{name}: mutation source bytes were not found")
    mutated = dict(texts)
    mutated[path] = mutated[path].replace(original, replacement, 1)
    if mutated[path] == texts[path]:
        raise AssertionError(f"{name}: mutation did not change source bytes")
    try:
        source_contract_test.validate(mutated)
    except AssertionError:
        return
    raise AssertionError(f"{name}: source contract accepted the mutation")


def main() -> None:
    expect_rejected(
        "disabled legacy delete",
        "src/activities/home/FileBrowserActivity.cpp",
        "if (!Storage.remove(fullPath.c_str()))",
        "if (true)",
    )
    expect_rejected(
        "disabled strict canonical recents load",
        "src/RecentBooksStore.cpp",
        "readStrictRecentDeleteDocument(doc, scratch->json)",
        "removedStrictRecentDeleteDocument(doc, scratch->json)",
    )
    expect_rejected(
        "disabled directory delete recovery fence",
        "src/activities/home/FileBrowserActivity.cpp",
        "PdfDeleteUtils::recoverPendingPdfDelete()",
        "PdfDeleteUtils::Result::NoPendingDelete",
    )
    expect_rejected(
        "disabled directory delete orchestrator",
        "src/activities/home/FileBrowserActivity.cpp",
        "PdfDirectoryDeleteScan::deleteDirectoryNoThrow(dirPath, callbacks)",
        "removedDirectoryDelete(dirPath, callbacks)",
    )
    expect_rejected(
        "disabled no-throw walk workspace",
        "src/util/PdfDirectoryDeleteScan.cpp",
        "auto workspace = makeUniqueNoThrow<LegacyWalkWorkspace>();",
        "auto workspace = std::make_unique<LegacyWalkWorkspace>();",
    )
    expect_rejected(
        "disabled scanner tombstone fence",
        "src/util/PdfDirectoryDeleteScan.cpp",
        "endsWith(name, PdfDelete::kTombstoneSuffix)",
        "false",
    )
    expect_rejected(
        "legacy format sent to PDF journal spool",
        "src/util/PdfDirectoryDeleteScan.cpp",
        "if (isJournaledPdfPath(name))",
        "if (isJournaledPdfPath(name) || FsHelpers::hasEpubExtension(name))",
    )
    expect_rejected(
        "opened spool while directory reader is active",
        "src/util/PdfDirectoryDeleteScan.cpp",
        """const uint16_t directoryLength = workspace.currentLength;
        Status status = Status::Complete;
        if (!closeLegacyEnumeration(workspace, &status)) return status;""",
        """const uint16_t directoryLength = workspace.currentLength;
        Status status = Status::Complete;
        if (false) return status;""",
    )
    expect_rejected(
        "disabled sealed spool validation",
        "src/util/PdfDirectoryDeleteScan.cpp",
        "status = validateSpool(rootPath, *workspace, *spool);",
        "status = Status::Complete;",
    )
    expect_rejected(
        "disabled spool root binding",
        "src/util/PdfDirectoryDeleteScan.cpp",
        "isRootBoundPdfPath(rootPath, path)",
        "isJournaledPdfPath(path)",
    )
    expect_rejected(
        "disabled spool aggregate crc",
        "src/util/PdfDirectoryDeleteScan.cpp",
        "actualRecordsCrc == expectedRecordsCrc",
        "true",
    )
    expect_rejected(
        "disabled exact spool length",
        "src/util/PdfDirectoryDeleteScan.cpp",
        "file.position() == footerOffset",
        "true",
    )
    expect_rejected(
        "disabled simulator hidden tombstone fence",
        "src/util/PdfDirectoryDeleteScan.cpp",
        "simulatorHiddenTombstoneStatus(rootPath)",
        "Status::Complete",
    )
    expect_rejected(
        "disabled strict books array validation",
        "src/RecentBooksStore.cpp",
        "if (!books.is<JsonArrayConst>()) return false;",
        "if (false) return false;",
    )
    expect_rejected(
        "disabled deletion JSON overflow check",
        "src/RecentBooksStore.cpp",
        "if (doc.overflowed()) return false;",
        "if (false) return false;",
    )
    expect_rejected(
        "disabled fallible output reserve",
        "src/RecentBooksStore.cpp",
        "if (!reserveRecentDeleteString(json, measured)) return false;",
        "if (false) return false;",
    )
    expect_rejected(
        "disabled exact serialization length",
        "src/RecentBooksStore.cpp",
        "json.length() == measured;",
        "true;",
    )
    expect_rejected(
        "disabled web tombstone fence",
        "src/network/CrossPointWebServer.cpp",
        "if (itemPath.endsWith(PdfDelete::kTombstoneSuffix))",
        "if (false)",
    )
    print("PDF deletion source negative controls passed")


if __name__ == "__main__":
    main()
