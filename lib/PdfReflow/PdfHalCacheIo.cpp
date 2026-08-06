#include "PdfHalCacheIo.h"

#include <algorithm>
#include <cstring>
#include <utility>

namespace {

PdfStatus invalidHandle() { return PdfStatus::failure(PdfError::InvalidArgument); }

bool validHandle(const PdfHalCacheIoContext& context, const PdfCacheHandle handle) {
  return handle.valid() && handle.value < PDF_HAL_CACHE_HANDLE_COUNT && context.used[handle.value] &&
         context.files[handle.value].isOpen();
}

PdfStatus openFile(void* rawContext, const char* const path, const PdfCacheOpenMode mode,
                   PdfCacheHandle* const handle) {
  if (rawContext == nullptr || path == nullptr || handle == nullptr) {
    return PdfStatus::failure(PdfError::InvalidArgument);
  }
  auto& context = *static_cast<PdfHalCacheIoContext*>(rawContext);
  *handle = PdfCacheHandle{};
  uint8_t slot = PDF_HAL_CACHE_HANDLE_COUNT;
  for (uint8_t index = 0; index < PDF_HAL_CACHE_HANDLE_COUNT; ++index) {
    if (!context.used[index]) {
      slot = index;
      break;
    }
  }
  if (slot == PDF_HAL_CACHE_HANDLE_COUNT) {
    return PdfStatus::failure(PdfError::LimitExceeded);
  }
  if (mode == PdfCacheOpenMode::Read && !Storage.exists(path)) {
    return PdfStatus::failure(PdfError::InvalidOffset);
  }
  const oflag_t flags = mode == PdfCacheOpenMode::Read          ? O_RDONLY
                        : mode == PdfCacheOpenMode::ReadWrite   ? static_cast<oflag_t>(O_RDWR | O_CREAT)
                        : mode == PdfCacheOpenMode::Write       ? static_cast<oflag_t>(O_WRONLY | O_CREAT)
                                                                : static_cast<oflag_t>(O_WRONLY | O_CREAT | O_TRUNC);
  HalFile opened = Storage.open(path, flags);
  if (!opened) {
    return PdfStatus::failure(PdfError::IoFailure);
  }
  context.files[slot] = std::move(opened);
  context.used[slot] = true;
  handle->value = slot;
  return PdfStatus::success();
}

PdfStatus readFile(void* rawContext, const PdfCacheHandle handle, const uint64_t offset, uint8_t* const destination,
                   const size_t requested, size_t* const bytesRead) {
  if (rawContext == nullptr || destination == nullptr || bytesRead == nullptr) {
    return PdfStatus::failure(PdfError::InvalidArgument, offset);
  }
  auto& context = *static_cast<PdfHalCacheIoContext*>(rawContext);
  *bytesRead = 0;
  if (!validHandle(context, handle)) {
    return invalidHandle();
  }
  HalFile& file = context.files[handle.value];
  if (!file.seek64(offset)) {
    return PdfStatus::failure(PdfError::IoFailure, offset);
  }
  const int result = file.read(destination, requested);
  if (result < 0) {
    return PdfStatus::failure(PdfError::IoFailure, offset);
  }
  *bytesRead = static_cast<size_t>(result);
  return PdfStatus::success();
}

PdfStatus writeFile(void* rawContext, const PdfCacheHandle handle, const uint8_t* const source, const size_t requested,
                    size_t* const bytesWritten) {
  if (rawContext == nullptr || source == nullptr || bytesWritten == nullptr) {
    return PdfStatus::failure(PdfError::InvalidArgument);
  }
  auto& context = *static_cast<PdfHalCacheIoContext*>(rawContext);
  *bytesWritten = 0;
  if (!validHandle(context, handle)) {
    return invalidHandle();
  }
  *bytesWritten = context.files[handle.value].write(source, requested);
  return PdfStatus::success();
}

PdfStatus flushFile(void* rawContext, const PdfCacheHandle handle) {
  if (rawContext == nullptr) {
    return invalidHandle();
  }
  auto& context = *static_cast<PdfHalCacheIoContext*>(rawContext);
  if (!validHandle(context, handle)) {
    return invalidHandle();
  }
  // HalFile::flush has no result on either SdFat or the simulator. The
  // following sync call is the observable durability gate.
  context.files[handle.value].flush();
  return PdfStatus::success();
}

PdfStatus syncFile(void* rawContext, const PdfCacheHandle handle) {
  if (rawContext == nullptr) {
    return invalidHandle();
  }
  auto& context = *static_cast<PdfHalCacheIoContext*>(rawContext);
  if (!validHandle(context, handle)) {
    return invalidHandle();
  }
  return context.files[handle.value].sync() ? PdfStatus::success() : PdfStatus::failure(PdfError::IoFailure);
}

PdfStatus closeFile(void* rawContext, PdfCacheHandle* const handle) {
  if (rawContext == nullptr || handle == nullptr || !handle->valid() || handle->value >= PDF_HAL_CACHE_HANDLE_COUNT) {
    return invalidHandle();
  }
  auto& context = *static_cast<PdfHalCacheIoContext*>(rawContext);
  const uint8_t slot = handle->value;
  if (!context.used[slot]) {
    *handle = PdfCacheHandle{};
    return invalidHandle();
  }
  const bool closed = context.files[slot].close();
  context.used[slot] = false;
  *handle = PdfCacheHandle{};
  return closed ? PdfStatus::success() : PdfStatus::failure(PdfError::IoFailure);
}

PdfStatus removePath(void* rawContext, const char* const path, const bool recursive) {
  if (rawContext == nullptr || path == nullptr) {
    return PdfStatus::failure(PdfError::InvalidArgument);
  }
  if (!Storage.exists(path)) {
    return PdfStatus::failure(PdfError::InvalidOffset);
  }
  const bool removed = recursive ? Storage.removeDir(path) : Storage.remove(path);
  return removed ? PdfStatus::success() : PdfStatus::failure(PdfError::IoFailure);
}

PdfStatus renamePath(void* rawContext, const char* const sourcePath, const char* const destinationPath) {
  if (rawContext == nullptr || sourcePath == nullptr || destinationPath == nullptr) {
    return PdfStatus::failure(PdfError::InvalidArgument);
  }
  if (!Storage.exists(sourcePath)) {
    return PdfStatus::failure(PdfError::InvalidOffset);
  }
  if (Storage.exists(destinationPath)) {
    return PdfStatus::failure(PdfError::IoFailure);
  }
  return Storage.rename(sourcePath, destinationPath) ? PdfStatus::success() : PdfStatus::failure(PdfError::IoFailure);
}

PdfStatus makeDirectory(void* rawContext, const char* const path) {
  if (rawContext == nullptr || path == nullptr) {
    return PdfStatus::failure(PdfError::InvalidArgument);
  }
  return (Storage.exists(path) || Storage.mkdir(path)) ? PdfStatus::success() : PdfStatus::failure(PdfError::IoFailure);
}

PdfStatus listDirectory(void* rawContext, const char* const path, const PdfCacheListVisitor visitor,
                        void* const visitorContext) {
  if (rawContext == nullptr || path == nullptr || visitor == nullptr) {
    return PdfStatus::failure(PdfError::InvalidArgument);
  }
  HalFile directory = Storage.open(path, O_RDONLY);
  if (!directory || !directory.isDirectory()) {
    directory.close();
    return PdfStatus::failure(PdfError::InvalidOffset);
  }
  PdfStatus status = PdfStatus::success();
  while (status) {
    HalFile entryFile = directory.openNextFile();
    if (!entryFile) {
      break;
    }
    PdfCacheDirEntry entry{};
    const size_t length = entryFile.getName(entry.name, sizeof(entry.name));
    if (length == 0 || length >= sizeof(entry.name)) {
      status = PdfStatus::failure(PdfError::LimitExceeded);
    } else {
      entry.nameLength = static_cast<uint8_t>(length);
      entry.directory = entryFile.isDirectory();
      entry.symlinkLike = false;
      status = visitor(visitorContext, entry);
    }
    const bool closed = entryFile.close();
    if (status && !closed) {
      status = PdfStatus::failure(PdfError::IoFailure);
    }
  }
  const bool closed = directory.close();
  if (status && !closed) {
    status = PdfStatus::failure(PdfError::IoFailure);
  }
  return status;
}

PdfStatus storageCapacity(void* rawContext, PdfCacheCapacity* const capacity) {
  if (rawContext == nullptr || capacity == nullptr) {
    return PdfStatus::failure(PdfError::InvalidArgument);
  }
  *capacity = {};
#ifdef HAL_STORAGE_HAS_CACHE_METADATA
  const HalStorageCapacityInfo halCapacity = Storage.capacityInfo();
  capacity->total = {halCapacity.total.known, halCapacity.total.value};
  capacity->free = {halCapacity.free.known, halCapacity.free.value};
#endif
  return PdfStatus::success();
}

PdfStatus fileMetadata(void* rawContext, const PdfCacheHandle handle, PdfCacheFileMetadata* const metadata) {
  if (rawContext == nullptr || metadata == nullptr) {
    return PdfStatus::failure(PdfError::InvalidArgument);
  }
  auto& context = *static_cast<PdfHalCacheIoContext*>(rawContext);
  if (!validHandle(context, handle)) {
    return invalidHandle();
  }
  HalFile& file = context.files[handle.value];
  if (metadata->operation == PdfCacheMetadataOperation::Seek) {
    const uint64_t offset = metadata->size;
    return file.seek64(offset) ? PdfStatus::success() : PdfStatus::failure(PdfError::IoFailure, offset);
  }
  if (metadata->operation == PdfCacheMetadataOperation::Truncate) {
    const uint64_t length = metadata->size;
#ifdef SIMULATOR
    // The external native-simulator HalFile contract does not expose its POSIX
    // descriptor. Keep this build explicit until that dependency adds truncate64.
    return PdfStatus::failure(PdfError::Unsupported, length);
#else
    return file.truncate64(length) ? PdfStatus::success() : PdfStatus::failure(PdfError::IoFailure, length);
#endif
  }
  *metadata = {};
  metadata->size = file.fileSize64();
  metadata->directory = file.isDirectory();
  metadata->symlinkLike = false;
#ifdef HAL_STORAGE_HAS_CACHE_METADATA
  uint64_t modificationTime = 0;
  if (file.modificationTime(&modificationTime)) {
    metadata->modificationTime = {true, modificationTime};
  }
#endif
  return PdfStatus::success();
}

}  // namespace

PdfStatus pdfHalCacheRename(void* context, const char* sourcePath, const char* destinationPath) {
  return renamePath(context, sourcePath, destinationPath);
}

PdfCacheIo pdfHalCacheIo(PdfHalCacheIoContext& context) {
  return {&context,  openFile,   readFile,      writeFile,     flushFile,       syncFile,
          closeFile, removePath, makeDirectory, listDirectory, storageCapacity, fileMetadata};
}
