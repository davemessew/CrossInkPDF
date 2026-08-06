import re
from pathlib import Path
from typing import Mapping

ROOT = Path(__file__).resolve().parents[2]

CONTRACT_TOKEN = re.compile(
    r'"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|'
    r"[A-Za-z_]\w*|\d+(?:\.\d+)?[A-Za-z_]*|"
    r"::|->|&&|\|\||==|!=|<=|>=|<<|>>|\+\+|--|[^\s]"
)


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
        "src/util/PdfDeleteUtils.h",
        "src/util/PdfDirectoryDeleteScan.cpp",
        "src/util/PdfDirectoryDeleteScan.h",
        "src/RecentBooksStore.cpp",
        "src/RecentBooksStore.h",
        "src/activities/home/BookActions.cpp",
        "src/activities/home/BookActions.h",
        "src/BookmarkStore.cpp",
        "src/BookmarkStore.h",
        "src/ClippingStore.cpp",
        "src/ClippingStore.h",
        "lib/Epub/Epub.cpp",
        "lib/Epub/Epub.h",
    ]
    return {
        path: overrides.get(path, (ROOT / path).read_text(encoding="utf-8"))
        for path in paths
    }


def require(texts: Mapping[str, str], path: str, needle: str) -> None:
    text = texts[path]
    if needle not in text:
        raise AssertionError(f"{path}: missing source contract: {needle}")


def contract_pattern(fragment: str) -> re.Pattern[str]:
    tokens = CONTRACT_TOKEN.findall(fragment)
    if not tokens:
        raise ValueError("source contract fragment must contain syntax")
    return re.compile(r"\s*".join(re.escape(token) for token in tokens))


def require_ignoring_whitespace(
    texts: Mapping[str, str], path: str, fragment: str
) -> None:
    if contract_pattern(fragment).search(texts[path]) is None:
        raise AssertionError(f"{path}: missing source contract: {fragment}")


def require_order_ignoring_whitespace(
    text: str, description: str, *fragments: str
) -> None:
    matches = [contract_pattern(fragment).search(text) for fragment in fragments]
    positions = [match.start() if match is not None else -1 for match in matches]
    if any(position < 0 for position in positions) or positions != sorted(positions):
        raise AssertionError(description)


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
        "src/util/PdfDirectoryDeleteScan.h",
        "CommittedWithCleanupWarning",
    )
    for forbidden in [
        "std::vector<std::string>",
        "metadataPaths.emplace_back",
        "legacyPaths.push_back",
        "std::string childPath",
    ]:
        require_absent(
            texts,
            "src/util/PdfDirectoryDeleteScan.cpp",
            forbidden,
        )
    require(
        texts,
        "src/activities/home/FileBrowserActivity.cpp",
        """if (FsHelpers::hasPdfExtension(fullPath)) {
      if (!BookActions::deletePdfBook(fullPath))""",
    )
    directory_pdf_callback = function_text(
        texts,
        "src/activities/home/FileBrowserActivity.cpp",
        "bool deleteDirectoryPdf",
    )
    for needle in [
        "static_cast<DirectoryDeleteContext*>(context)",
        "deleteContext->pdfSession",
        "BookActions::deleteDirectoryPdfBookNoPathAlloc(",
        "*deleteContext->pdfSession, path",
    ]:
        require({"callback": directory_pdf_callback}, "callback", needle)
    for forbidden in ["BookActions::deletePdfBook(", "std::string(", "std::string{"]:
        require_absent({"callback": directory_pdf_callback}, "callback", forbidden)
    require(
        texts,
        "src/activities/home/FileBrowserActivity.cpp",
        "bool clearDirectoryLegacyMetadata(void*, const std::string_view path)",
    )
    clear_directory_metadata = function_text(
        texts,
        "src/activities/home/FileBrowserActivity.cpp",
        "bool clearDirectoryLegacyMetadata",
    )
    require(
        {"callback": clear_directory_metadata},
        "callback",
        "return BookActions::clearDirectoryLegacyMetadataNoPathAlloc(path);",
    )
    for owning_conversion in ["std::string(", "std::string{"]:
        require_absent(
            {"callback": clear_directory_metadata}, "callback", owning_conversion
        )
    require(
        texts,
        "src/activities/home/BookActions.h",
        "void clearFileMetadata(const std::string& fullPath);",
    )
    require(
        texts,
        "src/activities/home/BookActions.cpp",
        "void clearFileMetadata(const std::string& fullPath)",
    )
    ordinary_clear = function_text(
        texts,
        "src/activities/home/BookActions.cpp",
        "void clearFileMetadata(const std::string& fullPath)",
    )
    for needle in [
        'Epub(fullPath, "/.crosspoint").clearCache();',
        'BookmarkStore::deleteForFilePath(fullPath, "epub");',
        'ClippingStore::deleteForFilePath(fullPath, "epub");',
    ]:
        require({"ordinary": ordinary_clear}, "ordinary", needle)
    require_absent(
        {"ordinary": ordinary_clear}, "ordinary", "NoPathAlloc"
    )
    require(
        texts,
        "src/activities/home/BookActions.h",
        "bool clearDirectoryLegacyMetadataNoPathAlloc(std::string_view fullPath);",
    )
    require(
        texts,
        "src/activities/home/BookActions.h",
        "bool deleteDirectoryPdfBookNoPathAlloc(",
    )
    directory_pdf_action = function_text(
        texts,
        "src/activities/home/BookActions.cpp",
        "bool deleteDirectoryPdfBookNoPathAlloc",
    )
    for needle in [
        "PdfDeleteUtils::DirectoryDeleteSession& session",
        "const std::string_view fullPath",
        "FsHelpers::hasPdfExtension(fullPath)",
        "PdfDeleteUtils::deletePdfBookNoPathAlloc(session, fullPath)",
    ]:
        require({"action": directory_pdf_action}, "action", needle)
    for forbidden in ["std::string(", "PdfDeleteUtils::deletePdfBook(fullPath)"]:
        require_absent({"action": directory_pdf_action}, "action", forbidden)
    require(
        texts,
        "src/activities/home/BookActions.cpp",
        "bool clearDirectoryLegacyMetadataNoPathAlloc(const std::string_view fullPath)",
    )
    clear_file_metadata = function_text(
        texts,
        "src/activities/home/BookActions.cpp",
        "bool clearDirectoryLegacyMetadataNoPathAlloc(const std::string_view fullPath)",
    )
    for needle in [
        "bool success = true;",
        "const bool cacheDeleted =",
        "Epub::clearCacheForFilePathNoPathAlloc(fullPath, \"/.crosspoint\")",
        "const bool bookmarksDeleted =",
        "BookmarkStore::deleteLegacyForFilePathNoPathAlloc(fullPath, \"epub\")",
        "const bool clippingsDeleted =",
        "ClippingStore::deleteLegacyForFilePathNoPathAlloc(fullPath, \"epub\")",
        "success = cacheDeleted && bookmarksDeleted && clippingsDeleted;",
        "return success;",
    ]:
        require({"clear": clear_file_metadata}, "clear", needle)
    for owning_call in [
        "std::string(",
        "Epub epub(",
        "::deleteForFilePath(",
    ]:
        require_absent({"clear": clear_file_metadata}, "clear", owning_call)
    for path, signature in [
        (
            "lib/Epub/Epub.h",
            "static bool clearCacheForFilePathNoPathAlloc(std::string_view filepath, const char* cacheDir);",
        ),
        (
            "src/BookmarkStore.h",
            "static bool deleteLegacyForFilePathNoPathAlloc(std::string_view filePath,",
        ),
        (
            "src/ClippingStore.h",
            "static bool deleteLegacyForFilePathNoPathAlloc(std::string_view filePath,",
        ),
    ]:
        require(texts, path, signature)
    for path, marker in [
        ("lib/Epub/Epub.cpp", "bool Epub::clearCacheForFilePathNoPathAlloc"),
        ("src/BookmarkStore.cpp", "bool BookmarkStore::deleteLegacyForFilePathNoPathAlloc"),
        ("src/ClippingStore.cpp", "bool ClippingStore::deleteLegacyForFilePathNoPathAlloc"),
    ]:
        no_path_alloc_delete = function_text(texts, path, marker)
        require({"delete": no_path_alloc_delete}, "delete", "[64]")
        for owning_path in ["std::string(", "std::string{"]:
            require_absent(
                {"delete": no_path_alloc_delete}, "delete", owning_path
            )
    epub_no_path = function_text(
        texts,
        "lib/Epub/Epub.cpp",
        "bool Epub::clearCacheForFilePathNoPathAlloc",
    )
    epub_no_path_needles = [
        "char currentPath[64]",
        "char legacyPath[64]",
        "formatEpubCachePath(",
        "ZipFile::fnvHash64(filepath.data(), filepath.size())",
        "formatEpubCachePath(legacyPath",
        "std::hash<std::string_view>{}(filepath)",
        "if (!Storage.exists(currentPath) && std::strcmp(legacyPath, currentPath) != 0 &&",
        "Storage.exists(legacyPath)",
        "Storage.rename(legacyPath, currentPath)",
        "if (Storage.exists(currentPath))",
        "Storage.removeDir(currentPath)",
        "const bool legacyRemains",
        "return migrationComplete && !legacyRemains;",
    ]
    epub_no_path_positions = [
        epub_no_path.find(needle) for needle in epub_no_path_needles
    ]
    if any(position < 0 for position in epub_no_path_positions) or (
        epub_no_path_positions != sorted(epub_no_path_positions)
    ):
        raise AssertionError(
            "EPUB no-path-allocation cleanup must preserve migrate-then-delete semantics"
        )
    epub_formatter = function_text(
        texts, "lib/Epub/Epub.cpp", "bool formatEpubCachePath"
    )
    for needle in [
        "output == nullptr",
        "capacity == 0U",
        "cacheDir == nullptr",
        'snprintf(output, capacity, "%s/epub_%llu"',
        "static_cast<unsigned long long>(hash)",
        "static_cast<size_t>(written) < capacity",
    ]:
        require({"formatter": epub_formatter}, "formatter", needle)
    require(
        texts,
        "lib/Epub/Epub.h",
        "static bool formatCachePathForTest(char* output, size_t capacity, const char* cacheDir,",
    )
    bookmark_no_path = function_text(
        texts,
        "src/BookmarkStore.cpp",
        "bool BookmarkStore::deleteLegacyForFilePathNoPathAlloc",
    )
    bookmark_no_path_needles = [
        "char currentPath[64]",
        "char legacyPath[64]",
        "uzlib_crc32(filePath.data()",
        "std::hash<std::string_view>{}(filePath)",
        "snprintf(currentPath",
        "snprintf(legacyPath",
        "currentWritten < 0",
        "legacyWritten < 0",
        "Storage.exists(currentPath)",
        "deleteBookmarkStorePathNoPathAlloc(currentPath",
        "Storage.exists(legacyPath)",
        "deleteBookmarkStorePathNoPathAlloc(legacyPath",
    ]
    bookmark_no_path_positions = [
        bookmark_no_path.find(needle) for needle in bookmark_no_path_needles
    ]
    if any(position < 0 for position in bookmark_no_path_positions) or (
        bookmark_no_path_positions != sorted(bookmark_no_path_positions)
    ):
        raise AssertionError(
            "bookmark no-path-allocation cleanup must preflight exact current and legacy paths"
        )
    clipping_no_path = function_text(
        texts,
        "src/ClippingStore.cpp",
        "bool ClippingStore::deleteLegacyForFilePathNoPathAlloc",
    )
    require({"clipping": clipping_no_path}, "clipping", "uzlib_crc32(filePath.data()")
    require_function_order(
        {"src/ClippingStore.cpp": clipping_no_path},
        "src/ClippingStore.cpp",
        "bool ClippingStore::deleteLegacyForFilePathNoPathAlloc",
        "char path[64]",
        "snprintf(path",
        "static_cast<size_t>(written) >= sizeof(path)",
        "deleteStorePathNoPathAlloc(path",
    )
    bookmark_pdf_no_path = function_text(
        texts,
        "src/BookmarkStore.cpp",
        "bool BookmarkStore::deletePdfForFilePathNoPathAlloc",
    )
    for needle in [
        "char currentPath[64]",
        "char legacyPath[64]",
        "char artifactPath[68]",
        "uzlib_crc32(filePath.data()",
        "std::hash<std::string_view>{}(filePath)",
    ]:
        require({"bookmark": bookmark_pdf_no_path}, "bookmark", needle)
    require_function_order(
        {"src/BookmarkStore.cpp": bookmark_pdf_no_path},
        "src/BookmarkStore.cpp",
        "bool BookmarkStore::deletePdfForFilePathNoPathAlloc",
        "currentWritten < 0",
        "legacyWritten < 0",
        "deleteArtifact(currentPath, PDF_TRANSACTION_BACKUP_SUFFIX",
        "deleteArtifact(currentPath, PDF_TRANSACTION_TEMP_SUFFIX",
        "deleteArtifact(legacyPath, PDF_TRANSACTION_BACKUP_SUFFIX",
        "deleteArtifact(legacyPath, PDF_TRANSACTION_TEMP_SUFFIX",
        "deleteBookmarkStorePathNoPathAlloc(legacyPath",
        "deleteBookmarkStorePathNoPathAlloc(currentPath",
    )
    clipping_pdf_no_path = function_text(
        texts,
        "src/ClippingStore.cpp",
        "bool ClippingStore::deletePdfForFilePathNoPathAlloc",
    )
    for needle in [
        "char canonicalPath[64]",
        "char artifactPath[68]",
        "uzlib_crc32(filePath.data()",
    ]:
        require({"clipping": clipping_pdf_no_path}, "clipping", needle)
    require_function_order(
        {"src/ClippingStore.cpp": clipping_pdf_no_path},
        "src/ClippingStore.cpp",
        "bool ClippingStore::deletePdfForFilePathNoPathAlloc",
        "canonicalWritten < 0",
        "deleteArtifact(PDF_TRANSACTION_BACKUP_SUFFIX",
        "deleteArtifact(PDF_TRANSACTION_TEMP_SUFFIX",
        "deleteStorePathNoPathAlloc(canonicalPath",
    )
    for body in [bookmark_pdf_no_path, clipping_pdf_no_path]:
        for owning_path in ["std::string(", "std::string{"]:
            require_absent({"delete": body}, "delete", owning_path)
    require(
        texts,
        "src/util/PdfDirectoryDeleteScan.h",
        "bool (*clearLegacyMetadata)(void* context, std::string_view path) = nullptr;",
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
        "src/activities/home/RecentBooksActivity.cpp",
        "src/activities/home/RecentBooksGridActivity.cpp",
    ]:
        require_absent(texts, path, "PdfDeleteUtils::deletePdfBook")
    require(texts, "src/network/CrossPointWebServer.cpp", "PdfDeleteUtils::deletePdfBook(itemPath.c_str())")
    require(texts, "src/main.cpp", "PdfDeleteUtils::recoverPendingPdfDelete()")
    require(texts, "src/util/BookMoveUtils.cpp", "PdfDeleteUtils::mutationFenceForPath(oldPath)")
    require(texts, "src/RecentBooksStore.cpp", "RecentBooksStore::removeByPathDurably")
    for path, signature in [
        (
            "src/util/PdfDeleteUtils.h",
            "DirectoryDeleteSessionPtr makeDirectoryDeleteSessionNoThrow();",
        ),
        (
            "src/util/PdfDeleteUtils.h",
            "Result deletePdfBookNoPathAlloc(DirectoryDeleteSession& session,",
        ),
        (
            "src/RecentBooksStore.h",
            "bool removeByPathDurablyNoPathAlloc(std::string_view path);",
        ),
        (
            "src/BookmarkStore.h",
            "static bool deletePdfForFilePathNoPathAlloc(std::string_view filePath);",
        ),
        (
            "src/ClippingStore.h",
            "static bool deletePdfForFilePathNoPathAlloc(std::string_view filePath);",
        ),
    ]:
        require(texts, path, signature)
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
    require({"legacy": legacy_discovery}, "legacy", "isLegacyMetadataPath(name)")
    legacy_metadata_predicate = function_text(
        texts,
        "src/util/PdfDirectoryDeleteScan.cpp",
        "bool isLegacyMetadataPath",
    )
    for legacy_extension in [
        "hasEpubExtension",
        "hasXtcExtension",
        "hasTxtExtension",
        "hasMarkdownExtension",
    ]:
        require(
            {"legacy": legacy_metadata_predicate},
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
    require_function_order(
        texts,
        "src/util/PdfDeleteUtils.cpp",
        "PdfDeleteUtils::Result deleteWithSession",
        "coordinator.begin(request)",
        "const PdfDeleteUtils::Result result = resultFor(coordinator.recover());",
    )
    require(texts, "src/util/PdfDeleteUtils.cpp", "Storage.removeDir(workspace.cache)")
    require(texts, "src/util/PdfDeleteUtils.cpp", "BookmarkStore::deletePdfForFilePathNoPathAlloc(")
    require(texts, "src/util/PdfDeleteUtils.cpp", "ClippingStore::deletePdfForFilePathNoPathAlloc(")
    require(texts, "src/util/PdfDeleteUtils.cpp", "RECENT_BOOKS.removeByPathDurablyNoPathAlloc(")
    require_absent(texts, "src/util/PdfDeleteUtils.cpp", "std::string sourcePath;")
    require_absent(texts, "src/util/PdfDeleteUtils.cpp", "workspace.sourcePath.assign")
    require(
        texts,
        "src/util/PdfDeleteUtils.cpp",
        "auto session = makeUniqueNoThrow<DirectoryDeleteSession>();",
    )
    require_function_order(
        texts,
        "src/util/PdfDeleteUtils.cpp",
        "Result deletePdfBookNoPathAlloc",
        "deleteWithSession(session.session, sourcePath)",
    )
    require_function_order(
        texts,
        "src/activities/home/FileBrowserActivity.cpp",
        "bool prepareDirectoryPdfDelete",
        "PdfDeleteUtils::recoverPendingPdfDelete()",
        "PdfDeleteUtils::Result::NoPendingDelete",
        "PdfDeleteUtils::makeDirectoryDeleteSessionNoThrow()",
        "return deleteContext->pdfSession != nullptr;",
    )
    require_function_order(
        texts,
        "src/activities/home/FileBrowserActivity.cpp",
        "bool isPdfDirectoryDeleteEntry",
        "FsHelpers::hasPdfExtension(name)",
        "PdfDelete::kTombstoneSuffix",
        "name.compare(name.size() - tombstoneSuffix.size(), tombstoneSuffix.size(), tombstoneSuffix) != 0",
        "name.remove_suffix(tombstoneSuffix.size())",
        "return FsHelpers::hasPdfExtension(name);",
    )
    require_function_order(
        texts,
        "src/activities/home/FileBrowserActivity.cpp",
        "bool closeMetadataEntry",
        "!entry.isOpen()",
        "entry.close()",
    )
    require_function_order(
        texts,
        "src/activities/home/FileBrowserActivity.cpp",
        "DirectoryMetadataScanStatus collectMetadataPathsRecursively",
        "isPdfDirectoryDeleteEntry(nameView)",
        "if (isDirectory)",
        "if (!closeMetadataEntry(file))",
        "collectMetadataPathsRecursively(childPath, paths)",
    )
    require_function_order(
        texts,
        "src/activities/home/FileBrowserActivity.cpp",
        "void FileBrowserActivity::promptDeleteDirectory",
        "std::vector<std::string> metadataPaths;",
        "collectMetadataPathsRecursively(dirPath, metadataPaths)",
        "scanStatus == DirectoryMetadataScanStatus::Failed",
        "scanStatus == DirectoryMetadataScanStatus::PdfFound",
        "std::vector<std::string>().swap(metadataPaths);",
        "prepareDirectoryPdfDelete(&deleteContext)",
        "PdfDirectoryDeleteScan::DeleteCallbacks callbacks",
        "PdfDirectoryDeleteScan::deletePdfDirectoryNoThrow(dirPath, callbacks)",
        "Storage.removeDir(dirPath.c_str())",
        "BookActions::clearFileMetadata(metadataPath);",
    )
    require_ignoring_whitespace(
        texts,
        "src/activities/home/FileBrowserActivity.cpp",
        """
        if (scanStatus == DirectoryMetadataScanStatus::Failed) {
          LOG_ERR("FileBrowser", "Directory scan failed before delete: %s", dirPath.c_str());
          return;
        }
        """,
    )
    require_absent(
        texts,
        "src/activities/home/FileBrowserActivity.cpp",
        "PdfDirectoryDeleteScan::containsPdfNoThrow(dirPath, &containsPdf)",
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
    require_function_order(
        texts,
        "src/util/PdfDirectoryDeleteScan.cpp",
        "Status containsPdfNoThrow",
        "simulatorHiddenTombstoneStatus(rootPath)",
        "Status::ReservedTombstone",
        "makeUniqueNoThrow<LegacyDiscoveryWorkspace>()",
        "discoverLegacyTreeOnce(*workspace, rootPath, nullptr)",
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
    require_ignoring_whitespace(
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
        "Status deletePdfDirectoryNoThrow",
        "cleanupSpoolFiles()",
        "simulatorHiddenTombstoneStatus(rootPath)",
        "makeUniqueNoThrow<LegacyWalkWorkspace>()",
        "discoverLegacyTree(*workspace, rootPath, workspace->spool)",
        "sealSpool(*workspace->spool)",
        "validateSpool(rootPath, workspace->currentPath,",
        "readReplayRecord(",
        "callbacks.deletePdf(callbacks.context, workspace->currentPath)",
        "Storage.removeDir(rootPath.c_str())",
        "workspace->spool->replayOffset = SPOOL_HEADER_BYTES",
        "if (!callbacks.clearLegacyMetadata(callbacks.context,",
        "metadataCleanupComplete = false;",
    )
    require_function_order(
        texts,
        "src/util/PdfDirectoryDeleteScan.cpp",
        "Status finishCommittedWithSpoolCleanup",
        "cleanupSpoolFiles()",
        "Status::Complete",
        "Status::CommittedWithCleanupWarning",
    )
    require_function_order(
        texts,
        "src/activities/home/FileBrowserActivity.cpp",
        "void FileBrowserActivity::promptDeleteDirectory",
        "PdfDirectoryDeleteScan::deletePdfDirectoryNoThrow(dirPath, callbacks)",
        "PdfDirectoryDeleteScan::Status::CommittedWithCleanupWarning",
        "Directory deleted, but metadata or scratch cleanup was incomplete",
        "} else if (status != PdfDirectoryDeleteScan::Status::Complete)",
        "loadFiles()",
        "requestUpdate(true)",
    )
    require(
        texts,
        "src/util/PdfDirectoryDeleteScan.cpp",
        "std::filesystem::recursive_directory_iterator current(physicalRoot, error);",
    )
    directory_delete = function_text(
        texts,
        "src/util/PdfDirectoryDeleteScan.cpp",
        "Status deletePdfDirectoryNoThrow",
    )
    if directory_delete.count("cleanupSpoolFiles()") < 1 or (
        "finishCommittedWithSpoolCleanup(" not in directory_delete
    ):
        raise AssertionError(
            "directory delete must clean stale spool files before work and after replay"
        )
    for forbidden in [
        "std::vector<std::string> legacyPaths;",
        "std::unique_ptr<SpoolWorkspace> spool;",
    ]:
        if forbidden in directory_delete:
            raise AssertionError(
                f"strict PDF coordinator left state on the task stack: {forbidden}"
            )
    strict_post_commit = directory_delete[
        directory_delete.find("if (!Storage.removeDir(rootPath.c_str()))") :
    ]
    strict_post_commit_needles = [
        "workspace->spool->replayOffset = SPOOL_HEADER_BYTES",
        "HalFile replayFile;",
        "openReplayFile(",
        "bool metadataCleanupComplete = true;",
        "readOpenReplayRecord(",
        "if (!callbacks.clearLegacyMetadata(callbacks.context,",
        "metadataCleanupComplete = false;",
        "replayFile.close()",
        "status = Status::CloseFailure",
        "status = Status::MetadataCleanupFailure",
        "finishCommittedWithSpoolCleanup(status)",
    ]
    strict_post_commit_positions = [
        strict_post_commit.find(needle) for needle in strict_post_commit_needles
    ]
    if any(position < 0 for position in strict_post_commit_positions) or (
        strict_post_commit_positions != sorted(strict_post_commit_positions)
    ):
        raise AssertionError(
            "strict post-commit metadata replay must use one sequential reader and warning classification"
        )
    if strict_post_commit.count("openReplayFile(") != 1:
        raise AssertionError(
            "strict post-commit metadata replay must open its spool exactly once"
        )
    legacy_delete = function_text(
        texts,
        "src/util/PdfDirectoryDeleteScan.cpp",
        "Status deleteLegacyDirectoryNoThrow",
    )
    legacy_needles = [
        "discoverLegacyTreeWithRetry(rootPath)",
        "if (discovery.status != Status::Complete)",
        "return abortIncompleteLegacyDiscovery(discovery)",
        "if (discovery.foundPdf)",
        "commitLegacyDirectoryDelete(rootPath, callbacks,",
    ]
    legacy_positions = [legacy_delete.find(needle) for needle in legacy_needles]
    if any(position < 0 for position in legacy_positions) or legacy_positions != sorted(
        legacy_positions
    ):
        raise AssertionError(
            "legacy directory deletion must fail closed unless discovery completes"
        )
    for forbidden in [
        "cleanupSpoolFiles()",
        "preparePdfDelete",
        "kPathCapacity",
        "kEntryNameCapacity",
    ]:
        if forbidden in legacy_delete:
            raise AssertionError(
                f"legacy directory deletion must not enter PDF-only work: {forbidden}"
            )

    classification = function_text(
        texts,
        "src/util/PdfDirectoryDeleteScan.cpp",
        "LegacyTreeDiscovery discoverLegacyTreeOnce",
    )
    for needle in [
        "nextDirectoryEntry(directory, entry)",
        "LegacyEntryName& name = workspace.name",
        "if (!isDirectory &&",
        "endsWith(nameView, PdfDelete::kTombstoneSuffix)",
        "isJournaledPdfPath(nameView)",
    ]:
        if needle not in classification:
            raise AssertionError(
                f"legacy/PDF routing classification is missing: {needle}"
            )
    pdf_classification = classification[
        classification.find("if (!isDirectory &&") :
    ]
    pdf_classification_needles = [
        "discovery.foundPdf = true",
        "closeLegacyDiscovery(",
    ]
    pdf_classification_positions = [
        pdf_classification.find(needle) for needle in pdf_classification_needles
    ]
    if any(position < 0 for position in pdf_classification_positions) or (
        pdf_classification_positions != sorted(pdf_classification_positions)
    ):
        raise AssertionError(
            "positive PDF classification must survive a later close failure"
        )
    for forbidden in ["cleanupSpoolFiles()", "kPathCapacity"]:
        if forbidden in classification:
            raise AssertionError(
                f"read-only directory classification entered PDF mutation work: {forbidden}"
            )
    for forbidden in [
        "std::vector<std::string> pendingDirectories",
        "std::string name;",
        "buildLegacyChildPath",
    ]:
        if forbidden in classification:
            raise AssertionError(
                f"legacy discovery added avoidable STL traversal/name heap: {forbidden}"
            )
    metadata_retention_needles = [
        "isLegacyMetadataPath(nameView)",
        "closeLegacyDiscovery(",
        "createSpool(rootPath, metadataSpoolPath)",
        "appendSpoolRecord(",
        "SpoolRecordKind::LegacyMetadata",
        "truncateLegacyPath(path, parentLength)",
    ]
    metadata_retention = classification[
        classification.find(
            "if (metadataSpoolPath != nullptr && isLegacyMetadataPath(nameView))"
        ) :
    ]
    metadata_retention_positions = [
        metadata_retention.find(needle) for needle in metadata_retention_needles
    ]
    if any(position < 0 for position in metadata_retention_positions) or (
        metadata_retention_positions != sorted(metadata_retention_positions)
    ):
        raise AssertionError(
            "legacy discovery must retain every metadata path in the typed scratch spool"
        )

    name_reader = function_text(
        texts,
        "src/util/PdfDirectoryDeleteScan.cpp",
        "Status readEntryNameNoThrow",
    )
    name_needles = [
        "entry.getName(name.local, sizeof(name.local))",
        "length < sizeof(name.local) - 1U",
        "name.dynamicCapacity == 0U",
        "if (!name.dynamic || name.dynamicCapacity < capacity)",
        "makeUniqueNoThrow<char[]>(capacity)",
        "name.dynamicCapacity = capacity",
        "entry.getName(name.dynamic.get(), name.dynamicCapacity)",
        "length < capacity - 1U",
    ]
    name_positions = [name_reader.find(needle) for needle in name_needles]
    if any(position < 0 for position in name_positions) or name_positions != sorted(
        name_positions
    ):
        raise AssertionError(
            "legacy name reads must treat capacity-minus-one as truncation and grow fallibly"
        )

    retry = function_text(
        texts,
        "src/util/PdfDirectoryDeleteScan.cpp",
        "LegacyTreeDiscovery discoverLegacyTreeWithRetry",
    )
    retry_needles = [
        "makeUniqueNoThrow<LegacyDiscoveryWorkspace>()",
        "if (!workspace)",
        "LEGACY_SPOOL_FIRST_PATH",
        "isRetryableLegacyDiscoveryFailure(discovery.status)",
        "sealLegacyMetadataSpool(discovery)",
        "LEGACY_SPOOL_RETRY_PATH",
        "retry.foundPdf = retry.foundPdf || discovery.foundPdf",
        "sealLegacyMetadataSpool(retry)",
        "discovery.metadataSpool.recordCount >",
        "retry.metadataSpool = discovery.metadataSpool",
        "discovery = retry",
    ]
    retry_positions = [retry.find(needle) for needle in retry_needles]
    if any(position < 0 for position in retry_positions) or retry_positions != sorted(
        retry_positions
    ):
        raise AssertionError("legacy discovery must retry one transient traversal failure")
    require_function_order(
        texts,
        "src/util/PdfDirectoryDeleteScan.cpp",
        "LegacyTreeDiscovery discoverLegacyTreeWithRetry",
        "if (discovery.foundPdf)",
        "discovery.hasMetadataSpool = false",
        "if (!cleanupLegacySpoolFiles())",
        "discovery.status = Status::SpoolCleanupFailure",
    )

    incomplete_abort = function_text(
        texts,
        "src/util/PdfDirectoryDeleteScan.cpp",
        "Status abortIncompleteLegacyDiscovery",
    )
    incomplete_abort_fragments = [
        "const Status discoveryStatus = discovery.status;",
        "if (discoveryStatus == Status::SpoolCleanupFailure) return discoveryStatus;",
        "if (!cleanupLegacySpoolFiles()) return Status::SpoolCleanupFailure;",
    ]
    incomplete_abort_matches = [
        contract_pattern(fragment).search(incomplete_abort)
        for fragment in incomplete_abort_fragments
    ]
    discovery_status_returns = list(
        contract_pattern("return discoveryStatus;").finditer(incomplete_abort)
    )
    incomplete_abort_positions = [
        match.start() if match is not None else -1
        for match in incomplete_abort_matches
    ]
    if len(discovery_status_returns) != 2:
        incomplete_abort_positions.append(-1)
    else:
        incomplete_abort_positions.append(discovery_status_returns[-1].start())
    if any(position < 0 for position in incomplete_abort_positions) or (
        incomplete_abort_positions != sorted(incomplete_abort_positions)
    ):
        raise AssertionError(
            "incomplete discovery must clean scratch state and preserve its failure status"
        )
    for forbidden in ["commitLegacyDirectoryDelete(", "Storage.removeDir("]:
        if forbidden in incomplete_abort:
            raise AssertionError(
                f"incomplete discovery must not mutate the directory: {forbidden}"
            )

    legacy_commit = function_text(
        texts,
        "src/util/PdfDirectoryDeleteScan.cpp",
        "Status commitLegacyDirectoryDelete",
    )
    commit_needles = [
        "makeUniqueNoThrow<char[]>(",
        "validateSpool(",
        "Storage.removeDir(rootPath.c_str())",
        "HalFile replayFile;",
        "openReplayFile(",
        "bool metadataCleanupComplete = true;",
        "readOpenReplayRecord(",
        "kind != SpoolRecordKind::LegacyMetadata",
        "if (!callbacks.clearLegacyMetadata(callbacks.context,",
        "metadataCleanupComplete = false;",
        "replayFile.close()",
        "replayStatus = Status::CloseFailure",
        "replayStatus = Status::MetadataCleanupFailure",
    ]
    commit_positions = [legacy_commit.find(needle) for needle in commit_needles]
    if any(position < 0 for position in commit_positions) or commit_positions != sorted(
        commit_positions
    ):
        raise AssertionError("legacy commit must remove the tree before clearing metadata")
    require(
        {"legacy_commit": legacy_commit},
        "legacy_commit",
        "Status::CommittedWithCleanupWarning",
    )
    require_absent(
        {"legacy_commit": legacy_commit}, "legacy_commit", "readReplayRecord("
    )
    if legacy_commit.count("openReplayFile(") != 1:
        raise AssertionError(
            "legacy post-commit metadata replay must open its spool exactly once"
        )

    routed_delete = function_text(
        texts,
        "src/util/PdfDirectoryDeleteScan.cpp",
        "Status deleteDirectoryNoThrow",
    )
    routed_needles = [
        "simulatorHiddenTombstoneStatus(rootPath)",
        "discoverLegacyTreeWithRetry(rootPath)",
        "if (discovery.status != Status::Complete)",
        "return abortIncompleteLegacyDiscovery(discovery)",
        "if (!discovery.foundPdf)",
        "return commitLegacyDirectoryDelete(rootPath, callbacks,",
        "callbacks.preparePdfDelete(callbacks.context)",
        "return deletePdfDirectoryNoThrow(rootPath, callbacks)",
    ]
    routed_positions = [routed_delete.find(needle) for needle in routed_needles]
    if any(position < 0 for position in routed_positions) or routed_positions != sorted(
        routed_positions
    ):
        raise AssertionError(
            "directory routing must discover once before choosing legacy or recovered PDF deletion"
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
        "status = createSpool(rootPath,",
        "status = appendSpoolRecord(",
        "SpoolRecordKind::Pdf",
    ]
    require_order_ignoring_whitespace(
        pdf_branch,
        "PDF record must enter the typed spool with no directory reader open",
        *discovery_needles,
    )
    for needle in [
        "isLegacyMetadataPath(name)",
        "SpoolRecordKind::LegacyMetadata",
    ]:
        if needle not in discovery:
            raise AssertionError(
                "strict discovery must retain legacy metadata in the typed spool"
            )
    require_function_order(
        texts,
        "src/util/PdfDirectoryDeleteScan.cpp",
        "Status validateSpool",
        "file.fileSize64()",
        "SPOOL_HEADER_MAGIC",
        "SPOOL_FOOTER_MAGIC",
        "maxPathBytes == spool.maxPathBytes",
        "actualRecordsBytes",
        "isRootBoundRecordPath(rootPath, path, kind)",
        "actualRecordsCrc == expectedRecordsCrc",
        "file.position() == footerOffset",
    )
    replay = function_text(
        texts,
        "src/util/PdfDirectoryDeleteScan.cpp",
        "Status readOpenReplayRecord",
    )
    require({"replay": replay}, "replay", "readExact(file")
    replay_needles = [
        "isRootBoundRecordPath(rootPath, path, kind)",
        "spool.replayOffset +=",
        "*kindOut = kind",
        "return Status::Complete",
    ]
    replay_positions = [
        replay.find(needle) for needle in replay_needles
    ]
    if any(position < 0 for position in replay_positions) or replay_positions != sorted(
        replay_positions
    ):
        raise AssertionError("open replay must validate each path before advancing")
    require_function_order(
        texts,
        "src/util/PdfDirectoryDeleteScan.cpp",
        "Status readReplayRecord",
        "openReplayFile(",
        "readOpenReplayRecord(",
        "file.close()",
    )
    require(
        texts,
        "src/util/PdfDirectoryDeleteScan.h",
        "Status deleteDirectoryNoThrow(const std::string& rootPath,",
    )
    require(
        texts,
        "src/util/PdfDirectoryDeleteScan.h",
        "Status deleteLegacyDirectoryNoThrow(const std::string& rootPath,",
    )
    require(
        texts,
        "src/util/PdfDirectoryDeleteScan.h",
        "Status deletePdfDirectoryNoThrow(const std::string& rootPath,",
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
        "static_assert(sizeof(LegacyDiscoveryWorkspace) <= 1024U);",
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
        "bool RecentBooksStore::removeByPathDurablyNoPathAlloc",
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
        "bool RecentBooksStore::removeByPathDurably(const std::string& path)",
        "return removeByPathDurablyNoPathAlloc(path);",
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
        """bool deletePdfBook(const std::string& fullPath) {
  if (!FsHelpers::hasPdfExtension(fullPath)) return false;
  return PdfDeleteUtils::deletePdfBook(fullPath) == PdfDeleteUtils::Result::Complete;
}""",
    )


def main() -> None:
    validate(sources())
    print("PDF deletion source contract passed")


if __name__ == "__main__":
    main()
