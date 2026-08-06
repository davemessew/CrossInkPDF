#pragma once

#include <cstddef>
#include <cstdint>

#include "PdfCacheIo.h"
#include "PdfImageExtractor.h"
#include "PdfTypes.h"
#include "PdfWorkBudget.h"

inline constexpr uint16_t PDF_MASK_SPOOL_VERSION = 2;
inline constexpr uint8_t PDF_MASK_SPOOL_MAX_RECORDS = 64;

struct PdfMaskSpoolRecord {
  uint64_t contentHash = 0;
  uint64_t baseOffset = 0;
  uint64_t baseBytes = 0;
  uint64_t alphaOffset = 0;
  uint64_t alphaBytes = 0;
  uint32_t baseCrc32 = 0;
  uint32_t alphaCrc32 = 0;
  uint32_t sourceCrc32 = 0;
  uint16_t width = 0;
  uint16_t height = 0;
};

struct PdfMaskPlaneConfig {
  PdfCacheIo io{};
  PdfCacheHandle handle{};
  uint64_t* writeOffset = nullptr;
  PdfMaskSpoolRecord* record = nullptr;
  uint32_t sourceWidth = 0;
  uint32_t sourceHeight = 0;
  uint16_t outputWidth = 0;
  uint16_t outputHeight = 0;
  uint8_t bitsPerComponent = 0;
  uint8_t predictor = 1;
  PdfImageDecode decode = PdfImageDecode::Normal;
  bool explicitMask = false;
  uint8_t* rowWorkspace = nullptr;
  size_t rowWorkspaceBytes = 0;
  uint8_t* outputWorkspace = nullptr;
  size_t outputWorkspaceBytes = 0;
};

enum class PdfMaskSpoolReadStage : uint8_t {
  Idle,
  Header,
  Footer,
  Records,
  Planes,
  Complete,
};

enum class PdfMaskSpoolCloseStage : uint8_t {
  Idle,
  Records,
  Footer,
  Flush,
  Sync,
  Close,
  Complete,
};

struct PdfMaskSpoolCloseRuntime {
  uint32_t recordsCrc = 0;
  uint8_t recordIndex = 0;
  PdfMaskSpoolCloseStage stage = PdfMaskSpoolCloseStage::Idle;
};

static_assert(sizeof(PdfMaskSpoolCloseRuntime) <= 8, "mask spool close state must remain a small fixed workspace");

struct PdfMaskSpoolReadRuntime {
  uint64_t metadataSize = 0;
  uint64_t expectedPayloadOffset = 0;
  uint64_t planeConsumed = 0;
  uint32_t expectedRecordsCrc = 0;
  uint32_t recordsCrc = 0;
  uint32_t planeCrc = 0;
  uint8_t recordIndex = 0;
  uint8_t planeIndex = 0;
  PdfMaskSpoolReadStage stage = PdfMaskSpoolReadStage::Idle;
};

static_assert(sizeof(PdfMaskSpoolReadRuntime) <= 48, "mask spool validation must remain a small fixed workspace");

class PdfMaskPlaneWriter {
 public:
  PdfStatus begin(const PdfMaskPlaneConfig& config);
  PdfByteSink decodedSink();
  PdfStatus finish();

 private:
  static PdfStatus writeDecoded(void* context, const uint8_t* source, size_t requested, size_t* bytesWritten);
  PdfStatus consume(const uint8_t* source, size_t requested, size_t* bytesWritten);
  PdfStatus finishRow();

  PdfMaskPlaneConfig config_{};
  size_t sourceRowBytes_ = 0;
  size_t rowPosition_ = 0;
  uint32_t sourceRow_ = 0;
  uint32_t nextOutputSourceRow_ = 0;
  uint16_t outputRow_ = 0;
  uint8_t pngFilter_ = 0;
  uint8_t pngUpLeft_ = 0;
  bool readingPngFilter_ = false;
  bool initialized_ = false;
};

class PdfMaskSpool {
 public:
  PdfStatus beginWrite(const PdfCacheIo& io, const char* path);
  PdfStatus beginRecord(uint64_t contentHash, uint32_t sourceCrc32, uint16_t width, uint16_t height,
                        PdfByteSink* baseSink);
  PdfStatus beginAlpha(const PdfMaskPlaneConfig& config, PdfMaskPlaneWriter* plane);
  PdfStatus finishRecord();
  PdfStatus beginCloseWrite(PdfMaskSpoolCloseRuntime* runtime);
  PdfStepResult stepCloseWrite(PdfMaskSpoolCloseRuntime& runtime, PdfWorkBudget& budget);
  PdfStatus closeWrite();

  PdfStatus beginRead(const PdfCacheIo& io, const char* path, uint8_t* ioWorkspace, size_t ioWorkspaceBytes,
                      PdfMaskSpoolReadRuntime* runtime);
  PdfStepResult stepReadOpen(PdfMaskSpoolReadRuntime& runtime, PdfWorkBudget& budget);
  PdfStatus read(uint64_t offset, uint8_t* destination, size_t requested, size_t* bytesRead) const;
  PdfStatus closeRead();
  void abort();

  uint8_t recordCount() const { return recordCount_; }
  const PdfMaskSpoolRecord& record(uint8_t index) const { return records_[index]; }

 private:
  static PdfStatus writeBase(void* context, const uint8_t* source, size_t requested, size_t* bytesWritten);
  PdfStatus append(const uint8_t* source, size_t requested, uint32_t* crc, uint64_t* bytesWritten);

  PdfCacheIo io_{};
  PdfCacheHandle handle_{};
  PdfMaskSpoolRecord records_[PDF_MASK_SPOOL_MAX_RECORDS]{};
  char path_[PDF_CACHE_PATH_CAPACITY]{};
  uint8_t* ioWorkspace_ = nullptr;
  size_t ioWorkspaceBytes_ = 0;
  uint64_t writeOffset_ = 0;
  uint64_t recordsOffset_ = 0;
  uint8_t recordCount_ = 0;
  bool writing_ = false;
  bool reading_ = false;
  bool recordOpen_ = false;
  bool alphaOpen_ = false;
};

static_assert(sizeof(PdfMaskSpool) <= 4096, "mask spool state must fit in the phase-reused run workspace");
static_assert(sizeof(PdfMaskPlaneWriter) <= 256, "mask row adapter must remain a small phase-reused object");
