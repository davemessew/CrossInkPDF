#include "PdfHalReflowDocument.h"

#include "Memory.h"

PdfStatus PdfHalReflowDocument::load(const char* const sourcePath, const char* const cacheDirectory) {
  PdfStatus status = initialize(pdfHalCacheIo(ioContext_), sourcePath, cacheDirectory);
  return status ? loadCompletedCache() : status;
}

std::unique_ptr<PdfHalReflowDocument> loadPdfHalReflowDocumentNoThrow(const char* const sourcePath,
                                                                      const char* const cacheDirectory,
                                                                      PdfStatus* const status) {
  auto document = makeUniqueNoThrow<PdfHalReflowDocument>();
  PdfStatus result =
      document ? document->load(sourcePath, cacheDirectory) : PdfStatus::failure(PdfError::InsufficientMemory);
  if (status != nullptr) {
    *status = result;
  }
  if (!result) {
    document.reset();
  }
  return document;
}
