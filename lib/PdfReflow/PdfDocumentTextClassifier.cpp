#include "PdfDocumentTextClassifier.h"

#include <limits>

#include "PdfLimits.h"

PdfStatus PdfDocumentTextClassifier::begin(const uint32_t totalPages) {
  if (totalPages == 0 || totalPages > PdfLimits::MaxPages) {
    return PdfStatus::failure(PdfError::InvalidArgument, totalPages);
  }
  totalPages_ = totalPages;
  observedPages_ = 0;
  visibleScalars_ = 0;
  hiddenScalars_ = 0;
  sampleVisibleScalars_ = 0;
  sampleHiddenScalars_ = 0;
  sampleImages_ = 0;
  return PdfStatus::success();
}

PdfStatus PdfDocumentTextClassifier::observePage(const uint32_t pageOrdinal, const PdfPageTextStats& stats) {
  if (totalPages_ == 0 || pageOrdinal != observedPages_ || pageOrdinal >= totalPages_) {
    return PdfStatus::failure(PdfError::InvalidArgument, pageOrdinal);
  }
  const uint64_t totalScalars = static_cast<uint64_t>(visibleScalars_) + hiddenScalars_ +
                                stats.meaningfulVisibleScalars + stats.meaningfulHiddenScalars;
  const bool sampled = pageOrdinal < SamplePageLimit;
  const uint64_t sampleScalars = static_cast<uint64_t>(sampleVisibleScalars_) + sampleHiddenScalars_ +
                                 (sampled ? stats.meaningfulVisibleScalars : 0) +
                                 (sampled ? stats.meaningfulHiddenScalars : 0);
  if (stats.meaningfulVisibleScalars > std::numeric_limits<uint32_t>::max() - visibleScalars_ ||
      stats.meaningfulHiddenScalars > std::numeric_limits<uint32_t>::max() - hiddenScalars_ ||
      totalScalars > std::numeric_limits<uint32_t>::max() || sampleScalars > std::numeric_limits<uint32_t>::max()) {
    return PdfStatus::failure(PdfError::LimitExceeded, pageOrdinal);
  }
  if (sampled && (stats.meaningfulVisibleScalars > std::numeric_limits<uint32_t>::max() - sampleVisibleScalars_ ||
                  stats.meaningfulHiddenScalars > std::numeric_limits<uint32_t>::max() - sampleHiddenScalars_ ||
                  stats.imageCount > std::numeric_limits<uint32_t>::max() - sampleImages_)) {
    return PdfStatus::failure(PdfError::LimitExceeded, pageOrdinal);
  }
  if (sampled) {
    sampleVisibleScalars_ += stats.meaningfulVisibleScalars;
    sampleHiddenScalars_ += stats.meaningfulHiddenScalars;
    sampleImages_ += stats.imageCount;
  }
  visibleScalars_ += stats.meaningfulVisibleScalars;
  hiddenScalars_ += stats.meaningfulHiddenScalars;
  ++observedPages_;
  return PdfStatus::success();
}

PdfDocumentTextKind PdfDocumentTextClassifier::sampledKind() const {
  if (sampleVisibleScalars_ != 0 && sampleHiddenScalars_ != 0) {
    return PdfDocumentTextKind::Mixed;
  }
  if (sampleVisibleScalars_ != 0) {
    return PdfDocumentTextKind::BornDigital;
  }
  if (sampleHiddenScalars_ != 0) {
    return PdfDocumentTextKind::HiddenOcr;
  }
  return sampleImages_ != 0 ? PdfDocumentTextKind::ImageOnlyCandidate : PdfDocumentTextKind::Unknown;
}

PdfStatus PdfDocumentTextClassifier::finish(const PdfStatus extractionStatus) const {
  if (!extractionStatus.ok()) {
    return extractionStatus;
  }
  if (totalPages_ == 0 || observedPages_ != totalPages_) {
    return PdfStatus::failure(PdfError::InvalidArgument, observedPages_);
  }
  if (visibleScalars_ == 0 && hiddenScalars_ == 0) {
    return PdfStatus::failure(PdfError::NoReadableText, observedPages_);
  }
  return PdfStatus::success();
}
