#include "QemuAcceptance.h"

#ifdef CROSSINK_QEMU

#include <Arduino.h>
#include <HalDisplay.h>
#include <HalPowerManager.h>
#include <HalStorage.h>
#include <PdfCacheStore.h>
#include <PdfDocumentTextClassifier.h>
#include <PdfHalCacheIo.h>
#include <PdfHalIo.h>
#include <PdfHiddenText.h>
#include <PdfPageTree.h>
#include <PdfReadingOrder.h>
#include <PdfRunStore.h>
#include <PdfSemanticWriter.h>
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
constexpr char PDF_SPILL_PATH[] = "/qemu/pdf-run-spill.tmp";
constexpr char PDF_CACHE_ROOT[] = "/qemu/pdf_cache_accept";
constexpr char PDF_CACHE_GENERATION_ONE[] = "/qemu/pdf_cache_accept/gen_1";
constexpr char PDF_CACHE_GENERATION_TWO[] = "/qemu/pdf_cache_accept/gen_2";
constexpr char PDF_CACHE_METADATA_PATH[] = "/qemu/pdf_cache_accept/gen_1/metadata.bin";
constexpr char PDF_CACHE_PARTIAL_PATH[] = "/qemu/pdf_cache_accept/gen_2/partial.bin";
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
  PdfTextRun reductionRuns[4]{};
  uint8_t reductionText[256]{};
  PdfReadingOrderItem reductionOrder[4]{};
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

  if (!checkStorage() || !checkPdfCore() || !checkPdfCache() || !checkFrame() || !checkInput(input)) {
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
