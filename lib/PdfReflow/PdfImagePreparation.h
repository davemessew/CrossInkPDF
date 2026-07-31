#pragma once

#include <cstdint>

#include "PdfImageExtractor.h"
#include "PdfPageModel.h"
#include "PdfTypes.h"

enum class PdfImageOmitReason : uint8_t {
  None = 0,
  InvalidPlacement,
  TinyDecoration,
  Rule,
  Background,
  RepeatedDecoration,
  NoNearbySemanticContent,
};

struct PdfImageMeaningInput {
  PdfImagePlacement placement{};
  uint16_t pageWidth = 0;
  uint16_t pageHeight = 0;
  uint16_t sourcePageIndex = 0;
  uint32_t nearestAnchorOrdinal = 0;
  uint8_t repetitionCount = 1;
  bool nearbySemanticContent = false;
  bool firstMeaningfulEarlyImage = false;
};

struct PdfImageMeaningDecision {
  uint32_t anchorOrdinal = 0;
  PdfImageOmitReason omitReason = PdfImageOmitReason::None;
  bool retain = false;
  bool coverCandidate = false;
};

PdfStatus pdfClassifyMeaningfulImage(const PdfImageMeaningInput& input, PdfImageMeaningDecision* decision);
PdfStatus pdfMakePreparationImageWorkspace(uint8_t* pageText, size_t pageTextBytes, uint8_t* decoderOutput,
                                           size_t decoderOutputBytes, PdfImageWorkspace* workspace);
