#pragma once

#if defined(SIMULATOR) || defined(CROSSINK_QEMU)

#include <cstddef>
#include <cstdint>

#define CROSSINK_PDF_ACCEPTANCE_FRAMEBUFFER_GUARD_ENABLED 1

struct PdfAcceptanceFramebufferSnapshot {
  const uint8_t* pointer = nullptr;
  size_t bytes = 0;
  uint64_t hash = 0;
};

constexpr bool pdfAcceptanceFramebufferUnchanged(
    const PdfAcceptanceFramebufferSnapshot& expected,
    const PdfAcceptanceFramebufferSnapshot& observed) {
  return expected.pointer != nullptr && expected.bytes == 48000 &&
         expected.hash != 0 && observed.pointer == expected.pointer &&
         observed.bytes == expected.bytes && observed.hash == expected.hash;
}

inline bool pdfAcceptanceObserveFramebuffer(
    const PdfAcceptanceFramebufferSnapshot& expected,
    const PdfAcceptanceFramebufferSnapshot& observed, uint32_t& checks,
    uint32_t& violations) {
  ++checks;
  const bool unchanged =
      pdfAcceptanceFramebufferUnchanged(expected, observed);
  if (!unchanged) {
    ++violations;
  }
  return unchanged;
}

#endif  // defined(SIMULATOR) || defined(CROSSINK_QEMU)
