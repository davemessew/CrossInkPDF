#include "PdfHalReflowDocument.h"

#include "Memory.h"

PdfStatus PdfHalReflowDocument::load(const char* const sourcePath, const char* const cacheDirectory,
                                     const uint64_t* const cacheHashOverride) {
  PdfStatus status = initialize(pdfHalCacheIo(ioContext_), sourcePath, cacheDirectory, cacheHashOverride);
  return status ? loadCompletedCache() : status;
}

std::unique_ptr<PdfHalReflowDocument> loadPdfHalReflowDocumentNoThrow(const char* const sourcePath,
                                                                      const char* const cacheDirectory,
                                                                      PdfStatus* const status,
                                                                      const uint64_t* const cacheHashOverride) {
  auto document = makeUniqueNoThrow<PdfHalReflowDocument>();
  PdfStatus result = document ? document->load(sourcePath, cacheDirectory, cacheHashOverride)
                              : PdfStatus::failure(PdfError::InsufficientMemory);
  if (status != nullptr) {
    *status = result;
  }
  if (!result) {
    document.reset();
  }
  return document;
}
