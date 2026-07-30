#pragma once

#include <cstdint>

#include "PdfCacheFormat.h"
#include "PdfIo.h"
#include "PdfSourceIdentity.h"

enum class PdfBuildPhase : uint8_t {
  None,
  Discover,
  ParsePages,
  EmitSections,
  EmitImages,
  Finalize,
  Complete,
  Failed,
  Cancelled,
};

struct PdfBuildCheckpoint {
  uint32_t sequence = 0;
  PdfSourceIdentity source{};
  uint32_t generation = 0;
  PdfBuildPhase phase = PdfBuildPhase::None;
  uint32_t lastVerifiedPage = 0;
  uint32_t lastVerifiedObject = 0;
  uint32_t emittedSections = 0;
  uint32_t emittedImages = 0;
  uint32_t cumulativeWords = 0;
  uint64_t outputBytes = 0;
  uint32_t warningFlags = 0;
};

struct PdfCheckpointGate {
  uint32_t completedPages = 0;
  uint64_t outputBytes = 0;
  uint32_t committedAtMs = 0;
};

PdfStatus pdfEncodeBuildCheckpoint(const PdfBuildCheckpoint& checkpoint, const PdfByteSink& destination);
PdfStatus pdfDecodeBuildCheckpoint(const PdfByteSource& source, PdfBuildCheckpoint* checkpoint);
bool pdfCheckpointDue(const PdfCheckpointGate& gate, uint32_t completedPages, uint64_t outputBytes, uint32_t nowMs,
                      bool forced);
void pdfCheckpointCommitted(PdfCheckpointGate* gate, uint32_t completedPages, uint64_t outputBytes, uint32_t nowMs);
