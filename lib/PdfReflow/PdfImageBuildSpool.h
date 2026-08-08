#pragma once

#include <cstddef>
#include <cstdint>

#include "PdfCacheIo.h"
#include "PdfCacheManifest.h"
#include "PdfImageObject.h"
#include "PdfStreamDecoder.h"
#include "PdfWorkBudget.h"

inline constexpr uint16_t PDF_IMAGE_BUILD_SPOOL_VERSION = 4;
inline constexpr uint8_t PDF_IMAGE_BUILD_SPOOL_MAX_RECORDS = 64;
inline constexpr uint16_t PDF_IMAGE_BUILD_PALETTE_BYTES = 768;
inline constexpr size_t PDF_IMAGE_BUILD_RECORD_BYTES = 880;

struct PdfDeferredImageRecord {
  PdfObjectReference reference{};
  PdfObjectReference auxiliaryReference{};
  uint64_t streamOffset = 0;
  uint64_t streamLength = 0;
  uint64_t contentHash = 0;
  uint64_t auxiliaryStreamOffset = 0;
  uint64_t auxiliaryStreamLength = 0;
  uint32_t sourceCrc32 = 0;
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t auxiliaryWidth = 0;
  uint32_t auxiliaryHeight = 0;
  PdfStreamFilter filters[PdfLimits::MaxFiltersPerStream]{};
  PdfStreamFilter auxiliaryFilters[PdfLimits::MaxFiltersPerStream]{};
  uint8_t bitsPerComponent = 0;
  uint8_t predictor = 1;
  uint8_t auxiliaryBitsPerComponent = 0;
  uint8_t auxiliaryPredictor = 1;
  uint8_t filterCount = 0;
  uint8_t auxiliaryFilterCount = 0;
  PdfImageColorSpace colorSpace = PdfImageColorSpace::Gray;
  PdfImageDecode decode = PdfImageDecode::Normal;
  PdfImageColorSpace auxiliaryColorSpace = PdfImageColorSpace::Gray;
  PdfImageDecode auxiliaryDecode = PdfImageDecode::Normal;
  PdfImageAuxiliaryKind auxiliaryKind = PdfImageAuxiliaryKind::SoftMask;
  uint16_t paletteBytes = 0;
  uint16_t paletteEntries = 0;
  uint8_t palette[PDF_IMAGE_BUILD_PALETTE_BYTES]{};
  uint16_t sectionIndex = 0;
  uint16_t tagLength = 0;
  uint32_t tagOffset = 0;
  uint8_t imageMaskPaintLuminance = 0;
  bool hasAuxiliary = false;
};

enum class PdfImageSpoolReadStage : uint8_t {
  Idle,
  Header,
  Footer,
  Records,
  Complete,
};

struct PdfImageSpoolReadRuntime {
  uint64_t fileBytes = 0;
  uint32_t expectedRecordsCrc32 = 0;
  uint32_t recordsCrc32 = 0;
  uint8_t nextRecord = 0;
  PdfImageSpoolReadStage stage = PdfImageSpoolReadStage::Idle;
};

class PdfImageBuildSpool {
 public:
  PdfStatus beginWrite(const PdfCacheIo& io, const char* path, uint8_t* workspace, size_t workspaceBytes);
  PdfStatus append(const PdfDeferredImageRecord& record);
  PdfStatus closeWrite();

  PdfStatus beginRead(const PdfCacheIo& io, const char* path, uint8_t* workspace, size_t workspaceBytes,
                      PdfImageSpoolReadRuntime* runtime);
  PdfStepResult stepReadOpen(PdfImageSpoolReadRuntime& runtime, PdfWorkBudget& budget);
  PdfStatus readRecord(uint8_t index, PdfDeferredImageRecord* record) const;
  PdfStatus closeRead();
  PdfStatus readRecordDetached(uint8_t index, PdfDeferredImageRecord* record);
  void abort();
  void remove();

  uint8_t recordCount() const { return recordCount_; }
  bool writing() const { return writing_; }
  bool reading() const { return reading_; }
  bool validated() const { return validated_; }

 private:
  PdfStatus readEncodedBytes(PdfCacheHandle handle, uint8_t index) const;
  PdfStatus readEncodedRecord(PdfCacheHandle handle, uint8_t index, PdfDeferredImageRecord* record) const;

  PdfCacheIo io_{};
  PdfCacheHandle handle_{};
  char path_[PDF_CACHE_PATH_CAPACITY]{};
  uint8_t* workspace_ = nullptr;
  size_t workspaceBytes_ = 0;
  uint32_t recordsCrc32_ = 0;
  uint8_t recordCount_ = 0;
  bool writing_ = false;
  bool reading_ = false;
  bool validated_ = false;
};

static_assert(sizeof(PdfDeferredImageRecord) <= 896, "one deferred image record must remain a bounded fixed record");
static_assert(sizeof(PdfImageBuildSpool) <= 256,
              "image build spool state must remain small enough for preparation state");

inline constexpr size_t PDF_IMAGE_FILE_RECORD_BYTES = 116;

class PdfImageFileSpool {
 public:
  PdfStatus beginWrite(const PdfCacheIo& io, const char* path);
  PdfStatus append(const PdfRequiredFileRecord& record);
  PdfStatus closeWrite();
  PdfStatus beginRead(const PdfCacheIo& io, const char* path, uint8_t* workspace, size_t workspaceBytes,
                      PdfImageSpoolReadRuntime* runtime);
  PdfStepResult stepReadOpen(PdfImageSpoolReadRuntime& runtime, PdfWorkBudget& budget);
  PdfStatus readRecord(uint8_t index, PdfRequiredFileRecord* record) const;
  PdfStatus closeRead();
  void abort();
  void remove();

  uint8_t recordCount() const { return recordCount_; }
  bool writing() const { return writing_; }
  bool reading() const { return reading_; }

 private:
  PdfStatus readEncodedRecord(uint8_t index, PdfRequiredFileRecord* record) const;

  PdfCacheIo io_{};
  PdfCacheHandle handle_{};
  char path_[PDF_CACHE_PATH_CAPACITY]{};
  uint8_t* workspace_ = nullptr;
  size_t workspaceBytes_ = 0;
  uint32_t recordsCrc32_ = 0;
  uint8_t recordCount_ = 0;
  bool writing_ = false;
  bool reading_ = false;
};

static_assert(sizeof(PdfImageFileSpool) <= 256,
              "image file spool state must remain small enough for preparation state");
static_assert(sizeof(PdfImageSpoolReadRuntime) <= 32, "incremental spool validation must remain fixed and small");
