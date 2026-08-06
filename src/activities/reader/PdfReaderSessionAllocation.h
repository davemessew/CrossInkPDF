#pragma once

#include <Memory.h>
#include <ReflowDocument.h>

#include <memory>

// Keep the format gate next to the fallible allocation so ordinary EPUB
// sessions cannot accidentally allocate PDF-only reader state.
template <typename State>
std::unique_ptr<State> allocatePdfReaderSessionState(const ReflowDocumentFormat format) {
  if (format != ReflowDocumentFormat::Pdf) {
    return {};
  }
  return makeUniqueNoThrow<State>();
}
