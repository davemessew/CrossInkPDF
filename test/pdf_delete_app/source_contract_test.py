from pathlib import Path
from typing import Mapping

ROOT = Path(__file__).resolve().parents[2]


def sources(overrides: Mapping[str, str] | None = None) -> dict[str, str]:
    overrides = overrides or {}
    paths = [
        "src/activities/home/FileBrowserActivity.cpp",
        "src/activities/home/RecentBooksActivity.cpp",
        "src/activities/home/RecentBooksGridActivity.cpp",
        "src/network/CrossPointWebServer.cpp",
        "src/main.cpp",
        "src/util/BookMoveUtils.cpp",
        "src/util/PdfDeleteUtils.cpp",
        "src/util/PdfDirectoryDeleteScan.cpp",
        "src/util/PdfDirectoryDeleteScan.h",
        "src/RecentBooksStore.cpp",
        "src/activities/home/BookActions.cpp",
    ]
    return {
        path: overrides.get(path, (ROOT / path).read_text(encoding="utf-8"))
        for path in paths
    }


def require(texts: Mapping[str, str], path: str, needle: str) -> None:
    text = texts[path]
    if needle not in text:
        raise AssertionError(f"{path}: missing source contract: {needle}")


def require_absent(texts: Mapping[str, str], path: str, needle: str) -> None:
    if needle in texts[path]:
        raise AssertionError(f"{path}: forbidden source contract: {needle}")


def require_order(texts: Mapping[str, str], path: str, *needles: str) -> None:
    text = texts[path]
    positions = [text.find(needle) for needle in needles]
    if any(position < 0 for position in positions) or positions != sorted(positions):
        raise AssertionError(f"{path}: source order contract failed: {needles}")


def require_function_order(
    texts: Mapping[str, str], path: str, marker: str, *needles: str
) -> None:
    text = texts[path]
    start = text.find(marker)
    opening = text.find("{", start)
    if start < 0 or opening < 0:
        raise AssertionError(f"{path}: missing function marker: {marker}")
    depth = 0
    end = -1
    for index in range(opening, len(text)):
        if text[index] == "{":
            depth += 1
        elif text[index] == "}":
            depth -= 1
            if depth == 0:
                end = index + 1
                break
    if end < 0:
        raise AssertionError(f"{path}: unterminated function marker: {marker}")
    body = text[start:end]
    positions = [body.find(needle) for needle in needles]
    if any(position < 0 for position in positions) or positions != sorted(positions):
        raise AssertionError(
            f"{path}: function order contract failed for {marker}: {needles}"
        )


def function_text(texts: Mapping[str, str], path: str, marker: str) -> str:
    text = texts[path]
    start = text.find(marker)
    opening = text.find("{", start)
    if start < 0 or opening < 0:
        raise AssertionError(f"{path}: missing function marker: {marker}")
    depth = 0
    for index in range(opening, len(text)):
        if text[index] == "{":
            depth += 1
        elif text[index] == "}":
            depth -= 1
            if depth == 0:
                return text[start : index + 1]
    raise AssertionError(f"{path}: unterminated function marker: {marker}")


def validate(texts: Mapping[str, str]) -> None:
    require(
        texts,
        "src/activities/home/FileBrowserActivity.cpp",
        """if (FsHelpers::hasPdfExtension(fullPath)) {
      if (!BookActions::deletePdfBook(fullPath))""",
    )
    require(
        texts,
        "src/activities/home/FileBrowserActivity.cpp",
        "return path != nullptr && BookActions::deletePdfBook(path);",
    )
    require(
        texts,
        "src/activities/home/FileBrowserActivity.cpp",
        "void clearDirectoryLegacyMetadata(void*, const std::string& path)",
    )
    for path in [
        "src/activities/home/RecentBooksActivity.cpp",
        "src/activities/home/RecentBooksGridActivity.cpp",
    ]:
        require(
            texts,
            path,
            """if (FsHelpers::hasPdfExtension(path)) {
      if (!BookActions::deletePdfBook(path))""",
        )
    for path in [
        "src/activities/home/RecentBooksActivity.cpp",
        "src/activities/home/RecentBooksGridActivity.cpp",
    ]:
        require_absent(texts, path, '#include "util/PdfDeleteUtils.h"')
    for path in [
        "src/activities/home/FileBrowserActivity.cpp",
        "src/activities/home/RecentBooksActivity.cpp",
        "src/activities/home/RecentBooksGridActivity.cpp",
    ]:
        require_absent(texts, path, "PdfDeleteUtils::deletePdfBook")
    require(texts, "src/network/CrossPointWebServer.cpp", "PdfDeleteUtils::deletePdfBook(itemPath.c_str())")
    require(texts, "src/main.cpp", "PdfDeleteUtils::recoverPendingPdfDelete()")
    require(texts, "src/util/BookMoveUtils.cpp", "PdfDeleteUtils::mutationFenceForPath(oldPath)")
    require(texts, "src/RecentBooksStore.cpp", "RecentBooksStore::removeByPathDurably")
    require_absent(
        texts,
        "src/activities/home/FileBrowserActivity.cpp",
        "collectLegacyMetadataPathsRecursively",
    )
    require_absent(
        texts,
        "src/activities/home/FileBrowserActivity.cpp",
        "bool hasLegacyFileMetadata",
    )
    legacy_discovery = function_text(
        texts,
        "src/util/PdfDirectoryDeleteScan.cpp",
        "Status discoverLegacyTree",
    )
    for legacy_extension in [
        "hasEpubExtension",
        "hasXtcExtension",
        "hasTxtExtension",
        "hasMarkdownExtension",
    ]:
        require(
            {"legacy": legacy_discovery},
            "legacy",
            legacy_extension,
        )
    journaled_pdf_predicate = function_text(
        texts,
        "src/util/PdfDirectoryDeleteScan.cpp",
        "bool isJournaledPdfPath(const std::string_view path) {",
    )
    require({"pdf": journaled_pdf_predicate}, "pdf", "hasPdfExtension")
    for legacy_extension in [
        "hasEpubExtension",
        "hasXtcExtension",
        "hasTxtExtension",
        "hasMarkdownExtension",
    ]:
        require_absent({"pdf": journaled_pdf_predicate}, "pdf", legacy_extension)

    # The pre-PDF statements remain together inside the explicit else blocks.
    require(
        texts,
        "src/activities/home/FileBrowserActivity.cpp",
        """    } else {
      BookActions::clearFileMetadata(fullPath);
      if (!Storage.remove(fullPath.c_str())) {
        LOG_ERR("FileBrowser", "Failed to delete file: %s", fullPath.c_str());
        return;
      }
    }""",
    )
    for path, tag, marker in [
        (
            "src/activities/home/RecentBooksActivity.cpp",
            "RBA",
            "void RecentBooksActivity::promptDeleteBook",
        ),
        (
            "src/activities/home/RecentBooksGridActivity.cpp",
            "RBGA",
            "void RecentBooksGridActivity::promptDeleteBook",
        ),
    ]:
        require_function_order(
            texts,
            path,
            marker,
            "BookActions::clearFileMetadata(path)",
            "Storage.remove(path.c_str())",
            f'LOG_ERR("{tag}", "Failed to delete file: %s", path.c_str())',
            "RECENT_BOOKS.removeByPath(path)",
        )
    require(
        texts,
        "src/network/CrossPointWebServer.cpp",
        """      } else {
        success = Storage.remove(itemPath.c_str());
        clearBookCache(itemPath.c_str());
      }""",
    )

    require_order(
        texts,
        "src/main.cpp",
        "RECENT_BOOKS.loadFromFile();",
        "PdfDeleteUtils::recoverPendingPdfDelete()",
        "BookMoveUtils::recoverPendingBookMove()",
        "BookMoveUtils::migrationCacheHash(openBookPath",
    )
    require_order(
        texts,
        "src/util/BookMoveUtils.cpp",
        "journalPresence.store(JournalPresence::MoveStarting",
        "PdfDeleteUtils::mutationFenceForPath(oldPath)",
        "makeUniqueNoThrow<BookMoveSession>()",
    )
    require_order(
        texts,
        "src/util/PdfDeleteUtils.cpp",
        "coordinator.begin(request)",
        "const Result result = resultFor(coordinator.recover());",
    )
    require(texts, "src/util/PdfDeleteUtils.cpp", "Storage.removeDir(workspace.cache)")
    require(texts, "src/util/PdfDeleteUtils.cpp", "BookmarkStore::deleteForFilePath(workspace.sourcePath, \"pdf\")")
    require(texts, "src/util/PdfDeleteUtils.cpp", "ClippingStore::deleteForFilePath(workspace.sourcePath, \"pdf\")")
    require(texts, "src/util/PdfDeleteUtils.cpp", "RECENT_BOOKS.removeByPathDurably(workspace.sourcePath)")
    require_function_order(
        texts,
        "src/activities/home/FileBrowserActivity.cpp",
        "void FileBrowserActivity::promptDeleteDirectory",
        "PdfDeleteUtils::recoverPendingPdfDelete()",
        "PdfDirectoryDeleteScan::DeleteCallbacks callbacks",
        "PdfDirectoryDeleteScan::deleteDirectoryNoThrow(dirPath, callbacks)",
    )
    require_function_order(
        texts,
        "src/network/CrossPointWebServer.cpp",
        "void CrossPointWebServer::handleDelete() const",
        "if (!itemPath.startsWith(\"/\"))",
        "if (itemPath.endsWith(PdfDelete::kTombstoneSuffix))",
        "if (isProtectedPath(itemPath))",
        "if (!Storage.exists(itemPath.c_str()))",
        "HalFile f = Storage.open(itemPath.c_str())",
    )
    require(
        texts,
        "src/util/PdfDirectoryDeleteScan.cpp",
        "auto workspace = makeUniqueNoThrow<LegacyWalkWorkspace>();",
    )
    require_absent(
        texts,
        "src/util/PdfDirectoryDeleteScan.cpp",
        "makeUniqueNoThrow<Workspace>()",
    )
    require(
        texts,
        "src/util/PdfDirectoryDeleteScan.cpp",
        "endsWith(name, PdfDelete::kTombstoneSuffix)",
    )
    require(
        texts,
        "src/util/PdfDirectoryDeleteScan.cpp",
        "const DirectoryNextResult next =\n          nextDirectoryEntry(workspace.directory, workspace.entry);",
    )
    require(
        texts,
        "src/util/PdfDirectoryDeleteScan.cpp",
        "return errno == 0 ? DirectoryNextResult::End : DirectoryNextResult::Error;",
    )
    require_function_order(
        texts,
        "src/util/PdfDirectoryDeleteScan.cpp",
        "Status deleteDirectoryNoThrow",
        "cleanupSpoolFiles()",
        "simulatorHiddenTombstoneStatus(rootPath)",
        "makeUniqueNoThrow<LegacyWalkWorkspace>()",
        "discoverLegacyTree(*workspace, rootPath, legacyPaths, spool)",
        "sealSpool(*spool)",
        "validateSpool(rootPath, *workspace, *spool)",
        "readReplayRecord(rootPath, *workspace, *spool)",
        "callbacks.deletePdf(callbacks.context, workspace->currentPath)",
        "Storage.removeDir(rootPath.c_str())",
        "callbacks.clearLegacyMetadata(callbacks.context, path)",
    )
    require(
        texts,
        "src/util/PdfDirectoryDeleteScan.cpp",
        "std::filesystem::recursive_directory_iterator current(physicalRoot, error);",
    )
    directory_delete = function_text(
        texts,
        "src/util/PdfDirectoryDeleteScan.cpp",
        "Status deleteDirectoryNoThrow",
    )
    if directory_delete.count("cleanupSpoolFiles()") < 2:
        raise AssertionError(
            "directory delete must clean stale spool files before work and after replay"
        )
    discovery = function_text(
        texts,
        "src/util/PdfDirectoryDeleteScan.cpp",
        "Status discoverLegacyTree",
    )
    pdf_branch_start = discovery.find("if (isJournaledPdfPath(name))")
    if pdf_branch_start < 0:
        raise AssertionError("directory discovery is missing the PDF branch")
    pdf_branch = discovery[pdf_branch_start:]
    discovery_needles = [
        "if (!closeLegacyEnumeration(workspace, &status)) return status;",
        "if (!spool)",
        "spool = makeUniqueNoThrow<SpoolWorkspace>();",
        "status = createSpool(rootPath);",
        "status = appendSpoolRecord(",
    ]
    positions = [pdf_branch.find(needle) for needle in discovery_needles]
    if any(position < 0 for position in positions) or positions != sorted(positions):
        raise AssertionError(
            "PDF spool must be deferred until the first PDF and opened with no directory reader"
        )
    if texts["src/util/PdfDirectoryDeleteScan.cpp"].count("createSpool(rootPath)") != 1:
        raise AssertionError("PDF spool creation must have one deferred call site")
    require_function_order(
        texts,
        "src/util/PdfDirectoryDeleteScan.cpp",
        "Status validateSpool",
        "file.fileSize64()",
        "SPOOL_HEADER_MAGIC",
        "SPOOL_FOOTER_MAGIC",
        "actualRecordsBytes",
        "isRootBoundPdfPath(rootPath, path)",
        "actualRecordsCrc == expectedRecordsCrc",
        "file.position() == footerOffset",
    )
    replay = function_text(
        texts,
        "src/util/PdfDirectoryDeleteScan.cpp",
        "Status readReplayRecord",
    )
    require({"replay": replay}, "replay", "readExact(file")
    replay_validated = replay[replay.find("isRootBoundPdfPath(rootPath, path)") :]
    replay_needles = [
        "isRootBoundPdfPath(rootPath, path)",
        "const bool closed = file.close();",
        "return Status::Complete",
    ]
    replay_positions = [
        replay_validated.find(needle) for needle in replay_needles
    ]
    if any(position < 0 for position in replay_positions) or replay_positions != sorted(
        replay_positions
    ):
        raise AssertionError("replay must close the spool before returning a PDF path")
    require(
        texts,
        "src/util/PdfDirectoryDeleteScan.h",
        "Status deleteDirectoryNoThrow(const std::string& rootPath,",
    )
    for obsolete in [
        "kMaxDepth",
        "kMaxDirectories",
        "kMaxMetadataPaths",
        "kPathArenaCapacity",
        "struct Workspace",
        "scanNoThrow",
        "metadataPathAt",
    ]:
        require_absent(texts, "src/util/PdfDirectoryDeleteScan.h", obsolete)
    require(
        texts,
        "src/util/PdfDirectoryDeleteScan.cpp",
        "static_assert(sizeof(LegacyWalkWorkspace) <= 2U * 1024U);",
    )
    require(
        texts,
        "src/util/PdfDirectoryDeleteScan.cpp",
        "static_assert(sizeof(SpoolWorkspace) <= 32U);",
    )
    require(
        texts,
        "src/RecentBooksStore.cpp",
        """BookMoveDurableFile::restoreCanonicalForRead(
      RecentBooksStore::getFilePath(), RECENT_BOOKS_DELETE_TEMP,
      RECENT_BOOKS_DELETE_BACKUP)""",
    )
    require_function_order(
        texts,
        "src/RecentBooksStore.cpp",
        "bool RecentBooksStore::removeByPathDurably",
        "if (!prepareRecentDeleteStateForRead())",
        "makeUniqueNoThrow<RecentRemovalScratch>()",
        "readStrictRecentDeleteDocument(doc, scratch->json)",
        "serializeRecentDeleteDocument(doc, scratch->json)",
        "BookMoveDurableFile::replace(",
        "recentBooks.erase(found)",
    )
    require_function_order(
        texts,
        "src/RecentBooksStore.cpp",
        "bool readStrictRecentDeleteDocument",
        "file.fileSize64()",
        "fileSize > RECENT_BOOKS_DELETE_JSON_MAX_BYTES",
        "reserveRecentDeleteString(json, static_cast<size_t>(fileSize))",
        "file.read(chunk, wanted)",
        "json.length() != fileSize",
        "deserializeJson(doc, json)",
        "doc.overflowed()",
        "root.is<JsonObjectConst>()",
        "books.is<JsonArrayConst>()",
        "path.is<const char*>()",
    )
    require_function_order(
        texts,
        "src/RecentBooksStore.cpp",
        "bool serializeRecentDeleteDocument",
        "if (doc.overflowed())",
        "const size_t measured = measureJson(doc);",
        "measured > RECENT_BOOKS_DELETE_JSON_MAX_BYTES",
        "reserveRecentDeleteString(json, measured)",
        "const size_t written = serializeJson(doc, writer);",
        "writer.complete()",
        "written == measured",
        "json.length() == measured",
    )
    require(
        texts,
        "src/RecentBooksStore.cpp",
        "value.reserve(static_cast<unsigned int>(capacity))",
    )
    require(
        texts,
        "src/RecentBooksStore.cpp",
        "Tolerate a missing/invalid 'books' key (treat as empty list)",
    )
    require_function_order(
        texts,
        "src/RecentBooksStore.cpp",
        "bool RecentBooksStore::loadFromFile()",
        "if (!prepareRecentDeleteStateForRead())",
        "Storage.exists(RECENT_BOOKS_MOVE_BACKUP)",
        "PersistableStore<RecentBooksStore>::loadFromFile()",
    )
    if "clearBookCachePreservingUserState" in texts["src/util/PdfDeleteUtils.cpp"]:
        raise AssertionError("PDF full deletion must not use the preserving cache-clear path")
    require(
        texts,
        "src/activities/home/BookActions.cpp",
        """// PDF_BOOK_ACTIONS_PARITY_BEGIN: delete
bool deletePdfBook(const std::string& fullPath) {
  if (!FsHelpers::hasPdfExtension(fullPath)) return false;
  return PdfDeleteUtils::deletePdfBook(fullPath) == PdfDeleteUtils::Result::Complete;
}
// PDF_BOOK_ACTIONS_PARITY_END: delete""",
    )


def main() -> None:
    validate(sources())
    print("PDF deletion source contract passed")


if __name__ == "__main__":
    main()
