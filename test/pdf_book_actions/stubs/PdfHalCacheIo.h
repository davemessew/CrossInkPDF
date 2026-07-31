#pragma once

#include "PdfCachedProductState.h"

struct PdfHalCacheIoContext {};

inline PdfCacheIo pdfHalCacheIo(PdfHalCacheIoContext& context) { return {&context}; }
