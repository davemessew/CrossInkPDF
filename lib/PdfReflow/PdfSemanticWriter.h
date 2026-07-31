#pragma once

#include <cstddef>
#include <cstdint>

#include "PdfTypes.h"
#include "PdfWordCounter.h"

namespace PdfSemanticWriterLimits {

inline constexpr size_t MinimumOutputBufferBytes = 128;
inline constexpr size_t PublisherLabelBytes = 16;
inline constexpr size_t AnchorBytes = 10;

}  // namespace PdfSemanticWriterLimits

enum class PdfSemanticBlockKind : uint8_t {
  Paragraph,
  Heading,
  TableCell,
};

struct PdfSemanticBlock {
  PdfSemanticBlockKind kind = PdfSemanticBlockKind::Paragraph;
  uint32_t anchorOrdinal = 0;
  uint8_t headingLevel = 0;
};

struct PdfSemanticBlockRecord {
  uint32_t anchorOrdinal = 0;
  uint32_t cumulativeWordStart = 0;
  uint32_t wordCount = 0;
  char anchor[PdfSemanticWriterLimits::AnchorBytes]{};
  uint8_t reserved[2]{};
};

static_assert(sizeof(PdfSemanticBlockRecord) == 24);

struct PdfSemanticBlockSink {
  using EmitFn = PdfStatus (*)(void* context, const PdfSemanticBlockRecord& record);

  void* context = nullptr;
  EmitFn emit = nullptr;

  constexpr bool valid() const { return emit != nullptr; }
};

struct PdfSemanticWriterWorkspace {
  uint8_t* output = nullptr;
  size_t outputCapacity = 0;
};

PdfStatus pdfFormatSemanticAnchor(uint32_t ordinal, char destination[PdfSemanticWriterLimits::AnchorBytes]);
PdfStatus pdfTruncatePublisherLabel(const uint8_t* source, size_t sourceLength,
                                    char destination[PdfSemanticWriterLimits::PublisherLabelBytes],
                                    size_t* destinationLength);

class PdfSemanticWriter {
 public:
  PdfStatus begin(PdfByteSink output, PdfSemanticBlockSink blockSink, PdfSemanticWriterWorkspace workspace,
                  uint32_t initialWords = 0);
  PdfStatus beginBlock(const PdfSemanticBlock& block);
  PdfStatus writeText(const uint8_t* text, size_t length);
  PdfStatus writeRetainedImage(const uint8_t* resource, size_t length, uint16_t width, uint16_t height);
  PdfStatus beginInternalLink(const uint8_t* href, size_t length);
  PdfStatus endInternalLink();
  PdfStatus endBlock();

  PdfStatus beginTable();
  PdfStatus beginTableRow();
  PdfStatus endTableRow();
  PdfStatus endTable();
  PdfStatus writePublisherPageBreak(const uint8_t* label, size_t length);
  PdfStatus writePublisherPageBreak(uint32_t sourcePageIndex, const uint8_t* label, size_t length);

  PdfStatus flush();
  PdfStatus finish();

  uint32_t totalWords() const { return totalWords_; }
  bool blockOpen() const { return blockOpen_; }

 private:
  PdfStatus append(const uint8_t* bytes, size_t length);
  PdfStatus appendLiteral(const char* literal);
  PdfStatus appendEscaped(const uint8_t* bytes, size_t length, bool attribute);
  PdfStatus flushBuffer();
  PdfStatus validateUtf8(const uint8_t* bytes, size_t length) const;
  PdfStatus fail(PdfStatus status);

  PdfByteSink output_{};
  PdfSemanticBlockSink blockSink_{};
  PdfSemanticWriterWorkspace workspace_{};
  PdfWordCounter wordCounter_{};
  PdfStatus status_{};
  size_t outputLength_ = 0;
  uint32_t totalWords_ = 0;
  uint32_t currentAnchorOrdinal_ = 0;
  uint32_t currentWordStart_ = 0;
  uint32_t lastAnchorOrdinal_ = 0;
  PdfSemanticBlockKind currentKind_ = PdfSemanticBlockKind::Paragraph;
  uint8_t currentHeadingLevel_ = 0;
  bool initialized_ = false;
  bool finished_ = false;
  bool blockOpen_ = false;
  bool linkOpen_ = false;
  bool tableOpen_ = false;
  bool tableRowOpen_ = false;
  bool hasLastAnchor_ = false;
};

static_assert(sizeof(PdfSemanticWriter) <= 192);
