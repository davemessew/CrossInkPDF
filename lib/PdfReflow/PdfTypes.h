#pragma once

#include <cstddef>
#include <cstdint>

enum class PdfError : uint8_t {
  None,
  InvalidArgument,
  InvalidOffset,
  UnexpectedEof,
  IoFailure,
  BudgetExhausted,
  LimitExceeded,
  Unsupported,
  Malformed,
  Encrypted,
  Cancelled,
};

struct PdfStatus {
  PdfError error = PdfError::None;
  uint64_t offset = 0;

  constexpr bool ok() const { return error == PdfError::None; }
  constexpr explicit operator bool() const { return ok(); }

  static constexpr PdfStatus success() { return {}; }
  static constexpr PdfStatus failure(const PdfError error, const uint64_t offset = 0) {
    return {error, offset};
  }
};

enum class PdfStepState : uint8_t {
  Complete,
  Yielded,
  Failed,
};

struct PdfStepResult {
  PdfStepState state = PdfStepState::Complete;
  PdfStatus status{};

  constexpr bool complete() const { return state == PdfStepState::Complete; }
  constexpr bool yielded() const { return state == PdfStepState::Yielded; }
  constexpr bool failed() const { return state == PdfStepState::Failed; }

  static constexpr PdfStepResult completed() { return {}; }
  static constexpr PdfStepResult paused() {
    return {PdfStepState::Yielded, PdfStatus::failure(PdfError::BudgetExhausted)};
  }
  static constexpr PdfStepResult failure(const PdfStatus status) { return {PdfStepState::Failed, status}; }
};

struct PdfByteSource {
  using ReadAtFn = PdfStatus (*)(void* context, uint64_t offset, uint8_t* destination, size_t requested,
                                 size_t* bytesRead);

  void* context = nullptr;
  uint64_t size = 0;
  ReadAtFn readAt = nullptr;

  constexpr bool valid() const { return readAt != nullptr; }
};

struct PdfByteSink {
  using WriteFn = PdfStatus (*)(void* context, const uint8_t* source, size_t requested, size_t* bytesWritten);

  void* context = nullptr;
  WriteFn write = nullptr;

  constexpr bool valid() const { return write != nullptr; }
};

struct PdfFixedRecordStore {
  using ReadFn = PdfStatus (*)(void* context, uint32_t ordinal, void* record, size_t recordSize);
  using WriteFn = PdfStatus (*)(void* context, uint32_t ordinal, const void* record, size_t recordSize);

  void* context = nullptr;
  uint32_t capacity = 0;
  size_t recordSize = 0;
  ReadFn read = nullptr;
  WriteFn write = nullptr;

  constexpr bool valid() const { return recordSize != 0 && read != nullptr && write != nullptr; }
};

enum class PdfTokenKind : uint8_t {
  End,
  Integer,
  Real,
  Name,
  String,
  HexString,
  Keyword,
  ArrayBegin,
  ArrayEnd,
  DictionaryBegin,
  DictionaryEnd,
};

struct PdfToken {
  uint32_t length = 0;
  PdfTokenKind kind = PdfTokenKind::End;
  uint8_t reserved[3]{};
  char bytes[112]{};
};

struct PdfTextRun {
  uint32_t textOffset = 0;
  uint32_t textLength = 0;
  uint32_t sourceOrder = 0;
  int32_t xMin = 0;
  int32_t yMin = 0;
  int32_t xMax = 0;
  int32_t yMax = 0;
  int32_t baseline = 0;
  uint16_t fontId = 0;
  uint16_t flags = 0;
};

static_assert(sizeof(PdfToken) <= 128, "PDF tokens must fit the bounded token workspace");
static_assert(sizeof(PdfTextRun) <= 48, "PDF text runs must fit the bounded page workspace");
