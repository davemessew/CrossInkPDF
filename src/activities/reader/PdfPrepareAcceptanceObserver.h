#pragma once

#include <I18nKeys.h>
#include <PdfTypes.h>

constexpr StrId pdfPrepareErrorTranslationKey(const PdfError error) {
  switch (error) {
    case PdfError::NoReadableText:
      return StrId::STR_PDF_NO_READABLE_TEXT;
    case PdfError::Encrypted:
      return StrId::STR_PDF_ENCRYPTED;
    case PdfError::UnsupportedFilter:
      return StrId::STR_PDF_UNSUPPORTED_FILTER;
    case PdfError::UnsupportedEncoding:
      return StrId::STR_PDF_UNSUPPORTED_ENCODING;
    case PdfError::InsufficientMemory:
      return StrId::STR_PDF_INSUFFICIENT_MEMORY;
    case PdfError::InsufficientStorage:
      return StrId::STR_PDF_INSUFFICIENT_STORAGE;
    case PdfError::Cancelled:
      return StrId::STR_PDF_PREPARATION_PAUSED;
    case PdfError::ExpansionLimit:
    case PdfError::LimitExceeded:
    case PdfError::Malformed:
    case PdfError::InvalidOffset:
    case PdfError::UnexpectedEof:
      return StrId::STR_PDF_DAMAGED_OR_UNSAFE;
    case PdfError::Unsupported:
      return StrId::STR_PDF_UNSUPPORTED;
    default:
      return StrId::STR_PDF_PREPARATION_FAILED;
  }
}

#if defined(SIMULATOR) || defined(CROSSINK_QEMU)

struct PdfPrepareAcceptanceObservation {
  PdfError error = PdfError::None;
  StrId translationKey = StrId::STR_PDF_PREPARATION_FAILED;
};

constexpr PdfPrepareAcceptanceObservation pdfPrepareAcceptanceObservationFor(
    const PdfError error) {
  return {error, pdfPrepareErrorTranslationKey(error)};
}

bool pdfObserveActivePrepareFailure(PdfPrepareAcceptanceObservation* observation);

#endif
