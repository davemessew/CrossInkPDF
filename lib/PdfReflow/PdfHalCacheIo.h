#pragma once

#include <HalStorage.h>

#include "PdfCacheIo.h"

constexpr uint8_t PDF_HAL_CACHE_HANDLE_COUNT = 3;

struct PdfHalCacheIoContext {
  HalFile files[PDF_HAL_CACHE_HANDLE_COUNT];
  bool used[PDF_HAL_CACHE_HANDLE_COUNT]{};
};

PdfCacheIo pdfHalCacheIo(PdfHalCacheIoContext& context);
PdfStatus pdfHalCacheRename(void* context, const char* sourcePath,
                            const char* destinationPath);
