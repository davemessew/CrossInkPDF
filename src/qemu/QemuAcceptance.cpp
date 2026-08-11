#include "QemuAcceptance.h"

#ifdef CROSSINK_QEMU

#include <Arduino.h>
#include <Epub.h>
#include <HalDisplay.h>
#include <HalPowerManager.h>
#include <HalStorage.h>
#include <PdfCacheStore.h>
#include <PdfDocumentTextClassifier.h>
#include <PdfHalCacheIo.h>
#include <PdfHalIo.h>
#include <PdfHalReflowDocument.h>
#include <PdfHiddenText.h>
#include <PdfLayoutWordIndex.h>
#include <PdfLimits.h>
#include <PdfPageTree.h>
#include <PdfPreparation.h>
#include <PdfReaderProgressState.h>
#include <PdfReadingOrder.h>
#include <PdfRunStore.h>
#include <PdfSavedItemsStore.h>
#include <PdfSemanticWriter.h>
#include <QemuHalControl.h>
#include <QemuSemihost.h>
#include <esp_rom_sys.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <optional>
#include <string>

#include "CrossPointSettings.h"
#include "Epub/Page.h"
#include "Epub/Section.h"
#include "GfxRenderer.h"
#include "MappedInputManager.h"
#include "Memory.h"
#include "PdfAcceptanceFramebufferGuard.h"
#include "BookmarkStore.h"
#include "activities/reader/EpubReaderUtils.h"
#include "activities/reader/ReaderUtils.h"
#include "components/UITheme.h"

namespace {
constexpr char SENTINEL_PATH[] = "/qemu/sentinel.txt";
constexpr char SENTINEL_CONTENT[] = "crossink-qemu-sentinel-v1\n";
constexpr char STORAGE_PARITY_ROOT[] = "/qemu/hal_open_parity";
constexpr char STORAGE_PARITY_FILE[] = "/qemu/hal_open_parity/existing.bin";
constexpr char STORAGE_PARITY_MISSING[] = "/qemu/hal_open_parity/missing.bin";
constexpr char STORAGE_IMPLICIT_PARENT[] = "/qemu/hal_missing_parent";
constexpr char STORAGE_IMPLICIT_CHILD[] = "/qemu/hal_missing_parent/file.bin";
constexpr char PDF_FIXTURE_PATH[] = "/qemu/classic_text.pdf";
constexpr char PDF_FRESH_FIXTURE_PATH[] = "/qemu/classic_text_fresh.pdf";
constexpr char PDF_FONT_SIX_PATH[] = "/qemu/font_size_6.pdf";
constexpr char PDF_FONT_SEVENTY_TWO_PATH[] = "/qemu/font_size_72.pdf";
constexpr char PDF_NAVIGATION_FIXTURE_PATH[] = "/qemu/navigation_outline.pdf";
constexpr char PDF_IMAGE_FIXTURE_PATH[] = "/qemu/raster_cover_caption.pdf";
constexpr char PDF_OCR_FIXTURE_PATH[] = "/qemu/hidden_ocr.pdf";
constexpr char PDF_COLUMNS_TABLE_FIXTURE_PATH[] = "/qemu/columns_table.pdf";
constexpr char PDF_JPEG_FIXTURE_PATH[] = "/qemu/jpeg_caption.pdf";
constexpr char EPUB_ORACLE_PATH[] = "/qemu/epub_oracle.epub";
constexpr char PDF_ACCEPTANCE_STATE_PATH[] = "/qemu/pdf_acceptance_state.bin";
constexpr char REFLOW_CACHE_DIRECTORY[] = "/.crosspoint";
constexpr char PDF_SPILL_PATH[] = "/qemu/pdf-run-spill.tmp";
constexpr char PDF_CACHE_ROOT[] = "/qemu/pdf_cache_accept";
constexpr char PDF_CACHE_GENERATION_ONE[] = "/qemu/pdf_cache_accept/gen_1";
constexpr char PDF_CACHE_GENERATION_TWO[] = "/qemu/pdf_cache_accept/gen_2";
constexpr char PDF_CACHE_METADATA_PATH[] = "/qemu/pdf_cache_accept/gen_1/metadata.bin";
constexpr char PDF_CACHE_PARTIAL_PATH[] = "/qemu/pdf_cache_accept/gen_2/partial.bin";
constexpr char PDF_EXPECTED_TEXT[] = "Hello PDF";
constexpr char HOST_PDF_PATH[] = "qemu_input.pdf";
constexpr char HOST_PDF_XHTML_PATH[] = "qemu_output.xhtml";
constexpr char SD_PDF_PATH[] = "/qemu/input.pdf";
constexpr uint32_t EXPECTED_FRAME_BYTES = 48000;
constexpr uint32_t EXPECTED_FRAME_CRC32 = 0x0F7C8C45;
constexpr uint32_t kEsp32C3FirmwareRamCeilingBytes = 380U * 1024U;
// The ESP32-C3 linker reserves part of SRAM for ROM/IDF use. The remaining
// data, BSS, and heap region is just over 320 KiB on the physical device. Keep
// QEMU on that same memory map instead of silently accepting a smaller model.
constexpr uint32_t kEsp32C3MinimumFirmwareVisibleRamBytes = 320U * 1024U;
constexpr uint32_t kMaximumPreparationSteps = 100000;
constexpr uint16_t kMaximumCancellationSlices = 256;
constexpr uint32_t kMaximumCancellationSliceMilliseconds = 8;
constexpr uint32_t kMaximumCancellationSliceMicroseconds = 8000;
constexpr uint32_t kMaximumCancellationSliceOperations = 32;
constexpr size_t kMaximumIoRequestBytes = 4U * 1024U;
constexpr size_t kMaximumPreparationIoRequestBytes = PdfLimits::InterpreterSourceBufferBytes;
constexpr uint32_t kQemuSlowAtomicWriteMicroseconds = 30000;
// LittleFS may spend one flash erase cycle creating a directory entry in QEMU.
// Keep the non-I/O slice ceiling at 8 ms, but allow that indivisible storage
// operation up to the same bounded limit as other multi-call storage sessions.
constexpr uint32_t kQemuSlowAtomicOpenWriteMicroseconds = 60000;
constexpr uint32_t kQemuSlowAtomicRenameMicroseconds = 24000;
constexpr uint32_t kQemuSlowAtomicRemoveMicroseconds = 30000;
constexpr uint32_t kQemuSlowAtomicOpenReadMicroseconds = 16000;
constexpr uint32_t kQemuSlowAtomicStorageSessionMicroseconds = 60000;
constexpr uint32_t kQemuSlowAtomicNonIoMicroseconds = 500;
constexpr uint32_t kQemuSlowAtomicAggregateRequestBytes = 3072;
constexpr uint32_t kQemuSlowAtomicAggregateCallbackMicroseconds = 550000;
constexpr uint32_t kQemuSlowAtomicAggregateNonIoMicroseconds = 5000;
constexpr uint16_t kQemuSlowAtomicWriteCount = 22;
constexpr uint16_t kQemuSlowAtomicRenameCount = 2;
constexpr uint16_t kQemuSlowAtomicOpenReadCount = 8;
constexpr uint16_t kQemuSlowAtomicTotalCount = 32;
constexpr uint16_t kCachedPageTurns = 100;
constexpr uint32_t kPersistentStateMagic = 0x51415044U;
constexpr uint16_t kPersistentStateVersion = 1;
constexpr uint64_t kFnvOffset = 14695981039346656037ULL;
constexpr uint64_t kFnvPrime = 1099511628211ULL;
constexpr uint32_t kExpectedEpubSourceBytes = 3616;
constexpr uint64_t kExpectedEpubSourceHash = 0x92563E7E3D33C382ULL;
constexpr uint64_t kExpectedEpubSectionZeroHash = 0x46385061C46C2FE4ULL;
constexpr uint64_t kExpectedEpubSectionOneHash = 0x4060CB229041492DULL;
constexpr uint64_t kExpectedEpubCssHash = 0x3CC3246E367521F4ULL;
constexpr uint16_t kExpectedEpubRepresentativePages = 3;
constexpr size_t kExpectedEpubSectionCacheBytes = 3869;
constexpr uint64_t kExpectedEpubSectionCacheHash = 0x5A4EB3E9690894B7ULL;
constexpr uint64_t kExpectedEpubFrameHash = 0xE99DC1B84A90C006ULL;
constexpr uint32_t kExpectedEpubProgressMillionths = 643454;
constexpr uint32_t kExpectedEpubBookmarkMillionths = 333333;
constexpr char kEpubCssResource[] = "OEBPS/styles/test.css";
constexpr char kEpubBookmarkSnippet[] = "qemu-epub-smoke";
constexpr uint64_t kExpectedPdfTypographySemanticHash = 0x95EE2813D71DFE2EULL;
constexpr uint64_t kExpectedPdfTypographyTextHash = 0xE1AC47B687F6E82AULL;
constexpr uint64_t kExpectedPdfOcrSemanticHash = 0xDFAE2740CD6F6513ULL;
constexpr uint64_t kExpectedPdfColumnsSemanticHash = 0x715E72B598FFFFE3ULL;
constexpr uint64_t kExpectedPdfTableSemanticHash = 0x4BD86B77E1579064ULL;
constexpr uint64_t kExpectedPdfJpegSemanticHash = 0xE72D737B2BF7D6CFULL;
constexpr char kExpectedPdfColumnsText[] = "Left one. Left two. Right one. Right two.";
constexpr char kExpectedPdfTableText[] = "Name Value Alpha 10";

struct RawFixtureSpec {
  const char* path = nullptr;
  uint32_t bytes = 0;
  uint64_t fnv = 0;
};

constexpr std::array<RawFixtureSpec, 15> kRawFixtures = {{
    {PDF_FIXTURE_PATH, 619, 0xBC5CD80ECBAF1F0FULL},
    {PDF_FRESH_FIXTURE_PATH, 619, 0xBC5CD80ECBAF1F0FULL},
    {PDF_FONT_SIX_PATH, 641, 0xA7394A01CD30C6F6ULL},
    {PDF_FONT_SEVENTY_TWO_PATH, 642, 0x9BB17FE86915CE9CULL},
    {PDF_NAVIGATION_FIXTURE_PATH, 3453, 0xA70F3A6A71121373ULL},
    {PDF_IMAGE_FIXTURE_PATH, 892, 0x49EDB13A70D2EB85ULL},
    {PDF_OCR_FIXTURE_PATH, 905, 0x02D687669DEE51B6ULL},
    {PDF_COLUMNS_TABLE_FIXTURE_PATH, 779, 0x50553E5606E1A520ULL},
    {PDF_JPEG_FIXTURE_PATH, 1217, 0x34150C9C94CB6690ULL},
    {"/qemu/bad_startxref.pdf", 647, 0x2176142C9AE0AC77ULL},
    {"/qemu/oversized_length.pdf", 612, 0x2335A1FEC241C0CCULL},
    {"/qemu/encrypted.pdf", 734, 0x1B2E21D7AE57C6DCULL},
    {"/qemu/lzw_required.pdf", 631, 0xFD27A20C68CFBC62ULL},
    {"/qemu/scan_only.pdf", 846, 0xA3044AC85E159A2AULL},
    {"/qemu/flate_bomb.pdf", 1643, 0x6FF4513F664489E3ULL},
}};

constexpr std::array<const char*, 6> kNegativeFixtures = {
    "/qemu/bad_startxref.pdf", "/qemu/oversized_length.pdf", "/qemu/encrypted.pdf",
    "/qemu/lzw_required.pdf",  "/qemu/scan_only.pdf",        "/qemu/flate_bomb.pdf",
};

enum class AcceptancePhase : uint8_t {
  NotStarted,
  BootTwoResume,
  BootTwoTypography,
  BootTwoNavigation,
  BootTwoImage,
  BootTwoPositiveCorpus,
  BootTwoProgress,
  BootTwoCacheReopen,
  BootTwoNegativeCorpus,
  BootTwoEpub,
  BootTwoProductTracer,
  BootTwoStorage,
  BootTwoFrame,
  BootTwoInput,
  WaitingForPowerSaving,
  Failed,
  Finished,
};

struct AcceptanceState {
  AcceptancePhase phase = AcceptancePhase::NotStarted;
  uint32_t idleStartedAt = 0;
  uint32_t heapStart = 0;
  uint32_t minFreeHeap = 0;
  uint32_t minMaxAllocation = 0;
  uint32_t minStackMargin = 0;
  uint32_t maxPdfAllocation = 0;
  uint32_t pdfParserEntries = 0;
  uint32_t pdfExtractionEntries = 0;
  uint32_t pdfFramebufferGuardChecks = 0;
  uint32_t pdfFramebufferGuardFailures = 0;
  uint32_t pdfFramebufferGuardControls = 0;
  uint32_t pdfFramebufferGuardRejections = 0;
  uint8_t bootSequence = 0;
  bool pdfTracerReady = false;
  bool fullAcceptanceReady = false;
};

AcceptanceState state;

struct QemuPdfResourceContext {
  AcceptanceState* acceptance = nullptr;
  bool forceInsufficientMemory = false;
};

QemuPdfResourceContext pdfResourceContext{&state, false};
QemuPdfResourceContext forcedOomResourceContext{&state, true};

struct PersistentAcceptanceState {
  uint32_t magic = kPersistentStateMagic;
  uint16_t version = kPersistentStateVersion;
  uint16_t nextBoot = 1;
  uint32_t generation = 0;
  uint32_t freshSteps = 0;
  PdfPreparationWorkCounters freshWork{};
  uint16_t cancellationSteps = 0;
  uint16_t cancellationSlices = 0;
  uint32_t cancellationMaxSliceMs = 0;
  uint32_t cancellationMaxSliceIo = 0;
  uint32_t maximumIoRequest = 0;
  uint32_t checksum = 0;
};
static_assert(sizeof(PersistentAcceptanceState) <= 128);

PersistentAcceptanceState persistentAcceptance;

struct ReaderLayout {
  int top = 0;
  int right = 0;
  int bottom = 0;
  int left = 0;
  uint16_t width = 0;
  uint16_t height = 0;
};

struct TypographySignature {
  uint64_t semantic = kFnvOffset;
  uint64_t text = kFnvOffset;
  uint32_t frame = 0;
  uint32_t words = 0;
  uint16_t pages = 0;
};

struct EpubOracle {
  uint64_t xhtml0 = kFnvOffset;
  uint64_t xhtml1 = kFnvOffset;
  uint64_t css = kFnvOffset;
  uint64_t cache = kFnvOffset;
  uint64_t frame = kFnvOffset;
};

class EpubEmbeddedStyleScope final {
 public:
  EpubEmbeddedStyleScope() : saved_(SETTINGS.embeddedStyle) { SETTINGS.embeddedStyle = 0; }
  ~EpubEmbeddedStyleScope() { SETTINGS.embeddedStyle = saved_; }

  EpubEmbeddedStyleScope(const EpubEmbeddedStyleScope&) = delete;
  EpubEmbeddedStyleScope& operator=(const EpubEmbeddedStyleScope&) = delete;

 private:
  uint8_t saved_ = 0;
};

class FnvPrint final : public Print {
 public:
  size_t write(const uint8_t byte) override {
    hash_ ^= byte;
    hash_ *= kFnvPrime;
    ++bytes_;
    return 1;
  }

  size_t write(const uint8_t* source, const size_t length) override {
    if (source == nullptr) {
      return 0;
    }
    for (size_t index = 0; index < length; ++index) {
      hash_ ^= source[index];
      hash_ *= kFnvPrime;
    }
    bytes_ += length;
    return length;
  }

  void separator() { write(0); }
  uint64_t hash() const { return hash_; }
  size_t bytes() const { return bytes_; }

 private:
  uint64_t hash_ = kFnvOffset;
  size_t bytes_ = 0;
};

class SemanticTextFnvPrint final : public Print {
 public:
  explicit SemanticTextFnvPrint(char* capture = nullptr, const size_t capacity = 0)
      : capture_(capture), capacity_(capacity) {}

  size_t write(const uint8_t byte) override { return write(&byte, 1); }

  size_t write(const uint8_t* source, const size_t length) override {
    if (source == nullptr) {
      return 0;
    }
    for (size_t index = 0; index < length; ++index) {
      const uint8_t byte = source[index];
      if (insideEntity_) {
        hashByte(byte);
        insideEntity_ = byte != static_cast<uint8_t>(';');
        continue;
      }
      if (byte == static_cast<uint8_t>('<')) {
        insideTag_ = true;
        pendingSpace_ = bytes_ != 0;
        continue;
      }
      if (insideTag_) {
        insideTag_ = byte != static_cast<uint8_t>('>');
        continue;
      }
      if (isAsciiWhitespace(byte)) {
        pendingSpace_ = bytes_ != 0;
        continue;
      }
      if (pendingSpace_) {
        hashByte(static_cast<uint8_t>(' '));
        pendingSpace_ = false;
      }
      hashByte(byte);
      insideEntity_ = byte == static_cast<uint8_t>('&');
    }
    return length;
  }

  uint64_t hash() const { return hash_; }
  size_t bytes() const { return bytes_; }
  bool captureComplete() const { return !captureOverflow_; }

 private:
  static bool isAsciiWhitespace(const uint8_t byte) {
    return byte == static_cast<uint8_t>(' ') || byte == static_cast<uint8_t>('\t') ||
           byte == static_cast<uint8_t>('\r') || byte == static_cast<uint8_t>('\n') ||
           byte == static_cast<uint8_t>('\f');
  }

  void hashByte(const uint8_t byte) {
    if (capture_ != nullptr) {
      if (bytes_ < capacity_) {
        capture_[bytes_] = static_cast<char>(byte);
      } else {
        captureOverflow_ = true;
      }
    }
    hash_ ^= byte;
    hash_ *= kFnvPrime;
    ++bytes_;
  }

  uint64_t hash_ = kFnvOffset;
  size_t bytes_ = 0;
  char* capture_ = nullptr;
  size_t capacity_ = 0;
  bool captureOverflow_ = false;
  bool insideTag_ = false;
  bool insideEntity_ = false;
  bool pendingSpace_ = false;
};

class CountingPrint final : public Print {
 public:
  CountingPrint(const char* first, const char* second, const char* third)
      : first_(first), second_(second), third_(third) {}

  size_t write(const uint8_t byte) override { return write(&byte, 1); }

  size_t write(const uint8_t* source, const size_t length) override {
    if (source == nullptr) {
      return 0;
    }
    for (size_t index = 0; index < length; ++index) {
      advance(first_, source[index], &firstOffset_, &firstCount_);
      advance(second_, source[index], &secondOffset_, &secondCount_);
      advance(third_, source[index], &thirdOffset_, &thirdCount_);
    }
    return length;
  }

  uint16_t firstCount() const { return firstCount_; }
  uint16_t secondCount() const { return secondCount_; }
  uint16_t thirdCount() const { return thirdCount_; }

 private:
  static void advance(const char* pattern, const uint8_t byte, size_t* offset, uint16_t* count) {
    if (pattern == nullptr || pattern[0] == '\0') {
      return;
    }
    if (byte == static_cast<uint8_t>(pattern[*offset])) {
      ++*offset;
      if (pattern[*offset] == '\0') {
        ++*count;
        *offset = 0;
      }
      return;
    }
    *offset = byte == static_cast<uint8_t>(pattern[0]) ? 1U : 0U;
  }

  const char* first_ = nullptr;
  const char* second_ = nullptr;
  const char* third_ = nullptr;
  size_t firstOffset_ = 0;
  size_t secondOffset_ = 0;
  size_t thirdOffset_ = 0;
  uint16_t firstCount_ = 0;
  uint16_t secondCount_ = 0;
  uint16_t thirdCount_ = 0;
};

class TracedPdfCacheIo {
 public:
  enum class Operation : uint8_t {
    None,
    Open,
    Read,
    Write,
    Flush,
    Sync,
    Close,
    Remove,
    Mkdir,
    List,
    Capacity,
    Metadata,
    Rename,
    Multiple,
  };

  enum class OpenMode : uint8_t {
    None,
    Read,
    Write,
    ReadWrite,
    WriteTruncate,
  };

  struct SliceTrace {
    uint32_t calls = 0;
    size_t requestBytes = 0;
    uint32_t callbackElapsedUs = 0;
    Operation operation = Operation::None;
    OpenMode openMode = OpenMode::None;
    bool recursive = false;
  };

  TracedPdfCacheIo() : base_(pdfHalCacheIo(baseContext_)) {}

  PdfCacheIo io() { return {this, open, read, write, flush, sync, close, remove, mkdir, list, capacity, metadata}; }

  void resetSliceTrace() { sliceTrace_ = {}; }

  SliceTrace sliceTrace() const { return sliceTrace_; }

  static const char* operationName(const Operation operation) {
    switch (operation) {
      case Operation::Open:
        return "open";
      case Operation::Read:
        return "read";
      case Operation::Write:
        return "write";
      case Operation::Flush:
        return "flush";
      case Operation::Sync:
        return "sync";
      case Operation::Close:
        return "close";
      case Operation::Remove:
        return "remove";
      case Operation::Mkdir:
        return "mkdir";
      case Operation::List:
        return "list";
      case Operation::Capacity:
        return "capacity";
      case Operation::Metadata:
        return "metadata";
      case Operation::Rename:
        return "rename";
      case Operation::Multiple:
        return "multiple";
      case Operation::None:
      default:
        return "none";
    }
  }

  static const char* openModeName(const OpenMode mode) {
    switch (mode) {
      case OpenMode::Read:
        return "read";
      case OpenMode::Write:
        return "write";
      case OpenMode::ReadWrite:
        return "readwrite";
      case OpenMode::WriteTruncate:
        return "write_truncate";
      case OpenMode::None:
      default:
        return "none";
    }
  }

  static PdfStatus rename(void* context, const char* sourcePath, const char* destinationPath) {
    if (context == nullptr) {
      return PdfStatus::failure(PdfError::InvalidArgument);
    }
    auto& owner = self(context);
    const uint32_t startedAtUs = micros();
    owner.beginSliceOperation(Operation::Rename, 0, false);
    if (destinationPath != nullptr && std::strcmp(destinationPath, owner.retainedPath_) == 0) {
      ++owner.retainedWriteTruncate;
    }
    const PdfStatus status = pdfHalCacheRename(&owner.baseContext_, sourcePath, destinationPath);
    owner.finishSliceOperation(startedAtUs);
    return status;
  }

  void trackRetainedPath(const char* path) {
    std::snprintf(retainedPath_, sizeof(retainedPath_), "%s", path == nullptr ? "" : path);
    retainedWriteTruncate = 0;
    retainedRemove = 0;
  }

  void trackSourcePath(const char* path) {
    std::snprintf(trackedSourcePath_, sizeof(trackedSourcePath_), "%s", path == nullptr ? "" : path);
    sourceReadOpenCount = 0;
    sourceReadCount = 0;
    sourceMaximumReadRequest = 0;
    trackedSourceHandles_.fill(false);
  }

  uint32_t ioCalls = 0;
  size_t maximumRequest = 0;
  uint32_t retainedWriteTruncate = 0;
  uint32_t retainedRemove = 0;
  uint32_t sourceReadOpenCount = 0;
  uint32_t sourceReadCount = 0;
  size_t sourceMaximumReadRequest = 0;

 private:
  static TracedPdfCacheIo& self(void* context) { return *static_cast<TracedPdfCacheIo*>(context); }

  void beginSliceOperation(const Operation operation, const size_t requestBytes, const bool recursive) {
    ++ioCalls;
    ++sliceTrace_.calls;
    if (sliceTrace_.calls == 1) {
      sliceTrace_.operation = operation;
      sliceTrace_.requestBytes = requestBytes;
      sliceTrace_.recursive = recursive;
      return;
    }
    sliceTrace_.operation = Operation::Multiple;
    sliceTrace_.requestBytes = std::max(sliceTrace_.requestBytes, requestBytes);
    sliceTrace_.recursive = sliceTrace_.recursive || recursive;
  }

  void finishSliceOperation(const uint32_t startedAtUs) {
    sliceTrace_.callbackElapsedUs += micros() - startedAtUs;
  }

  static PdfStatus open(void* context, const char* path, const PdfCacheOpenMode mode, PdfCacheHandle* handle) {
    if (context == nullptr || path == nullptr || handle == nullptr) {
      return PdfStatus::failure(PdfError::InvalidArgument);
    }
    auto& owner = self(context);
    const uint32_t startedAtUs = micros();
    owner.beginSliceOperation(Operation::Open, 0, false);
    owner.sliceTrace_.openMode = mode == PdfCacheOpenMode::Read          ? OpenMode::Read
                                 : mode == PdfCacheOpenMode::Write       ? OpenMode::Write
                                 : mode == PdfCacheOpenMode::ReadWrite   ? OpenMode::ReadWrite
                                                                         : OpenMode::WriteTruncate;
    if (mode == PdfCacheOpenMode::WriteTruncate && std::strcmp(path, owner.retainedPath_) == 0) {
      ++owner.retainedWriteTruncate;
    }
    const PdfStatus status = owner.base_.open(owner.base_.context, path, mode, handle);
    if (status && handle->value < owner.trackedSourceHandles_.size()) {
      const bool tracked = mode == PdfCacheOpenMode::Read && std::strcmp(path, owner.trackedSourcePath_) == 0;
      owner.trackedSourceHandles_[handle->value] = tracked;
      if (tracked) {
        ++owner.sourceReadOpenCount;
      }
    }
    owner.finishSliceOperation(startedAtUs);
    return status;
  }

  static PdfStatus read(void* context, const PdfCacheHandle handle, const uint64_t offset, uint8_t* destination,
                        const size_t requested, size_t* bytesRead) {
    if (context == nullptr) {
      return PdfStatus::failure(PdfError::InvalidArgument);
    }
    auto& owner = self(context);
    const uint32_t startedAtUs = micros();
    owner.beginSliceOperation(Operation::Read, requested, false);
    owner.maximumRequest = std::max(owner.maximumRequest, requested);
    const PdfStatus status = owner.base_.read(owner.base_.context, handle, offset, destination, requested, bytesRead);
    if (handle.value < owner.trackedSourceHandles_.size() && owner.trackedSourceHandles_[handle.value]) {
      ++owner.sourceReadCount;
      owner.sourceMaximumReadRequest = std::max(owner.sourceMaximumReadRequest, requested);
    }
    owner.finishSliceOperation(startedAtUs);
    return status;
  }

  static PdfStatus write(void* context, const PdfCacheHandle handle, const uint8_t* source, const size_t requested,
                         size_t* bytesWritten) {
    if (context == nullptr) {
      return PdfStatus::failure(PdfError::InvalidArgument);
    }
    auto& owner = self(context);
    const uint32_t startedAtUs = micros();
    owner.beginSliceOperation(Operation::Write, requested, false);
    owner.maximumRequest = std::max(owner.maximumRequest, requested);
    const PdfStatus status = owner.base_.write(owner.base_.context, handle, source, requested, bytesWritten);
    owner.finishSliceOperation(startedAtUs);
    return status;
  }

  static PdfStatus flush(void* context, const PdfCacheHandle handle) {
    auto& owner = self(context);
    const uint32_t startedAtUs = micros();
    owner.beginSliceOperation(Operation::Flush, 0, false);
    const PdfStatus status = owner.base_.flush(owner.base_.context, handle);
    owner.finishSliceOperation(startedAtUs);
    return status;
  }

  static PdfStatus sync(void* context, const PdfCacheHandle handle) {
    auto& owner = self(context);
    const uint32_t startedAtUs = micros();
    owner.beginSliceOperation(Operation::Sync, 0, false);
    const PdfStatus status = owner.base_.sync(owner.base_.context, handle);
    owner.finishSliceOperation(startedAtUs);
    return status;
  }

  static PdfStatus close(void* context, PdfCacheHandle* handle) {
    auto& owner = self(context);
    const uint32_t startedAtUs = micros();
    owner.beginSliceOperation(Operation::Close, 0, false);
    const uint8_t slot = handle == nullptr ? 0xff : handle->value;
    const PdfStatus status = owner.base_.close(owner.base_.context, handle);
    if (slot < owner.trackedSourceHandles_.size()) {
      owner.trackedSourceHandles_[slot] = false;
    }
    owner.finishSliceOperation(startedAtUs);
    return status;
  }

  static PdfStatus remove(void* context, const char* path, const bool recursive) {
    auto& owner = self(context);
    const uint32_t startedAtUs = micros();
    owner.beginSliceOperation(Operation::Remove, 0, recursive);
    const size_t removedLength = path == nullptr ? 0 : std::strlen(path);
    const size_t retainedLength = std::strlen(owner.retainedPath_);
    const bool removesParent = recursive && removedLength != 0 && removedLength < retainedLength &&
                               std::strncmp(path, owner.retainedPath_, removedLength) == 0 &&
                               owner.retainedPath_[removedLength] == '/';
    if ((path != nullptr && std::strcmp(path, owner.retainedPath_) == 0) || removesParent) {
      ++owner.retainedRemove;
    }
    const PdfStatus status = owner.base_.remove(owner.base_.context, path, recursive);
    owner.finishSliceOperation(startedAtUs);
    return status;
  }

  static PdfStatus mkdir(void* context, const char* path) {
    auto& owner = self(context);
    const uint32_t startedAtUs = micros();
    owner.beginSliceOperation(Operation::Mkdir, 0, false);
    const PdfStatus status = owner.base_.mkdir(owner.base_.context, path);
    owner.finishSliceOperation(startedAtUs);
    return status;
  }

  static PdfStatus list(void* context, const char* path, const PdfCacheListVisitor visitor, void* visitorContext) {
    auto& owner = self(context);
    const uint32_t startedAtUs = micros();
    owner.beginSliceOperation(Operation::List, 0, false);
    const PdfStatus status = owner.base_.list(owner.base_.context, path, visitor, visitorContext);
    owner.finishSliceOperation(startedAtUs);
    return status;
  }

  static PdfStatus capacity(void* context, PdfCacheCapacity* result) {
    auto& owner = self(context);
    const uint32_t startedAtUs = micros();
    owner.beginSliceOperation(Operation::Capacity, 0, false);
    const PdfStatus status = owner.base_.capacity(owner.base_.context, result);
    owner.finishSliceOperation(startedAtUs);
    return status;
  }

  static PdfStatus metadata(void* context, const PdfCacheHandle handle, PdfCacheFileMetadata* result) {
    auto& owner = self(context);
    const uint32_t startedAtUs = micros();
    owner.beginSliceOperation(Operation::Metadata, 0, false);
    const PdfStatus status = owner.base_.metadata(owner.base_.context, handle, result);
    owner.finishSliceOperation(startedAtUs);
    return status;
  }

  PdfHalCacheIoContext baseContext_{};
  PdfCacheIo base_{};
  SliceTrace sliceTrace_{};
  char retainedPath_[PDF_CACHE_PATH_CAPACITY]{};
  char trackedSourcePath_[PDF_CACHE_PATH_CAPACITY]{};
  std::array<bool, PDF_HAL_CACHE_HANDLE_COUNT> trackedSourceHandles_{};
};

struct QemuSlowAtomicTotals {
  uint16_t total = 0;
  uint16_t writes = 0;
  uint16_t renames = 0;
  uint16_t openReads = 0;
  uint32_t requestBytes = 0;
  uint32_t callbackUs = 0;
  uint32_t nonIoUs = 0;
  uint32_t maxTotalUs = 0;
  uint32_t maxCallbackUs = 0;
};

bool qemuSlowAtomicAllowed(const TracedPdfCacheIo::SliceTrace& trace, const uint32_t elapsedUs,
                           const uint32_t nonIoUs) {
  if (elapsedUs <= kMaximumCancellationSliceMicroseconds || trace.calls == 0 || trace.recursive ||
      trace.callbackElapsedUs > elapsedUs || nonIoUs > kQemuSlowAtomicNonIoMicroseconds) {
    return false;
  }
  const bool singleCall = trace.calls == 1;
  if (trace.operation == TracedPdfCacheIo::Operation::Write) {
    return singleCall && trace.openMode == TracedPdfCacheIo::OpenMode::None && trace.requestBytes >= 1 &&
           trace.requestBytes <= 1024 && trace.callbackElapsedUs <= kQemuSlowAtomicWriteMicroseconds;
  }
  if (trace.operation == TracedPdfCacheIo::Operation::Rename) {
    return singleCall && trace.openMode == TracedPdfCacheIo::OpenMode::None && trace.requestBytes == 0 &&
           trace.callbackElapsedUs <= kQemuSlowAtomicRenameMicroseconds;
  }
  if (trace.operation == TracedPdfCacheIo::Operation::Remove) {
    return singleCall && !trace.recursive && trace.openMode == TracedPdfCacheIo::OpenMode::None &&
           trace.requestBytes == 0 && trace.callbackElapsedUs <= kQemuSlowAtomicRemoveMicroseconds;
  }
  if (trace.operation == TracedPdfCacheIo::Operation::Open) {
    const bool readOpen = trace.openMode == TracedPdfCacheIo::OpenMode::Read &&
                          trace.callbackElapsedUs <= kQemuSlowAtomicOpenReadMicroseconds;
    const bool writeOpen =
        (trace.openMode == TracedPdfCacheIo::OpenMode::Write ||
         trace.openMode == TracedPdfCacheIo::OpenMode::ReadWrite ||
         trace.openMode == TracedPdfCacheIo::OpenMode::WriteTruncate) &&
        trace.callbackElapsedUs <= kQemuSlowAtomicOpenWriteMicroseconds;
    return singleCall && trace.requestBytes == 0 && (readOpen || writeOpen);
  }
  if (trace.operation == TracedPdfCacheIo::Operation::Multiple) {
    return trace.calls >= 2 && trace.calls <= 5 && trace.requestBytes <= 1024 &&
           trace.callbackElapsedUs <= kQemuSlowAtomicStorageSessionMicroseconds;
  }
  return false;
}

bool recordQemuSlowAtomic(QemuSlowAtomicTotals& totals, const uint16_t slice, const uint32_t elapsedUs,
                          const TracedPdfCacheIo::SliceTrace& trace, const uint32_t nonIoUs) {
  ++totals.total;
  if (trace.operation == TracedPdfCacheIo::Operation::Write ||
      trace.operation == TracedPdfCacheIo::Operation::Remove ||
      (trace.operation == TracedPdfCacheIo::Operation::Open &&
       trace.openMode != TracedPdfCacheIo::OpenMode::Read) ||
      (trace.operation == TracedPdfCacheIo::Operation::Multiple &&
       trace.openMode != TracedPdfCacheIo::OpenMode::Read)) {
    ++totals.writes;
  } else if (trace.operation == TracedPdfCacheIo::Operation::Rename) {
    ++totals.renames;
  } else if (trace.operation == TracedPdfCacheIo::Operation::Open ||
             trace.operation == TracedPdfCacheIo::Operation::Multiple) {
    ++totals.openReads;
  } else {
    return false;
  }
  totals.requestBytes += static_cast<uint32_t>(trace.requestBytes);
  totals.callbackUs += trace.callbackElapsedUs;
  totals.nonIoUs += nonIoUs;
  totals.maxTotalUs = std::max(totals.maxTotalUs, elapsedUs);
  totals.maxCallbackUs = std::max(totals.maxCallbackUs, trace.callbackElapsedUs);
  if (totals.total > kQemuSlowAtomicTotalCount || totals.writes > kQemuSlowAtomicWriteCount ||
      totals.renames > kQemuSlowAtomicRenameCount || totals.openReads > kQemuSlowAtomicOpenReadCount ||
      totals.requestBytes > kQemuSlowAtomicAggregateRequestBytes ||
      totals.callbackUs > kQemuSlowAtomicAggregateCallbackMicroseconds ||
      totals.nonIoUs > kQemuSlowAtomicAggregateNonIoMicroseconds) {
    return false;
  }
  esp_rom_printf(
      "QEMU_PDF_SLOW_ATOMIC index=%u slice=%u calls=%lu kind=%s mode=%s "
      "recursive=%u request=%lu total_us=%lu callback_us=%lu nonio_us=%lu\n",
      static_cast<unsigned>(totals.total - 1), static_cast<unsigned>(slice),
      static_cast<unsigned long>(trace.calls), TracedPdfCacheIo::operationName(trace.operation),
      TracedPdfCacheIo::openModeName(trace.openMode), static_cast<unsigned>(trace.recursive),
      static_cast<unsigned long>(trace.requestBytes), static_cast<unsigned long>(elapsedUs),
      static_cast<unsigned long>(trace.callbackElapsedUs), static_cast<unsigned long>(nonIoUs));
  return true;
}

void emitQemuSlowAtomicSummary(const QemuSlowAtomicTotals& totals, const uint32_t generation,
                               const uint16_t slices) {
  esp_rom_printf(
      "QEMU_PDF_SLOW_ATOMIC_SUMMARY generation=%lu slices=%u total=%u write=%u "
      "rename=%u open_read=%u request_bytes=%lu callback_us=%lu nonio_us=%lu "
      "max_total_us=%lu max_callback_us=%lu\n",
      static_cast<unsigned long>(generation), static_cast<unsigned>(slices), static_cast<unsigned>(totals.total),
      static_cast<unsigned>(totals.writes), static_cast<unsigned>(totals.renames),
      static_cast<unsigned>(totals.openReads), static_cast<unsigned long>(totals.requestBytes),
      static_cast<unsigned long>(totals.callbackUs), static_cast<unsigned long>(totals.nonIoUs),
      static_cast<unsigned long>(totals.maxTotalUs), static_cast<unsigned long>(totals.maxCallbackUs));
}

#ifdef CROSSINK_QEMU_TIMING_DIAGNOSTIC
struct TimingDiagnosticRecord {
  uint16_t slice = 0;
  uint32_t totalUs = 0;
  uint32_t callbackUs = 0;
  uint32_t nonIoUs = 0;
  uint32_t requestBytes = 0;
  TracedPdfCacheIo::Operation operation = TracedPdfCacheIo::Operation::None;
  TracedPdfCacheIo::OpenMode openMode = TracedPdfCacheIo::OpenMode::None;
  bool recursive = false;
};

std::array<TimingDiagnosticRecord, kMaximumCancellationSlices> timingDiagnosticRecords{};
uint16_t timingDiagnosticRecordCount = 0;

void recordTimingDiagnostic(const uint16_t slice, const uint32_t totalUs,
                            const TracedPdfCacheIo::SliceTrace& trace, const uint32_t nonIoUs) {
  if (timingDiagnosticRecordCount >= timingDiagnosticRecords.size()) {
    return;
  }
  timingDiagnosticRecords[timingDiagnosticRecordCount++] = {
      slice, totalUs, trace.callbackElapsedUs, nonIoUs, static_cast<uint32_t>(trace.requestBytes),
      trace.operation, trace.openMode, trace.recursive};
}

void emitTimingDiagnostics(const uint16_t totalSlices) {
  for (uint16_t index = 0; index < timingDiagnosticRecordCount; ++index) {
    const TimingDiagnosticRecord& record = timingDiagnosticRecords[index];
    esp_rom_printf(
        "QEMU_PDF_TIMING_SAMPLE index=%u slice=%u total_us=%lu io_kind=%s io_mode=%s "
        "io_recursive=%u io_request=%lu callback_us=%lu nonio_us=%lu\n",
        static_cast<unsigned>(index), static_cast<unsigned>(record.slice),
        static_cast<unsigned long>(record.totalUs), TracedPdfCacheIo::operationName(record.operation),
        TracedPdfCacheIo::openModeName(record.openMode), static_cast<unsigned>(record.recursive),
        static_cast<unsigned long>(record.requestBytes), static_cast<unsigned long>(record.callbackUs),
        static_cast<unsigned long>(record.nonIoUs));
  }
  esp_rom_printf("QEMU_PDF_TIMING_SUMMARY total_slices=%u violations=%u capacity=%u\n",
                 static_cast<unsigned>(totalSlices), static_cast<unsigned>(timingDiagnosticRecordCount),
                 static_cast<unsigned>(timingDiagnosticRecords.size()));
}
#endif

class PatternPrint final : public Print {
 public:
  PatternPrint(const char* first, const char* second) : first_(first), second_(second) {}

  size_t write(const uint8_t byte) override { return write(&byte, 1); }

  size_t write(const uint8_t* source, const size_t length) override {
    if (source == nullptr) {
      return 0;
    }
    for (size_t index = 0; index < length; ++index) {
      advance(first_, source[index], &firstOffset_, &firstFound_);
      advance(second_, source[index], &secondOffset_, &secondFound_);
    }
    return length;
  }

  bool matched() const { return firstFound_ && secondFound_; }

 private:
  static void advance(const char* pattern, const uint8_t byte, size_t* offset, bool* found) {
    if (*found || pattern == nullptr || pattern[0] == '\0') {
      *found = true;
      return;
    }
    if (byte == static_cast<uint8_t>(pattern[*offset])) {
      ++*offset;
      if (pattern[*offset] == '\0') {
        *found = true;
      }
    } else {
      *offset = byte == static_cast<uint8_t>(pattern[0]) ? 1U : 0U;
    }
  }

  const char* first_ = nullptr;
  const char* second_ = nullptr;
  size_t firstOffset_ = 0;
  size_t secondOffset_ = 0;
  bool firstFound_ = false;
  bool secondFound_ = false;
};

struct MemoryRecordContext {
  uint8_t* bytes = nullptr;
  size_t recordSize = 0;
  uint32_t capacity = 0;
  bool accessRequired = false;
  bool accessOpen = false;
  uint32_t accessOpenCount = 0;
  uint32_t accessCloseCount = 0;
};

struct PdfCoreAcceptanceWorkspace {
  uint8_t sourceBuffer[PdfLimits::SourceBufferBytes]{};
  PdfValue values[128]{};
  PdfDictionaryEntry dictionaryEntries[128]{};
  PdfArrayItem arrayItems[128]{};
  uint8_t objectText[2048]{};
  PdfXrefEntry xrefEntries[128]{};
  PdfPageTreeRecord traversalRecords[64]{};
  PdfTextRun reductionRuns[4]{};
  uint8_t reductionText[256]{};
  PdfReadingOrderItem reductionOrder[4]{};
  PdfPageInfo pageScratch{};
  PdfPageInfo firstPage{};
  char transcript[32]{};
  uint8_t semanticOutput[384]{};
  uint8_t semanticBuffer[PdfSemanticWriterLimits::MinimumOutputBufferBytes]{};
  PdfSemanticBlockRecord semanticRecord{};
  uint32_t pageCount = 0;
};
static_assert(sizeof(PdfCoreAcceptanceWorkspace) <= 32768);

struct ReductionSinkContext {
  PdfRunStore* runs = nullptr;
  char* transcript = nullptr;
  size_t capacity = 0;
  size_t length = 0;
};

struct SemanticSinkContext {
  PdfCoreAcceptanceWorkspace* workspace = nullptr;
  size_t length = 0;
  uint32_t records = 0;
};

struct SingleCacheRecordSource {
  PdfRequiredFileRecord record{};

  static PdfStatus read(void* context, const uint32_t index, PdfRequiredFileRecord* output) {
    if (context == nullptr || output == nullptr || index != 0) {
      return PdfStatus::failure(PdfError::InvalidArgument, index);
    }
    *output = static_cast<SingleCacheRecordSource*>(context)->record;
    return PdfStatus::success();
  }

  PdfRequiredFileTableSource source() { return {this, 1, read}; }
};

void fail(const char* component, const char* reason) {
  esp_rom_printf("QEMU_%s_FAIL reason=%s\n", component, reason);
  state.phase = AcceptancePhase::Failed;
}

extern "C" uint8_t _data_start;
extern "C" uint8_t _data_end;
extern "C" uint8_t _bss_start;
extern "C" uint8_t _bss_end;
extern "C" uint8_t _noinit_start;
extern "C" uint8_t _noinit_end;

bool checkRamBudget() {
  const uintptr_t dataStart = reinterpret_cast<uintptr_t>(&_data_start);
  const uintptr_t dataEnd = reinterpret_cast<uintptr_t>(&_data_end);
  const uintptr_t bssStart = reinterpret_cast<uintptr_t>(&_bss_start);
  const uintptr_t bssEnd = reinterpret_cast<uintptr_t>(&_bss_end);
  const uintptr_t noinitStart = reinterpret_cast<uintptr_t>(&_noinit_start);
  const uintptr_t noinitEnd = reinterpret_cast<uintptr_t>(&_noinit_end);
  if (dataEnd < dataStart || bssEnd < bssStart || noinitEnd < noinitStart) {
    fail("RAM", "linker_symbols");
    return false;
  }

  const uint64_t staticBytes = static_cast<uint64_t>(dataEnd - dataStart) +
                               static_cast<uint64_t>(bssEnd - bssStart) +
                               static_cast<uint64_t>(noinitEnd - noinitStart);
  const uint32_t heapBytes = ESP.getHeapSize();
  const uint64_t firmwareVisibleBytes = staticBytes + heapBytes;
  if (firmwareVisibleBytes < kEsp32C3MinimumFirmwareVisibleRamBytes) {
    fail("RAM", "below_device_map");
    return false;
  }
  if (firmwareVisibleBytes > kEsp32C3FirmwareRamCeilingBytes) {
    fail("RAM", "ceiling");
    return false;
  }

  esp_rom_printf("QEMU_RAM_PASS limit=%lu visible=%llu static=%llu heap=%lu framebuffer=%lu\n",
                 static_cast<unsigned long>(kEsp32C3FirmwareRamCeilingBytes),
                 static_cast<unsigned long long>(firmwareVisibleBytes),
                 static_cast<unsigned long long>(staticBytes), static_cast<unsigned long>(heapBytes),
                 static_cast<unsigned long>(EXPECTED_FRAME_BYTES));
  return true;
}

uint32_t stackMarginBytes() {
  // ESP-IDF reports this high-water mark in bytes (unlike upstream FreeRTOS).
  return static_cast<uint32_t>(uxTaskGetStackHighWaterMark(nullptr));
}

void sampleRuntime() {
  const uint32_t freeHeap = ESP.getFreeHeap();
  const uint32_t reportedMinimum = ESP.getMinFreeHeap();
  const uint32_t maxAllocation = ESP.getMaxAllocHeap();
  const uint32_t stackMargin = stackMarginBytes();

  if (freeHeap < state.minFreeHeap) {
    state.minFreeHeap = freeHeap;
  }
  if (reportedMinimum < state.minFreeHeap) {
    state.minFreeHeap = reportedMinimum;
  }
  if (maxAllocation < state.minMaxAllocation) {
    state.minMaxAllocation = maxAllocation;
  }
  if (stackMargin < state.minStackMargin) {
    state.minStackMargin = stackMargin;
  }
}

void emitRuntimeSample() {
  sampleRuntime();
  esp_rom_printf("QEMU_RUNTIME heap_start=%lu min_free=%lu min_max_alloc=%lu max_alloc=%lu stack_margin=%lu\n",
                 static_cast<unsigned long>(state.heapStart), static_cast<unsigned long>(state.minFreeHeap),
                 static_cast<unsigned long>(state.minMaxAllocation), static_cast<unsigned long>(state.maxPdfAllocation),
                 static_cast<unsigned long>(state.minStackMargin));
}

uint32_t qemuNowMs(void*) { return millis(); }

PdfResourceSnapshot qemuPdfResources(void* context) {
  const auto* resources = static_cast<const QemuPdfResourceContext*>(context);
  if (resources != nullptr && resources->forceInsufficientMemory) {
    return {0, 0, stackMarginBytes()};
  }
  return {ESP.getFreeHeap(), ESP.getMaxAllocHeap(), stackMarginBytes()};
}

void recordPdfResourceEvent(void* context, const PdfResourceEvent& event) {
  auto* resources = static_cast<QemuPdfResourceContext*>(context);
  if (resources == nullptr || resources->acceptance == nullptr || event.event != PdfResourceEventKind::Acquired) {
    return;
  }
  const uint32_t bytes = event.bytes > UINT32_MAX ? UINT32_MAX : static_cast<uint32_t>(event.bytes);
  resources->acceptance->maxPdfAllocation = std::max(resources->acceptance->maxPdfAllocation, bytes);
}

uint64_t fnvBytes(const uint8_t* bytes, const size_t length, uint64_t hash = kFnvOffset) {
  if (bytes == nullptr) {
    return hash;
  }
  for (size_t index = 0; index < length; ++index) {
    hash ^= bytes[index];
    hash *= kFnvPrime;
  }
  return hash;
}

PdfAcceptanceFramebufferSnapshot qemuFramebufferSnapshot(GfxRenderer& renderer) {
  const uint8_t* const framebuffer = renderer.getFrameBuffer();
  const size_t framebufferBytes = renderer.getBufferSize();
  if (framebuffer == nullptr || framebufferBytes != EXPECTED_FRAME_BYTES) {
    return {framebuffer, framebufferBytes, 0};
  }
  return {framebuffer, framebufferBytes, fnvBytes(framebuffer, framebufferBytes)};
}

uint16_t countAsciiWords(const char* text, const size_t length) {
  uint16_t words = 0;
  bool insideWord = false;
  for (size_t index = 0; index < length; ++index) {
    const bool whitespace = text[index] == ' ' || text[index] == '\t' || text[index] == '\r' || text[index] == '\n';
    if (!whitespace && !insideWord) {
      ++words;
    }
    insideWord = !whitespace;
  }
  return words;
}

uint32_t persistentChecksum(const PersistentAcceptanceState& persistent) {
  uint64_t hash = kFnvOffset;
  const auto add = [&hash](const auto& value) {
    hash = fnvBytes(reinterpret_cast<const uint8_t*>(&value), sizeof(value), hash);
  };
  add(persistent.magic);
  add(persistent.version);
  add(persistent.nextBoot);
  add(persistent.generation);
  add(persistent.freshSteps);
  add(persistent.freshWork.xrefSteps);
  add(persistent.freshWork.pagesWalked);
  add(persistent.freshWork.contentTokens);
  add(persistent.freshWork.sectionsEmitted);
  add(persistent.freshWork.imagesEmitted);
  add(persistent.freshWork.sourceBytesRead);
  add(persistent.cancellationSteps);
  add(persistent.cancellationSlices);
  add(persistent.cancellationMaxSliceMs);
  add(persistent.cancellationMaxSliceIo);
  add(persistent.maximumIoRequest);
  return static_cast<uint32_t>(hash);
}

bool savePersistentState(PersistentAcceptanceState persistent) {
  persistent.checksum = persistentChecksum(persistent);
  HalFile file = Storage.open(PDF_ACCEPTANCE_STATE_PATH, static_cast<oflag_t>(O_WRONLY | O_CREAT | O_TRUNC));
  if (!file) {
    return false;
  }
  const bool saved = file.write(&persistent, sizeof(persistent)) == sizeof(persistent) && file.sync();
  const bool closed = file.close();
  return saved && closed;
}

bool loadPersistentState(PersistentAcceptanceState* persistent) {
  if (persistent == nullptr || !Storage.exists(PDF_ACCEPTANCE_STATE_PATH)) {
    return false;
  }
  HalFile file = Storage.open(PDF_ACCEPTANCE_STATE_PATH, O_RDONLY);
  if (!file || file.fileSize64() != sizeof(*persistent)) {
    file.close();
    return false;
  }
  const bool loaded = file.read(persistent, sizeof(*persistent)) == static_cast<int>(sizeof(*persistent));
  const bool closed = file.close();
  return loaded && closed && persistent->magic == kPersistentStateMagic &&
         persistent->version == kPersistentStateVersion && persistent->nextBoot == 1 &&
         persistent->checksum == persistentChecksum(*persistent);
}

void pinReaderSettings(GfxRenderer& renderer) {
  SETTINGS.uiTheme = CrossPointSettings::UI_THEME::CLASSIC;
  SETTINGS.fontFamily = CrossPointSettings::FONT_FAMILY::LEXENDDECA;
  SETTINGS.fontSize = CrossPointSettings::FONT_SIZE::MEDIUM;
  SETTINGS.lineHeightPercent = 100;
  SETTINGS.orientation = CrossPointSettings::ORIENTATION::PORTRAIT;
  SETTINGS.screenMargin = 5;
  SETTINGS.publisherPageNumbers = 0;
  SETTINGS.paragraphAlignment = CrossPointSettings::PARAGRAPH_ALIGNMENT::BOOK_STYLE;
  SETTINGS.embeddedStyle = 1;
  SETTINGS.hyphenationEnabled = 0;
  SETTINGS.textAntiAliasing = 0;
  SETTINGS.readerDarkMode = 0;
  SETTINGS.imageRendering = CrossPointSettings::IMAGE_RENDERING::IMAGES_DISPLAY;
  SETTINGS.extraParagraphSpacing = 1;
  SETTINGS.forceParagraphIndents = 0;
  SETTINGS.bionicReadingEnabled = 0;
  SETTINGS.guideReadingEnabled = 0;
  SETTINGS.epubRenderMode = 0;
  SETTINGS.sdFontFamilyName[0] = '\0';
  SETTINGS.statusBarChapterPageCount = 0;
  SETTINGS.statusBarBookProgressPercentage = 0;
  SETTINGS.stablePageNumbers = 0;
  SETTINGS.statusBarProgressBar = CrossPointSettings::STATUS_BAR_PROGRESS_BAR::HIDE_PROGRESS;
  SETTINGS.statusBarTitle = CrossPointSettings::STATUS_BAR_TITLE::HIDE_TITLE;
  SETTINGS.statusBarTimeLeft = CrossPointSettings::STATUS_BAR_TIME_LEFT::TIME_LEFT_HIDE;
  SETTINGS.statusBarBattery = 0;
  SETTINGS.hideClock = CrossPointSettings::HIDE_CLOCK_MODE::HIDE_CLOCK_ALWAYS;
  UITheme::getInstance().reload();
  ReaderUtils::applyOrientation(renderer, SETTINGS.orientation);
}

bool computeReaderLayout(GfxRenderer& renderer, ReaderLayout* layout) {
  if (layout == nullptr || renderer.getScreenWidth() != 480 || renderer.getScreenHeight() != 800 ||
      UITheme::getStatusBarHeight() != 0 || ReaderUtils::getTopClockStatusBarReservedHeight() != 0) {
    return false;
  }
  renderer.getOrientedViewableTRBL(&layout->top, &layout->right, &layout->bottom, &layout->left);
  layout->top += SETTINGS.screenMargin;
  layout->right += SETTINGS.screenMargin;
  layout->bottom += std::max<int>(SETTINGS.screenMargin, ReaderUtils::STATUS_BAR_TEXT_PADDING);
  layout->left += SETTINGS.screenMargin;
  const int width = renderer.getScreenWidth() - layout->left - layout->right;
  const int height = renderer.getScreenHeight() - layout->top - layout->bottom;
  if (width <= 0 || height <= 0 || width > UINT16_MAX || height > UINT16_MAX) {
    return false;
  }
  layout->width = static_cast<uint16_t>(width);
  layout->height = static_cast<uint16_t>(height);
  return true;
}

PdfPreparationConfig preparationConfig(TracedPdfCacheIo& traced, const char* sourcePath, GfxRenderer& renderer,
                                       QemuPdfResourceContext* resources = &pdfResourceContext) {
  return {
      traced.io(),
      sourcePath,
      REFLOW_CACHE_DIRECTORY,
      nullptr,
      qemuNowMs,
      {resources, qemuPdfResources, recordPdfResourceEvent},
      TracedPdfCacheIo::rename,
      static_cast<uint16_t>(renderer.getScreenWidth()),
      static_cast<uint16_t>(renderer.getScreenHeight()),
  };
}

PdfStatus beginTrackedPdfPreparation(PdfPreparation& preparation, const PdfPreparationConfig& config) {
  ++state.pdfParserEntries;
  return preparation.begin(config);
}

PdfStepResult stepTrackedPdfPreparation(PdfPreparation& preparation) {
  ++state.pdfExtractionEntries;
  return preparation.step();
}

void yieldAfterPdfPreparationStep(const PdfStepResult& result) {
  if (result.yielded()) {
    yield();
  }
}

PdfStepResult runPreparation(PdfPreparation& preparation, uint32_t* steps = nullptr) {
  for (uint32_t step = 0; step < kMaximumPreparationSteps; ++step) {
    const PdfStepResult result = stepTrackedPdfPreparation(preparation);
    if (steps != nullptr) {
      ++*steps;
    }
    sampleRuntime();
    yieldAfterPdfPreparationStep(result);
    if (!result.yielded()) {
      return result;
    }
  }
  return PdfStepResult::failure(PdfStatus::failure(PdfError::BudgetExhausted));
}

bool preparePdf(const char* path, GfxRenderer& renderer, uint32_t* steps = nullptr,
                PdfPreparationWorkCounters* counters = nullptr) {
  TracedPdfCacheIo traced;
  auto preparation = makeUniqueNoThrow<PdfPreparation>();
  if (!preparation) {
    return false;
  }
  PdfStatus status = beginTrackedPdfPreparation(*preparation, preparationConfig(traced, path, renderer));
  PdfStepResult result = PdfStepResult::failure(status);
  if (status) {
    result = stepTrackedPdfPreparation(*preparation);
    if (steps != nullptr) {
      ++*steps;
    }
    sampleRuntime();
    yieldAfterPdfPreparationStep(result);
  }
  const PdfAcceptanceFramebufferSnapshot framebufferBefore = qemuFramebufferSnapshot(renderer);
  if (framebufferBefore.hash == 0) {
    return false;
  }
  if (result.yielded()) {
    for (uint32_t step = 1; step < kMaximumPreparationSteps; ++step) {
      result = stepTrackedPdfPreparation(*preparation);
      if (steps != nullptr) {
        ++*steps;
      }
      sampleRuntime();
      yieldAfterPdfPreparationStep(result);
      if (!result.yielded()) {
        break;
      }
    }
  }
  if (counters != nullptr) {
    *counters = preparation->workCounters();
  }
  const PdfAcceptanceFramebufferSnapshot framebufferAfter =
      qemuFramebufferSnapshot(renderer);
  const bool framebufferUnchanged =
      pdfAcceptanceObserveFramebuffer(framebufferBefore, framebufferAfter,
                                      state.pdfFramebufferGuardChecks,
                                      state.pdfFramebufferGuardFailures);

  const uint8_t differentPointerSentinel = 0;
  const PdfAcceptanceFramebufferSnapshot changedHash{
      framebufferBefore.pointer, framebufferBefore.bytes,
      framebufferBefore.hash ^ 1U};
  const PdfAcceptanceFramebufferSnapshot changedPointer{
      &differentPointerSentinel, framebufferBefore.bytes, framebufferBefore.hash};
  const bool changedHashRejected =
      !pdfAcceptanceObserveFramebuffer(framebufferBefore, changedHash,
                                       state.pdfFramebufferGuardControls,
                                       state.pdfFramebufferGuardRejections);
  const bool changedPointerRejected =
      !pdfAcceptanceObserveFramebuffer(framebufferBefore, changedPointer,
                                       state.pdfFramebufferGuardControls,
                                       state.pdfFramebufferGuardRejections);
  const bool accepted = result.complete() && traced.maximumRequest <= kMaximumPreparationIoRequestBytes &&
                        framebufferUnchanged &&
                        changedHashRejected && changedPointerRejected;
  return accepted;
}

std::shared_ptr<PdfReflowDocument> loadPdfDocument(const char* path);

bool hostPdfAvailable() {
  const int input = QemuSemihost::openRead(HOST_PDF_PATH);
  if (input < 0) {
    return false;
  }
  return QemuSemihost::close(input);
}

bool importHostPdf(uint64_t* const importedBytes) {
  if (importedBytes == nullptr) {
    return false;
  }
  *importedBytes = 0;
  const int input = QemuSemihost::openRead(HOST_PDF_PATH);
  const int64_t inputLength = QemuSemihost::length(input);
  if (input < 0 || inputLength < 0) {
    QemuSemihost::close(input);
    return false;
  }
  HalFile output = Storage.open(SD_PDF_PATH, static_cast<oflag_t>(O_WRONLY | O_CREAT | O_TRUNC));
  if (!output) {
    QemuSemihost::close(input);
    return false;
  }

  static uint8_t copyBuffer[512];
  bool success = true;
  while (success && *importedBytes < static_cast<uint64_t>(inputLength)) {
    const size_t requested = static_cast<size_t>(std::min<uint64_t>(sizeof(copyBuffer),
                                                                    inputLength - *importedBytes));
    const size_t read = QemuSemihost::read(input, copyBuffer, requested);
    success = read == requested && output.write(copyBuffer, read) == read;
    *importedBytes += read;
  }
  success = success && output.sync();
  success = output.close() && success;
  success = QemuSemihost::close(input) && success;
  return success;
}

class SemihostPrint final : public Print {
 public:
  explicit SemihostPrint(const int handle) : handle_(handle) {}

  size_t write(const uint8_t byte) override { return write(&byte, 1); }
  size_t write(const uint8_t* const buffer, const size_t size) override {
    return QemuSemihost::write(handle_, buffer, size);
  }

 private:
  int handle_ = -1;
};

bool exportPdfXhtml(const PdfReflowDocument& document) {
  const int output = QemuSemihost::openReadWrite(HOST_PDF_XHTML_PATH);
  if (output < 0 || !QemuSemihost::seek(output, 0)) {
    QemuSemihost::close(output);
    return false;
  }
  SemihostPrint sink(output);
  bool success = true;
  for (int section = 0; success && section < document.getSectionCount(); ++section) {
    success = document.streamSection(section, sink, 512);
  }
  return QemuSemihost::close(output) && success;
}

bool runExternalPdf(GfxRenderer& renderer) {
  uint64_t inputBytes = 0;
  if (!checkRamBudget() || !importHostPdf(&inputBytes)) {
    esp_rom_printf("QEMU_PDF_SD_FAIL stage=import bytes=%llu\n", static_cast<unsigned long long>(inputBytes));
    return false;
  }

  const uint32_t startedAt = millis();
  const PdfAcceptanceFramebufferSnapshot framebufferBefore = qemuFramebufferSnapshot(renderer);
  TracedPdfCacheIo traced;
  auto preparation = makeUniqueNoThrow<PdfPreparation>();
  if (!preparation) {
    esp_rom_printf("QEMU_PDF_SD_FAIL stage=allocate bytes=%llu\n", static_cast<unsigned long long>(inputBytes));
    return false;
  }

  PdfStatus status = beginTrackedPdfPreparation(*preparation, preparationConfig(traced, SD_PDF_PATH, renderer));
  PdfStepResult result = PdfStepResult::failure(status);
  uint32_t steps = 0;
  uint8_t lastPhase = static_cast<uint8_t>(preparation->phase());
  uint8_t lastProgress = preparation->progressPercent();
  if (status) {
    do {
      lastPhase = static_cast<uint8_t>(preparation->phase());
      lastProgress = preparation->progressPercent();
      result = stepTrackedPdfPreparation(*preparation);
      ++steps;
      sampleRuntime();
      yieldAfterPdfPreparationStep(result);
    } while (result.yielded());
  }

  const uint8_t progress = preparation->progressPercent();
  const uint8_t phase = static_cast<uint8_t>(preparation->phase());
  const uint32_t preparedWords = preparation->totalWords();
  const size_t peakWorkspace = preparation->resourcePeakBytes();
  const PdfStatus terminalStatus = result.status;
  preparation.reset();

  const PdfAcceptanceFramebufferSnapshot framebufferAfter = qemuFramebufferSnapshot(renderer);
  const bool framebufferUnchanged = framebufferBefore.pointer == framebufferAfter.pointer &&
                                    framebufferBefore.bytes == framebufferAfter.bytes &&
                                    framebufferBefore.hash == framebufferAfter.hash;
  if (!result.complete() || !framebufferUnchanged || QemuHalControl::storageReaderCount() != 0) {
    esp_rom_printf(
        "QEMU_PDF_SD_FAIL stage=prepare error=%u offset=%llu progress=%u phase=%u last_progress=%u last_phase=%u "
        "steps=%lu readers=%lu\n",
        static_cast<unsigned>(terminalStatus.error), static_cast<unsigned long long>(terminalStatus.offset),
        static_cast<unsigned>(progress), static_cast<unsigned>(phase), static_cast<unsigned>(lastProgress),
        static_cast<unsigned>(lastPhase), static_cast<unsigned long>(steps),
        static_cast<unsigned long>(QemuHalControl::storageReaderCount()));
    return false;
  }

  const std::shared_ptr<PdfReflowDocument> document = loadPdfDocument(SD_PDF_PATH);
  if (!document) {
    esp_rom_printf("QEMU_PDF_SD_FAIL stage=load progress=%u phase=%u steps=%lu\n", static_cast<unsigned>(progress),
                   static_cast<unsigned>(phase), static_cast<unsigned long>(steps));
    return false;
  }
  ReaderLayout layout;
  if (!computeReaderLayout(renderer, &layout)) {
    esp_rom_printf("QEMU_PDF_SD_FAIL stage=layout_geometry\n");
    return false;
  }
  (void)exportPdfXhtml(*document);
  for (int sectionIndex = 0; sectionIndex < document->getSectionCount(); ++sectionIndex) {
    Section section(document, sectionIndex, renderer, "_qemu_external");
    ReaderRenderSpec spec = SETTINGS.readerRenderSpec(layout.width, layout.height, EpubRenderMode::Light);
    spec.fontId = SETTINGS.getReaderFontId();
    spec.embeddedStyle = false;
    bool imagesSuppressed = false;
    bool lowMemory = false;
    const bool built = section.createSectionFile(spec, nullptr, &imagesSuppressed, &lowMemory);
    esp_rom_printf(
        "QEMU_PDF_SD_LAYOUT section=%d built=%u low_memory=%u pages=%u images_suppressed=%u free=%lu "
        "max_alloc=%lu\n",
        sectionIndex, built ? 1U : 0U, lowMemory ? 1U : 0U, static_cast<unsigned>(section.pageCount),
        imagesSuppressed ? 1U : 0U, static_cast<unsigned long>(ESP.getFreeHeap()),
        static_cast<unsigned long>(ESP.getMaxAllocHeap()));
    sampleRuntime();
    if (!built || lowMemory || section.pageCount == 0U) {
      esp_rom_printf("QEMU_PDF_SD_FAIL stage=layout section=%d\n", sectionIndex);
      return false;
    }
  }
  const uint32_t elapsed = millis() - startedAt;
  esp_rom_printf(
      "QEMU_PDF_SD_RESULT bytes=%llu elapsed_ms=%lu steps=%lu sections=%d words=%lu prepared_words=%lu "
      "workspace_peak=%lu card=%llu reader_peak=%lu optional_content_skipped=%u\n",
      static_cast<unsigned long long>(inputBytes), static_cast<unsigned long>(elapsed),
      static_cast<unsigned long>(steps), document->getSectionCount(),
      static_cast<unsigned long>(document->getTotalWordCount()), static_cast<unsigned long>(preparedWords),
      static_cast<unsigned long>(peakWorkspace), static_cast<unsigned long long>(QemuHalControl::storageCapacity()),
      static_cast<unsigned long>(QemuHalControl::storageReaderPeak()),
      document->optionalContentWasSkipped() ? 1U : 0U);
  emitRuntimeSample();
  esp_rom_printf("QEMU_PDF_SD_PASS\n");
  return true;
}

std::shared_ptr<PdfReflowDocument> loadPdfDocument(const char* path) {
  PdfStatus status{};
  auto loaded = loadPdfHalReflowDocumentNoThrow(path, REFLOW_CACHE_DIRECTORY, &status);
  if (!loaded || !status) {
    return {};
  }
  return std::shared_ptr<PdfReflowDocument>(std::move(loaded));
}

bool loadOrCreateSection(const std::shared_ptr<ReflowDocument>& document, GfxRenderer& renderer,
                         const ReaderLayout& layout, const int sectionIndex, const char* suffix, const bool cachedOnly,
                         std::unique_ptr<Section>* output) {
  if (output == nullptr) {
    return false;
  }
  *output = makeUniqueNoThrow<Section>(document, sectionIndex, renderer, suffix);
  if (!*output) {
    return false;
  }
  const int fontId = SETTINGS.getReaderFontId();
  const EpubRenderMode renderMode = document->getFormat() == ReflowDocumentFormat::Epub
                                        ? EpubRenderMode::CrossInkDefault
                                        : EpubRenderMode::Light;
  ReaderRenderSpec spec = SETTINGS.readerRenderSpec(layout.width, layout.height, renderMode);
  spec.fontId = fontId;
  spec.embeddedStyle = document->getFormat() == ReflowDocumentFormat::Epub && SETTINGS.embeddedStyle;
  const bool loaded = (*output)->loadSectionFile(spec);
  if (loaded) {
    return (*output)->pageCount > 0;
  }
  if (cachedOnly) {
    return false;
  }
  bool imagesSuppressed = false;
  bool lowMemory = false;
  const bool created = (*output)->createSectionFile(spec, nullptr, &imagesSuppressed, &lowMemory);
  return created && !lowMemory && (*output)->pageCount > 0;
}

bool renderSectionPage(Section& section, const uint16_t pageIndex, GfxRenderer& renderer, const ReaderLayout& layout,
                       uint32_t* frame) {
  if (frame == nullptr || pageIndex >= section.pageCount) {
    return false;
  }
  section.currentPage = pageIndex;
  auto page = section.loadPageFromSectionFile();
  if (!page) {
    return false;
  }
  renderer.clearScreen(0xFF);
  page->render(renderer, SETTINGS.getReaderFontId(), layout.left, layout.top, true);
  *frame = QemuHalControl::frameCrc32();
  return true;
}

bool hashFileWithSeparator(const char* path, uint64_t* hash, size_t* bytes) {
  if (path == nullptr || hash == nullptr || bytes == nullptr) {
    return false;
  }
  HalFile file = Storage.open(path, O_RDONLY);
  if (!file) {
    return false;
  }
  std::array<uint8_t, 128> buffer{};
  while (true) {
    const int count = file.read(buffer.data(), buffer.size());
    if (count < 0) {
      file.close();
      return false;
    }
    if (count == 0) {
      break;
    }
    *hash = fnvBytes(buffer.data(), static_cast<size_t>(count), *hash);
    *bytes += static_cast<size_t>(count);
  }
  const bool closed = file.close();
  const uint8_t separator = 0;
  *hash = fnvBytes(&separator, 1, *hash);
  return closed;
}

bool hashFileExact(const char* path, uint64_t* hash, size_t* bytes) {
  if (path == nullptr || hash == nullptr || bytes == nullptr) {
    return false;
  }
  HalFile file = Storage.open(path, O_RDONLY);
  if (!file) {
    return false;
  }
  std::array<uint8_t, 128> buffer{};
  while (true) {
    const int count = file.read(buffer.data(), buffer.size());
    if (count < 0) {
      file.close();
      return false;
    }
    if (count == 0) {
      break;
    }
    *hash = fnvBytes(buffer.data(), static_cast<size_t>(count), *hash);
    *bytes += static_cast<size_t>(count);
  }
  return file.close();
}

bool epubSourceMatches() {
  uint64_t hash = kFnvOffset;
  size_t bytes = 0;
  return hashFileExact(EPUB_ORACLE_PATH, &hash, &bytes) && bytes == kExpectedEpubSourceBytes &&
         hash == kExpectedEpubSourceHash;
}

bool epubCacheResidueAbsent(const std::string& cachePath) {
  static constexpr std::array<const char*, 12> kForbiddenSuffixes = {{
      "/css_rules.cache",
      "/css_rules.cache.tmp",
      "/css_rules.cache.bak",
      "/sections/1.bin.pwi",
      "/sections/1.bin.pwi.tmp",
      "/sections/1.bin.tmp",
      "/sections/1.bin.bak",
      "/html/.tmp_1.html",
      "/progress.bin.tmp",
      "/progress.bin.bak",
      "/spine.bin.tmp",
      "/toc.bin.tmp",
  }};
  char path[PDF_CACHE_PATH_CAPACITY]{};
  for (const char* suffix : kForbiddenSuffixes) {
    const int length = std::snprintf(path, sizeof(path), "%s%s", cachePath.c_str(), suffix);
    if (length <= 0 || static_cast<size_t>(length) >= sizeof(path) || Storage.exists(path)) {
      return false;
    }
  }
  return true;
}

PdfStatus readMemoryRecord(void* context, const uint32_t ordinal, void* record, const size_t recordSize) {
  if (context == nullptr || record == nullptr) {
    return PdfStatus::failure(PdfError::InvalidArgument, ordinal);
  }
  auto& records = *static_cast<MemoryRecordContext*>(context);
  if (records.accessRequired && !records.accessOpen) {
    return PdfStatus::failure(PdfError::InvalidArgument, ordinal);
  }
  if (recordSize != records.recordSize || ordinal >= records.capacity) {
    return PdfStatus::failure(PdfError::InvalidOffset, ordinal);
  }
  std::memcpy(record, records.bytes + ordinal * recordSize, recordSize);
  return PdfStatus::success();
}

PdfStatus writeMemoryRecord(void* context, const uint32_t ordinal, const void* record, const size_t recordSize) {
  if (context == nullptr || record == nullptr) {
    return PdfStatus::failure(PdfError::InvalidArgument, ordinal);
  }
  auto& records = *static_cast<MemoryRecordContext*>(context);
  if (records.accessRequired && !records.accessOpen) {
    return PdfStatus::failure(PdfError::InvalidArgument, ordinal);
  }
  if (recordSize != records.recordSize || ordinal >= records.capacity) {
    return PdfStatus::failure(PdfError::InvalidOffset, ordinal);
  }
  std::memcpy(records.bytes + ordinal * recordSize, record, recordSize);
  return PdfStatus::success();
}

PdfFixedRecordStore memoryRecordStore(MemoryRecordContext& context) {
  return {&context, context.capacity, context.recordSize, readMemoryRecord, writeMemoryRecord};
}

PdfStatus setMemoryRecordAccess(void* context, const bool required) {
  if (context == nullptr) {
    return PdfStatus::failure(PdfError::InvalidArgument);
  }
  auto& records = *static_cast<MemoryRecordContext*>(context);
  if (!records.accessRequired || records.accessOpen == required) {
    return PdfStatus::failure(PdfError::InvalidArgument);
  }
  records.accessOpen = required;
  if (required) {
    ++records.accessOpenCount;
  } else {
    ++records.accessCloseCount;
  }
  return PdfStatus::success();
}

template <typename Stepper>
PdfStatus runPdfCoreStepper(Stepper& stepper) {
  constexpr uint32_t MAX_SLICES = 100000;
  for (uint32_t slice = 0; slice < MAX_SLICES; ++slice) {
    PdfWorkBudget budget{32, PdfLimits::SourceBufferBytes};
    const PdfStepResult result = stepper.step(budget);
    if (result.complete()) {
      return PdfStatus::success();
    }
    if (result.failed()) {
      return result.status;
    }
  }
  return PdfStatus::failure(PdfError::BudgetExhausted);
}

PdfStatus captureFirstPdfPage(void* context, const PdfPageInfo& page) {
  if (context == nullptr) {
    return PdfStatus::failure(PdfError::InvalidArgument);
  }
  auto& workspace = *static_cast<PdfCoreAcceptanceWorkspace*>(context);
  if (workspace.pageCount == 0) {
    workspace.firstPage = page;
  }
  ++workspace.pageCount;
  return PdfStatus::success();
}

PdfStatus appendReductionRun(void* context, const PdfReadingOrderItem& item) {
  if (context == nullptr) {
    return PdfStatus::failure(PdfError::InvalidArgument);
  }
  auto& sink = *static_cast<ReductionSinkContext*>(context);
  if (sink.runs == nullptr || sink.transcript == nullptr) {
    return PdfStatus::failure(PdfError::InvalidArgument);
  }
  PdfTextRun run{};
  PdfStatus status = sink.runs->readRun(item.runOrdinal, &run);
  if (!status.ok()) {
    return status;
  }
  const size_t separator = sink.length == 0 ? 0 : 1;
  if (separator + run.textLength > sink.capacity - sink.length) {
    return PdfStatus::failure(PdfError::LimitExceeded, sink.length);
  }
  if (separator != 0) {
    sink.transcript[sink.length++] = ' ';
  }
  status = sink.runs->readTextExact(item.runOrdinal, 0, reinterpret_cast<uint8_t*>(sink.transcript + sink.length),
                                    run.textLength);
  if (status.ok()) {
    sink.length += run.textLength;
  }
  return status;
}

PdfStatus writeSemanticBytes(void* context, const uint8_t* source, const size_t requested, size_t* bytesWritten) {
  if (context == nullptr || source == nullptr || bytesWritten == nullptr) {
    return PdfStatus::failure(PdfError::InvalidArgument);
  }
  auto& sink = *static_cast<SemanticSinkContext*>(context);
  if (sink.workspace == nullptr || requested > sizeof(sink.workspace->semanticOutput) - sink.length) {
    return PdfStatus::failure(PdfError::InsufficientStorage, sink.length);
  }
  std::memcpy(sink.workspace->semanticOutput + sink.length, source, requested);
  sink.length += requested;
  *bytesWritten = requested;
  return PdfStatus::success();
}

PdfStatus captureSemanticRecord(void* context, const PdfSemanticBlockRecord& record) {
  if (context == nullptr) {
    return PdfStatus::failure(PdfError::InvalidArgument);
  }
  auto& sink = *static_cast<SemanticSinkContext*>(context);
  if (sink.workspace == nullptr || sink.records != 0) {
    return PdfStatus::failure(PdfError::LimitExceeded, sink.records);
  }
  sink.workspace->semanticRecord = record;
  ++sink.records;
  return PdfStatus::success();
}

bool checkPdfSemantic(PdfCoreAcceptanceWorkspace& workspace) {
  static constexpr char EXPECTED[] =
      "<?xml version=\"1.0\" encoding=\"UTF-8\"?>"
      "<html xmlns=\"http://www.w3.org/1999/xhtml\"><head><meta charset=\"UTF-8\"/></head><body>"
      "<p id=\"b00000000\">First Second</p></body></html>";
  std::memset(workspace.semanticOutput, 0, sizeof(workspace.semanticOutput));
  workspace.semanticRecord = {};
  SemanticSinkContext sink{&workspace};
  PdfSemanticWriter writer;
  PdfStatus status = writer.begin({&sink, writeSemanticBytes}, {&sink, captureSemanticRecord},
                                  {workspace.semanticBuffer, sizeof(workspace.semanticBuffer)});
  if (status.ok()) {
    status = writer.beginBlock({PdfSemanticBlockKind::Paragraph, 0, 0});
  }
  if (status.ok()) {
    status =
        writer.writeText(reinterpret_cast<const uint8_t*>(workspace.transcript), std::strlen(workspace.transcript));
  }
  if (status.ok()) {
    status = writer.endBlock();
  }
  if (status.ok()) {
    status = writer.finish();
  }
  if (!status.ok() || sink.records != 1 || writer.totalWords() != 2 || sink.length != sizeof(EXPECTED) - 1 ||
      workspace.semanticRecord.cumulativeWordStart != 0 || workspace.semanticRecord.wordCount != 2 ||
      std::strcmp(workspace.semanticRecord.anchor, "b00000000") != 0 ||
      std::memcmp(workspace.semanticOutput, EXPECTED, sizeof(EXPECTED) - 1) != 0) {
    fail("PDF_SEMANTIC", "result");
    return false;
  }
  esp_rom_printf("QEMU_PDF_SEMANTIC_PASS words=%lu bytes=%lu\n", static_cast<unsigned long>(writer.totalWords()),
                 static_cast<unsigned long>(sink.length));
  return true;
}

bool checkPdfReduction(PdfCoreAcceptanceWorkspace& workspace) {
  const PdfRectangle page{
      PdfFixed16::fromInteger(0).raw,
      PdfFixed16::fromInteger(0).raw,
      PdfFixed16::fromInteger(612).raw,
      PdfFixed16::fromInteger(792).raw,
  };
  PdfRunStore runs({
      workspace.reductionRuns,
      static_cast<uint16_t>(sizeof(workspace.reductionRuns) / sizeof(workspace.reductionRuns[0])),
      workspace.reductionText,
      sizeof(workspace.reductionText),
      {},
      {},
  });
  PdfStatus status = runs.reset();
  PdfTextRun second{};
  second.sourceOrder = 1;
  second.xMin = PdfFixed16::fromInteger(72).raw;
  second.xMax = PdfFixed16::fromInteger(140).raw;
  second.yMin = PdfFixed16::fromInteger(650).raw;
  second.yMax = PdfFixed16::fromInteger(662).raw;
  second.baselineX = second.xMin;
  second.baseline = second.yMin;
  second.baselineDx = second.xMax - second.xMin;
  PdfTextRun first = second;
  first.sourceOrder = 0;
  first.yMin = PdfFixed16::fromInteger(710).raw;
  first.yMax = PdfFixed16::fromInteger(722).raw;
  first.baseline = first.yMin;
  static constexpr uint8_t SECOND_TEXT[] = "Second";
  static constexpr uint8_t FIRST_TEXT[] = "First";
  if (status.ok()) {
    status = runs.append(second, SECOND_TEXT, sizeof(SECOND_TEXT) - 1);
  }
  if (status.ok()) {
    status = runs.append(first, FIRST_TEXT, sizeof(FIRST_TEXT) - 1);
  }

  std::memset(workspace.transcript, 0, sizeof(workspace.transcript));
  ReductionSinkContext sink{&runs, workspace.transcript, sizeof(workspace.transcript), 0};
  PdfReadingOrderReducer reducer({
      workspace.reductionOrder,
      static_cast<uint16_t>(sizeof(workspace.reductionOrder) / sizeof(workspace.reductionOrder[0])),
  });
  uint32_t emitted = 0;
  if (status.ok()) {
    status = reducer.reduce(runs, page, 0, nullptr, 0, {&sink, appendReductionRun}, &emitted);
  }

  static constexpr uint8_t OCR_TEXT[] = "Readable OCR";
  PdfTextRun hidden{};
  hidden.textLength = sizeof(OCR_TEXT) - 1;
  hidden.xMin = PdfFixed16::fromInteger(72).raw;
  hidden.xMax = PdfFixed16::fromInteger(170).raw;
  hidden.yMin = PdfFixed16::fromInteger(620).raw;
  hidden.yMax = PdfFixed16::fromInteger(632).raw;
  hidden.baselineX = hidden.xMin;
  hidden.baseline = hidden.yMin;
  hidden.baselineDx = hidden.xMax - hidden.xMin;
  hidden.flags = PdfTextHidden;
  PdfImagePlacement image{};
  image.xMin = PdfFixed16::fromInteger(60).raw;
  image.xMax = PdfFixed16::fromInteger(240).raw;
  image.yMin = PdfFixed16::fromInteger(560).raw;
  image.yMax = PdfFixed16::fromInteger(720).raw;
  const PdfHiddenTextContext hiddenContext{
      page, &hidden, 1, OCR_TEXT, sizeof(OCR_TEXT) - 1, &image, 1,
  };
  const PdfHiddenTextDecision hiddenDecision = pdfClassifyHiddenText(hiddenContext, 0);

  PdfDocumentTextClassifier classifier;
  PdfStatus classifierStatus = classifier.begin(1);
  if (classifierStatus.ok()) {
    classifierStatus = classifier.observePage(0, {11, 0, 1});
  }
  if (classifierStatus.ok()) {
    classifierStatus = classifier.finish(PdfStatus::success());
  }

  static constexpr char EXPECTED[] = "First Second";
  if (!status.ok() || !classifierStatus.ok() || hiddenDecision != PdfHiddenTextDecision::Qualified || emitted != 2 ||
      sink.length != sizeof(EXPECTED) - 1 || std::memcmp(workspace.transcript, EXPECTED, sink.length) != 0) {
    fail("PDF_REFLOW", "reduction");
    return false;
  }

  HalFile spill = Storage.open(PDF_SPILL_PATH, static_cast<oflag_t>(O_RDWR | O_CREAT | O_TRUNC));
  PdfHalByteStoreContext spillContext;
  status = spill ? pdfInitializeHalByteStore(&spillContext, spill, 128) : PdfStatus::failure(PdfError::IoFailure);
  static constexpr uint8_t SPILL_TEXT[] = "spill";
  if (status.ok()) {
    PdfByteStore byteStore = pdfHalByteStore(spillContext);
    status = pdfWriteExact(pdfByteStoreSink(byteStore), SPILL_TEXT, sizeof(SPILL_TEXT) - 1);
    uint8_t roundTrip[sizeof(SPILL_TEXT) - 1]{};
    if (status.ok()) {
      status = pdfReadExact(pdfByteStoreSource(byteStore), 0, roundTrip, sizeof(roundTrip));
    }
    if (status.ok() && std::memcmp(roundTrip, SPILL_TEXT, sizeof(roundTrip)) != 0) {
      status = PdfStatus::failure(PdfError::Malformed);
    }
  }
  if (status.ok()) {
    const PdfFixedRecordStore recordStore = pdfHalFixedRecordStore(spill, sizeof(PdfTextRun), 1);
    status = pdfWriteRecord(recordStore, 0, &first);
    PdfTextRun roundTrip{};
    if (status.ok()) {
      status = pdfReadRecord(recordStore, 0, &roundTrip);
    }
    if (status.ok() && (roundTrip.sourceOrder != first.sourceOrder || roundTrip.xMin != first.xMin ||
                        roundTrip.baseline != first.baseline)) {
      status = PdfStatus::failure(PdfError::Malformed);
    }
  }
  const bool spillClosed = spill.close();
  const bool spillRemoved = Storage.remove(PDF_SPILL_PATH);
  if (!status.ok() || !spillClosed || !spillRemoved) {
    fail("PDF_REFLOW", "hal_spill");
    return false;
  }
  esp_rom_printf("QEMU_PDF_REFLOW_PASS runs=%lu bytes=%lu\n", static_cast<unsigned long>(emitted),
                 static_cast<unsigned long>(sink.length));
  return true;
}

bool checkPdfCore() {
  const uint32_t opensBefore = QemuHalControl::storageOpenCount();
  const uint32_t closesBefore = QemuHalControl::storageCloseCount();
  HalFile file = Storage.open(PDF_FIXTURE_PATH, O_RDONLY);
  if (!file) {
    fail("PDF_CORE", "open");
    return false;
  }

  auto workspace = makeUniqueNoThrow<PdfCoreAcceptanceWorkspace>();
  if (!workspace) {
    file.close();
    fail("PDF_CORE", "workspace_oom");
    return false;
  }

  const PdfByteSource source = pdfHalByteSource(file);
  PdfObjectArena arena{
      workspace->values,
      static_cast<uint16_t>(sizeof(workspace->values) / sizeof(workspace->values[0])),
      workspace->dictionaryEntries,
      static_cast<uint16_t>(sizeof(workspace->dictionaryEntries) / sizeof(workspace->dictionaryEntries[0])),
      workspace->arrayItems,
      static_cast<uint16_t>(sizeof(workspace->arrayItems) / sizeof(workspace->arrayItems[0])),
      workspace->objectText,
      static_cast<uint16_t>(sizeof(workspace->objectText)),
  };
  MemoryRecordContext xrefRecords{
      reinterpret_cast<uint8_t*>(workspace->xrefEntries),
      sizeof(PdfXrefEntry),
      static_cast<uint32_t>(sizeof(workspace->xrefEntries) / sizeof(workspace->xrefEntries[0])),
  };
  MemoryRecordContext traversalRecords{
      reinterpret_cast<uint8_t*>(workspace->traversalRecords),
      sizeof(PdfPageTreeRecord),
      static_cast<uint32_t>(sizeof(workspace->traversalRecords) / sizeof(workspace->traversalRecords[0])),
  };
  traversalRecords.accessRequired = true;
  PdfXrefTable xref(memoryRecordStore(xrefRecords));
  auto xrefParser =
      makeUniqueNoThrow<PdfXrefParser>(source, workspace->sourceBuffer, sizeof(workspace->sourceBuffer), arena, xref);
  if (!xrefParser) {
    file.close();
    fail("PDF_CORE", "xref_oom");
    return false;
  }
  xrefParser->begin();
  PdfStatus status = runPdfCoreStepper(*xrefParser);
  xrefParser.reset();
  if (!status.ok()) {
    file.close();
    fail("PDF_CORE", "xref");
    return false;
  }

  PdfObjectReference catalogReference;
  if (!xref.root(&catalogReference)) {
    file.close();
    fail("PDF_CORE", "root");
    return false;
  }
  auto resolver = makeUniqueNoThrow<PdfObjectResolver>(source, xref, workspace->sourceBuffer,
                                                       sizeof(workspace->sourceBuffer), arena);
  if (!resolver) {
    file.close();
    fail("PDF_CORE", "resolver_oom");
    return false;
  }
  status = resolver->begin(catalogReference);
  if (status.ok()) {
    status = runPdfCoreStepper(*resolver);
  }
  uint16_t pagesIndex = PDF_INVALID_INDEX;
  if (!status.ok() || !pdfDictionaryFind(arena, resolver->result().rootIndex, "Pages", &pagesIndex) ||
      pagesIndex >= arena.valueCount || arena.values[pagesIndex].kind != PdfValueKind::Reference) {
    file.close();
    fail("PDF_CORE", "catalog");
    return false;
  }
  const PdfObjectReference pagesReference{
      arena.values[pagesIndex].objectNumber,
      arena.values[pagesIndex].generation,
  };

  auto walker = makeUniqueNoThrow<PdfPageTreeWalker>(
      *resolver, arena, memoryRecordStore(traversalRecords), captureFirstPdfPage, workspace.get(),
      setMemoryRecordAccess, &traversalRecords, &workspace->pageScratch);
  if (!walker) {
    file.close();
    fail("PDF_CORE", "page_tree_oom");
    return false;
  }
  status = walker->begin(pagesReference);
  if (status.ok()) {
    status = runPdfCoreStepper(*walker);
  }
  walker.reset();
  if (!status.ok() || workspace->pageCount != 1 || workspace->firstPage.contentCount != 1 ||
      traversalRecords.accessOpenCount == 0 ||
      traversalRecords.accessOpenCount != traversalRecords.accessCloseCount || traversalRecords.accessOpen) {
    file.close();
    fail("PDF_CORE", "page_tree");
    return false;
  }

  status = resolver->begin(workspace->firstPage.contents[0]);
  if (status.ok()) {
    status = runPdfCoreStepper(*resolver);
  }
  if (!status.ok() || !resolver->result().hasStream) {
    file.close();
    fail("PDF_CORE", "content");
    return false;
  }
  const PdfResolvedObject content = resolver->result();
  resolver.reset();

  PdfByteRange streamRange;
  status = pdfInitializeByteRange(source, content.streamOffset, content.streamLength, &streamRange);
  if (!status.ok()) {
    file.close();
    fail("PDF_CORE", "stream_range");
    return false;
  }
  auto contentLexer = makeUniqueNoThrow<PdfLexer>(pdfByteRangeSource(streamRange), workspace->sourceBuffer,
                                                  sizeof(workspace->sourceBuffer));
  if (!contentLexer) {
    file.close();
    fail("PDF_CORE", "content_lexer_oom");
    return false;
  }
  size_t transcriptLength = 0;
  while (true) {
    PdfToken token;
    PdfStepResult tokenResult;
    do {
      PdfWorkBudget budget{32, PdfLimits::SourceBufferBytes};
      tokenResult = contentLexer->next(token, budget);
    } while (tokenResult.yielded());
    if (tokenResult.failed()) {
      file.close();
      fail("PDF_CORE", "content_lex");
      return false;
    }
    if (token.kind == PdfTokenKind::End) {
      break;
    }
    if (token.kind != PdfTokenKind::String) {
      continue;
    }
    const size_t separator = transcriptLength == 0 ? 0 : 1;
    if (transcriptLength + separator + token.length >= sizeof(workspace->transcript)) {
      file.close();
      fail("PDF_CORE", "text_limit");
      return false;
    }
    if (separator != 0) {
      workspace->transcript[transcriptLength++] = ' ';
    }
    std::memcpy(workspace->transcript + transcriptLength, token.bytes, token.length);
    transcriptLength += token.length;
  }

  const bool adapterKeptFileOpen = QemuHalControl::storageCloseCount() == closesBefore;
  const bool textMatches = transcriptLength == sizeof(PDF_EXPECTED_TEXT) - 1 &&
                           std::memcmp(workspace->transcript, PDF_EXPECTED_TEXT, transcriptLength) == 0;
  const bool closed = file.close();
  const bool countersBalanced =
      QemuHalControl::storageOpenCount() == opensBefore + 1 && QemuHalControl::storageCloseCount() == closesBefore + 1;
  if (!adapterKeptFileOpen || !textMatches || !closed || !countersBalanced) {
    fail("PDF_CORE", "result");
    return false;
  }

  esp_rom_printf("QEMU_PDF_CORE bytes=%lu\n", static_cast<unsigned long>(transcriptLength));
  esp_rom_printf("QEMU_PDF_CORE_PASS\n");
  return checkPdfReduction(*workspace) && checkPdfSemantic(*workspace);
}

bool checkPdfCache() {
  if (Storage.exists(PDF_CACHE_ROOT) && !Storage.removeDir(PDF_CACHE_ROOT)) {
    fail("PDF_CACHE", "preclean");
    return false;
  }
  const uint32_t opensBefore = QemuHalControl::storageOpenCount();
  const uint32_t closesBefore = QemuHalControl::storageCloseCount();
  auto fingerprintWorkspace = makeUniqueNoThrow<uint8_t[]>(PDF_SOURCE_FINGERPRINT_BYTES);
  if (!fingerprintWorkspace) {
    fail("PDF_CACHE", "workspace_oom");
    return false;
  }

  PdfHalCacheIoContext halContext;
  const PdfCacheIo io = pdfHalCacheIo(halContext);
  PdfCacheCapacity capacity{};
  PdfSourceIdentity sourceIdentity{};
  PdfStatus status = io.capacity(io.context, &capacity);
  if (status.ok()) {
    status = pdfComputeSourceIdentity(io, PDF_FIXTURE_PATH, fingerprintWorkspace.get(), PDF_SOURCE_FINGERPRINT_BYTES,
                                      &sourceIdentity);
  }
  PdfCacheStore store;
  if (status.ok()) {
    status = store.initialize(io, PDF_CACHE_ROOT);
  }
  if (status.ok()) {
    status = store.ensureGeneration(1);
  }

  static constexpr uint8_t METADATA[] = {'m', 'e', 't', 'a'};
  PdfCacheTrackedWriter writer{};
  if (status.ok()) {
    status = pdfOpenTrackedCacheWriter(io, PDF_CACHE_METADATA_PATH, "metadata.bin", PdfCacheFileKind::Required,
                                       sizeof(METADATA), &writer);
  }
  if (status.ok()) {
    status = pdfWriteTrackedCacheFile(&writer, METADATA, sizeof(METADATA));
  }
  SingleCacheRecordSource table;
  if (status.ok()) {
    status = pdfCloseTrackedCacheFile(&writer, &table.record);
  } else if (writer.open) {
    pdfAbortTrackedCacheFile(&writer);
  }

  PdfCacheManifest committedManifest{};
  committedManifest.sequence = 1;
  committedManifest.completed = true;
  committedManifest.source = sourceIdentity;
  committedManifest.generation = 1;
  committedManifest.totalWords = 2;
  committedManifest.requiredFileCount = 1;
  committedManifest.requiredFileBytes = table.record.size;
  committedManifest.requiredFileLedger = pdfUpdateRequiredFileLedger(PDF_CACHE_FNV64_OFFSET, table.record);
  PdfCacheManifestSelection prior{};
  PdfCacheManifestSelection committed{};
  if (status.ok()) {
    status = store.loadManifestSlots(sourceIdentity, &prior);
  }
  if (status.ok()) {
    status = store.commitManifest(committedManifest, table.source(),
                                  {true, committedManifest.requiredFileCount, committedManifest.requiredFileBytes,
                                   committedManifest.requiredFileLedger},
                                  prior, &committed);
  }

  PdfBuildCheckpoint checkpoint{};
  checkpoint.sequence = 1;
  checkpoint.source = sourceIdentity;
  checkpoint.generation = 1;
  checkpoint.phase = PdfBuildPhase::Complete;
  checkpoint.lastVerifiedPage = 1;
  checkpoint.emittedSections = 1;
  checkpoint.cumulativeWords = 2;
  checkpoint.outputBytes = sizeof(METADATA);
  if (status.ok()) {
    status = store.commitCheckpoint(checkpoint);
  }
  PdfBuildCheckpointSelection recoveredCheckpoint{};
  if (status.ok()) {
    status = store.loadCheckpointSlots(sourceIdentity, &recoveredCheckpoint);
  }

  if (status.ok()) {
    status = store.ensureGeneration(2);
  }
  PdfCacheTrackedWriter partialWriter{};
  if (status.ok()) {
    status = pdfOpenTrackedCacheWriter(io, PDF_CACHE_PARTIAL_PATH, "partial.bin", PdfCacheFileKind::Optional,
                                       sizeof(METADATA), &partialWriter);
  }
  PdfRequiredFileRecord ignoredRecord{};
  if (status.ok()) {
    status = pdfWriteTrackedCacheFile(&partialWriter, METADATA, sizeof(METADATA));
  }
  if (status.ok()) {
    status = pdfCloseTrackedCacheFile(&partialWriter, &ignoredRecord);
  } else if (partialWriter.open) {
    pdfAbortTrackedCacheFile(&partialWriter);
  }
  if (status.ok()) {
    status = store.cleanupUnreferencedGenerations();
  }

  fingerprintWorkspace.reset();
  const bool resultMatches = status.ok() && capacity.total.known && capacity.free.known && committed.selected &&
                             committed.manifest.sequence == 1 && recoveredCheckpoint.selected &&
                             recoveredCheckpoint.checkpoint.sequence == 1 && Storage.exists(PDF_CACHE_GENERATION_ONE) &&
                             !Storage.exists(PDF_CACHE_GENERATION_TWO);
  const bool cleaned = Storage.exists(PDF_CACHE_ROOT) && Storage.removeDir(PDF_CACHE_ROOT);
  const bool countersBalanced =
      QemuHalControl::storageOpenCount() - opensBefore == QemuHalControl::storageCloseCount() - closesBefore;
  if (!resultMatches || !cleaned || !countersBalanced) {
    fail("PDF_CACHE", !status.ok() ? "transaction" : (!cleaned ? "cleanup" : "result"));
    return false;
  }
  esp_rom_printf("QEMU_PDF_CACHE_PASS files=1 words=2 capacity=%llu free=%llu\n",
                 static_cast<unsigned long long>(capacity.total.value),
                 static_cast<unsigned long long>(capacity.free.value));
  return true;
}

bool clearPdfCache(const char* sourcePath) {
  char cacheRoot[PDF_CACHE_PATH_CAPACITY]{};
  const PdfStatus status = pdfFormatCacheRoot(REFLOW_CACHE_DIRECTORY, sourcePath, cacheRoot, sizeof(cacheRoot));
  return status && (!Storage.exists(cacheRoot) || Storage.removeDir(cacheRoot));
}

bool checkRawPdfFixtures() {
  std::array<uint8_t, 128> buffer{};
  for (const RawFixtureSpec& fixture : kRawFixtures) {
    HalFile file = Storage.open(fixture.path, O_RDONLY);
    if (!file || file.fileSize64() != fixture.bytes) {
      file.close();
      fail("PDF_RAW", "open_or_size");
      return false;
    }
    uint64_t hash = kFnvOffset;
    uint32_t bytes = 0;
    while (true) {
      const int count = file.read(buffer.data(), buffer.size());
      if (count < 0) {
        file.close();
        fail("PDF_RAW", "read");
        return false;
      }
      if (count == 0) {
        break;
      }
      hash = fnvBytes(buffer.data(), static_cast<size_t>(count), hash);
      bytes += static_cast<uint32_t>(count);
    }
    const bool closed = file.close();
    if (!closed || bytes != fixture.bytes || hash != fixture.fnv) {
      fail("PDF_RAW", "mutated");
      return false;
    }
  }
  esp_rom_printf("QEMU_PDF_RAW_PASS files=15 unchanged=15\n");
  return true;
}

bool checkPdfCancellation(GfxRenderer& renderer, PersistentAcceptanceState* persistent) {
  if (persistent == nullptr || !clearPdfCache(PDF_FRESH_FIXTURE_PATH) || !clearPdfCache(PDF_FIXTURE_PATH)) {
    fail("PDF_CANCEL", "preclean");
    return false;
  }

  persistent->freshSteps = 0;
  if (!preparePdf(PDF_FRESH_FIXTURE_PATH, renderer, &persistent->freshSteps, &persistent->freshWork) ||
      persistent->freshSteps == 0) {
    fail("PDF_CANCEL", "fresh_baseline");
    return false;
  }

  TracedPdfCacheIo traced;
  auto preparation = makeUniqueNoThrow<PdfPreparation>();
  if (!preparation) {
    fail("PDF_CANCEL", "oom");
    return false;
  }
  PdfStatus status = beginTrackedPdfPreparation(*preparation, preparationConfig(traced, PDF_FIXTURE_PATH, renderer));
  uint32_t setupSteps = 0;
  PdfStepResult result = PdfStepResult::paused();
  while (status && result.yielded() && preparation->workCounters().sectionsEmitted == 0 &&
         setupSteps < kMaximumPreparationSteps) {
    result = stepTrackedPdfPreparation(*preparation);
    ++setupSteps;
    sampleRuntime();
    yieldAfterPdfPreparationStep(result);
  }
  if (!status || !result.yielded() || preparation->workCounters().sectionsEmitted == 0 ||
      preparation->generation() == 0) {
    fail("PDF_CANCEL", "retained_checkpoint");
    return false;
  }

  persistent->generation = preparation->generation();
  preparation->requestCancel();
  bool terminal = false;
#ifdef CROSSINK_QEMU_TIMING_DIAGNOSTIC
  timingDiagnosticRecordCount = 0;
#endif
  uint32_t cancellationMaxSliceUs = 0;
  uint32_t cancellationMaxCallbackUs = 0;
  TracedPdfCacheIo::Operation cancellationMaxCallbackOperation = TracedPdfCacheIo::Operation::None;
  QemuSlowAtomicTotals slowAtomicTotals;
  for (uint16_t slice = 0; slice < kMaximumCancellationSlices; ++slice) {
    traced.resetSliceTrace();
    const uint32_t startedAtUs = micros();
    const uint32_t startedAt = millis();
    result = stepTrackedPdfPreparation(*preparation);
    const uint32_t elapsed = millis() - startedAt;
    const uint32_t elapsedUs = micros() - startedAtUs;
    const TracedPdfCacheIo::SliceTrace sliceTrace = traced.sliceTrace();
    const uint32_t ioCalls = sliceTrace.calls;
    const uint32_t nonIoUs =
        sliceTrace.callbackElapsedUs > elapsedUs ? 0 : elapsedUs - sliceTrace.callbackElapsedUs;
    ++persistent->cancellationSlices;
    ++persistent->cancellationSteps;
    persistent->cancellationMaxSliceMs = std::max(persistent->cancellationMaxSliceMs, elapsed);
    cancellationMaxSliceUs = std::max(cancellationMaxSliceUs, elapsedUs);
    if (sliceTrace.callbackElapsedUs > cancellationMaxCallbackUs) {
      cancellationMaxCallbackUs = sliceTrace.callbackElapsedUs;
      cancellationMaxCallbackOperation = sliceTrace.operation;
    }
    persistent->cancellationMaxSliceIo = std::max(persistent->cancellationMaxSliceIo, ioCalls);
    persistent->maximumIoRequest =
        std::max<uint32_t>(persistent->maximumIoRequest, static_cast<uint32_t>(traced.maximumRequest));
    sampleRuntime();
    const bool timingExceeded = elapsedUs > kMaximumCancellationSliceMicroseconds;
    const bool slowAtomicAllowed =
        timingExceeded && qemuSlowAtomicAllowed(sliceTrace, elapsedUs, nonIoUs) &&
        recordQemuSlowAtomic(slowAtomicTotals, slice, elapsedUs, sliceTrace, nonIoUs);
    const bool timingViolation = timingExceeded && !slowAtomicAllowed;
    const bool otherLimitExceeded = ioCalls > kMaximumCancellationSliceOperations ||
                                    traced.maximumRequest > kMaximumPreparationIoRequestBytes ||
                                    preparation->generation() != persistent->generation;
#ifdef CROSSINK_QEMU_TIMING_DIAGNOSTIC
    if (timingExceeded) {
      recordTimingDiagnostic(slice, elapsedUs, sliceTrace, nonIoUs);
    }
    if (timingViolation || otherLimitExceeded) {
#else
    if (timingViolation || otherLimitExceeded) {
#endif
      esp_rom_printf(
          "QEMU_PDF_CANCEL_FAIL reason=slice_budget elapsed_ms=%lu "
          "elapsed_us=%lu io_calls=%lu io_kind=%s io_mode=%s io_recursive=%u "
          "io_request=%lu callback_us=%lu nonio_us=%lu "
          "max_io_request=%lu generation=%lu "
          "expected_generation=%lu\n",
          static_cast<unsigned long>(elapsed), static_cast<unsigned long>(elapsedUs),
          static_cast<unsigned long>(ioCalls), TracedPdfCacheIo::operationName(sliceTrace.operation),
          TracedPdfCacheIo::openModeName(sliceTrace.openMode), static_cast<unsigned>(sliceTrace.recursive),
          static_cast<unsigned long>(sliceTrace.requestBytes),
          static_cast<unsigned long>(sliceTrace.callbackElapsedUs), static_cast<unsigned long>(nonIoUs),
          static_cast<unsigned long>(traced.maximumRequest),
          static_cast<unsigned long>(preparation->generation()),
          static_cast<unsigned long>(persistent->generation));
      return false;
    }
    if (result.yielded()) {
      yieldAfterPdfPreparationStep(result);
      continue;
    }
    terminal = result.failed() && result.status.error == PdfError::Cancelled;
    break;
  }
  if (!terminal || persistent->cancellationSlices < 2 || !savePersistentState(*persistent)) {
    fail("PDF_CANCEL", !terminal ? "terminal" : "state");
    return false;
  }
  emitQemuSlowAtomicSummary(slowAtomicTotals, persistent->generation, persistent->cancellationSlices);

#ifdef CROSSINK_QEMU_TIMING_DIAGNOSTIC
  emitTimingDiagnostics(persistent->cancellationSlices);
  esp_rom_printf(
      "QEMU_PDF_CANCEL_DIAGNOSTIC generation=%lu steps=%lu cancel_slices=%u violations=%u "
      "max_slice_us=%lu max_callback_us=%lu max_callback_kind=%s max_slice_io=%lu max_io_request=%lu\n",
      static_cast<unsigned long>(persistent->generation),
      static_cast<unsigned long>(setupSteps + persistent->cancellationSteps),
      static_cast<unsigned>(persistent->cancellationSlices), static_cast<unsigned>(timingDiagnosticRecordCount),
      static_cast<unsigned long>(cancellationMaxSliceUs), static_cast<unsigned long>(cancellationMaxCallbackUs),
      TracedPdfCacheIo::operationName(cancellationMaxCallbackOperation),
      static_cast<unsigned long>(persistent->cancellationMaxSliceIo),
      static_cast<unsigned long>(persistent->maximumIoRequest));
#else
  esp_rom_printf(
      "QEMU_PDF_CANCEL_PASS generation=%lu steps=%lu cancel_slices=%u "
      "max_slice_ms=%lu max_slice_us=%lu max_callback_us=%lu "
      "max_callback_kind=%s "
      "max_slice_io=%lu max_io_request=%lu\n",
      static_cast<unsigned long>(persistent->generation),
      static_cast<unsigned long>(setupSteps + persistent->cancellationSteps),
      static_cast<unsigned>(persistent->cancellationSlices),
      static_cast<unsigned long>(persistent->cancellationMaxSliceMs),
      static_cast<unsigned long>(cancellationMaxSliceUs), static_cast<unsigned long>(cancellationMaxCallbackUs),
      TracedPdfCacheIo::operationName(cancellationMaxCallbackOperation),
      static_cast<unsigned long>(persistent->cancellationMaxSliceIo),
      static_cast<unsigned long>(persistent->maximumIoRequest));
#endif
  return true;
}

bool workCountersResumeLess(const PdfPreparationWorkCounters& resumed, const PdfPreparationWorkCounters& fresh) {
  const bool bounded = resumed.xrefSteps <= fresh.xrefSteps && resumed.pagesWalked <= fresh.pagesWalked &&
                       resumed.contentTokens <= fresh.contentTokens &&
                       resumed.sectionsEmitted <= fresh.sectionsEmitted &&
                       resumed.imagesEmitted <= fresh.imagesEmitted && resumed.sourceBytesRead <= fresh.sourceBytesRead;
  const bool strictlyLess =
      resumed.xrefSteps < fresh.xrefSteps || resumed.pagesWalked < fresh.pagesWalked ||
      resumed.contentTokens < fresh.contentTokens || resumed.sectionsEmitted < fresh.sectionsEmitted ||
      resumed.imagesEmitted < fresh.imagesEmitted || resumed.sourceBytesRead < fresh.sourceBytesRead;
  return bounded && strictlyLess;
}

bool checkPdfResume(GfxRenderer& renderer, const PersistentAcceptanceState& persistent) {
  char cacheRoot[PDF_CACHE_PATH_CAPACITY]{};
  char retainedPath[PDF_CACHE_PATH_CAPACITY]{};
  PdfStatus status = pdfFormatCacheRoot(REFLOW_CACHE_DIRECTORY, PDF_FIXTURE_PATH, cacheRoot, sizeof(cacheRoot));
  const int pathLength = std::snprintf(retainedPath, sizeof(retainedPath), "%s/gen_%lu/sections/000000.xhtml",
                                       cacheRoot, static_cast<unsigned long>(persistent.generation));
  if (!status || pathLength <= 0 || static_cast<size_t>(pathLength) >= sizeof(retainedPath) ||
      !Storage.exists(retainedPath)) {
    fail("PDF_RESUME", "retained_path");
    return false;
  }

  TracedPdfCacheIo traced;
  traced.trackRetainedPath(retainedPath);
  auto preparation = makeUniqueNoThrow<PdfPreparation>();
  if (!preparation) {
    fail("PDF_RESUME", "oom");
    return false;
  }
  status = beginTrackedPdfPreparation(*preparation, preparationConfig(traced, PDF_FIXTURE_PATH, renderer));
  uint32_t resumedSteps = 0;
  const PdfStepResult result = status ? runPreparation(*preparation, &resumedSteps) : PdfStepResult::failure(status);
  const PdfPreparationWorkCounters resumedWork = preparation->workCounters();
  const bool retained = traced.retainedWriteTruncate == 0 && traced.retainedRemove == 0;
  if (!result.complete() || !preparation->resumedFromCheckpoint() ||
      preparation->generation() != persistent.generation || resumedSteps >= persistent.freshSteps ||
      !workCountersResumeLess(resumedWork, persistent.freshWork) || !retained) {
    fail("PDF_RESUME", !retained ? "retained_rewritten" : "continuation");
    return false;
  }

  esp_rom_printf(
      "QEMU_PDF_RESUME_PASS generation=%lu resumed=1 fresh_steps=%lu resumed_steps=%lu retained_truncate=%lu "
      "retained_remove=%lu\n",
      static_cast<unsigned long>(persistent.generation), static_cast<unsigned long>(persistent.freshSteps),
      static_cast<unsigned long>(resumedSteps), static_cast<unsigned long>(traced.retainedWriteTruncate),
      static_cast<unsigned long>(traced.retainedRemove));
  return true;
}

bool captureTypographySignature(const char* path, const char* suffix, GfxRenderer& renderer, const ReaderLayout& layout,
                                TypographySignature* signature) {
  if (signature == nullptr) {
    return false;
  }
  const auto document = loadPdfDocument(path);
  if (!document || document->getTotalWordCount() != 4 || document->getSectionCount() != 1) {
    return false;
  }
  SemanticTextFnvPrint semantic;
  if (!document->streamSection(0, semantic, 256)) {
    return false;
  }
  std::unique_ptr<Section> section;
  if (!loadOrCreateSection(document, renderer, layout, 0, suffix, false, &section)) {
    return false;
  }
  uint64_t textHash = kFnvOffset;
  for (uint16_t pageIndex = 0; pageIndex < section->pageCount; ++pageIndex) {
    section->currentPage = pageIndex;
    const std::string text = section->getTextFromSectionFile();
    textHash = fnvBytes(reinterpret_cast<const uint8_t*>(text.data()), text.size(), textHash);
    const uint8_t separator = 0;
    textHash = fnvBytes(&separator, 1, textHash);
  }
  uint32_t frame = 0;
  if (!renderSectionPage(*section, 0, renderer, layout, &frame)) {
    return false;
  }
  *signature = {semantic.hash(), textHash, frame, document->getTotalWordCount(), section->pageCount};
  return true;
}

bool checkPdfTypography(GfxRenderer& renderer) {
  if (!preparePdf(PDF_FONT_SIX_PATH, renderer)) {
    fail("PDF_TYPOGRAPHY", "prepare_six");
    return false;
  }
  if (!preparePdf(PDF_FONT_SEVENTY_TWO_PATH, renderer)) {
    fail("PDF_TYPOGRAPHY", "prepare_seventy_two");
    return false;
  }
  ReaderLayout layout;
  TypographySignature six;
  TypographySignature seventyTwo;
  if (!computeReaderLayout(renderer, &layout)) {
    fail("PDF_TYPOGRAPHY", "layout");
    return false;
  }
  renderer.clearScreen(0xFF);
  const uint32_t blankFrame = QemuHalControl::frameCrc32();
  if (
      !captureTypographySignature(PDF_FONT_SIX_PATH, "_qemu_pdf_typography", renderer, layout, &six) ||
      !captureTypographySignature(PDF_FONT_SEVENTY_TWO_PATH, "_qemu_pdf_typography", renderer, layout, &seventyTwo) ||
      six.semantic != kExpectedPdfTypographySemanticHash || six.text != kExpectedPdfTypographyTextHash ||
      six.semantic != seventyTwo.semantic || six.text != seventyTwo.text || six.frame == 0 || seventyTwo.frame == 0 ||
      six.frame != seventyTwo.frame || six.frame == blankFrame || seventyTwo.frame == blankFrame ||
      six.words != 4 || six.pages != 1 || six.words != seventyTwo.words || six.pages != seventyTwo.pages) {
    esp_rom_printf(
        "QEMU_PDF_TYPOGRAPHY_FAIL reason=source_font_leaked semantic_six=%08lX%08lX "
        "semantic_seventy_two=%08lX%08lX text_six=%08lX%08lX text_seventy_two=%08lX%08lX frame_six=%08lX "
        "frame_seventy_two=%08lX blank=%08lX words_six=%lu words_seventy_two=%lu pages_six=%u "
        "pages_seventy_two=%u\n",
        static_cast<unsigned long>(six.semantic >> 32U), static_cast<unsigned long>(six.semantic),
        static_cast<unsigned long>(seventyTwo.semantic >> 32U), static_cast<unsigned long>(seventyTwo.semantic),
        static_cast<unsigned long>(six.text >> 32U), static_cast<unsigned long>(six.text),
        static_cast<unsigned long>(seventyTwo.text >> 32U), static_cast<unsigned long>(seventyTwo.text),
        static_cast<unsigned long>(six.frame), static_cast<unsigned long>(seventyTwo.frame),
        static_cast<unsigned long>(blankFrame), static_cast<unsigned long>(six.words),
        static_cast<unsigned long>(seventyTwo.words), static_cast<unsigned>(six.pages),
        static_cast<unsigned>(seventyTwo.pages));
    state.phase = AcceptancePhase::Failed;
    return false;
  }
  esp_rom_printf(
      "QEMU_PDF_TYPOGRAPHY_PASS semantic_six=%08lX%08lX semantic_seventy_two=%08lX%08lX "
      "text_six=%08lX%08lX text_seventy_two=%08lX%08lX frame_six=%08lX frame_seventy_two=%08lX "
      "blank=%08lX words_six=%lu "
      "words_seventy_two=%lu pages_six=%u pages_seventy_two=%u font_id=%d font_size=2 line_height=100\n",
      static_cast<unsigned long>(six.semantic >> 32U), static_cast<unsigned long>(six.semantic),
      static_cast<unsigned long>(seventyTwo.semantic >> 32U), static_cast<unsigned long>(seventyTwo.semantic),
      static_cast<unsigned long>(six.text >> 32U), static_cast<unsigned long>(six.text),
      static_cast<unsigned long>(seventyTwo.text >> 32U), static_cast<unsigned long>(seventyTwo.text),
      static_cast<unsigned long>(six.frame), static_cast<unsigned long>(seventyTwo.frame),
      static_cast<unsigned long>(blankFrame), static_cast<unsigned long>(six.words),
      static_cast<unsigned long>(seventyTwo.words), static_cast<unsigned>(six.pages),
      static_cast<unsigned>(seventyTwo.pages), SETTINGS.getReaderFontId());
  return true;
}

bool checkPdfFullNavigation(GfxRenderer& renderer) {
  if (!preparePdf(PDF_NAVIGATION_FIXTURE_PATH, renderer)) {
    fail("PDF_NAV", "prepare");
    return false;
  }
  const auto document = loadPdfDocument(PDF_NAVIGATION_FIXTURE_PATH);
  ReaderLayout layout;
  if (!document || !computeReaderLayout(renderer, &layout) || document->getSectionCount() != 2 ||
      document->getTocEntryCount() != 3 || document->getTotalWordCount() != 10 ||
      document->getTitle() != "XMP Navigation" || document->getAuthor() != "XMP Author" ||
      document->getLanguage() != "de-CH") {
    fail("PDF_NAV", "metadata");
    return false;
  }

  const ReflowTocEntry part = document->getTocEntry(0);
  const ReflowTocEntry chapterOne = document->getTocEntry(1);
  const ReflowTocEntry chapterTwo = document->getTocEntry(2);
  CountingPrint content("href=\"sections/", "aria-label=", ">Index</p>");
  for (int sectionIndex = 0; sectionIndex < document->getSectionCount(); ++sectionIndex) {
    std::unique_ptr<Section> section;
    if (!loadOrCreateSection(document, renderer, layout, sectionIndex, "_qemu_pdf_navigation", false, &section) ||
        !document->streamSection(sectionIndex, content, 256)) {
      fail("PDF_NAV", "section");
      return false;
    }
  }
  const bool navigationMatches =
      part.title == "Part One" && part.level == 1 && part.sectionIndex == 0 && chapterOne.title == "Chapter One" &&
      chapterOne.level == 2 && chapterOne.parentIndex == 0 && chapterTwo.title == "Chapter Two" &&
      chapterTwo.level == 2 && chapterTwo.sectionIndex == 1 && chapterTwo.anchor == "b00000003" &&
      document->resolveHrefToSectionIndex(chapterTwo.href) == 1 && document->getTocIndexForSectionIndex(0) == 0 &&
      document->getTocIndexForSectionIndex(1) == 2 && content.firstCount() == 2 && content.secondCount() == 2 &&
      content.thirdCount() == 1;
  if (!navigationMatches) {
    fail("PDF_NAV", "content");
    return false;
  }
  esp_rom_printf("QEMU_PDF_NAV_PASS chapters=3 sections=2 words=10 links=2 labels=2 index=1\n");
  return true;
}

bool checkPdfImage(GfxRenderer& renderer) {
  if (!preparePdf(PDF_IMAGE_FIXTURE_PATH, renderer)) {
    fail("PDF_IMAGE", "prepare");
    return false;
  }
  const auto document = loadPdfDocument(PDF_IMAGE_FIXTURE_PATH);
  ReaderLayout layout;
  CountingPrint content("<img ", "Image cover caption.", "");
  if (!document || !computeReaderLayout(renderer, &layout) || !document->streamSection(0, content, 256) ||
      content.firstCount() != 1 || content.secondCount() != 1) {
    fail("PDF_IMAGE", "semantic");
    return false;
  }
  SETTINGS.imageRendering = CrossPointSettings::IMAGE_RENDERING::IMAGES_DISPLAY;
  std::unique_ptr<Section> withImage;
  uint32_t imageFrame = 0;
  if (!loadOrCreateSection(document, renderer, layout, 0, "_qemu_pdf_image", false, &withImage) ||
      !renderSectionPage(*withImage, 0, renderer, layout, &imageFrame)) {
    fail("PDF_IMAGE", "display");
    return false;
  }
  withImage.reset();
  SETTINGS.imageRendering = CrossPointSettings::IMAGE_RENDERING::IMAGES_SUPPRESS;
  std::unique_ptr<Section> withoutImage;
  uint32_t blankFrame = 0;
  const bool suppressed =
      loadOrCreateSection(document, renderer, layout, 0, "_qemu_pdf_image_suppressed", false, &withoutImage) &&
      renderSectionPage(*withoutImage, 0, renderer, layout, &blankFrame);
  SETTINGS.imageRendering = CrossPointSettings::IMAGE_RENDERING::IMAGES_DISPLAY;
  if (!suppressed || imageFrame == blankFrame) {
    fail("PDF_IMAGE", "frame");
    return false;
  }
  esp_rom_printf("QEMU_PDF_IMAGE_PASS retained=%u frame=%08lX blank=%08lX\n",
                 static_cast<unsigned>(content.firstCount()), static_cast<unsigned long>(imageFrame),
                 static_cast<unsigned long>(blankFrame));
  return true;
}

bool checkPdfPositiveCorpus(GfxRenderer& renderer) {
  uint64_t ocrHash = kFnvOffset;
  uint32_t ocrWords = 0;
  if (!preparePdf(PDF_OCR_FIXTURE_PATH, renderer)) {
    fail("PDF_CORPUS_OCR", "prepare");
    return false;
  }
  {
    const auto ocr = loadPdfDocument(PDF_OCR_FIXTURE_PATH);
    SemanticTextFnvPrint ocrSemantic;
    if (!ocr || ocr->getSectionCount() != 1 || !ocr->streamSection(0, ocrSemantic, 256) ||
        ocr->getTotalWordCount() != 4 || ocrSemantic.hash() != kExpectedPdfOcrSemanticHash) {
      fail("PDF_CORPUS_OCR", "text_layer");
      return false;
    }
    ocrHash = ocrSemantic.hash();
    ocrWords = ocr->getTotalWordCount();
  }

  uint64_t columnsHash = kFnvOffset;
  uint64_t tableHash = kFnvOffset;
  uint16_t columnsWords = 0;
  uint16_t tableWords = 0;
  if (!preparePdf(PDF_COLUMNS_TABLE_FIXTURE_PATH, renderer)) {
    fail("PDF_CORPUS_COLUMNS", "prepare");
    return false;
  }
  {
    const auto columns = loadPdfDocument(PDF_COLUMNS_TABLE_FIXTURE_PATH);
    std::array<char, 64> semanticText{};
    SemanticTextFnvPrint semantic(semanticText.data(), semanticText.size());
    constexpr size_t columnsBytes = sizeof(kExpectedPdfColumnsText) - 1U;
    constexpr size_t tableBytes = sizeof(kExpectedPdfTableText) - 1U;
    constexpr size_t tableOffset = columnsBytes + 1U;
    if (!columns || columns->getSectionCount() != 1 || !columns->streamSection(0, semantic, 256) ||
        columns->getTotalWordCount() != 12 || !semantic.captureComplete() || semantic.bytes() < columnsBytes ||
        std::memcmp(semanticText.data(), kExpectedPdfColumnsText, columnsBytes) != 0) {
      fail("PDF_CORPUS_COLUMNS", "reading_order");
      return false;
    }
    columnsHash = fnvBytes(reinterpret_cast<const uint8_t*>(semanticText.data()), columnsBytes);
    columnsWords = countAsciiWords(semanticText.data(), columnsBytes);
    if (columnsHash != kExpectedPdfColumnsSemanticHash || columnsWords != 8) {
      fail("PDF_CORPUS_COLUMNS", "reading_order");
      return false;
    }
    if (semantic.bytes() != tableOffset + tableBytes || semanticText[columnsBytes] != ' ' ||
        std::memcmp(semanticText.data() + tableOffset, kExpectedPdfTableText, tableBytes) != 0) {
      fail("PDF_CORPUS_TABLE", "row_major_order");
      return false;
    }
    tableHash = fnvBytes(reinterpret_cast<const uint8_t*>(semanticText.data() + tableOffset), tableBytes);
    tableWords = countAsciiWords(semanticText.data() + tableOffset, tableBytes);
    if (tableHash != kExpectedPdfTableSemanticHash || tableWords != 4) {
      fail("PDF_CORPUS_TABLE", "row_major_order");
      return false;
    }
  }

  uint64_t jpegHash = kFnvOffset;
  uint32_t jpegWords = 0;
  uint16_t jpegRetained = 0;
  uint32_t jpegFrame = 0;
  uint32_t blankFrame = 0;
  if (!preparePdf(PDF_JPEG_FIXTURE_PATH, renderer)) {
    fail("PDF_CORPUS_JPEG", "prepare");
    return false;
  }
  {
    const auto jpeg = loadPdfDocument(PDF_JPEG_FIXTURE_PATH);
    ReaderLayout layout;
    SemanticTextFnvPrint jpegSemantic;
    CountingPrint jpegContent("<img ", "Figure caption.", "");
    const bool jpegDocumentLoaded = jpeg != nullptr;
    const bool jpegLayoutReady = jpegDocumentLoaded && computeReaderLayout(renderer, &layout);
    const int32_t jpegSections = jpegDocumentLoaded ? jpeg->getSectionCount() : -1;
    bool jpegSemanticStreamed = false;
    bool jpegContentStreamed = false;
    if (jpegLayoutReady && jpegSections == 1) {
      jpegSemanticStreamed = jpeg->streamSection(0, jpegSemantic, 256);
      if (jpegSemanticStreamed) {
        jpegContentStreamed = jpeg->streamSection(0, jpegContent, 256);
      }
    }
    jpegHash = jpegSemantic.hash();
    jpegWords = jpegDocumentLoaded ? jpeg->getTotalWordCount() : 0;
    jpegRetained = jpegContent.firstCount();
    const uint16_t jpegCaptions = jpegContent.secondCount();
    if (!jpegDocumentLoaded || !jpegLayoutReady || jpegSections != 1 || !jpegSemanticStreamed ||
        !jpegContentStreamed || jpeg->getTotalWordCount() != 2 ||
        jpegSemantic.hash() != kExpectedPdfJpegSemanticHash || jpegContent.firstCount() != 1 || jpegCaptions != 1) {
      esp_rom_printf(
          "QEMU_PDF_CORPUS_JPEG_DIAGNOSTIC document=%u layout=%u sections=%ld "
          "semantic_stream=%u content_stream=%u words=%lu "
          "semantic=%08lX%08lX expected=%08lX%08lX semantic_bytes=%lu "
          "image_tags=%u captions=%u\n",
          static_cast<unsigned>(jpegDocumentLoaded), static_cast<unsigned>(jpegLayoutReady),
          static_cast<long>(jpegSections), static_cast<unsigned>(jpegSemanticStreamed),
          static_cast<unsigned>(jpegContentStreamed), static_cast<unsigned long>(jpegWords),
          static_cast<unsigned long>(jpegHash >> 32U), static_cast<unsigned long>(jpegHash),
          static_cast<unsigned long>(kExpectedPdfJpegSemanticHash >> 32U),
          static_cast<unsigned long>(kExpectedPdfJpegSemanticHash), static_cast<unsigned long>(jpegSemantic.bytes()),
          static_cast<unsigned>(jpegRetained), static_cast<unsigned>(jpegCaptions));
      fail("PDF_CORPUS_JPEG", "retained");
      return false;
    }

    SETTINGS.imageRendering = CrossPointSettings::IMAGE_RENDERING::IMAGES_DISPLAY;
    std::unique_ptr<Section> withImage;
    if (!loadOrCreateSection(jpeg, renderer, layout, 0, "_qemu_pdf_positive_jpeg", false, &withImage) ||
        !renderSectionPage(*withImage, 0, renderer, layout, &jpegFrame)) {
      SETTINGS.imageRendering = CrossPointSettings::IMAGE_RENDERING::IMAGES_DISPLAY;
      fail("PDF_CORPUS_JPEG", "decode");
      return false;
    }
    withImage.reset();
    SETTINGS.imageRendering = CrossPointSettings::IMAGE_RENDERING::IMAGES_SUPPRESS;
    std::unique_ptr<Section> withoutImage;
    const bool suppressed =
        loadOrCreateSection(jpeg, renderer, layout, 0, "_qemu_pdf_positive_jpeg_suppressed", false, &withoutImage) &&
        renderSectionPage(*withoutImage, 0, renderer, layout, &blankFrame);
    SETTINGS.imageRendering = CrossPointSettings::IMAGE_RENDERING::IMAGES_DISPLAY;
    if (!suppressed || jpegFrame == 0 || jpegFrame == blankFrame) {
      fail("PDF_CORPUS_JPEG", "frame");
      return false;
    }
  }

  esp_rom_printf(
      "QEMU_PDF_POSITIVE_PASS ocr=%08lX%08lX ocr_words=%lu "
      "columns=%08lX%08lX columns_words=%u table=%08lX%08lX table_words=%u "
      "jpeg=%08lX%08lX jpeg_words=%lu retained=%u decoded=1 frame=%08lX blank=%08lX\n",
      static_cast<unsigned long>(ocrHash >> 32U), static_cast<unsigned long>(ocrHash),
      static_cast<unsigned long>(ocrWords), static_cast<unsigned long>(columnsHash >> 32U),
      static_cast<unsigned long>(columnsHash), static_cast<unsigned>(columnsWords),
      static_cast<unsigned long>(tableHash >> 32U), static_cast<unsigned long>(tableHash),
      static_cast<unsigned>(tableWords), static_cast<unsigned long>(jpegHash >> 32U),
      static_cast<unsigned long>(jpegHash), static_cast<unsigned long>(jpegWords), static_cast<unsigned>(jpegRetained),
      static_cast<unsigned long>(jpegFrame), static_cast<unsigned long>(blankFrame));
  return true;
}

bool copyAnchor(char destination[PDF_SAVED_ITEM_ANCHOR_BYTES], const char source[REFLOW_SEMANTIC_ANCHOR_BYTES]) {
  size_t length = 0;
  while (length < REFLOW_SEMANTIC_ANCHOR_BYTES && source[length] != '\0') {
    ++length;
  }
  if (length >= PDF_SAVED_ITEM_ANCHOR_BYTES) {
    return false;
  }
  std::memset(destination, 0, PDF_SAVED_ITEM_ANCHOR_BYTES);
  std::memcpy(destination, source, length);
  return true;
}

bool checkPdfProgressAndSavedItems(GfxRenderer& renderer) {
  auto document = loadPdfDocument(PDF_NAVIGATION_FIXTURE_PATH);
  ReaderLayout layout;
  if (!document || !computeReaderLayout(renderer, &layout) || document->getTotalWordCount() != 10) {
    fail("PDF_PROGRESS", "document");
    return false;
  }
  const uint32_t totalWords = document->getTotalWordCount();
  const uint32_t targetCursor = totalWords * 3U / 5U;
  std::array<std::unique_ptr<Section>, 2> sections;
  for (int sectionIndex = 0; sectionIndex < 2; ++sectionIndex) {
    if (!loadOrCreateSection(document, renderer, layout, sectionIndex, "_qemu_pdf_navigation", true,
                             &sections[static_cast<size_t>(sectionIndex)])) {
      fail("PDF_PROGRESS", "section");
      return false;
    }
  }

  ReflowReadingPosition selected;
  ReflowPageSemanticRange selectedRange{};
  bool selectedRangeFound = false;
  for (size_t sectionIndex = 0; sectionIndex < sections.size() && !selectedRangeFound; ++sectionIndex) {
    Section& section = *sections[sectionIndex];
    for (uint16_t pageIndex = 0; pageIndex < section.pageCount; ++pageIndex) {
      const auto range = section.getSemanticRangeForPage(pageIndex);
      if (!range || !range->valid || range->wordCursor < targetCursor) {
        continue;
      }
      selected.sectionIndex = static_cast<int>(sectionIndex);
      selected.pageNumber = pageIndex;
      selected.pageCount = section.pageCount;
      selected.hasPageCount = true;
      if (pdfPopulateReadingPositionFromRange(*range, &selected)) {
        selectedRange = *range;
        selectedRangeFound = true;
        break;
      }
    }
  }
  selected.pageNumber = 0;
  selected.pageCount = 0;
  selected.hasPageCount = false;
  ReflowReadingPosition nonTerminal = selected;
  nonTerminal.wordCursor = targetCursor;
  bool positionSaved = false;
  if (selectedRangeFound && targetCursor == 6 && nonTerminal.wordCursor != 0 &&
      nonTerminal.wordCursor < selected.wordCursor &&
      selected.wordCursor == totalWords) {
    positionSaved = document->saveReadingPosition(nonTerminal);
  }
  if (!selectedRangeFound || selected.wordCursor == 0 || selected.wordCursor > totalWords ||
      !positionSaved) {
#ifdef CROSSINK_QEMU_TIMING_DIAGNOSTIC
    float computedProgress = 0.0F;
    const bool progressOk =
        selectedRangeFound && pdfCalculateWordCursorProgress(selected.wordCursor, totalWords, &computedProgress);
    const uint32_t progressMillionths =
        progressOk ? static_cast<uint32_t>(computedProgress * 1000000.0F + 0.5F) : 0;
    for (size_t sectionIndex = 0; sectionIndex < sections.size(); ++sectionIndex) {
      Section& section = *sections[sectionIndex];
      for (uint16_t pageIndex = 0; pageIndex < section.pageCount; ++pageIndex) {
        const auto range = section.getSemanticRangeForPage(pageIndex);
        esp_rom_printf(
            "QEMU_PDF_PROGRESS_PAGE section=%u page=%u page_count=%u found=%u valid=%u first=%lu last=%lu "
            "cursor=%lu\n",
            static_cast<unsigned>(sectionIndex), static_cast<unsigned>(pageIndex),
            static_cast<unsigned>(section.pageCount), static_cast<unsigned>(range.has_value()),
            static_cast<unsigned>(range && range->valid),
            static_cast<unsigned long>(range ? range->firstGlobalWordOrdinal : 0),
            static_cast<unsigned long>(range ? range->lastGlobalWordOrdinal : 0),
            static_cast<unsigned long>(range ? range->wordCursor : 0));
      }
    }
    esp_rom_printf(
        "QEMU_PDF_PROGRESS_DIAGNOSTIC selected_range=%u section=%d page=%d word_start=%lu word_cursor=%lu "
        "total_words=%lu saved=%u progress_ok=%u millionths=%lu page_count=%d\n",
        static_cast<unsigned>(selectedRangeFound), selected.sectionIndex, selected.pageNumber,
        static_cast<unsigned long>(selected.globalWordOrdinal), static_cast<unsigned long>(selected.wordCursor),
        static_cast<unsigned long>(totalWords), static_cast<unsigned>(positionSaved), static_cast<unsigned>(progressOk),
        static_cast<unsigned long>(progressMillionths), selected.pageCount);
#endif
    fail("PDF_PROGRESS", "position");
    return false;
  }

  auto progressDocument = loadPdfDocument(PDF_NAVIGATION_FIXTURE_PATH);
  ReflowReadingPosition nonTerminalResumed;
  if (!progressDocument || !progressDocument->loadReadingPosition(nonTerminalResumed) ||
      !pdfReadingPositionsEqualExact(nonTerminal, nonTerminalResumed) ||
      nonTerminalResumed.wordCursor != targetCursor ||
      !progressDocument->saveReadingPosition(selected)) {
    fail("PDF_PROGRESS", "mid_reopen");
    return false;
  }

  std::array<PdfSavedItem, 2> items{};
  PdfSavedItem& bookmark = items[0];
  bookmark.itemId = 1;
  bookmark.kind = PdfSavedItemKind::Bookmark;
  bookmark.flags = PDF_SAVED_ITEM_HAS_START_SEMANTIC;
  bookmark.sectionIndex = static_cast<uint16_t>(selected.sectionIndex);
  bookmark.startGlobalWordOrdinal = selectedRange.firstGlobalWordOrdinal;
  bookmark.startBlockWordOffset = selectedRange.firstBlockWordOffset;
  PdfSavedItem& clipping = items[1];
  clipping.itemId = 2;
  clipping.kind = PdfSavedItemKind::Clipping;
  clipping.flags = PDF_SAVED_ITEM_HAS_START_SEMANTIC | PDF_SAVED_ITEM_HAS_END_SEMANTIC;
  clipping.sectionIndex = bookmark.sectionIndex;
  clipping.startGlobalWordOrdinal = bookmark.startGlobalWordOrdinal;
  clipping.endGlobalWordOrdinal = bookmark.startGlobalWordOrdinal;
  clipping.startBlockWordOffset = bookmark.startBlockWordOffset;
  clipping.endBlockWordOffset = bookmark.startBlockWordOffset;
  if (!copyAnchor(bookmark.startBlockAnchor, selectedRange.blockAnchor) ||
      !copyAnchor(clipping.startBlockAnchor, selectedRange.blockAnchor) ||
      !copyAnchor(clipping.endBlockAnchor, selectedRange.blockAnchor) ||
      !progressDocument->validatePdfSavedItem(bookmark) ||
      !progressDocument->validatePdfSavedItem(clipping) ||
      !progressDocument->savePdfSavedItems(items.data(), items.size())) {
    fail("PDF_PROGRESS", "saved_items");
    return false;
  }

  document.reset();
  progressDocument.reset();
  auto reopened = loadPdfDocument(PDF_NAVIGATION_FIXTURE_PATH);
  ReflowReadingPosition resumed;
  std::array<PdfSavedItem, 2> loadedItems{};
  PdfSavedItemsBuffer loaded{loadedItems.data(), static_cast<uint16_t>(loadedItems.size()), 0};
  if (!reopened || !reopened->loadReadingPosition(resumed) || !pdfReadingPositionsEqualExact(selected, resumed) ||
      resumed.wordCursor != selected.wordCursor ||
      !reopened->loadPdfSavedItems(&loaded) || loaded.count != 2 ||
      loaded.items[0].kind != PdfSavedItemKind::Bookmark || loaded.items[1].kind != PdfSavedItemKind::Clipping) {
    fail("PDF_PROGRESS", "reopen");
    return false;
  }
  float wordProgress = 0.0F;
  if (!pdfCalculateWordCursorProgress(resumed.wordCursor, totalWords, &wordProgress)) {
    fail("PDF_PROGRESS", "percent");
    return false;
  }
  float nonTerminalProgress = 0.0F;
  if (!pdfCalculateWordCursorProgress(nonTerminalResumed.wordCursor, totalWords,
                                      &nonTerminalProgress)) {
    fail("PDF_PROGRESS", "mid_percent");
    return false;
  }
  const uint32_t nonTerminalPercent =
      static_cast<uint32_t>(nonTerminalProgress * 100.0F + 0.5F);
  const uint32_t percent = static_cast<uint32_t>(wordProgress * 100.0F + 0.5F);
  if (state.pdfFramebufferGuardChecks == 0 ||
      state.pdfFramebufferGuardFailures != 0 ||
      state.pdfFramebufferGuardControls != state.pdfFramebufferGuardChecks * 2U ||
      state.pdfFramebufferGuardRejections != state.pdfFramebufferGuardControls) {
    fail("PDF_FRAMEBUFFER_GUARD", "changed");
    return false;
  }
  esp_rom_printf(
      "QEMU_PDF_FRAMEBUFFER_GUARD_PASS bytes=%lu checks=%lu violations=0 controls=%lu rejected=%lu\n",
      static_cast<unsigned long>(EXPECTED_FRAME_BYTES),
      static_cast<unsigned long>(state.pdfFramebufferGuardChecks),
      static_cast<unsigned long>(state.pdfFramebufferGuardControls),
      static_cast<unsigned long>(state.pdfFramebufferGuardRejections));
  esp_rom_printf(
      "QEMU_PDF_PROGRESS_MID_PASS words=10 cursor=%lu percent=%lu resumed=1\n",
      static_cast<unsigned long>(nonTerminalResumed.wordCursor),
      static_cast<unsigned long>(nonTerminalPercent));
  esp_rom_printf("QEMU_PDF_PROGRESS_PASS words=10 cursor=%lu percent=%lu bookmark=1 clipping=1 resumed=1\n",
                 static_cast<unsigned long>(resumed.wordCursor), static_cast<unsigned long>(percent));
  return true;
}

bool checkPdfCompletedCacheReopen(GfxRenderer& renderer) {
  TracedPdfCacheIo traced;
  traced.trackSourcePath(PDF_NAVIGATION_FIXTURE_PATH);
  auto loaded = makeUniqueNoThrow<PdfReflowDocument>();
  if (!loaded || !loaded->initialize(traced.io(), PDF_NAVIGATION_FIXTURE_PATH, REFLOW_CACHE_DIRECTORY) ||
      !loaded->loadCompletedCache()) {
    fail("PDF_CACHE_REOPEN", "document");
    return false;
  }
  const std::shared_ptr<PdfReflowDocument> document(std::move(loaded));
  ReaderLayout layout;
  std::unique_ptr<Section> section;
  if (!computeReaderLayout(renderer, &layout) ||
      !loadOrCreateSection(document, renderer, layout, 0, "_qemu_pdf_navigation", true, &section)) {
    fail("PDF_CACHE_REOPEN", "load");
    return false;
  }
  uint32_t stableFrame = 0;
  if (!renderSectionPage(*section, 0, renderer, layout, &stableFrame)) {
    fail("PDF_CACHE_REOPEN", "warmup");
    return false;
  }
  const uint32_t opensBeforeTurns = traced.sourceReadOpenCount;
  const uint32_t readsBeforeTurns = traced.sourceReadCount;
  const uint32_t parserEntriesBeforeTurns = state.pdfParserEntries;
  const uint32_t extractionEntriesBeforeTurns = state.pdfExtractionEntries;
  sampleRuntime();
  const PdfResourceSnapshot resourcesBeforeTurns = qemuPdfResources(&pdfResourceContext);
  for (uint16_t turn = 0; turn < kCachedPageTurns; ++turn) {
    uint32_t frame = 0;
    if (!renderSectionPage(*section, 0, renderer, layout, &frame) || frame != stableFrame) {
      fail("PDF_CACHE_REOPEN", "turn");
      return false;
    }
    sampleRuntime();
  }
  const uint32_t sourceOpens = traced.sourceReadOpenCount;
  const uint32_t sourceReads = traced.sourceReadCount;
  const size_t sourceMaximumReadRequest = traced.sourceMaximumReadRequest;
  const uint32_t parserEntriesAfterTurns = state.pdfParserEntries;
  const uint32_t extractionEntriesAfterTurns = state.pdfExtractionEntries;
  const PdfResourceSnapshot resourcesAfterTurns = qemuPdfResources(&pdfResourceContext);
  if (sourceOpens != 1 || sourceReads == 0 || sourceReads > 2 || sourceMaximumReadRequest == 0 ||
      sourceMaximumReadRequest > kMaximumIoRequestBytes || sourceOpens != opensBeforeTurns ||
      sourceReads != readsBeforeTurns) {
    fail("PDF_CACHE_REOPEN", "source_io");
    return false;
  }
  if (parserEntriesAfterTurns != parserEntriesBeforeTurns ||
      extractionEntriesAfterTurns != extractionEntriesBeforeTurns) {
    fail("PDF_CACHE_REOPEN", "parser_or_extraction");
    return false;
  }
  if (resourcesBeforeTurns.freeHeap == 0 || resourcesBeforeTurns.largestBlock == 0 ||
      resourcesBeforeTurns.stackMargin == 0 || resourcesAfterTurns.freeHeap == 0 ||
      resourcesAfterTurns.largestBlock == 0 || resourcesAfterTurns.stackMargin == 0 ||
      resourcesAfterTurns.freeHeap < resourcesBeforeTurns.freeHeap ||
      resourcesAfterTurns.largestBlock < resourcesBeforeTurns.largestBlock ||
      resourcesAfterTurns.stackMargin < resourcesBeforeTurns.stackMargin) {
    fail("PDF_CACHE_REOPEN", "resource_erosion");
    return false;
  }
  const uint32_t parserEntries = parserEntriesAfterTurns - parserEntriesBeforeTurns;
  const uint32_t extractionEntries = extractionEntriesAfterTurns - extractionEntriesBeforeTurns;
  esp_rom_printf(
      "QEMU_PDF_CACHE_REOPEN_PASS page_turns=100 extraction=%lu parser=%lu source_opens=%lu source_reads=%lu "
      "source_max_request=%lu "
      "heap_before=%lu heap_after=%lu largest_before=%lu largest_after=%lu stack_before=%lu stack_after=%lu "
      "frame=%08lX\n",
      static_cast<unsigned long>(extractionEntries), static_cast<unsigned long>(parserEntries),
      static_cast<unsigned long>(sourceOpens), static_cast<unsigned long>(sourceReads),
      static_cast<unsigned long>(sourceMaximumReadRequest),
      static_cast<unsigned long>(resourcesBeforeTurns.freeHeap),
      static_cast<unsigned long>(resourcesAfterTurns.freeHeap),
      static_cast<unsigned long>(resourcesBeforeTurns.largestBlock),
      static_cast<unsigned long>(resourcesAfterTurns.largestBlock),
      static_cast<unsigned long>(resourcesBeforeTurns.stackMargin),
      static_cast<unsigned long>(resourcesAfterTurns.stackMargin), static_cast<unsigned long>(stableFrame));
  return true;
}

bool rejectPdfFixture(const char* path, GfxRenderer& renderer) {
  if (!clearPdfCache(path)) {
    return false;
  }
  TracedPdfCacheIo traced;
  auto preparation = makeUniqueNoThrow<PdfPreparation>();
  if (!preparation) {
    return false;
  }
  const PdfStatus beginStatus = beginTrackedPdfPreparation(*preparation, preparationConfig(traced, path, renderer));
  bool rejected = false;
  if (!beginStatus) {
    rejected = beginStatus.error != PdfError::IoFailure && beginStatus.error != PdfError::InsufficientMemory;
  } else {
    const PdfStepResult result = runPreparation(*preparation);
    rejected = result.failed() && result.status.error != PdfError::BudgetExhausted &&
               result.status.error != PdfError::IoFailure && result.status.error != PdfError::InsufficientMemory;
  }
  if (!rejected || traced.maximumRequest > kMaximumPreparationIoRequestBytes) {
    return false;
  }
  TracedPdfCacheIo verifyIo;
  auto document = makeUniqueNoThrow<PdfReflowDocument>();
  if (!document) {
    return false;
  }
  const PdfStatus initialized = document->initialize(verifyIo.io(), path, REFLOW_CACHE_DIRECTORY);
  return !initialized || !document->loadCompletedCache();
}

bool checkPdfForcedOom(GfxRenderer& renderer) {
  if (!clearPdfCache(PDF_FONT_SIX_PATH)) {
    return false;
  }

  TracedPdfCacheIo traced;
  auto preparation = makeUniqueNoThrow<PdfPreparation>();
  if (!preparation) {
    return false;
  }
  const PdfStatus beginStatus = beginTrackedPdfPreparation(
      *preparation, preparationConfig(traced, PDF_FONT_SIX_PATH, renderer, &forcedOomResourceContext));
  PdfStepResult result = PdfStepResult::paused();
  if (beginStatus) {
    result = stepTrackedPdfPreparation(*preparation);
  }
  const bool exactInsufficientMemory =
      (!beginStatus && beginStatus.error == PdfError::InsufficientMemory) ||
      (beginStatus && result.failed() && result.status.error == PdfError::InsufficientMemory);
  preparation.reset();

  TracedPdfCacheIo verifyIo;
  auto document = makeUniqueNoThrow<PdfReflowDocument>();
  if (!document) {
    return false;
  }
  const PdfStatus initialized = document->initialize(verifyIo.io(), PDF_FONT_SIX_PATH, REFLOW_CACHE_DIRECTORY);
  const bool completedCache = initialized && document->loadCompletedCache();
  return exactInsufficientMemory && initialized && !completedCache;
}

bool checkPdfNegativeCorpus(GfxRenderer& renderer) {
  uint8_t rejected = 0;
  if (!checkPdfForcedOom(renderer)) {
    fail("PDF_NEGATIVE", "forced_oom");
    return false;
  }
  ++rejected;
  for (const char* path : kNegativeFixtures) {
    if (!rejectPdfFixture(path, renderer)) {
      fail("PDF_NEGATIVE", "unsafe_result");
      return false;
    }
    ++rejected;
  }
  esp_rom_printf("QEMU_PDF_NEGATIVE_PASS checked=7 rejected=%u forced_oom=InsufficientMemory completed_cache=0\n",
                 static_cast<unsigned>(rejected));
  return true;
}

bool captureEpubFrame(Section& section, GfxRenderer& renderer, const ReaderLayout& layout, const uint16_t pageIndex,
                      uint64_t* frame) {
  if (frame == nullptr || pageIndex >= section.pageCount) {
    return false;
  }
  section.currentPage = pageIndex;
  auto page = section.loadPageFromSectionFile();
  if (!page) {
    return false;
  }
  renderer.clearScreen(0xFF);
  page->render(renderer, SETTINGS.getReaderFontId(), layout.left, layout.top, true);
  *frame = fnvBytes(renderer.getFrameBuffer(), renderer.getBufferSize());
  return true;
}

void emitEpubStage(const bool cached, const char* const stage, const int index = -1, const int pages = -1) {
  const char* const pass = cached ? "cached" : "uncached";
  if (index >= 0 && pages >= 0) {
    esp_rom_printf("QEMU_EPUB_STAGE pass=%s stage=%s index=%d pages=%d\n", pass, stage, index, pages);
  } else if (index >= 0) {
    esp_rom_printf("QEMU_EPUB_STAGE pass=%s stage=%s index=%d\n", pass, stage, index);
  } else {
    esp_rom_printf("QEMU_EPUB_STAGE pass=%s stage=%s\n", pass, stage);
  }
}

bool runEpubOraclePass(GfxRenderer& renderer, const bool cached, EpubOracle* oracle) {
  if (oracle == nullptr) {
    return false;
  }
  EpubEmbeddedStyleScope embeddedStyleScope;
  emitEpubStage(cached, "begin");
  auto unique = makeUniqueNoThrow<Epub>(std::string(EPUB_ORACLE_PATH), std::string(REFLOW_CACHE_DIRECTORY));
  if (!unique) {
    return false;
  }
  std::shared_ptr<Epub> epub(std::move(unique));
  if (!epub->load(!cached, true)) {
    return false;
  }
  emitEpubStage(cached, "load");
  constexpr ReflowCapabilitySet expectedCapabilities =
      ReflowCapability::ExternalProgressSync | ReflowCapability::NearbyProgressSync |
      ReflowCapability::PublisherRenderModes | ReflowCapability::EmbeddedStyles | ReflowCapability::SavedItems;
  if (epub->getFormat() != ReflowDocumentFormat::Epub || std::strcmp(epub->getStoreFormatKey(), "epub") != 0 ||
      epub->getCapabilities() != expectedCapabilities || epub->getSectionCount() != 2 ||
      epub->getTitle() != "Test: Synthetic Unicode Glyphs" || epub->getAuthor() != "CrossPoint Test Fixture" ||
      epub->getLanguage() != "en" || epub->getTocEntryCount() != 2) {
    return false;
  }

  constexpr std::array<const char*, 2> kSectionHrefs = {"OEBPS/chapter1.xhtml", "OEBPS/chapter2.xhtml"};
  constexpr std::array<size_t, 2> kSectionBytes = {1670, 1920};
  constexpr std::array<size_t, 2> kCumulativeBytes = {1670, 3590};
  constexpr std::array<const char*, 2> kTocTitles = {"Glyph Reference", "Block Stress Cases"};
  for (int sectionIndex = 0; sectionIndex < 2; ++sectionIndex) {
    const ReflowSectionInfo info = epub->getSectionInfo(sectionIndex);
    size_t sectionBytes = 0;
    const ReflowTocEntry toc = epub->getTocEntry(sectionIndex);
    if (info.href != kSectionHrefs[static_cast<size_t>(sectionIndex)] ||
        epub->getSpineItem(sectionIndex).href != info.href ||
        !epub->getSectionSize(sectionIndex, &sectionBytes) ||
        sectionBytes != kSectionBytes[static_cast<size_t>(sectionIndex)] || info.byteSize != sectionBytes ||
        info.cumulativeSize != kCumulativeBytes[static_cast<size_t>(sectionIndex)] ||
        epub->getCumulativeSectionSize(sectionIndex) != info.cumulativeSize ||
        toc.title != kTocTitles[static_cast<size_t>(sectionIndex)] || toc.href != info.href || !toc.anchor.empty() ||
        toc.level != 1 || toc.sectionIndex != sectionIndex ||
        epub->resolveHrefToSectionIndex(toc.href) != sectionIndex) {
      return false;
    }
  }

  ReaderLayout layout;
  if (!computeReaderLayout(renderer, &layout)) {
    return false;
  }

  constexpr std::array<size_t, 2> kExpectedStreamBytes = {1670, 1920};
  constexpr std::array<uint64_t, 2> kExpectedStreamHashes = {kExpectedEpubSectionZeroHash,
                                                             kExpectedEpubSectionOneHash};
  for (int sectionIndex = 0; sectionIndex < 2; ++sectionIndex) {
    FnvPrint stream;
    ReflowResource localSection;
    if (epub->getLocalSectionPath(sectionIndex, localSection) || localSection.kind != ReflowResourceKind::Streamed ||
        localSection.paginatorMayDelete || !epub->streamSection(sectionIndex, stream, 1024) ||
        stream.bytes() != kExpectedStreamBytes[static_cast<size_t>(sectionIndex)] ||
        stream.hash() != kExpectedStreamHashes[static_cast<size_t>(sectionIndex)]) {
      return false;
    }
    if (sectionIndex == 0) {
      oracle->xhtml0 = stream.hash();
    } else {
      oracle->xhtml1 = stream.hash();
    }
    emitEpubStage(cached, "xhtml", sectionIndex);
  }

  FnvPrint css;
  ReflowResource localCss;
  size_t cssBytes = 0;
  if (epub->resolveResource(0, kEpubCssResource, localCss) || localCss.kind != ReflowResourceKind::Streamed ||
      localCss.paginatorMayDelete || !epub->streamResource(0, kEpubCssResource, css, 256) ||
      !epub->getResourceSize(0, kEpubCssResource, &cssBytes) || cssBytes != 162 || css.bytes() != cssBytes ||
      css.hash() != kExpectedEpubCssHash) {
    return false;
  }
  oracle->css = css.hash();
  emitEpubStage(cached, "css", 0);

  std::unique_ptr<Section> section;
  if (!loadOrCreateSection(epub, renderer, layout, 1, "", cached, &section) ||
      section->pageCount != kExpectedEpubRepresentativePages) {
    return false;
  }
  emitEpubStage(cached, "section", 1, section->pageCount);

  char cacheFile[PDF_CACHE_PATH_CAPACITY]{};
  const int cacheFileLength =
      std::snprintf(cacheFile, sizeof(cacheFile), "%s/sections/1.bin", epub->getCachePath().c_str());
  size_t cacheBytes = 0;
  if (cacheFileLength <= 0 || static_cast<size_t>(cacheFileLength) >= sizeof(cacheFile) ||
      !hashFileWithSeparator(cacheFile, &oracle->cache, &cacheBytes) ||
      cacheBytes != kExpectedEpubSectionCacheBytes || oracle->cache != kExpectedEpubSectionCacheHash) {
    return false;
  }
  emitEpubStage(cached, "cache", 1);

  if (!captureEpubFrame(*section, renderer, layout, 1, &oracle->frame) || oracle->frame != kExpectedEpubFrameHash) {
    return false;
  }
  emitEpubStage(cached, "frame", 1);
  if (oracle->xhtml0 != kExpectedEpubSectionZeroHash || oracle->xhtml1 != kExpectedEpubSectionOneHash ||
      oracle->css != kExpectedEpubCssHash) {
    return false;
  }
  emitEpubStage(cached, "end");
  return true;
}

bool verifyEpubProgressAndBookmark() {
  auto unique = makeUniqueNoThrow<Epub>(std::string(EPUB_ORACLE_PATH), std::string(REFLOW_CACHE_DIRECTORY));
  if (!unique || !unique->load(false, true)) {
    return false;
  }
  std::shared_ptr<Epub> epub(std::move(unique));
  constexpr int kSectionIndex = 1;
  constexpr int kPageIndex = 1;
  constexpr int kPageCount = kExpectedEpubRepresentativePages;
  if (!EpubReaderUtils::saveProgress(*epub, kSectionIndex, kPageIndex, kPageCount)) {
    return false;
  }

  BOOKMARKS.unload();
  BookmarkStore::deleteForFilePath(epub->getPath(), epub->getStoreFormatKey());
  if (!BOOKMARKS.loadForBook(epub->getPath(), epub->getTitle(), epub->getAuthor(), epub->getStoreFormatKey()) ||
      BOOKMARKS.addBookmark(kSectionIndex, 1.0F / static_cast<float>(kPageCount), kPageCount, "Block Stress Cases",
                            UINT16_MAX, kEpubBookmarkSnippet) != BookmarkStore::AddResult::Added) {
    BOOKMARKS.unload();
    return false;
  }
  BOOKMARKS.unload();
  epub.reset();

  auto reopenedUnique = makeUniqueNoThrow<Epub>(std::string(EPUB_ORACLE_PATH), std::string(REFLOW_CACHE_DIRECTORY));
  if (!reopenedUnique || !reopenedUnique->load(false, true)) {
    return false;
  }
  std::shared_ptr<Epub> reopened(std::move(reopenedUnique));
  ReflowReadingPosition documentProgress;
  EpubReaderUtils::Progress readerProgress;
  const uint32_t progressMillionths = static_cast<uint32_t>(std::lround(
      reopened->calculateProgress(kSectionIndex, 1.0F / static_cast<float>(kPageCount)) * 1000000.0F));
  if (!reopened->loadReadingPosition(documentProgress) ||
      !EpubReaderUtils::loadProgress(*reopened, readerProgress, "QEMU") ||
      documentProgress.sectionIndex != kSectionIndex || documentProgress.pageNumber != kPageIndex ||
      !documentProgress.hasPageCount || documentProgress.pageCount != kPageCount ||
      readerProgress.spineIndex != kSectionIndex || readerProgress.pageNumber != kPageIndex ||
      !readerProgress.hasPageCount || readerProgress.pageCount != kPageCount ||
      progressMillionths != kExpectedEpubProgressMillionths) {
    return false;
  }

  if (!BOOKMARKS.loadForBook(reopened->getPath(), reopened->getTitle(), reopened->getAuthor(),
                             reopened->getStoreFormatKey()) ||
      BOOKMARKS.getBookmarks().size() != 1) {
    BOOKMARKS.unload();
    return false;
  }
  const Bookmark& bookmark = BOOKMARKS.getBookmarks().front();
  const uint32_t bookmarkMillionths =
      static_cast<uint32_t>(std::lround(bookmark.progress * 1000000.0F));
  if (bookmark.spineIndex != kSectionIndex || bookmark.paragraphIndex != UINT16_MAX ||
      bookmarkMillionths != kExpectedEpubBookmarkMillionths ||
      std::strcmp(bookmark.chapterTitle, "Block Stress Cases") != 0 ||
      std::strcmp(bookmark.snippet, "qemu-epub-smoke") != 0) {
    BOOKMARKS.clearAll();
    BOOKMARKS.unload();
    return false;
  }
  BOOKMARKS.clearAll();
  BOOKMARKS.unload();
  esp_rom_printf(
      "QEMU_EPUB_PROGRESS_PASS section=1 page=1 pages=3 book_millionths=%lu bookmark_millionths=%lu "
      "snippet=qemu-epub-smoke\n",
      static_cast<unsigned long>(progressMillionths), static_cast<unsigned long>(bookmarkMillionths));
  return true;
}

bool checkEpubOracle(GfxRenderer& renderer) {
  const std::string cachePath = Epub::cachePathForFilePath(EPUB_ORACLE_PATH, REFLOW_CACHE_DIRECTORY);
  if (!epubSourceMatches() || (Storage.exists(cachePath.c_str()) && !Storage.removeDir(cachePath.c_str())) ||
      Epub::hasCache(EPUB_ORACLE_PATH, REFLOW_CACHE_DIRECTORY)) {
    fail("EPUB_ORACLE", "preclean");
    return false;
  }
  emitEpubStage(false, "preclean");
  EpubOracle uncached;
  if (!runEpubOraclePass(renderer, false, &uncached)) {
    fail("EPUB_ORACLE", "uncached");
    return false;
  }
  esp_rom_printf(
      "QEMU_EPUB_ORACLE_PASS pass=uncached xhtml0=%08lX%08lX xhtml1=%08lX%08lX css=%08lX%08lX "
      "cache=%08lX%08lX frame=%08lX%08lX\n",
      static_cast<unsigned long>(uncached.xhtml0 >> 32U), static_cast<unsigned long>(uncached.xhtml0),
      static_cast<unsigned long>(uncached.xhtml1 >> 32U), static_cast<unsigned long>(uncached.xhtml1),
      static_cast<unsigned long>(uncached.css >> 32U), static_cast<unsigned long>(uncached.css),
      static_cast<unsigned long>(uncached.cache >> 32U), static_cast<unsigned long>(uncached.cache),
      static_cast<unsigned long>(uncached.frame >> 32U), static_cast<unsigned long>(uncached.frame));

  EpubOracle cached;
  if (!runEpubOraclePass(renderer, true, &cached) || std::memcmp(&uncached, &cached, sizeof(uncached)) != 0) {
    fail("EPUB_ORACLE", "cached");
    return false;
  }
  esp_rom_printf(
      "QEMU_EPUB_ORACLE_PASS pass=cached xhtml0=%08lX%08lX xhtml1=%08lX%08lX css=%08lX%08lX "
      "cache=%08lX%08lX frame=%08lX%08lX\n",
      static_cast<unsigned long>(cached.xhtml0 >> 32U), static_cast<unsigned long>(cached.xhtml0),
      static_cast<unsigned long>(cached.xhtml1 >> 32U), static_cast<unsigned long>(cached.xhtml1),
      static_cast<unsigned long>(cached.css >> 32U), static_cast<unsigned long>(cached.css),
      static_cast<unsigned long>(cached.cache >> 32U), static_cast<unsigned long>(cached.cache),
      static_cast<unsigned long>(cached.frame >> 32U), static_cast<unsigned long>(cached.frame));
  if (!verifyEpubProgressAndBookmark() || !epubCacheResidueAbsent(cachePath) || !epubSourceMatches()) {
    fail("EPUB_ORACLE", "reopen_or_residue");
    return false;
  }
  return true;
}

bool checkPdfProductTracer(GfxRenderer& renderer) {
  char cacheRoot[PDF_CACHE_PATH_CAPACITY]{};
  PdfStatus status = pdfFormatCacheRoot("/.crosspoint", PDF_FIXTURE_PATH, cacheRoot, sizeof(cacheRoot));
  if (!status.ok()) {
    fail("PDF_TRACER", "cache_path");
    return false;
  }
  if (Storage.exists(cacheRoot) && !Storage.removeDir(cacheRoot)) {
    fail("PDF_TRACER", "preclean");
    return false;
  }

  PdfHalCacheIoContext preparationIoContext;
  auto preparation = makeUniqueNoThrow<PdfPreparation>();
  if (!preparation) {
    fail("PDF_TRACER", "preparation_oom");
    return false;
  }
  const PdfPreparationConfig config{
      pdfHalCacheIo(preparationIoContext),
      PDF_FIXTURE_PATH,
      "/.crosspoint",
      nullptr,
      qemuNowMs,
      {&pdfResourceContext, qemuPdfResources, recordPdfResourceEvent},
      pdfHalCacheRename,
      static_cast<uint16_t>(renderer.getScreenWidth()),
      static_cast<uint16_t>(renderer.getScreenHeight()),
  };
  status = beginTrackedPdfPreparation(*preparation, config);
  PdfStepResult result = PdfStepResult::paused();
  for (uint32_t slice = 0; status.ok() && result.yielded() && slice < 100000; ++slice) {
    result = stepTrackedPdfPreparation(*preparation);
    sampleRuntime();
    yieldAfterPdfPreparationStep(result);
  }
  if (!status.ok() || !result.complete() || preparation->totalWords() != 2 ||
      preparation->resourcePeakBytes() != PdfLimits::TotalWorkspaceBytes) {
    fail("PDF_TRACER", !status.ok() ? "begin" : "prepare");
    return false;
  }
  const size_t preparationPeak = preparation->resourcePeakBytes();
  preparation.reset();
  sampleRuntime();

  bool rendered = false;
  bool progressVerified = false;
  bool progressSaved = false;
  uint16_t pageCount = 0;
  uint32_t renderedFrame = 0;
  ReflowReadingPosition expectedPosition;
  {
    PdfStatus documentStatus{};
    auto loaded = loadPdfHalReflowDocumentNoThrow(PDF_FIXTURE_PATH, "/.crosspoint", &documentStatus);
    if (!loaded || !documentStatus.ok() || loaded->getTotalWordCount() != 2) {
      fail("PDF_TRACER", "document");
      return false;
    }
    std::shared_ptr<ReflowDocument> document(std::move(loaded));

    int marginTop = 0;
    int marginRight = 0;
    int marginBottom = 0;
    int marginLeft = 0;
    renderer.getOrientedViewableTRBL(&marginTop, &marginRight, &marginBottom, &marginLeft);
    marginTop += SETTINGS.screenMargin;
    marginRight += SETTINGS.screenMargin;
    marginBottom += SETTINGS.screenMargin;
    marginLeft += SETTINGS.screenMargin;
    const int availableWidth = renderer.getScreenWidth() - marginLeft - marginRight;
    const int availableHeight = renderer.getScreenHeight() - marginTop - marginBottom;
    if (availableWidth <= 0 || availableHeight <= 0 || availableWidth > UINT16_MAX || availableHeight > UINT16_MAX) {
      fail("PDF_TRACER", "viewport");
      return false;
    }

    Section section(document, 0, renderer, "_qemu_pdf");
    bool imagesSuppressed = false;
    bool lowMemory = false;
    const int fontId = SETTINGS.getReaderFontId();
    ReaderRenderSpec spec = SETTINGS.readerRenderSpec(static_cast<uint16_t>(availableWidth),
                                                      static_cast<uint16_t>(availableHeight), EpubRenderMode::Light);
    spec.fontId = fontId;
    spec.embeddedStyle = false;
    const bool built = section.createSectionFile(spec, nullptr, &imagesSuppressed, &lowMemory);
    const std::string text = built ? section.getTextFromSectionFile() : std::string{};
    section.currentPage = 0;
    auto page = built ? section.loadPageFromSectionFile() : nullptr;
    pageCount = section.pageCount;
    display.clearScreen(0xFF);
    const uint32_t blankFrame = QemuHalControl::frameCrc32();
    if (page) {
      page->renderText(renderer, fontId, marginLeft, marginTop);
      renderedFrame = QemuHalControl::frameCrc32();
    }
    const auto semantic = built ? section.getSemanticRangeForPage(0) : std::optional<ReflowPageSemanticRange>{};
    float wordProgress = 0.0F;
    expectedPosition.sectionIndex = 0;
    expectedPosition.pageNumber = 0;
    expectedPosition.pageCount = pageCount;
    expectedPosition.hasPageCount = pageCount > 0;
    const bool populatedPosition = semantic && pdfPopulateReadingPositionFromRange(*semantic, &expectedPosition);
    expectedPosition.pageNumber = 0;
    expectedPosition.pageCount = 0;
    expectedPosition.hasPageCount = false;
    progressSaved =
        populatedPosition && semantic->valid && semantic->firstGlobalWordOrdinal == 0 &&
        semantic->lastGlobalWordOrdinal == 1 && semantic->wordCursor == 2 &&
        pdfCalculateWordCursorProgress(semantic->wordCursor, document->getTotalWordCount(), &wordProgress) &&
        wordProgress == 1.0F && document->saveReadingPosition(expectedPosition);
    rendered = built && !lowMemory && text == PDF_EXPECTED_TEXT && page != nullptr && pageCount > 0 &&
               renderedFrame != 0 && renderedFrame != blankFrame;
  }

  // Recreate the complete document and progress store before claiming
  // persistence. A same-object read could accidentally pass from cached state.
  if (progressSaved) {
    PdfStatus reopenStatus{};
    auto reopened = loadPdfHalReflowDocumentNoThrow(PDF_FIXTURE_PATH, "/.crosspoint", &reopenStatus);
    ReflowReadingPosition persistedPosition;
    progressVerified = reopened && reopenStatus.ok() && reopened->getTotalWordCount() == 2 &&
                       reopened->loadReadingPosition(persistedPosition) &&
                       pdfReadingPositionsEqualExact(expectedPosition, persistedPosition);
  }

  const bool cleaned = Storage.exists(cacheRoot) && Storage.removeDir(cacheRoot);
  if (!rendered || !progressVerified || !cleaned) {
    fail("PDF_TRACER", !rendered ? "shared_reader" : (!progressVerified ? "progress" : "cleanup"));
    return false;
  }
  esp_rom_printf("QEMU_PDF_LEGACY_PROGRESS words=2 reached=2 percent=100 persisted=1 cursor=2\n");
  esp_rom_printf("QEMU_PDF_TRACER text=Hello_PDF words=2 pages=%u frame=%08lX heap=%lu\n",
                 static_cast<unsigned>(pageCount), static_cast<unsigned long>(renderedFrame),
                 static_cast<unsigned long>(preparationPeak));
  state.pdfTracerReady = true;
  return true;
}

bool checkPdfNavigation(GfxRenderer& renderer) {
  char cacheRoot[PDF_CACHE_PATH_CAPACITY]{};
  PdfStatus status = pdfFormatCacheRoot("/.crosspoint", PDF_NAVIGATION_FIXTURE_PATH, cacheRoot, sizeof(cacheRoot));
  if (!status.ok()) {
    fail("PDF_NAV", "cache_path");
    return false;
  }
  if (Storage.exists(cacheRoot) && !Storage.removeDir(cacheRoot)) {
    fail("PDF_NAV", "preclean");
    return false;
  }

  PdfHalCacheIoContext preparationIoContext;
  auto preparation = makeUniqueNoThrow<PdfPreparation>();
  if (!preparation) {
    fail("PDF_NAV", "preparation_oom");
    return false;
  }
  const PdfPreparationConfig config{
      pdfHalCacheIo(preparationIoContext),
      PDF_NAVIGATION_FIXTURE_PATH,
      "/.crosspoint",
      nullptr,
      qemuNowMs,
      {&pdfResourceContext, qemuPdfResources, recordPdfResourceEvent},
      pdfHalCacheRename,
      static_cast<uint16_t>(renderer.getScreenWidth()),
      static_cast<uint16_t>(renderer.getScreenHeight()),
  };
  status = beginTrackedPdfPreparation(*preparation, config);
  PdfStepResult result = PdfStepResult::paused();
  for (uint32_t slice = 0; status.ok() && result.yielded() && slice < 100000; ++slice) {
    result = stepTrackedPdfPreparation(*preparation);
    sampleRuntime();
    yieldAfterPdfPreparationStep(result);
  }
  if (!status.ok() || !result.complete() || preparation->totalWords() != 10 ||
      preparation->resourcePeakBytes() != PdfLimits::TotalWorkspaceBytes) {
    fail("PDF_NAV", !status.ok() ? "begin" : "prepare");
    return false;
  }
  preparation.reset();

  PdfStatus documentStatus{};
  auto loaded = loadPdfHalReflowDocumentNoThrow(PDF_NAVIGATION_FIXTURE_PATH, "/.crosspoint", &documentStatus);
  if (!loaded || !documentStatus.ok() || loaded->getSectionCount() != 2 || loaded->getTocEntryCount() != 3 ||
      loaded->getTotalWordCount() != 10 || loaded->getTitle() != "XMP Navigation" ||
      loaded->getAuthor() != "XMP Author" || loaded->getLanguage() != "de-CH") {
    fail("PDF_NAV", "document");
    return false;
  }

  const ReflowTocEntry part = loaded->getTocEntry(0);
  const ReflowTocEntry chapterOne = loaded->getTocEntry(1);
  const ReflowTocEntry chapterTwo = loaded->getTocEntry(2);
  PatternPrint firstSection("aria-label=\"i\"", "sections/000001.xhtml#b00000003");
  PatternPrint secondSection("aria-label=\"A-1\"", ">Index</p>");
  const bool streamed = loaded->streamSection(0, firstSection, 256) && loaded->streamSection(1, secondSection, 256);
  const bool navigationMatches =
      part.title == "Part One" && part.level == 1 && part.sectionIndex == 0 && chapterOne.title == "Chapter One" &&
      chapterOne.level == 2 && chapterOne.parentIndex == 0 && chapterTwo.title == "Chapter Two" &&
      chapterTwo.level == 2 && chapterTwo.sectionIndex == 1 && chapterTwo.anchor == "b00000003" &&
      loaded->resolveHrefToSectionIndex(chapterTwo.href) == 1 && loaded->getTocIndexForSectionIndex(0) == 0 &&
      loaded->getTocIndexForSectionIndex(1) == 2 && streamed && firstSection.matched() && secondSection.matched();
  loaded.reset();

  const bool cleaned = Storage.exists(cacheRoot) && Storage.removeDir(cacheRoot);
  if (!navigationMatches || !cleaned) {
    fail("PDF_NAV", !navigationMatches ? "content" : "cleanup");
    return false;
  }
  esp_rom_printf("QEMU_PDF_LEGACY_NAV chapters=3 sections=2 words=10 labels=2\n");
  return true;
}

bool checkStorageOpenParity() {
  if (Storage.exists(STORAGE_PARITY_ROOT) && !Storage.removeDir(STORAGE_PARITY_ROOT)) {
    fail("STORAGE", "parity_preclean");
    return false;
  }
  if (Storage.exists(STORAGE_IMPLICIT_PARENT) && !Storage.removeDir(STORAGE_IMPLICIT_PARENT)) {
    fail("STORAGE", "implicit_parent_preclean");
    return false;
  }

  FsFile missingRead = Storage.open(STORAGE_PARITY_MISSING, O_RDONLY);
  if (missingRead || Storage.exists(STORAGE_PARITY_MISSING)) {
    fail("STORAGE", "missing_read");
    return false;
  }
  FsFile implicitParent =
      Storage.open(STORAGE_IMPLICIT_CHILD, static_cast<oflag_t>(O_WRONLY | O_CREAT | O_TRUNC));
  if (implicitParent || Storage.exists(STORAGE_IMPLICIT_PARENT)) {
    fail("STORAGE", "implicit_parent");
    return false;
  }
  if (!Storage.mkdir(STORAGE_PARITY_ROOT)) {
    fail("STORAGE", "explicit_parent");
    return false;
  }

  constexpr char seed[] = "abc";
  FsFile truncateCreate =
      Storage.open(STORAGE_PARITY_FILE, static_cast<oflag_t>(O_WRONLY | O_CREAT | O_TRUNC));
  if (!truncateCreate || truncateCreate.write(seed, sizeof(seed) - 1U) != sizeof(seed) - 1U ||
      !truncateCreate.close()) {
    fail("STORAGE", "write_truncate");
    return false;
  }

  FsFile preserveExisting = Storage.open(STORAGE_PARITY_FILE, static_cast<oflag_t>(O_RDWR | O_CREAT));
  char preserved[sizeof(seed) - 1U]{};
  if (!preserveExisting || preserveExisting.read(preserved, sizeof(preserved)) != static_cast<int>(sizeof(preserved)) ||
      std::memcmp(preserved, seed, sizeof(preserved)) != 0 || !preserveExisting.close()) {
    fail("STORAGE", "preserve_existing");
    return false;
  }

  FsFile prefixTruncate = Storage.open(STORAGE_PARITY_FILE, O_RDWR);
  if (!prefixTruncate || !prefixTruncate.truncate64(2U) || prefixTruncate.fileSize64() != 2U ||
      prefixTruncate.position() != 2U) {
    fail("STORAGE", "truncate_state");
    return false;
  }
  char prefix[2]{};
  if (!prefixTruncate.seek64(0) || prefixTruncate.read(prefix, sizeof(prefix)) != static_cast<int>(sizeof(prefix)) ||
      std::memcmp(prefix, seed, sizeof(prefix)) != 0) {
    fail("STORAGE", "truncate_prefix");
    return false;
  }
  if (!prefixTruncate.seek64(2U) || prefixTruncate.truncate64(3U) || !prefixTruncate ||
      prefixTruncate.fileSize64() != 2U || prefixTruncate.position() != 2U || !prefixTruncate.close()) {
    fail("STORAGE", "truncate_oversize");
    return false;
  }

  FsFile readOnlyTruncate = Storage.open(STORAGE_PARITY_FILE, O_RDONLY);
  if (!readOnlyTruncate || readOnlyTruncate.truncate64(1U) || !readOnlyTruncate ||
      readOnlyTruncate.fileSize64() != 2U || !readOnlyTruncate.close()) {
    fail("STORAGE", "truncate_read_only");
    return false;
  }
  esp_rom_printf("QEMU_STORAGE_TRUNCATE_PASS size=2 position=2 prefix=ab\n");

  FsFile missingReadWrite = Storage.open(STORAGE_PARITY_MISSING, static_cast<oflag_t>(O_RDWR | O_CREAT));
  if (!missingReadWrite || missingReadWrite.write(seed, sizeof(seed) - 1U) != sizeof(seed) - 1U ||
      !missingReadWrite.close()) {
    fail("STORAGE", "missing_readwrite");
    return false;
  }

  constexpr char replacement[] = "z";
  FsFile truncateExisting =
      Storage.open(STORAGE_PARITY_FILE, static_cast<oflag_t>(O_WRONLY | O_CREAT | O_TRUNC));
  if (!truncateExisting || truncateExisting.write(replacement, sizeof(replacement) - 1U) != sizeof(replacement) - 1U ||
      !truncateExisting.close()) {
    fail("STORAGE", "truncate_existing");
    return false;
  }
  FsFile verifyTruncate = Storage.open(STORAGE_PARITY_FILE, O_RDONLY);
  char verified = '\0';
  const bool truncateMatches = verifyTruncate && verifyTruncate.fileSize64() == 1U && verifyTruncate.read(&verified, 1) == 1 &&
                               verified == replacement[0] && verifyTruncate.close();
  if (!truncateMatches) {
    fail("STORAGE", "truncate_verify");
    return false;
  }

  if (!Storage.removeDir(STORAGE_PARITY_ROOT)) {
    fail("STORAGE", "parity_cleanup");
    return false;
  }
  return true;
}

bool checkStorage() {
  if (!checkStorageOpenParity()) {
    return false;
  }
  const uint32_t opensBefore = QemuHalControl::storageOpenCount();
  const uint32_t closesBefore = QemuHalControl::storageCloseCount();
  FsFile sentinel = Storage.open(SENTINEL_PATH, O_RDONLY);
  if (!sentinel) {
    fail("STORAGE", "open");
    return false;
  }

  char bytes[sizeof(SENTINEL_CONTENT) - 1];
  const uint64_t fileSize = sentinel.fileSize64();
  const int bytesRead = sentinel.read(bytes, sizeof(bytes));
  const bool closed = sentinel.close();
  const bool countersBalanced =
      QemuHalControl::storageOpenCount() == opensBefore + 1 && QemuHalControl::storageCloseCount() == closesBefore + 1;

  if (fileSize != sizeof(bytes) || bytesRead != static_cast<int>(sizeof(bytes)) || !closed || !countersBalanced ||
      std::memcmp(bytes, SENTINEL_CONTENT, sizeof(bytes)) != 0) {
    fail("STORAGE", "content");
    return false;
  }

  esp_rom_printf("QEMU_STORAGE_PASS path=%s bytes=%lu\n", SENTINEL_PATH, static_cast<unsigned long>(sizeof(bytes)));
  return true;
}

bool checkFrame() {
  if (display.getBufferSize() != EXPECTED_FRAME_BYTES || display.getFrameBuffer() == nullptr) {
    fail("FRAME", "buffer");
    return false;
  }

  display.clearScreen(0xFF);
  display.getFrameBuffer()[0] = 0x7F;
  display.displayBuffer(HalDisplay::FAST_REFRESH);
  const uint32_t crc32 = QemuHalControl::frameCrc32();
  if (crc32 != EXPECTED_FRAME_CRC32) {
    fail("FRAME", "crc32");
    return false;
  }

  esp_rom_printf("QEMU_FRAME_PASS bytes=%lu crc32=%08lX\n", static_cast<unsigned long>(display.getBufferSize()),
                 static_cast<unsigned long>(crc32));
  return true;
}

bool checkInput(MappedInputManager& input) {
  constexpr MappedInputManager::Button BUTTON = MappedInputManager::Button::Down;
  input.simulatorInjectPress(BUTTON);
  const bool pressObserved = input.wasPressed(BUTTON);
  input.simulatorClearInputFrame();
  input.simulatorInjectRelease(BUTTON);
  const bool releaseObserved = input.wasReleased(BUTTON);
  input.simulatorClearInputFrame();

  if (!pressObserved || !releaseObserved || input.isPressed(BUTTON)) {
    fail("INPUT", "logical_down");
    return false;
  }

  esp_rom_printf("QEMU_INPUT_PASS button=DOWN press=1 release=1\n");
  return true;
}
}  // namespace

void qemuAcceptanceBegin(MappedInputManager& input, GfxRenderer& renderer) {
  (void)input;
  state = {};
  persistentAcceptance = {};
  PersistentAcceptanceState persistent;
  const bool persistentStateExists = Storage.exists(PDF_ACCEPTANCE_STATE_PATH);
  const bool resumedBoot = persistentStateExists && loadPersistentState(&persistent);
  if (persistentStateExists && !resumedBoot) {
    esp_rom_printf("QEMU_BOOT seq=0\n");
    fail("PDF_STATE", "corrupt");
    return;
  }
  state.bootSequence = resumedBoot ? 1 : 0;
  esp_rom_printf("QEMU_BOOT seq=%u\n", static_cast<unsigned>(state.bootSequence));
  state.heapStart = ESP.getFreeHeap();
  state.minFreeHeap = state.heapStart;
  state.minMaxAllocation = ESP.getMaxAllocHeap();
  state.minStackMargin = stackMarginBytes();
  sampleRuntime();
  pinReaderSettings(renderer);

  if (hostPdfAvailable()) {
    state.phase = runExternalPdf(renderer) ? AcceptancePhase::Finished : AcceptancePhase::Failed;
    return;
  }

  if (!resumedBoot) {
    if (!checkRamBudget() || !checkStorage() || !checkPdfCore() || !checkPdfCache() || !checkRawPdfFixtures() ||
        !checkPdfCancellation(renderer, &persistent)) {
      return;
    }
    emitRuntimeSample();
    esp_rom_printf("QEMU_EXPECT_RESET seq=0\n");
    delay(10);
    ESP.restart();
    fail("PDF_RESET", "returned");
    return;
  }

  persistentAcceptance = persistent;
  state.phase = AcceptancePhase::BootTwoResume;
}

void qemuAcceptanceTick(MappedInputManager& input, GfxRenderer& renderer) {
  switch (state.phase) {
    case AcceptancePhase::BootTwoResume:
      if (checkPdfResume(renderer, persistentAcceptance)) {
        state.phase = AcceptancePhase::BootTwoTypography;
      }
      return;
    case AcceptancePhase::BootTwoTypography:
      if (checkPdfTypography(renderer)) {
        state.phase = AcceptancePhase::BootTwoNavigation;
      }
      return;
    case AcceptancePhase::BootTwoNavigation:
      if (checkPdfFullNavigation(renderer)) {
        state.phase = AcceptancePhase::BootTwoImage;
      }
      return;
    case AcceptancePhase::BootTwoImage:
      if (checkPdfImage(renderer)) {
        state.phase = AcceptancePhase::BootTwoPositiveCorpus;
      }
      return;
    case AcceptancePhase::BootTwoPositiveCorpus:
      if (checkPdfPositiveCorpus(renderer)) {
        state.phase = AcceptancePhase::BootTwoProgress;
      }
      return;
    case AcceptancePhase::BootTwoProgress:
      if (checkPdfProgressAndSavedItems(renderer)) {
        state.phase = AcceptancePhase::BootTwoCacheReopen;
      }
      return;
    case AcceptancePhase::BootTwoCacheReopen:
      if (checkPdfCompletedCacheReopen(renderer)) {
        state.phase = AcceptancePhase::BootTwoNegativeCorpus;
      }
      return;
    case AcceptancePhase::BootTwoNegativeCorpus:
      if (checkPdfNegativeCorpus(renderer)) {
        state.phase = AcceptancePhase::BootTwoEpub;
      }
      return;
    case AcceptancePhase::BootTwoEpub:
      if (checkEpubOracle(renderer)) {
        state.phase = AcceptancePhase::BootTwoProductTracer;
      }
      return;
    case AcceptancePhase::BootTwoProductTracer:
      if (checkPdfProductTracer(renderer)) {
        state.phase = AcceptancePhase::BootTwoStorage;
      }
      return;
    case AcceptancePhase::BootTwoStorage:
      if (checkStorage()) {
        state.phase = AcceptancePhase::BootTwoFrame;
      }
      return;
    case AcceptancePhase::BootTwoFrame:
      if (checkFrame()) {
        state.phase = AcceptancePhase::BootTwoInput;
      }
      return;
    case AcceptancePhase::BootTwoInput:
      if (checkInput(input)) {
        state.fullAcceptanceReady = true;
        state.idleStartedAt = millis();
        state.phase = AcceptancePhase::WaitingForPowerSaving;
        sampleRuntime();
      }
      return;
    case AcceptancePhase::WaitingForPowerSaving:
      break;
    default:
      return;
  }

  sampleRuntime();
  if (millis() - state.idleStartedAt < HalPowerManager::IDLE_POWER_SAVING_MS || !QemuHalControl::powerSavingEnabled()) {
    return;
  }

  esp_rom_printf("QEMU_POWER_PASS idle_ms=%lu saving=1\n",
                 static_cast<unsigned long>(HalPowerManager::IDLE_POWER_SAVING_MS));
  emitRuntimeSample();
  if (state.pdfTracerReady) {
    esp_rom_printf("QEMU_PDF_TRACER_PASS\n");
  }
  esp_rom_printf("QEMU_TRACER_PASS\n");
  if (state.fullAcceptanceReady) {
#ifdef CROSSINK_QEMU_TIMING_DIAGNOSTIC
    esp_rom_printf("QEMU_TIMING_DIAGNOSTIC_COMPLETE\n");
#else
    esp_rom_printf("QEMU_TEST_PASS\n");
#endif
  }
  state.phase = AcceptancePhase::Finished;
}

#endif
