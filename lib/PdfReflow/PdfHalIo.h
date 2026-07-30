#pragma once

#include <cstddef>
#include <cstdint>

#include "PdfTypes.h"

class HalFile;

struct PdfHalByteStoreContext {
  HalFile* file = nullptr;
  uint64_t capacity = 0;
  uint64_t logicalSize = 0;
};

PdfByteSource pdfHalByteSource(HalFile& file);
PdfByteSink pdfHalByteSink(HalFile& file);
PdfFixedRecordStore pdfHalFixedRecordStore(HalFile& file, size_t recordSize, uint32_t capacity);
PdfStatus pdfInitializeHalByteStore(PdfHalByteStoreContext* context, HalFile& file, uint64_t capacity);
PdfByteStore pdfHalByteStore(PdfHalByteStoreContext& context);
