#include "PdfImageCache.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <new>

namespace {

bool copyPath(const char* const source, char* const destination, const size_t capacity) {
  if (source == nullptr || destination == nullptr || capacity == 0) {
    return false;
  }
  const size_t length = std::strlen(source);
  if (length == 0 || length >= capacity) {
    destination[0] = '\0';
    return false;
  }
  std::memcpy(destination, source, length + 1);
  return true;
}

template <typename T>
void resetInPlace(T& value) {
  value.~T();
  new (&value) T();
}

}  // namespace

PdfStatus PdfImageCache::begin(const PdfImageCacheConfig& config) {
  if (writer_.open || writer_.fullPath[0] != '\0') {
    pdfAbortTrackedCacheFile(&writer_);
  }
  cacheRoot_[0] = '\0';
  relativePath_[0] = '\0';
  resetInPlace(writer_);
  entryCount_ = 0;
  initialized_ = false;
  if (!config.io.valid() || config.rename == nullptr || config.cacheRoot == nullptr || config.generation == 0 ||
      config.budget == nullptr || config.ioWorkspace == nullptr || config.ioWorkspaceBytes == 0 ||
      config.ioWorkspaceBytes > PdfLimits::DecoderOutputBytes || config.entries == nullptr ||
      config.entryCapacity == 0 || config.entryCapacity > PDF_IMAGE_CACHE_MAX_ENTRIES ||
      !copyPath(config.cacheRoot, cacheRoot_, sizeof(cacheRoot_))) {
    return PdfStatus::failure(PdfError::InvalidArgument);
  }
  char imageDirectory[PDF_CACHE_PATH_CAPACITY]{};
  const int written = std::snprintf(imageDirectory, sizeof(imageDirectory), "%s/gen_%lu/images", cacheRoot_,
                                    static_cast<unsigned long>(config.generation));
  if (written <= 0 || static_cast<size_t>(written) >= sizeof(imageDirectory)) {
    return PdfStatus::failure(PdfError::LimitExceeded);
  }
  const PdfStatus status = config.io.mkdir(config.io.context, imageDirectory);
  if (!status.ok()) {
    return status;
  }
  config_ = config;
  initialized_ = true;
  return PdfStatus::success();
}

PdfStatus PdfImageCache::formatTemporaryPaths(char* const fullPath, const size_t fullCapacity, char* const relativePath,
                                              const size_t relativeCapacity) const {
  if (!initialized_ || fullPath == nullptr || relativePath == nullptr) {
    return PdfStatus::failure(PdfError::InvalidArgument);
  }
  const int relativeWritten = std::snprintf(relativePath, relativeCapacity, "gen_%lu/images/build-jpeg.tmp",
                                            static_cast<unsigned long>(config_.generation));
  const int fullWritten = std::snprintf(fullPath, fullCapacity, "%s/%s", cacheRoot_, relativePath);
  if (relativeWritten <= 0 || static_cast<size_t>(relativeWritten) >= relativeCapacity || fullWritten <= 0 ||
      static_cast<size_t>(fullWritten) >= fullCapacity) {
    return PdfStatus::failure(PdfError::LimitExceeded);
  }
  return PdfStatus::success();
}

PdfStatus PdfImageCache::formatCapturedTemporaryPaths(const uint8_t temporaryOrdinal, char* const fullPath,
                                                      const size_t fullCapacity, char* const relativePath,
                                                      const size_t relativeCapacity) const {
  if (!initialized_ || temporaryOrdinal >= config_.entryCapacity || fullPath == nullptr || relativePath == nullptr) {
    return PdfStatus::failure(PdfError::InvalidArgument);
  }
  const int relativeWritten =
      std::snprintf(relativePath, relativeCapacity, "gen_%lu/images/build-inline-%02u.tmp",
                    static_cast<unsigned long>(config_.generation), static_cast<unsigned>(temporaryOrdinal));
  const int fullWritten = std::snprintf(fullPath, fullCapacity, "%s/%s", cacheRoot_, relativePath);
  if (relativeWritten <= 0 || static_cast<size_t>(relativeWritten) >= relativeCapacity || fullWritten <= 0 ||
      static_cast<size_t>(fullWritten) >= fullCapacity) {
    return PdfStatus::failure(PdfError::LimitExceeded);
  }
  return PdfStatus::success();
}

PdfStatus PdfImageCache::formatContentPaths(const uint64_t contentHash, const uint32_t sourceCrc32,
                                            const uint64_t sourceBytes, char* const fullPath, const size_t fullCapacity,
                                            char* const relativePath, const size_t relativeCapacity) const {
  if (!initialized_ || fullPath == nullptr || relativePath == nullptr || sourceBytes == 0) {
    return PdfStatus::failure(PdfError::InvalidArgument);
  }
  const int relativeWritten =
      std::snprintf(relativePath, relativeCapacity, "gen_%lu/images/%016llx-%08lx-%016llx.jpg",
                    static_cast<unsigned long>(config_.generation), static_cast<unsigned long long>(contentHash),
                    static_cast<unsigned long>(sourceCrc32), static_cast<unsigned long long>(sourceBytes));
  const int fullWritten = std::snprintf(fullPath, fullCapacity, "%s/%s", cacheRoot_, relativePath);
  if (relativeWritten <= 0 || static_cast<size_t>(relativeWritten) >= relativeCapacity || fullWritten <= 0 ||
      static_cast<size_t>(fullWritten) >= fullCapacity) {
    return PdfStatus::failure(PdfError::LimitExceeded);
  }
  return PdfStatus::success();
}

PdfStatus PdfImageCache::beginJpeg(const PdfByteSource& encoded, const uint16_t width, const uint16_t height,
                                   PdfCachedImage* const result, PdfImageCacheRuntime* const runtime) {
  if (!initialized_ || runtime == nullptr || runtime->stage != PdfImageCacheJpegStage::Idle || writer_.open ||
      writer_.fullPath[0] != '\0' || !encoded.valid() || encoded.size == 0 || width == 0 || height == 0 ||
      result == nullptr) {
    return PdfStatus::failure(PdfError::InvalidArgument);
  }
  resetInPlace(*result);
  PdfStatus status =
      formatTemporaryPaths(result->fullPath, sizeof(result->fullPath), relativePath_, sizeof(relativePath_));
  if (!status.ok()) {
    return status;
  }
  status = pdfOpenTrackedCacheWriter(config_.io, result->fullPath, relativePath_, PdfCacheFileKind::Optional,
                                     encoded.size, &writer_);
  if (!status.ok()) {
    return status;
  }

  resetInPlace(*runtime);
  runtime->source = encoded;
  runtime->result = result;
  runtime->width = width;
  runtime->height = height;
  runtime->stage = PdfImageCacheJpegStage::Copy;
  return PdfStatus::success();
}

PdfStatus PdfImageCache::beginJpegCapture(const uint8_t temporaryOrdinal, const uint64_t byteLimit,
                                          PdfImageCacheRuntime* const runtime) {
  if (!initialized_ || runtime == nullptr || runtime->stage != PdfImageCacheJpegStage::Idle || writer_.open ||
      writer_.fullPath[0] != '\0' || byteLimit == 0 || temporaryOrdinal >= config_.entryCapacity) {
    return PdfStatus::failure(PdfError::InvalidArgument);
  }
  char fullPath[PDF_CACHE_PATH_CAPACITY]{};
  PdfStatus status = formatCapturedTemporaryPaths(temporaryOrdinal, fullPath, sizeof(fullPath), relativePath_,
                                                  sizeof(relativePath_));
  if (!status.ok()) {
    return status;
  }
  status = pdfOpenTrackedCacheWriter(config_.io, fullPath, relativePath_, PdfCacheFileKind::Optional, byteLimit,
                                     &writer_);
  if (!status.ok()) {
    return status;
  }
  resetInPlace(*runtime);
  runtime->temporaryOrdinal = temporaryOrdinal;
  runtime->capturedSource = true;
  runtime->stage = PdfImageCacheJpegStage::Capture;
  return PdfStatus::success();
}

PdfStatus PdfImageCache::appendJpegCapture(const uint8_t* const bytes, const size_t length,
                                           PdfImageCacheRuntime& runtime) {
  if (!initialized_ || runtime.stage != PdfImageCacheJpegStage::Capture || !runtime.capturedSource ||
      runtime.temporaryOrdinal >= config_.entryCapacity || bytes == nullptr || length == 0) {
    return PdfStatus::failure(PdfError::InvalidArgument);
  }
  const PdfStatus status = pdfWriteTrackedCacheFile(&writer_, bytes, length);
  if (!status.ok()) {
    return failJpeg(status, runtime);
  }
  runtime.hash = pdfCacheFnv64(bytes, length, runtime.hash);
  runtime.crc32 = pdfCacheCrc32(bytes, length, runtime.crc32);
  runtime.bytes += length;
  return PdfStatus::success();
}

PdfStatus PdfImageCache::finishJpegCapture(PdfImageCacheRuntime& runtime, PdfCapturedJpeg* const captured) {
  if (!initialized_ || runtime.stage != PdfImageCacheJpegStage::Capture || !runtime.capturedSource ||
      runtime.temporaryOrdinal >= config_.entryCapacity || runtime.bytes == 0 || captured == nullptr) {
    return PdfStatus::failure(PdfError::InvalidArgument);
  }
  PdfRequiredFileRecord temporaryRecord{};
  PdfStatus status = pdfCloseTrackedCacheFile(&writer_, &temporaryRecord);
  if (!status.ok()) {
    return failJpeg(status, runtime);
  }
  if (temporaryRecord.size != runtime.bytes || temporaryRecord.crc32 != runtime.crc32) {
    return failJpeg(PdfStatus::failure(PdfError::Malformed), runtime);
  }
  *captured = {runtime.hash, runtime.bytes, runtime.crc32, runtime.temporaryOrdinal};
  resetInPlace(writer_);
  resetInPlace(runtime);
  return PdfStatus::success();
}

PdfStatus PdfImageCache::beginCapturedJpeg(const PdfCapturedJpeg& captured, const uint16_t width,
                                           const uint16_t height, PdfCachedImage* const result,
                                           PdfImageCacheRuntime* const runtime) {
  if (!initialized_ || runtime == nullptr || runtime->stage != PdfImageCacheJpegStage::Idle || writer_.open ||
      writer_.fullPath[0] != '\0' || captured.sourceBytes == 0 ||
      captured.temporaryOrdinal >= config_.entryCapacity || width == 0 || height == 0 || result == nullptr) {
    return PdfStatus::failure(PdfError::InvalidArgument);
  }
  resetInPlace(*result);
  PdfStatus status =
      formatCapturedTemporaryPaths(captured.temporaryOrdinal, result->fullPath, sizeof(result->fullPath),
                                   relativePath_, sizeof(relativePath_));
  if (!status.ok()) {
    return status;
  }
  resetInPlace(writer_);
  if (!copyPath(result->fullPath, writer_.fullPath, sizeof(writer_.fullPath)) ||
      !copyPath(relativePath_, writer_.record.path, sizeof(writer_.record.path))) {
    resetInPlace(writer_);
    return PdfStatus::failure(PdfError::LimitExceeded);
  }
  writer_.io = config_.io;
  writer_.record.pathLength = static_cast<uint8_t>(std::strlen(writer_.record.path));
  writer_.record.size = captured.sourceBytes;
  writer_.record.crc32 = captured.sourceCrc32;
  writer_.byteLimit = captured.sourceBytes;
  writer_.kind = PdfCacheFileKind::Optional;
  result->record.size = captured.sourceBytes;
  result->record.crc32 = captured.sourceCrc32;

  resetInPlace(*runtime);
  runtime->result = result;
  runtime->hash = captured.contentHash;
  runtime->bytes = captured.sourceBytes;
  runtime->crc32 = captured.sourceCrc32;
  runtime->width = width;
  runtime->height = height;
  runtime->temporaryOrdinal = captured.temporaryOrdinal;
  runtime->capturedSource = true;
  runtime->stage = PdfImageCacheJpegStage::Dedupe;
  return PdfStatus::success();
}

PdfStatus PdfImageCache::discardCapturedJpeg(const uint8_t temporaryOrdinal) {
  if (!initialized_ || writer_.open || writer_.fullPath[0] != '\0' ||
      temporaryOrdinal >= config_.entryCapacity) {
    return PdfStatus::failure(PdfError::InvalidArgument);
  }
  char fullPath[PDF_CACHE_PATH_CAPACITY]{};
  char relativePath[PDF_CACHE_REQUIRED_PATH_CAPACITY]{};
  const PdfStatus status = formatCapturedTemporaryPaths(temporaryOrdinal, fullPath, sizeof(fullPath), relativePath,
                                                        sizeof(relativePath));
  if (!status.ok()) {
    return status;
  }
  return config_.io.remove(config_.io.context, fullPath, false);
}

PdfStepResult PdfImageCache::stepJpeg(PdfImageCacheRuntime& runtime, PdfWorkBudget& budget) {
  if (!initialized_ || runtime.stage == PdfImageCacheJpegStage::Idle ||
      runtime.stage == PdfImageCacheJpegStage::Capture || runtime.result == nullptr) {
    return PdfStepResult::failure(PdfStatus::failure(PdfError::InvalidArgument));
  }

  if (runtime.stage == PdfImageCacheJpegStage::Copy) {
    if (runtime.pendingBytes == 0 && runtime.offset < runtime.source.size) {
      if (!budget.consumeOperation()) {
        return PdfStepResult::paused();
      }
      const size_t requested = budget.takeBytes(
          static_cast<size_t>(std::min<uint64_t>(config_.ioWorkspaceBytes, runtime.source.size - runtime.offset)));
      if (requested == 0) {
        return PdfStepResult::paused();
      }
      size_t bytesRead = 0;
      const PdfStatus status =
          runtime.source.readAt(runtime.source.context, runtime.offset, config_.ioWorkspace, requested, &bytesRead);
      if (!status.ok() || bytesRead == 0 || bytesRead > requested) {
        return PdfStepResult::failure(failJpeg(
            status.ok() ? PdfStatus::failure(bytesRead > requested ? PdfError::Malformed : PdfError::UnexpectedEof,
                                             runtime.offset)
                        : status,
            runtime));
      }
      runtime.hash = pdfCacheFnv64(config_.ioWorkspace, bytesRead, runtime.hash);
      runtime.crc32 = pdfCacheCrc32(config_.ioWorkspace, bytesRead, runtime.crc32);
      runtime.bytes += bytesRead;
      runtime.pendingBytes = bytesRead;
      return PdfStepResult::paused();
    }

    if (runtime.pendingBytes != 0) {
      if (!budget.consumeOperation()) {
        return PdfStepResult::paused();
      }
      const PdfStatus status = pdfWriteTrackedCacheFile(&writer_, config_.ioWorkspace, runtime.pendingBytes);
      if (!status.ok()) {
        return PdfStepResult::failure(failJpeg(status, runtime));
      }
      runtime.offset += runtime.pendingBytes;
      runtime.pendingBytes = 0;
      return PdfStepResult::paused();
    }

    runtime.stage = PdfImageCacheJpegStage::Close;
    return PdfStepResult::paused();
  }

  if (runtime.stage == PdfImageCacheJpegStage::Close) {
    PdfStatus status = pdfCloseTrackedCacheFile(&writer_, &runtime.result->record);
    if (!status.ok()) {
      return PdfStepResult::failure(failJpeg(status, runtime));
    }
    if (runtime.result->record.size != runtime.bytes || runtime.result->record.crc32 != runtime.crc32) {
      return PdfStepResult::failure(failJpeg(PdfStatus::failure(PdfError::Malformed), runtime));
    }
    runtime.dedupeIndex = 0;
    runtime.stage = PdfImageCacheJpegStage::Dedupe;
    return PdfStepResult::paused();
  }

  if (runtime.stage == PdfImageCacheJpegStage::Dedupe) {
    while (runtime.dedupeIndex < entryCount_) {
      if (!budget.consumeOperation()) {
        return PdfStepResult::paused();
      }
      const PdfImageCacheEntry& entry = config_.entries[runtime.dedupeIndex++];
      if (entry.kind != PdfImageCacheEntryKind::Jpeg || entry.contentHash != runtime.hash ||
          entry.sourceBytes != runtime.bytes || entry.sourceCrc32 != runtime.crc32) {
        continue;
      }
      PdfStatus status = config_.io.remove(config_.io.context, runtime.result->fullPath, false);
      if (!status.ok()) {
        return PdfStepResult::failure(failJpeg(status, runtime));
      }
      resetInPlace(writer_);
      resetInPlace(*runtime.result);
      status = formatContentPaths(entry.contentHash, entry.sourceCrc32, entry.sourceBytes, runtime.result->fullPath,
                                  sizeof(runtime.result->fullPath), runtime.result->record.path,
                                  sizeof(runtime.result->record.path));
      if (!status.ok()) {
        return PdfStepResult::failure(failJpeg(status, runtime));
      }
      runtime.result->record.pathLength = static_cast<uint8_t>(std::strlen(runtime.result->record.path));
      runtime.result->record.size = entry.sourceBytes;
      runtime.result->record.crc32 = entry.sourceCrc32;
      runtime.result->contentHash = entry.contentHash;
      runtime.result->width = runtime.width;
      runtime.result->height = runtime.height;
      runtime.result->reused = true;
      finishJpeg(runtime);
      return PdfStepResult::completed();
    }
    runtime.stage = PdfImageCacheJpegStage::Publish;
    return PdfStepResult::paused();
  }

  if (runtime.stage == PdfImageCacheJpegStage::Publish) {
    if (entryCount_ >= config_.entryCapacity) {
      return PdfStepResult::failure(failJpeg(PdfStatus::failure(PdfError::LimitExceeded, entryCount_), runtime));
    }
    PdfStatus status = formatContentPaths(runtime.hash, runtime.crc32, runtime.bytes, runtime.result->fullPath,
                                          sizeof(runtime.result->fullPath), runtime.result->record.path,
                                          sizeof(runtime.result->record.path));
    if (!status.ok()) {
      return PdfStepResult::failure(failJpeg(status, runtime));
    }
    runtime.result->record.pathLength = static_cast<uint8_t>(std::strlen(runtime.result->record.path));
    if (!budget.consumeOperation()) {
      return PdfStepResult::paused();
    }
    status = config_.rename(config_.io.context, writer_.fullPath, runtime.result->fullPath);
    if (!status.ok()) {
      return PdfStepResult::failure(failJpeg(status, runtime));
    }
    status = pdfReserveCacheBytes(config_.budget, runtime.result->record.size, PdfCacheFileKind::Optional);
    if (!status.ok()) {
      (void)config_.io.remove(config_.io.context, runtime.result->fullPath, false);
      return PdfStepResult::failure(failJpeg(status, runtime));
    }
    config_.entries[entryCount_] = {runtime.hash, runtime.bytes, runtime.crc32, UINT8_MAX,
                                    PdfImageCacheEntryKind::Jpeg};
    ++entryCount_;
    runtime.result->contentHash = runtime.hash;
    runtime.result->width = runtime.width;
    runtime.result->height = runtime.height;
    finishJpeg(runtime);
    return PdfStepResult::completed();
  }

  return PdfStepResult::failure(failJpeg(PdfStatus::failure(PdfError::InvalidArgument), runtime));
}

PdfStatus PdfImageCache::failJpeg(const PdfStatus status, PdfImageCacheRuntime& runtime) {
  abortJpeg(runtime);
  return status.ok() ? PdfStatus::failure(PdfError::Malformed) : status;
}

void PdfImageCache::finishJpeg(PdfImageCacheRuntime& runtime) {
  resetInPlace(writer_);
  resetInPlace(runtime);
}

void PdfImageCache::abortJpeg(PdfImageCacheRuntime& runtime) {
  if (writer_.open || writer_.fullPath[0] != '\0') {
    pdfAbortTrackedCacheFile(&writer_);
  }
  finishJpeg(runtime);
}

PdfStepResult PdfImageCache::stepRasterIdentity(const uint64_t contentHash, const uint32_t sourceCrc32,
                                                const uint64_t sourceBytes, uint8_t* const scanIndex,
                                                uint8_t* const identityIndex, PdfWorkBudget& budget) {
  if (!initialized_ || sourceBytes == 0 || scanIndex == nullptr || identityIndex == nullptr ||
      *scanIndex > entryCount_) {
    return PdfStepResult::failure(PdfStatus::failure(PdfError::InvalidArgument));
  }

  while (*scanIndex < entryCount_) {
    if (!budget.consumeOperation()) {
      return PdfStepResult::paused();
    }
    const uint8_t index = (*scanIndex)++;
    const PdfImageCacheEntry& entry = config_.entries[index];
    if (entry.kind == PdfImageCacheEntryKind::Raster && entry.contentHash == contentHash &&
        entry.sourceBytes == sourceBytes && entry.sourceCrc32 == sourceCrc32) {
      *identityIndex = index;
      return PdfStepResult::completed();
    }
  }

  if (!budget.consumeOperation()) {
    return PdfStepResult::paused();
  }
  if (entryCount_ >= config_.entryCapacity) {
    return PdfStepResult::failure(PdfStatus::failure(PdfError::LimitExceeded, entryCount_));
  }
  config_.entries[entryCount_] = {contentHash, sourceBytes, sourceCrc32, UINT8_MAX, PdfImageCacheEntryKind::Raster};
  *identityIndex = entryCount_++;
  return PdfStepResult::completed();
}

PdfStatus PdfImageCache::bindRasterRecord(const uint8_t identityIndex, const uint8_t recordIndex,
                                          uint8_t* const canonicalRecordIndex) {
  if (!initialized_ || identityIndex >= entryCount_ || recordIndex >= PDF_IMAGE_CACHE_MAX_ENTRIES ||
      canonicalRecordIndex == nullptr) {
    return PdfStatus::failure(PdfError::InvalidArgument, identityIndex);
  }
  PdfImageCacheEntry& entry = config_.entries[identityIndex];
  if (entry.kind != PdfImageCacheEntryKind::Raster) {
    return PdfStatus::failure(PdfError::Malformed, identityIndex);
  }
  if (entry.fileOrdinal == UINT8_MAX) {
    entry.fileOrdinal = recordIndex;
  } else if (entry.fileOrdinal > recordIndex) {
    return PdfStatus::failure(PdfError::Malformed, identityIndex);
  }
  *canonicalRecordIndex = entry.fileOrdinal;
  return PdfStatus::success();
}
