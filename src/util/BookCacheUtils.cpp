#include "BookCacheUtils.h"
#include "BookMoveUtils.h"

#include <Epub.h>
#include <FsHelpers.h>
#include <Logging.h>
#include <Memory.h>
#include <PdfSourceIdentity.h>
#include <Txt.h>
#include <Xtc.h>

#include <algorithm>
#include <cctype>
#if defined(SIMULATOR)
#include <cerrno>
#endif
#include <cstdio>
#include <cstring>
#include <iterator>
#include <limits>
#include <vector>

namespace {

struct PreservedCacheFile {
  const char* name;
  const char* tmpName;
};

constexpr PreservedCacheFile EPUB_USER_STATE_FILES[] = {
    {"progress.bin", "upload_preserve_progress.bin"},
    {"progress.bin.bak", "upload_preserve_progress.bin.bak"},
    {"reader_settings.bin", "upload_preserve_reader_settings.bin"},
    {"dictionary_history.txt", "upload_preserve_dictionary_history.txt"},
};

constexpr PreservedCacheFile PAGE_PROGRESS_FILES[] = {
    {"progress.bin", "upload_preserve_progress.bin"},
};

constexpr PreservedCacheFile CACHE_CLEAR_USER_STATE_FILES[] = {
    {"dictionary_history.txt", "clear_preserve_dictionary_history.txt"},
};

struct ResolvedPreservedCacheFile {
  std::string name;
  std::string tmpName;
};

struct StatsFileCandidate {
  std::string name;
  int version = -1;
};

constexpr size_t MAX_PRESERVED_CACHE_FILES = std::size(EPUB_USER_STATE_FILES) > std::size(PAGE_PROGRESS_FILES)
                                                 ? std::size(EPUB_USER_STATE_FILES)
                                                 : std::size(PAGE_PROGRESS_FILES);
constexpr size_t MAX_STATS_FILES_TO_PRESERVE = 8;
constexpr char STATS_PREFIX[] = "stats";
constexpr char STATS_SUFFIX[] = ".bin";
constexpr size_t MAX_PDF_GENERATIONS_TO_CLEAR = 32;

constexpr const char* PDF_DERIVED_ROOT_FILES[] = {
    "manifest.a", "manifest.b", "build.a", "build.b", "manifest.a.tmp", "manifest.b.tmp", "build.a.tmp", "build.b.tmp",
};
static_assert(std::size(PDF_DERIVED_ROOT_FILES) <= 8);
constexpr const char* PDF_DERIVED_ROOT_DIRECTORIES[] = {"sections"};
static_assert(std::size(PDF_DERIVED_ROOT_DIRECTORIES) <= 8);

struct PdfCacheClearWorkspace {
  char path[PDF_CACHE_PATH_CAPACITY]{};
  char entryName[PDF_CACHE_ENTRY_NAME_CAPACITY]{};
  uint32_t generations[MAX_PDF_GENERATIONS_TO_CLEAR]{};
  uint8_t generationCount = 0;
  uint8_t derivedRootMask = 0;
  uint8_t derivedDirectoryMask = 0;
};
static_assert(sizeof(PdfCacheClearWorkspace) <= 356);

enum class PdfCacheDirectoryNextResult : uint8_t {
  Entry,
  End,
  Error,
};

PdfCacheDirectoryNextResult nextPdfCacheEntry(FsFile& directory, FsFile& entry) {
#if defined(SIMULATOR)
  // The external native simulator HAL predates explicit directory status and
  // constructs HalFile with throwing host `new`. Keep that host-only behavior
  // isolated here. ESP32-C3 and QEMU compile the fallible, reusable-wrapper
  // branch below and never silently collapse allocation or I/O errors into EOF.
  if (!entry.close()) {
    return PdfCacheDirectoryNextResult::Error;
  }
  errno = 0;
  entry = directory.openNextFile();
  if (entry) {
    return PdfCacheDirectoryNextResult::Entry;
  }
  // POSIX readdir leaves errno at zero for clean EOF. A non-zero value is
  // conservatively treated as an error, so derived state is not deleted.
  return errno == 0 ? PdfCacheDirectoryNextResult::End : PdfCacheDirectoryNextResult::Error;
#else
  const HalDirectoryNextStatus status = directory.openNextFile(entry);
  if (status == HalDirectoryNextStatus::Entry) {
    return PdfCacheDirectoryNextResult::Entry;
  }
  if (status == HalDirectoryNextStatus::End) {
    return PdfCacheDirectoryNextResult::End;
  }
  return PdfCacheDirectoryNextResult::Error;
#endif
}

std::string getBookCachePath(const std::string& path) {
  if (FsHelpers::hasEpubExtension(path)) {
    return Epub(path, "/.crosspoint").getCachePath();
  }
  if (FsHelpers::hasXtcExtension(path)) {
    return Xtc(path, "/.crosspoint").getCachePath();
  }
  if (FsHelpers::hasTxtExtension(path)) {
    return Txt(path, "/.crosspoint").getCachePath();
  }
  return "";
}

bool parsePdfGenerationName(const char* const name, uint32_t& generation) {
  constexpr char PREFIX[] = "gen_";
  constexpr size_t PREFIX_LENGTH = std::size(PREFIX) - 1;
  if (!name || strncmp(name, PREFIX, PREFIX_LENGTH) != 0 || name[PREFIX_LENGTH] == '\0' || name[PREFIX_LENGTH] == '0') {
    return false;
  }

  uint32_t value = 0;
  for (size_t index = PREFIX_LENGTH; name[index] != '\0'; ++index) {
    const unsigned char character = static_cast<unsigned char>(name[index]);
    if (!std::isdigit(character)) {
      return false;
    }
    const uint32_t digit = character - static_cast<unsigned char>('0');
    if (value > (std::numeric_limits<uint32_t>::max() - digit) / 10U) {
      return false;
    }
    value = value * 10U + digit;
  }
  generation = value;
  return true;
}

bool appendPdfCacheLeaf(PdfCacheClearWorkspace& workspace, const size_t rootLength, const char* const leaf) {
  if (!leaf) {
    return false;
  }
  const size_t leafLength = strlen(leaf);
  if (rootLength + 1 + leafLength + 1 > sizeof(workspace.path)) {
    return false;
  }
  workspace.path[rootLength] = '/';
  memcpy(workspace.path + rootLength + 1, leaf, leafLength + 1);
  return true;
}

bool removePdfDerivedRootFiles(PdfCacheClearWorkspace& workspace, const size_t rootLength) {
  for (size_t index = 0; index < std::size(PDF_DERIVED_ROOT_FILES); ++index) {
    if ((workspace.derivedRootMask & (1U << index)) == 0) {
      continue;
    }
    const char* const name = PDF_DERIVED_ROOT_FILES[index];
    if (!appendPdfCacheLeaf(workspace, rootLength, name)) {
      LOG_ERR("BookCache", "PDF cache derived path is too long: %s", workspace.path);
      return false;
    }
    if (!Storage.remove(workspace.path)) {
      LOG_ERR("BookCache", "Failed to remove PDF derived cache file: %s", workspace.path);
      return false;
    }
    workspace.path[rootLength] = '\0';
  }
  return true;
}

bool removePdfDerivedRootDirectories(PdfCacheClearWorkspace& workspace, const size_t rootLength) {
  for (size_t index = 0; index < std::size(PDF_DERIVED_ROOT_DIRECTORIES); ++index) {
    if ((workspace.derivedDirectoryMask & (1U << index)) == 0) {
      continue;
    }
    const char* const name = PDF_DERIVED_ROOT_DIRECTORIES[index];
    if (!appendPdfCacheLeaf(workspace, rootLength, name)) {
      LOG_ERR("BookCache", "PDF cache derived directory path is too long: %s", workspace.path);
      return false;
    }
    if (!Storage.removeDir(workspace.path)) {
      LOG_ERR("BookCache", "Failed to remove PDF derived cache directory: %s", workspace.path);
      return false;
    }
    workspace.path[rootLength] = '\0';
  }
  return true;
}

[[gnu::noinline]] bool collectPdfGenerations(PdfCacheClearWorkspace& workspace) {
  FsFile directory = Storage.open(workspace.path);
  if (!directory) {
    LOG_ERR("BookCache", "Failed to open PDF cache directory: %s", workspace.path);
    return false;
  }
  if (!directory.isDirectory()) {
    directory.close();
    LOG_ERR("BookCache", "PDF cache path is not a directory: %s", workspace.path);
    return false;
  }

  bool ok = true;
  FsFile entry;
  while (ok) {
    const PdfCacheDirectoryNextResult result = nextPdfCacheEntry(directory, entry);
    if (result == PdfCacheDirectoryNextResult::End) {
      break;
    }
    if (result == PdfCacheDirectoryNextResult::Error) {
      LOG_ERR("BookCache", "Failed to enumerate PDF cache directory: %s", workspace.path);
      ok = false;
      break;
    }

    const bool isDirectory = entry.isDirectory();
    const size_t nameLength = entry.getName(workspace.entryName, sizeof(workspace.entryName));
    if (nameLength == 0 || nameLength >= sizeof(workspace.entryName)) {
      ok = false;
      break;
    }

    if (!isDirectory) {
      for (size_t index = 0; index < std::size(PDF_DERIVED_ROOT_FILES); ++index) {
        if (strcmp(workspace.entryName, PDF_DERIVED_ROOT_FILES[index]) == 0) {
          workspace.derivedRootMask |= static_cast<uint8_t>(1U << index);
          break;
        }
      }
      continue;
    }

    bool knownDerivedDirectory = false;
    for (size_t index = 0; index < std::size(PDF_DERIVED_ROOT_DIRECTORIES); ++index) {
      if (strcmp(workspace.entryName, PDF_DERIVED_ROOT_DIRECTORIES[index]) == 0) {
        workspace.derivedDirectoryMask |= static_cast<uint8_t>(1U << index);
        knownDerivedDirectory = true;
        break;
      }
    }
    if (knownDerivedDirectory) {
      continue;
    }

    uint32_t generation = 0;
    if (!parsePdfGenerationName(workspace.entryName, generation)) {
      continue;
    }
    if (workspace.generationCount >= MAX_PDF_GENERATIONS_TO_CLEAR) {
      LOG_ERR("BookCache", "Too many PDF cache generations: %s", workspace.path);
      ok = false;
      break;
    }
    workspace.generations[workspace.generationCount++] = generation;
  }
  const bool entryCloseOk = entry.close();
  const bool closeOk = directory.close();
  if (!entryCloseOk || !closeOk) {
    ok = false;
  }
  return ok;
}

bool removePdfGenerations(PdfCacheClearWorkspace& workspace, const size_t rootLength) {
  for (size_t index = 0; index < workspace.generationCount; ++index) {
    const int written = snprintf(workspace.path + rootLength, sizeof(workspace.path) - rootLength, "/gen_%lu",
                                 static_cast<unsigned long>(workspace.generations[index]));
    if (written <= 0 || static_cast<size_t>(written) >= sizeof(workspace.path) - rootLength) {
      LOG_ERR("BookCache", "PDF generation cache path is too long: %s", workspace.path);
      return false;
    }
    if (!Storage.removeDir(workspace.path)) {
      LOG_ERR("BookCache", "Failed to remove PDF cache generation: %s", workspace.path);
      return false;
    }
    workspace.path[rootLength] = '\0';
  }
  return true;
}

bool clearPdfDerivedCacheWorkspace(PdfCacheClearWorkspace& workspace) {
  const size_t rootLength = strlen(workspace.path);
  if (!collectPdfGenerations(workspace)) {
    return false;
  }
  return removePdfDerivedRootFiles(workspace, rootLength) && removePdfDerivedRootDirectories(workspace, rootLength) &&
         removePdfGenerations(workspace, rootLength);
}

std::unique_ptr<PdfCacheClearWorkspace> allocatePdfCacheClearWorkspace() {
  // This cold upload path needs 356 bytes of mutable path/list storage. One
  // fallible allocation keeps it off the small task stack, and the fixed arrays
  // avoid container growth while scanning and deleting cache entries.
  auto workspace = makeUniqueNoThrow<PdfCacheClearWorkspace>();
  if (!workspace) {
    LOG_ERR("BookCache", "Failed to allocate PDF cache-clear workspace (%u bytes)",
            static_cast<unsigned>(sizeof(PdfCacheClearWorkspace)));
  }
  return workspace;
}

[[gnu::noinline]] bool clearPdfDerivedCache(const std::string& path) {
  const uint64_t normalCacheHash = pdfPathHash64(path.c_str(), path.size());
  uint64_t resolvedCacheHash = normalCacheHash;
  bool readOnlyFallback = true;
  if (!BookMoveUtils::migrationCacheHash(path, normalCacheHash, &resolvedCacheHash, &readOnlyFallback) ||
      readOnlyFallback || resolvedCacheHash != normalCacheHash) {
    LOG_ERR("BookCache", "Refusing PDF cache deletion while migration state is unresolved");
    return false;
  }

  char cachePath[PDF_CACHE_PATH_CAPACITY]{};
  const PdfStatus status =
      pdfFormatCacheRootForHash("/.crosspoint", normalCacheHash, cachePath, sizeof(cachePath));
  if (!status) {
    LOG_ERR("BookCache", "Failed to resolve PDF cache path: %s", path.c_str());
    return false;
  }
  if (!Storage.exists(cachePath)) {
    return true;
  }

  auto workspace = allocatePdfCacheClearWorkspace();
  if (!workspace) {
    return false;
  }
  memcpy(workspace->path, cachePath, sizeof(cachePath));
  return clearPdfDerivedCacheWorkspace(*workspace);
}

size_t trimmedCacheDirectoryPathLength(const std::string& cachePath) {
  size_t length = cachePath.size();
  while (length > 1 && cachePath[length - 1] == '/') {
    --length;
  }
  return length;
}

bool isPdfCacheDirectoryPath(const std::string& cachePath) {
  const size_t length = trimmedCacheDirectoryPathLength(cachePath);
  size_t nameOffset = length;
  while (nameOffset != 0 && cachePath[nameOffset - 1] != '/') {
    --nameOffset;
  }
  constexpr char PDF_PREFIX[] = "pdf_";
  constexpr size_t PDF_PREFIX_LENGTH = std::size(PDF_PREFIX) - 1;
  return length - nameOffset >= PDF_PREFIX_LENGTH &&
         memcmp(cachePath.data() + nameOffset, PDF_PREFIX, PDF_PREFIX_LENGTH) == 0;
}

[[gnu::noinline]] bool clearPdfDerivedCacheDirectory(const std::string& cachePath) {
  const size_t pathLength = trimmedCacheDirectoryPathLength(cachePath);
  char normalizedPath[PDF_CACHE_PATH_CAPACITY]{};
  if (pathLength == 0 || pathLength >= sizeof(normalizedPath)) {
    LOG_ERR("BookCache", "PDF cache directory path is invalid");
    return false;
  }
  memcpy(normalizedPath, cachePath.c_str(), pathLength);
  if (!Storage.exists(normalizedPath)) {
    return true;
  }

  auto workspace = allocatePdfCacheClearWorkspace();
  if (!workspace) {
    return false;
  }
  memcpy(workspace->path, normalizedPath, sizeof(normalizedPath));
  return clearPdfDerivedCacheWorkspace(*workspace);
}

const PreservedCacheFile* preservedFilesForPath(const std::string& path, size_t& count) {
  if (FsHelpers::hasEpubExtension(path)) {
    count = std::size(EPUB_USER_STATE_FILES);
    return EPUB_USER_STATE_FILES;
  }
  if (FsHelpers::hasXtcExtension(path) || FsHelpers::hasTxtExtension(path)) {
    count = std::size(PAGE_PROGRESS_FILES);
    return PAGE_PROGRESS_FILES;
  }
  count = 0;
  return nullptr;
}

bool isStatsFileName(const char* name) {
  if (!name) {
    return false;
  }
  const size_t nameLen = strlen(name);
  constexpr size_t prefixLen = std::size(STATS_PREFIX) - 1;
  constexpr size_t suffixLen = std::size(STATS_SUFFIX) - 1;
  return nameLen >= prefixLen + suffixLen && strncmp(name, STATS_PREFIX, prefixLen) == 0 &&
         strcmp(name + nameLen - suffixLen, STATS_SUFFIX) == 0;
}

int statsFileVersion(const char* name) {
  if (!name || strcmp(name, "stats.bin") == 0) {
    return 0;
  }

  constexpr char VERSION_PREFIX[] = "stats_v";
  constexpr size_t versionPrefixLen = std::size(VERSION_PREFIX) - 1;
  constexpr size_t suffixLen = std::size(STATS_SUFFIX) - 1;
  const size_t nameLen = strlen(name);
  if (nameLen <= versionPrefixLen + suffixLen || strncmp(name, VERSION_PREFIX, versionPrefixLen) != 0 ||
      strcmp(name + nameLen - suffixLen, STATS_SUFFIX) != 0) {
    return -1;
  }

  int version = 0;
  for (size_t i = versionPrefixLen; i < nameLen - suffixLen; ++i) {
    if (name[i] < '0' || name[i] > '9') {
      return -1;
    }
    version = version * 10 + (name[i] - '0');
  }
  return version;
}

void appendFixedPreservedFiles(std::vector<ResolvedPreservedCacheFile>& files, const PreservedCacheFile* fixedFiles,
                               const size_t fixedCount) {
  for (size_t i = 0; i < fixedCount; ++i) {
    files.push_back({fixedFiles[i].name, fixedFiles[i].tmpName});
  }
}

bool appendStatsPreservedFiles(const std::string& cachePath, std::vector<ResolvedPreservedCacheFile>& files,
                               const char* tmpPrefix) {
  if (!tmpPrefix) {
    LOG_ERR("BookCache", "Missing stats preservation temp prefix: %s", cachePath.c_str());
    return false;
  }

  FsFile dir = Storage.open(cachePath.c_str());
  if (!dir) {
    if (Storage.exists(cachePath.c_str())) {
      LOG_ERR("BookCache", "Failed to open cache directory for stats preservation: %s", cachePath.c_str());
      return false;
    }
    return true;
  }
  if (!dir.isDirectory()) {
    dir.close();
    LOG_ERR("BookCache", "Cache path is not a directory during stats preservation: %s", cachePath.c_str());
    return false;
  }

  std::vector<StatsFileCandidate> candidates;
  candidates.reserve(MAX_STATS_FILES_TO_PRESERVE + 1);
  char name[96];
  for (FsFile file = dir.openNextFile(); file; file = dir.openNextFile()) {
    const bool isDirectory = file.isDirectory();
    const size_t nameLen = file.getName(name, sizeof(name));
    file.close();
    if (isDirectory || nameLen == 0 || !isStatsFileName(name)) {
      continue;
    }
    candidates.push_back({name, statsFileVersion(name)});
  }
  dir.close();

  std::sort(candidates.begin(), candidates.end(), [](const StatsFileCandidate& lhs, const StatsFileCandidate& rhs) {
    if (lhs.version != rhs.version) {
      return lhs.version > rhs.version;
    }
    return lhs.name > rhs.name;
  });

  for (size_t i = 0; i < candidates.size(); ++i) {
    if (i >= MAX_STATS_FILES_TO_PRESERVE) {
      LOG_DBG("BookCache", "Dropping older stats file during cache preservation: %s", candidates[i].name.c_str());
      continue;
    }
    files.push_back({candidates[i].name, std::string(tmpPrefix) + candidates[i].name});
  }
  return true;
}

bool resolvePreservedFiles(const std::string& cachePath, const PreservedCacheFile* fixedFiles, const size_t fixedCount,
                           const bool includeStatsFiles, const char* statsTmpPrefix,
                           std::vector<ResolvedPreservedCacheFile>& files) {
  files.clear();
  files.reserve(fixedCount + (includeStatsFiles ? MAX_STATS_FILES_TO_PRESERVE : 0));
  appendFixedPreservedFiles(files, fixedFiles, fixedCount);
  if (includeStatsFiles && !appendStatsPreservedFiles(cachePath, files, statsTmpPrefix)) {
    return false;
  }
  return true;
}

bool restorePreservedFiles(const std::string& cachePath, const std::vector<ResolvedPreservedCacheFile>& files,
                           const bool* movedFiles = nullptr) {
  if (files.empty()) {
    return true;
  }

  bool restoredAny = false;
  bool ok = true;
  for (size_t i = 0; i < files.size(); i++) {
    if (movedFiles && !movedFiles[i]) {
      continue;
    }
    const std::string tmpPath = cachePath + "." + files[i].tmpName;
    if (!Storage.exists(tmpPath.c_str())) {
      continue;
    }

    Storage.mkdir(cachePath.c_str());
    const std::string finalPath = cachePath + "/" + files[i].name;
    if (Storage.exists(finalPath.c_str())) {
      Storage.remove(finalPath.c_str());
    }
    if (!Storage.rename(tmpPath.c_str(), finalPath.c_str())) {
      LOG_ERR("BookCache", "Failed to restore preserved cache state: %s", finalPath.c_str());
      ok = false;
    } else {
      restoredAny = true;
    }
  }

  if (restoredAny) {
    LOG_DBG("BookCache", "Restored preserved user cache state: %s", cachePath.c_str());
  }
  return ok;
}

bool preserveUserStateFiles(const std::string& cachePath, const std::vector<ResolvedPreservedCacheFile>& files,
                            bool* movedFiles) {
  bool ok = true;
  for (size_t i = 0; i < files.size(); i++) {
    if (movedFiles) {
      movedFiles[i] = false;
    }
    const std::string sourcePath = cachePath + "/" + files[i].name;
    const std::string tmpPath = cachePath + "." + files[i].tmpName;

    if (Storage.exists(tmpPath.c_str()) && !Storage.remove(tmpPath.c_str())) {
      LOG_ERR("BookCache", "Failed to remove stale preserved state temp: %s", tmpPath.c_str());
      ok = false;
      continue;
    }
    if (!Storage.exists(sourcePath.c_str())) {
      continue;
    }
    if (!Storage.rename(sourcePath.c_str(), tmpPath.c_str())) {
      LOG_ERR("BookCache", "Failed to preserve cache state: %s", sourcePath.c_str());
      ok = false;
    } else if (movedFiles) {
      movedFiles[i] = true;
    }
  }
  return ok;
}

bool clearBookCacheForPath(const std::string& path) {
  if (FsHelpers::hasEpubExtension(path)) {
    return Epub(path, "/.crosspoint").clearCache();
  }
  if (FsHelpers::hasXtcExtension(path)) {
    return Xtc(path, "/.crosspoint").clearCache();
  }
  if (FsHelpers::hasTxtExtension(path)) {
    return Txt(path, "/.crosspoint").clearCache();
  }
  return false;
}

bool clearCacheDirectoryPreservingFiles(const std::string& cachePath, const PreservedCacheFile* fixedPreservedFiles,
                                        const size_t fixedPreservedCount, const bool includeStatsFiles,
                                        const char* statsTmpPrefix) {
  if (cachePath.empty()) {
    return false;
  }

  if (!Storage.exists(cachePath.c_str())) {
    return true;
  }

  std::vector<ResolvedPreservedCacheFile> preservedFiles;
  if (!resolvePreservedFiles(cachePath, fixedPreservedFiles, fixedPreservedCount, includeStatsFiles, statsTmpPrefix,
                             preservedFiles)) {
    return false;
  }

  bool movedFiles[MAX_PRESERVED_CACHE_FILES + MAX_STATS_FILES_TO_PRESERVE] = {};
  const bool preserveOk = preserveUserStateFiles(cachePath, preservedFiles, movedFiles);
  if (!preserveOk) {
    if (!restorePreservedFiles(cachePath, preservedFiles, movedFiles)) {
      LOG_ERR("BookCache", "Failed to roll back preserved state after aborting cache clear: %s", cachePath.c_str());
    }
    LOG_ERR("BookCache", "Aborted cache clear because preserved state could not be moved: %s", cachePath.c_str());
    return false;
  }

  const bool clearOk = Storage.removeDir(cachePath.c_str());
  const bool restoreOk = restorePreservedFiles(cachePath, preservedFiles, movedFiles);
  if (!clearOk) {
    LOG_ERR("BookCache", "Failed to clear cache directory: %s", cachePath.c_str());
  }
  return clearOk && restoreOk;
}

}  // namespace

bool isBookCacheDirectoryName(const char* name) {
  if (!name) {
    return false;
  }

  constexpr char EPUB_PREFIX[] = "epub_";
  constexpr char PDF_PREFIX[] = "pdf_";
  constexpr char TXT_PREFIX[] = "txt_";
  constexpr char XTC_PREFIX[] = "xtc_";

  return strncmp(name, EPUB_PREFIX, std::size(EPUB_PREFIX) - 1) == 0 ||
         strncmp(name, PDF_PREFIX, std::size(PDF_PREFIX) - 1) == 0 ||
         strncmp(name, TXT_PREFIX, std::size(TXT_PREFIX) - 1) == 0 ||
         strncmp(name, XTC_PREFIX, std::size(XTC_PREFIX) - 1) == 0;
}

void clearBookCache(const std::string& path) { clearBookCachePreservingUserState(path); }

// Keep the legacy preservation frame out of the PDF dispatcher. The legacy
// implementation predates PDF support and has a larger stats-sorting frame.
[[gnu::noinline]] static bool clearLegacyBookCachePreservingUserState(const std::string& path) {
  size_t preservedCount = 0;
  const PreservedCacheFile* preservedFiles = preservedFilesForPath(path, preservedCount);
  if (!preservedFiles || preservedCount == 0) {
    return clearBookCacheForPath(path);
  }

  const std::string cachePath = getBookCachePath(path);
  if (cachePath.empty()) {
    return false;
  }

  std::vector<ResolvedPreservedCacheFile> resolvedFiles;
  const bool includeStatsFiles = FsHelpers::hasEpubExtension(path) || FsHelpers::hasXtcExtension(path);
  if (!resolvePreservedFiles(cachePath, preservedFiles, preservedCount, includeStatsFiles, "upload_preserve_",
                             resolvedFiles)) {
    return false;
  }

  bool movedFiles[MAX_PRESERVED_CACHE_FILES + MAX_STATS_FILES_TO_PRESERVE] = {};
  const bool preserveOk = preserveUserStateFiles(cachePath, resolvedFiles, movedFiles);
  if (!preserveOk) {
    if (!restorePreservedFiles(cachePath, resolvedFiles, movedFiles)) {
      LOG_ERR("BookCache", "Failed to roll back preserved state after aborting cache clear: %s", cachePath.c_str());
    }
    LOG_ERR("BookCache", "Aborted cache clear because user state could not be preserved: %s", cachePath.c_str());
    return false;
  }
  const bool clearOk = clearBookCacheForPath(path);
  const bool restoreOk = restorePreservedFiles(cachePath, resolvedFiles, movedFiles);
  if (clearOk) {
  }
  return clearOk && restoreOk;
}

bool clearBookCachePreservingUserState(const std::string& path) {
  if (FsHelpers::hasPdfExtension(path)) {
    return clearPdfDerivedCache(path);
  }
  return clearLegacyBookCachePreservingUserState(path);
}

bool clearBookCacheDirectoryPreservingStats(const std::string& cachePath) {
  if (isPdfCacheDirectoryPath(cachePath)) {
    return clearPdfDerivedCacheDirectory(cachePath);
  }
  return clearCacheDirectoryPreservingFiles(cachePath, nullptr, 0, true, "clear_preserve_");
}
