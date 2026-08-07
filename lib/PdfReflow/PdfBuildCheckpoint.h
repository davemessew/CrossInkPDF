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

enum class PdfBuildResumePhase : uint8_t {
  None,
  CommitManifest,
  AfterEmitSections,
  AfterPage,
  AfterImage,
  AfterImageRepair,
  AfterDiscovery,
};

inline constexpr uint16_t PDF_BUILD_CHECKPOINT_CODEC_VERSION = 3;

struct PdfBuildCheckpoint {
  uint32_t sequence = 0;
  PdfSourceIdentity source{};
  uint32_t generation = 0;
  PdfBuildPhase phase = PdfBuildPhase::None;
  PdfBuildResumePhase resumePhase = PdfBuildResumePhase::None;
  uint32_t lastVerifiedPage = 0;
  uint32_t lastVerifiedObject = 0;
  uint32_t emittedSections = 0;
  uint32_t emittedImages = 0;
  uint32_t cumulativeWords = 0;
  uint64_t outputBytes = 0;
  uint32_t warningFlags = 0;
  // The append journal is bounded by the 8 MiB cache policy, so its committed
  // prefix fits in the codec's three formerly-reserved identity bytes.
  uint32_t journalBytes = 0;
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
