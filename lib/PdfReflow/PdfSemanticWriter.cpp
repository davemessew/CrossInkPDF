#include "PdfSemanticWriter.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <limits>

#include "PdfIo.h"
#include "PdfUnicode.h"

namespace {

static constexpr char DOCUMENT_BEGIN[] =
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
    "<html xmlns=\"http://www.w3.org/1999/xhtml\"><head><meta charset=\"UTF-8\"/></head><body>";
static constexpr char DOCUMENT_END[] = "</body></html>";
static constexpr char HEX_DIGITS[] = "0123456789abcdef";

bool isXmlScalar(const uint32_t scalar) {
  return scalar == 0x09 || scalar == 0x0A || scalar == 0x0D || (scalar >= 0x20 && scalar <= 0xD7FF) ||
         (scalar >= 0xE000 && scalar <= 0xFFFD) || (scalar >= 0x10000 && scalar <= 0x10FFFF);
}

PdfStatus validateUtf8Value(const uint8_t* const bytes, const size_t length) {
  if (bytes == nullptr && length != 0) {
    return PdfStatus::failure(PdfError::InvalidArgument);
  }
  size_t offset = 0;
  while (offset < length) {
    uint32_t scalar = 0;
    const PdfStatus status = pdfDecodeUtf8Scalar(bytes, length, &offset, &scalar);
    if (!status.ok()) {
      return status;
    }
    if (!isXmlScalar(scalar)) {
      return PdfStatus::failure(PdfError::Malformed, offset);
    }
  }
  return PdfStatus::success();
}

}  // namespace

PdfStatus pdfFormatSemanticAnchor(const uint32_t ordinal, char destination[PdfSemanticWriterLimits::AnchorBytes]) {
  if (destination == nullptr) {
    return PdfStatus::failure(PdfError::InvalidArgument, ordinal);
  }
  destination[0] = 'b';
  for (uint8_t index = 0; index < 8; ++index) {
    const uint8_t shift = static_cast<uint8_t>((7 - index) * 4);
    destination[index + 1] = HEX_DIGITS[(ordinal >> shift) & 0x0FU];
  }
  destination[9] = '\0';
  return PdfStatus::success();
}

PdfStatus pdfTruncatePublisherLabel(const uint8_t* const source, const size_t sourceLength,
                                    char destination[PdfSemanticWriterLimits::PublisherLabelBytes],
                                    size_t* const destinationLength) {
  if ((source == nullptr && sourceLength != 0) || destination == nullptr || destinationLength == nullptr) {
    return PdfStatus::failure(PdfError::InvalidArgument);
  }
  size_t sourceOffset = 0;
  size_t outputLength = 0;
  bool full = false;
  while (sourceOffset < sourceLength) {
    const size_t scalarOffset = sourceOffset;
    uint32_t scalar = 0;
    const PdfStatus status = pdfDecodeUtf8Scalar(source, sourceLength, &sourceOffset, &scalar);
    if (!status.ok()) {
      return status;
    }
    if (!isXmlScalar(scalar)) {
      return PdfStatus::failure(PdfError::Malformed, scalarOffset);
    }
    const size_t scalarBytes = sourceOffset - scalarOffset;
    if (!full && scalarBytes <= PdfSemanticWriterLimits::PublisherLabelBytes - 1 - outputLength) {
      std::memcpy(destination + outputLength, source + scalarOffset, scalarBytes);
      outputLength += scalarBytes;
    } else {
      full = true;
    }
  }
  destination[outputLength] = '\0';
  *destinationLength = outputLength;
  return PdfStatus::success();
}

PdfStatus PdfSemanticWriter::fail(const PdfStatus status) {
  if (status_.ok()) {
    status_ = status;
  }
  return status_;
}

PdfStatus PdfSemanticWriter::flushBuffer() {
  if (outputLength_ == 0) {
    return PdfStatus::success();
  }
  const PdfStatus status = pdfWriteExact(output_, workspace_.output, outputLength_);
  if (!status.ok()) {
    return fail(status);
  }
  outputLength_ = 0;
  return PdfStatus::success();
}

PdfStatus PdfSemanticWriter::append(const uint8_t* const bytes, const size_t length) {
  if (!status_.ok()) {
    return status_;
  }
  if ((bytes == nullptr && length != 0) || !initialized_ || finished_) {
    return fail(PdfStatus::failure(PdfError::InvalidArgument));
  }
  size_t offset = 0;
  while (offset < length) {
    if (outputLength_ == workspace_.outputCapacity) {
      const PdfStatus status = flushBuffer();
      if (!status.ok()) {
        return status;
      }
    }
    const size_t count = std::min(length - offset, workspace_.outputCapacity - outputLength_);
    std::memcpy(workspace_.output + outputLength_, bytes + offset, count);
    outputLength_ += count;
    offset += count;
  }
  return PdfStatus::success();
}

PdfStatus PdfSemanticWriter::appendLiteral(const char* const literal) {
  if (literal == nullptr) {
    return fail(PdfStatus::failure(PdfError::InvalidArgument));
  }
  return append(reinterpret_cast<const uint8_t*>(literal), std::strlen(literal));
}

PdfStatus PdfSemanticWriter::appendEscaped(const uint8_t* const bytes, const size_t length, const bool attribute) {
  size_t start = 0;
  for (size_t index = 0; index < length; ++index) {
    const char* replacement = nullptr;
    switch (bytes[index]) {
      case '&':
        replacement = "&amp;";
        break;
      case '<':
        replacement = "&lt;";
        break;
      case '>':
        replacement = "&gt;";
        break;
      case '"':
        replacement = attribute ? "&quot;" : nullptr;
        break;
      case '\'':
        replacement = attribute ? "&apos;" : nullptr;
        break;
      default:
        break;
    }
    if (replacement == nullptr) {
      continue;
    }
    PdfStatus status = append(bytes + start, index - start);
    if (!status.ok()) {
      return status;
    }
    status = appendLiteral(replacement);
    if (!status.ok()) {
      return status;
    }
    start = index + 1;
  }
  return append(bytes + start, length - start);
}

PdfStatus PdfSemanticWriter::validateUtf8(const uint8_t* const bytes, const size_t length) const {
  return validateUtf8Value(bytes, length);
}

PdfStatus PdfSemanticWriter::begin(const PdfByteSink output, const PdfSemanticBlockSink blockSink,
                                   const PdfSemanticWriterWorkspace workspace, const uint32_t initialWords) {
  output_ = output;
  blockSink_ = blockSink;
  workspace_ = workspace;
  status_ = PdfStatus::success();
  outputLength_ = 0;
  totalWords_ = initialWords;
  currentAnchorOrdinal_ = 0;
  currentWordStart_ = initialWords;
  lastAnchorOrdinal_ = 0;
  currentKind_ = PdfSemanticBlockKind::Paragraph;
  currentHeadingLevel_ = 0;
  initialized_ = output_.valid() && blockSink_.valid() && workspace_.output != nullptr &&
                 workspace_.outputCapacity >= PdfSemanticWriterLimits::MinimumOutputBufferBytes;
  finished_ = false;
  blockOpen_ = false;
  linkOpen_ = false;
  tableOpen_ = false;
  tableRowOpen_ = false;
  hasLastAnchor_ = false;
  if (!initialized_) {
    return fail(PdfStatus::failure(PdfError::InvalidArgument));
  }
  wordCounter_.reset();
  return appendLiteral(DOCUMENT_BEGIN);
}

PdfStatus PdfSemanticWriter::beginBlock(const PdfSemanticBlock& block) {
  if (!status_.ok()) {
    return status_;
  }
  const bool headingValid =
      block.kind != PdfSemanticBlockKind::Heading || (block.headingLevel >= 1 && block.headingLevel <= 6);
  const bool tableCellValid = block.kind != PdfSemanticBlockKind::TableCell || (tableOpen_ && tableRowOpen_);
  const bool ordinaryBlockValid = block.kind == PdfSemanticBlockKind::TableCell || (!tableOpen_ && !tableRowOpen_);
  if (!initialized_ || finished_ || blockOpen_ || linkOpen_ || !headingValid || !tableCellValid ||
      !ordinaryBlockValid) {
    return fail(PdfStatus::failure(PdfError::InvalidArgument, block.anchorOrdinal));
  }
  if (hasLastAnchor_ && block.anchorOrdinal <= lastAnchorOrdinal_) {
    return fail(PdfStatus::failure(PdfError::Malformed, block.anchorOrdinal));
  }
  char anchor[PdfSemanticWriterLimits::AnchorBytes]{};
  PdfStatus status = pdfFormatSemanticAnchor(block.anchorOrdinal, anchor);
  if (!status.ok()) {
    return fail(status);
  }
  status = wordCounter_.reset();
  if (!status.ok()) {
    return fail(status);
  }
  if (block.kind == PdfSemanticBlockKind::Paragraph) {
    status = appendLiteral("<p id=\"");
  } else if (block.kind == PdfSemanticBlockKind::TableCell) {
    status = appendLiteral("<td id=\"");
  } else {
    char headingBegin[4] = {'<', 'h', static_cast<char>('0' + block.headingLevel), '\0'};
    status = appendLiteral(headingBegin);
    if (status.ok()) {
      status = appendLiteral(" id=\"");
    }
  }
  if (status.ok()) {
    status = appendLiteral(anchor);
  }
  if (status.ok()) {
    status = appendLiteral("\">");
  }
  if (!status.ok()) {
    return status;
  }
  currentAnchorOrdinal_ = block.anchorOrdinal;
  currentWordStart_ = totalWords_;
  currentKind_ = block.kind;
  currentHeadingLevel_ = block.headingLevel;
  blockOpen_ = true;
  return PdfStatus::success();
}

PdfStatus PdfSemanticWriter::writeText(const uint8_t* const text, const size_t length) {
  if (!status_.ok()) {
    return status_;
  }
  if (!blockOpen_ || (text == nullptr && length != 0)) {
    return fail(PdfStatus::failure(PdfError::InvalidArgument));
  }
  PdfStatus status = wordCounter_.consume(text, length);
  if (!status.ok()) {
    return fail(status);
  }
  status = appendEscaped(text, length, false);
  return status.ok() ? status : fail(status);
}

PdfStatus PdfSemanticWriter::beginInternalLink(const uint8_t* const href, const size_t length) {
  if (!status_.ok()) {
    return status_;
  }
  if (!blockOpen_ || linkOpen_ || href == nullptr || length == 0) {
    return fail(PdfStatus::failure(PdfError::InvalidArgument));
  }
  PdfStatus status = validateUtf8(href, length);
  if (!status.ok()) {
    return fail(status);
  }
  status = appendLiteral("<a href=\"");
  if (status.ok()) {
    status = appendEscaped(href, length, true);
  }
  if (status.ok()) {
    status = appendLiteral("\">");
  }
  if (!status.ok()) {
    return status;
  }
  linkOpen_ = true;
  return PdfStatus::success();
}

PdfStatus PdfSemanticWriter::endInternalLink() {
  if (!status_.ok()) {
    return status_;
  }
  if (!blockOpen_ || !linkOpen_) {
    return fail(PdfStatus::failure(PdfError::InvalidArgument));
  }
  const PdfStatus status = appendLiteral("</a>");
  if (status.ok()) {
    linkOpen_ = false;
  }
  return status;
}

PdfStatus PdfSemanticWriter::endBlock() {
  if (!status_.ok()) {
    return status_;
  }
  if (!blockOpen_ || linkOpen_) {
    return fail(PdfStatus::failure(PdfError::InvalidArgument));
  }
  PdfStatus status = wordCounter_.finish();
  if (!status.ok()) {
    return fail(status);
  }
  const uint32_t blockWords = wordCounter_.words();
  if (blockWords > std::numeric_limits<uint32_t>::max() - totalWords_) {
    return fail(PdfStatus::failure(PdfError::LimitExceeded, currentAnchorOrdinal_));
  }
  if (currentKind_ == PdfSemanticBlockKind::Paragraph) {
    status = appendLiteral("</p>");
  } else if (currentKind_ == PdfSemanticBlockKind::TableCell) {
    status = appendLiteral("</td>");
  } else {
    char headingEnd[6] = {'<', '/', 'h', static_cast<char>('0' + currentHeadingLevel_), '>', '\0'};
    status = appendLiteral(headingEnd);
  }
  if (!status.ok()) {
    return status;
  }
  PdfSemanticBlockRecord record{};
  record.anchorOrdinal = currentAnchorOrdinal_;
  record.cumulativeWordStart = currentWordStart_;
  record.wordCount = blockWords;
  status = pdfFormatSemanticAnchor(currentAnchorOrdinal_, record.anchor);
  if (status.ok()) {
    status = blockSink_.emit(blockSink_.context, record);
  }
  if (!status.ok()) {
    return fail(status);
  }
  totalWords_ += blockWords;
  lastAnchorOrdinal_ = currentAnchorOrdinal_;
  hasLastAnchor_ = true;
  blockOpen_ = false;
  currentHeadingLevel_ = 0;
  return PdfStatus::success();
}

PdfStatus PdfSemanticWriter::beginTable() {
  if (!status_.ok()) {
    return status_;
  }
  if (!initialized_ || finished_ || blockOpen_ || linkOpen_ || tableOpen_ || tableRowOpen_) {
    return fail(PdfStatus::failure(PdfError::InvalidArgument));
  }
  const PdfStatus status = appendLiteral("<table><tbody>");
  if (status.ok()) {
    tableOpen_ = true;
  }
  return status;
}

PdfStatus PdfSemanticWriter::beginTableRow() {
  if (!status_.ok()) {
    return status_;
  }
  if (!tableOpen_ || tableRowOpen_ || blockOpen_ || linkOpen_) {
    return fail(PdfStatus::failure(PdfError::InvalidArgument));
  }
  const PdfStatus status = appendLiteral("<tr>");
  if (status.ok()) {
    tableRowOpen_ = true;
  }
  return status;
}

PdfStatus PdfSemanticWriter::endTableRow() {
  if (!status_.ok()) {
    return status_;
  }
  if (!tableOpen_ || !tableRowOpen_ || blockOpen_ || linkOpen_) {
    return fail(PdfStatus::failure(PdfError::InvalidArgument));
  }
  const PdfStatus status = appendLiteral("</tr>");
  if (status.ok()) {
    tableRowOpen_ = false;
  }
  return status;
}

PdfStatus PdfSemanticWriter::endTable() {
  if (!status_.ok()) {
    return status_;
  }
  if (!tableOpen_ || tableRowOpen_ || blockOpen_ || linkOpen_) {
    return fail(PdfStatus::failure(PdfError::InvalidArgument));
  }
  const PdfStatus status = appendLiteral("</tbody></table>");
  if (status.ok()) {
    tableOpen_ = false;
  }
  return status;
}

PdfStatus PdfSemanticWriter::writePublisherPageBreak(const uint8_t* const label, const size_t length) {
  return writePublisherPageBreak(UINT32_MAX, label, length);
}

PdfStatus PdfSemanticWriter::writePublisherPageBreak(const uint32_t sourcePageIndex, const uint8_t* const label,
                                                     const size_t length) {
  if (!status_.ok()) {
    return status_;
  }
  if (!initialized_ || finished_ || blockOpen_ || linkOpen_ || tableOpen_ || tableRowOpen_ ||
      (label == nullptr && length != 0)) {
    return fail(PdfStatus::failure(PdfError::InvalidArgument));
  }
  char truncated[PdfSemanticWriterLimits::PublisherLabelBytes]{};
  size_t truncatedLength = 0;
  PdfStatus status = pdfTruncatePublisherLabel(label, length, truncated, &truncatedLength);
  if (!status.ok()) {
    return fail(status);
  }
  status = appendLiteral("<span");
  if (status.ok() && sourcePageIndex != UINT32_MAX) {
    char pageAnchor[11]{};
    const int written =
        std::snprintf(pageAnchor, sizeof(pageAnchor), "p%08lx", static_cast<unsigned long>(sourcePageIndex));
    if (written != 9) {
      return fail(PdfStatus::failure(PdfError::LimitExceeded, sourcePageIndex));
    }
    status = appendLiteral(" id=\"");
    if (status.ok()) {
      status = appendLiteral(pageAnchor);
    }
    if (status.ok()) {
      status = appendLiteral("\"");
    }
  }
  if (status.ok()) {
    status = appendLiteral(" role=\"doc-pagebreak\" aria-label=\"");
  }
  if (status.ok()) {
    status = appendEscaped(reinterpret_cast<const uint8_t*>(truncated), truncatedLength, true);
  }
  if (status.ok()) {
    status = appendLiteral("\"></span>");
  }
  return status.ok() ? status : fail(status);
}

PdfStatus PdfSemanticWriter::flush() {
  if (!status_.ok()) {
    return status_;
  }
  if (!initialized_ || finished_) {
    return fail(PdfStatus::failure(PdfError::InvalidArgument));
  }
  return flushBuffer();
}

PdfStatus PdfSemanticWriter::finish() {
  if (!status_.ok()) {
    return status_;
  }
  if (!initialized_ || finished_ || blockOpen_ || linkOpen_ || tableOpen_ || tableRowOpen_) {
    return fail(PdfStatus::failure(PdfError::InvalidArgument));
  }
  PdfStatus status = appendLiteral(DOCUMENT_END);
  if (status.ok()) {
    status = flushBuffer();
  }
  if (status.ok()) {
    finished_ = true;
  }
  return status;
}
