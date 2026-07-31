#include "PdfDirectoryDeleteScan.h"

#include <FsHelpers.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>
#include <PdfDeleteJournal.h>

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <vector>
#if defined(SIMULATOR)
#include <filesystem>
#include <system_error>
#endif

namespace {

using PdfDirectoryDeleteScan::Status;

struct LegacyWalkWorkspace {
  HalFile directory;
  HalFile entry;
  char currentPath[PdfDirectoryDeleteScan::kPathCapacity]{};
  char entryName[PdfDirectoryDeleteScan::kEntryNameCapacity]{};
  char resumeName[PdfDirectoryDeleteScan::kEntryNameCapacity]{};
  uint16_t currentLength = 0;
  uint16_t rootLength = 0;
  bool resumeAfterEntry = false;
};

static_assert(sizeof(LegacyWalkWorkspace) <= 2U * 1024U);

struct SpoolWorkspace {
  uint32_t recordCount = 0;
  uint32_t recordsBytes = 0;
  uint32_t recordsCrc = 0;
  uint64_t replayOffset = 0;
};

static_assert(sizeof(SpoolWorkspace) <= 32U);

constexpr size_t SPOOL_HEADER_BYTES = 20U;
constexpr size_t SPOOL_RECORD_HEADER_BYTES = 8U;
constexpr size_t SPOOL_FOOTER_BYTES = 24U;
constexpr uint8_t SPOOL_VERSION = 1U;
constexpr uint8_t SPOOL_HEADER_MAGIC[8] = {'C', 'P', 'D', 'F',
                                           'D', 'S', 'H', '1'};
constexpr uint8_t SPOOL_FOOTER_MAGIC[8] = {'C', 'P', 'D', 'F',
                                           'D', 'S', 'F', '1'};

enum class DirectoryNextResult : uint8_t {
  Entry,
  End,
  Error,
};

DirectoryNextResult nextDirectoryEntry(HalFile& directory, HalFile& entry) {
#if defined(SIMULATOR)
  // The pinned native simulator HAL predates the reusable, result-bearing
  // directory API. This host-only seam still distinguishes POSIX EOF/error.
  if (!entry.close()) return DirectoryNextResult::Error;
  errno = 0;
  entry = directory.openNextFile();
  if (entry) return DirectoryNextResult::Entry;
  return errno == 0 ? DirectoryNextResult::End : DirectoryNextResult::Error;
#else
  const HalDirectoryNextStatus status = directory.openNextFile(entry);
  if (status == HalDirectoryNextStatus::Entry) return DirectoryNextResult::Entry;
  if (status == HalDirectoryNextStatus::End) return DirectoryNextResult::End;
  return DirectoryNextResult::Error;
#endif
}

bool endsWith(const std::string_view value, const std::string_view suffix) {
  return value.size() >= suffix.size() && value.substr(value.size() - suffix.size()) == suffix;
}

bool isJournaledPdfPath(std::string_view path);

bool isCanonicalAbsolutePath(const std::string_view path) {
  if (path.size() < 2U || path.front() != '/' || path.back() == '/' ||
      path.find('\\') != std::string_view::npos ||
      path.find('\0') != std::string_view::npos) {
    return false;
  }
  size_t segmentStart = 1U;
  while (segmentStart < path.size()) {
    const size_t segmentEnd = path.find('/', segmentStart);
    const size_t end =
        segmentEnd == std::string_view::npos ? path.size() : segmentEnd;
    const size_t length = end - segmentStart;
    if (length == 0U ||
        (length == 1U && path[segmentStart] == '.') ||
        (length == 2U && path[segmentStart] == '.' &&
         path[segmentStart + 1U] == '.')) {
      return false;
    }
    if (segmentEnd == std::string_view::npos) break;
    segmentStart = segmentEnd + 1U;
  }
  return true;
}

uint32_t crc32Update(uint32_t crc, const uint8_t* const bytes,
                     const size_t length) {
  if (bytes == nullptr && length != 0U) return 0U;
  crc = ~crc;
  for (size_t index = 0; index < length; ++index) {
    crc ^= bytes[index];
    for (uint8_t bit = 0; bit < 8U; ++bit) {
      crc = (crc >> 1U) ^ (0xEDB88320U & (0U - (crc & 1U)));
    }
  }
  return ~crc;
}

void writeLe16(uint8_t* const destination, const uint16_t value) {
  destination[0] = static_cast<uint8_t>(value);
  destination[1] = static_cast<uint8_t>(value >> 8U);
}

void writeLe32(uint8_t* const destination, const uint32_t value) {
  destination[0] = static_cast<uint8_t>(value);
  destination[1] = static_cast<uint8_t>(value >> 8U);
  destination[2] = static_cast<uint8_t>(value >> 16U);
  destination[3] = static_cast<uint8_t>(value >> 24U);
}

uint16_t readLe16(const uint8_t* const source) {
  return static_cast<uint16_t>(source[0]) |
         static_cast<uint16_t>(static_cast<uint16_t>(source[1]) << 8U);
}

uint32_t readLe32(const uint8_t* const source) {
  return static_cast<uint32_t>(source[0]) |
         (static_cast<uint32_t>(source[1]) << 8U) |
         (static_cast<uint32_t>(source[2]) << 16U) |
         (static_cast<uint32_t>(source[3]) << 24U);
}

bool writeExact(HalFile& file, const uint8_t* const bytes,
                const size_t length) {
  return bytes != nullptr && length != 0U &&
         file.write(bytes, length) == length;
}

bool readExact(HalFile& file, uint8_t* const bytes, const size_t length) {
  return bytes != nullptr && length != 0U &&
         file.read(bytes, length) == static_cast<int>(length);
}

bool cleanupSpoolFiles() {
  bool cleaned = true;
  if (Storage.exists(PdfDirectoryDeleteScan::kSpoolTempPath) &&
      !Storage.remove(PdfDirectoryDeleteScan::kSpoolTempPath)) {
    cleaned = false;
  }
  if (Storage.exists(PdfDirectoryDeleteScan::kSpoolSealedPath) &&
      !Storage.remove(PdfDirectoryDeleteScan::kSpoolSealedPath)) {
    cleaned = false;
  }
  return cleaned;
}

Status finishWithSpoolCleanup(const Status status) {
  return cleanupSpoolFiles() ? status : Status::SpoolCleanupFailure;
}

void encodeSpoolHeader(const std::string& rootPath,
                       uint8_t (&encoded)[SPOOL_HEADER_BYTES]) {
  std::memset(encoded, 0, sizeof(encoded));
  std::memcpy(encoded, SPOOL_HEADER_MAGIC, sizeof(SPOOL_HEADER_MAGIC));
  encoded[8] = SPOOL_VERSION;
  writeLe16(encoded + 10U, static_cast<uint16_t>(rootPath.size()));
  writeLe32(encoded + 12U,
            crc32Update(0, reinterpret_cast<const uint8_t*>(rootPath.data()),
                        rootPath.size()));
  writeLe32(encoded + 16U, crc32Update(0, encoded, 16U));
}

Status createSpool(const std::string& rootPath) {
  if (!Storage.exists("/.crosspoint") && !Storage.mkdir("/.crosspoint")) {
    return Status::SpoolOpenFailure;
  }
  HalFile file = Storage.open(PdfDirectoryDeleteScan::kSpoolTempPath,
                              O_RDWR | O_CREAT | O_TRUNC);
  if (!file) return Status::SpoolOpenFailure;
  uint8_t header[SPOOL_HEADER_BYTES];
  encodeSpoolHeader(rootPath, header);
  const bool written = writeExact(file, header, sizeof(header));
  const bool closed = file.close();
  if (!written) return Status::SpoolWriteFailure;
  return closed ? Status::Complete : Status::CloseFailure;
}

Status appendSpoolRecord(SpoolWorkspace& spool,
                         const std::string_view path) {
  constexpr uint32_t recordHeaderBytes =
      static_cast<uint32_t>(SPOOL_RECORD_HEADER_BYTES);
  if (path.empty() || path.size() >= PdfDirectoryDeleteScan::kPathCapacity ||
      path.size() > std::numeric_limits<uint16_t>::max() ||
      spool.recordCount == std::numeric_limits<uint32_t>::max() ||
      spool.recordsBytes >
          std::numeric_limits<uint32_t>::max() - recordHeaderBytes ||
      path.size() > std::numeric_limits<uint32_t>::max() -
                        recordHeaderBytes - spool.recordsBytes) {
    return Status::PathLimit;
  }

  HalFile file =
      Storage.open(PdfDirectoryDeleteScan::kSpoolTempPath, O_RDWR);
  if (!file) return Status::SpoolOpenFailure;
  const uint64_t end = file.fileSize64();
  if (!file.seek64(end)) {
    file.close();
    return Status::SpoolWriteFailure;
  }

  uint8_t recordHeader[SPOOL_RECORD_HEADER_BYTES]{};
  writeLe16(recordHeader, static_cast<uint16_t>(path.size()));
  writeLe32(
      recordHeader + 4U,
      crc32Update(0, reinterpret_cast<const uint8_t*>(path.data()),
                  path.size()));
  const bool headerWritten =
      writeExact(file, recordHeader, sizeof(recordHeader));
  const bool pathWritten =
      headerWritten &&
      writeExact(file, reinterpret_cast<const uint8_t*>(path.data()),
                 path.size());
  const bool closed = file.close();
  if (!headerWritten || !pathWritten) return Status::SpoolWriteFailure;
  if (!closed) return Status::CloseFailure;

  spool.recordsCrc =
      crc32Update(spool.recordsCrc, recordHeader, sizeof(recordHeader));
  spool.recordsCrc =
      crc32Update(spool.recordsCrc,
                  reinterpret_cast<const uint8_t*>(path.data()), path.size());
  spool.recordsBytes +=
      static_cast<uint32_t>(sizeof(recordHeader) + path.size());
  ++spool.recordCount;
  return Status::Complete;
}

Status sealSpool(const SpoolWorkspace& spool) {
  if (spool.recordCount == 0U || spool.recordsBytes == 0U) {
    return Status::SpoolCorrupt;
  }
  HalFile file =
      Storage.open(PdfDirectoryDeleteScan::kSpoolTempPath, O_RDWR);
  if (!file) return Status::SpoolOpenFailure;
  const uint64_t end = file.fileSize64();
  if (!file.seek64(end)) {
    file.close();
    return Status::SpoolWriteFailure;
  }

  uint8_t footer[SPOOL_FOOTER_BYTES]{};
  std::memcpy(footer, SPOOL_FOOTER_MAGIC, sizeof(SPOOL_FOOTER_MAGIC));
  writeLe32(footer + 8U, spool.recordCount);
  writeLe32(footer + 12U, spool.recordsBytes);
  writeLe32(footer + 16U, spool.recordsCrc);
  writeLe32(footer + 20U, crc32Update(0, footer, 20U));
  if (!writeExact(file, footer, sizeof(footer))) {
    file.close();
    return Status::SpoolWriteFailure;
  }
  if (!file.sync()) {
    file.close();
    return Status::SpoolSyncFailure;
  }
  if (!file.close()) return Status::CloseFailure;
  return Storage.rename(PdfDirectoryDeleteScan::kSpoolTempPath,
                        PdfDirectoryDeleteScan::kSpoolSealedPath)
             ? Status::Complete
             : Status::SpoolWriteFailure;
}

bool isRootBoundPdfPath(const std::string& rootPath,
                        const std::string_view path) {
  return isCanonicalAbsolutePath(path) &&
         path.size() > rootPath.size() + 1U &&
         path.compare(0, rootPath.size(), rootPath) == 0 &&
         path[rootPath.size()] == '/' && isJournaledPdfPath(path) &&
         !endsWith(path, PdfDelete::kTombstoneSuffix);
}

Status validateSpool(const std::string& rootPath,
                     LegacyWalkWorkspace& pathWorkspace,
                     SpoolWorkspace& spool) {
  HalFile file =
      Storage.open(PdfDirectoryDeleteScan::kSpoolSealedPath, O_RDONLY);
  if (!file) return Status::SpoolOpenFailure;
  const uint64_t fileSize = file.fileSize64();
  if (fileSize < SPOOL_HEADER_BYTES + SPOOL_RECORD_HEADER_BYTES + 1U +
                     SPOOL_FOOTER_BYTES) {
    file.close();
    return Status::SpoolCorrupt;
  }

  uint8_t header[SPOOL_HEADER_BYTES];
  if (!readExact(file, header, sizeof(header))) {
    file.close();
    return Status::SpoolReadFailure;
  }
  uint8_t footer[SPOOL_FOOTER_BYTES];
  const uint64_t footerOffset = fileSize - sizeof(footer);
  if (!file.seek64(footerOffset) ||
      !readExact(file, footer, sizeof(footer))) {
    file.close();
    return Status::SpoolReadFailure;
  }

  const uint32_t recordCount = readLe32(footer + 8U);
  const uint32_t recordsBytes = readLe32(footer + 12U);
  const uint32_t expectedRecordsCrc = readLe32(footer + 16U);
  const bool envelopeValid =
      std::memcmp(header, SPOOL_HEADER_MAGIC, sizeof(SPOOL_HEADER_MAGIC)) ==
          0 &&
      header[8] == SPOOL_VERSION && header[9] == 0U &&
      readLe16(header + 10U) == rootPath.size() &&
      readLe32(header + 12U) ==
          crc32Update(0,
                      reinterpret_cast<const uint8_t*>(rootPath.data()),
                      rootPath.size()) &&
      readLe32(header + 16U) == crc32Update(0, header, 16U) &&
      std::memcmp(footer, SPOOL_FOOTER_MAGIC, sizeof(SPOOL_FOOTER_MAGIC)) ==
          0 &&
      readLe32(footer + 20U) == crc32Update(0, footer, 20U) &&
      recordCount != 0U && recordsBytes != 0U &&
      static_cast<uint64_t>(SPOOL_HEADER_BYTES) + recordsBytes +
              SPOOL_FOOTER_BYTES ==
          fileSize;
  if (!envelopeValid || !file.seek64(SPOOL_HEADER_BYTES)) {
    file.close();
    return Status::SpoolCorrupt;
  }

  uint32_t actualRecordsCrc = 0;
  uint32_t actualRecordsBytes = 0;
  for (uint32_t index = 0; index < recordCount; ++index) {
    uint8_t recordHeader[SPOOL_RECORD_HEADER_BYTES];
    if (!readExact(file, recordHeader, sizeof(recordHeader))) {
      file.close();
      return Status::SpoolReadFailure;
    }
    const uint16_t pathLength = readLe16(recordHeader);
    const uint32_t encodedBytes =
        static_cast<uint32_t>(sizeof(recordHeader) + pathLength);
    if (recordHeader[2] != 0U || recordHeader[3] != 0U ||
        pathLength == 0U ||
        pathLength >= sizeof(pathWorkspace.currentPath) ||
        actualRecordsBytes > recordsBytes ||
        encodedBytes > recordsBytes - actualRecordsBytes) {
      file.close();
      return Status::SpoolCorrupt;
    }
    if (!readExact(file,
                   reinterpret_cast<uint8_t*>(pathWorkspace.currentPath),
                   pathLength)) {
      file.close();
      return Status::SpoolReadFailure;
    }
    pathWorkspace.currentPath[pathLength] = '\0';
    const std::string_view path(pathWorkspace.currentPath, pathLength);
    if (readLe32(recordHeader + 4U) !=
            crc32Update(0, reinterpret_cast<const uint8_t*>(path.data()),
                        path.size()) ||
        !isRootBoundPdfPath(rootPath, path)) {
      file.close();
      return Status::SpoolCorrupt;
    }
    actualRecordsCrc =
        crc32Update(actualRecordsCrc, recordHeader, sizeof(recordHeader));
    actualRecordsCrc =
        crc32Update(actualRecordsCrc,
                    reinterpret_cast<const uint8_t*>(path.data()),
                    path.size());
    actualRecordsBytes +=
        static_cast<uint32_t>(sizeof(recordHeader) + pathLength);
  }
  const bool valid =
      actualRecordsBytes == recordsBytes &&
      actualRecordsCrc == expectedRecordsCrc &&
      file.position() == footerOffset;
  const bool closed = file.close();
  if (!closed) return Status::CloseFailure;
  if (!valid) return Status::SpoolCorrupt;

  spool.recordCount = recordCount;
  spool.recordsBytes = recordsBytes;
  spool.recordsCrc = expectedRecordsCrc;
  spool.replayOffset = SPOOL_HEADER_BYTES;
  return Status::Complete;
}

Status readReplayRecord(const std::string& rootPath,
                        LegacyWalkWorkspace& pathWorkspace,
                        SpoolWorkspace& spool) {
  HalFile file =
      Storage.open(PdfDirectoryDeleteScan::kSpoolSealedPath, O_RDONLY);
  if (!file) return Status::SpoolOpenFailure;
  if (!file.seek64(spool.replayOffset)) {
    file.close();
    return Status::SpoolReadFailure;
  }
  uint8_t recordHeader[SPOOL_RECORD_HEADER_BYTES];
  if (!readExact(file, recordHeader, sizeof(recordHeader))) {
    file.close();
    return Status::SpoolReadFailure;
  }
  const uint16_t pathLength = readLe16(recordHeader);
  if (recordHeader[2] != 0U || recordHeader[3] != 0U ||
      pathLength == 0U || pathLength >= sizeof(pathWorkspace.currentPath) ||
      !readExact(file, reinterpret_cast<uint8_t*>(pathWorkspace.currentPath),
                 pathLength)) {
    file.close();
    return Status::SpoolReadFailure;
  }
  pathWorkspace.currentPath[pathLength] = '\0';
  const std::string_view path(pathWorkspace.currentPath, pathLength);
  const bool valid =
      readLe32(recordHeader + 4U) ==
          crc32Update(0, reinterpret_cast<const uint8_t*>(path.data()),
                      path.size()) &&
      isRootBoundPdfPath(rootPath, path);
  const bool closed = file.close();
  if (!closed) return Status::CloseFailure;
  if (!valid) return Status::SpoolCorrupt;
  spool.replayOffset += sizeof(recordHeader) + pathLength;
  return Status::Complete;
}

#if defined(SIMULATOR)
Status simulatorHiddenTombstoneStatus(const std::string& rootPath) {
  const char* configuredRoot = std::getenv("CROSSPOINT_SIM_SD");
  if (configuredRoot == nullptr || configuredRoot[0] == '\0') {
    configuredRoot = std::getenv("CROSSPOINT_EMU_SD");
  }
  const std::filesystem::path storageRoot =
      configuredRoot != nullptr && configuredRoot[0] != '\0'
          ? std::filesystem::path(configuredRoot)
          : std::filesystem::path("./fs_");
  const std::filesystem::path physicalRoot =
      storageRoot / std::filesystem::path(rootPath.substr(1));

  std::error_code error;
  if (!std::filesystem::is_directory(physicalRoot, error) || error) {
    return Status::OpenFailure;
  }

  std::filesystem::recursive_directory_iterator current(physicalRoot, error);
  const std::filesystem::recursive_directory_iterator end;
  if (error) return Status::OpenFailure;
  while (current != end) {
    const std::string filename = current->path().filename().string();
    if (endsWith(filename, PdfDelete::kTombstoneSuffix)) {
      return Status::ReservedTombstone;
    }
    current.increment(error);
    if (error) return Status::IterationFailure;
  }
  return Status::Complete;
}
#endif

bool isJournaledPdfPath(const std::string_view path) {
#if defined(CROSSINK_ENABLE_PDF) && CROSSINK_ENABLE_PDF
  return FsHelpers::hasPdfExtension(path);
#else
  (void)path;
  return false;
#endif
}

bool closeLegacyEnumeration(LegacyWalkWorkspace& workspace,
                            Status* const status) {
  const bool entryClosed = workspace.entry.close();
  const bool directoryClosed = workspace.directory.close();
  if (entryClosed && directoryClosed) return true;
  *status = Status::CloseFailure;
  return false;
}

bool initializeLegacyPath(LegacyWalkWorkspace& workspace,
                          const std::string& rootPath) {
  if (rootPath.size() >= sizeof(workspace.currentPath) ||
      rootPath.size() > std::numeric_limits<uint16_t>::max()) {
    return false;
  }
  std::memcpy(workspace.currentPath, rootPath.data(), rootPath.size());
  workspace.currentPath[rootPath.size()] = '\0';
  workspace.currentLength = static_cast<uint16_t>(rootPath.size());
  workspace.rootLength = workspace.currentLength;
  return true;
}

bool appendLegacyChild(LegacyWalkWorkspace& workspace,
                       const std::string_view name) {
  if (name.empty() || name == "." || name == ".." ||
      name.find('/') != std::string_view::npos ||
      name.find('\\') != std::string_view::npos) {
    return false;
  }
  const bool needsSlash =
      workspace.currentLength != 0U &&
      workspace.currentPath[workspace.currentLength - 1U] != '/';
  const size_t required = static_cast<size_t>(workspace.currentLength) +
                          static_cast<size_t>(needsSlash) + name.size();
  if (required >= sizeof(workspace.currentPath) ||
      required > std::numeric_limits<uint16_t>::max()) {
    return false;
  }
  size_t offset = workspace.currentLength;
  if (needsSlash) workspace.currentPath[offset++] = '/';
  std::memcpy(workspace.currentPath + offset, name.data(), name.size());
  workspace.currentLength = static_cast<uint16_t>(required);
  workspace.currentPath[workspace.currentLength] = '\0';
  return true;
}

bool ascendLegacyPath(LegacyWalkWorkspace& workspace) {
  if (workspace.currentLength <= workspace.rootLength) return false;
  size_t slash = workspace.currentLength;
  while (slash > workspace.rootLength &&
         workspace.currentPath[slash - 1U] != '/') {
    --slash;
  }
  if (slash <= workspace.rootLength) return false;
  const size_t nameLength =
      static_cast<size_t>(workspace.currentLength) - slash;
  if (nameLength == 0U || nameLength >= sizeof(workspace.resumeName)) {
    return false;
  }
  std::memcpy(workspace.resumeName, workspace.currentPath + slash, nameLength);
  workspace.resumeName[nameLength] = '\0';
  workspace.currentLength = static_cast<uint16_t>(slash - 1U);
  workspace.currentPath[workspace.currentLength] = '\0';
  workspace.resumeAfterEntry = true;
  return true;
}

Status discoverLegacyTree(
    LegacyWalkWorkspace& workspace, const std::string& rootPath,
    std::vector<std::string>& legacyPaths,
    std::unique_ptr<SpoolWorkspace>& spool) {
  while (true) {
    workspace.directory = Storage.open(workspace.currentPath);
    if (!workspace.directory || !workspace.directory.isDirectory()) {
      workspace.directory.close();
      return Status::OpenFailure;
    }

    bool reopenDirectory = false;
    while (true) {
      const DirectoryNextResult next =
          nextDirectoryEntry(workspace.directory, workspace.entry);
      if (next == DirectoryNextResult::Error) {
        Status status = Status::IterationFailure;
        closeLegacyEnumeration(workspace, &status);
        return status;
      }
      if (next == DirectoryNextResult::End) break;

      const size_t nameLength = workspace.entry.getName(
          workspace.entryName, sizeof(workspace.entryName));
      const bool isDirectory = workspace.entry.isDirectory();
      if (nameLength == 0U || nameLength >= sizeof(workspace.entryName)) {
        Status status = Status::PathLimit;
        closeLegacyEnumeration(workspace, &status);
        return status;
      }
      if (!workspace.entry.close()) {
        Status status = Status::CloseFailure;
        workspace.directory.close();
        return status;
      }

      if (workspace.resumeAfterEntry) {
        if (std::strcmp(workspace.entryName, workspace.resumeName) == 0) {
          workspace.resumeAfterEntry = false;
        }
        continue;
      }

      const std::string_view name(workspace.entryName, nameLength);
      if (endsWith(name, PdfDelete::kTombstoneSuffix)) {
        Status status = Status::ReservedTombstone;
        closeLegacyEnumeration(workspace, &status);
        return status;
      }
      if (isDirectory) {
        Status status = Status::Complete;
        if (!closeLegacyEnumeration(workspace, &status)) return status;
        if (!appendLegacyChild(workspace, name)) return Status::PathLimit;
        reopenDirectory = true;
        break;
      }
      if (isJournaledPdfPath(name)) {
        if (name.size() >= sizeof(workspace.resumeName)) {
          Status status = Status::PathLimit;
          closeLegacyEnumeration(workspace, &status);
          return status;
        }
        std::memcpy(workspace.resumeName, name.data(), name.size());
        workspace.resumeName[name.size()] = '\0';
        workspace.resumeAfterEntry = true;
        const uint16_t directoryLength = workspace.currentLength;
        Status status = Status::Complete;
        if (!closeLegacyEnumeration(workspace, &status)) return status;
        if (!appendLegacyChild(workspace, name)) return Status::PathLimit;

        if (!spool) {
          // The spool state is deliberately deferred until the first PDF so a
          // pure legacy tree never pays this allocation or any spool I/O.
          spool = makeUniqueNoThrow<SpoolWorkspace>();
          if (!spool) {
            workspace.currentLength = directoryLength;
            workspace.currentPath[directoryLength] = '\0';
            return Status::AllocationFailure;
          }
          status = createSpool(rootPath);
        }
        if (status == Status::Complete) {
          status = appendSpoolRecord(
              *spool,
              std::string_view(workspace.currentPath,
                               workspace.currentLength));
        }
        workspace.currentLength = directoryLength;
        workspace.currentPath[directoryLength] = '\0';
        if (status != Status::Complete) return status;
        reopenDirectory = true;
        break;
      }
      if (FsHelpers::hasEpubExtension(name) ||
          FsHelpers::hasXtcExtension(name) ||
          FsHelpers::hasTxtExtension(name) ||
          FsHelpers::hasMarkdownExtension(name)) {
        std::string childPath(workspace.currentPath,
                              workspace.currentLength);
        if (!childPath.empty() && childPath.back() != '/') {
          childPath.push_back('/');
        }
        childPath.append(name.data(), name.size());
        legacyPaths.push_back(std::move(childPath));
      }
    }
    if (reopenDirectory) continue;

    Status status = Status::Complete;
    if (!closeLegacyEnumeration(workspace, &status)) return status;
    if (workspace.currentLength == workspace.rootLength) {
      return Status::Complete;
    }
    if (!ascendLegacyPath(workspace)) return Status::PathLimit;
  }
}

}  // namespace

namespace PdfDirectoryDeleteScan {

Status deleteDirectoryNoThrow(const std::string& rootPath,
                              const DeleteCallbacks& callbacks) {
  if (!isCanonicalAbsolutePath(rootPath) ||
      rootPath.size() >= kPathCapacity ||
      endsWith(rootPath, PdfDelete::kTombstoneSuffix) ||
      callbacks.deletePdf == nullptr ||
      callbacks.clearLegacyMetadata == nullptr) {
    return Status::InvalidRoot;
  }
  if (!cleanupSpoolFiles()) return Status::SpoolCleanupFailure;

#if defined(SIMULATOR)
  const Status simulatorTombstoneStatus =
      simulatorHiddenTombstoneStatus(rootPath);
  if (simulatorTombstoneStatus != Status::Complete) {
    return simulatorTombstoneStatus;
  }
#endif

  // The fixed path/name state is too large for the activity task stack, but is
  // less than 2 KiB and allocated once for this rare destructive action.
  auto workspace = makeUniqueNoThrow<LegacyWalkWorkspace>();
  if (!workspace) {
    LOG_ERR("PdfDirDelete",
            "Out of memory allocating %u-byte legacy-compatible walker",
            static_cast<unsigned>(sizeof(LegacyWalkWorkspace)));
    return Status::AllocationFailure;
  }
  if (!initializeLegacyPath(*workspace, rootPath)) return Status::PathLimit;

  std::vector<std::string> legacyPaths;
  std::unique_ptr<SpoolWorkspace> spool;
  const Status discovery =
      discoverLegacyTree(*workspace, rootPath, legacyPaths, spool);
  if (discovery != Status::Complete) {
    return finishWithSpoolCleanup(discovery);
  }

  if (spool) {
    Status status = sealSpool(*spool);
    if (status == Status::Complete) {
      status = validateSpool(rootPath, *workspace, *spool);
    }
    if (status != Status::Complete) {
      return finishWithSpoolCleanup(status);
    }

    for (uint32_t index = 0; index < spool->recordCount; ++index) {
      status = readReplayRecord(rootPath, *workspace, *spool);
      if (status != Status::Complete) {
        return finishWithSpoolCleanup(status);
      }
      if (!callbacks.deletePdf(callbacks.context, workspace->currentPath)) {
        return finishWithSpoolCleanup(Status::PdfDeleteFailure);
      }
    }
    if (!cleanupSpoolFiles()) return Status::SpoolCleanupFailure;
  }

  if (!Storage.removeDir(rootPath.c_str())) {
    return Status::DirectoryDeleteFailure;
  }
  for (const std::string& path : legacyPaths) {
    callbacks.clearLegacyMetadata(callbacks.context, path);
  }
  return Status::Complete;
}

}  // namespace PdfDirectoryDeleteScan
