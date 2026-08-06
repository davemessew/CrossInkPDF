#pragma once

#include "PdfCacheIo.h"

struct PdfHalCacheIoContext {};

inline PdfCacheIo PDF_TEST_HAL_CACHE_IO{};

inline void pdfTestSetHalCacheIo(const PdfCacheIo& io) { PDF_TEST_HAL_CACHE_IO = io; }

inline PdfCacheIo pdfHalCacheIo(PdfHalCacheIoContext&) { return PDF_TEST_HAL_CACHE_IO; }
