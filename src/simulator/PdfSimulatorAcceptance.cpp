#ifdef SIMULATOR

#include "PdfSimulatorAcceptance.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <Epub/Page.h>
#include <Epub/Section.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <Memory.h>
#include <PdfCacheStore.h>
#include <PdfHalCacheIo.h>
#include <PdfLayoutWordIndex.h>
#include <PdfPreparation.h>
#include <PdfReaderProgressState.h>
#include <PdfReflowDocument.h>
#include <Print.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "CrossPointSettings.h"
#include "PdfAcceptanceFramebufferGuard.h"
#include "activities/RenderLock.h"
#include "activities/reader/ReaderUtils.h"
#include "components/UITheme.h"

namespace {

constexpr char kCacheDirectory[] = "/.crosspoint";
constexpr char kUncachedPass[] = "uncached";
constexpr char kCachedPass[] = "cached";
constexpr char kCancelPhase[] = "cancel";
constexpr char kResumePhase[] = "resume";
constexpr char kCachedPhase[] = "cached";
constexpr char kResultMarker[] = "SIM_PDF_ACCEPTANCE_RESULT ";
constexpr char kResetMarker[] = "SIM_PDF_ACCEPTANCE_RESET ";
constexpr char kCancelResume[] = "raster_cover_caption.pdf";
constexpr char kFreshResumeBaseline[] = "raster_cover_caption_fresh.pdf";
constexpr char kFontSix[] = "font_size_6.pdf";
constexpr char kFontSeventyTwo[] = "font_size_72.pdf";
constexpr char kNavigation[] = "navigation_outline.pdf";
constexpr char kImage[] = "raster_cover_caption.pdf";
constexpr std::array<const char*, 4> kNegativeFixtures = {
    "bad_startxref.pdf",
    "encrypted.pdf",
    "flate_bomb.pdf",
    "scan_only.pdf",
};
constexpr uint64_t kFnvOffset = 14695981039346656037ULL;
constexpr uint64_t kFnvPrime = 1099511628211ULL;
constexpr uint32_t kMaximumPreparationSteps = 100000;
constexpr uint16_t kMaximumCancellationSlices = 256;
constexpr uint32_t kMaximumCancellationSliceMilliseconds = 8;
constexpr uint32_t kMaximumCancellationSliceOperations = 32;
constexpr int64_t kMaximumPreparationStepMicroseconds = 8000;
constexpr uint32_t kMaximumCancellationElapsedMilliseconds =
    kMaximumCancellationSlices * kMaximumCancellationSliceMilliseconds;
constexpr size_t kMaximumIoRequestBytes = 4U * 1024U;
constexpr size_t kExpectedFramebufferBytes = 48000;
constexpr uint16_t kCachedPageTurns = 100;
constexpr char kResumeEvidencePath[] = "fs_/.crosspoint/pdf_simulator_resume.evidence";
constexpr char kResumeEvidenceTemporaryPath[] = "fs_/.crosspoint/pdf_simulator_resume.evidence.tmp";
constexpr size_t kResumeEvidenceBytes = 192;
constexpr size_t kRunnerNonceBytes = 32;

struct AcceptanceCounters {
  uint32_t preparationSteps = 0;
  uint32_t extractionRuns = 0;
  uint32_t parserCalls = 0;
  uint32_t sourceOpenCalls = 0;
  uint32_t sourceReadCalls = 0;
  uint64_t sourceReadBytes = 0;
  size_t sourceMaxRead = 0;
  uint32_t ioCalls = 0;
  size_t maxIoRequest = 0;
  uint32_t yieldedSlices = 0;
  uint16_t cachedPageTurns = 0;
  uint32_t pageTurnSourceOpens = 0;
  uint32_t pageTurnSourceReads = 0;
  uint8_t cancelledRuns = 0;
  uint8_t resumedRuns = 0;
  uint16_t cancellationSteps = 0;
  uint32_t cancellationElapsedMs = 0;
  uint32_t cancellationMaxSliceMs = 0;
  uint32_t cancellationMaxSliceIoCalls = 0;
  uint32_t freshBaselineSteps = 0;
  uint32_t freshBaselineParserSteps = 0;
  uint32_t freshBaselinePageSteps = 0;
  uint32_t freshBaselineImageSteps = 0;
  uint32_t resumedPreparationSteps = 0;
  uint32_t resumedParserSteps = 0;
  uint32_t resumedPageSteps = 0;
  uint32_t resumedImageSteps = 0;
  uint32_t maxActualStepUs = 0;
  uint32_t framebufferGuardChecks = 0;
  uint32_t framebufferGuardFailures = 0;
  uint32_t framebufferGuardControls = 0;
  uint32_t framebufferGuardRejections = 0;
};

class InstrumentedCacheIo {
 public:
  explicit InstrumentedCacheIo(AcceptanceCounters& counters)
      : base_(pdfHalCacheIo(baseContext_)), counters_(counters) {}

  PdfCacheIo io() {
    return {
        this, open, read, write, flush, sync, close, remove, mkdir, list, capacity, metadata,
    };
  }

  static PdfStatus rename(void* context, const char* sourcePath, const char* destinationPath) {
    if (context == nullptr) {
      return PdfStatus::failure(PdfError::InvalidArgument);
    }
    auto& self = *static_cast<InstrumentedCacheIo*>(context);
    ++self.counters_.ioCalls;
    return pdfHalCacheRename(&self.baseContext_, sourcePath, destinationPath);
  }

  void trackSource(std::string path) {
    trackedSource_ = std::move(path);
    counters_.sourceOpenCalls = 0;
    counters_.sourceReadCalls = 0;
    counters_.sourceReadBytes = 0;
    counters_.sourceMaxRead = 0;
    trackedHandles_.fill(false);
  }

 private:
  static InstrumentedCacheIo& self(void* context) { return *static_cast<InstrumentedCacheIo*>(context); }

  static PdfStatus open(void* context, const char* path, const PdfCacheOpenMode mode, PdfCacheHandle* handle) {
    if (context == nullptr || path == nullptr || handle == nullptr) {
      return PdfStatus::failure(PdfError::InvalidArgument);
    }
    auto& owner = self(context);
    ++owner.counters_.ioCalls;
    const PdfStatus status = owner.base_.open(owner.base_.context, path, mode, handle);
    if (status && handle->value < owner.trackedHandles_.size()) {
      const bool tracked = mode == PdfCacheOpenMode::Read && owner.trackedSource_ == path;
      owner.trackedHandles_[handle->value] = tracked;
      if (tracked) {
        ++owner.counters_.sourceOpenCalls;
      }
    }
    return status;
  }

  static PdfStatus read(void* context, const PdfCacheHandle handle, const uint64_t offset, uint8_t* destination,
                        const size_t requested, size_t* bytesRead) {
    if (context == nullptr) {
      return PdfStatus::failure(PdfError::InvalidArgument);
    }
    auto& owner = self(context);
    ++owner.counters_.ioCalls;
    owner.counters_.maxIoRequest = std::max(owner.counters_.maxIoRequest, requested);
    const PdfStatus status = owner.base_.read(owner.base_.context, handle, offset, destination, requested, bytesRead);
    if (handle.value < owner.trackedHandles_.size() && owner.trackedHandles_[handle.value]) {
      ++owner.counters_.sourceReadCalls;
      owner.counters_.sourceMaxRead = std::max(owner.counters_.sourceMaxRead, requested);
      if (status && bytesRead != nullptr) {
        owner.counters_.sourceReadBytes += *bytesRead;
      }
    }
    return status;
  }

  static PdfStatus write(void* context, const PdfCacheHandle handle, const uint8_t* source, const size_t requested,
                         size_t* bytesWritten) {
    if (context == nullptr) {
      return PdfStatus::failure(PdfError::InvalidArgument);
    }
    auto& owner = self(context);
    ++owner.counters_.ioCalls;
    owner.counters_.maxIoRequest = std::max(owner.counters_.maxIoRequest, requested);
    return owner.base_.write(owner.base_.context, handle, source, requested, bytesWritten);
  }

  static PdfStatus flush(void* context, const PdfCacheHandle handle) {
    auto& owner = self(context);
    ++owner.counters_.ioCalls;
    return owner.base_.flush(owner.base_.context, handle);
  }

  static PdfStatus sync(void* context, const PdfCacheHandle handle) {
    auto& owner = self(context);
    ++owner.counters_.ioCalls;
    return owner.base_.sync(owner.base_.context, handle);
  }

  static PdfStatus close(void* context, PdfCacheHandle* handle) {
    auto& owner = self(context);
    ++owner.counters_.ioCalls;
    const uint8_t slot = handle == nullptr ? 0xff : handle->value;
    const PdfStatus status = owner.base_.close(owner.base_.context, handle);
    if (slot < owner.trackedHandles_.size()) {
      owner.trackedHandles_[slot] = false;
    }
    return status;
  }

  static PdfStatus remove(void* context, const char* path, const bool recursive) {
    auto& owner = self(context);
    ++owner.counters_.ioCalls;
    return owner.base_.remove(owner.base_.context, path, recursive);
  }

  static PdfStatus mkdir(void* context, const char* path) {
    auto& owner = self(context);
    ++owner.counters_.ioCalls;
    return owner.base_.mkdir(owner.base_.context, path);
  }

  static PdfStatus list(void* context, const char* path, const PdfCacheListVisitor visitor, void* visitorContext) {
    auto& owner = self(context);
    ++owner.counters_.ioCalls;
    return owner.base_.list(owner.base_.context, path, visitor, visitorContext);
  }

  static PdfStatus capacity(void* context, PdfCacheCapacity* capacityResult) {
    auto& owner = self(context);
    ++owner.counters_.ioCalls;
    return owner.base_.capacity(owner.base_.context, capacityResult);
  }

  static PdfStatus metadata(void* context, const PdfCacheHandle handle, PdfCacheFileMetadata* metadataResult) {
    auto& owner = self(context);
    ++owner.counters_.ioCalls;
    return owner.base_.metadata(owner.base_.context, handle, metadataResult);
  }

  PdfHalCacheIoContext baseContext_{};
  PdfCacheIo base_{};
  AcceptanceCounters& counters_;
  std::string trackedSource_;
  std::array<bool, PDF_HAL_CACHE_HANDLE_COUNT> trackedHandles_{};
};

struct AcceptanceClock {
  uint32_t nowMs = 0;

  static uint32_t now(void* context) {
    auto& clock = *static_cast<AcceptanceClock*>(context);
    return clock.nowMs++;
  }
};

struct Layout {
  int top = 0;
  int right = 0;
  int bottom = 0;
  int left = 0;
  uint16_t width = 0;
  uint16_t height = 0;
};

struct PreparationWorkCounters {
  uint32_t steps = 0;
  uint32_t parserSteps = 0;
  uint32_t pageSteps = 0;
  uint32_t imageSteps = 0;
};

struct ResumeEvidence {
  std::string runnerNonce;
  PdfSourceIdentity source{};
  PdfBuildCheckpoint checkpoint{};
  PreparationWorkCounters phaseWork{};
  PdfPreparationWorkCounters productWork{};
  uint32_t generation = 0;
};

struct LayoutProbe {
  uint16_t pageIndex = 0;
  uint16_t elementIndex = 0;
  uint16_t wordIndex = UINT16_MAX;
  uint8_t tag = 0;
  int16_t x = 0;
  int16_t y = 0;
  int16_t wordX = 0;
  uint8_t wordStyle = 0;
  uint8_t wordFlags = 0;
  uint16_t wordLength = 0;
  uint64_t wordHash = kFnvOffset;
};

struct FrameTriplet {
  std::string semanticHash;
  uint32_t wordCount = 0;
  std::string textHash;
  std::string firstPageTextHash;
  uint16_t pageCount = 0;
  int fontId = 0;
  uint8_t fontSize = 0;
  uint8_t fontPointSize = 0;
  uint8_t lineHeightPercent = 0;
  std::string firstFrame;
  std::string middleFrame;
  std::string lastFrame;
  std::vector<LayoutProbe> layout;
};

class StringPrint final : public Print {
 public:
  size_t write(const uint8_t byte) override {
    contents_.push_back(static_cast<char>(byte));
    return 1;
  }

  size_t write(const uint8_t* source, const size_t length) override {
    if (source == nullptr) {
      return 0;
    }
    contents_.append(reinterpret_cast<const char*>(source), length);
    return length;
  }

  const std::string& contents() const { return contents_; }

 private:
  std::string contents_;
};

uint64_t hashBytes(const uint8_t* bytes, const size_t length, uint64_t hash = kFnvOffset) {
  for (size_t index = 0; index < length; ++index) {
    hash ^= bytes[index];
    hash *= kFnvPrime;
  }
  return hash;
}

PdfAcceptanceFramebufferSnapshot preparationFramebufferSnapshot(
    GfxRenderer& renderer) {
  const uint8_t* const framebuffer = renderer.getFrameBuffer();
  const size_t framebufferBytes = renderer.getBufferSize();
  if (framebuffer == nullptr || framebufferBytes != kExpectedFramebufferBytes) {
    return {framebuffer, framebufferBytes, 0};
  }
  return {framebuffer, framebufferBytes, hashBytes(framebuffer, framebufferBytes)};
}

bool verifyPreparationFramebufferUnchanged(
    GfxRenderer& renderer,
    const PdfAcceptanceFramebufferSnapshot& expected,
    AcceptanceCounters& counters,
    std::string& error) {
  const PdfAcceptanceFramebufferSnapshot observed =
      preparationFramebufferSnapshot(renderer);
  const bool unchanged =
      pdfAcceptanceObserveFramebuffer(expected, observed,
                                      counters.framebufferGuardChecks,
                                      counters.framebufferGuardFailures);

  const uint8_t differentPointerSentinel = 0;
  const PdfAcceptanceFramebufferSnapshot changedHash{
      expected.pointer, expected.bytes, expected.hash ^ 1U};
  const PdfAcceptanceFramebufferSnapshot changedPointer{
      &differentPointerSentinel, expected.bytes, expected.hash};
  const bool changedHashRejected =
      !pdfAcceptanceObserveFramebuffer(expected, changedHash,
                                       counters.framebufferGuardControls,
                                       counters.framebufferGuardRejections);
  const bool changedPointerRejected =
      !pdfAcceptanceObserveFramebuffer(expected, changedPointer,
                                       counters.framebufferGuardControls,
                                       counters.framebufferGuardRejections);

  if (!unchanged || !changedHashRejected || !changedPointerRejected) {
    error = unchanged
                ? "PDF framebuffer guard accepted a changed hash or pointer"
                : "PDF preparation changed or replaced the 48 KiB framebuffer";
  }
  return unchanged && changedHashRejected && changedPointerRejected;
}

uint64_t hashString(const std::string& text, uint64_t hash = kFnvOffset) {
  return hashBytes(reinterpret_cast<const uint8_t*>(text.data()), text.size(), hash);
}

std::string hashHex(const uint64_t hash) {
  char output[17]{};
  std::snprintf(output, sizeof(output), "%016llX", static_cast<unsigned long long>(hash));
  return output;
}

bool isAsciiWhitespace(const char value) {
  return value == ' ' || value == '\t' || value == '\r' || value == '\n' || value == '\f';
}

std::string normalizeSemanticText(const std::string& xhtml) {
  std::string normalized;
  normalized.reserve(xhtml.size());
  bool insideTag = false;
  bool pendingSpace = false;
  for (const char value : xhtml) {
    if (value == '<') {
      insideTag = true;
      pendingSpace = !normalized.empty();
      continue;
    }
    if (insideTag) {
      if (value == '>') {
        insideTag = false;
      }
      continue;
    }
    if (isAsciiWhitespace(value)) {
      pendingSpace = !normalized.empty();
      continue;
    }
    if (pendingSpace) {
      normalized.push_back(' ');
      pendingSpace = false;
    }
    normalized.push_back(value);
  }
  return normalized;
}

bool layoutProbeEqual(const LayoutProbe& left, const LayoutProbe& right) {
  return left.pageIndex == right.pageIndex && left.elementIndex == right.elementIndex &&
         left.wordIndex == right.wordIndex && left.tag == right.tag && left.x == right.x && left.y == right.y &&
         left.wordX == right.wordX && left.wordStyle == right.wordStyle && left.wordFlags == right.wordFlags &&
         left.wordLength == right.wordLength && left.wordHash == right.wordHash;
}

std::string describeLayoutProbe(const LayoutProbe* probe) {
  if (probe == nullptr) {
    return "missing";
  }
  char output[224]{};
  std::snprintf(output, sizeof(output),
                "page=%u,element=%u,tag=%u,x=%d,y=%d,word=%u,word_x=%d,style=%u,flags=%u,text_len=%u,"
                "text_hash=%016llX",
                probe->pageIndex, probe->elementIndex, probe->tag, probe->x, probe->y, probe->wordIndex, probe->wordX,
                probe->wordStyle, probe->wordFlags, probe->wordLength,
                static_cast<unsigned long long>(probe->wordHash));
  return output;
}

std::string describeFirstLayoutDifference(const FrameTriplet& left, const FrameTriplet& right) {
  const size_t common = std::min(left.layout.size(), right.layout.size());
  size_t index = 0;
  while (index < common && layoutProbeEqual(left.layout[index], right.layout[index])) {
    ++index;
  }
  if (index == common && left.layout.size() == right.layout.size()) {
    return "none";
  }
  const LayoutProbe* leftProbe = index < left.layout.size() ? &left.layout[index] : nullptr;
  const LayoutProbe* rightProbe = index < right.layout.size() ? &right.layout[index] : nullptr;
  return "index=" + std::to_string(index) + ",pdf_6={" + describeLayoutProbe(leftProbe) + "},pdf_72={" +
         describeLayoutProbe(rightProbe) + "}";
}

size_t countSubstring(const std::string& text, const char* needle) {
  size_t count = 0;
  size_t cursor = 0;
  while ((cursor = text.find(needle, cursor)) != std::string::npos) {
    ++count;
    cursor += std::strlen(needle);
  }
  return count;
}

std::string fixturePath(const std::string& root, const char* name) { return root + "/" + name; }

void putU16(uint8_t* const destination, const uint16_t value) {
  destination[0] = static_cast<uint8_t>(value);
  destination[1] = static_cast<uint8_t>(value >> 8U);
}

void putU32(uint8_t* const destination, const uint32_t value) {
  for (uint8_t byte = 0; byte < 4; ++byte) {
    destination[byte] = static_cast<uint8_t>(value >> (byte * 8U));
  }
}

void putU64(uint8_t* const destination, const uint64_t value) {
  for (uint8_t byte = 0; byte < 8; ++byte) {
    destination[byte] = static_cast<uint8_t>(value >> (byte * 8U));
  }
}

uint16_t getU16(const uint8_t* const source) {
  return static_cast<uint16_t>(source[0]) | static_cast<uint16_t>(source[1] << 8U);
}

uint32_t getU32(const uint8_t* const source) {
  uint32_t value = 0;
  for (uint8_t byte = 0; byte < 4; ++byte) {
    value |= static_cast<uint32_t>(source[byte]) << (byte * 8U);
  }
  return value;
}

uint64_t getU64(const uint8_t* const source) {
  uint64_t value = 0;
  for (uint8_t byte = 0; byte < 8; ++byte) {
    value |= static_cast<uint64_t>(source[byte]) << (byte * 8U);
  }
  return value;
}

bool runnerNonceValid(const char* const nonce) {
  if (nonce == nullptr || std::strlen(nonce) != kRunnerNonceBytes) {
    return false;
  }
  for (size_t index = 0; index < kRunnerNonceBytes; ++index) {
    const char value = nonce[index];
    if (!((value >= '0' && value <= '9') || (value >= 'a' && value <= 'f'))) {
      return false;
    }
  }
  return true;
}

bool checkpointEvidenceEqual(const PdfBuildCheckpoint& left, const PdfBuildCheckpoint& right) {
  return left.sequence == right.sequence && pdfSourceIdentityEqual(left.source, right.source) &&
         left.generation == right.generation && left.phase == right.phase && left.resumePhase == right.resumePhase &&
         left.lastVerifiedPage == right.lastVerifiedPage && left.lastVerifiedObject == right.lastVerifiedObject &&
         left.emittedSections == right.emittedSections && left.emittedImages == right.emittedImages &&
         left.cumulativeWords == right.cumulativeWords && left.outputBytes == right.outputBytes &&
         left.warningFlags == right.warningFlags;
}

bool encodeResumeEvidence(const ResumeEvidence& evidence, uint8_t output[kResumeEvidenceBytes]) {
  if (evidence.runnerNonce.size() != kRunnerNonceBytes ||
      evidence.checkpoint.phase != PdfBuildPhase::Cancelled ||
      evidence.checkpoint.resumePhase != PdfBuildResumePhase::AfterImageRepair ||
      evidence.generation == 0 || evidence.generation != evidence.checkpoint.generation ||
      !pdfSourceIdentityEqual(evidence.source, evidence.checkpoint.source)) {
    return false;
  }
  std::memset(output, 0, kResumeEvidenceBytes);
  std::memcpy(output, "PSR1", 4);
  putU16(output + 4, 1);
  putU16(output + 6, kResumeEvidenceBytes);
  output[8] = static_cast<uint8_t>(evidence.runnerNonce.size());
  std::memcpy(output + 9, evidence.runnerNonce.data(), evidence.runnerNonce.size());
  output[41] = evidence.source.modificationTime.known ? 1U : 0U;
  output[42] = static_cast<uint8_t>(evidence.checkpoint.resumePhase);
  output[43] = static_cast<uint8_t>(evidence.checkpoint.phase);
  putU64(output + 48, evidence.source.size);
  putU64(output + 56, evidence.source.modificationTime.value);
  putU64(output + 64, evidence.source.headFingerprint);
  putU64(output + 72, evidence.source.tailFingerprint);
  putU32(output + 80, evidence.generation);
  putU32(output + 84, evidence.checkpoint.sequence);
  putU32(output + 88, evidence.checkpoint.lastVerifiedPage);
  putU32(output + 92, evidence.checkpoint.lastVerifiedObject);
  putU32(output + 96, evidence.checkpoint.emittedSections);
  putU32(output + 100, evidence.checkpoint.emittedImages);
  putU32(output + 104, evidence.checkpoint.cumulativeWords);
  putU32(output + 108, evidence.checkpoint.warningFlags);
  putU64(output + 112, evidence.checkpoint.outputBytes);
  putU32(output + 120, evidence.phaseWork.steps);
  putU32(output + 124, evidence.phaseWork.parserSteps);
  putU32(output + 128, evidence.phaseWork.pageSteps);
  putU32(output + 132, evidence.phaseWork.imageSteps);
  putU32(output + 136, evidence.productWork.xrefSteps);
  putU32(output + 140, evidence.productWork.pagesWalked);
  putU32(output + 144, evidence.productWork.contentTokens);
  putU32(output + 148, evidence.productWork.sectionsEmitted);
  putU32(output + 152, evidence.productWork.imagesEmitted);
  putU64(output + 160, evidence.productWork.sourceBytesRead);
  putU32(output + 188, pdfCacheCrc32(output, 188));
  return true;
}

bool decodeResumeEvidence(const uint8_t input[kResumeEvidenceBytes], const char* const expectedNonce,
                          ResumeEvidence& evidence) {
  if (std::memcmp(input, "PSR1", 4) != 0 || getU16(input + 4) != 1 ||
      getU16(input + 6) != kResumeEvidenceBytes || input[8] != kRunnerNonceBytes ||
      getU32(input + 188) != pdfCacheCrc32(input, 188) ||
      std::any_of(input + 168, input + 188, [](const uint8_t value) { return value != 0; })) {
    return false;
  }
  evidence = {};
  evidence.runnerNonce.assign(reinterpret_cast<const char*>(input + 9), kRunnerNonceBytes);
  if (evidence.runnerNonce != expectedNonce) {
    return false;
  }
  evidence.source.size = getU64(input + 48);
  evidence.source.modificationTime.known = input[41] != 0;
  evidence.source.modificationTime.value = getU64(input + 56);
  evidence.source.headFingerprint = getU64(input + 64);
  evidence.source.tailFingerprint = getU64(input + 72);
  evidence.generation = getU32(input + 80);
  evidence.checkpoint.sequence = getU32(input + 84);
  evidence.checkpoint.source = evidence.source;
  evidence.checkpoint.generation = evidence.generation;
  evidence.checkpoint.phase = static_cast<PdfBuildPhase>(input[43]);
  evidence.checkpoint.resumePhase = static_cast<PdfBuildResumePhase>(input[42]);
  evidence.checkpoint.lastVerifiedPage = getU32(input + 88);
  evidence.checkpoint.lastVerifiedObject = getU32(input + 92);
  evidence.checkpoint.emittedSections = getU32(input + 96);
  evidence.checkpoint.emittedImages = getU32(input + 100);
  evidence.checkpoint.cumulativeWords = getU32(input + 104);
  evidence.checkpoint.warningFlags = getU32(input + 108);
  evidence.checkpoint.outputBytes = getU64(input + 112);
  evidence.phaseWork.steps = getU32(input + 120);
  evidence.phaseWork.parserSteps = getU32(input + 124);
  evidence.phaseWork.pageSteps = getU32(input + 128);
  evidence.phaseWork.imageSteps = getU32(input + 132);
  evidence.productWork.xrefSteps = getU32(input + 136);
  evidence.productWork.pagesWalked = getU32(input + 140);
  evidence.productWork.contentTokens = getU32(input + 144);
  evidence.productWork.sectionsEmitted = getU32(input + 148);
  evidence.productWork.imagesEmitted = getU32(input + 152);
  evidence.productWork.sourceBytesRead = getU64(input + 160);
  uint8_t canonical[kResumeEvidenceBytes]{};
  return encodeResumeEvidence(evidence, canonical) && std::memcmp(input, canonical, sizeof(canonical)) == 0;
}

bool writeResumeEvidence(const ResumeEvidence& evidence, std::string& error) {
  uint8_t encoded[kResumeEvidenceBytes]{};
  if (!encodeResumeEvidence(evidence, encoded)) {
    error = "cannot encode cross-process resume evidence";
    return false;
  }
  std::FILE* output = std::fopen(kResumeEvidenceTemporaryPath, "wb");
  if (output == nullptr) {
    error = "cannot create cross-process resume evidence";
    return false;
  }
  const bool written = std::fwrite(encoded, 1, sizeof(encoded), output) == sizeof(encoded);
  const bool flushed = written && std::fflush(output) == 0;
  const bool closed = std::fclose(output) == 0;
  if (!written || !flushed || !closed || std::rename(kResumeEvidenceTemporaryPath, kResumeEvidencePath) != 0) {
    (void)std::remove(kResumeEvidenceTemporaryPath);
    error = "cannot publish cross-process resume evidence";
    return false;
  }
  return true;
}

bool readResumeEvidence(const char* const expectedNonce, ResumeEvidence& evidence, std::string& error) {
  uint8_t encoded[kResumeEvidenceBytes]{};
  std::FILE* input = std::fopen(kResumeEvidencePath, "rb");
  if (input == nullptr) {
    error = "cross-process resume evidence is missing";
    return false;
  }
  const bool read = std::fread(encoded, 1, sizeof(encoded), input) == sizeof(encoded);
  const bool exactLength = read && std::fgetc(input) == EOF;
  const bool closed = std::fclose(input) == 0;
  if (!read || !exactLength || !closed || !decodeResumeEvidence(encoded, expectedNonce, evidence)) {
    error = "cross-process resume evidence is malformed or stale";
    return false;
  }
  return true;
}

PdfResourceSnapshot measureResources(void*) {
  return {
      ESP.getFreeHeap(),
      ESP.getMaxAllocHeap(),
      16U * 1024U,
  };
}

void ignoreResourceEvent(void*, const PdfResourceEvent&) {}

void pinReaderSettings(GfxRenderer& renderer) {
  RenderLock lock;
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

void applyReaderLayout(GfxRenderer& renderer, const uint8_t orientation, const uint8_t fontSize, const uint8_t margin) {
  RenderLock lock;
  SETTINGS.orientation = orientation;
  SETTINGS.fontSize = fontSize;
  SETTINGS.screenMargin = margin;
  UITheme::getInstance().reload();
  ReaderUtils::applyOrientation(renderer, SETTINGS.orientation);
}

bool computeLayout(GfxRenderer& renderer, Layout& layout, std::string& error) {
  renderer.getOrientedViewableTRBL(&layout.top, &layout.right, &layout.bottom, &layout.left);
  layout.top += SETTINGS.screenMargin;
  layout.right += SETTINGS.screenMargin;
  layout.bottom += std::max<int>(SETTINGS.screenMargin, ReaderUtils::STATUS_BAR_TEXT_PADDING);
  layout.left += SETTINGS.screenMargin;
  const int width = renderer.getScreenWidth() - layout.left - layout.right;
  const int height = renderer.getScreenHeight() - layout.top - layout.bottom;
  if (width <= 0 || height <= 0 || width > UINT16_MAX || height > UINT16_MAX) {
    error = "reader viewport is outside uint16 bounds";
    return false;
  }
  layout.width = static_cast<uint16_t>(width);
  layout.height = static_cast<uint16_t>(height);
  return true;
}

void accountPreparationWork(const PdfPreparationPhase phase, PreparationWorkCounters& work) {
  ++work.steps;
  switch (phase) {
    case PdfPreparationPhase::ParseXref:
    case PdfPreparationPhase::ResolveCatalog:
    case PdfPreparationPhase::ResolveNavigation:
    case PdfPreparationPhase::ReadXmpMetadata:
      ++work.parserSteps;
      return;
    case PdfPreparationPhase::WalkPages:
    case PdfPreparationPhase::ResolveContent:
    case PdfPreparationPhase::ExtractText:
    case PdfPreparationPhase::OpenSection:
    case PdfPreparationPhase::EmitSection:
    case PdfPreparationPhase::CloseSection:
      ++work.pageSteps;
      return;
    case PdfPreparationPhase::ResolveImageResources:
    case PdfPreparationPhase::CacheImage:
    case PdfPreparationPhase::SpoolNavigation:
    case PdfPreparationPhase::DecodeImages:
    case PdfPreparationPhase::RestoreNavigation:
    case PdfPreparationPhase::RepairImageSections:
      ++work.imageSteps;
      return;
    default:
      return;
  }
}

bool measuredPreparationStep(PdfPreparation& preparation, AcceptanceClock& clock, AcceptanceCounters& counters,
                             PdfStepResult& result, std::string& error,
                             PreparationWorkCounters* const work = nullptr) {
  if (work != nullptr) {
    accountPreparationWork(preparation.phase(), *work);
  }
  const uint32_t syntheticStartedAtMs = clock.nowMs;
  const auto actualStartedAt = std::chrono::steady_clock::now();
  result = preparation.step();
  const int64_t actualElapsedUs =
      std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - actualStartedAt)
          .count();
  ++counters.preparationSteps;
  if (clock.nowMs <= syntheticStartedAtMs) {
    error = "deterministic acceptance clock did not advance";
    return false;
  }
  if (actualElapsedUs < 0 || actualElapsedUs > kMaximumPreparationStepMicroseconds) {
    error = "preparation step exceeded 8 ms actual wall time";
    return false;
  }
  counters.maxActualStepUs = std::max(counters.maxActualStepUs, static_cast<uint32_t>(actualElapsedUs));
  if (result.yielded()) {
    ++counters.yieldedSlices;
  }
  return true;
}

PdfStepResult runPreparation(PdfPreparation& preparation, AcceptanceClock& clock, AcceptanceCounters& counters,
                             std::string& error, PreparationWorkCounters* const work = nullptr,
                             const uint32_t maximumSteps = kMaximumPreparationSteps) {
  for (uint32_t step = 0; step < maximumSteps; ++step) {
    PdfStepResult result{};
    if (!measuredPreparationStep(preparation, clock, counters, result, error, work)) {
      return PdfStepResult::failure(PdfStatus::failure(PdfError::BudgetExhausted));
    }
    if (!result.yielded()) {
      return result;
    }
  }
  return PdfStepResult::failure(PdfStatus::failure(PdfError::BudgetExhausted));
}

PdfPreparationConfig preparationConfig(InstrumentedCacheIo& io, AcceptanceClock& clock, const char* sourcePath,
                                       GfxRenderer& renderer) {
  return {
      io.io(),
      sourcePath,
      kCacheDirectory,
      &clock,
      AcceptanceClock::now,
      {nullptr, measureResources, ignoreResourceEvent},
      InstrumentedCacheIo::rename,
      static_cast<uint16_t>(renderer.getScreenWidth()),
      static_cast<uint16_t>(renderer.getScreenHeight()),
  };
}

bool prepareValid(const std::string& path, InstrumentedCacheIo& io, AcceptanceClock& clock,
                   AcceptanceCounters& counters, GfxRenderer& renderer,
                   PreparationWorkCounters* const completedWork,
                   PdfPreparationWorkCounters* const completedProductWork,
                   const ResumeEvidence* const expectedResume, std::string& error) {
  const PdfAcceptanceFramebufferSnapshot framebufferBefore =
      preparationFramebufferSnapshot(renderer);
  auto preparation = makeUniqueNoThrow<PdfPreparation>();
  if (!preparation) {
    error = "cannot allocate PDF preparation";
    return false;
  }
  PdfStatus status = preparation->begin(preparationConfig(io, clock, path.c_str(), renderer));
  ++counters.extractionRuns;
  if (!status) {
    error = "cannot begin PDF preparation";
    return false;
  }
  if (expectedResume != nullptr) {
    PdfCacheStore cacheStore;
    PdfBuildCheckpointSelection resumeSelection;
    status = cacheStore.initialize(io.io(), preparation->cacheRoot());
    if (status) {
      status = cacheStore.loadCheckpointSlots(expectedResume->source, &resumeSelection);
    }
    if (!status || !resumeSelection.selected ||
        !checkpointEvidenceEqual(resumeSelection.checkpoint, expectedResume->checkpoint)) {
      error = "durable resume checkpoint does not match cancellation evidence";
      return false;
    }
  }
  const PdfStepResult result = runPreparation(*preparation, clock, counters, error, completedWork);
  if (!result.complete()) {
    error = "valid PDF did not finish on-device preparation";
    return false;
  }
  if (completedProductWork != nullptr) {
    *completedProductWork = preparation->workCounters();
  }
  if (expectedResume != nullptr) {
    if (!preparation->resumedFromCheckpoint() ||
        preparation->resumedPhase() != PdfBuildResumePhase::AfterImageRepair ||
        preparation->generation() != expectedResume->generation ||
        !pdfSourceIdentityEqual(preparation->sourceIdentity(), expectedResume->source)) {
      error = "cancelled PDF did not resume from its checkpoint";
      return false;
    }
    ++counters.resumedRuns;
  }
  return verifyPreparationFramebufferUnchanged(renderer, framebufferBefore,
                                                counters, error);
}

bool prepareCancellationCheckpoint(const std::string& path, InstrumentedCacheIo& io, AcceptanceClock& clock,
                                   AcceptanceCounters& counters, GfxRenderer& renderer, const char* const runnerNonce,
                                   ResumeEvidence& evidence, std::string& error) {
  const PdfAcceptanceFramebufferSnapshot framebufferBefore =
      preparationFramebufferSnapshot(renderer);
  (void)std::remove(kResumeEvidenceTemporaryPath);
  (void)std::remove(kResumeEvidencePath);
  auto preparation = makeUniqueNoThrow<PdfPreparation>();
  if (!preparation) {
    error = "cannot allocate cancellation preparation";
    return false;
  }
  PdfStatus status = preparation->begin(preparationConfig(io, clock, path.c_str(), renderer));
  ++counters.extractionRuns;
  if (!status) {
    error = "cannot begin cancellation preparation";
    return false;
  }

  PreparationWorkCounters completedWork;
  bool checkpointBoundaryReached = false;
  for (uint32_t step = 0; step < kMaximumPreparationSteps; ++step) {
    const PdfPreparationWorkCounters productWork = preparation->workCounters();
    if (preparation->phase() == PdfPreparationPhase::CloseSource && productWork.sectionsEmitted != 0 &&
        productWork.imagesEmitted != 0) {
      checkpointBoundaryReached = true;
      break;
    }
    PdfStepResult result{};
    if (!measuredPreparationStep(*preparation, clock, counters, result, error, &completedWork)) {
      return false;
    }
    if (!result.yielded()) {
      error = "cancellation preparation finished before AfterImageRepair";
      return false;
    }
  }
  if (!checkpointBoundaryReached) {
    error = "cancellation preparation did not reach AfterImageRepair";
    return false;
  }

  const PdfSourceIdentity source = preparation->sourceIdentity();
  const uint32_t generation = preparation->generation();
  const PdfPreparationWorkCounters productWork = preparation->workCounters();
  preparation->requestCancel();
  bool cancellationReachedTerminal = false;
  for (uint16_t cancellationSlice = 0; cancellationSlice < kMaximumCancellationSlices; ++cancellationSlice) {
    const uint32_t sliceStartedAtMs = clock.nowMs;
    const uint32_t sliceStartedAtIoCalls = counters.ioCalls;
    PdfStepResult cancelledResult{};
    if (!measuredPreparationStep(*preparation, clock, counters, cancelledResult, error)) {
      return false;
    }
    ++counters.cancellationSteps;
    const uint32_t cancellationElapsedMs = clock.nowMs - sliceStartedAtMs;
    const uint32_t cancellationIoCalls = counters.ioCalls - sliceStartedAtIoCalls;
    counters.cancellationElapsedMs += cancellationElapsedMs;
    counters.cancellationMaxSliceMs = std::max(counters.cancellationMaxSliceMs, cancellationElapsedMs);
    counters.cancellationMaxSliceIoCalls = std::max(counters.cancellationMaxSliceIoCalls, cancellationIoCalls);
    if (cancellationElapsedMs > kMaximumCancellationSliceMilliseconds) {
      error = "preparation cancellation exceeded the 8 ms slice";
      return false;
    }
    if (cancellationIoCalls > kMaximumCancellationSliceOperations) {
      error = "preparation cancellation exceeded the 32-operation slice";
      return false;
    }
    if (counters.maxIoRequest > kMaximumIoRequestBytes) {
      error = "preparation cancellation exceeded the 4 KiB I/O slice";
      return false;
    }
    if (counters.cancellationElapsedMs > kMaximumCancellationElapsedMilliseconds) {
      error = "preparation cancellation exceeded total elapsed work";
      return false;
    }
    if (preparation->generation() != generation || !pdfSourceIdentityEqual(preparation->sourceIdentity(), source)) {
      error = "preparation cancellation changed source or generation";
      return false;
    }
    if (cancelledResult.yielded()) {
      continue;
    }
    if (!cancelledResult.failed() || cancelledResult.status.error != PdfError::Cancelled) {
      error = "preparation cancellation did not reach terminal Cancelled";
      return false;
    }
    cancellationReachedTerminal = true;
    break;
  }
  if (!cancellationReachedTerminal) {
    error = "preparation cancellation exhausted bounded slices";
    return false;
  }
  if (counters.cancellationSteps <= 1 || generation == 0 ||
      preparation->phase() != PdfPreparationPhase::Cancelled ||
      preparation->status().error != PdfError::Cancelled ||
      preparation->durableResumePhase() != PdfBuildResumePhase::AfterImageRepair) {
    error = "preparation cancellation did not publish durable AfterImageRepair state";
    return false;
  }

  PdfCacheStore cacheStore;
  PdfBuildCheckpointSelection checkpointSelection;
  status = cacheStore.initialize(io.io(), preparation->cacheRoot());
  if (status) {
    status = cacheStore.loadCheckpointSlots(source, &checkpointSelection);
  }
  if (!status || !checkpointSelection.selected ||
      checkpointSelection.checkpoint.phase != PdfBuildPhase::Cancelled ||
      checkpointSelection.checkpoint.resumePhase != PdfBuildResumePhase::AfterImageRepair ||
      checkpointSelection.checkpoint.generation != generation ||
      !pdfSourceIdentityEqual(checkpointSelection.checkpoint.source, source)) {
    error = "cannot decode durable AfterImageRepair checkpoint";
    return false;
  }

  evidence.runnerNonce = runnerNonce;
  evidence.source = source;
  evidence.generation = generation;
  evidence.checkpoint = checkpointSelection.checkpoint;
  evidence.phaseWork = completedWork;
  evidence.productWork = productWork;
  if (!writeResumeEvidence(evidence, error)) {
    return false;
  }
  ++counters.cancelledRuns;
  return verifyPreparationFramebufferUnchanged(renderer, framebufferBefore,
                                                counters, error);
}

bool rejectDuringPreparation(const std::string& path, InstrumentedCacheIo& io, AcceptanceClock& clock,
                             AcceptanceCounters& counters, GfxRenderer& renderer, std::string& error) {
  const PdfAcceptanceFramebufferSnapshot framebufferBefore =
      preparationFramebufferSnapshot(renderer);
  auto preparation = makeUniqueNoThrow<PdfPreparation>();
  if (!preparation) {
    error = "cannot allocate negative PDF preparation";
    return false;
  }
  PdfStatus status = preparation->begin(preparationConfig(io, clock, path.c_str(), renderer));
  ++counters.extractionRuns;
  bool rejected = false;
  if (!status) {
    rejected = status.error != PdfError::IoFailure &&
               status.error != PdfError::InsufficientMemory;
  } else {
    const PdfStepResult result = runPreparation(*preparation, clock, counters, error);
    rejected = result.failed() && result.status.error != PdfError::BudgetExhausted &&
               result.status.error != PdfError::IoFailure &&
               result.status.error != PdfError::InsufficientMemory;
  }
  if (!rejected) {
    error = "unsafe PDF did not fail closed during preparation";
    return false;
  }
  return verifyPreparationFramebufferUnchanged(renderer, framebufferBefore,
                                                counters, error);
}

std::shared_ptr<PdfReflowDocument> loadDocument(const PdfCacheIo& io, const std::string& path, std::string& error) {
  auto document = std::make_shared<PdfReflowDocument>();
  PdfStatus status = document->initialize(io, path.c_str(), kCacheDirectory);
  if (status) {
    status = document->loadCompletedCache();
  }
  if (!status) {
    error = "cannot reopen completed PDF cache";
    return nullptr;
  }
  return document;
}

bool rejectMissingCompletedCache(const PdfCacheIo& io, const std::string& path) {
  auto document = std::make_shared<PdfReflowDocument>();
  const PdfStatus initialized = document->initialize(io, path.c_str(), kCacheDirectory);
  return !initialized || !document->loadCompletedCache();
}

bool loadOrCreateSection(const std::shared_ptr<ReflowDocument>& document, GfxRenderer& renderer, const Layout& layout,
                         const int sectionIndex, const char* suffix, const bool cachedPass,
                         AcceptanceCounters& counters, std::unique_ptr<Section>& output, std::string& error) {
  output = makeUniqueNoThrow<Section>(document, sectionIndex, renderer, suffix);
  if (!output) {
    error = "cannot allocate shared reflow section";
    return false;
  }
  const int fontId = SETTINGS.getReaderFontId();
  const bool loaded = output->loadSectionFile(
      fontId, SETTINGS.getReaderLineCompression(), SETTINGS.extraParagraphSpacing, SETTINGS.forceParagraphIndents,
      SETTINGS.paragraphAlignment, layout.width, layout.height, SETTINGS.hyphenationEnabled, false,
      SETTINGS.imageRendering, SETTINGS.bionicReadingEnabled, SETTINGS.guideReadingEnabled, EpubRenderMode::Light);
  if (loaded) {
    return output->pageCount > 0;
  }
  if (cachedPass) {
    error = "cached pass attempted XHTML pagination";
    return false;
  }
  ++counters.parserCalls;
  bool imagesSuppressed = false;
  bool lowMemory = false;
  const bool created = output->createSectionFile(
      fontId, SETTINGS.getReaderLineCompression(), SETTINGS.extraParagraphSpacing, SETTINGS.forceParagraphIndents,
      SETTINGS.paragraphAlignment, layout.width, layout.height, SETTINGS.hyphenationEnabled, false,
      SETTINGS.imageRendering, SETTINGS.bionicReadingEnabled, SETTINGS.guideReadingEnabled, nullptr, &imagesSuppressed,
      &lowMemory, EpubRenderMode::Light);
  if (!created || lowMemory || output->pageCount == 0) {
    error = "cannot paginate prepared PDF XHTML";
    return false;
  }
  return true;
}

std::string frameHash(GfxRenderer& renderer) {
  return hashHex(hashBytes(renderer.getFrameBuffer(), renderer.getBufferSize()));
}

bool renderPage(Section& section, const uint16_t pageIndex, GfxRenderer& renderer, const Layout& layout,
                std::string& hash, std::string& error) {
  if (pageIndex >= section.pageCount) {
    error = "page index is outside prepared section";
    return false;
  }
  section.currentPage = pageIndex;
  auto page = section.loadPageFromSectionFile();
  if (!page) {
    error = "cannot deserialize prepared PDF page";
    return false;
  }
  renderer.clearScreen(0xff);
  page->render(renderer, SETTINGS.getReaderFontId(), layout.left, layout.top, true);
  hash = frameHash(renderer);
  return true;
}

bool capturePageLayout(const Page& page, const uint16_t pageIndex, std::vector<LayoutProbe>& output) {
  for (size_t elementIndex = 0; elementIndex < page.elements.size(); ++elementIndex) {
    const std::shared_ptr<PageElement>& element = page.elements[elementIndex];
    if (!element || elementIndex > UINT16_MAX) {
      return false;
    }
    LayoutProbe probe;
    probe.pageIndex = pageIndex;
    probe.elementIndex = static_cast<uint16_t>(elementIndex);
    probe.tag = static_cast<uint8_t>(element->getTag());
    probe.x = element->xPos;
    probe.y = element->yPos;
    if (element->getTag() != TAG_PageLine) {
      output.push_back(probe);
      continue;
    }
    const auto& line = static_cast<const PageLine&>(*element);
    const std::shared_ptr<TextBlock>& block = line.getBlock();
    if (!block || block->wordCount() == 0) {
      output.push_back(probe);
      continue;
    }
    for (uint16_t wordIndex = 0; wordIndex < block->wordCount(); ++wordIndex) {
      LayoutProbe wordProbe = probe;
      wordProbe.wordIndex = wordIndex;
      wordProbe.wordX = block->wordXpos(wordIndex);
      wordProbe.wordStyle = static_cast<uint8_t>(block->wordStyle(wordIndex));
      wordProbe.wordFlags = block->wordFlags(wordIndex);
      wordProbe.wordLength = block->wordTextLen(wordIndex);
      wordProbe.wordHash =
          hashBytes(reinterpret_cast<const uint8_t*>(block->wordText(wordIndex)), wordProbe.wordLength);
      output.push_back(wordProbe);
    }
  }
  return true;
}

bool captureTypography(const std::shared_ptr<ReflowDocument>& document, GfxRenderer& renderer, const bool cachedPass,
                       AcceptanceCounters& counters, const char* suffix, FrameTriplet& output, std::string& error) {
  Layout layout;
  if (!computeLayout(renderer, layout, error)) {
    return false;
  }
  std::unique_ptr<Section> section;
  if (!loadOrCreateSection(document, renderer, layout, 0, suffix, cachedPass, counters, section, error)) {
    return false;
  }
  StringPrint semanticXhtml;
  if (!document->streamSection(0, semanticXhtml, 256)) {
    error = "cannot stream prepared PDF typography XHTML";
    return false;
  }
  output.semanticHash = hashHex(hashString(normalizeSemanticText(semanticXhtml.contents())));
  output.wordCount = document->getTotalWordCount();
  output.pageCount = section->pageCount;
  output.fontId = SETTINGS.getReaderFontId();
  output.fontSize = SETTINGS.fontSize;
  output.fontPointSize = CrossPointSettings::getReaderFontPointSize(SETTINGS.getEffectiveReaderFontSize());
  output.lineHeightPercent = SETTINGS.lineHeightPercent;
  uint64_t textHash = kFnvOffset;
  for (uint16_t page = 0; page < section->pageCount; ++page) {
    section->currentPage = page;
    auto layoutPage = section->loadPageFromSectionFile();
    if (!layoutPage || !capturePageLayout(*layoutPage, page, output.layout)) {
      error = "cannot inspect prepared PDF page layout";
      return false;
    }
    const std::string text = section->getTextFromSectionFile();
    if (page == 0) {
      output.firstPageTextHash = hashHex(hashString(text));
    }
    textHash = hashString(text, textHash);
    const uint8_t separator = 0;
    textHash = hashBytes(&separator, 1, textHash);
  }
  output.textHash = hashHex(textHash);
  const uint16_t middle = static_cast<uint16_t>(section->pageCount / 2U);
  return renderPage(*section, 0, renderer, layout, output.firstFrame, error) &&
         renderPage(*section, middle, renderer, layout, output.middleFrame, error) &&
         renderPage(*section, static_cast<uint16_t>(section->pageCount - 1U), renderer, layout, output.lastFrame,
                    error);
}

void appendTypography(JsonObject object, const FrameTriplet& frame) {
  object["semantic_hash"] = frame.semanticHash;
  object["word_count"] = frame.wordCount;
  object["text_hash"] = frame.textHash;
  object["first_page_text_hash"] = frame.firstPageTextHash;
  object["page_count"] = frame.pageCount;
  object["font_id"] = frame.fontId;
  object["font_size"] = frame.fontSize;
  object["font_point_size"] = frame.fontPointSize;
  object["line_height_percent"] = frame.lineHeightPercent;
  object["first_frame"] = frame.firstFrame;
  object["middle_frame"] = frame.middleFrame;
  object["last_frame"] = frame.lastFrame;
}

void logTypographyDiagnostic(const char* fixture, const FrameTriplet& frame) {
  const std::string firstLayout = describeLayoutProbe(frame.layout.empty() ? nullptr : &frame.layout.front());
  std::printf(
      "SIM_PDF_TYPOGRAPHY_DIAG fixture=%s semantic_hash=%s words=%lu page_text_hash=%s first_page_text_hash=%s "
      "pages=%u font_id=%d font_size=%u font_point_size=%u line_height_percent=%u first_frame=%s middle_frame=%s "
      "last_frame=%s first_layout={%s}\n",
      fixture, frame.semanticHash.c_str(), static_cast<unsigned long>(frame.wordCount), frame.textHash.c_str(),
      frame.firstPageTextHash.c_str(), frame.pageCount, frame.fontId, frame.fontSize, frame.fontPointSize,
      frame.lineHeightPercent, frame.firstFrame.c_str(), frame.middleFrame.c_str(), frame.lastFrame.c_str(),
      firstLayout.c_str());
}

bool validateEveryInternalLinkTarget(const PdfReflowDocument& document,
                                     const std::vector<std::string>& sectionContents,
                                     size_t& resolvedLinks, std::string& error) {
  resolvedLinks = 0;
  constexpr char hrefPrefix[] = "href=\"";
  constexpr char sectionPrefix[] = "sections/";
  for (const std::string& source : sectionContents) {
    size_t cursor = 0;
    while ((cursor = source.find(hrefPrefix, cursor)) != std::string::npos) {
      const size_t hrefStart = cursor + sizeof(hrefPrefix) - 1U;
      const size_t hrefEnd = source.find('"', hrefStart);
      if (hrefEnd == std::string::npos) {
        error = "PDF internal href is unterminated";
        return false;
      }
      cursor = hrefEnd + 1U;
      const std::string href = source.substr(hrefStart, hrefEnd - hrefStart);
      if (href.compare(0, sizeof(sectionPrefix) - 1U, sectionPrefix) != 0) {
        continue;
      }
      const size_t fragmentOffset = href.find('#');
      const int targetSection = document.resolveHrefToSectionIndex(href);
      if (fragmentOffset == std::string::npos || fragmentOffset + 1U == href.size() || targetSection < 0 ||
          static_cast<size_t>(targetSection) >= sectionContents.size()) {
        error = "PDF internal href does not resolve to a section and fragment";
        return false;
      }
      const std::string fragment = href.substr(fragmentOffset + 1U);
      const std::string anchor = "id=\"" + fragment + "\"";
      const std::string& target = sectionContents[static_cast<size_t>(targetSection)];
      if (target.find(anchor) == std::string::npos) {
        error = "PDF internal href fragment does not name a target anchor";
        return false;
      }
      ++resolvedLinks;
    }
  }
  return true;
}

bool captureNavigation(const std::shared_ptr<PdfReflowDocument>& document, GfxRenderer& renderer, const bool cachedPass,
                       AcceptanceCounters& counters, JsonObject navigation, JsonObject progress, std::string& error) {
  if (document->getSectionCount() != 2 || document->getTocEntryCount() != 3 || document->getTotalWordCount() != 10) {
    error = "PDF chapter/index metadata is incomplete";
    return false;
  }
  Layout layout;
  if (!computeLayout(renderer, layout, error)) {
    return false;
  }

  std::vector<std::unique_ptr<Section>> sections;
  sections.reserve(static_cast<size_t>(document->getSectionCount()));
  std::vector<std::string> sectionContents;
  sectionContents.reserve(static_cast<size_t>(document->getSectionCount()));
  std::string streamed;
  for (int index = 0; index < document->getSectionCount(); ++index) {
    std::unique_ptr<Section> section;
    if (!loadOrCreateSection(document, renderer, layout, index, "_sim_pdf_navigation", cachedPass, counters, section,
                             error)) {
      return false;
    }
    sections.push_back(std::move(section));
    StringPrint sectionText;
    if (!document->streamSection(index, sectionText, 256)) {
      error = "cannot stream prepared chapter XHTML";
      return false;
    }
    sectionContents.push_back(sectionText.contents());
    streamed += sectionText.contents();
  }

  uint64_t tocHash = kFnvOffset;
  for (int index = 0; index < document->getTocEntryCount(); ++index) {
    const ReflowTocEntry entry = document->getTocEntry(index);
    if (entry.sectionIndex < 0 || document->resolveHrefToSectionIndex(entry.href) != entry.sectionIndex) {
      error = "PDF TOC entry does not resolve to its chapter";
      return false;
    }
    tocHash = hashString(entry.title, tocHash);
    tocHash = hashString(entry.href, tocHash);
    tocHash = hashBytes(&entry.level, sizeof(entry.level), tocHash);
  }
  const size_t internalLinks = countSubstring(streamed, "href=\"sections/");
  const size_t publisherLabels = countSubstring(streamed, "aria-label=");
  size_t resolvedInternalLinks = 0;
  if (!validateEveryInternalLinkTarget(*document, sectionContents, resolvedInternalLinks, error)) {
    return false;
  }
  if (internalLinks < 2 || resolvedInternalLinks != internalLinks || publisherLabels < 2 ||
      streamed.find(">Index</p>") == std::string::npos) {
    error = "PDF internal links, labels, or index did not survive preparation";
    return false;
  }
  navigation["sections"] = document->getSectionCount();
  navigation["toc_entries"] = document->getTocEntryCount();
  navigation["internal_links"] = internalLinks;
  navigation["resolved_internal_links"] = resolvedInternalLinks;
  navigation["publisher_labels"] = publisherLabels;
  navigation["toc_hash"] = hashHex(tocHash);

  const uint32_t totalWords = document->getTotalWordCount();
  const uint32_t targetCursor = totalWords * 3U / 5U;
  ReflowReadingPosition selected;
  bool selectedRange = false;
  for (size_t sectionIndex = 0; sectionIndex < sections.size() && !selectedRange; ++sectionIndex) {
    Section& section = *sections[sectionIndex];
    for (uint16_t page = 0; page < section.pageCount; ++page) {
      const auto range = section.getSemanticRangeForPage(page);
      if (!range || !range->valid || range->wordCursor < targetCursor) {
        continue;
      }
      selected.sectionIndex = static_cast<int>(sectionIndex);
      selected.pageNumber = page;
      selected.pageCount = section.pageCount;
      selected.hasPageCount = true;
      selectedRange = pdfPopulateReadingPositionFromRange(*range, &selected);
      if (selectedRange) {
        break;
      }
    }
  }
  if (!selectedRange || selected.wordCursor == 0 || selected.wordCursor > totalWords) {
    error = "cannot select exact PDF word progress";
    return false;
  }

  selected.pageNumber = 0;
  selected.pageCount = 0;
  selected.hasPageCount = false;
  ReflowReadingPosition nonTerminal = selected;
  nonTerminal.wordCursor = targetCursor;
  if (targetCursor != 6 || nonTerminal.wordCursor == 0 ||
      nonTerminal.wordCursor >= selected.wordCursor ||
      selected.wordCursor != totalWords) {
    error = "PDF progress fixture does not expose both 60 and 100 percent";
    return false;
  }

  ReflowReadingPosition nonTerminalResumed;
  ReflowReadingPosition resumed;
  if (cachedPass) {
    if (!document->loadReadingPosition(resumed) || !pdfReadingPositionsEqualExact(selected, resumed)) {
      error = "cached pass did not resume the exact word position";
      return false;
    }
  }
  if (!document->saveReadingPosition(nonTerminal) ||
      !document->loadReadingPosition(nonTerminalResumed) ||
      !pdfReadingPositionsEqualExact(nonTerminal, nonTerminalResumed)) {
    error = "non-terminal PDF word position did not persist";
    return false;
  }
  if (!document->saveReadingPosition(selected) ||
      !document->loadReadingPosition(resumed) ||
      !pdfReadingPositionsEqualExact(selected, resumed)) {
    error = "terminal PDF word position did not persist";
    return false;
  }
  progress["total_words"] = totalWords;
  progress["nonterminal_saved_cursor"] = nonTerminal.wordCursor;
  progress["nonterminal_resumed_cursor"] =
      nonTerminalResumed.wordCursor;
  float wordProgress = 0.0F;
  if (!pdfCalculateWordCursorProgress(nonTerminalResumed.wordCursor, totalWords, &wordProgress)) {
    error = "production PDF word progress rejected the non-terminal cursor";
    return false;
  }
  progress["nonterminal_percent_millionths"] =
      static_cast<uint32_t>(std::lround(wordProgress * 1000000.0F));
  progress["saved_cursor"] = selected.wordCursor;
  progress["resumed_cursor"] = resumed.wordCursor;
  float terminalProgress = 0.0F;
  if (!pdfCalculateWordCursorProgress(resumed.wordCursor, totalWords, &terminalProgress)) {
    error = "production PDF word progress rejected the terminal cursor";
    return false;
  }
  progress["percent_millionths"] =
      static_cast<uint32_t>(std::lround(terminalProgress * 1000000.0F));

  if (cachedPass) {
    const uint32_t opensBefore = counters.sourceOpenCalls;
    const uint32_t readsBefore = counters.sourceReadCalls;
    for (uint16_t turn = 0; turn < kCachedPageTurns; ++turn) {
      Section& section = *sections[turn % sections.size()];
      const uint16_t page = static_cast<uint16_t>((turn / sections.size()) % section.pageCount);
      std::string ignoredHash;
      if (!renderPage(section, page, renderer, layout, ignoredHash, error)) {
        return false;
      }
    }
    counters.cachedPageTurns = kCachedPageTurns;
    counters.pageTurnSourceOpens = counters.sourceOpenCalls - opensBefore;
    counters.pageTurnSourceReads = counters.sourceReadCalls - readsBefore;
    if (counters.pageTurnSourceOpens != 0 || counters.pageTurnSourceReads != 0) {
      error = "cached page turns reopened the source PDF";
      return false;
    }
  }
  return true;
}

bool captureImage(const std::shared_ptr<ReflowDocument>& document, GfxRenderer& renderer, const bool cachedPass,
                  AcceptanceCounters& counters, JsonObject image, std::string& error) {
  Layout layout;
  if (!computeLayout(renderer, layout, error)) {
    return false;
  }
  StringPrint xhtml;
  if (!document->streamSection(0, xhtml, 256)) {
    error = "cannot inspect prepared image XHTML";
    return false;
  }
  const size_t retained = countSubstring(xhtml.contents(), "<img ");
  if (retained == 0) {
    error = "prepared PDF retained no supported image";
    return false;
  }

  SETTINGS.imageRendering = CrossPointSettings::IMAGE_RENDERING::IMAGES_DISPLAY;
  std::unique_ptr<Section> withImage;
  if (!loadOrCreateSection(document, renderer, layout, 0, "_sim_pdf_image", cachedPass, counters, withImage, error)) {
    return false;
  }
  std::string imageFrame;
  if (!renderPage(*withImage, 0, renderer, layout, imageFrame, error)) {
    return false;
  }
  withImage->currentPage = 0;
  auto imagePage = withImage->loadPageFromSectionFile();
  const PageImage* pageImageElement = nullptr;
  if (imagePage) {
    for (const std::shared_ptr<PageElement>& element : imagePage->elements) {
      if (element && element->getTag() == TAG_PageImage) {
        pageImageElement = &static_cast<const PageImage&>(*element);
        break;
      }
    }
  }
  if (pageImageElement == nullptr) {
    error = "prepared PDF image page has no PageImage element";
    return false;
  }
  const PageImage& pageImage = *pageImageElement;
  const int imageX = layout.left + pageImage.xPos;
  const int imageY = layout.top + pageImage.yPos;
  const int imageWidth = pageImage.getImageBlock().getWidth();
  const int imageHeight = pageImage.getImageBlock().getHeight();
  const size_t imageRegionBytes = renderer.getRegionByteSize(imageX, imageY, imageWidth, imageHeight);
  auto imageRegion = makeUniqueNoThrow<uint8_t[]>(imageRegionBytes);
  if (imageWidth <= 0 || imageHeight <= 0 || imageRegionBytes == 0 || !imageRegion ||
      !renderer.copyRegionToBuffer(imageX, imageY, imageWidth, imageHeight, imageRegion.get(), imageRegionBytes)) {
    error = "cannot inspect PageImage framebuffer rectangle";
    return false;
  }
  uint32_t nonWhitePixels = 0;
  for (size_t index = 0; index < imageRegionBytes; ++index) {
    uint8_t nonWhiteBits = static_cast<uint8_t>(~imageRegion[index]);
    for (uint8_t bit = 0; bit < 8; ++bit) {
      nonWhitePixels += nonWhiteBits & 1U;
      nonWhiteBits >>= 1U;
    }
  }
  if (nonWhitePixels == 0) {
    error = "PageImage rectangle rendered no deterministic non-white pixels";
    return false;
  }

  SETTINGS.imageRendering = CrossPointSettings::IMAGE_RENDERING::IMAGES_SUPPRESS;
  std::unique_ptr<Section> withoutImage;
  if (!loadOrCreateSection(document, renderer, layout, 0, "_sim_pdf_image_suppressed", cachedPass, counters,
                           withoutImage, error)) {
    SETTINGS.imageRendering = CrossPointSettings::IMAGE_RENDERING::IMAGES_DISPLAY;
    return false;
  }
  std::string suppressedFrame;
  const bool rendered = renderPage(*withoutImage, 0, renderer, layout, suppressedFrame, error);
  SETTINGS.imageRendering = CrossPointSettings::IMAGE_RENDERING::IMAGES_DISPLAY;
  if (!rendered || imageFrame == suppressedFrame) {
    error = "retained PDF image did not affect the framebuffer";
    return false;
  }
  image["retained"] = retained;
  image["frame_hash"] = imageFrame;
  image["blank_hash"] = suppressedFrame;
  image["page_image_found"] = true;
  JsonArray imageRect = image["rect"].to<JsonArray>();
  imageRect.add(imageX);
  imageRect.add(imageY);
  imageRect.add(imageWidth);
  imageRect.add(imageHeight);
  image["non_white_pixels"] = nonWhitePixels;
  image["region_hash"] = hashHex(hashBytes(imageRegion.get(), imageRegionBytes));
  return true;
}

void appendSourceIdentity(JsonObject object, const PdfSourceIdentity& source) {
  object["size"] = source.size;
  object["modification_time_known"] = source.modificationTime.known;
  object["modification_time"] = source.modificationTime.value;
  object["head_fingerprint"] = source.headFingerprint;
  object["tail_fingerprint"] = source.tailFingerprint;
}

void appendCheckpoint(JsonObject object, const PdfBuildCheckpoint& checkpoint) {
  object["name"] = "after_image_repair";
  object["sequence"] = checkpoint.sequence;
  object["resume_phase"] = "after_image_repair";
  object["last_verified_page"] = checkpoint.lastVerifiedPage;
  object["last_verified_object"] = checkpoint.lastVerifiedObject;
  object["emitted_sections"] = checkpoint.emittedSections;
  object["emitted_images"] = checkpoint.emittedImages;
  object["cumulative_words"] = checkpoint.cumulativeWords;
  object["output_bytes"] = checkpoint.outputBytes;
  object["warning_flags"] = checkpoint.warningFlags;
}

void appendCounterSnapshot(JsonObject object, const ResumeEvidence& evidence) {
  object["preparation_steps"] = evidence.phaseWork.steps;
  object["parser_steps"] = evidence.phaseWork.parserSteps;
  object["page_steps"] = evidence.phaseWork.pageSteps;
  object["image_steps"] = evidence.phaseWork.imageSteps;
  object["xref_steps"] = evidence.productWork.xrefSteps;
  object["pages_walked"] = evidence.productWork.pagesWalked;
  object["content_tokens"] = evidence.productWork.contentTokens;
  object["sections_emitted"] = evidence.productWork.sectionsEmitted;
  object["images_emitted"] = evidence.productWork.imagesEmitted;
  object["source_bytes_read"] = evidence.productWork.sourceBytesRead;
}

void appendContinuity(JsonObject object, const ResumeEvidence& evidence, const bool resumed) {
  object["checkpoint_name"] = "after_image_repair";
  appendSourceIdentity(object["source_identity"].to<JsonObject>(), evidence.source);
  object["generation"] = evidence.generation;
  appendCheckpoint(object[resumed ? "resumed_checkpoint" : "checkpoint"].to<JsonObject>(), evidence.checkpoint);
  appendCounterSnapshot(object["counter_snapshot"].to<JsonObject>(), evidence);
  if (resumed) {
    object["resumed_from_checkpoint"] = true;
  }
}

void emitAcceptanceResult(JsonDocument& result) {
  std::string serialized;
  serializeJson(result, serialized);
  std::printf("%s%s\n", kResultMarker, serialized.c_str());
  std::fflush(stdout);
}

bool runCancellationPhase(GfxRenderer& renderer, const std::string& fixtureRoot, const char* const runnerNonce,
                          std::string& error) {
  AcceptanceCounters counters;
  auto instrumented = makeUniqueNoThrow<InstrumentedCacheIo>(counters);
  if (!instrumented) {
    error = "cannot allocate cancellation I/O instrumentation";
    return false;
  }
  AcceptanceClock clock;
  ResumeEvidence evidence;
  if (!prepareCancellationCheckpoint(fixturePath(fixtureRoot, kCancelResume), *instrumented, clock, counters, renderer,
                                     runnerNonce, evidence, error)) {
    return false;
  }

  JsonDocument result;
  result["schema_version"] = 1;
  result["pass"] = kUncachedPass;
  result["phase"] = kCancelPhase;
  result["runner_nonce"] = runnerNonce;
  appendContinuity(result["continuity"].to<JsonObject>(), evidence, false);
  JsonObject counterJson = result["counters"].to<JsonObject>();
  counterJson["cancellation_steps"] = counters.cancellationSteps;
  counterJson["cancellation_elapsed_ms"] = counters.cancellationElapsedMs;
  counterJson["cancellation_max_slice_ms"] = counters.cancellationMaxSliceMs;
  counterJson["cancellation_max_slice_io_calls"] = counters.cancellationMaxSliceIoCalls;
  counterJson["max_actual_step_us"] = counters.maxActualStepUs;
  counterJson["framebuffer_guard_checks"] = counters.framebufferGuardChecks;
  counterJson["framebuffer_guard_failures"] = counters.framebufferGuardFailures;
  counterJson["framebuffer_guard_controls"] = counters.framebufferGuardControls;
  counterJson["framebuffer_guard_rejections"] = counters.framebufferGuardRejections;
  emitAcceptanceResult(result);
  return true;
}

}  // namespace

bool runPdfSimulatorAcceptance(GfxRenderer& renderer, std::string& error) {
  const char* passName = std::getenv("CROSSINK_SIMULATOR_PDF_ACCEPTANCE_PASS");
  const char* phaseName = std::getenv("CROSSINK_SIMULATOR_PDF_ACCEPTANCE_PHASE");
  const char* runnerNonce = std::getenv("CROSSINK_SIMULATOR_PDF_ACCEPTANCE_NONCE");
  const char* fixtureRootRaw = std::getenv("CROSSINK_SIMULATOR_PDF_FIXTURE_ROOT");
  if (passName == nullptr || phaseName == nullptr || !runnerNonceValid(runnerNonce) || fixtureRootRaw == nullptr ||
      fixtureRootRaw[0] == '\0') {
    error = "PDF simulator acceptance environment is incomplete";
    return false;
  }
  const bool cancelPhase = std::strcmp(phaseName, kCancelPhase) == 0;
  const bool resumePhase = std::strcmp(phaseName, kResumePhase) == 0;
  const bool cachedPass = std::strcmp(phaseName, kCachedPhase) == 0;
  if ((!cancelPhase && !resumePhase && !cachedPass) ||
      ((cancelPhase || resumePhase) && std::strcmp(passName, kUncachedPass) != 0) ||
      (cachedPass && std::strcmp(passName, kCachedPass) != 0)) {
    error = "PDF simulator acceptance pass/phase combination is invalid";
    return false;
  }

  std::printf("%s{\"phase\":\"%s\",\"runner_nonce\":\"%s\"}\n", kResetMarker, phaseName, runnerNonce);
  std::fflush(stdout);
  pinReaderSettings(renderer);
  const std::string fixtureRoot(fixtureRootRaw);
  if (cancelPhase) {
    return runCancellationPhase(renderer, fixtureRoot, runnerNonce, error);
  }
  const std::string cancelResumePath = fixturePath(fixtureRoot, kCancelResume);
  const std::string freshResumeBaselinePath = fixturePath(fixtureRoot, kFreshResumeBaseline);
  const std::string fontSixPath = fixturePath(fixtureRoot, kFontSix);
  const std::string fontSeventyTwoPath = fixturePath(fixtureRoot, kFontSeventyTwo);
  const std::string navigationPath = fixturePath(fixtureRoot, kNavigation);
  const std::string imagePath = fixturePath(fixtureRoot, kImage);

  AcceptanceCounters counters;
  auto instrumented = makeUniqueNoThrow<InstrumentedCacheIo>(counters);
  if (!instrumented) {
    error = "cannot allocate simulator I/O instrumentation";
    return false;
  }
  AcceptanceClock clock;
  PreparationWorkCounters freshBaselineWork;
  PreparationWorkCounters resumedWork;
  PdfPreparationWorkCounters freshBaselineProductWork;
  PdfPreparationWorkCounters resumedProductWork;
  ResumeEvidence resumeEvidence;
  uint8_t negativeRejected = 0;

  if (!cachedPass) {
    if (!readResumeEvidence(runnerNonce, resumeEvidence, error) ||
        !prepareValid(freshResumeBaselinePath, *instrumented, clock, counters, renderer, &freshBaselineWork,
                      &freshBaselineProductWork, nullptr, error) ||
        !prepareValid(cancelResumePath, *instrumented, clock, counters, renderer, &resumedWork, &resumedProductWork,
                      &resumeEvidence, error) ||
        !prepareValid(fontSixPath, *instrumented, clock, counters, renderer, nullptr, nullptr, nullptr, error) ||
        !prepareValid(fontSeventyTwoPath, *instrumented, clock, counters, renderer, nullptr, nullptr, nullptr, error) ||
        !prepareValid(navigationPath, *instrumented, clock, counters, renderer, nullptr, nullptr, nullptr, error)) {
      return false;
    }
    counters.freshBaselineSteps = freshBaselineWork.steps;
    counters.freshBaselineParserSteps = freshBaselineWork.parserSteps;
    counters.freshBaselinePageSteps = freshBaselineWork.pageSteps;
    counters.freshBaselineImageSteps = freshBaselineWork.imageSteps;
    counters.resumedPreparationSteps = resumedWork.steps;
    counters.resumedParserSteps = resumedWork.parserSteps;
    counters.resumedPageSteps = resumedWork.pageSteps;
    counters.resumedImageSteps = resumedWork.imageSteps;
    const uint64_t freshPageProductWork = static_cast<uint64_t>(freshBaselineProductWork.pagesWalked) +
                                          freshBaselineProductWork.contentTokens +
                                          freshBaselineProductWork.sectionsEmitted;
    const uint64_t resumedPageProductWork = static_cast<uint64_t>(resumedProductWork.pagesWalked) +
                                            resumedProductWork.contentTokens + resumedProductWork.sectionsEmitted;
    if (resumedWork.steps >= freshBaselineWork.steps || resumedWork.parserSteps >= freshBaselineWork.parserSteps ||
        resumedWork.pageSteps >= freshBaselineWork.pageSteps || resumedWork.imageSteps >= freshBaselineWork.imageSteps ||
        resumedProductWork.xrefSteps >= freshBaselineProductWork.xrefSteps ||
        resumedPageProductWork >= freshPageProductWork ||
        resumedProductWork.imagesEmitted >= freshBaselineProductWork.imagesEmitted ||
        resumedProductWork.sourceBytesRead >= freshBaselineProductWork.sourceBytesRead) {
      error = "cross-process resume did not reduce every completed work category";
      return false;
    }
    for (const char* fixture : kNegativeFixtures) {
      if (!rejectDuringPreparation(fixturePath(fixtureRoot, fixture), *instrumented, clock, counters, renderer,
                                   error)) {
        return false;
      }
      ++negativeRejected;
    }
  } else {
    for (const char* fixture : kNegativeFixtures) {
      if (!rejectMissingCompletedCache(instrumented->io(), fixturePath(fixtureRoot, fixture))) {
        error = "unsafe PDF unexpectedly had a completed cache";
        return false;
      }
      ++negativeRejected;
    }
  }

  instrumented->trackSource(navigationPath);
  auto navigationDocument = loadDocument(instrumented->io(), navigationPath, error);
  if (!navigationDocument) {
    return false;
  }
  if (counters.sourceOpenCalls != 1 || counters.sourceReadCalls > 2 || counters.sourceMaxRead > 4096) {
    error = "completed cache identity transaction is not bounded";
    return false;
  }

  JsonDocument result;
  result["schema_version"] = 1;
  result["pass"] = passName;
  result["phase"] = phaseName;
  result["runner_nonce"] = runnerNonce;
  if (resumePhase) {
    appendContinuity(result["continuity"].to<JsonObject>(), resumeEvidence, true);
  }
  JsonObject oracle = result["oracle"].to<JsonObject>();
  JsonObject navigation = oracle["navigation"].to<JsonObject>();
  JsonObject progress = oracle["progress"].to<JsonObject>();
  if (!captureNavigation(navigationDocument, renderer, cachedPass, counters, navigation, progress, error)) {
    return false;
  }

  auto fontSixDocument = loadDocument(instrumented->io(), fontSixPath, error);
  auto fontSeventyTwoDocument = loadDocument(instrumented->io(), fontSeventyTwoPath, error);
  if (!fontSixDocument || !fontSeventyTwoDocument) {
    return false;
  }
  FrameTriplet fontSix;
  FrameTriplet fontSeventyTwo;
  FrameTriplet positiveFont;
  FrameTriplet landscape;
  FrameTriplet wideMargin;
  applyReaderLayout(renderer, CrossPointSettings::ORIENTATION::PORTRAIT, CrossPointSettings::FONT_SIZE::MEDIUM, 5);
  if (!captureTypography(fontSixDocument, renderer, cachedPass, counters, "_sim_pdf_typography_medium", fontSix,
                         error) ||
      !captureTypography(fontSeventyTwoDocument, renderer, cachedPass, counters, "_sim_pdf_typography_medium",
                         fontSeventyTwo, error)) {
    return false;
  }
  if (fontSix.semanticHash != fontSeventyTwo.semanticHash || fontSix.wordCount != fontSeventyTwo.wordCount ||
      fontSix.textHash != fontSeventyTwo.textHash || fontSix.pageCount != fontSeventyTwo.pageCount ||
      fontSix.firstFrame != fontSeventyTwo.firstFrame || fontSix.middleFrame != fontSeventyTwo.middleFrame ||
      fontSix.lastFrame != fontSeventyTwo.lastFrame) {
    logTypographyDiagnostic("font_size_6.pdf", fontSix);
    logTypographyDiagnostic("font_size_72.pdf", fontSeventyTwo);
    const std::string firstLayoutDifference = describeFirstLayoutDifference(fontSix, fontSeventyTwo);
    std::printf("SIM_PDF_TYPOGRAPHY_DIAG first_layout_difference=%s\n", firstLayoutDifference.c_str());
    std::fflush(stdout);
    error = "PDF font sizes leaked into device typography";
    return false;
  }
  applyReaderLayout(renderer, CrossPointSettings::ORIENTATION::PORTRAIT, CrossPointSettings::FONT_SIZE::LARGE, 5);
  if (!captureTypography(fontSixDocument, renderer, cachedPass, counters, "_sim_pdf_typography_large", positiveFont,
                         error)) {
    return false;
  }
  if (positiveFont.textHash != fontSix.textHash ||
      (positiveFont.pageCount == fontSix.pageCount && positiveFont.firstFrame == fontSix.firstFrame &&
       positiveFont.middleFrame == fontSix.middleFrame && positiveFont.lastFrame == fontSix.lastFrame)) {
    error = "device font-size positive control did not change reflow";
    return false;
  }
  applyReaderLayout(renderer, CrossPointSettings::ORIENTATION::LANDSCAPE_CCW, CrossPointSettings::FONT_SIZE::MEDIUM, 5);
  if (!captureTypography(fontSixDocument, renderer, cachedPass, counters, "_sim_pdf_typography_landscape", landscape,
                         error)) {
    return false;
  }
  applyReaderLayout(renderer, CrossPointSettings::ORIENTATION::PORTRAIT, CrossPointSettings::FONT_SIZE::MEDIUM, 20);
  if (!captureTypography(fontSixDocument, renderer, cachedPass, counters, "_sim_pdf_typography_margin", wideMargin,
                         error)) {
    return false;
  }
  if (landscape.firstFrame == fontSix.firstFrame || wideMargin.firstFrame == fontSix.firstFrame) {
    error = "orientation or margin positive control did not change reflow";
    return false;
  }

  applyReaderLayout(renderer, CrossPointSettings::ORIENTATION::PORTRAIT, CrossPointSettings::FONT_SIZE::MEDIUM, 5);
  JsonObject typography = oracle["typography"].to<JsonObject>();
  appendTypography(typography["pdf_6"].to<JsonObject>(), fontSix);
  appendTypography(typography["pdf_72"].to<JsonObject>(), fontSeventyTwo);
  appendTypography(typography["device_font_positive"].to<JsonObject>(), positiveFont);
  typography["fixed_page_canvas"] = false;
  JsonObject layoutControls = oracle["layout_controls"].to<JsonObject>();
  layoutControls["portrait_frame"] = fontSix.firstFrame;
  layoutControls["landscape_frame"] = landscape.firstFrame;
  layoutControls["wide_margin_frame"] = wideMargin.firstFrame;

  auto imageDocument = loadDocument(instrumented->io(), imagePath, error);
  if (!imageDocument ||
      !captureImage(imageDocument, renderer, cachedPass, counters, oracle["image"].to<JsonObject>(), error)) {
    return false;
  }

  JsonObject route = oracle["route"].to<JsonObject>();
  route["raw_pdf"] = true;
  route["on_device_preparation"] = true;
  route["cancelled"] = true;
  route["resumed"] = true;
  route["cached_reopen"] = true;

  JsonObject negative = result["negative"].to<JsonObject>();
  negative["checked"] = kNegativeFixtures.size();
  negative["rejected"] = negativeRejected;
  JsonObject counterJson = result["counters"].to<JsonObject>();
  counterJson["preparation_steps"] = counters.preparationSteps;
  counterJson["extraction_runs"] = counters.extractionRuns;
  counterJson["parser_calls"] = counters.parserCalls;
  counterJson["source_open_calls"] = counters.sourceOpenCalls;
  counterJson["source_read_calls"] = counters.sourceReadCalls;
  counterJson["source_read_bytes"] = counters.sourceReadBytes;
  counterJson["source_max_read"] = counters.sourceMaxRead;
  counterJson["cached_page_turns"] = counters.cachedPageTurns;
  counterJson["page_turn_source_opens"] = counters.pageTurnSourceOpens;
  counterJson["page_turn_source_reads"] = counters.pageTurnSourceReads;
  counterJson["io_calls"] = counters.ioCalls;
  counterJson["max_io_request"] = counters.maxIoRequest;
  counterJson["yielded_slices"] = counters.yieldedSlices;
  counterJson["cancelled_runs"] = counters.cancelledRuns;
  counterJson["resumed_runs"] = counters.resumedRuns;
  counterJson["cancellation_steps"] = counters.cancellationSteps;
  counterJson["cancellation_elapsed_ms"] = counters.cancellationElapsedMs;
  counterJson["cancellation_max_slice_ms"] = counters.cancellationMaxSliceMs;
  counterJson["cancellation_max_slice_io_calls"] = counters.cancellationMaxSliceIoCalls;
  counterJson["fresh_baseline_steps"] = counters.freshBaselineSteps;
  counterJson["fresh_baseline_parser_steps"] = counters.freshBaselineParserSteps;
  counterJson["fresh_baseline_page_steps"] = counters.freshBaselinePageSteps;
  counterJson["fresh_baseline_image_steps"] = counters.freshBaselineImageSteps;
  counterJson["resumed_preparation_steps"] = counters.resumedPreparationSteps;
  counterJson["resumed_parser_steps"] = counters.resumedParserSteps;
  counterJson["resumed_page_steps"] = counters.resumedPageSteps;
  counterJson["resumed_image_steps"] = counters.resumedImageSteps;
  counterJson["max_actual_step_us"] = counters.maxActualStepUs;
  counterJson["framebuffer_guard_checks"] = counters.framebufferGuardChecks;
  counterJson["framebuffer_guard_failures"] = counters.framebufferGuardFailures;
  counterJson["framebuffer_guard_controls"] = counters.framebufferGuardControls;
  counterJson["framebuffer_guard_rejections"] = counters.framebufferGuardRejections;

  if ((!cachedPass && (counters.cancelledRuns != 0 || counters.resumedRuns != 1 || counters.cancellationSteps != 0 ||
                       counters.cancellationElapsedMs != 0 || counters.cancellationMaxSliceMs != 0 ||
                       counters.cancellationMaxSliceIoCalls != 0 || counters.framebufferGuardChecks == 0)) ||
      (cachedPass && (counters.preparationSteps != 0 || counters.extractionRuns != 0 || counters.parserCalls != 0 ||
                      counters.framebufferGuardChecks != 0)) ||
      counters.framebufferGuardFailures != 0 ||
      counters.framebufferGuardControls != counters.framebufferGuardChecks * 2U ||
      counters.framebufferGuardRejections != counters.framebufferGuardControls ||
      counters.maxIoRequest > kMaximumIoRequestBytes || negativeRejected != kNegativeFixtures.size()) {
    error = "PDF CPU, I/O, cancellation, or fail-closed counters are invalid";
    return false;
  }

  emitAcceptanceResult(result);
  return true;
}

#endif
