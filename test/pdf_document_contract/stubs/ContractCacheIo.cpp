#include "ContractCacheIo.h"

#include <algorithm>
#include <cstring>
#include <utility>

namespace {

constexpr uint8_t kInvalidHandle = 0xff;

bool directChildName(const std::string& parent, const std::string& candidate, std::string* const name) {
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

ContractCacheIo::ContractCacheIo() {
  capacity_.total = {true, 64ULL * 1024ULL * 1024ULL};
  capacity_.free = {true, 32ULL * 1024ULL * 1024ULL};
}

PdfCacheIo ContractCacheIo::io() {
  return {this,       openThunk,   readThunk,  writeThunk, flushThunk,    syncThunk,
          closeThunk, removeThunk, mkdirThunk, listThunk,  capacityThunk, metadataThunk};
}

void ContractCacheIo::addFile(const std::string& path, const std::vector<uint8_t>& bytes) {
  Node& node = nodes_[path];
  node.bytes = bytes;
  node.modificationTime = 1234;
  node.directory = false;
}

bool ContractCacheIo::exists(const std::string& path) const { return nodes_.contains(path); }

bool ContractCacheIo::isDirectory(const std::string& path) const {
  const auto found = nodes_.find(path);
  return found != nodes_.end() && found->second.directory;
}

const std::vector<uint8_t>& ContractCacheIo::bytes(const std::string& path) const { return nodes_.at(path).bytes; }

uint32_t ContractCacheIo::openHandleCount() const {
  return static_cast<uint32_t>(std::count_if(handles_.begin(), handles_.end(), [](const OpenHandle& handle) {
    return handle.open;
  }));
}

uint32_t ContractCacheIo::syncCount(const std::string& path) const {
  const auto found = syncCounts_.find(path);
  return found == syncCounts_.end() ? 0U : found->second;
}

void ContractCacheIo::resetMetrics() {
  metrics_ = {};
  syncCounts_.clear();
}

void ContractCacheIo::recordOperation() {
  ++metrics_.operations;
  if (operationClock_ != nullptr) {
    *operationClock_ += millisecondsPerOperation_;
  }
}

void ContractCacheIo::traceOperation(const char* operation, const std::string& path) {
  operationTrace_.push_back(path.empty() ? std::string(operation) : std::string(operation) + ":" + path);
}

PdfStatus ContractCacheIo::openThunk(void* context, const char* path, const PdfCacheOpenMode mode,
                                     PdfCacheHandle* handle) {
  return static_cast<ContractCacheIo*>(context)->open(path, mode, handle);
}

PdfStatus ContractCacheIo::readThunk(void* context, const PdfCacheHandle handle, const uint64_t offset,
                                     uint8_t* destination, const size_t requested, size_t* bytesRead) {
  return static_cast<ContractCacheIo*>(context)->read(handle, offset, destination, requested, bytesRead);
}

PdfStatus ContractCacheIo::writeThunk(void* context, const PdfCacheHandle handle, const uint8_t* source,
                                      const size_t requested, size_t* bytesWritten) {
  return static_cast<ContractCacheIo*>(context)->write(handle, source, requested, bytesWritten);
}

PdfStatus ContractCacheIo::flushThunk(void* context, const PdfCacheHandle handle) {
  return static_cast<ContractCacheIo*>(context)->flush(handle);
}

PdfStatus ContractCacheIo::syncThunk(void* context, const PdfCacheHandle handle) {
  return static_cast<ContractCacheIo*>(context)->sync(handle);
}

PdfStatus ContractCacheIo::closeThunk(void* context, PdfCacheHandle* handle) {
  return static_cast<ContractCacheIo*>(context)->close(handle);
}

PdfStatus ContractCacheIo::removeThunk(void* context, const char* path, const bool recursive) {
  return static_cast<ContractCacheIo*>(context)->remove(path, recursive);
}

PdfStatus ContractCacheIo::renameThunk(void* context, const char* sourcePath, const char* destinationPath) {
  return static_cast<ContractCacheIo*>(context)->rename(sourcePath, destinationPath);
}

PdfStatus ContractCacheIo::mkdirThunk(void* context, const char* path) {
  return static_cast<ContractCacheIo*>(context)->mkdir(path);
}

PdfStatus ContractCacheIo::listThunk(void* context, const char* path, const PdfCacheListVisitor visitor,
                                     void* visitorContext) {
  return static_cast<ContractCacheIo*>(context)->list(path, visitor, visitorContext);
}

PdfStatus ContractCacheIo::capacityThunk(void* context, PdfCacheCapacity* capacity) {
  return context == nullptr ? PdfStatus::failure(PdfError::InvalidArgument)
                            : static_cast<ContractCacheIo*>(context)->capacity(capacity);
}

PdfStatus ContractCacheIo::metadataThunk(void* context, const PdfCacheHandle handle,
                                         PdfCacheFileMetadata* metadata) {
  return static_cast<ContractCacheIo*>(context)->metadata(handle, metadata);
}

PdfStatus ContractCacheIo::open(const char* path, const PdfCacheOpenMode mode, PdfCacheHandle* handle) {
  recordOperation();
  ++metrics_.opens;
  traceOperation("open", path == nullptr ? std::string{} : std::string(path));
  if (path == nullptr || handle == nullptr) {
    return PdfStatus::failure(PdfError::InvalidArgument);
  }
  *handle = {};
  const std::string key(path);
  auto found = nodes_.find(key);
  if (mode == PdfCacheOpenMode::Read) {
    if (found == nodes_.end() || found->second.directory) {
      return PdfStatus::failure(PdfError::InvalidOffset);
    }
  } else if (mode == PdfCacheOpenMode::WriteTruncate) {
    Node& node = nodes_[key];
    node.bytes.clear();
    node.directory = false;
  } else {
    Node& node = nodes_[key];
    if (node.directory) {
      return PdfStatus::failure(PdfError::InvalidOffset);
    }
    node.directory = false;
  }
  const bool readable = mode == PdfCacheOpenMode::Read || mode == PdfCacheOpenMode::ReadWrite;
  for (uint8_t index = 0; index < handles_.size(); ++index) {
    if (!handles_[index].open) {
      handles_[index] = {key, 0, readable, mode != PdfCacheOpenMode::Read, true};
      handle->value = index;
      const uint32_t readers = static_cast<uint32_t>(
          std::count_if(handles_.begin(), handles_.end(), [](const OpenHandle& candidate) {
            return candidate.open && candidate.readable;
          }));
      maximumReadHandleCount_ = std::max(maximumReadHandleCount_, readers);
      if (readers > 1U) {
        for (uint8_t prior = 0; prior < handles_.size(); ++prior) {
          if (prior != index && handles_[prior].open && handles_[prior].readable) {
            lastReaderOverlap_ = handles_[prior].path + " + " + key;
            break;
          }
        }
      }
      return PdfStatus::success();
    }
  }
  return PdfStatus::failure(PdfError::LimitExceeded);
}

PdfStatus ContractCacheIo::read(const PdfCacheHandle handle, const uint64_t offset, uint8_t* destination,
                                const size_t requested, size_t* bytesRead) {
  recordOperation();
  ++metrics_.reads;
  traceOperation("read", handle.valid() && handle.value < handles_.size() ? handles_[handle.value].path : "");
  if (!handle.valid() || handle.value >= handles_.size() || !handles_[handle.value].open ||
      !handles_[handle.value].readable || destination == nullptr ||
      bytesRead == nullptr) {
    return PdfStatus::failure(PdfError::InvalidArgument, offset);
  }
  const auto found = nodes_.find(handles_[handle.value].path);
  if (found == nodes_.end() || found->second.directory || offset > found->second.bytes.size()) {
    return PdfStatus::failure(PdfError::InvalidOffset, offset);
  }
  const size_t available = found->second.bytes.size() - static_cast<size_t>(offset);
  const bool shortRead = shortReadBytes_ != static_cast<size_t>(-1);
  *bytesRead = std::min(requested, available);
  if (shortRead) {
    *bytesRead = std::min(*bytesRead, shortReadBytes_);
  }
  shortReadBytes_ = static_cast<size_t>(-1);
  if (*bytesRead != 0) {
    std::memcpy(destination, found->second.bytes.data() + static_cast<size_t>(offset), *bytesRead);
  }
  metrics_.bytesRead += *bytesRead;
  return PdfStatus::success();
}

PdfStatus ContractCacheIo::write(const PdfCacheHandle handle, const uint8_t* source, const size_t requested,
                                 size_t* bytesWritten) {
  recordOperation();
  ++metrics_.writes;
  traceOperation("write", handle.valid() && handle.value < handles_.size() ? handles_[handle.value].path : "");
  if (!handle.valid() || handle.value >= handles_.size() || !handles_[handle.value].open ||
      !handles_[handle.value].writable || source == nullptr || bytesWritten == nullptr) {
    return PdfStatus::failure(PdfError::InvalidArgument);
  }
  OpenHandle& open = handles_[handle.value];
  Node& node = nodes_[open.path];
  if (open.position > node.bytes.size()) {
    return PdfStatus::failure(PdfError::InvalidOffset, open.position);
  }
  const bool fail = failingWriteBytes_ != static_cast<size_t>(-1);
  const bool shortWrite = shortWriteBytes_ != static_cast<size_t>(-1);
  const size_t accepted = fail ? std::min(requested, failingWriteBytes_)
                               : shortWrite ? std::min(requested, shortWriteBytes_) : requested;
  failingWriteBytes_ = static_cast<size_t>(-1);
  shortWriteBytes_ = static_cast<size_t>(-1);
  if (open.position + accepted > node.bytes.size()) {
    node.bytes.resize(open.position + accepted);
  }
  if (accepted != 0) {
    std::memcpy(node.bytes.data() + open.position, source, accepted);
  }
  open.position += accepted;
  *bytesWritten = accepted;
  metrics_.bytesWritten += accepted;
  return fail ? PdfStatus::failure(PdfError::IoFailure, open.position) : PdfStatus::success();
}

PdfStatus ContractCacheIo::flush(const PdfCacheHandle handle) {
  recordOperation();
  ++metrics_.flushes;
  traceOperation("flush", handle.valid() && handle.value < handles_.size() ? handles_[handle.value].path : "");
  return handle.valid() && handle.value < handles_.size() && handles_[handle.value].open
             ? PdfStatus::success()
             : PdfStatus::failure(PdfError::InvalidArgument);
}

PdfStatus ContractCacheIo::sync(const PdfCacheHandle handle) {
  recordOperation();
  ++metrics_.syncs;
  traceOperation("sync", handle.valid() && handle.value < handles_.size() ? handles_[handle.value].path : "");
  if (!handle.valid() || handle.value >= handles_.size() || !handles_[handle.value].open) {
    return PdfStatus::failure(PdfError::InvalidArgument);
  }
  ++syncCounts_[handles_[handle.value].path];
  return PdfStatus::success();
}

PdfStatus ContractCacheIo::close(PdfCacheHandle* handle) {
  recordOperation();
  ++metrics_.closes;
  traceOperation("close", handle != nullptr && handle->valid() && handle->value < handles_.size()
                              ? handles_[handle->value].path
                              : "");
  if (handle == nullptr || !handle->valid() || handle->value >= handles_.size() || !handles_[handle->value].open) {
    return PdfStatus::failure(PdfError::InvalidArgument);
  }
  handles_[handle->value] = {};
  handle->value = kInvalidHandle;
  return PdfStatus::success();
}

PdfStatus ContractCacheIo::remove(const char* path, const bool recursive) {
  recordOperation();
  if (path == nullptr) {
    return PdfStatus::failure(PdfError::InvalidArgument);
  }
  const std::string key(path);
  const auto found = nodes_.find(key);
  if (found == nodes_.end()) {
    return PdfStatus::failure(PdfError::InvalidOffset);
  }
  if (found->second.directory && !recursive) {
    return PdfStatus::failure(PdfError::InvalidArgument);
  }
  if (recursive) {
    const std::string prefix = key + '/';
    for (auto iterator = nodes_.begin(); iterator != nodes_.end();) {
      if (iterator->first == key || iterator->first.starts_with(prefix)) {
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

PdfStatus ContractCacheIo::rename(const char* sourcePath, const char* destinationPath) {
  recordOperation();
  if (sourcePath == nullptr || destinationPath == nullptr) {
    return PdfStatus::failure(PdfError::InvalidArgument);
  }
  const std::string source(sourcePath);
  const std::string destination(destinationPath);
  auto found = nodes_.find(source);
  if (found == nodes_.end() || found->second.directory || nodes_.contains(destination)) {
    return PdfStatus::failure(PdfError::IoFailure);
  }
  for (const OpenHandle& handle : handles_) {
    if (handle.open && (handle.path == source || handle.path == destination)) {
      return PdfStatus::failure(PdfError::IoFailure);
    }
  }
  nodes_.emplace(destination, std::move(found->second));
  nodes_.erase(found);
  return PdfStatus::success();
}

PdfStatus ContractCacheIo::mkdir(const char* path) {
  recordOperation();
  if (path == nullptr) {
    return PdfStatus::failure(PdfError::InvalidArgument);
  }
  Node& node = nodes_[path];
  node.bytes.clear();
  node.directory = true;
  return PdfStatus::success();
}

PdfStatus ContractCacheIo::list(const char* path, const PdfCacheListVisitor visitor, void* visitorContext) {
  recordOperation();
  if (path == nullptr || visitor == nullptr) {
    return PdfStatus::failure(PdfError::InvalidArgument);
  }
  const auto parent = nodes_.find(path);
  if (parent == nodes_.end() || !parent->second.directory) {
    return PdfStatus::failure(PdfError::InvalidOffset);
  }
  for (const auto& [candidate, node] : nodes_) {
    std::string name;
    if (!directChildName(path, candidate, &name)) {
      continue;
    }
    if (name.size() >= PDF_CACHE_ENTRY_NAME_CAPACITY) {
      return PdfStatus::failure(PdfError::LimitExceeded);
    }
    PdfCacheDirEntry entry{};
    std::memcpy(entry.name, name.data(), name.size());
    entry.name[name.size()] = '\0';
    entry.nameLength = static_cast<uint8_t>(name.size());
    entry.directory = node.directory;
    const PdfStatus status = visitor(visitorContext, entry);
    if (!status.ok()) {
      return status;
    }
  }
  return PdfStatus::success();
}

PdfStatus ContractCacheIo::capacity(PdfCacheCapacity* capacity) {
  recordOperation();
  if (capacity == nullptr) {
    return PdfStatus::failure(PdfError::InvalidArgument);
  }
  *capacity = capacity_;
  return PdfStatus::success();
}

PdfStatus ContractCacheIo::metadata(const PdfCacheHandle handle, PdfCacheFileMetadata* metadata) {
  recordOperation();
  traceOperation("metadata", handle.valid() && handle.value < handles_.size() ? handles_[handle.value].path : "");
  if (!handle.valid() || handle.value >= handles_.size() || !handles_[handle.value].open || metadata == nullptr) {
    return PdfStatus::failure(PdfError::InvalidArgument);
  }
  const auto found = nodes_.find(handles_[handle.value].path);
  if (found == nodes_.end()) {
    return PdfStatus::failure(PdfError::InvalidOffset);
  }
  if (metadata->operation == PdfCacheMetadataOperation::Seek) {
    if (!handles_[handle.value].writable || metadata->size > found->second.bytes.size()) {
      return PdfStatus::failure(PdfError::InvalidOffset, metadata->size);
    }
    handles_[handle.value].position = static_cast<size_t>(metadata->size);
    return PdfStatus::success();
  }
  if (metadata->operation == PdfCacheMetadataOperation::Truncate) {
    if (!handles_[handle.value].writable || metadata->size > found->second.bytes.size()) {
      return PdfStatus::failure(PdfError::InvalidOffset, metadata->size);
    }
    found->second.bytes.resize(static_cast<size_t>(metadata->size));
    handles_[handle.value].position = static_cast<size_t>(metadata->size);
    return PdfStatus::success();
  }
  metadata->size = found->second.bytes.size();
  metadata->modificationTime = {true, found->second.modificationTime};
  metadata->directory = found->second.directory;
  metadata->symlinkLike = false;
  return PdfStatus::success();
}
