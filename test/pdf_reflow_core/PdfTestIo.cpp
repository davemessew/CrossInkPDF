#include "PdfTestIo.h"

#include <algorithm>
#include <cstring>
#include <utility>

PdfTestByteSource::PdfTestByteSource(std::vector<uint8_t> bytes) : bytes_(std::move(bytes)) {}

PdfByteSource PdfTestByteSource::source(const uint64_t advertisedSize) {
  return {this, advertisedSize == 0 ? bytes_.size() : advertisedSize, readAt};
}

PdfStatus PdfTestByteSource::readAt(void* context, const uint64_t offset, uint8_t* destination,
                                    const size_t requested, size_t* bytesRead) {
  auto& source = *static_cast<PdfTestByteSource*>(context);
  if (destination == nullptr || bytesRead == nullptr) {
    return PdfStatus::failure(PdfError::InvalidArgument, offset);
  }
  if (offset >= source.failureOffset_) {
    return PdfStatus::failure(PdfError::IoFailure, offset);
  }
  if (offset >= source.bytes_.size()) {
    *bytesRead = 0;
    return PdfStatus::success();
  }
  const size_t available = source.bytes_.size() - static_cast<size_t>(offset);
  *bytesRead = std::min({requested, available, source.maximumRead_});
  std::memcpy(destination, source.bytes_.data() + offset, *bytesRead);
  return PdfStatus::success();
}

PdfByteSink PdfTestByteSink::sink() {
  return {this, write};
}

PdfStatus PdfTestByteSink::write(void* context, const uint8_t* source, const size_t requested,
                                 size_t* bytesWritten) {
  auto& sink = *static_cast<PdfTestByteSink*>(context);
  if (source == nullptr || bytesWritten == nullptr) {
    return PdfStatus::failure(PdfError::InvalidArgument);
  }
  *bytesWritten = std::min(requested, sink.maximumWrite_);
  sink.bytes_.insert(sink.bytes_.end(), source, source + *bytesWritten);
  return PdfStatus::success();
}
