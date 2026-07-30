#include "PdfPreparation.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <new>

#include "Memory.h"

namespace {

constexpr uint32_t kSliceMilliseconds = 8;
constexpr uint32_t kSliceOperations = 32;
constexpr uint64_t kSectionByteLimit = 1024ULL * 1024ULL;
constexpr uint16_t kArenaEntryCount = 128;
constexpr uint16_t kArenaTextBytes = 1536;
constexpr uint32_t kXrefRecordCount = 256;
constexpr uint32_t kTraversalRecordCount = 64;

struct ArenaWorkspace {
  PdfValue values[kArenaEntryCount]{};
  PdfDictionaryEntry dictionaries[kArenaEntryCount]{};
  PdfArrayItem arrays[kArenaEntryCount]{};
  uint8_t text[kArenaTextBytes]{};
};

struct RecordWorkspace {
  PdfXrefEntry xref[kXrefRecordCount]{};
  PdfPageTreeRecord traversal[kTraversalRecordCount]{};
};

static_assert(sizeof(ArenaWorkspace) <= PdfLimits::PageTextBytes);
static_assert(sizeof(RecordWorkspace) <= PdfLimits::PageRunBytes);

bool copyPath(const char* source, char* destination, const size_t capacity) {
  if (source == nullptr || destination == nullptr || capacity == 0) {
    return false;
  }
  const size_t length = std::strlen(source);
  if (length == 0 || length >= capacity) {
    destination[0] = '\0';
    return false;
  }
  std::memcpy(destination, source, length + 1);
  return true;
}

uint32_t deterministicGeneration(const PdfSourceIdentity& identity) {
  uint32_t generation = static_cast<uint32_t>(identity.headFingerprint ^ (identity.headFingerprint >> 32U) ^
                                              identity.tailFingerprint ^ (identity.tailFingerprint >> 32U));
  if (generation == 0) {
    generation = 1;
  }
  return generation;
}

}  // namespace

bool PdfPreparationPaintGate::shouldPaint(const uint8_t progressPercent, const uint32_t nowMs) {
  if (intermediatePaintCount_ >= 10 || progressPercent < lastPaintPercent_ ||
      static_cast<uint8_t>(progressPercent - lastPaintPercent_) < 10 || nowMs - lastPaintMs_ < 15000U) {
    return false;
  }
  lastPaintPercent_ = progressPercent;
  lastPaintMs_ = nowMs;
  ++intermediatePaintCount_;
  return true;
}

PdfPreparation::~PdfPreparation() {
  destroyParsers();
  if (sectionWriter_.open) {
    pdfAbortTrackedCacheFile(&sectionWriter_);
  }
  (void)closeSource();
  releaseWorkspaces();
}

PdfStatus PdfPreparation::begin(const PdfPreparationConfig& config) {
  destroyParsers();
  if (sectionWriter_.open) {
    pdfAbortTrackedCacheFile(&sectionWriter_);
  }
  (void)closeSource();
  releaseWorkspaces();
  resources_.reset();

  if (!config.io.valid() || config.sourcePath == nullptr || config.cacheDirectory == nullptr ||
      config.resourceHooks.measure == nullptr || !copyPath(config.sourcePath, sourcePath_, sizeof(sourcePath_))) {
    phase_ = PdfPreparationPhase::Failed;
    status_ = PdfStatus::failure(PdfError::InvalidArgument);
    return status_;
  }

  config_ = config;
  status_ = pdfFormatCacheRoot(config.cacheDirectory, sourcePath_, cacheRoot_, sizeof(cacheRoot_));
  if (!status_) {
    phase_ = PdfPreparationPhase::Failed;
    return status_;
  }

  resources_.emplace(config.resourceHooks);
  sourceHandle_ = {};
  sourceMetadata_ = {};
  sourceIdentity_ = {};
  sourceContext_ = {};
  arena_ = {};
  xrefRecords_ = {};
  traversalRecords_ = {};
  firstPage_ = {};
  pageCount_ = 0;
  contentRange_ = {};
  transcriptLength_ = 0;
  cacheStore_ = {};
  cacheCapacity_ = {};
  cacheBudget_ = {};
  checkpointSelection_ = PdfBuildCheckpointSelection{};
  sectionWriter_ = {};
  sectionRecord_ = {};
  semanticWriter_ = {};
  generation_ = 0;
  sequence_ = 0;
  totalWords_ = 0;
  progressPercent_ = 0;
  allocationIndex_ = 0;
  cancelRequested_ = false;
  resumedFromCheckpoint_ = false;
  phase_ = PdfPreparationPhase::ResourceGate;
  status_ = PdfStatus::success();
  return status_;
}

size_t PdfPreparation::resourceCurrentBytes() const { return resources_.has_value() ? resources_->currentBytes() : 0; }

size_t PdfPreparation::resourcePeakBytes() const { return resources_.has_value() ? resources_->peakBytes() : 0; }

uint32_t PdfPreparation::nowMs() const { return config_.nowMs == nullptr ? 0 : config_.nowMs(config_.clockContext); }

void PdfPreparation::setPhase(const PdfPreparationPhase phase, const uint8_t progressPercent) {
  phase_ = phase;
  progressPercent_ = progressPercent;
}

PdfStepResult PdfPreparation::pause() { return PdfStepResult::paused(); }

PdfStepResult PdfPreparation::fail(const PdfStatus status) {
  status_ = status.ok() ? PdfStatus::failure(PdfError::Malformed) : status;
  destroyParsers();
  if (sectionWriter_.open) {
    pdfAbortTrackedCacheFile(&sectionWriter_);
  }
  (void)closeSource();
  if (generation_ != 0 && sourceIdentity_.size != 0) {
    (void)commitCheckpoint(PdfBuildPhase::Failed);
  }
  releaseWorkspaces();
  phase_ = PdfPreparationPhase::Failed;
  return PdfStepResult::failure(status_);
}

PdfStepResult PdfPreparation::cancel() {
  destroyParsers();
  if (sectionWriter_.open) {
    pdfAbortTrackedCacheFile(&sectionWriter_);
  }
  (void)closeSource();
  if (generation_ != 0 && sourceIdentity_.size != 0) {
    (void)commitCheckpoint(PdfBuildPhase::Cancelled);
  }
  releaseWorkspaces();
  status_ = PdfStatus::failure(PdfError::Cancelled);
  phase_ = PdfPreparationPhase::Cancelled;
  return PdfStepResult::failure(status_);
}

bool PdfPreparation::stopRequested(void* context) {
  auto& self = *static_cast<PdfPreparation*>(context);
  if (self.cancelRequested_) {
    return true;
  }
  return self.config_.nowMs != nullptr && self.nowMs() - self.sliceStartedAtMs_ >= kSliceMilliseconds;
}

PdfStatus PdfPreparation::readSource(void* context, const uint64_t offset, uint8_t* destination, const size_t requested,
                                     size_t* bytesRead) {
  if (context == nullptr) {
    return PdfStatus::failure(PdfError::InvalidArgument, offset);
  }
  auto& source = *static_cast<SourceContext*>(context);
  if (source.io == nullptr || source.handle == nullptr || !source.handle->valid() || offset > source.size ||
      requested > PdfLimits::SourceBufferBytes) {
    return PdfStatus::failure(PdfError::InvalidArgument, offset);
  }
  return source.io->read(source.io->context, *source.handle, offset, destination, requested, bytesRead);
}

PdfByteSource PdfPreparation::source() { return {&sourceContext_, sourceMetadata_.size, readSource}; }

PdfStatus PdfPreparation::readMemoryRecord(void* context, const uint32_t ordinal, void* record,
                                           const size_t recordSize) {
  if (context == nullptr || record == nullptr) {
    return PdfStatus::failure(PdfError::InvalidArgument, ordinal);
  }
  auto& records = *static_cast<MemoryRecordContext*>(context);
  if (recordSize != records.recordSize || ordinal >= records.capacity) {
    return PdfStatus::failure(PdfError::InvalidOffset, ordinal);
  }
  std::memcpy(record, records.bytes + static_cast<size_t>(ordinal) * recordSize, recordSize);
  return PdfStatus::success();
}

PdfStatus PdfPreparation::writeMemoryRecord(void* context, const uint32_t ordinal, const void* record,
                                            const size_t recordSize) {
  if (context == nullptr || record == nullptr) {
    return PdfStatus::failure(PdfError::InvalidArgument, ordinal);
  }
  auto& records = *static_cast<MemoryRecordContext*>(context);
  if (recordSize != records.recordSize || ordinal >= records.capacity) {
    return PdfStatus::failure(PdfError::InvalidOffset, ordinal);
  }
  std::memcpy(records.bytes + static_cast<size_t>(ordinal) * recordSize, record, recordSize);
  return PdfStatus::success();
}

PdfFixedRecordStore PdfPreparation::recordStore(MemoryRecordContext& context) {
  return {&context, context.capacity, context.recordSize, readMemoryRecord, writeMemoryRecord};
}

PdfStatus PdfPreparation::capturePage(void* context, const PdfPageInfo& page) {
  if (context == nullptr) {
    return PdfStatus::failure(PdfError::InvalidArgument);
  }
  auto& self = *static_cast<PdfPreparation*>(context);
  if (self.pageCount_ == 0) {
    self.firstPage_ = page;
  }
  ++self.pageCount_;
  return self.pageCount_ <= 1 ? PdfStatus::success() : PdfStatus::failure(PdfError::Unsupported, page.pageIndex);
}

PdfStatus PdfPreparation::writeSection(void* context, const uint8_t* source, const size_t requested,
                                       size_t* bytesWritten) {
  if (context == nullptr || bytesWritten == nullptr || requested > PdfLimits::SourceBufferBytes) {
    return PdfStatus::failure(PdfError::InvalidArgument);
  }
  auto& self = *static_cast<PdfPreparation*>(context);
  const PdfStatus status = pdfWriteTrackedCacheFile(&self.sectionWriter_, source, requested);
  *bytesWritten = status ? requested : 0;
  return status;
}

PdfStatus PdfPreparation::emitBlock(void*, const PdfSemanticBlockRecord&) { return PdfStatus::success(); }

PdfStatus PdfPreparation::readRequiredFile(void* context, const uint32_t index, PdfRequiredFileRecord* record) {
  if (context == nullptr || record == nullptr || index != 0) {
    return PdfStatus::failure(PdfError::InvalidArgument, index);
  }
  *record = static_cast<PdfPreparation*>(context)->sectionRecord_;
  return PdfStatus::success();
}

void PdfPreparation::destroyParsers() {
  contentLexer_.reset();
  pageWalker_.reset();
  resolver_.reset();
  xrefParser_.reset();
  xref_.reset();
}

PdfStatus PdfPreparation::closeSource() {
  if (!sourceHandle_.valid() || !config_.io.valid()) {
    sourceHandle_ = {};
    sourceContext_ = {};
    return PdfStatus::success();
  }
  const PdfStatus status = config_.io.close(config_.io.context, &sourceHandle_);
  sourceContext_ = {};
  return status;
}

void PdfPreparation::releaseWorkspaces() {
  if (!resources_.has_value()) {
    dictionary_.reset();
    sourceWindow_.reset();
    decoderOutput_.reset();
    pageText_.reset();
    runRecords_.reset();
    operandScratch_.reset();
    return;
  }

  if (operandScratch_) {
    operandScratch_.reset();
    (void)resources_->release(PdfResourceKind::OperandScratch);
  }
  if (runRecords_) {
    runRecords_.reset();
    (void)resources_->release(PdfResourceKind::RunRecords);
  }
  if (pageText_) {
    pageText_.reset();
    (void)resources_->release(PdfResourceKind::PageText);
  }
  if (decoderOutput_) {
    decoderOutput_.reset();
    (void)resources_->release(PdfResourceKind::DecoderOutput);
  }
  if (sourceWindow_) {
    sourceWindow_.reset();
    (void)resources_->release(PdfResourceKind::SourceWindow);
  }
  if (dictionary_) {
    dictionary_.reset();
    (void)resources_->release(PdfResourceKind::InflateDictionary);
  }
}

bool PdfPreparation::allocateNextWorkspace() {
  if (!resources_.has_value() || allocationIndex_ >= PDF_RESOURCE_SLOT_COUNT) {
    return false;
  }

  PdfResourceKind kind = PdfResourceKind::InflateDictionary;
  size_t bytes = 0;
  bool allocated = false;
  switch (allocationIndex_) {
    case 0:
      kind = PdfResourceKind::InflateDictionary;
      bytes = PdfLimits::UzlibDictionaryBytes;
      dictionary_ = makeUniqueNoThrow<uint8_t[]>(bytes);
      allocated = dictionary_ != nullptr;
      break;
    case 1:
      kind = PdfResourceKind::SourceWindow;
      bytes = PdfLimits::SourceBufferBytes;
      sourceWindow_ = makeUniqueNoThrow<uint8_t[]>(bytes);
      allocated = sourceWindow_ != nullptr;
      break;
    case 2:
      kind = PdfResourceKind::DecoderOutput;
      bytes = PdfLimits::DecoderOutputBytes;
      decoderOutput_ = makeUniqueNoThrow<uint8_t[]>(bytes);
      allocated = decoderOutput_ != nullptr;
      break;
    case 3:
      kind = PdfResourceKind::PageText;
      bytes = PdfLimits::PageTextBytes;
      pageText_ = makeUniqueNoThrow<uint8_t[]>(bytes);
      allocated = pageText_ != nullptr;
      break;
    case 4:
      kind = PdfResourceKind::RunRecords;
      bytes = PdfLimits::PageRunBytes;
      runRecords_ = makeUniqueNoThrow<uint8_t[]>(bytes);
      allocated = runRecords_ != nullptr;
      break;
    case 5:
      kind = PdfResourceKind::OperandScratch;
      bytes = PdfLimits::OperandOrderHistogramBytes;
      operandScratch_ = makeUniqueNoThrow<uint8_t[]>(bytes);
      allocated = operandScratch_ != nullptr;
      break;
    default:
      return false;
  }

  if (!allocated || !resources_->acquire(kind, bytes)) {
    switch (allocationIndex_) {
      case 0:
        dictionary_.reset();
        break;
      case 1:
        sourceWindow_.reset();
        break;
      case 2:
        decoderOutput_.reset();
        break;
      case 3:
        pageText_.reset();
        break;
      case 4:
        runRecords_.reset();
        break;
      case 5:
        operandScratch_.reset();
        break;
      default:
        break;
    }
    return false;
  }
  ++allocationIndex_;
  return true;
}

PdfStatus PdfPreparation::initializeParserStorage() {
  if (!pageText_ || !runRecords_) {
    return PdfStatus::failure(PdfError::InsufficientMemory);
  }
  auto* arenaWorkspace = new (pageText_.get()) ArenaWorkspace{};
  auto* recordWorkspace = new (runRecords_.get()) RecordWorkspace{};
  arena_ = {
      arenaWorkspace->values,       static_cast<uint16_t>(std::size(arenaWorkspace->values)),
      arenaWorkspace->dictionaries, static_cast<uint16_t>(std::size(arenaWorkspace->dictionaries)),
      arenaWorkspace->arrays,       static_cast<uint16_t>(std::size(arenaWorkspace->arrays)),
      arenaWorkspace->text,         static_cast<uint16_t>(std::size(arenaWorkspace->text)),
  };
  xrefRecords_ = {
      reinterpret_cast<uint8_t*>(recordWorkspace->xref),
      sizeof(PdfXrefEntry),
      static_cast<uint32_t>(std::size(recordWorkspace->xref)),
  };
  traversalRecords_ = {
      reinterpret_cast<uint8_t*>(recordWorkspace->traversal),
      sizeof(PdfPageTreeRecord),
      static_cast<uint32_t>(std::size(recordWorkspace->traversal)),
  };
  xref_.emplace(recordStore(xrefRecords_));
  return PdfStatus::success();
}

PdfStatus PdfPreparation::setupCache() {
  PdfStatus status = config_.io.mkdir(config_.io.context, config_.cacheDirectory);
  if (!status) {
    return status;
  }
  status = cacheStore_.initialize(config_.io, cacheRoot_);
  if (!status) {
    return status;
  }
  status = cacheStore_.loadCheckpointSlots(sourceIdentity_, &checkpointSelection_);
  if (!status) {
    return status;
  }
  if (checkpointSelection_.selected) {
    resumedFromCheckpoint_ = true;
    generation_ = checkpointSelection_.checkpoint.generation;
    sequence_ = checkpointSelection_.checkpoint.sequence + 1U;
    if (sequence_ == 0) {
      sequence_ = 1;
    }
  } else {
    generation_ = deterministicGeneration(sourceIdentity_);
    sequence_ = 1;
  }
  if (generation_ == 0) {
    generation_ = 1;
  }
  status = cacheStore_.ensureGeneration(generation_);
  if (!status) {
    return status;
  }
  status = config_.io.capacity(config_.io.context, &cacheCapacity_);
  if (!status) {
    return status;
  }
  status = pdfInitializeCacheBudget(sourceIdentity_.size, cacheCapacity_, 0, &cacheBudget_);
  if (!status || cacheBudget_.limit == 0) {
    return status ? PdfStatus::failure(PdfError::InsufficientStorage) : status;
  }

  const int relativeLength = std::snprintf(sectionRelativePath_, sizeof(sectionRelativePath_), "gen_%lu/section.xhtml",
                                           static_cast<unsigned long>(generation_));
  const int fullLength = std::snprintf(sectionPath_, sizeof(sectionPath_), "%s/%s", cacheRoot_, sectionRelativePath_);
  if (relativeLength < 0 || static_cast<size_t>(relativeLength) >= sizeof(sectionRelativePath_) || fullLength < 0 ||
      static_cast<size_t>(fullLength) >= sizeof(sectionPath_)) {
    return PdfStatus::failure(PdfError::LimitExceeded);
  }
  return PdfStatus::success();
}

PdfStatus PdfPreparation::startXref() {
  if (!xref_.has_value() || !sourceWindow_) {
    return PdfStatus::failure(PdfError::InvalidArgument);
  }
  xrefParser_.emplace(source(), sourceWindow_.get(), PdfLimits::SourceBufferBytes, arena_, *xref_);
  xrefParser_->begin();
  return PdfStatus::success();
}

PdfStatus PdfPreparation::finishXref() {
  PdfObjectReference root{};
  if (!xref_.has_value() || !xref_->root(&root)) {
    return PdfStatus::failure(PdfError::Malformed);
  }
  xrefParser_.reset();
  resolver_.emplace(source(), *xref_, sourceWindow_.get(), PdfLimits::SourceBufferBytes, arena_);
  return resolver_->begin(root);
}

PdfStatus PdfPreparation::finishCatalog() {
  if (!resolver_.has_value()) {
    return PdfStatus::failure(PdfError::InvalidArgument);
  }
  uint16_t pagesIndex = PDF_INVALID_INDEX;
  if (!pdfDictionaryFind(arena_, resolver_->result().rootIndex, "Pages", &pagesIndex) ||
      pagesIndex >= arena_.valueCount || arena_.values[pagesIndex].kind != PdfValueKind::Reference) {
    return PdfStatus::failure(PdfError::Malformed);
  }
  const PdfValue& pages = arena_.values[pagesIndex];
  pageCount_ = 0;
  firstPage_ = {};
  pageWalker_.emplace(*resolver_, arena_, recordStore(traversalRecords_), capturePage, this);
  return pageWalker_->begin({pages.objectNumber, pages.generation});
}

PdfStatus PdfPreparation::finishPageTree() {
  pageWalker_.reset();
  if (!resolver_.has_value() || pageCount_ == 0) {
    return PdfStatus::failure(PdfError::NoReadableText);
  }
  if (pageCount_ != 1 || firstPage_.contentCount != 1) {
    return PdfStatus::failure(PdfError::Unsupported);
  }
  return resolver_->begin(firstPage_.contents[0]);
}

PdfStatus PdfPreparation::finishContentObject() {
  if (!resolver_.has_value() || !resolver_->result().hasStream) {
    return PdfStatus::failure(PdfError::Malformed);
  }
  const PdfResolvedObject content = resolver_->result();
  PdfStatus status = pdfInitializeByteRange(source(), content.streamOffset, content.streamLength, &contentRange_);
  if (!status) {
    return status;
  }
  resolver_.reset();
  xref_.reset();
  arena_ = {};
  transcriptLength_ = 0;
  contentLexer_.emplace(pdfByteRangeSource(contentRange_), sourceWindow_.get(), PdfLimits::SourceBufferBytes);
  return PdfStatus::success();
}

PdfStatus PdfPreparation::appendContentToken(const PdfToken& token) {
  if (token.kind != PdfTokenKind::String && token.kind != PdfTokenKind::HexString) {
    return PdfStatus::success();
  }
  const size_t separator = transcriptLength_ == 0 ? 0 : 1;
  if (!pageText_ || token.length > PdfLimits::PageTextBytes ||
      transcriptLength_ > PdfLimits::PageTextBytes - separator - token.length) {
    return PdfStatus::failure(PdfError::LimitExceeded);
  }
  if (separator != 0) {
    pageText_[transcriptLength_++] = ' ';
  }
  std::memcpy(pageText_.get() + transcriptLength_, token.bytes, token.length);
  transcriptLength_ += token.length;
  return PdfStatus::success();
}

PdfStatus PdfPreparation::openSection() {
  if (transcriptLength_ == 0) {
    return PdfStatus::failure(PdfError::NoReadableText);
  }
  const uint64_t byteLimit = std::min<uint64_t>(cacheBudget_.limit, kSectionByteLimit);
  if (byteLimit == 0) {
    return PdfStatus::failure(PdfError::InsufficientStorage);
  }
  PdfStatus status = pdfOpenTrackedCacheWriter(config_.io, sectionPath_, sectionRelativePath_,
                                               PdfCacheFileKind::Required, byteLimit, &sectionWriter_);
  if (!status) {
    return status;
  }
  status = semanticWriter_.begin({this, writeSection}, {this, emitBlock},
                                 {operandScratch_.get(), PdfLimits::OperandOrderHistogramBytes});
  if (!status) {
    pdfAbortTrackedCacheFile(&sectionWriter_);
  }
  return status;
}

PdfStatus PdfPreparation::emitSection() {
  PdfStatus status = semanticWriter_.beginBlock({PdfSemanticBlockKind::Paragraph, 0, 0});
  if (status) {
    status = semanticWriter_.writeText(pageText_.get(), transcriptLength_);
  }
  if (status) {
    status = semanticWriter_.endBlock();
  }
  if (status) {
    status = semanticWriter_.finish();
  }
  if (status) {
    totalWords_ = semanticWriter_.totalWords();
  }
  return status;
}

PdfStatus PdfPreparation::closeSection() {
  PdfStatus status = pdfCloseTrackedCacheFile(&sectionWriter_, &sectionRecord_);
  if (status) {
    status = pdfReserveCacheBytes(&cacheBudget_, sectionRecord_.size, PdfCacheFileKind::Required);
  }
  return status;
}

PdfStatus PdfPreparation::commitManifest() {
  PdfCacheManifest manifest{};
  manifest.sequence = sequence_;
  manifest.completed = true;
  manifest.source = sourceIdentity_;
  manifest.generation = generation_;
  manifest.totalWords = totalWords_;
  manifest.requiredFileCount = 1;
  manifest.requiredFileBytes = sectionRecord_.size;
  manifest.requiredFileLedger = pdfUpdateRequiredFileLedger(PDF_CACHE_FNV64_OFFSET, sectionRecord_);
  const PdfRequiredFileTableSource files{this, 1, readRequiredFile};
  const PdfCacheCommitEvidence evidence{
      true,
      1,
      sectionRecord_.size,
      manifest.requiredFileLedger,
  };
  const PdfCacheManifestSelection prior{};
  return cacheStore_.commitManifest(manifest, files, evidence, prior, nullptr);
}

PdfStatus PdfPreparation::commitCheckpoint(const PdfBuildPhase phase) {
  PdfBuildCheckpoint checkpoint{};
  checkpoint.sequence = sequence_;
  checkpoint.source = sourceIdentity_;
  checkpoint.generation = generation_;
  checkpoint.phase = phase;
  checkpoint.lastVerifiedPage = pageCount_;
  checkpoint.emittedSections = sectionRecord_.size == 0 ? 0 : 1;
  checkpoint.cumulativeWords = totalWords_;
  checkpoint.outputBytes = sectionRecord_.size;
  return cacheStore_.commitCheckpoint(checkpoint);
}

PdfStepResult PdfPreparation::step() {
  if (phase_ == PdfPreparationPhase::Complete) {
    return PdfStepResult::completed();
  }
  if (phase_ == PdfPreparationPhase::Failed || phase_ == PdfPreparationPhase::Cancelled) {
    return PdfStepResult::failure(status_);
  }
  if (phase_ == PdfPreparationPhase::Idle) {
    return fail(PdfStatus::failure(PdfError::InvalidArgument));
  }
  if (cancelRequested_) {
    return cancel();
  }

  sliceStartedAtMs_ = nowMs();
  PdfWorkBudget budget{kSliceOperations, PdfLimits::SourceBufferBytes, this, stopRequested};
  PdfStatus operation = PdfStatus::success();

  switch (phase_) {
    case PdfPreparationPhase::ResourceGate:
      if (!resources_.has_value() || !resources_->canStart()) {
        return fail(PdfStatus::failure(PdfError::InsufficientMemory));
      }
      setPhase(PdfPreparationPhase::AllocateWorkspaces, 1);
      return pause();

    case PdfPreparationPhase::AllocateWorkspaces:
      if (!allocateNextWorkspace()) {
        return fail(PdfStatus::failure(PdfError::InsufficientMemory));
      }
      progressPercent_ = static_cast<uint8_t>(1 + allocationIndex_);
      if (allocationIndex_ == PDF_RESOURCE_SLOT_COUNT) {
        operation = initializeParserStorage();
        if (!operation) {
          return fail(operation);
        }
        setPhase(PdfPreparationPhase::OpenSource, 8);
      }
      return pause();

    case PdfPreparationPhase::OpenSource:
      operation = config_.io.open(config_.io.context, sourcePath_, PdfCacheOpenMode::Read, &sourceHandle_);
      if (operation) {
        operation = config_.io.metadata(config_.io.context, sourceHandle_, &sourceMetadata_);
      }
      if (!operation || sourceMetadata_.directory || sourceMetadata_.symlinkLike) {
        return fail(operation ? PdfStatus::failure(PdfError::InvalidArgument) : operation);
      }
      sourceContext_ = {&config_.io, &sourceHandle_, sourceMetadata_.size};
      sourceIdentity_.size = sourceMetadata_.size;
      sourceIdentity_.modificationTime = sourceMetadata_.modificationTime;
      setPhase(PdfPreparationPhase::FingerprintHead, 10);
      return pause();

    case PdfPreparationPhase::FingerprintHead: {
      const size_t length = static_cast<size_t>(std::min<uint64_t>(sourceMetadata_.size, PDF_SOURCE_FINGERPRINT_BYTES));
      size_t bytesRead = 0;
      if (length != 0) {
        operation = config_.io.read(config_.io.context, sourceHandle_, 0, sourceWindow_.get(), length, &bytesRead);
      }
      if (!operation || bytesRead != length) {
        return fail(operation ? PdfStatus::failure(PdfError::UnexpectedEof, bytesRead) : operation);
      }
      sourceIdentity_.headFingerprint = pdfFingerprintSourceWindow(
          PdfSourceFingerprintWindow::Head, sourceMetadata_.size, 0, sourceWindow_.get(), length);
      if (sourceMetadata_.size <= PDF_SOURCE_FINGERPRINT_BYTES) {
        sourceIdentity_.tailFingerprint = pdfFingerprintSourceWindow(
            PdfSourceFingerprintWindow::Tail, sourceMetadata_.size, 0, sourceWindow_.get(), length);
        setPhase(PdfPreparationPhase::PrepareCache, 14);
      } else {
        setPhase(PdfPreparationPhase::FingerprintTail, 12);
      }
      return pause();
    }

    case PdfPreparationPhase::FingerprintTail: {
      const uint64_t offset = sourceMetadata_.size - PDF_SOURCE_FINGERPRINT_BYTES;
      size_t bytesRead = 0;
      operation = config_.io.read(config_.io.context, sourceHandle_, offset, sourceWindow_.get(),
                                  PDF_SOURCE_FINGERPRINT_BYTES, &bytesRead);
      if (!operation || bytesRead != PDF_SOURCE_FINGERPRINT_BYTES) {
        return fail(operation ? PdfStatus::failure(PdfError::UnexpectedEof, offset + bytesRead) : operation);
      }
      sourceIdentity_.tailFingerprint =
          pdfFingerprintSourceWindow(PdfSourceFingerprintWindow::Tail, sourceMetadata_.size, offset,
                                     sourceWindow_.get(), PDF_SOURCE_FINGERPRINT_BYTES);
      setPhase(PdfPreparationPhase::PrepareCache, 14);
      return pause();
    }

    case PdfPreparationPhase::PrepareCache:
      operation = setupCache();
      if (!operation) {
        return fail(operation);
      }
      operation = startXref();
      if (!operation) {
        return fail(operation);
      }
      setPhase(PdfPreparationPhase::ParseXref, 20);
      return pause();

    case PdfPreparationPhase::ParseXref: {
      const PdfStepResult result = xrefParser_->step(budget);
      if (cancelRequested_) {
        return cancel();
      }
      if (result.failed()) {
        return fail(result.status);
      }
      if (result.yielded()) {
        return pause();
      }
      operation = finishXref();
      if (!operation) {
        return fail(operation);
      }
      setPhase(PdfPreparationPhase::ResolveCatalog, 35);
      return pause();
    }

    case PdfPreparationPhase::ResolveCatalog: {
      const PdfStepResult result = resolver_->step(budget);
      if (cancelRequested_) {
        return cancel();
      }
      if (result.failed()) {
        return fail(result.status);
      }
      if (result.yielded()) {
        return pause();
      }
      operation = finishCatalog();
      if (!operation) {
        return fail(operation);
      }
      setPhase(PdfPreparationPhase::WalkPages, 45);
      return pause();
    }

    case PdfPreparationPhase::WalkPages: {
      const PdfStepResult result = pageWalker_->step(budget);
      if (cancelRequested_) {
        return cancel();
      }
      if (result.failed()) {
        return fail(result.status);
      }
      if (result.yielded()) {
        return pause();
      }
      operation = finishPageTree();
      if (!operation) {
        return fail(operation);
      }
      setPhase(PdfPreparationPhase::ResolveContent, 55);
      return pause();
    }

    case PdfPreparationPhase::ResolveContent: {
      const PdfStepResult result = resolver_->step(budget);
      if (cancelRequested_) {
        return cancel();
      }
      if (result.failed()) {
        return fail(result.status);
      }
      if (result.yielded()) {
        return pause();
      }
      operation = finishContentObject();
      if (!operation) {
        return fail(operation);
      }
      setPhase(PdfPreparationPhase::ExtractText, 65);
      return pause();
    }

    case PdfPreparationPhase::ExtractText:
      while (budget.operationsRemaining != 0 && budget.bytesRemaining != 0 && !budget.stopRequested()) {
        PdfToken token{};
        const PdfStepResult result = contentLexer_->next(token, budget);
        if (cancelRequested_) {
          return cancel();
        }
        if (result.failed()) {
          return fail(result.status);
        }
        if (result.yielded()) {
          return pause();
        }
        if (token.kind == PdfTokenKind::End) {
          contentLexer_.reset();
          setPhase(PdfPreparationPhase::CloseSource, 78);
          return pause();
        }
        operation = appendContentToken(token);
        if (!operation) {
          return fail(operation);
        }
      }
      return pause();

    case PdfPreparationPhase::CloseSource:
      operation = closeSource();
      if (!operation) {
        return fail(operation);
      }
      setPhase(PdfPreparationPhase::OpenSection, 82);
      return pause();

    case PdfPreparationPhase::OpenSection:
      operation = openSection();
      if (!operation) {
        return fail(operation);
      }
      setPhase(PdfPreparationPhase::EmitSection, 86);
      return pause();

    case PdfPreparationPhase::EmitSection:
      operation = emitSection();
      if (!operation) {
        return fail(operation);
      }
      setPhase(PdfPreparationPhase::CloseSection, 90);
      return pause();

    case PdfPreparationPhase::CloseSection:
      operation = closeSection();
      if (!operation) {
        return fail(operation);
      }
      setPhase(PdfPreparationPhase::CommitManifest, 94);
      return pause();

    case PdfPreparationPhase::CommitManifest:
      operation = commitManifest();
      if (!operation) {
        return fail(operation);
      }
      setPhase(PdfPreparationPhase::CommitCheckpoint, 97);
      return pause();

    case PdfPreparationPhase::CommitCheckpoint:
      operation = commitCheckpoint(PdfBuildPhase::Complete);
      if (!operation) {
        return fail(operation);
      }
      setPhase(PdfPreparationPhase::Cleanup, 99);
      return pause();

    case PdfPreparationPhase::Cleanup:
      operation = cacheStore_.cleanupUnreferencedGenerations();
      if (!operation) {
        return fail(operation);
      }
      destroyParsers();
      releaseWorkspaces();
      status_ = PdfStatus::success();
      setPhase(PdfPreparationPhase::Complete, 100);
      return PdfStepResult::completed();

    case PdfPreparationPhase::Complete:
      return PdfStepResult::completed();
    case PdfPreparationPhase::Failed:
    case PdfPreparationPhase::Cancelled:
      return PdfStepResult::failure(status_);
    case PdfPreparationPhase::Idle:
    default:
      return fail(PdfStatus::failure(PdfError::InvalidArgument));
  }
}
