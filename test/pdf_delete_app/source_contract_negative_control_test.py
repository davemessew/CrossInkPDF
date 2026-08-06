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


def expect_rejected_ignoring_whitespace(
    name: str,
    path: str,
    original: str,
    replacement: str,
    *,
    last: bool = False,
) -> None:
    texts = source_contract_test.sources()
    matches = list(source_contract_test.contract_pattern(original).finditer(texts[path]))
    if not matches:
        raise AssertionError(f"{name}: mutation source structure was not found")
    match = matches[-1] if last else matches[0]
    mutated = dict(texts)
    mutated[path] = (
        texts[path][: match.start()] + replacement + texts[path][match.end() :]
    )
    try:
        source_contract_test.validate(mutated)
    except AssertionError:
        return
    raise AssertionError(f"{name}: source contract accepted the mutation")


def expect_rejected_last(
    name: str, path: str, original: str, replacement: str
) -> None:
    texts = source_contract_test.sources()
    position = texts[path].rfind(original)
    if position < 0:
        raise AssertionError(f"{name}: mutation source bytes were not found")
    mutated = dict(texts)
    mutated[path] = (
        texts[path][:position]
        + replacement
        + texts[path][position + len(original) :]
    )
    try:
        source_contract_test.validate(mutated)
    except AssertionError:
        return
    raise AssertionError(f"{name}: source contract accepted the mutation")


def main() -> None:
    expect_rejected_ignoring_whitespace(
        "collapsed committed cleanup warning into hard failure",
        "src/util/PdfDirectoryDeleteScan.cpp",
        "return cleaned && postCommitStatus == Status::Complete\n"
        "             ? Status::Complete\n"
        "             : Status::CommittedWithCleanupWarning;",
        "return cleaned ? Status::Complete : Status::SpoolCleanupFailure;",
    )
    expect_rejected_ignoring_whitespace(
        "discarded error-aware directory iteration",
        "src/util/PdfDirectoryDeleteScan.cpp",
        "const DirectoryNextResult next =\n"
        "          nextDirectoryEntry(workspace.directory, workspace.entry);",
        "const DirectoryNextResult next = DirectoryNextResult::End;",
    )
    expect_rejected(
        "returned before FileBrowser refresh after committed deletion",
        "src/activities/home/FileBrowserActivity.cpp",
        "} else if (status != PdfDirectoryDeleteScan::Status::Complete) {",
        "}\n    if (status != PdfDirectoryDeleteScan::Status::Complete) {",
    )
    expect_rejected(
        "disabled FileBrowser committed-cleanup warning branch",
        "src/activities/home/FileBrowserActivity.cpp",
        "PdfDirectoryDeleteScan::Status::CommittedWithCleanupWarning",
        "PdfDirectoryDeleteScan::Status::Complete",
    )
    expect_rejected(
        "reintroduced callback path allocation",
        "src/activities/home/FileBrowserActivity.cpp",
        "return BookActions::clearDirectoryLegacyMetadataNoPathAlloc(path);",
        "return BookActions::clearDirectoryLegacyMetadataNoPathAlloc(std::string(path));",
    )
    expect_rejected(
        "reintroduced owning PDF directory callback",
        "src/activities/home/FileBrowserActivity.cpp",
        "BookActions::deleteDirectoryPdfBookNoPathAlloc(\n                                *deleteContext->pdfSession, path)",
        "BookActions::deletePdfBook(path)",
    )
    expect_rejected(
        "disabled reusable PDF directory workspace",
        "src/activities/home/FileBrowserActivity.cpp",
        "PdfDeleteUtils::makeDirectoryDeleteSessionNoThrow();",
        "{};",
    )
    expect_rejected(
        "changed baseline ordinary metadata API",
        "src/activities/home/BookActions.h",
        "void clearFileMetadata(const std::string& fullPath);",
        "void clearFileMetadata(std::string_view fullPath);",
    )
    expect_rejected(
        "reintroduced owning directory metadata callback API",
        "src/activities/home/BookActions.h",
        "bool clearDirectoryLegacyMetadataNoPathAlloc(std::string_view fullPath);",
        "bool clearDirectoryLegacyMetadataNoPathAlloc(const std::string& fullPath);",
    )
    expect_rejected(
        "discarded aggregate directory metadata cleanup result",
        "src/activities/home/BookActions.cpp",
        "success = cacheDeleted && bookmarksDeleted && clippingsDeleted;",
        "success = true;",
    )
    expect_rejected(
        "discarded result-bearing metadata callback type",
        "src/util/PdfDirectoryDeleteScan.h",
        "bool (*clearLegacyMetadata)(void* context, std::string_view path) = nullptr;",
        "void (*clearLegacyMetadata)(void* context, std::string_view path) = nullptr;",
    )
    expect_rejected(
        "inverted legacy metadata callback failure aggregation",
        "src/util/PdfDirectoryDeleteScan.cpp",
        "if (!callbacks.clearLegacyMetadata(callbacks.context,",
        "if (callbacks.clearLegacyMetadata(callbacks.context,",
    )
    expect_rejected_last(
        "inverted strict metadata callback failure aggregation",
        "src/util/PdfDirectoryDeleteScan.cpp",
        "if (!callbacks.clearLegacyMetadata(callbacks.context,",
        "if (callbacks.clearLegacyMetadata(callbacks.context,",
    )
    expect_rejected(
        "discarded EPUB current-cache-wins semantics",
        "lib/Epub/Epub.cpp",
        "if (!Storage.exists(currentPath) && std::strcmp(legacyPath, currentPath) != 0 &&",
        "if (std::strcmp(legacyPath, currentPath) != 0 &&",
    )
    expect_rejected(
        "reported remaining EPUB legacy cache as clean",
        "lib/Epub/Epub.cpp",
        "return migrationComplete && !legacyRemains;",
        "return true;",
    )
    expect_rejected(
        "used current EPUB hash for the legacy cache path",
        "lib/Epub/Epub.cpp",
        "std::hash<std::string_view>{}(filepath)",
        "ZipFile::fnvHash64(filepath.data(), filepath.size())",
    )
    expect_rejected(
        "removed EPUB legacy cache migration",
        "lib/Epub/Epub.cpp",
        "Storage.rename(legacyPath, currentPath)",
        "Storage.removeDir(legacyPath)",
    )
    expect_rejected(
        "accepted a truncated 64-byte EPUB cache path",
        "lib/Epub/Epub.cpp",
        "static_cast<size_t>(written) < capacity",
        "static_cast<size_t>(written) <= capacity",
    )
    expect_rejected(
        "used CRC for the legacy bookmark compatibility path",
        "src/BookmarkStore.cpp",
        "std::hash<std::string_view>{}(filePath)",
        "static_cast<size_t>(crc)",
    )
    expect_rejected(
        "removed legacy bookmark path preflight",
        "src/BookmarkStore.cpp",
        "snprintf(legacyPath",
        "snprintf(currentPath",
    )
    expect_rejected(
        "reintroduced owning PDF bookmark cleanup",
        "src/util/PdfDeleteUtils.cpp",
        "BookmarkStore::deletePdfForFilePathNoPathAlloc(",
        "BookmarkStore::deleteForFilePath(",
    )
    expect_rejected(
        "reintroduced owning PDF clipping cleanup",
        "src/util/PdfDeleteUtils.cpp",
        "ClippingStore::deletePdfForFilePathNoPathAlloc(",
        "ClippingStore::deleteForFilePath(",
    )
    expect_rejected(
        "reintroduced unchecked PDF path owner",
        "src/util/PdfDeleteUtils.cpp",
        "Targets targets{};",
        "std::string sourcePath;\n  Targets targets{};",
    )
    expect_rejected(
        "reintroduced throwing reusable workspace allocation",
        "src/util/PdfDeleteUtils.cpp",
        "auto session = makeUniqueNoThrow<DirectoryDeleteSession>();",
        "auto session = std::make_unique<DirectoryDeleteSession>();",
    )
    expect_rejected_last(
        "changed clipping compatibility path hash",
        "src/ClippingStore.cpp",
        "uzlib_crc32(filePath.data(), static_cast<unsigned int>(filePath.size()), 0);",
        "static_cast<uint32_t>(std::hash<std::string_view>{}(filePath));",
    )
    expect_rejected_ignoring_whitespace(
        "reopened the legacy replay spool per record",
        "src/util/PdfDirectoryDeleteScan.cpp",
        "replayStatus = readOpenReplayRecord(",
        "replayStatus = readReplayRecord(",
    )
    expect_rejected(
        "reported strict post-commit replay failure as a hard failure",
        "src/util/PdfDirectoryDeleteScan.cpp",
        "return finishCommittedWithSpoolCleanup(status);",
        "return finishWithSpoolCleanup(status);",
    )
    expect_rejected(
        "discarded strict metadata callback failure",
        "src/util/PdfDirectoryDeleteScan.cpp",
        "status = Status::MetadataCleanupFailure;",
        "status = Status::Complete;",
    )
    expect_rejected_last(
        "reported strict post-commit replay close failure as hard",
        "src/util/PdfDirectoryDeleteScan.cpp",
        "status = Status::CloseFailure;",
        "return finishWithSpoolCleanup(Status::CloseFailure);",
    )
    expect_rejected(
        "discarded legacy metadata callback failure",
        "src/util/PdfDirectoryDeleteScan.cpp",
        "replayStatus = Status::MetadataCleanupFailure;",
        "replayStatus = Status::Complete;",
    )
    expect_rejected(
        "reported legacy post-commit replay close failure as hard",
        "src/util/PdfDirectoryDeleteScan.cpp",
        "replayStatus = Status::CloseFailure;",
        "return Status::CloseFailure;",
    )
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
        "PdfDirectoryDeleteScan::deletePdfDirectoryNoThrow(dirPath, callbacks)",
        "removedDirectoryDelete(dirPath, callbacks)",
    )
    expect_rejected(
        "sent pure legacy directory into PDF deletion",
        "src/util/PdfDirectoryDeleteScan.cpp",
        "return commitLegacyDirectoryDelete(rootPath, callbacks, discovery);",
        "return deletePdfDirectoryNoThrow(rootPath, callbacks);",
    )
    expect_rejected(
        "reintroduced a second legacy traversal",
        "src/util/PdfDirectoryDeleteScan.cpp",
        "return commitLegacyDirectoryDelete(rootPath, callbacks, discovery);",
        "return deleteLegacyDirectoryNoThrow(rootPath, callbacks);",
    )
    expect_rejected(
        "disabled one-shot traversal retry",
        "src/util/PdfDirectoryDeleteScan.cpp",
        "if (isRetryableLegacyDiscoveryFailure(discovery.status))",
        "if (false)",
    )
    expect_rejected(
        "disabled reusable no-throw discovery workspace",
        "src/util/PdfDirectoryDeleteScan.cpp",
        "auto workspace = makeUniqueNoThrow<LegacyDiscoveryWorkspace>();",
        "auto workspace = std::make_unique<LegacyDiscoveryWorkspace>();",
    )
    expect_rejected(
        "discarded positive PDF classification across retry",
        "src/util/PdfDirectoryDeleteScan.cpp",
        "retry.foundPdf = retry.foundPdf || discovery.foundPdf;",
        "retry.foundPdf = retry.foundPdf;",
    )
    expect_rejected(
        "discarded farther legacy metadata prefix",
        "src/util/PdfDirectoryDeleteScan.cpp",
        "retry.metadataSpool = discovery.metadataSpool;",
        "retry.metadataSpool = {};",
    )
    expect_rejected(
        "ignored positive-route legacy spool cleanup failure",
        "src/util/PdfDirectoryDeleteScan.cpp",
        "if (!cleanupLegacySpoolFiles()) {\n      discovery.status = Status::SpoolCleanupFailure;\n    }",
        "cleanupLegacySpoolFiles();",
    )
    expect_rejected(
        "lost PDF classification on close failure",
        "src/util/PdfDirectoryDeleteScan.cpp",
        "discovery.foundPdf = true;",
        "discovery.foundPdf = false;",
    )
    expect_rejected_ignoring_whitespace(
        "classified a PDF-suffixed directory as a PDF file",
        "src/util/PdfDirectoryDeleteScan.cpp",
        "if (!isDirectory &&\n        (endsWith(nameView, PdfDelete::kTombstoneSuffix) ||",
        "if (endsWith(nameView, PdfDelete::kTombstoneSuffix) ||",
    )
    expect_rejected_ignoring_whitespace(
        "committed after incomplete legacy discovery",
        "src/util/PdfDirectoryDeleteScan.cpp",
        "return abortIncompleteLegacyDiscovery(discovery);",
        "return commitLegacyDirectoryDelete(rootPath, callbacks, discovery);",
    )
    expect_rejected_ignoring_whitespace(
        "committed routed deletion after incomplete discovery",
        "src/util/PdfDirectoryDeleteScan.cpp",
        "return abortIncompleteLegacyDiscovery(discovery);",
        "return commitLegacyDirectoryDelete(rootPath, callbacks, discovery);",
        last=True,
    )
    expect_rejected(
        "reintroduced throwing metadata vector growth",
        "src/util/PdfDirectoryDeleteScan.cpp",
        "LegacyTreeDiscovery discovery;",
        "std::vector<std::string> metadataPaths;\n  LegacyTreeDiscovery discovery;",
    )
    expect_rejected_ignoring_whitespace(
        "opened legacy metadata spool with directory reader active",
        "src/util/PdfDirectoryDeleteScan.cpp",
        """if (metadataSpoolPath != nullptr && isLegacyMetadataPath(nameView)) {
      const uint64_t resumeOffset = static_cast<uint64_t>(directory.position());
      discovery.status = closeLegacyDiscovery(
          entry, directory, Status::Complete);""",
        """if (metadataSpoolPath != nullptr && isLegacyMetadataPath(nameView)) {
      const uint64_t resumeOffset = static_cast<uint64_t>(directory.position());
      discovery.status = Status::Complete;""",
    )
    expect_rejected(
        "disabled fallible legacy replay allocation",
        "src/util/PdfDirectoryDeleteScan.cpp",
        "replayPath = makeUniqueNoThrow<char[]>(",
        "replayPath = std::make_unique<char[]>(",
    )
    expect_rejected(
        "accepted a truncated inline simulator name",
        "src/util/PdfDirectoryDeleteScan.cpp",
        "length < sizeof(name.local) - 1U",
        "length <= sizeof(name.local) - 1U",
    )
    expect_rejected(
        "accepted a truncated grown simulator name",
        "src/util/PdfDirectoryDeleteScan.cpp",
        "length < capacity - 1U",
        "length <= capacity - 1U",
    )
    expect_rejected(
        "reallocated the long-name buffer for every entry",
        "src/util/PdfDirectoryDeleteScan.cpp",
        "if (!name.dynamic || name.dynamicCapacity < capacity)",
        "if (true)",
    )
    expect_rejected(
        "disabled long-name PDF routing",
        "src/util/PdfDirectoryDeleteScan.cpp",
        "isJournaledPdfPath(nameView)",
        "false",
    )
    expect_rejected(
        "disabled PDF-only recovery callback",
        "src/util/PdfDirectoryDeleteScan.cpp",
        "!callbacks.preparePdfDelete(callbacks.context)",
        "false",
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
        "status = validateSpool(rootPath, workspace->currentPath,",
        "status = removedValidateSpool(rootPath, workspace->currentPath,",
    )
    expect_rejected(
        "disabled spool root binding",
        "src/util/PdfDirectoryDeleteScan.cpp",
        "isRootBoundRecordPath(rootPath, path, kind)",
        "isJournaledPdfPath(path)",
    )
    expect_rejected(
        "disabled typed legacy metadata record",
        "src/util/PdfDirectoryDeleteScan.cpp",
        "SpoolRecordKind::LegacyMetadata);",
        "SpoolRecordKind::Pdf);",
    )
    expect_rejected(
        "disabled sealed maximum path binding",
        "src/util/PdfDirectoryDeleteScan.cpp",
        "maxPathBytes == spool.maxPathBytes",
        "true",
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
    expect_rejected_last(
        "disabled public-route simulator hidden tombstone fence",
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
