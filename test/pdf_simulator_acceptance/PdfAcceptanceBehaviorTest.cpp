#include <cstdint>

#include "PdfAcceptanceFramebufferGuard.h"
#include "activities/reader/PdfPrepareAcceptanceObserver.h"

namespace {

bool verifyFailureObservation() {
  const PdfPrepareAcceptanceObservation encrypted =
      pdfPrepareAcceptanceObservationFor(PdfError::Encrypted);
  const PdfPrepareAcceptanceObservation malformed =
      pdfPrepareAcceptanceObservationFor(PdfError::Malformed);
  return encrypted.error == PdfError::Encrypted &&
         encrypted.translationKey == StrId::STR_PDF_ENCRYPTED &&
         malformed.error == PdfError::Malformed &&
         malformed.translationKey == StrId::STR_PDF_DAMAGED_OR_UNSAFE &&
         malformed.error != encrypted.error &&
         malformed.translationKey != encrypted.translationKey;
}

bool verifyFramebufferObservation() {
  const uint8_t first = 0;
  const uint8_t second = 0;
  constexpr uint64_t hash = 0x5A5AA5A55A5AA5A5ULL;
  const PdfAcceptanceFramebufferSnapshot baseline{&first, 48000, hash};
  const PdfAcceptanceFramebufferSnapshot unchanged{&first, 48000, hash};
  const PdfAcceptanceFramebufferSnapshot changedHash{&first, 48000, hash ^ 1U};
  const PdfAcceptanceFramebufferSnapshot changedPointer{&second, 48000, hash};
  uint32_t checks = 0;
  uint32_t violations = 0;

  if (!pdfAcceptanceObserveFramebuffer(baseline, unchanged, checks, violations) ||
      checks != 1 || violations != 0) {
    return false;
  }
  if (pdfAcceptanceObserveFramebuffer(baseline, changedHash, checks, violations) ||
      checks != 2 || violations != 1) {
    return false;
  }
  if (pdfAcceptanceObserveFramebuffer(baseline, changedPointer, checks, violations) ||
      checks != 3 || violations != 2) {
    return false;
  }
  return true;
}

}  // namespace

int main() {
  if (!verifyFailureObservation()) {
    return 1;
  }
  if (!verifyFramebufferObservation()) {
    return 2;
  }
  return 0;
}
