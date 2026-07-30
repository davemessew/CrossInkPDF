#pragma once

#include <memory>

#include "PdfHalCacheIo.h"
#include "PdfReflowDocument.h"

class PdfHalReflowDocument final : public PdfReflowDocument {
 public:
  PdfStatus load(const char* sourcePath, const char* cacheDirectory);

 private:
  PdfHalCacheIoContext ioContext_{};
};

std::unique_ptr<PdfHalReflowDocument> loadPdfHalReflowDocumentNoThrow(const char* sourcePath,
                                                                      const char* cacheDirectory,
                                                                      PdfStatus* status = nullptr);
