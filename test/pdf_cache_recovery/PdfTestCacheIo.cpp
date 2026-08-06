#include "PdfTestCacheIo.h"

#include <algorithm>
#include <cstring>
#include <limits>

#include "PdfCacheFormat.h"

namespace {

constexpr uint8_t kInvalidHandle = 0xff;

void writeLe32(uint8_t* const destination, const uint32_t value) {
  destination[0] = static_cast<uint8_t>(value);
  destination[1] = static_cast<uint8_t>(value >> 8U);
  destination[2] = static_cast<uint8_t>(value >> 16U);
  destination[3] = static_cast<uint8_t>(value >> 24U);
}

bool isDirectChild(const std::string& parent, const std::string& candidate, std::string* name) {
  std::string prefix = parent;
  if (prefix.empty() || prefix.back() != '/') {
    prefix.push_back('/');
  }
  if (candidate.size() <= prefix.size() || candidate.compare(0, prefix.size(), prefix) != 0) {
    return false;
  }
  const std::string remainder = candidate.substr(prefix.size());
  if (remainder.find('/') != std::string::npos) {
    return false;
  }
  *name = remainder;
  return true;
}

}  // namespace

PdfTestCacheIo::PdfTestCacheIo() {
  capacity_.total = {true, 64ULL * 1024ULL * 1024ULL};
  capacity_.free = {true, 32ULL * 1024ULL * 1024ULL};
}

PdfCacheIo PdfTestCacheIo::io() {
  return {this,       openThunk,   readThunk,  writeThunk, flushThunk,    syncThunk,
          closeThunk, removeThunk, mkdirThunk, listThunk,  capacityThunk, metadataThunk};
}

void PdfTestCacheIo::addDirectory(const std::string& path, const bool symlinkLike) {
  Node& node = nodes_[path];
  node.directory = true;
  node.symlinkLike = symlinkLike;
}

void PdfTestCacheIo::addFile(const std::string& path, const std::vector<uint8_t>& bytes,
                             const uint64_t modificationTime, const bool modificationTimeKnown) {
  Node& node = nodes_[path];
  node.bytes = bytes;
  node.modificationTime = modificationTime;
  node.modificationTimeKnown = modificationTimeKnown;
  node.directory = false;
  node.symlinkLike = false;
}

void PdfTestCacheIo::addFile(const std::string& path, const std::string& bytes) {
  addFile(path, std::vector<uint8_t>(bytes.begin(), bytes.end()));
}

void PdfTestCacheIo::truncateFile(const std::string& path, const size_t length) {
  auto found = nodes_.find(path);
  if (found != nodes_.end() && !found->second.directory && length < found->second.bytes.size()) {
    found->second.bytes.resize(length);
  }
}

void PdfTestCacheIo::corruptByte(const std::string& path, const size_t offset, const uint8_t mask) {
  auto found = nodes_.find(path);
  if (found != nodes_.end() && offset < found->second.bytes.size()) {
    found->second.bytes[offset] ^= mask;
  }
}

void PdfTestCacheIo::mutateByteBeforeNextRead(const std::string& path, const size_t offset, const uint8_t mask) {
  pendingReadMutationPath_ = path;
  pendingReadMutationOffset_ = offset;
  pendingReadMutationMask_ = mask;
  pendingReadMutation_ = true;
}

bool PdfTestCacheIo::exists(const std::string& path) const { return nodes_.find(path) != nodes_.end(); }

bool PdfTestCacheIo::isDirectory(const std::string& path) const {
  const auto found = nodes_.find(path);
  return found != nodes_.end() && found->second.directory;
}

const std::vector<uint8_t>& PdfTestCacheIo::bytes(const std::string& path) const { return nodes_.at(path).bytes; }

std::vector<std::string> PdfTestCacheIo::paths() const {
  std::vector<std::string> result;
  result.reserve(nodes_.size());
  for (const auto& [path, node] : nodes_) {
    (void)node;
    result.push_back(path);
  }
  return result;
}

void PdfTestCacheIo::setCapacity(const uint64_t total, const uint64_t free, const bool known) {
  capacity_.total = {known, total};
  capacity_.free = {known, free};
}

void PdfTestCacheIo::fail(const PdfTestFaultPoint point, const uint32_t occurrence) {
  faultPoint_ = point;
  faultOccurrence_ = occurrence;
  faultSeen_ = 0;
}

void PdfTestCacheIo::clearFault() {
  faultPoint_ = PdfTestFaultPoint::None;
  faultOccurrence_ = 0;
  faultSeen_ = 0;
}

void PdfTestCacheIo::setWriteAllowance(const uint64_t bytes) { writeAllowance_ = bytes; }

void PdfTestCacheIo::clearWriteAllowance() { writeAllowance_ = UINT64_MAX; }

void PdfTestCacheIo::setMaximumReadHandles(const uint8_t maximum) { maximumReadHandles_ = maximum; }

uint32_t PdfTestCacheIo::openCalls() const { return openCalls_; }
uint32_t PdfTestCacheIo::readCalls() const { return readCalls_; }
uint32_t PdfTestCacheIo::writeCalls() const { return writeCalls_; }
uint32_t PdfTestCacheIo::flushCalls() const { return flushCalls_; }
uint32_t PdfTestCacheIo::syncCalls() const { return syncCalls_; }
uint32_t PdfTestCacheIo::closeCalls() const { return closeCalls_; }
uint32_t PdfTestCacheIo::removeCalls() const { return removeCalls_; }
uint32_t PdfTestCacheIo::renameCalls() const { return renameCalls_; }
uint32_t PdfTestCacheIo::mkdirCalls() const { return mkdirCalls_; }
uint32_t PdfTestCacheIo::listCalls() const { return listCalls_; }
uint32_t PdfTestCacheIo::capacityCalls() const { return capacityCalls_; }
uint32_t PdfTestCacheIo::metadataCalls() const { return metadataCalls_; }
uint32_t PdfTestCacheIo::readMutationsApplied() const { return readMutationsApplied_; }
uint32_t PdfTestCacheIo::operationCalls() const {
  return openCalls_ + readCalls_ + writeCalls_ + flushCalls_ + syncCalls_ + closeCalls_ + removeCalls_ + mkdirCalls_ +
         listCalls_ + capacityCalls_ + metadataCalls_ + renameCalls_;
}
uint64_t PdfTestCacheIo::bytesReadTotal() const { return bytesReadTotal_; }
uint64_t PdfTestCacheIo::bytesWrittenTotal() const { return bytesWrittenTotal_; }
size_t PdfTestCacheIo::maximumReadRequest() const { return maximumReadRequest_; }
size_t PdfTestCacheIo::maximumWriteRequest() const { return maximumWriteRequest_; }
uint32_t PdfTestCacheIo::openCallsForPath(const std::string& path) const {
  const auto found = pathOpenCalls_.find(path);
  return found == pathOpenCalls_.end() ? 0 : found->second;
}
const std::vector<std::string>& PdfTestCacheIo::syncObservations() const { return syncObservations_; }
void PdfTestCacheIo::clearSyncObservations() { syncObservations_.clear(); }
const std::vector<PdfTestReadObservation>& PdfTestCacheIo::readObservations() const { return readObservations_; }
void PdfTestCacheIo::clearReadObservations() { readObservations_.clear(); }
const std::vector<PdfTestOpenObservation>& PdfTestCacheIo::openObservations() const { return openObservations_; }
void PdfTestCacheIo::clearOpenObservations() { openObservations_.clear(); }
const std::vector<std::string>& PdfTestCacheIo::removeObservations() const { return removeObservations_; }
void PdfTestCacheIo::clearRemoveObservations() { removeObservations_.clear(); }
const std::vector<std::string>& PdfTestCacheIo::events() const { return events_; }
void PdfTestCacheIo::clearEvents() { events_.clear(); }

uint32_t PdfTestCacheIo::openHandleCount() const {
  uint32_t count = 0;
  for (const auto& handle : handles_) {
    if (handle.open) {
      ++count;
    }
  }
  return count;
}

std::vector<std::string> PdfTestCacheIo::openHandlePaths() const {
  std::vector<std::string> paths;
  for (const auto& handle : handles_) {
    if (handle.open) {
      paths.push_back(handle.path);
    }
  }
  return paths;
}

PdfStatus PdfTestCacheIo::openThunk(void* context, const char* path, const PdfCacheOpenMode mode,
                                    PdfCacheHandle* handle) {
  return static_cast<PdfTestCacheIo*>(context)->open(path, mode, handle);
}

PdfStatus PdfTestCacheIo::readThunk(void* context, const PdfCacheHandle handle, const uint64_t offset,
                                    uint8_t* destination, const size_t requested, size_t* bytesRead) {
  return static_cast<PdfTestCacheIo*>(context)->read(handle, offset, destination, requested, bytesRead);
}

PdfStatus PdfTestCacheIo::writeThunk(void* context, const PdfCacheHandle handle, const uint8_t* source,
                                     const size_t requested, size_t* bytesWritten) {
  return static_cast<PdfTestCacheIo*>(context)->write(handle, source, requested, bytesWritten);
}

PdfStatus PdfTestCacheIo::flushThunk(void* context, const PdfCacheHandle handle) {
  return static_cast<PdfTestCacheIo*>(context)->flush(handle);
}

PdfStatus PdfTestCacheIo::syncThunk(void* context, const PdfCacheHandle handle) {
  return static_cast<PdfTestCacheIo*>(context)->sync(handle);
}

PdfStatus PdfTestCacheIo::closeThunk(void* context, PdfCacheHandle* handle) {
  return static_cast<PdfTestCacheIo*>(context)->close(handle);
}

PdfStatus PdfTestCacheIo::removeThunk(void* context, const char* path, const bool recursive) {
  return static_cast<PdfTestCacheIo*>(context)->remove(path, recursive);
}

PdfStatus PdfTestCacheIo::renameThunk(void* context, const char* sourcePath, const char* destinationPath) {
  return static_cast<PdfTestCacheIo*>(context)->rename(sourcePath, destinationPath);
}

PdfStatus PdfTestCacheIo::mkdirThunk(void* context, const char* path) {
  return static_cast<PdfTestCacheIo*>(context)->mkdir(path);
}

PdfStatus PdfTestCacheIo::listThunk(void* context, const char* path, const PdfCacheListVisitor visitor,
                                    void* visitorContext) {
  return static_cast<PdfTestCacheIo*>(context)->list(path, visitor, visitorContext);
}

PdfStatus PdfTestCacheIo::capacityThunk(void* context, PdfCacheCapacity* capacity) {
  return static_cast<PdfTestCacheIo*>(context)->capacity(capacity);
}

PdfStatus PdfTestCacheIo::metadataThunk(void* context, const PdfCacheHandle handle, PdfCacheFileMetadata* metadata) {
  return static_cast<PdfTestCacheIo*>(context)->metadata(handle, metadata);
}

bool PdfTestCacheIo::shouldFail(const PdfTestFaultPoint point) {
  if (faultPoint_ != point) {
    return false;
  }
  ++faultSeen_;
  return faultSeen_ == faultOccurrence_;
}

PdfStatus PdfTestCacheIo::open(const char* path, const PdfCacheOpenMode mode, PdfCacheHandle* handle) {
  ++openCalls_;
  if (path == nullptr || handle == nullptr) {
    return PdfStatus::failure(PdfError::InvalidArgument);
  }
  *handle = {};
  if (shouldFail(PdfTestFaultPoint::Open)) {
    return PdfStatus::failure(PdfError::IoFailure);
  }
  const std::string key(path);
  ++pathOpenCalls_[key];
  openObservations_.push_back({key, mode});
  events_.push_back("open:" + key);
  auto found = nodes_.find(key);
  const bool readCapable = mode == PdfCacheOpenMode::Read || mode == PdfCacheOpenMode::ReadWrite;
  if (readCapable) {
    if (pendingReadMutation_ && key == pendingReadMutationPath_ && found != nodes_.end() &&
        found->second.bytes.size() >= sizeof(uint32_t) &&
        pendingReadMutationOffset_ < found->second.bytes.size() - sizeof(uint32_t)) {
      auto& bytes = found->second.bytes;
      bytes[pendingReadMutationOffset_] ^= pendingReadMutationMask_;
      writeLe32(bytes.data() + bytes.size() - sizeof(uint32_t),
                pdfCacheCrc32(bytes.data(), bytes.size() - sizeof(uint32_t)));
      pendingReadMutation_ = false;
      ++readMutationsApplied_;
    }
    if (found == nodes_.end() || found->second.directory) {
      return PdfStatus::failure(PdfError::InvalidOffset);
    }
    uint8_t readHandles = 0;
    for (const OpenHandle& candidate : handles_) {
      if (candidate.open && candidate.readable) {
        ++readHandles;
      }
    }
    if (readHandles >= maximumReadHandles_) {
      return PdfStatus::failure(PdfError::LimitExceeded);
    }
  } else if (mode == PdfCacheOpenMode::WriteTruncate) {
    Node& node = nodes_[key];
    node.bytes.clear();
    node.directory = false;
    node.symlinkLike = false;
  } else {
    Node& node = nodes_[key];
    if (node.directory) {
      return PdfStatus::failure(PdfError::InvalidOffset);
    }
    node.directory = false;
    node.symlinkLike = false;
  }
  for (uint8_t index = 0; index < 8; ++index) {
    if (!handles_[index].open) {
      handles_[index] = {key, 0, readCapable, mode != PdfCacheOpenMode::Read, true};
      handle->value = index;
      return PdfStatus::success();
    }
  }
  return PdfStatus::failure(PdfError::LimitExceeded);
}

PdfStatus PdfTestCacheIo::read(const PdfCacheHandle handle, const uint64_t offset, uint8_t* destination,
                               const size_t requested, size_t* bytesRead) {
  ++readCalls_;
  maximumReadRequest_ = std::max(maximumReadRequest_, requested);
  if (!handle.valid() || handle.value >= 8 || !handles_[handle.value].open ||
      !handles_[handle.value].readable || destination == nullptr ||
      bytesRead == nullptr) {
    return PdfStatus::failure(PdfError::InvalidArgument, offset);
  }
  *bytesRead = 0;
  if (shouldFail(PdfTestFaultPoint::Read)) {
    return PdfStatus::failure(PdfError::IoFailure, offset);
  }
  const auto found = nodes_.find(handles_[handle.value].path);
  if (found == nodes_.end() || found->second.directory || offset > found->second.bytes.size()) {
    return PdfStatus::failure(PdfError::InvalidOffset, offset);
  }
  const size_t available = found->second.bytes.size() - static_cast<size_t>(offset);
  *bytesRead = std::min(requested, available);
  if (*bytesRead != 0) {
    std::memcpy(destination, found->second.bytes.data() + static_cast<size_t>(offset), *bytesRead);
  }
  readObservations_.push_back({handles_[handle.value].path, offset, requested, *bytesRead});
  bytesReadTotal_ += *bytesRead;
  return PdfStatus::success();
}

PdfStatus PdfTestCacheIo::write(const PdfCacheHandle handle, const uint8_t* source, const size_t requested,
                                size_t* bytesWritten) {
  ++writeCalls_;
  maximumWriteRequest_ = std::max(maximumWriteRequest_, requested);
  if (!handle.valid() || handle.value >= 8 || !handles_[handle.value].open || !handles_[handle.value].writable ||
      source == nullptr || bytesWritten == nullptr) {
    return PdfStatus::failure(PdfError::InvalidArgument);
  }
  *bytesWritten = 0;
  if (shouldFail(PdfTestFaultPoint::Write)) {
    return PdfStatus::failure(PdfError::IoFailure);
  }
  const size_t allowed = static_cast<size_t>(std::min<uint64_t>(static_cast<uint64_t>(requested), writeAllowance_));
  OpenHandle& openHandle = handles_[handle.value];
  Node& node = nodes_[openHandle.path];
  if (openHandle.position > node.bytes.size()) {
    return PdfStatus::failure(PdfError::InvalidOffset, openHandle.position);
  }
  if (openHandle.position + allowed > node.bytes.size()) {
    node.bytes.resize(openHandle.position + allowed);
  }
  if (allowed != 0) {
    std::memcpy(node.bytes.data() + openHandle.position, source, allowed);
  }
  openHandle.position += allowed;
  writeAllowance_ -= allowed;
  *bytesWritten = allowed;
  bytesWrittenTotal_ += allowed;
  return PdfStatus::success();
}

PdfStatus PdfTestCacheIo::flush(const PdfCacheHandle handle) {
  ++flushCalls_;
  if (!handle.valid() || handle.value >= 8 || !handles_[handle.value].open) {
    return PdfStatus::failure(PdfError::InvalidArgument);
  }
  return shouldFail(PdfTestFaultPoint::Flush) ? PdfStatus::failure(PdfError::IoFailure) : PdfStatus::success();
}

PdfStatus PdfTestCacheIo::sync(const PdfCacheHandle handle) {
  ++syncCalls_;
  if (!handle.valid() || handle.value >= 8 || !handles_[handle.value].open) {
    return PdfStatus::failure(PdfError::InvalidArgument);
  }
  syncObservations_.push_back(handles_[handle.value].path);
  return shouldFail(PdfTestFaultPoint::Sync) ? PdfStatus::failure(PdfError::IoFailure) : PdfStatus::success();
}

PdfStatus PdfTestCacheIo::close(PdfCacheHandle* handle) {
  ++closeCalls_;
  if (handle == nullptr || !handle->valid() || handle->value >= 8 || !handles_[handle->value].open) {
    return PdfStatus::failure(PdfError::InvalidArgument);
  }
  const bool failClose = shouldFail(PdfTestFaultPoint::Close);
  events_.push_back("close:" + handles_[handle->value].path);
  handles_[handle->value] = {};
  handle->value = kInvalidHandle;
  return failClose ? PdfStatus::failure(PdfError::IoFailure) : PdfStatus::success();
}

PdfStatus PdfTestCacheIo::remove(const char* path, const bool recursive) {
  ++removeCalls_;
  if (path == nullptr) {
    return PdfStatus::failure(PdfError::InvalidArgument);
  }
  removeObservations_.emplace_back(path);
  if (shouldFail(PdfTestFaultPoint::Remove)) {
    return PdfStatus::failure(PdfError::IoFailure);
  }
  const std::string key(path);
  auto found = nodes_.find(key);
  if (found == nodes_.end()) {
    return PdfStatus::failure(PdfError::InvalidOffset);
  }
  if (found->second.directory && !recursive) {
    return PdfStatus::failure(PdfError::InvalidArgument);
  }
  if (recursive) {
    const std::string prefix = key + "/";
    for (auto iterator = nodes_.begin(); iterator != nodes_.end();) {
      if (iterator->first == key || iterator->first.compare(0, prefix.size(), prefix) == 0) {
        iterator = nodes_.erase(iterator);
      } else {
        ++iterator;
      }
    }
  } else {
    nodes_.erase(found);
  }
  return PdfStatus::success();
}

PdfStatus PdfTestCacheIo::rename(const char* sourcePath, const char* destinationPath) {
  ++renameCalls_;
  if (sourcePath == nullptr || destinationPath == nullptr) {
    return PdfStatus::failure(PdfError::InvalidArgument);
  }
  if (shouldFail(PdfTestFaultPoint::Rename)) {
    return PdfStatus::failure(PdfError::IoFailure);
  }
  const std::string source(sourcePath);
  const std::string destination(destinationPath);
  auto found = nodes_.find(source);
  if (found == nodes_.end()) {
    return PdfStatus::failure(PdfError::InvalidOffset);
  }
  if (found->second.directory || nodes_.find(destination) != nodes_.end()) {
    return PdfStatus::failure(PdfError::IoFailure);
  }
  for (const OpenHandle& handle : handles_) {
    if (handle.open && (handle.path == source || handle.path == destination)) {
      return PdfStatus::failure(PdfError::IoFailure);
    }
  }
  nodes_.emplace(destination, std::move(found->second));
  nodes_.erase(found);
  events_.push_back("rename:" + source + "->" + destination);
  return PdfStatus::success();
}

PdfStatus PdfTestCacheIo::mkdir(const char* path) {
  ++mkdirCalls_;
  if (path == nullptr) {
    return PdfStatus::failure(PdfError::InvalidArgument);
  }
  if (shouldFail(PdfTestFaultPoint::Mkdir)) {
    return PdfStatus::failure(PdfError::IoFailure);
  }
  addDirectory(path);
  return PdfStatus::success();
}

PdfStatus PdfTestCacheIo::list(const char* path, const PdfCacheListVisitor visitor, void* visitorContext) {
  ++listCalls_;
  if (path == nullptr || visitor == nullptr) {
    return PdfStatus::failure(PdfError::InvalidArgument);
  }
  if (shouldFail(PdfTestFaultPoint::List)) {
    return PdfStatus::failure(PdfError::IoFailure);
  }
  const auto parent = nodes_.find(path);
  if (parent == nodes_.end() || !parent->second.directory) {
    return PdfStatus::failure(PdfError::InvalidOffset);
  }
  for (const auto& [candidate, node] : nodes_) {
    std::string name;
    if (!isDirectChild(path, candidate, &name)) {
      continue;
    }
    if (name.size() >= PDF_CACHE_ENTRY_NAME_CAPACITY) {
      return PdfStatus::failure(PdfError::LimitExceeded);
    }
    PdfCacheDirEntry entry{};
    entry.nameLength = static_cast<uint8_t>(name.size());
    std::memcpy(entry.name, name.data(), name.size());
    entry.name[name.size()] = '\0';
    entry.directory = node.directory;
    entry.symlinkLike = node.symlinkLike;
    const PdfStatus status = visitor(visitorContext, entry);
    if (!status) {
      return status;
    }
  }
  return PdfStatus::success();
}

PdfStatus PdfTestCacheIo::capacity(PdfCacheCapacity* capacity) {
  ++capacityCalls_;
  if (capacity == nullptr) {
    return PdfStatus::failure(PdfError::InvalidArgument);
  }
  if (shouldFail(PdfTestFaultPoint::Capacity)) {
    return PdfStatus::failure(PdfError::IoFailure);
  }
  *capacity = capacity_;
  return PdfStatus::success();
}

PdfStatus PdfTestCacheIo::metadata(const PdfCacheHandle handle, PdfCacheFileMetadata* metadata) {
  ++metadataCalls_;
  if (!handle.valid() || handle.value >= 8 || !handles_[handle.value].open || metadata == nullptr) {
    return PdfStatus::failure(PdfError::InvalidArgument);
  }
  if (shouldFail(PdfTestFaultPoint::Metadata)) {
    return PdfStatus::failure(PdfError::IoFailure);
  }
  if (metadata->operation == PdfCacheMetadataOperation::Seek) {
    return seek(handle, metadata->size);
  }
  if (metadata->operation == PdfCacheMetadataOperation::Truncate) {
    OpenHandle& opened = handles_[handle.value];
    auto found = nodes_.find(opened.path);
    if (!opened.writable || metadata->size > SIZE_MAX || found == nodes_.end() || found->second.directory ||
        metadata->size > found->second.bytes.size()) {
      return PdfStatus::failure(PdfError::IoFailure, metadata->size);
    }
    found->second.bytes.resize(static_cast<size_t>(metadata->size));
    opened.position = static_cast<size_t>(metadata->size);
    return PdfStatus::success();
  }
  const Node& node = nodes_.at(handles_[handle.value].path);
  metadata->size = node.bytes.size();
  metadata->modificationTime = {node.modificationTimeKnown, node.modificationTime};
  metadata->directory = node.directory;
  metadata->symlinkLike = node.symlinkLike;
  return PdfStatus::success();
}

PdfStatus PdfTestCacheIo::seek(const PdfCacheHandle handle, const uint64_t offset) {
  if (!handle.valid() || handle.value >= 8 || !handles_[handle.value].open || offset > SIZE_MAX) {
    return PdfStatus::failure(PdfError::InvalidArgument, offset);
  }
  const auto found = nodes_.find(handles_[handle.value].path);
  if (found == nodes_.end() || found->second.directory || offset > found->second.bytes.size()) {
    return PdfStatus::failure(PdfError::InvalidOffset, offset);
  }
  handles_[handle.value].position = static_cast<size_t>(offset);
  return PdfStatus::success();
}
