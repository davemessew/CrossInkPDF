#include "PdfLexer.h"

#include <algorithm>
#include <cstring>

#include "PdfLimits.h"

namespace {

bool isWhitespace(const uint8_t byte) {
  return byte == 0 || byte == '\t' || byte == '\n' || byte == '\f' || byte == '\r' || byte == ' ';
}

bool isDelimiter(const uint8_t byte) {
  switch (byte) {
    case '(':
    case ')':
    case '<':
    case '>':
    case '[':
    case ']':
    case '{':
    case '}':
    case '/':
    case '%':
      return true;
    default:
      return false;
  }
}

int hexValue(const uint8_t byte) {
  if (byte >= '0' && byte <= '9') {
    return byte - '0';
  }
  if (byte >= 'A' && byte <= 'F') {
    return byte - 'A' + 10;
  }
  if (byte >= 'a' && byte <= 'f') {
    return byte - 'a' + 10;
  }
  return -1;
}

PdfTokenKind classifyWord(const PdfToken& token) {
  size_t position = 0;
  if (token.length != 0 && (token.bytes[0] == '+' || token.bytes[0] == '-')) {
    position = 1;
  }
  bool sawDigit = false;
  bool sawDecimal = false;
  for (; position < token.length; ++position) {
    const char byte = token.bytes[position];
    if (byte >= '0' && byte <= '9') {
      sawDigit = true;
      continue;
    }
    if (byte == '.' && !sawDecimal) {
      sawDecimal = true;
      continue;
    }
    return PdfTokenKind::Keyword;
  }
  if (!sawDigit) {
    return PdfTokenKind::Keyword;
  }
  return sawDecimal ? PdfTokenKind::Real : PdfTokenKind::Integer;
}

}  // namespace

PdfLexer::PdfLexer(const PdfByteSource& source, uint8_t* sourceBuffer, const size_t sourceBufferSize)
    : source_(source), sourceBuffer_(sourceBuffer), sourceBufferSize_(sourceBufferSize) {}

void PdfLexer::setSource(const PdfByteSource& source, const uint64_t offset) {
  source_ = source;
  reset(offset);
}

void PdfLexer::reset(const uint64_t offset) {
  bufferedBytes_ = 0;
  bufferPosition_ = 0;
  position_ = offset;
  lastTokenOffset_ = offset;
  pendingTokenOffset_ = offset;
  pendingToken_ = {};
  unreadToken_ = {};
  mode_ = Mode::Idle;
  literalDepth_ = 0;
  hexNibble_ = 0;
  octalValue_ = 0;
  octalDigits_ = 0;
  hasHexNibble_ = false;
  operationCharged_ = false;
  hasUnreadToken_ = false;
  inlineSkipActive_ = false;
  inlineSkipState_ = InlineSkipState::NeedSeparator;
  inlineBytesScanned_ = 0;
}

PdfStepResult PdfLexer::next(PdfToken& token, PdfWorkBudget& budget, uint8_t* const stringBuffer,
                             const size_t stringBufferSize) {
  token = {};
  if (!source_.valid() || sourceBuffer_ == nullptr || sourceBufferSize_ == 0 ||
      sourceBufferSize_ > PdfLimits::SourceBufferBytes) {
    return PdfStepResult::failure(PdfStatus::failure(PdfError::InvalidArgument, position_));
  }
  if (position_ > source_.size) {
    return PdfStepResult::failure(PdfStatus::failure(PdfError::InvalidOffset, position_));
  }

  if (!operationCharged_) {
    if (budget.cancelRequested()) {
      return PdfStepResult::failure(PdfStatus::failure(PdfError::Cancelled, position_));
    }
    if (!budget.consumeOperation()) {
      return PdfStepResult::paused();
    }
    operationCharged_ = true;
  }

  if (hasUnreadToken_) {
    token = unreadToken_;
    lastTokenOffset_ = unreadTokenOffset_;
    hasUnreadToken_ = false;
    operationCharged_ = false;
    return PdfStepResult::completed();
  }

  while (true) {
    uint8_t byte = 0;
    PdfStatus status;
    const ByteResult byteResult = peek(byte, budget, status);
    if (byteResult == ByteResult::Yielded) {
      return PdfStepResult::paused();
    }
    if (byteResult == ByteResult::Failed) {
      operationCharged_ = false;
      return PdfStepResult::failure(status);
    }

    if (mode_ == Mode::Idle) {
      if (byteResult == ByteResult::End) {
        startToken(position_);
        return finish(token, PdfTokenKind::End);
      }
      if (isWhitespace(byte)) {
        consume();
        continue;
      }
      if (byte == '%') {
        consume();
        mode_ = Mode::Comment;
        continue;
      }

      startToken(position_);
      consume();
      switch (byte) {
        case '[':
          return finish(token, PdfTokenKind::ArrayBegin);
        case ']':
          return finish(token, PdfTokenKind::ArrayEnd);
        case '/':
          mode_ = Mode::Name;
          continue;
        case '(':
          mode_ = Mode::Literal;
          literalDepth_ = 1;
          continue;
        case '<':
          mode_ = Mode::LessThan;
          continue;
        case '>':
          mode_ = Mode::GreaterThan;
          continue;
        default:
          mode_ = Mode::Word;
          if (!append(byte, status)) {
            operationCharged_ = false;
            return PdfStepResult::failure(status);
          }
          continue;
      }
    }

    if (mode_ == Mode::Comment) {
      if (byteResult == ByteResult::End) {
        mode_ = Mode::Idle;
        continue;
      }
      consume();
      if (byte == '\r' || byte == '\n') {
        mode_ = Mode::Idle;
      }
      continue;
    }

    if (mode_ == Mode::Word) {
      if (byteResult == ByteResult::End || isWhitespace(byte) || isDelimiter(byte)) {
        return finish(token, classifyWord(pendingToken_));
      }
      consume();
      if (!append(byte, status)) {
        operationCharged_ = false;
        return PdfStepResult::failure(status);
      }
      continue;
    }

    if (mode_ == Mode::Name) {
      if (byteResult == ByteResult::End || isWhitespace(byte) || isDelimiter(byte)) {
        return finish(token, PdfTokenKind::Name);
      }
      consume();
      if (byte == '#') {
        mode_ = Mode::NameHexFirst;
        continue;
      }
      if (!append(byte, status)) {
        operationCharged_ = false;
        return PdfStepResult::failure(status);
      }
      continue;
    }

    if (mode_ == Mode::NameHexFirst || mode_ == Mode::NameHexSecond) {
      if (byteResult == ByteResult::End) {
        operationCharged_ = false;
        return PdfStepResult::failure(PdfStatus::failure(PdfError::UnexpectedEof, position_));
      }
      const int value = hexValue(byte);
      if (value < 0) {
        operationCharged_ = false;
        return PdfStepResult::failure(PdfStatus::failure(PdfError::Malformed, position_));
      }
      consume();
      if (mode_ == Mode::NameHexFirst) {
        hexNibble_ = static_cast<uint8_t>(value);
        mode_ = Mode::NameHexSecond;
        continue;
      }
      if (!append(static_cast<uint8_t>((hexNibble_ << 4) | value), status)) {
        operationCharged_ = false;
        return PdfStepResult::failure(status);
      }
      mode_ = Mode::Name;
      continue;
    }

    if (mode_ == Mode::LessThan) {
      if (byteResult == ByteResult::End) {
        operationCharged_ = false;
        return PdfStepResult::failure(PdfStatus::failure(PdfError::UnexpectedEof, position_));
      }
      if (byte == '<') {
        consume();
        return finish(token, PdfTokenKind::DictionaryBegin);
      }
      mode_ = Mode::HexString;
      hasHexNibble_ = false;
      continue;
    }

    if (mode_ == Mode::GreaterThan) {
      if (byteResult == ByteResult::End) {
        operationCharged_ = false;
        return PdfStepResult::failure(PdfStatus::failure(PdfError::UnexpectedEof, position_));
      }
      if (byte != '>') {
        operationCharged_ = false;
        return PdfStepResult::failure(PdfStatus::failure(PdfError::Malformed, pendingTokenOffset_));
      }
      consume();
      return finish(token, PdfTokenKind::DictionaryEnd);
    }

    if (mode_ == Mode::Literal) {
      if (byteResult == ByteResult::End) {
        operationCharged_ = false;
        return PdfStepResult::failure(PdfStatus::failure(PdfError::UnexpectedEof, position_));
      }
      consume();
      if (byte == '\\') {
        mode_ = Mode::LiteralEscape;
        continue;
      }
      if (byte == '(') {
        if (literalDepth_ >= PdfLimits::MaxContainerNesting) {
          operationCharged_ = false;
          return PdfStepResult::failure(PdfStatus::failure(PdfError::LimitExceeded, position_ - 1));
        }
        ++literalDepth_;
        if (!appendString(byte, stringBuffer, stringBufferSize, status)) {
          operationCharged_ = false;
          return PdfStepResult::failure(status);
        }
        continue;
      }
      if (byte == ')') {
        --literalDepth_;
        if (literalDepth_ == 0) {
          return finish(token, PdfTokenKind::String);
        }
        if (!appendString(byte, stringBuffer, stringBufferSize, status)) {
          operationCharged_ = false;
          return PdfStepResult::failure(status);
        }
        continue;
      }
      if (!appendString(byte, stringBuffer, stringBufferSize, status)) {
        operationCharged_ = false;
        return PdfStepResult::failure(status);
      }
      continue;
    }

    if (mode_ == Mode::LiteralEscape) {
      if (byteResult == ByteResult::End) {
        operationCharged_ = false;
        return PdfStepResult::failure(PdfStatus::failure(PdfError::UnexpectedEof, position_));
      }
      consume();
      uint8_t decoded = byte;
      switch (byte) {
        case 'n':
          decoded = '\n';
          break;
        case 'r':
          decoded = '\r';
          break;
        case 't':
          decoded = '\t';
          break;
        case 'b':
          decoded = '\b';
          break;
        case 'f':
          decoded = '\f';
          break;
        case '\n':
          mode_ = Mode::Literal;
          continue;
        case '\r': {
          uint8_t following = 0;
          const ByteResult followingResult = peek(following, budget, status);
          if (followingResult == ByteResult::Yielded) {
            return PdfStepResult::paused();
          }
          if (followingResult == ByteResult::Failed) {
            operationCharged_ = false;
            return PdfStepResult::failure(status);
          }
          if (followingResult == ByteResult::Available && following == '\n') {
            consume();
          }
          mode_ = Mode::Literal;
          continue;
        }
        default:
          if (byte >= '0' && byte <= '7') {
            octalValue_ = static_cast<uint8_t>(byte - '0');
            octalDigits_ = 1;
            mode_ = Mode::LiteralOctal;
            continue;
          }
          break;
      }
      if (!appendString(decoded, stringBuffer, stringBufferSize, status)) {
        operationCharged_ = false;
        return PdfStepResult::failure(status);
      }
      mode_ = Mode::Literal;
      continue;
    }

    if (mode_ == Mode::LiteralOctal) {
      if (byteResult == ByteResult::Available && byte >= '0' && byte <= '7' && octalDigits_ < 3) {
        consume();
        octalValue_ = static_cast<uint8_t>((octalValue_ << 3) | (byte - '0'));
        ++octalDigits_;
        if (octalDigits_ < 3) {
          continue;
        }
      }
      if (!appendString(octalValue_, stringBuffer, stringBufferSize, status)) {
        operationCharged_ = false;
        return PdfStepResult::failure(status);
      }
      mode_ = Mode::Literal;
      continue;
    }

    if (mode_ == Mode::HexString) {
      if (byteResult == ByteResult::End) {
        operationCharged_ = false;
        return PdfStepResult::failure(PdfStatus::failure(PdfError::UnexpectedEof, position_));
      }
      if (isWhitespace(byte)) {
        consume();
        continue;
      }
      if (byte == '>') {
        consume();
        if (hasHexNibble_) {
          if (!appendString(static_cast<uint8_t>(hexNibble_ << 4), stringBuffer, stringBufferSize, status)) {
            operationCharged_ = false;
            return PdfStepResult::failure(status);
          }
          hasHexNibble_ = false;
        }
        return finish(token, PdfTokenKind::HexString);
      }
      const int value = hexValue(byte);
      if (value < 0) {
        operationCharged_ = false;
        return PdfStepResult::failure(PdfStatus::failure(PdfError::Malformed, position_));
      }
      consume();
      if (!hasHexNibble_) {
        hexNibble_ = static_cast<uint8_t>(value);
        hasHexNibble_ = true;
        continue;
      }
      if (!appendString(static_cast<uint8_t>((hexNibble_ << 4) | value), stringBuffer, stringBufferSize, status)) {
        operationCharged_ = false;
        return PdfStepResult::failure(status);
      }
      hasHexNibble_ = false;
      continue;
    }
  }
}

PdfStepResult PdfLexer::skipInlineImageData(PdfWorkBudget& budget) {
  if (!source_.valid() || sourceBuffer_ == nullptr || sourceBufferSize_ == 0 ||
      sourceBufferSize_ > PdfLimits::SourceBufferBytes || mode_ != Mode::Idle || hasUnreadToken_) {
    return PdfStepResult::failure(PdfStatus::failure(PdfError::InvalidArgument, position_));
  }
  if (!inlineSkipActive_) {
    inlineSkipActive_ = true;
    inlineSkipState_ = InlineSkipState::NeedSeparator;
    inlineBytesScanned_ = 0;
  }
  if (!budget.consumeOperation()) {
    return PdfStepResult::paused();
  }

  uint16_t scannedThisStep = 0;
  while (scannedThisStep < 256) {
    uint8_t byte = 0;
    PdfStatus status;
    const ByteResult result = peek(byte, budget, status);
    if (result == ByteResult::Yielded) {
      return PdfStepResult::paused();
    }
    if (result == ByteResult::Failed) {
      inlineSkipActive_ = false;
      return PdfStepResult::failure(status);
    }
    if (result == ByteResult::End) {
      inlineSkipActive_ = false;
      return PdfStepResult::failure(PdfStatus::failure(PdfError::Malformed, position_));
    }
    consume();
    ++scannedThisStep;
    ++inlineBytesScanned_;
    if (inlineBytesScanned_ > PdfLimits::MaxExpandedRequiredStreamBytes) {
      inlineSkipActive_ = false;
      return PdfStepResult::failure(PdfStatus::failure(PdfError::ExpansionLimit, position_));
    }

    switch (inlineSkipState_) {
      case InlineSkipState::NeedSeparator:
        if (!isWhitespace(byte)) {
          inlineSkipActive_ = false;
          return PdfStepResult::failure(PdfStatus::failure(PdfError::Malformed, position_ - 1));
        }
        inlineSkipState_ = InlineSkipState::Data;
        break;
      case InlineSkipState::Data:
        if (isWhitespace(byte)) {
          inlineSkipState_ = InlineSkipState::Whitespace;
        }
        break;
      case InlineSkipState::Whitespace:
        if (byte == 'E') {
          inlineSkipState_ = InlineSkipState::WhitespaceE;
        } else if (!isWhitespace(byte)) {
          inlineSkipState_ = InlineSkipState::Data;
        }
        break;
      case InlineSkipState::WhitespaceE:
        if (byte == 'I') {
          inlineSkipState_ = InlineSkipState::WhitespaceEI;
        } else {
          inlineSkipState_ = isWhitespace(byte) ? InlineSkipState::Whitespace : InlineSkipState::Data;
        }
        break;
      case InlineSkipState::WhitespaceEI:
        if (isWhitespace(byte)) {
          inlineSkipActive_ = false;
          inlineSkipState_ = InlineSkipState::NeedSeparator;
          return PdfStepResult::completed();
        }
        inlineSkipState_ = InlineSkipState::Data;
        break;
    }
  }
  return PdfStepResult::paused();
}

bool PdfLexer::unread(const PdfToken& token) {
  if (hasUnreadToken_ || operationCharged_ || mode_ != Mode::Idle) {
    return false;
  }
  unreadToken_ = token;
  unreadTokenOffset_ = lastTokenOffset_;
  hasUnreadToken_ = true;
  return true;
}

bool PdfLexer::bufferedRange(const uint64_t sourceOffset, size_t* const bufferOffset, size_t* const length) const {
  if (bufferOffset == nullptr || length == nullptr || position_ < bufferPosition_) {
    return false;
  }
  const uint64_t bufferSourceOffset = position_ - bufferPosition_;
  if (sourceOffset < bufferSourceOffset || sourceOffset - bufferSourceOffset > bufferedBytes_) {
    return false;
  }
  *bufferOffset = static_cast<size_t>(sourceOffset - bufferSourceOffset);
  *length = bufferedBytes_ - *bufferOffset;
  return true;
}

PdfLexer::ByteResult PdfLexer::peek(uint8_t& byte, PdfWorkBudget& budget, PdfStatus& status) {
  if (bufferPosition_ < bufferedBytes_) {
    byte = sourceBuffer_[bufferPosition_];
    return ByteResult::Available;
  }
  if (position_ >= source_.size) {
    return ByteResult::End;
  }
  if (budget.cancelRequested()) {
    status = PdfStatus::failure(PdfError::Cancelled, position_);
    return ByteResult::Failed;
  }
  const uint64_t remaining64 = source_.size - position_;
  const size_t remaining = remaining64 > static_cast<uint64_t>(SIZE_MAX) ? SIZE_MAX : static_cast<size_t>(remaining64);
  const size_t desired = std::min({sourceBufferSize_, PdfLimits::SourceBufferBytes, remaining});
  const size_t requested = budget.takeBytes(desired);
  if (requested == 0) {
    return ByteResult::Yielded;
  }

  size_t bytesRead = 0;
  status = source_.readAt(source_.context, position_, sourceBuffer_, requested, &bytesRead);
  if (!status.ok()) {
    return ByteResult::Failed;
  }
  if (bytesRead > requested) {
    status = PdfStatus::failure(PdfError::IoFailure, position_);
    return ByteResult::Failed;
  }
  if (bytesRead == 0) {
    status = PdfStatus::failure(PdfError::UnexpectedEof, position_);
    return ByteResult::Failed;
  }
  bufferedBytes_ = bytesRead;
  bufferPosition_ = 0;
  byte = sourceBuffer_[0];
  return ByteResult::Available;
}

void PdfLexer::consume() {
  ++bufferPosition_;
  ++position_;
}

bool PdfLexer::append(const uint8_t byte, PdfStatus& status) {
  if (pendingToken_.length >= sizeof(pendingToken_.bytes)) {
    status = PdfStatus::failure(PdfError::LimitExceeded, position_);
    return false;
  }
  pendingToken_.bytes[pendingToken_.length++] = static_cast<char>(byte);
  return true;
}

bool PdfLexer::appendString(const uint8_t byte, uint8_t* const stringBuffer, const size_t stringBufferSize,
                            PdfStatus& status) {
  if (stringBuffer == nullptr) {
    return append(byte, status);
  }
  pendingToken_.reserved[0] = 1;
  if (pendingToken_.length < stringBufferSize) {
    stringBuffer[pendingToken_.length++] = byte;
  } else {
    pendingToken_.reserved[1] = 1;
  }
  return true;
}

PdfStepResult PdfLexer::finish(PdfToken& token, const PdfTokenKind kind) {
  token = pendingToken_;
  token.kind = kind;
  lastTokenOffset_ = pendingTokenOffset_;
  mode_ = Mode::Idle;
  operationCharged_ = false;
  return PdfStepResult::completed();
}

void PdfLexer::startToken(const uint64_t offset) {
  pendingToken_ = {};
  pendingTokenOffset_ = offset;
}
