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
constexpr uint16_t kPreparationPageLimit = 32;
constexpr uint16_t kPreparationOutlineLimit = 32;
constexpr uint8_t kPreparationNamedDestinationLimit = 16;
constexpr uint8_t kPreparationPageLabelLimit = 16;
constexpr uint16_t kPreparationLinkLimit = 32;

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

struct PreparedOutlinePending {
  PdfObjectReference reference{};
  int16_t parentIndex = -1;
};

struct PreparedLink {
  PdfRawDestination destination{};
  uint16_t sourcePageIndex = 0;
};

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

bool asciiEqualInsensitive(const char* const value, const char* const expected, const size_t length) {
  for (size_t index = 0; index < length; ++index) {
    char left = value[index];
    char right = expected[index];
    if (left >= 'A' && left <= 'Z') {
      left = static_cast<char>(left - 'A' + 'a');
    }
    if (right >= 'A' && right <= 'Z') {
      right = static_cast<char>(right - 'A' + 'a');
    }
    if (left != right) {
      return false;
    }
  }
  return true;
}

void sourceFallbackTitle(const char* const path, const uint8_t** const title, size_t* const length) {
  const char* start = path;
  for (const char* cursor = path; *cursor != '\0'; ++cursor) {
    if (*cursor == '/' || *cursor == '\\') {
      start = cursor + 1;
    }
  }
  size_t titleLength = std::strlen(start);
  if (titleLength >= 4 && asciiEqualInsensitive(start + titleLength - 4, ".pdf", 4)) {
    titleLength -= 4;
  }
  *title = reinterpret_cast<const uint8_t*>(start);
  *length = titleLength;
}

uint32_t deterministicGeneration(const PdfSourceIdentity& identity) {
  uint32_t generation = static_cast<uint32_t>(identity.headFingerprint ^ (identity.headFingerprint >> 32U) ^
                                              identity.tailFingerprint ^ (identity.tailFingerprint >> 32U));
  if (generation == 0) {
    generation = 1;
  }
  return generation;
}

bool parseTokenInt16(const PdfToken& token, int16_t* const value) {
  if (value == nullptr || (token.kind != PdfTokenKind::Integer && token.kind != PdfTokenKind::Real) ||
      token.length == 0) {
    return false;
  }
  size_t index = 0;
  bool negative = false;
  if (token.bytes[index] == '-' || token.bytes[index] == '+') {
    negative = token.bytes[index] == '-';
    if (++index == token.length) {
      return false;
    }
  }
  int32_t parsed = 0;
  bool digit = false;
  while (index < token.length && token.bytes[index] != '.') {
    if (token.bytes[index] < '0' || token.bytes[index] > '9') {
      return false;
    }
    digit = true;
    parsed = parsed * 10 + token.bytes[index] - '0';
    if (parsed > INT16_MAX + (negative ? 1 : 0)) {
      return false;
    }
    ++index;
  }
  if (!digit) {
    return false;
  }
  *value = static_cast<int16_t>(negative ? -parsed : parsed);
  return true;
}

}  // namespace

struct PdfPreparation::ExtractedBlockRecord {
  uint16_t textOffset = 0;
  uint16_t textLength = 0;
  int16_t sourceFontSize = 0;
  uint16_t reserved = 0;
};

struct PdfPreparation::NavigationWorkspace {
  PdfPageInfo pages[kPreparationPageLimit]{};
  PdfOutlineEntry outlineEntries[kPreparationOutlineLimit]{};
  PdfNamedDestinationRecord namedDestinations[kPreparationNamedDestinationLimit]{};
  PdfPageLabelRange pageLabels[kPreparationPageLabelLimit]{};
  PreparedLink links[kPreparationLinkLimit]{};
  PreparedOutlinePending outlinePending[kPreparationOutlineLimit]{};
  PdfObjectReference outlineSeen[kPreparationOutlineLimit]{};
  PdfMetadataSection sections[kPreparationPageLimit]{};
  PdfRequiredFileRecord sectionFiles[kPreparationPageLimit]{};
  uint32_t pageFirstAnchors[kPreparationPageLimit]{};
  uint16_t pageFirstSections[kPreparationPageLimit]{};
  uint16_t linkCount = 0;
};

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
  if (metadataWriter_.open) {
    pdfAbortTrackedCacheFile(&metadataWriter_);
  }
  if (outlineWriter_.open) {
    pdfAbortTrackedCacheFile(&outlineWriter_);
  }
  (void)closeSource();
  releaseWorkspaces();
}

PdfStatus PdfPreparation::begin(const PdfPreparationConfig& config) {
  destroyParsers();
  if (sectionWriter_.open) {
    pdfAbortTrackedCacheFile(&sectionWriter_);
  }
  if (metadataWriter_.open) {
    pdfAbortTrackedCacheFile(&metadataWriter_);
  }
  if (outlineWriter_.open) {
    pdfAbortTrackedCacheFile(&outlineWriter_);
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
  const uint8_t* fallbackTitle = nullptr;
  size_t fallbackTitleLength = 0;
  sourceFallbackTitle(sourcePath_, &fallbackTitle, &fallbackTitleLength);
  status_ = metadataBuilder_.begin(fallbackTitle, fallbackTitleLength);
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
  navigation_ = nullptr;
  namedDestinations_.reset();
  pageLabels_.reset();
  outlineBuilder_.reset();
  catalogNavigation_ = {};
  infoReference_ = {};
  activeNavigationReference_ = {};
  pageCount_ = 0;
  currentPageIndex_ = 0;
  currentContentIndex_ = 0;
  extractedBlockCount_ = 0;
  currentBlockIndex_ = 0;
  sectionCount_ = 0;
  explicitOutlineCount_ = 0;
  outlinePendingCount_ = 0;
  outlineSeenCount_ = 0;
  currentOutlineParent_ = -1;
  currentAnnotationPage_ = 0;
  currentAnnotationIndex_ = 0;
  navigationStage_ = 0;
  navigationTask_ = NavigationTask::None;
  hasInfoReference_ = false;
  contentRange_ = {};
  transcriptLength_ = 0;
  currentFontSize_ = 0;
  lastNumericValue_ = 0;
  nextAnchorOrdinal_ = 0;
  currentSectionFirstWord_ = 0;
  currentSectionFirstAnchor_ = 0;
  cumulativeSectionBytes_ = 0;
  xmpStreamOffset_ = 0;
  xmpStreamLength_ = 0;
  coverCandidateSourceCount_ = 0;
  for (auto& source : coverCandidateSources_) {
    source = {};
  }
  cacheStore_ = {};
  cacheCapacity_ = {};
  cacheBudget_ = {};
  checkpointSelection_ = PdfBuildCheckpointSelection{};
  sectionWriter_ = {};
  metadataWriter_ = {};
  outlineWriter_ = {};
  sectionRecord_ = {};
  metadataRecord_ = {};
  outlineRecord_ = {};
  semanticWriter_ = {};
  metadata_ = {};
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
  if (metadataWriter_.open) {
    pdfAbortTrackedCacheFile(&metadataWriter_);
  }
  if (outlineWriter_.open) {
    pdfAbortTrackedCacheFile(&outlineWriter_);
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
  if (metadataWriter_.open) {
    pdfAbortTrackedCacheFile(&metadataWriter_);
  }
  if (outlineWriter_.open) {
    pdfAbortTrackedCacheFile(&outlineWriter_);
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
  if (self.navigation_ == nullptr || self.pageCount_ >= kPreparationPageLimit) {
    return PdfStatus::failure(PdfError::LimitExceeded, page.pageIndex);
  }
  self.navigation_->pages[self.pageCount_] = page;
  if (page.pageIndex < PdfLimits::MaxCoverScanPages && page.hasResources &&
      self.coverCandidateSourceCount_ < PdfLimits::MaxCoverCandidateSources) {
    const PdfObjectReference reference = page.resourcesIndirect ? page.resourceReference : page.resourceOwner;
    if (reference.objectNumber != 0) {
      bool alreadyRecorded = false;
      for (uint8_t index = 0; index < self.coverCandidateSourceCount_; ++index) {
        if (self.coverCandidateSources_[index].reference == reference) {
          alreadyRecorded = true;
          break;
        }
      }
      if (!alreadyRecorded) {
        self.coverCandidateSources_[self.coverCandidateSourceCount_++] = {
            reference,
            static_cast<uint16_t>(page.pageIndex),
            page.resourcesIndirect,
        };
      }
    }
  }
  ++self.pageCount_;
  return PdfStatus::success();
}

bool PdfPreparation::coverCandidateSource(const uint8_t index, PdfCoverCandidateSource* const output) const {
  if (output == nullptr || index >= coverCandidateSourceCount_) {
    return false;
  }
  *output = coverCandidateSources_[index];
  return true;
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

PdfStatus PdfPreparation::writeMetadata(void* context, const uint8_t* source, const size_t requested,
                                        size_t* bytesWritten) {
  if (context == nullptr || bytesWritten == nullptr) {
    return PdfStatus::failure(PdfError::InvalidArgument);
  }
  auto& self = *static_cast<PdfPreparation*>(context);
  const uint64_t before = self.metadataWriter_.record.size;
  const PdfStatus status = pdfWriteTrackedCacheFile(&self.metadataWriter_, source, requested);
  *bytesWritten = static_cast<size_t>(self.metadataWriter_.record.size - before);
  return status;
}

PdfStatus PdfPreparation::writeOutline(void* context, const uint8_t* source, const size_t requested,
                                       size_t* bytesWritten) {
  if (context == nullptr || bytesWritten == nullptr) {
    return PdfStatus::failure(PdfError::InvalidArgument);
  }
  auto& self = *static_cast<PdfPreparation*>(context);
  const uint64_t before = self.outlineWriter_.record.size;
  const PdfStatus status = pdfWriteTrackedCacheFile(&self.outlineWriter_, source, requested);
  *bytesWritten = static_cast<size_t>(self.outlineWriter_.record.size - before);
  return status;
}

PdfStatus PdfPreparation::emitBlock(void*, const PdfSemanticBlockRecord&) { return PdfStatus::success(); }

PdfStatus PdfPreparation::readMetadataSection(void* context, const uint16_t index, PdfMetadataSection* record) {
  if (context == nullptr || record == nullptr) {
    return PdfStatus::failure(PdfError::InvalidArgument, index);
  }
  auto& self = *static_cast<PdfPreparation*>(context);
  if (self.navigation_ == nullptr || index >= self.sectionCount_) {
    return PdfStatus::failure(PdfError::InvalidOffset, index);
  }
  *record = self.navigation_->sections[index];
  return PdfStatus::success();
}

PdfStatus PdfPreparation::readOutlineEntry(void* context, const uint16_t index, PdfOutlineEntry* record) {
  if (context == nullptr || record == nullptr) {
    return PdfStatus::failure(PdfError::InvalidArgument, index);
  }
  auto& self = *static_cast<PdfPreparation*>(context);
  if (self.navigation_ == nullptr || !self.outlineBuilder_.has_value() || index >= self.outlineBuilder_->count()) {
    return PdfStatus::failure(PdfError::InvalidOffset, index);
  }
  *record = self.navigation_->outlineEntries[index];
  return PdfStatus::success();
}

PdfStatus PdfPreparation::readRequiredFile(void* context, const uint32_t index, PdfRequiredFileRecord* record) {
  if (context == nullptr || record == nullptr) {
    return PdfStatus::failure(PdfError::InvalidArgument, index);
  }
  auto& self = *static_cast<PdfPreparation*>(context);
  if (self.navigation_ == nullptr || index >= static_cast<uint32_t>(self.sectionCount_) + 2U) {
    return PdfStatus::failure(PdfError::InvalidOffset, index);
  }
  if (index < self.sectionCount_) {
    *record = self.navigation_->sectionFiles[index];
  } else if (index == self.sectionCount_) {
    *record = self.metadataRecord_;
  } else {
    *record = self.outlineRecord_;
  }
  return PdfStatus::success();
}

void PdfPreparation::destroyParsers() {
  contentLexer_.reset();
  pageWalker_.reset();
  resolver_.reset();
  xrefParser_.reset();
  xref_.reset();
  outlineBuilder_.reset();
  pageLabels_.reset();
  namedDestinations_.reset();
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
  navigation_ = nullptr;
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
  static_assert(sizeof(NavigationWorkspace) <= PdfLimits::UzlibDictionaryBytes);
  static_assert(sizeof(ExtractedBlockRecord) <= 8);
  if (!dictionary_ || !pageText_ || !runRecords_ || !decoderOutput_) {
    return PdfStatus::failure(PdfError::InsufficientMemory);
  }
  navigation_ = new (dictionary_.get()) NavigationWorkspace{};
  for (uint32_t index = 0; index < kPreparationPageLimit; ++index) {
    navigation_->pageFirstAnchors[index] = UINT32_MAX;
    navigation_->pageFirstSections[index] = UINT16_MAX;
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
  char sectionDirectory[PDF_CACHE_PATH_CAPACITY]{};
  const int sectionDirectoryLength = std::snprintf(sectionDirectory, sizeof(sectionDirectory), "%s/gen_%lu/sections",
                                                   cacheRoot_, static_cast<unsigned long>(generation_));
  if (sectionDirectoryLength < 0 || static_cast<size_t>(sectionDirectoryLength) >= sizeof(sectionDirectory)) {
    return PdfStatus::failure(PdfError::LimitExceeded);
  }
  status = config_.io.mkdir(config_.io.context, sectionDirectory);
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

  const int relativeLength = std::snprintf(sectionRelativePath_, sizeof(sectionRelativePath_),
                                           "gen_%lu/sections/000000.xhtml", static_cast<unsigned long>(generation_));
  const int fullLength = std::snprintf(sectionPath_, sizeof(sectionPath_), "%s/%s", cacheRoot_, sectionRelativePath_);
  if (relativeLength < 0 || static_cast<size_t>(relativeLength) >= sizeof(sectionRelativePath_) || fullLength < 0 ||
      static_cast<size_t>(fullLength) >= sizeof(sectionPath_)) {
    return PdfStatus::failure(PdfError::LimitExceeded);
  }
  const int metadataRelativeLength = std::snprintf(metadataRelativePath_, sizeof(metadataRelativePath_),
                                                   "gen_%lu/metadata.bin", static_cast<unsigned long>(generation_));
  const int metadataFullLength =
      std::snprintf(metadataPath_, sizeof(metadataPath_), "%s/%s", cacheRoot_, metadataRelativePath_);
  const int outlineRelativeLength = std::snprintf(outlineRelativePath_, sizeof(outlineRelativePath_),
                                                  "gen_%lu/outline.bin", static_cast<unsigned long>(generation_));
  const int outlineFullLength =
      std::snprintf(outlinePath_, sizeof(outlinePath_), "%s/%s", cacheRoot_, outlineRelativePath_);
  if (metadataRelativeLength < 0 || static_cast<size_t>(metadataRelativeLength) >= sizeof(metadataRelativePath_) ||
      metadataFullLength < 0 || static_cast<size_t>(metadataFullLength) >= sizeof(metadataPath_) ||
      outlineRelativeLength < 0 || static_cast<size_t>(outlineRelativeLength) >= sizeof(outlineRelativePath_) ||
      outlineFullLength < 0 || static_cast<size_t>(outlineFullLength) >= sizeof(outlinePath_)) {
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
  if (!resolver_.has_value() || navigation_ == nullptr) {
    return PdfStatus::failure(PdfError::InvalidArgument);
  }
  PdfStatus status = pdfReadCatalogNavigation(arena_, resolver_->result().rootIndex, &catalogNavigation_);
  if (!status) {
    return status;
  }
  status = pdfApplyCatalogMetadata(catalogNavigation_, &metadataBuilder_);
  if (!status) {
    return status;
  }
  hasInfoReference_ = xref_.has_value() && xref_->info(&infoReference_);
  pageCount_ = 0;
  pageWalker_.emplace(*resolver_, arena_, recordStore(traversalRecords_), capturePage, this);
  return pageWalker_->begin(catalogNavigation_.pages);
}

PdfStatus PdfPreparation::finishPageTree() {
  pageWalker_.reset();
  if (!resolver_.has_value() || navigation_ == nullptr || pageCount_ == 0) {
    return PdfStatus::failure(PdfError::NoReadableText);
  }
  return beginNavigationDiscovery();
}

PdfStatus PdfPreparation::beginNavigationDiscovery() {
  if (navigation_ == nullptr || !resolver_.has_value()) {
    return PdfStatus::failure(PdfError::InvalidArgument);
  }
  namedDestinations_.emplace(PdfNamedDestinationWorkspace{
      navigation_->namedDestinations,
      static_cast<uint8_t>(std::size(navigation_->namedDestinations)),
  });
  pageLabels_.emplace(PdfPageLabelWorkspace{
      navigation_->pageLabels,
      static_cast<uint8_t>(std::size(navigation_->pageLabels)),
  });
  outlineBuilder_.emplace(
      PdfOutlineWorkspace{navigation_->outlineEntries, static_cast<uint16_t>(std::size(navigation_->outlineEntries))});
  PdfStatus status = namedDestinations_->begin();
  if (status) {
    status = pageLabels_->begin();
  }
  if (status) {
    status = outlineBuilder_->begin();
  }
  if (!status) {
    return status;
  }
  navigationStage_ = 0;
  navigationTask_ = NavigationTask::None;
  outlinePendingCount_ = 0;
  outlineSeenCount_ = 0;
  explicitOutlineCount_ = 0;
  currentAnnotationPage_ = 0;
  currentAnnotationIndex_ = 0;
  navigation_->linkCount = 0;
  return startNextNavigationObject();
}

PdfStatus PdfPreparation::startNextNavigationObject() {
  if (!resolver_.has_value() || navigation_ == nullptr) {
    return PdfStatus::failure(PdfError::InvalidArgument);
  }
  while (true) {
    switch (navigationStage_) {
      case 0:
        ++navigationStage_;
        if (hasInfoReference_) {
          navigationTask_ = NavigationTask::Info;
          activeNavigationReference_ = infoReference_;
          return resolver_->begin(activeNavigationReference_);
        }
        break;
      case 1:
        ++navigationStage_;
        if (catalogNavigation_.hasMetadata) {
          navigationTask_ = NavigationTask::Xmp;
          activeNavigationReference_ = catalogNavigation_.metadata;
          return resolver_->begin(activeNavigationReference_);
        }
        break;
      case 2:
        ++navigationStage_;
        if (catalogNavigation_.hasNamedDestinations) {
          navigationTask_ = NavigationTask::NamedDestinations;
          activeNavigationReference_ = catalogNavigation_.namedDestinations;
          return resolver_->begin(activeNavigationReference_);
        }
        break;
      case 3:
        ++navigationStage_;
        if (catalogNavigation_.hasPageLabels) {
          navigationTask_ = NavigationTask::PageLabels;
          activeNavigationReference_ = catalogNavigation_.pageLabels;
          return resolver_->begin(activeNavigationReference_);
        }
        break;
      case 4:
        ++navigationStage_;
        if (catalogNavigation_.hasOutlines) {
          navigationTask_ = NavigationTask::OutlineRoot;
          activeNavigationReference_ = catalogNavigation_.outlines;
          return resolver_->begin(activeNavigationReference_);
        }
        break;
      case 5:
        if (outlinePendingCount_ != 0) {
          const PreparedOutlinePending pending = navigation_->outlinePending[--outlinePendingCount_];
          navigationTask_ = NavigationTask::OutlineNode;
          activeNavigationReference_ = pending.reference;
          currentOutlineParent_ = pending.parentIndex;
          return resolver_->begin(activeNavigationReference_);
        }
        ++navigationStage_;
        break;
      case 6:
        while (currentAnnotationPage_ < pageCount_) {
          const PdfPageInfo& page = navigation_->pages[currentAnnotationPage_];
          if (currentAnnotationIndex_ < page.annotationCount) {
            navigationTask_ = NavigationTask::Annotation;
            activeNavigationReference_ = page.annotations[currentAnnotationIndex_++];
            return resolver_->begin(activeNavigationReference_);
          }
          ++currentAnnotationPage_;
          currentAnnotationIndex_ = 0;
        }
        ++navigationStage_;
        break;
      default:
        navigationTask_ = NavigationTask::Complete;
        return PdfStatus::success();
    }
  }
}

PdfStatus PdfPreparation::resolveDestination(const PdfRawDestination& raw,
                                             PdfResolvedDestination* const destination) const {
  if (destination == nullptr || navigation_ == nullptr) {
    return PdfStatus::failure(PdfError::InvalidArgument);
  }
  *destination = {};
  PdfRawDestination explicitDestination = raw;
  if (raw.kind == PdfRawDestinationKind::Named) {
    if (!namedDestinations_.has_value()) {
      return PdfStatus::failure(PdfError::InvalidOffset);
    }
    const PdfStatus status =
        namedDestinations_->resolve(reinterpret_cast<const uint8_t*>(raw.name), raw.nameLength, &explicitDestination);
    if (!status) {
      return status;
    }
  }
  if (explicitDestination.kind != PdfRawDestinationKind::Explicit) {
    return PdfStatus::failure(PdfError::InvalidOffset);
  }
  for (uint16_t page = 0; page < pageCount_; ++page) {
    if (navigation_->pages[page].pageReference == explicitDestination.pageReference) {
      destination->sectionIndex =
          navigation_->pageFirstSections[page] == UINT16_MAX ? page : navigation_->pageFirstSections[page];
      destination->sourcePageIndex = page;
      destination->anchorOrdinal =
          navigation_->pageFirstAnchors[page] == UINT32_MAX ? 0 : navigation_->pageFirstAnchors[page];
      destination->resolved = true;
      return PdfStatus::success();
    }
  }
  return PdfStatus::failure(PdfError::InvalidOffset, explicitDestination.pageReference.objectNumber);
}

PdfStatus PdfPreparation::finishNavigationObject() {
  if (!resolver_.has_value() || navigation_ == nullptr || !outlineBuilder_.has_value()) {
    return PdfStatus::failure(PdfError::InvalidArgument);
  }
  const PdfResolvedObject resolved = resolver_->result();
  PdfStatus status = PdfStatus::success();
  switch (navigationTask_) {
    case NavigationTask::Info:
      status = pdfApplyInfoMetadata(arena_, resolved.rootIndex, &metadataBuilder_);
      break;
    case NavigationTask::Xmp:
      if (!resolved.hasStream || resolved.streamLength > PdfLimits::DecoderOutputBytes) {
        return PdfStatus::failure(PdfError::LimitExceeded, resolved.streamLength);
      }
      xmpStreamOffset_ = resolved.streamOffset;
      xmpStreamLength_ = resolved.streamLength;
      return PdfStatus::success();
    case NavigationTask::NamedDestinations:
      status = pdfReadNamedDestinations(arena_, resolved.rootIndex, &*namedDestinations_);
      break;
    case NavigationTask::PageLabels:
      status = pdfReadPageLabels(arena_, resolved.rootIndex, &*pageLabels_);
      break;
    case NavigationTask::OutlineRoot: {
      PdfObjectReference first{};
      status = pdfReadOutlineRoot(arena_, resolved.rootIndex, &first);
      if (status) {
        navigation_->outlinePending[outlinePendingCount_++] = {first, -1};
      }
      break;
    }
    case NavigationTask::OutlineNode: {
      for (uint16_t index = 0; index < outlineSeenCount_; ++index) {
        if (navigation_->outlineSeen[index] == activeNavigationReference_) {
          return PdfStatus::failure(PdfError::Malformed, activeNavigationReference_.objectNumber);
        }
      }
      if (outlineSeenCount_ >= kPreparationOutlineLimit) {
        return PdfStatus::failure(PdfError::LimitExceeded, activeNavigationReference_.objectNumber);
      }
      navigation_->outlineSeen[outlineSeenCount_++] = activeNavigationReference_;
      PdfRawOutlineNode node{};
      status = pdfReadOutlineNode(arena_, resolved.rootIndex, &node);
      if (!status) {
        break;
      }
      PdfResolvedDestination destination{};
      const PdfStatus destinationStatus = resolveDestination(node.destination, &destination);
      int16_t childParent = currentOutlineParent_;
      if (destinationStatus) {
        const PdfOutlineCandidate candidate{
            activeNavigationReference_,
            currentOutlineParent_,
            destination,
            reinterpret_cast<const uint8_t*>(node.title),
            node.titleLength,
        };
        status = outlineBuilder_->append(candidate);
        if (!status) {
          break;
        }
        childParent = static_cast<int16_t>(outlineBuilder_->count() - 1);
        explicitOutlineCount_ = outlineBuilder_->count();
      }
      const uint16_t needed = static_cast<uint16_t>((node.hasNext ? 1 : 0) + (node.hasFirstChild ? 1 : 0));
      if (outlinePendingCount_ > kPreparationOutlineLimit - needed) {
        return PdfStatus::failure(PdfError::LimitExceeded, activeNavigationReference_.objectNumber);
      }
      if (node.hasNext) {
        navigation_->outlinePending[outlinePendingCount_++] = {node.next, currentOutlineParent_};
      }
      if (node.hasFirstChild) {
        navigation_->outlinePending[outlinePendingCount_++] = {node.firstChild, childParent};
      }
      break;
    }
    case NavigationTask::Annotation: {
      PdfRawLinkAnnotation annotation{};
      status = pdfReadLinkAnnotation(arena_, resolved.rootIndex, &annotation);
      if (!status && status.error == PdfError::Unsupported) {
        status = PdfStatus::success();
      } else if (status && annotation.action == PdfActionKind::GoTo &&
                 annotation.destination.kind != PdfRawDestinationKind::None) {
        if (navigation_->linkCount >= kPreparationLinkLimit) {
          return PdfStatus::failure(PdfError::LimitExceeded, activeNavigationReference_.objectNumber);
        }
        navigation_->links[navigation_->linkCount++] = {annotation.destination, currentAnnotationPage_};
      }
      break;
    }
    case NavigationTask::None:
    case NavigationTask::Complete:
    default:
      return PdfStatus::failure(PdfError::InvalidArgument);
  }
  if (!status) {
    return status;
  }
  navigationTask_ = NavigationTask::None;
  return startNextNavigationObject();
}

PdfStatus PdfPreparation::readXmpMetadata() {
  if (!decoderOutput_ || xmpStreamLength_ > PdfLimits::DecoderOutputBytes) {
    return PdfStatus::failure(PdfError::LimitExceeded, xmpStreamLength_);
  }
  size_t bytesRead = 0;
  PdfStatus status = source().readAt(source().context, xmpStreamOffset_, decoderOutput_.get(),
                                     static_cast<size_t>(xmpStreamLength_), &bytesRead);
  if (!status) {
    return status;
  }
  if (bytesRead != xmpStreamLength_) {
    return PdfStatus::failure(PdfError::UnexpectedEof, xmpStreamOffset_ + bytesRead);
  }
  status = pdfApplyXmpMetadata(decoderOutput_.get(), bytesRead, &metadataBuilder_);
  if (!status) {
    return status;
  }
  navigationTask_ = NavigationTask::None;
  return startNextNavigationObject();
}

PdfStatus PdfPreparation::beginCurrentPageContent() {
  if (!resolver_.has_value() || navigation_ == nullptr || currentPageIndex_ >= pageCount_) {
    return PdfStatus::failure(PdfError::InvalidArgument, currentPageIndex_);
  }
  const PdfPageInfo& page = navigation_->pages[currentPageIndex_];
  if (page.contentCount != 1) {
    return PdfStatus::failure(page.contentCount == 0 ? PdfError::NoReadableText : PdfError::Unsupported,
                              currentPageIndex_);
  }
  currentContentIndex_ = 0;
  transcriptLength_ = 0;
  extractedBlockCount_ = 0;
  currentBlockIndex_ = 0;
  currentFontSize_ = 0;
  lastNumericValue_ = 0;
  return resolver_->begin(page.contents[0]);
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
  contentLexer_.emplace(pdfByteRangeSource(contentRange_), sourceWindow_.get(), PdfLimits::SourceBufferBytes);
  return PdfStatus::success();
}

PdfStatus PdfPreparation::finishExtractedPage() {
  contentLexer_.reset();
  if (transcriptLength_ == 0 || extractedBlockCount_ == 0) {
    return PdfStatus::failure(PdfError::NoReadableText, currentPageIndex_);
  }
  return PdfStatus::success();
}

PdfStatus PdfPreparation::appendContentToken(const PdfToken& token) {
  if (token.kind == PdfTokenKind::Integer || token.kind == PdfTokenKind::Real) {
    int16_t value = 0;
    if (parseTokenInt16(token, &value)) {
      lastNumericValue_ = value;
    }
    return PdfStatus::success();
  }
  if (token.kind == PdfTokenKind::Keyword) {
    if (token.length == 2 && token.bytes[0] == 'T' && token.bytes[1] == 'f') {
      currentFontSize_ = lastNumericValue_;
    }
    return PdfStatus::success();
  }
  if (token.kind != PdfTokenKind::String && token.kind != PdfTokenKind::HexString) {
    return PdfStatus::success();
  }
  const size_t blockCapacity = PdfLimits::DecoderOutputBytes / sizeof(ExtractedBlockRecord);
  if (!pageText_ || !decoderOutput_ || token.length == 0 || token.length > UINT16_MAX ||
      extractedBlockCount_ >= blockCapacity || token.length > PdfLimits::PageTextBytes ||
      transcriptLength_ > PdfLimits::PageTextBytes - token.length) {
    return PdfStatus::failure(PdfError::LimitExceeded);
  }
  auto* const blocks = reinterpret_cast<ExtractedBlockRecord*>(decoderOutput_.get());
  blocks[extractedBlockCount_++] = {
      static_cast<uint16_t>(transcriptLength_),
      static_cast<uint16_t>(token.length),
      currentFontSize_,
      0,
  };
  std::memcpy(pageText_.get() + transcriptLength_, token.bytes, token.length);
  transcriptLength_ += token.length;
  return PdfStatus::success();
}

PdfStatus PdfPreparation::formatCurrentSectionPath() {
  const int relativeLength =
      std::snprintf(sectionRelativePath_, sizeof(sectionRelativePath_), "gen_%lu/sections/%06u.xhtml",
                    static_cast<unsigned long>(generation_), sectionCount_);
  const int fullLength = std::snprintf(sectionPath_, sizeof(sectionPath_), "%s/%s", cacheRoot_, sectionRelativePath_);
  if (relativeLength < 0 || static_cast<size_t>(relativeLength) >= sizeof(sectionRelativePath_) || fullLength < 0 ||
      static_cast<size_t>(fullLength) >= sizeof(sectionPath_)) {
    return PdfStatus::failure(PdfError::LimitExceeded, sectionCount_);
  }
  return PdfStatus::success();
}

PdfStatus PdfPreparation::openSection() {
  if (transcriptLength_ == 0 || extractedBlockCount_ == 0 || navigation_ == nullptr ||
      sectionCount_ >= kPreparationPageLimit) {
    return PdfStatus::failure(PdfError::NoReadableText);
  }
  PdfStatus status = formatCurrentSectionPath();
  if (!status) {
    return status;
  }
  const uint64_t used = cacheBudget_.requiredBytes + cacheBudget_.optionalBytes;
  const uint64_t byteLimit =
      used >= cacheBudget_.limit ? 0 : std::min<uint64_t>(cacheBudget_.limit - used, kSectionByteLimit);
  if (byteLimit == 0) {
    return PdfStatus::failure(PdfError::InsufficientStorage);
  }
  status = pdfOpenTrackedCacheWriter(config_.io, sectionPath_, sectionRelativePath_, PdfCacheFileKind::Required,
                                     byteLimit, &sectionWriter_);
  if (!status) {
    return status;
  }
  status = semanticWriter_.begin({this, writeSection}, {this, emitBlock},
                                 {operandScratch_.get(), PdfLimits::OperandOrderHistogramBytes}, totalWords_);
  if (!status) {
    pdfAbortTrackedCacheFile(&sectionWriter_);
  } else {
    if (navigation_->pageFirstSections[currentPageIndex_] == UINT16_MAX) {
      navigation_->pageFirstSections[currentPageIndex_] = sectionCount_;
    }
    currentSectionFirstWord_ = totalWords_;
    currentSectionFirstAnchor_ = nextAnchorOrdinal_;
  }
  return status;
}

PdfStatus PdfPreparation::formatInternalLink(const uint16_t sourcePageIndex, const uint8_t* const text,
                                             const size_t textLength, char* const href, const size_t capacity,
                                             size_t* const hrefLength) const {
  if (navigation_ == nullptr || text == nullptr || textLength == 0 || href == nullptr || capacity == 0 ||
      hrefLength == nullptr) {
    return PdfStatus::failure(PdfError::InvalidArgument);
  }
  href[0] = '\0';
  *hrefLength = 0;
  for (uint16_t linkIndex = 0; linkIndex < navigation_->linkCount; ++linkIndex) {
    const PreparedLink& link = navigation_->links[linkIndex];
    if (link.sourcePageIndex != sourcePageIndex) {
      continue;
    }
    PdfResolvedDestination destination{};
    if (!resolveDestination(link.destination, &destination)) {
      continue;
    }
    bool titleMatches = false;
    for (uint16_t outlineIndex = 0; outlineIndex < explicitOutlineCount_; ++outlineIndex) {
      const PdfOutlineEntry& entry = navigation_->outlineEntries[outlineIndex];
      if (entry.sourcePageIndex == destination.sourcePageIndex && entry.titleLength == textLength &&
          std::memcmp(entry.title, text, textLength) == 0) {
        titleMatches = true;
        break;
      }
    }
    if (!titleMatches) {
      continue;
    }
    if (navigation_->pageFirstAnchors[destination.sourcePageIndex] == UINT32_MAX) {
      return PdfStatus::failure(PdfError::InvalidOffset, destination.sourcePageIndex);
    }
    destination.anchorOrdinal = navigation_->pageFirstAnchors[destination.sourcePageIndex];
    return pdfResolveInternalAction(PdfActionKind::GoTo, destination, href, capacity, hrefLength);
  }
  return PdfStatus::failure(PdfError::InvalidOffset);
}

PdfStatus PdfPreparation::emitSection() {
  if (navigation_ == nullptr || !pageLabels_.has_value() || currentPageIndex_ >= pageCount_) {
    return PdfStatus::failure(PdfError::InvalidArgument, currentPageIndex_);
  }
  char pageLabel[PdfSemanticWriterLimits::PublisherLabelBytes]{};
  size_t pageLabelLength = 0;
  PdfStatus status = pageLabels_->format(currentPageIndex_, pageLabel, sizeof(pageLabel), &pageLabelLength);
  if (!status) {
    const int length =
        std::snprintf(pageLabel, sizeof(pageLabel), "%lu", static_cast<unsigned long>(currentPageIndex_ + 1U));
    if (length <= 0 || static_cast<size_t>(length) >= sizeof(pageLabel)) {
      return PdfStatus::failure(PdfError::LimitExceeded, currentPageIndex_);
    }
    pageLabelLength = static_cast<size_t>(length);
    status = PdfStatus::success();
  }
  if (currentBlockIndex_ == 0) {
    navigation_->pageFirstAnchors[currentPageIndex_] = nextAnchorOrdinal_;
    if (currentPageIndex_ + 1U < pageCount_ && navigation_->pageFirstAnchors[currentPageIndex_ + 1U] == UINT32_MAX) {
      navigation_->pageFirstAnchors[currentPageIndex_ + 1U] = nextAnchorOrdinal_ + extractedBlockCount_;
    }
    status = semanticWriter_.writePublisherPageBreak(currentPageIndex_, reinterpret_cast<const uint8_t*>(pageLabel),
                                                     pageLabelLength);
  }
  auto* const blocks = reinterpret_cast<const ExtractedBlockRecord*>(decoderOutput_.get());
  uint16_t endBlock = extractedBlockCount_;
  if (explicitOutlineCount_ == 0) {
    for (uint16_t index = static_cast<uint16_t>(currentBlockIndex_ + 1U); index < extractedBlockCount_; ++index) {
      if (blocks[index].sourceFontSize >= 18) {
        endBlock = index;
        break;
      }
    }
  }
  for (uint16_t index = currentBlockIndex_; status && index < endBlock; ++index) {
    const ExtractedBlockRecord& record = blocks[index];
    const uint8_t* const text = pageText_.get() + record.textOffset;
    const bool heading = record.sourceFontSize >= 18;
    if (heading && explicitOutlineCount_ == 0) {
      status = outlineBuilder_->appendHeading(text, record.textLength, sectionCount_, nextAnchorOrdinal_, 1);
    }
    if (status) {
      status = semanticWriter_.beginBlock({heading ? PdfSemanticBlockKind::Heading : PdfSemanticBlockKind::Paragraph,
                                           nextAnchorOrdinal_, static_cast<uint8_t>(heading ? 1 : 0)});
    }
    char href[PdfOutlineLimits::HrefBytes]{};
    size_t hrefLength = 0;
    bool linked = false;
    if (status && formatInternalLink(static_cast<uint16_t>(currentPageIndex_), text, record.textLength, href,
                                     sizeof(href), &hrefLength)) {
      status = semanticWriter_.beginInternalLink(reinterpret_cast<const uint8_t*>(href), hrefLength);
      linked = status.ok();
    }
    if (status) {
      status = semanticWriter_.writeText(text, record.textLength);
    }
    if (status && linked) {
      status = semanticWriter_.endInternalLink();
    }
    if (status) {
      status = semanticWriter_.endBlock();
    }
    if (status) {
      ++nextAnchorOrdinal_;
    }
  }
  if (status) {
    status = semanticWriter_.finish();
  }
  if (status) {
    totalWords_ = semanticWriter_.totalWords();
    currentBlockIndex_ = endBlock;
  }
  return status;
}

PdfStatus PdfPreparation::closeSection() {
  PdfStatus status = pdfCloseTrackedCacheFile(&sectionWriter_, &sectionRecord_);
  if (status) {
    status = pdfReserveCacheBytes(&cacheBudget_, sectionRecord_.size, PdfCacheFileKind::Required);
  }
  if (status) {
    if (sectionRecord_.size == 0 || sectionRecord_.size > UINT32_MAX ||
        cumulativeSectionBytes_ > UINT32_MAX - sectionRecord_.size) {
      return PdfStatus::failure(PdfError::LimitExceeded, sectionCount_);
    }
    cumulativeSectionBytes_ += sectionRecord_.size;
    navigation_->sectionFiles[sectionCount_] = sectionRecord_;
    navigation_->sections[sectionCount_] = {
        static_cast<uint32_t>(sectionRecord_.size),
        static_cast<uint32_t>(cumulativeSectionBytes_),
        currentSectionFirstWord_,
        totalWords_ - currentSectionFirstWord_,
        currentSectionFirstAnchor_,
        -1,
        0,
    };
    ++sectionCount_;
  }
  return status;
}

PdfStatus PdfPreparation::prepareNavigationRecords() {
  if (navigation_ == nullptr || !outlineBuilder_.has_value() || sectionCount_ == 0) {
    return PdfStatus::failure(PdfError::InvalidArgument);
  }
  for (uint16_t index = 0; index < explicitOutlineCount_; ++index) {
    PdfOutlineEntry& entry = navigation_->outlineEntries[index];
    if (entry.sourcePageIndex >= pageCount_ || navigation_->pageFirstAnchors[entry.sourcePageIndex] == UINT32_MAX) {
      return PdfStatus::failure(PdfError::InvalidOffset, entry.sourcePageIndex);
    }
    entry.sectionIndex = navigation_->pageFirstSections[entry.sourcePageIndex];
    entry.anchorOrdinal = navigation_->pageFirstAnchors[entry.sourcePageIndex];
    PdfStatus status = pdfFormatSemanticAnchor(entry.anchorOrdinal, entry.anchor);
    if (!status) {
      return status;
    }
  }
  metadata_ = metadataBuilder_.metadata();
  PdfStatus status = outlineBuilder_->finish(reinterpret_cast<const uint8_t*>(metadata_.title), metadata_.titleLength);
  if (!status) {
    return status;
  }
  metadata_.sectionCount = sectionCount_;
  metadata_.outlineCount = outlineBuilder_->count();
  metadata_.totalWords = totalWords_;
  for (uint16_t outlineIndex = 0; outlineIndex < outlineBuilder_->count(); ++outlineIndex) {
    const PdfOutlineEntry& entry = navigation_->outlineEntries[outlineIndex];
    if (entry.sectionIndex < sectionCount_ && navigation_->sections[entry.sectionIndex].tocIndex < 0) {
      navigation_->sections[entry.sectionIndex].tocIndex = static_cast<int16_t>(outlineIndex);
    }
  }
  return PdfStatus::success();
}

PdfStatus PdfPreparation::openMetadata() {
  const uint64_t used = cacheBudget_.requiredBytes + cacheBudget_.optionalBytes;
  const uint64_t byteLimit =
      used >= cacheBudget_.limit ? 0 : std::min<uint64_t>(cacheBudget_.limit - used, 16ULL * 1024ULL);
  if (byteLimit == 0) {
    return PdfStatus::failure(PdfError::InsufficientStorage);
  }
  return pdfOpenTrackedCacheWriter(config_.io, metadataPath_, metadataRelativePath_, PdfCacheFileKind::Required,
                                   byteLimit, &metadataWriter_);
}

PdfStatus PdfPreparation::writeMetadata() {
  return pdfEncodeMetadata(metadata_, {this, sectionCount_, readMetadataSection}, {this, writeMetadata});
}

PdfStatus PdfPreparation::closeMetadata() {
  PdfStatus status = pdfCloseTrackedCacheFile(&metadataWriter_, &metadataRecord_);
  if (status) {
    status = pdfReserveCacheBytes(&cacheBudget_, metadataRecord_.size, PdfCacheFileKind::Required);
  }
  return status;
}

PdfStatus PdfPreparation::openOutline() {
  const uint64_t used = cacheBudget_.requiredBytes + cacheBudget_.optionalBytes;
  const uint64_t byteLimit =
      used >= cacheBudget_.limit ? 0 : std::min<uint64_t>(cacheBudget_.limit - used, 64ULL * 1024ULL);
  if (byteLimit == 0) {
    return PdfStatus::failure(PdfError::InsufficientStorage);
  }
  return pdfOpenTrackedCacheWriter(config_.io, outlinePath_, outlineRelativePath_, PdfCacheFileKind::Required,
                                   byteLimit, &outlineWriter_);
}

PdfStatus PdfPreparation::writeOutline() {
  return pdfEncodeOutline({this, outlineBuilder_->count(), readOutlineEntry}, {this, writeOutline});
}

PdfStatus PdfPreparation::closeOutline() {
  PdfStatus status = pdfCloseTrackedCacheFile(&outlineWriter_, &outlineRecord_);
  if (status) {
    status = pdfReserveCacheBytes(&cacheBudget_, outlineRecord_.size, PdfCacheFileKind::Required);
  }
  return status;
}

PdfStatus PdfPreparation::commitManifest() {
  if (navigation_ == nullptr) {
    return PdfStatus::failure(PdfError::InvalidArgument);
  }
  const uint32_t requiredCount = static_cast<uint32_t>(sectionCount_) + 2U;
  PdfCacheManifest manifest{};
  manifest.sequence = sequence_;
  manifest.completed = true;
  manifest.source = sourceIdentity_;
  manifest.generation = generation_;
  manifest.totalWords = totalWords_;
  manifest.requiredFileCount = requiredCount;
  manifest.requiredFileBytes = metadataRecord_.size + outlineRecord_.size;
  manifest.requiredFileLedger = PDF_CACHE_FNV64_OFFSET;
  for (uint16_t index = 0; index < sectionCount_; ++index) {
    if (manifest.requiredFileBytes > UINT64_MAX - navigation_->sectionFiles[index].size) {
      return PdfStatus::failure(PdfError::LimitExceeded, index);
    }
    manifest.requiredFileBytes += navigation_->sectionFiles[index].size;
    manifest.requiredFileLedger =
        pdfUpdateRequiredFileLedger(manifest.requiredFileLedger, navigation_->sectionFiles[index]);
  }
  manifest.requiredFileLedger = pdfUpdateRequiredFileLedger(manifest.requiredFileLedger, metadataRecord_);
  manifest.requiredFileLedger = pdfUpdateRequiredFileLedger(manifest.requiredFileLedger, outlineRecord_);
  const PdfRequiredFileTableSource files{this, requiredCount, readRequiredFile};
  const PdfCacheCommitEvidence evidence{
      true,
      requiredCount,
      manifest.requiredFileBytes,
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
  checkpoint.emittedSections = sectionCount_;
  checkpoint.cumulativeWords = totalWords_;
  checkpoint.outputBytes = cumulativeSectionBytes_ + metadataRecord_.size + outlineRecord_.size;
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
      setPhase(PdfPreparationPhase::ResolveNavigation, 55);
      return pause();
    }

    case PdfPreparationPhase::ResolveNavigation: {
      if (navigationTask_ == NavigationTask::Complete) {
        currentPageIndex_ = 0;
        operation = beginCurrentPageContent();
        if (!operation) {
          return fail(operation);
        }
        setPhase(PdfPreparationPhase::ResolveContent, 64);
        return pause();
      }
      const NavigationTask completedTask = navigationTask_;
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
      operation = finishNavigationObject();
      if (!operation) {
        return fail(operation);
      }
      if (completedTask == NavigationTask::Xmp) {
        setPhase(PdfPreparationPhase::ReadXmpMetadata, 60);
      }
      return pause();
    }

    case PdfPreparationPhase::ReadXmpMetadata:
      operation = readXmpMetadata();
      if (!operation) {
        return fail(operation);
      }
      setPhase(PdfPreparationPhase::ResolveNavigation, 62);
      return pause();

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
      setPhase(PdfPreparationPhase::ExtractText, 70);
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
          operation = finishExtractedPage();
          if (!operation) {
            return fail(operation);
          }
          setPhase(PdfPreparationPhase::OpenSection, 78);
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
      operation = prepareNavigationRecords();
      if (!operation) {
        return fail(operation);
      }
      setPhase(PdfPreparationPhase::OpenMetadata, 91);
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
      if (currentBlockIndex_ < extractedBlockCount_) {
        setPhase(PdfPreparationPhase::OpenSection, 78);
      } else if (++currentPageIndex_ < pageCount_) {
        operation = beginCurrentPageContent();
        if (!operation) {
          return fail(operation);
        }
        setPhase(PdfPreparationPhase::ResolveContent, 70);
      } else {
        setPhase(PdfPreparationPhase::CloseSource, 90);
      }
      return pause();

    case PdfPreparationPhase::OpenMetadata:
      operation = openMetadata();
      if (!operation) {
        return fail(operation);
      }
      setPhase(PdfPreparationPhase::WriteMetadata, 92);
      return pause();

    case PdfPreparationPhase::WriteMetadata:
      operation = writeMetadata();
      if (!operation) {
        return fail(operation);
      }
      setPhase(PdfPreparationPhase::CloseMetadata, 93);
      return pause();

    case PdfPreparationPhase::CloseMetadata:
      operation = closeMetadata();
      if (!operation) {
        return fail(operation);
      }
      setPhase(PdfPreparationPhase::OpenOutline, 94);
      return pause();

    case PdfPreparationPhase::OpenOutline:
      operation = openOutline();
      if (!operation) {
        return fail(operation);
      }
      setPhase(PdfPreparationPhase::WriteOutline, 95);
      return pause();

    case PdfPreparationPhase::WriteOutline:
      operation = writeOutline();
      if (!operation) {
        return fail(operation);
      }
      setPhase(PdfPreparationPhase::CloseOutline, 96);
      return pause();

    case PdfPreparationPhase::CloseOutline:
      operation = closeOutline();
      if (!operation) {
        return fail(operation);
      }
      setPhase(PdfPreparationPhase::CommitManifest, 97);
      return pause();

    case PdfPreparationPhase::CommitManifest:
      operation = commitManifest();
      if (!operation) {
        return fail(operation);
      }
      setPhase(PdfPreparationPhase::CommitCheckpoint, 98);
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
