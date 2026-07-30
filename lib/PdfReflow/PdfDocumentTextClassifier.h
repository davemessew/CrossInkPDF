#pragma once

#include <cstdint>

#include "PdfTypes.h"

enum class PdfDocumentTextKind : uint8_t {
  Unknown,
  BornDigital,
  HiddenOcr,
  Mixed,
  ImageOnlyCandidate,
};

struct PdfPageTextStats {
  uint32_t meaningfulVisibleScalars = 0;
  uint32_t meaningfulHiddenScalars = 0;
  uint16_t imageCount = 0;
};

class PdfDocumentTextClassifier {
 public:
  static constexpr uint8_t SamplePageLimit = 3;

  PdfStatus begin(uint32_t totalPages);
  PdfStatus observePage(uint32_t pageOrdinal, const PdfPageTextStats& stats);
  PdfDocumentTextKind sampledKind() const;
  PdfStatus finish(PdfStatus extractionStatus) const;

  uint32_t observedPages() const { return observedPages_; }
  uint32_t meaningfulScalarCount() const { return visibleScalars_ + hiddenScalars_; }

 private:
  uint32_t totalPages_ = 0;
  uint32_t observedPages_ = 0;
  uint32_t visibleScalars_ = 0;
  uint32_t hiddenScalars_ = 0;
  uint32_t sampleVisibleScalars_ = 0;
  uint32_t sampleHiddenScalars_ = 0;
  uint32_t sampleImages_ = 0;
};
