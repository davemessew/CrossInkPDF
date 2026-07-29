#pragma once

#include <cstddef>

#include "PdfTypes.h"

class HalFile;

PdfByteSource pdfHalByteSource(HalFile& file);
PdfByteSink pdfHalByteSink(HalFile& file);
PdfFixedRecordStore pdfHalFixedRecordStore(HalFile& file, size_t recordSize, uint32_t capacity);
