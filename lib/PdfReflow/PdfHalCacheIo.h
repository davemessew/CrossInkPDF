#pragma once

#include <HalStorage.h>

#include "PdfCacheIo.h"

// Preparation deliberately keeps durable writers open across pages to avoid
// repeated SD sync/open cycles. Complex documents can concurrently need the
// source, page records, journal, section, content, and temporary stores.
constexpr uint8_t PDF_HAL_CACHE_HANDLE_COUNT = 8;

struct PdfHalCacheIoContext {
  HalFile files[PDF_HAL_CACHE_HANDLE_COUNT];
  bool used[PDF_HAL_CACHE_HANDLE_COUNT]{};
};

PdfCacheIo pdfHalCacheIo(PdfHalCacheIoContext& context);
PdfStatus pdfHalCacheRename(void* context, const char* sourcePath,
                            const char* destinationPath);
