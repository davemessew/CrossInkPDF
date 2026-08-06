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
#if defined(SIMULATOR)
#include <filesystem>
#include <system_error>
#endif

namespace {

using PdfDirectoryDeleteScan::Status;

struct SpoolWorkspace {
  uint32_t recordCount = 0;
  uint32_t recordsBytes = 0;
  uint32_t recordsCrc = 0;
  uint32_t maxPathBytes = 0;
  uint64_t replayOffset = 0;
};

static_assert(sizeof(SpoolWorkspace) <= 32U);

struct LegacyWalkWorkspace {
  HalFile directory;
  HalFile entry;
  char currentPath[PdfDirectoryDeleteScan::kPathCapacity]{};
  char entryName[PdfDirectoryDeleteScan::kEntryNameCapacity]{};
  char resumeName[PdfDirectoryDeleteScan::kEntryNameCapacity]{};
  uint16_t currentLength = 0;
  uint16_t rootLength = 0;
  bool resumeAfterEntry = false;
  std::unique_ptr<SpoolWorkspace> spool;
};

static_assert(sizeof(LegacyWalkWorkspace) <= 2U * 1024U);

constexpr size_t SPOOL_HEADER_BYTES = 20U;
constexpr size_t SPOOL_RECORD_HEADER_BYTES = 12U;
constexpr size_t SPOOL_FOOTER_BYTES = 28U;
constexpr uint8_t SPOOL_VERSION = 2U;
constexpr uint8_t SPOOL_HEADER_MAGIC[8] = {'C', 'P', 'D', 'F', 'D', 'S', 'H', '1'};
constexpr uint8_t SPOOL_FOOTER_MAGIC[8] = {'C', 'P', 'D', 'F', 'D', 'S', 'F', '1'};
constexpr char LEGACY_SPOOL_FIRST_PATH[] = "/.crosspoint/pdf-directory-delete.legacy-a";
constexpr char LEGACY_SPOOL_RETRY_PATH[] = "/.crosspoint/pdf-directory-delete.legacy-b";

enum class SpoolRecordKind : uint8_t {
  Pdf = 1U,
  LegacyMetadata = 2U,
};

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
  if (path.size() < 2U || path.front() != '/' || path.back() == '/' || path.find('\\') != std::string_view::npos ||
      path.find('\0') != std::string_view::npos) {
    return false;
  }
  size_t segmentStart = 1U;
  while (segmentStart < path.size()) {
    const size_t segmentEnd = path.find('/', segmentStart);
    const size_t end = segmentEnd == std::string_view::npos ? path.size() : segmentEnd;
    const size_t length = end - segmentStart;
    if (length == 0U || (length == 1U && path[segmentStart] == '.') ||
        (length == 2U && path[segmentStart] == '.' && path[segmentStart + 1U] == '.')) {
      return false;
    }
    if (segmentEnd == std::string_view::npos) break;
    segmentStart = segmentEnd + 1U;
  }
  return true;
}

uint32_t crc32Update(uint32_t crc, const uint8_t* const bytes, const size_t length) {
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
  return static_cast<uint16_t>(source[0]) | static_cast<uint16_t>(static_cast<uint16_t>(source[1]) << 8U);
}

uint32_t readLe32(const uint8_t* const source) {
  return static_cast<uint32_t>(source[0]) | (static_cast<uint32_t>(source[1]) << 8U) |
         (static_cast<uint32_t>(source[2]) << 16U) | (static_cast<uint32_t>(source[3]) << 24U);
}

bool writeExact(HalFile& file, const uint8_t* const bytes, const size_t length) {
  return bytes != nullptr && length != 0U && file.write(bytes, length) == length;
}

bool readExact(HalFile& file, uint8_t* const bytes, const size_t length) {
  return bytes != nullptr && length != 0U && file.read(bytes, length) == static_cast<int>(length);
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

Status finishCommittedWithSpoolCleanup(const Status postCommitStatus) {
  const bool cleaned = cleanupSpoolFiles();
  if (postCommitStatus != Status::Complete) {
    LOG_ERR("PdfDirDelete", "Directory delete committed but metadata replay failed: %u",
            static_cast<unsigned>(postCommitStatus));
  }
  return cleaned && postCommitStatus == Status::Complete ? Status::Complete : Status::CommittedWithCleanupWarning;
}

void encodeSpoolHeader(const std::string& rootPath, uint8_t (&encoded)[SPOOL_HEADER_BYTES]) {
  std::memset(encoded, 0, sizeof(encoded));
  std::memcpy(encoded, SPOOL_HEADER_MAGIC, sizeof(SPOOL_HEADER_MAGIC));
  encoded[8] = SPOOL_VERSION;
  writeLe16(encoded + 10U, static_cast<uint16_t>(rootPath.size()));
  writeLe32(encoded + 12U, crc32Update(0, reinterpret_cast<const uint8_t*>(rootPath.data()), rootPath.size()));
  writeLe32(encoded + 16U, crc32Update(0, encoded, 16U));
}

Status createSpool(const std::string& rootPath, const char* const spoolPath) {
  if (spoolPath == nullptr) return Status::InvalidRoot;
  if (!Storage.exists("/.crosspoint") && !Storage.mkdir("/.crosspoint")) {
    return Status::SpoolOpenFailure;
  }
  HalFile file = Storage.open(spoolPath, O_RDWR | O_CREAT | O_TRUNC);
  if (!file) return Status::SpoolOpenFailure;
  uint8_t header[SPOOL_HEADER_BYTES];
  encodeSpoolHeader(rootPath, header);
  const bool written = writeExact(file, header, sizeof(header));
  const bool closed = file.close();
  if (!written) return Status::SpoolWriteFailure;
  return closed ? Status::Complete : Status::CloseFailure;
}

Status appendSpoolRecord(SpoolWorkspace& spool, const char* const spoolPath, const std::string_view path,
                         const SpoolRecordKind kind) {
  constexpr uint32_t recordHeaderBytes = static_cast<uint32_t>(SPOOL_RECORD_HEADER_BYTES);
  if (spoolPath == nullptr || path.empty() || path.size() > std::numeric_limits<uint32_t>::max() ||
      spool.recordCount == std::numeric_limits<uint32_t>::max() ||
      spool.recordsBytes > std::numeric_limits<uint32_t>::max() - recordHeaderBytes ||
      path.size() > std::numeric_limits<uint32_t>::max() - recordHeaderBytes - spool.recordsBytes) {
    return Status::PathLimit;
  }

  HalFile file = Storage.open(spoolPath, O_RDWR);
  if (!file) return Status::SpoolOpenFailure;
  const uint64_t end = file.fileSize64();
  if (!file.seek64(end)) {
    file.close();
    return Status::SpoolWriteFailure;
  }

  uint8_t recordHeader[SPOOL_RECORD_HEADER_BYTES]{};
  writeLe32(recordHeader, static_cast<uint32_t>(path.size()));
  recordHeader[4] = static_cast<uint8_t>(kind);
  writeLe32(recordHeader + 8U, crc32Update(0, reinterpret_cast<const uint8_t*>(path.data()), path.size()));
  const bool headerWritten = writeExact(file, recordHeader, sizeof(recordHeader));
  const bool pathWritten =
      headerWritten && writeExact(file, reinterpret_cast<const uint8_t*>(path.data()), path.size());
  const bool closed = file.close();
  if (!headerWritten || !pathWritten) return Status::SpoolWriteFailure;
  if (!closed) return Status::CloseFailure;

  spool.recordsCrc = crc32Update(spool.recordsCrc, recordHeader, sizeof(recordHeader));
  spool.recordsCrc = crc32Update(spool.recordsCrc, reinterpret_cast<const uint8_t*>(path.data()), path.size());
  spool.recordsBytes += static_cast<uint32_t>(sizeof(recordHeader) + path.size());
  if (path.size() > spool.maxPathBytes) {
    spool.maxPathBytes = static_cast<uint32_t>(path.size());
  }
  ++spool.recordCount;
  return Status::Complete;
}

Status sealSpoolInPlace(const SpoolWorkspace& spool, const char* const spoolPath) {
  if (spool.recordCount == 0U || spool.recordsBytes == 0U) {
    return Status::SpoolCorrupt;
  }
  HalFile file = Storage.open(spoolPath, O_RDWR);
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
  writeLe32(footer + 20U, spool.maxPathBytes);
  writeLe32(footer + 24U, crc32Update(0, footer, 24U));
  if (!writeExact(file, footer, sizeof(footer))) {
    file.close();
    return Status::SpoolWriteFailure;
  }
  if (!file.sync()) {
    file.close();
    return Status::SpoolSyncFailure;
  }
  return file.close() ? Status::Complete : Status::CloseFailure;
}

Status sealSpool(const SpoolWorkspace& spool) {
  const Status sealed = sealSpoolInPlace(spool, PdfDirectoryDeleteScan::kSpoolTempPath);
  if (sealed != Status::Complete) return sealed;
  return Storage.rename(PdfDirectoryDeleteScan::kSpoolTempPath, PdfDirectoryDeleteScan::kSpoolSealedPath)
             ? Status::Complete
             : Status::SpoolWriteFailure;
}

bool isLegacyMetadataPath(const std::string_view path) {
  return FsHelpers::hasEpubExtension(path) || FsHelpers::hasXtcExtension(path) || FsHelpers::hasTxtExtension(path) ||
         FsHelpers::hasMarkdownExtension(path);
}

bool isRootBoundRecordPath(const std::string& rootPath, const std::string_view path, const SpoolRecordKind kind) {
  if (!isCanonicalAbsolutePath(path) || path.size() <= rootPath.size() + 1U ||
      path.compare(0, rootPath.size(), rootPath) != 0 || path[rootPath.size()] != '/') {
    return false;
  }
  if (kind == SpoolRecordKind::Pdf) {
    return isJournaledPdfPath(path) && !endsWith(path, PdfDelete::kTombstoneSuffix);
  }
  return kind == SpoolRecordKind::LegacyMetadata && isLegacyMetadataPath(path);
}

Status validateSpool(const std::string& rootPath, char* const pathBuffer, const size_t pathCapacity,
                     SpoolWorkspace& spool, const char* const spoolPath) {
  if (pathBuffer == nullptr || pathCapacity == 0U || spoolPath == nullptr) {
    return Status::InvalidRoot;
  }
  HalFile file = Storage.open(spoolPath, O_RDONLY);
  if (!file) return Status::SpoolOpenFailure;
  const uint64_t fileSize = file.fileSize64();
  if (fileSize < SPOOL_HEADER_BYTES + SPOOL_RECORD_HEADER_BYTES + 1U + SPOOL_FOOTER_BYTES) {
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
  if (!file.seek64(footerOffset) || !readExact(file, footer, sizeof(footer))) {
    file.close();
    return Status::SpoolReadFailure;
  }

  const uint32_t recordCount = readLe32(footer + 8U);
  const uint32_t recordsBytes = readLe32(footer + 12U);
  const uint32_t expectedRecordsCrc = readLe32(footer + 16U);
  const uint32_t maxPathBytes = readLe32(footer + 20U);
  const bool envelopeValid =
      std::memcmp(header, SPOOL_HEADER_MAGIC, sizeof(SPOOL_HEADER_MAGIC)) == 0 && header[8] == SPOOL_VERSION &&
      header[9] == 0U && readLe16(header + 10U) == rootPath.size() &&
      readLe32(header + 12U) == crc32Update(0, reinterpret_cast<const uint8_t*>(rootPath.data()), rootPath.size()) &&
      readLe32(header + 16U) == crc32Update(0, header, 16U) &&
      std::memcmp(footer, SPOOL_FOOTER_MAGIC, sizeof(SPOOL_FOOTER_MAGIC)) == 0 &&
      readLe32(footer + 24U) == crc32Update(0, footer, 24U) && recordCount != 0U && recordsBytes != 0U &&
      maxPathBytes != 0U && maxPathBytes == spool.maxPathBytes &&
      static_cast<uint64_t>(maxPathBytes) + 1U <= pathCapacity &&
      static_cast<uint64_t>(SPOOL_HEADER_BYTES) + recordsBytes + SPOOL_FOOTER_BYTES == fileSize;
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
    const uint32_t pathLength = readLe32(recordHeader);
    const SpoolRecordKind kind = static_cast<SpoolRecordKind>(recordHeader[4]);
    const uint32_t encodedBytes = static_cast<uint32_t>(sizeof(recordHeader) + pathLength);
    if ((kind != SpoolRecordKind::Pdf && kind != SpoolRecordKind::LegacyMetadata) || recordHeader[5] != 0U ||
        recordHeader[6] != 0U || recordHeader[7] != 0U || pathLength == 0U ||
        static_cast<uint64_t>(pathLength) + 1U > pathCapacity || actualRecordsBytes > recordsBytes ||
        encodedBytes > recordsBytes - actualRecordsBytes) {
      file.close();
      return Status::SpoolCorrupt;
    }
    if (!readExact(file, reinterpret_cast<uint8_t*>(pathBuffer), pathLength)) {
      file.close();
      return Status::SpoolReadFailure;
    }
    pathBuffer[pathLength] = '\0';
    const std::string_view path(pathBuffer, pathLength);
    if (readLe32(recordHeader + 8U) != crc32Update(0, reinterpret_cast<const uint8_t*>(path.data()), path.size()) ||
        !isRootBoundRecordPath(rootPath, path, kind)) {
      file.close();
      return Status::SpoolCorrupt;
    }
    actualRecordsCrc = crc32Update(actualRecordsCrc, recordHeader, sizeof(recordHeader));
    actualRecordsCrc = crc32Update(actualRecordsCrc, reinterpret_cast<const uint8_t*>(path.data()), path.size());
    actualRecordsBytes += static_cast<uint32_t>(sizeof(recordHeader) + pathLength);
  }
  const bool valid =
      actualRecordsBytes == recordsBytes && actualRecordsCrc == expectedRecordsCrc && file.position() == footerOffset;
  const bool closed = file.close();
  if (!closed) return Status::CloseFailure;
  if (!valid) return Status::SpoolCorrupt;

  spool.recordCount = recordCount;
  spool.recordsBytes = recordsBytes;
  spool.recordsCrc = expectedRecordsCrc;
  spool.maxPathBytes = maxPathBytes;
  spool.replayOffset = SPOOL_HEADER_BYTES;
  return Status::Complete;
}

Status readOpenReplayRecord(const std::string& rootPath, char* const pathBuffer, const size_t pathCapacity,
                            SpoolWorkspace& spool, HalFile& file, SpoolRecordKind* const kindOut) {
  if (pathBuffer == nullptr || pathCapacity == 0U || kindOut == nullptr || !file) {
    return Status::InvalidRoot;
  }
  uint8_t recordHeader[SPOOL_RECORD_HEADER_BYTES];
  if (!readExact(file, recordHeader, sizeof(recordHeader))) {
    return Status::SpoolReadFailure;
  }
  const uint32_t pathLength = readLe32(recordHeader);
  const SpoolRecordKind kind = static_cast<SpoolRecordKind>(recordHeader[4]);
  if ((kind != SpoolRecordKind::Pdf && kind != SpoolRecordKind::LegacyMetadata) || recordHeader[5] != 0U ||
      recordHeader[6] != 0U || recordHeader[7] != 0U || pathLength == 0U ||
      static_cast<uint64_t>(pathLength) + 1U > pathCapacity ||
      !readExact(file, reinterpret_cast<uint8_t*>(pathBuffer), pathLength)) {
    return Status::SpoolReadFailure;
  }
  pathBuffer[pathLength] = '\0';
  const std::string_view path(pathBuffer, pathLength);
  if (readLe32(recordHeader + 8U) != crc32Update(0, reinterpret_cast<const uint8_t*>(path.data()), path.size()) ||
      !isRootBoundRecordPath(rootPath, path, kind)) {
    return Status::SpoolCorrupt;
  }
  spool.replayOffset += sizeof(recordHeader) + pathLength;
  *kindOut = kind;
  return Status::Complete;
}

Status openReplayFile(HalFile& file, const SpoolWorkspace& spool, const char* const spoolPath) {
  if (spoolPath == nullptr) return Status::InvalidRoot;
  file = Storage.open(spoolPath, O_RDONLY);
  if (!file) return Status::SpoolOpenFailure;
  if (!file.seek64(spool.replayOffset)) return Status::SpoolReadFailure;
  return Status::Complete;
}

Status readReplayRecord(const std::string& rootPath, char* const pathBuffer, const size_t pathCapacity,
                        SpoolWorkspace& spool, const char* const spoolPath, SpoolRecordKind* const kindOut) {
  if (pathBuffer == nullptr || pathCapacity == 0U || spoolPath == nullptr || kindOut == nullptr) {
    return Status::InvalidRoot;
  }
  HalFile file;
  Status status = openReplayFile(file, spool, spoolPath);
  if (status == Status::Complete) {
    status = readOpenReplayRecord(rootPath, pathBuffer, pathCapacity, spool, file, kindOut);
  }
  const bool closed = file.close();
  if (status != Status::Complete) return status;
  return closed ? Status::Complete : Status::CloseFailure;
}

#if defined(SIMULATOR)
Status simulatorHiddenTombstoneStatus(const std::string& rootPath) {
  const char* configuredRoot = std::getenv("CROSSPOINT_SIM_SD");
  if (configuredRoot == nullptr || configuredRoot[0] == '\0') {
    configuredRoot = std::getenv("CROSSPOINT_EMU_SD");
  }
  const std::filesystem::path storageRoot = configuredRoot != nullptr && configuredRoot[0] != '\0'
                                                ? std::filesystem::path(configuredRoot)
                                                : std::filesystem::path("./fs_");
  const std::filesystem::path physicalRoot = storageRoot / std::filesystem::path(rootPath.substr(1));

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

struct LegacyTreeDiscovery {
  Status status = Status::Complete;
  bool foundPdf = false;
  bool hasMetadataSpool = false;
  SpoolWorkspace metadataSpool;
  const char* metadataSpoolPath = nullptr;
};

constexpr size_t LEGACY_INLINE_NAME_CAPACITY = 128U;
constexpr size_t LEGACY_INLINE_PATH_CAPACITY = 256U;
constexpr size_t LEGACY_INLINE_DEPTH = 8U;

struct LegacyEntryName {
  char local[LEGACY_INLINE_NAME_CAPACITY]{};
  std::unique_ptr<char[]> dynamic;
  size_t dynamicCapacity = 0;
  const char* data = local;
  size_t length = 0;

  std::string_view view() const { return {data, length}; }
};

struct LegacyPathBuffer {
  char local[LEGACY_INLINE_PATH_CAPACITY]{};
  std::unique_ptr<char[]> dynamic;
  char* data = local;
  size_t length = 0;
  size_t capacity = sizeof(local);
};

struct LegacyTraversalFrame {
  size_t parentLength = 0;
  size_t consumedEntries = 0;
  uint64_t resumeOffset = 0;
};

struct LegacyTraversalStack {
  LegacyTraversalFrame local[LEGACY_INLINE_DEPTH]{};
  std::unique_ptr<LegacyTraversalFrame[]> dynamic;
  LegacyTraversalFrame* data = local;
  size_t size = 0;
  size_t capacity = LEGACY_INLINE_DEPTH;
};

struct LegacyDiscoveryWorkspace {
  LegacyPathBuffer path;
  LegacyTraversalStack stack;
  LegacyEntryName name;
  HalFile directory;
  HalFile entry;
};

static_assert(sizeof(LegacyDiscoveryWorkspace) <= 1024U);

Status readEntryNameNoThrow(HalFile& entry, LegacyEntryName& name) {
  size_t length = entry.getName(name.local, sizeof(name.local));
  if (length > 0U && length < sizeof(name.local) - 1U) {
    name.data = name.local;
    name.length = length;
    return Status::Complete;
  }

  size_t capacity = name.dynamicCapacity == 0U ? sizeof(name.local) * 2U : name.dynamicCapacity;
  while (true) {
    if (!name.dynamic || name.dynamicCapacity < capacity) {
      auto dynamicName = makeUniqueNoThrow<char[]>(capacity);
      if (!dynamicName) return Status::AllocationFailure;
      name.dynamic = std::move(dynamicName);
      name.dynamicCapacity = capacity;
    }
    length = entry.getName(name.dynamic.get(), name.dynamicCapacity);
    if (length > 0U && length < capacity - 1U) {
      name.data = name.dynamic.get();
      name.length = length;
      return Status::Complete;
    }
    if (capacity > std::numeric_limits<size_t>::max() / 2U) {
      return Status::AllocationFailure;
    }
    capacity *= 2U;
  }
}

Status ensureLegacyPathCapacity(LegacyPathBuffer& path, const size_t required) {
  if (required <= path.capacity) return Status::Complete;
  size_t capacity = path.capacity;
  while (capacity < required) {
    if (capacity > std::numeric_limits<size_t>::max() / 2U) {
      return Status::AllocationFailure;
    }
    capacity *= 2U;
  }
  auto dynamicPath = makeUniqueNoThrow<char[]>(capacity);
  if (!dynamicPath) return Status::AllocationFailure;
  std::memcpy(dynamicPath.get(), path.data, path.length + 1U);
  path.dynamic = std::move(dynamicPath);
  path.data = path.dynamic.get();
  path.capacity = capacity;
  return Status::Complete;
}

Status initializeLegacyPath(LegacyPathBuffer& path, const std::string& rootPath) {
  if (rootPath.size() == std::numeric_limits<size_t>::max()) {
    return Status::AllocationFailure;
  }
  const Status capacityStatus = ensureLegacyPathCapacity(path, rootPath.size() + 1U);
  if (capacityStatus != Status::Complete) return capacityStatus;
  std::memcpy(path.data, rootPath.data(), rootPath.size());
  path.length = rootPath.size();
  path.data[path.length] = '\0';
  return Status::Complete;
}

Status appendLegacyPath(LegacyPathBuffer& path, const std::string_view name) {
  const size_t separator = path.length != 0U && path.data[path.length - 1U] != '/' ? 1U : 0U;
  const size_t maximum = std::numeric_limits<size_t>::max();
  if (separator > maximum - path.length) {
    return Status::AllocationFailure;
  }
  const size_t prefixLength = path.length + separator;
  if (prefixLength == maximum || name.size() > maximum - prefixLength - 1U) {
    return Status::AllocationFailure;
  }
  const size_t required = prefixLength + name.size() + 1U;
  const Status capacityStatus = ensureLegacyPathCapacity(path, required);
  if (capacityStatus != Status::Complete) return capacityStatus;
  if (separator != 0U) path.data[path.length++] = '/';
  std::memcpy(path.data + path.length, name.data(), name.size());
  path.length += name.size();
  path.data[path.length] = '\0';
  return Status::Complete;
}

void truncateLegacyPath(LegacyPathBuffer& path, const size_t length) {
  path.length = length;
  path.data[length] = '\0';
}

Status pushLegacyTraversalFrame(LegacyTraversalStack& stack, const LegacyTraversalFrame& frame) {
  if (stack.size == stack.capacity) {
    if (stack.capacity > std::numeric_limits<size_t>::max() / 2U ||
        stack.capacity * 2U > std::numeric_limits<size_t>::max() / sizeof(LegacyTraversalFrame)) {
      return Status::AllocationFailure;
    }
    const size_t capacity = stack.capacity * 2U;
    auto dynamicFrames = makeUniqueNoThrow<LegacyTraversalFrame[]>(capacity);
    if (!dynamicFrames) return Status::AllocationFailure;
    std::memcpy(dynamicFrames.get(), stack.data, stack.size * sizeof(LegacyTraversalFrame));
    stack.dynamic = std::move(dynamicFrames);
    stack.data = stack.dynamic.get();
    stack.capacity = capacity;
  }
  stack.data[stack.size++] = frame;
  return Status::Complete;
}

LegacyTraversalFrame popLegacyTraversalFrame(LegacyTraversalStack& stack) { return stack.data[--stack.size]; }

Status closeLegacyDiscovery(HalFile& entry, HalFile& directory, const Status status) {
  const bool entryClosed = entry.close();
  const bool directoryClosed = directory.close();
  return entryClosed && directoryClosed ? status : Status::CloseFailure;
}

LegacyTreeDiscovery discoverLegacyTreeOnce(LegacyDiscoveryWorkspace& workspace, const std::string& rootPath,
                                           const char* const metadataSpoolPath) {
  LegacyTreeDiscovery discovery;
  discovery.metadataSpoolPath = metadataSpoolPath;
  LegacyPathBuffer& path = workspace.path;
  LegacyTraversalStack& stack = workspace.stack;
  LegacyEntryName& name = workspace.name;
  HalFile& directory = workspace.directory;
  HalFile& entry = workspace.entry;
  stack.size = 0U;
  name.data = name.local;
  name.length = 0U;
  entry.close();
  directory.close();
  discovery.status = initializeLegacyPath(path, rootPath);
  if (discovery.status != Status::Complete) return discovery;
  directory = Storage.open(path.data);
  if (!directory || !directory.isDirectory()) {
    discovery.status = directory.close() ? Status::OpenFailure : Status::CloseFailure;
    return discovery;
  }

  size_t consumedEntries = 0U;
  while (true) {
    const DirectoryNextResult next = nextDirectoryEntry(directory, entry);
    if (next == DirectoryNextResult::Error) {
      discovery.status = closeLegacyDiscovery(entry, directory, Status::IterationFailure);
      return discovery;
    }
    if (next == DirectoryNextResult::End) {
      discovery.status = closeLegacyDiscovery(entry, directory, Status::Complete);
      if (discovery.status != Status::Complete || stack.size == 0U) {
        return discovery;
      }

      const LegacyTraversalFrame frame = popLegacyTraversalFrame(stack);
      truncateLegacyPath(path, frame.parentLength);
      directory = Storage.open(path.data);
      if (!directory || !directory.isDirectory()) {
        discovery.status = directory.close() ? Status::OpenFailure : Status::CloseFailure;
        return discovery;
      }
#if defined(SIMULATOR)
      for (size_t index = 0; index < frame.consumedEntries; ++index) {
        if (nextDirectoryEntry(directory, entry) != DirectoryNextResult::Entry) {
          discovery.status = closeLegacyDiscovery(entry, directory, Status::IterationFailure);
          return discovery;
        }
      }
#else
      if (!directory.seek64(frame.resumeOffset)) {
        discovery.status = closeLegacyDiscovery(entry, directory, Status::IterationFailure);
        return discovery;
      }
#endif
      consumedEntries = frame.consumedEntries;
      continue;
    }

    ++consumedEntries;
    const bool isDirectory = entry.isDirectory();
    const Status nameStatus = readEntryNameNoThrow(entry, name);
    if (nameStatus != Status::Complete) {
      discovery.status = closeLegacyDiscovery(entry, directory, nameStatus);
      return discovery;
    }
    const std::string_view nameView = name.view();
    if (nameView.empty() || nameView == "." || nameView == ".." || nameView.find('/') != std::string_view::npos ||
        nameView.find('\\') != std::string_view::npos) {
      discovery.status = closeLegacyDiscovery(entry, directory, Status::PathLimit);
      return discovery;
    }
    if (!isDirectory && (endsWith(nameView, PdfDelete::kTombstoneSuffix) || isJournaledPdfPath(nameView))) {
      discovery.foundPdf = true;
      discovery.status = closeLegacyDiscovery(entry, directory, Status::Complete);
      return discovery;
    }

    const size_t parentLength = path.length;
    const Status pathStatus = appendLegacyPath(path, nameView);
    if (pathStatus != Status::Complete) {
      discovery.status = closeLegacyDiscovery(entry, directory, pathStatus);
      return discovery;
    }
    if (isDirectory) {
      const LegacyTraversalFrame frame{parentLength, consumedEntries, static_cast<uint64_t>(directory.position())};
      const Status frameStatus = pushLegacyTraversalFrame(stack, frame);
      if (frameStatus != Status::Complete) {
        truncateLegacyPath(path, parentLength);
        discovery.status = closeLegacyDiscovery(entry, directory, frameStatus);
        return discovery;
      }
      discovery.status = closeLegacyDiscovery(entry, directory, Status::Complete);
      if (discovery.status != Status::Complete) return discovery;
      directory = Storage.open(path.data);
      if (!directory || !directory.isDirectory()) {
        discovery.status = directory.close() ? Status::OpenFailure : Status::CloseFailure;
        return discovery;
      }
      consumedEntries = 0U;
      continue;
    }

    if (metadataSpoolPath != nullptr && isLegacyMetadataPath(nameView)) {
      const uint64_t resumeOffset = static_cast<uint64_t>(directory.position());
      discovery.status = closeLegacyDiscovery(entry, directory, Status::Complete);
      if (discovery.status != Status::Complete) return discovery;
      if (!discovery.hasMetadataSpool) {
        discovery.status = createSpool(rootPath, metadataSpoolPath);
        if (discovery.status == Status::Complete) {
          discovery.hasMetadataSpool = true;
        }
      }
      if (discovery.status == Status::Complete) {
        discovery.status = appendSpoolRecord(discovery.metadataSpool, metadataSpoolPath,
                                             std::string_view(path.data, path.length), SpoolRecordKind::LegacyMetadata);
      }
      if (discovery.status != Status::Complete) {
        discovery.status = closeLegacyDiscovery(entry, directory, discovery.status);
        return discovery;
      }
      truncateLegacyPath(path, parentLength);
      directory = Storage.open(path.data);
      if (!directory || !directory.isDirectory()) {
        discovery.status = directory.close() ? Status::OpenFailure : Status::CloseFailure;
        return discovery;
      }
#if defined(SIMULATOR)
      (void)resumeOffset;
      for (size_t index = 0; index < consumedEntries; ++index) {
        if (nextDirectoryEntry(directory, entry) != DirectoryNextResult::Entry) {
          discovery.status = closeLegacyDiscovery(entry, directory, Status::IterationFailure);
          return discovery;
        }
      }
#else
      if (!directory.seek64(resumeOffset)) {
        discovery.status = closeLegacyDiscovery(entry, directory, Status::IterationFailure);
        return discovery;
      }
#endif
      continue;
    }
    truncateLegacyPath(path, parentLength);
  }
}

bool isRetryableLegacyDiscoveryFailure(const Status status) {
  return status == Status::OpenFailure || status == Status::IterationFailure || status == Status::CloseFailure;
}

bool cleanupLegacySpoolFiles() {
  bool cleaned = true;
  for (const char* const path : {LEGACY_SPOOL_FIRST_PATH, LEGACY_SPOOL_RETRY_PATH}) {
    if (Storage.exists(path) && !Storage.remove(path)) cleaned = false;
  }
  return cleaned;
}

Status sealLegacyMetadataSpool(const LegacyTreeDiscovery& discovery) {
  if (!discovery.hasMetadataSpool) return Status::Complete;
  return sealSpoolInPlace(discovery.metadataSpool, discovery.metadataSpoolPath);
}

LegacyTreeDiscovery discoverLegacyTreeWithRetry(const std::string& rootPath) {
  auto workspace = makeUniqueNoThrow<LegacyDiscoveryWorkspace>();
  if (!workspace) {
    LegacyTreeDiscovery failure;
    failure.status = Status::AllocationFailure;
    return failure;
  }
  LegacyTreeDiscovery discovery = discoverLegacyTreeOnce(*workspace, rootPath, LEGACY_SPOOL_FIRST_PATH);
  if (isRetryableLegacyDiscoveryFailure(discovery.status)) {
    const Status firstSeal = sealLegacyMetadataSpool(discovery);
    if (firstSeal != Status::Complete) {
      discovery.status = firstSeal;
      return discovery;
    }
    LegacyTreeDiscovery retry = discoverLegacyTreeOnce(*workspace, rootPath, LEGACY_SPOOL_RETRY_PATH);
    retry.foundPdf = retry.foundPdf || discovery.foundPdf;
    // Traversal order is deterministic, so the farther attempt contains the
    // useful metadata prefix. Keep its sealed spool without merging paths.
    const Status retrySeal = sealLegacyMetadataSpool(retry);
    if (retrySeal != Status::Complete) {
      retry.status = retrySeal;
      return retry;
    }
    if (!retry.foundPdf && discovery.metadataSpool.recordCount > retry.metadataSpool.recordCount) {
      retry.hasMetadataSpool = discovery.hasMetadataSpool;
      retry.metadataSpool = discovery.metadataSpool;
      retry.metadataSpoolPath = discovery.metadataSpoolPath;
    }
    discovery = retry;
  } else {
    const Status seal = sealLegacyMetadataSpool(discovery);
    if (seal != Status::Complete) discovery.status = seal;
  }
  if (discovery.foundPdf) {
    discovery.hasMetadataSpool = false;
    if (!cleanupLegacySpoolFiles()) {
      discovery.status = Status::SpoolCleanupFailure;
    }
  }
  return discovery;
}

Status commitLegacyDirectoryDelete(const std::string& rootPath,
                                   const PdfDirectoryDeleteScan::DeleteCallbacks& callbacks,
                                   const LegacyTreeDiscovery& discovery) {
  std::unique_ptr<char[]> replayPath;
  SpoolWorkspace replaySpool = discovery.metadataSpool;
  if (discovery.hasMetadataSpool) {
    if (replaySpool.maxPathBytes == std::numeric_limits<uint32_t>::max()) {
      return Status::AllocationFailure;
    }
    replayPath = makeUniqueNoThrow<char[]>(static_cast<size_t>(replaySpool.maxPathBytes) + 1U);
    if (!replayPath) return Status::AllocationFailure;
    const Status validation =
        validateSpool(rootPath, replayPath.get(), static_cast<size_t>(replaySpool.maxPathBytes) + 1U, replaySpool,
                      discovery.metadataSpoolPath);
    if (validation != Status::Complete) return validation;
  }
  if (!Storage.removeDir(rootPath.c_str())) {
    return Status::DirectoryDeleteFailure;
  }
  if (discovery.hasMetadataSpool) {
    HalFile replayFile;
    Status replayStatus = openReplayFile(replayFile, replaySpool, discovery.metadataSpoolPath);
    bool metadataCleanupComplete = true;
    for (uint32_t index = 0; index < replaySpool.recordCount; ++index) {
      if (replayStatus != Status::Complete) break;
      SpoolRecordKind kind = SpoolRecordKind::Pdf;
      replayStatus =
          readOpenReplayRecord(rootPath, replayPath.get(), static_cast<size_t>(replaySpool.maxPathBytes) + 1U,
                               replaySpool, replayFile, &kind);
      if (replayStatus != Status::Complete) break;
      if (kind != SpoolRecordKind::LegacyMetadata) {
        replayStatus = Status::SpoolCorrupt;
        break;
      }
      if (!callbacks.clearLegacyMetadata(callbacks.context, replayPath.get())) {
        metadataCleanupComplete = false;
      }
    }
    const bool replayClosed = replayFile.close();
    if (replayStatus == Status::Complete && !replayClosed) {
      replayStatus = Status::CloseFailure;
    }
    if (replayStatus == Status::Complete && !metadataCleanupComplete) {
      replayStatus = Status::MetadataCleanupFailure;
    }
    if (replayStatus != Status::Complete) {
      cleanupLegacySpoolFiles();
      LOG_ERR("PdfDirDelete", "Legacy directory delete committed but metadata replay failed: %u",
              static_cast<unsigned>(replayStatus));
      return Status::CommittedWithCleanupWarning;
    }
  }
  return cleanupLegacySpoolFiles() ? Status::Complete : Status::CommittedWithCleanupWarning;
}

Status abortIncompleteLegacyDiscovery(const LegacyTreeDiscovery& discovery) {
  const Status discoveryStatus = discovery.status;
  if (discoveryStatus == Status::SpoolCleanupFailure) return discoveryStatus;
  if (!cleanupLegacySpoolFiles()) return Status::SpoolCleanupFailure;
  return discoveryStatus;
}

bool closeLegacyEnumeration(LegacyWalkWorkspace& workspace, Status* const status) {
  const bool entryClosed = workspace.entry.close();
  const bool directoryClosed = workspace.directory.close();
  if (entryClosed && directoryClosed) return true;
  *status = Status::CloseFailure;
  return false;
}

bool initializeLegacyPath(LegacyWalkWorkspace& workspace, const std::string& rootPath) {
  if (rootPath.size() >= sizeof(workspace.currentPath) || rootPath.size() > std::numeric_limits<uint16_t>::max()) {
    return false;
  }
  std::memcpy(workspace.currentPath, rootPath.data(), rootPath.size());
  workspace.currentPath[rootPath.size()] = '\0';
  workspace.currentLength = static_cast<uint16_t>(rootPath.size());
  workspace.rootLength = workspace.currentLength;
  return true;
}

bool appendLegacyChild(LegacyWalkWorkspace& workspace, const std::string_view name) {
  if (name.empty() || name == "." || name == ".." || name.find('/') != std::string_view::npos ||
      name.find('\\') != std::string_view::npos) {
    return false;
  }
  const bool needsSlash = workspace.currentLength != 0U && workspace.currentPath[workspace.currentLength - 1U] != '/';
  const size_t required = static_cast<size_t>(workspace.currentLength) + static_cast<size_t>(needsSlash) + name.size();
  if (required >= sizeof(workspace.currentPath) || required > std::numeric_limits<uint16_t>::max()) {
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
  while (slash > workspace.rootLength && workspace.currentPath[slash - 1U] != '/') {
    --slash;
  }
  if (slash <= workspace.rootLength) return false;
  const size_t nameLength = static_cast<size_t>(workspace.currentLength) - slash;
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

Status discoverLegacyTree(LegacyWalkWorkspace& workspace, const std::string& rootPath,
                          std::unique_ptr<SpoolWorkspace>& spool) {
  while (true) {
    workspace.directory = Storage.open(workspace.currentPath);
    if (!workspace.directory || !workspace.directory.isDirectory()) {
      workspace.directory.close();
      return Status::OpenFailure;
    }

    bool reopenDirectory = false;
    while (true) {
      const DirectoryNextResult next = nextDirectoryEntry(workspace.directory, workspace.entry);
      if (next == DirectoryNextResult::Error) {
        Status status = Status::IterationFailure;
        closeLegacyEnumeration(workspace, &status);
        return status;
      }
      if (next == DirectoryNextResult::End) break;

      const size_t nameLength = workspace.entry.getName(workspace.entryName, sizeof(workspace.entryName));
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
          status = createSpool(rootPath, PdfDirectoryDeleteScan::kSpoolTempPath);
        }
        if (status == Status::Complete) {
          status =
              appendSpoolRecord(*spool, PdfDirectoryDeleteScan::kSpoolTempPath,
                                std::string_view(workspace.currentPath, workspace.currentLength), SpoolRecordKind::Pdf);
        }
        workspace.currentLength = directoryLength;
        workspace.currentPath[directoryLength] = '\0';
        if (status != Status::Complete) return status;
        reopenDirectory = true;
        break;
      }
      if (isLegacyMetadataPath(name)) {
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
          spool = makeUniqueNoThrow<SpoolWorkspace>();
          if (!spool) {
            workspace.currentLength = directoryLength;
            workspace.currentPath[directoryLength] = '\0';
            return Status::AllocationFailure;
          }
          status = createSpool(rootPath, PdfDirectoryDeleteScan::kSpoolTempPath);
        }
        if (status == Status::Complete) {
          status = appendSpoolRecord(*spool, PdfDirectoryDeleteScan::kSpoolTempPath,
                                     std::string_view(workspace.currentPath, workspace.currentLength),
                                     SpoolRecordKind::LegacyMetadata);
        }
        workspace.currentLength = directoryLength;
        workspace.currentPath[directoryLength] = '\0';
        if (status != Status::Complete) return status;
        reopenDirectory = true;
        break;
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

Status containsPdfNoThrow(const std::string& rootPath, bool* const containsPdf) {
  if (containsPdf == nullptr || !isCanonicalAbsolutePath(rootPath)) {
    return Status::InvalidRoot;
  }
  *containsPdf = false;

#if defined(SIMULATOR)
  const Status simulatorTombstoneStatus = simulatorHiddenTombstoneStatus(rootPath);
  if (simulatorTombstoneStatus == Status::ReservedTombstone) {
    return Status::ReservedTombstone;
  }
  if (simulatorTombstoneStatus != Status::Complete) return simulatorTombstoneStatus;
#endif

  auto workspace = makeUniqueNoThrow<LegacyDiscoveryWorkspace>();
  if (!workspace) return Status::AllocationFailure;

  LegacyTreeDiscovery discovery = discoverLegacyTreeOnce(*workspace, rootPath, nullptr);
  if (isRetryableLegacyDiscoveryFailure(discovery.status)) {
    LegacyTreeDiscovery retry = discoverLegacyTreeOnce(*workspace, rootPath, nullptr);
    retry.foundPdf = retry.foundPdf || discovery.foundPdf;
    discovery = retry;
  }
  if (discovery.status != Status::Complete) return discovery.status;
  *containsPdf = discovery.foundPdf;
  return Status::Complete;
}

Status deleteLegacyDirectoryNoThrow(const std::string& rootPath, const DeleteCallbacks& callbacks) {
  if (!isCanonicalAbsolutePath(rootPath) || callbacks.clearLegacyMetadata == nullptr) {
    return Status::InvalidRoot;
  }

  LegacyTreeDiscovery discovery = discoverLegacyTreeWithRetry(rootPath);
  if (discovery.status != Status::Complete) {
    return abortIncompleteLegacyDiscovery(discovery);
  }
  if (discovery.foundPdf) return Status::PdfDeleteFailure;
  return commitLegacyDirectoryDelete(rootPath, callbacks, discovery);
}

Status deletePdfDirectoryNoThrow(const std::string& rootPath, const DeleteCallbacks& callbacks) {
  if (!isCanonicalAbsolutePath(rootPath) || rootPath.size() >= kPathCapacity ||
      endsWith(rootPath, PdfDelete::kTombstoneSuffix) || callbacks.deletePdf == nullptr ||
      callbacks.clearLegacyMetadata == nullptr) {
    return Status::InvalidRoot;
  }
  if (!cleanupSpoolFiles()) return Status::SpoolCleanupFailure;

#if defined(SIMULATOR)
  const Status simulatorTombstoneStatus = simulatorHiddenTombstoneStatus(rootPath);
  if (simulatorTombstoneStatus != Status::Complete) {
    return simulatorTombstoneStatus;
  }
#endif

  // The fixed path/name state is too large for the activity task stack, but is
  // less than 2 KiB and allocated once for this rare destructive action.
  auto workspace = makeUniqueNoThrow<LegacyWalkWorkspace>();
  if (!workspace) {
    LOG_ERR("PdfDirDelete", "Out of memory allocating %u-byte legacy-compatible walker",
            static_cast<unsigned>(sizeof(LegacyWalkWorkspace)));
    return Status::AllocationFailure;
  }
  if (!initializeLegacyPath(*workspace, rootPath)) return Status::PathLimit;

  const Status discovery = discoverLegacyTree(*workspace, rootPath, workspace->spool);
  if (discovery != Status::Complete) {
    return finishWithSpoolCleanup(discovery);
  }

  if (workspace->spool) {
    Status status = sealSpool(*workspace->spool);
    if (status == Status::Complete) {
      status = validateSpool(rootPath, workspace->currentPath, sizeof(workspace->currentPath), *workspace->spool,
                             kSpoolSealedPath);
    }
    if (status != Status::Complete) {
      return finishWithSpoolCleanup(status);
    }

    for (uint32_t index = 0; index < workspace->spool->recordCount; ++index) {
      SpoolRecordKind kind = SpoolRecordKind::LegacyMetadata;
      status = readReplayRecord(rootPath, workspace->currentPath, sizeof(workspace->currentPath), *workspace->spool,
                                kSpoolSealedPath, &kind);
      if (status != Status::Complete) {
        return finishWithSpoolCleanup(status);
      }
      if (kind == SpoolRecordKind::Pdf && !callbacks.deletePdf(callbacks.context, workspace->currentPath)) {
        return finishWithSpoolCleanup(Status::PdfDeleteFailure);
      }
    }
  }

  if (!Storage.removeDir(rootPath.c_str())) {
    return finishWithSpoolCleanup(Status::DirectoryDeleteFailure);
  }
  if (workspace->spool) {
    workspace->spool->replayOffset = SPOOL_HEADER_BYTES;
    HalFile replayFile;
    Status status = openReplayFile(replayFile, *workspace->spool, kSpoolSealedPath);
    bool metadataCleanupComplete = true;
    for (uint32_t index = 0; index < workspace->spool->recordCount; ++index) {
      if (status != Status::Complete) break;
      SpoolRecordKind kind = SpoolRecordKind::Pdf;
      status = readOpenReplayRecord(rootPath, workspace->currentPath, sizeof(workspace->currentPath), *workspace->spool,
                                    replayFile, &kind);
      if (status != Status::Complete) break;
      if (kind == SpoolRecordKind::LegacyMetadata) {
        if (!callbacks.clearLegacyMetadata(callbacks.context, workspace->currentPath)) {
          metadataCleanupComplete = false;
        }
      }
    }
    const bool replayClosed = replayFile.close();
    if (status == Status::Complete && !replayClosed) {
      status = Status::CloseFailure;
    }
    if (status == Status::Complete && !metadataCleanupComplete) {
      status = Status::MetadataCleanupFailure;
    }
    if (status != Status::Complete) {
      return finishCommittedWithSpoolCleanup(status);
    }
  }
  return finishCommittedWithSpoolCleanup(Status::Complete);
}

Status deleteDirectoryNoThrow(const std::string& rootPath, const DeleteCallbacks& callbacks) {
  if (!isCanonicalAbsolutePath(rootPath) || callbacks.deletePdf == nullptr ||
      callbacks.clearLegacyMetadata == nullptr) {
    return Status::InvalidRoot;
  }

#if defined(SIMULATOR)
  const Status simulatorTombstoneStatus = simulatorHiddenTombstoneStatus(rootPath);
  if (simulatorTombstoneStatus != Status::Complete) {
    return simulatorTombstoneStatus;
  }
#endif

  {
    LegacyTreeDiscovery discovery = discoverLegacyTreeWithRetry(rootPath);
    if (discovery.status != Status::Complete) {
      return abortIncompleteLegacyDiscovery(discovery);
    }
    if (!discovery.foundPdf) {
      return commitLegacyDirectoryDelete(rootPath, callbacks, discovery);
    }
  }
  if (callbacks.preparePdfDelete != nullptr && !callbacks.preparePdfDelete(callbacks.context)) {
    return Status::PdfRecoveryFailure;
  }
  return deletePdfDirectoryNoThrow(rootPath, callbacks);
}

}  // namespace PdfDirectoryDeleteScan
