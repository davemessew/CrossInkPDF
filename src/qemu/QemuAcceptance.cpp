#include "QemuAcceptance.h"

#ifdef CROSSINK_QEMU

#include <Arduino.h>
#include <HalDisplay.h>
#include <HalPowerManager.h>
#include <HalStorage.h>
#include <PdfHalIo.h>
#include <PdfPageTree.h>
#include <QemuHalControl.h>
#include <esp_rom_sys.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <cstring>

#include "MappedInputManager.h"
#include "Memory.h"

namespace {
constexpr char SENTINEL_PATH[] = "/qemu/sentinel.txt";
constexpr char SENTINEL_CONTENT[] = "crossink-qemu-sentinel-v1\n";
constexpr char PDF_FIXTURE_PATH[] = "/qemu/classic_text.pdf";
constexpr char PDF_EXPECTED_TEXT[] = "Hello PDF";
constexpr uint32_t EXPECTED_FRAME_BYTES = 48000;
constexpr uint32_t EXPECTED_FRAME_CRC32 = 0x0F7C8C45;
constexpr uint32_t NO_ALLOCATION_SAMPLE = 0;

enum class AcceptancePhase : uint8_t { NotStarted, WaitingForPowerSaving, Failed, Finished };

struct AcceptanceState {
  AcceptancePhase phase = AcceptancePhase::NotStarted;
  uint32_t idleStartedAt = 0;
  uint32_t heapStart = 0;
  uint32_t minFreeHeap = 0;
  uint32_t minMaxAllocation = 0;
  uint32_t minStackMargin = 0;
};

AcceptanceState state;

struct MemoryRecordContext {
  uint8_t* bytes = nullptr;
  size_t recordSize = 0;
  uint32_t capacity = 0;
};

struct PdfCoreAcceptanceWorkspace {
  uint8_t sourceBuffer[PdfLimits::SourceBufferBytes]{};
  PdfValue values[128]{};
  PdfDictionaryEntry dictionaryEntries[128]{};
  PdfArrayItem arrayItems[128]{};
  uint8_t objectText[2048]{};
  PdfXrefEntry xrefEntries[128]{};
  PdfPageTreeRecord traversalRecords[64]{};
  PdfPageInfo firstPage{};
  char transcript[32]{};
  uint32_t pageCount = 0;
};
static_assert(sizeof(PdfCoreAcceptanceWorkspace) <= 32768);

void fail(const char* component, const char* reason) {
  esp_rom_printf("QEMU_%s_FAIL reason=%s\n", component, reason);
  state.phase = AcceptancePhase::Failed;
}

uint32_t stackMarginBytes() {
  return static_cast<uint32_t>(uxTaskGetStackHighWaterMark(nullptr)) * static_cast<uint32_t>(sizeof(StackType_t));
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

PdfStatus readMemoryRecord(void* context, const uint32_t ordinal, void* record, const size_t recordSize) {
  if (context == nullptr || record == nullptr) {
    return PdfStatus::failure(PdfError::InvalidArgument, ordinal);
  }
  auto& records = *static_cast<MemoryRecordContext*>(context);
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
  if (recordSize != records.recordSize || ordinal >= records.capacity) {
    return PdfStatus::failure(PdfError::InvalidOffset, ordinal);
  }
  std::memcpy(records.bytes + ordinal * recordSize, record, recordSize);
  return PdfStatus::success();
}

PdfFixedRecordStore memoryRecordStore(MemoryRecordContext& context) {
  return {&context, context.capacity, context.recordSize, readMemoryRecord, writeMemoryRecord};
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

  auto walker = makeUniqueNoThrow<PdfPageTreeWalker>(*resolver, arena, memoryRecordStore(traversalRecords),
                                                     captureFirstPdfPage, workspace.get());
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
  if (!status.ok() || workspace->pageCount != 1 || workspace->firstPage.contentCount != 1) {
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
  return true;
}

bool checkStorage() {
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

void qemuAcceptanceBegin(MappedInputManager& input) {
  state = {};
  esp_rom_printf("QEMU_BOOT seq=0\n");

  if (!checkStorage() || !checkPdfCore() || !checkFrame() || !checkInput(input)) {
    return;
  }

  state.heapStart = ESP.getFreeHeap();
  state.minFreeHeap = state.heapStart;
  state.minMaxAllocation = ESP.getMaxAllocHeap();
  state.minStackMargin = stackMarginBytes();
  state.idleStartedAt = millis();
  state.phase = AcceptancePhase::WaitingForPowerSaving;
  sampleRuntime();
}

void qemuAcceptanceTick() {
  if (state.phase != AcceptancePhase::WaitingForPowerSaving) {
    return;
  }

  sampleRuntime();
  if (millis() - state.idleStartedAt < HalPowerManager::IDLE_POWER_SAVING_MS || !QemuHalControl::powerSavingEnabled()) {
    return;
  }

  esp_rom_printf("QEMU_POWER_PASS idle_ms=%lu saving=1\n",
                 static_cast<unsigned long>(HalPowerManager::IDLE_POWER_SAVING_MS));
  esp_rom_printf("QEMU_RUNTIME heap_start=%lu min_free=%lu min_max_alloc=%lu max_alloc=%lu stack_margin=%lu\n",
                 static_cast<unsigned long>(state.heapStart), static_cast<unsigned long>(state.minFreeHeap),
                 static_cast<unsigned long>(state.minMaxAllocation), static_cast<unsigned long>(NO_ALLOCATION_SAMPLE),
                 static_cast<unsigned long>(state.minStackMargin));
  esp_rom_printf("QEMU_TRACER_PASS\n");
  state.phase = AcceptancePhase::Finished;
}

#endif
