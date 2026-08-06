#pragma once

#include <cstddef>
#include <cstdint>

#include "PdfTypes.h"
#include "PdfWorkBudget.h"

struct PdfReadExactState {
  uint64_t sourceOffset = 0;
  uint8_t* destination = nullptr;
  size_t length = 0;
  size_t completed = 0;
};

struct PdfByteRange {
  PdfByteSource parent{};
  uint64_t offset = 0;
  uint64_t length = 0;
};

PdfStatus pdfInitializeByteRange(const PdfByteSource& parent, uint64_t offset, uint64_t length, PdfByteRange* range);
PdfByteSource pdfByteRangeSource(PdfByteRange& range);

PdfStepResult pdfStepReadExact(const PdfByteSource& source, PdfReadExactState& state, PdfWorkBudget& budget);
PdfStatus pdfReadExact(const PdfByteSource& source, uint64_t offset, uint8_t* destination, size_t length);

PdfStatus pdfWriteExact(const PdfByteSink& sink, const uint8_t* source, size_t length);
PdfStatus pdfReadRecord(const PdfFixedRecordStore& store, uint32_t ordinal, void* record);
PdfStatus pdfWriteRecord(const PdfFixedRecordStore& store, uint32_t ordinal, const void* record);
PdfByteSource pdfByteStoreSource(PdfByteStore& store);
PdfByteSink pdfByteStoreSink(PdfByteStore& store);
