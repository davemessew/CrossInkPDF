#pragma once

#include <cstddef>
#include <cstdint>

#include "PdfTypes.h"
#include "PdfWorkBudget.h"

class PdfLexer {
 public:
  PdfLexer(const PdfByteSource& source, uint8_t* sourceBuffer, size_t sourceBufferSize);

  void setSource(const PdfByteSource& source, uint64_t offset = 0);
  void reset(uint64_t offset = 0);
  PdfStepResult next(PdfToken& token, PdfWorkBudget& budget);
  bool unread(const PdfToken& token);

  uint64_t position() const { return position_; }
  uint64_t tokenOffset() const { return lastTokenOffset_; }

 private:
  enum class Mode : uint8_t {
    Idle,
    Comment,
    Word,
    Name,
    NameHexFirst,
    NameHexSecond,
    LessThan,
    GreaterThan,
    Literal,
    LiteralEscape,
    LiteralOctal,
    HexString,
  };

  enum class ByteResult : uint8_t {
    Available,
    End,
    Yielded,
    Failed,
  };

  ByteResult peek(uint8_t& byte, PdfWorkBudget& budget, PdfStatus& status);
  void consume();
  bool append(uint8_t byte, PdfStatus& status);
  PdfStepResult finish(PdfToken& token, PdfTokenKind kind);
  void startToken(uint64_t offset);

  PdfByteSource source_{};
  uint8_t* sourceBuffer_ = nullptr;
  size_t sourceBufferSize_ = 0;
  size_t bufferedBytes_ = 0;
  size_t bufferPosition_ = 0;
  uint64_t position_ = 0;
  uint64_t lastTokenOffset_ = 0;
  uint64_t pendingTokenOffset_ = 0;
  PdfToken pendingToken_{};
  PdfToken unreadToken_{};
  uint64_t unreadTokenOffset_ = 0;
  Mode mode_ = Mode::Idle;
  uint8_t literalDepth_ = 0;
  uint8_t hexNibble_ = 0;
  uint8_t octalValue_ = 0;
  uint8_t octalDigits_ = 0;
  bool hasHexNibble_ = false;
  bool operationCharged_ = false;
  bool hasUnreadToken_ = false;
};
