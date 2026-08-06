#pragma once

#include <HalStorage.h>
#include <PdfLayoutWordIndex.h>

namespace pdf_hal_test_detail {

inline PdfStatus readAt(void* context, const uint64_t offset, uint8_t* destination, const size_t requested,
                        size_t* bytesRead) {
  if (context == nullptr || destination == nullptr || bytesRead == nullptr || offset > UINT32_MAX) {
    return PdfStatus::failure(PdfError::InvalidArgument, offset);
  }
  auto& file = *static_cast<HalFile*>(context);
  if (!file.seek(static_cast<uint32_t>(offset))) {
    return PdfStatus::failure(PdfError::IoFailure, offset);
  }
  const int result = file.read(destination, requested);
  if (result < 0) {
    return PdfStatus::failure(PdfError::IoFailure, offset);
  }
  *bytesRead = static_cast<size_t>(result);
  return PdfStatus::success();
}

inline PdfStatus write(void* context, const uint8_t* source, const size_t requested, size_t* bytesWritten) {
  if (context == nullptr || source == nullptr || bytesWritten == nullptr) {
    return PdfStatus::failure(PdfError::InvalidArgument);
  }
  *bytesWritten = static_cast<HalFile*>(context)->write(source, requested);
  return PdfStatus::success();
}

}  // namespace pdf_hal_test_detail

inline PdfByteSink pdfHalByteSink(HalFile& file) { return {&file, pdf_hal_test_detail::write}; }
inline PdfByteSource pdfHalByteSource(HalFile& file) {
  return {&file, file.size(), pdf_hal_test_detail::readAt};
}
