#pragma once

#include <cstddef>
#include <cstdint>

#include "PdfCacheIo.h"

// Session-only fixed-record storage. Records are deliberately raw because the
// file is removed before the prepared generation is published; durable resume
// records keep their existing versioned encodings.
class PdfFixedRecordSpool {
 public:
  PdfFixedRecordSpool() = default;
  ~PdfFixedRecordSpool() { abortClose(); }

  PdfFixedRecordSpool(const PdfFixedRecordSpool&) = delete;
  PdfFixedRecordSpool& operator=(const PdfFixedRecordSpool&) = delete;
  PdfFixedRecordSpool(PdfFixedRecordSpool&&) = delete;
  PdfFixedRecordSpool& operator=(PdfFixedRecordSpool&&) = delete;

  // `io` must outlive this spool. PdfPreparation declares its config before
  // the spools so member destruction preserves that lifetime.
  PdfStatus configure(const PdfCacheIo* io, size_t recordSize, uint32_t capacity);
  PdfStatus open(const char* path, PdfCacheOpenMode mode, uint32_t existingRecordCount = 0);
  // Reopens an existing complete prefix for write-only append. Metadata and
  // end offset are checked before writes so resume cannot silently overwrite
  // or create a hole in the spool.
  PdfStatus openForAppend(const char* path, uint32_t existingRecordCount);
  // Opens an already complete session spool for bounded in-place updates.
  // Generic ReadWrite access stays rejected by open(), and store().write only
  // appends while a WriteTruncate session is active.
  PdfStatus openForUpdates(const char* path, uint32_t existingRecordCount);
  // Appends a contiguous group in one storage write. `records` must contain
  // exactly `count * recordSize` bytes in fixed-record order.
  PdfStatus appendRecords(const void* records, uint32_t count);
  // Reads a contiguous group in one storage operation. The destination must
  // have room for exactly `count * recordSize` bytes.
  PdfStatus readRecords(uint32_t ordinal, void* records, uint32_t count);
  PdfStatus rewriteExisting(uint32_t ordinal, const void* record, size_t recordSize);
  PdfStatus flush();
  PdfStatus sync();
  PdfStatus close();
  void abortClose();

  PdfFixedRecordStore store();
  bool isOpen() const { return handle_.valid(); }
  uint32_t recordCount() const { return recordCount_; }
  uint32_t readOperations() const { return readOperations_; }
  uint64_t readBytes() const { return static_cast<uint64_t>(readOperations_) * recordSize_; }
  uint32_t writeOperations() const { return writeOperations_; }
  uint64_t writeBytes() const { return static_cast<uint64_t>(writeOperations_) * recordSize_; }

 private:
  static PdfStatus readRecord(void* context, uint32_t ordinal, void* record, size_t recordSize);
  static PdfStatus writeRecord(void* context, uint32_t ordinal, const void* record, size_t recordSize);

  const PdfCacheIo* io_ = nullptr;
  PdfCacheHandle handle_{};
  size_t recordSize_ = 0;
  uint32_t capacity_ = 0;
  uint32_t recordCount_ = 0;
  uint32_t readOperations_ = 0;
  uint32_t writeOperations_ = 0;
  PdfCacheOpenMode mode_ = PdfCacheOpenMode::Read;
};

// Mutable session storage for algorithms that must append records and later
// patch links between them. The caller controls short access sessions so a
// ReadWrite handle never overlaps the PDF source or xref reader.
class PdfMutableRecordSpool {
 public:
  PdfMutableRecordSpool() = default;
  ~PdfMutableRecordSpool() { abortClose(); }

  PdfMutableRecordSpool(const PdfMutableRecordSpool&) = delete;
  PdfMutableRecordSpool& operator=(const PdfMutableRecordSpool&) = delete;

  PdfStatus configure(const PdfCacheIo* io, size_t recordSize, uint32_t capacity);
  PdfStatus create(const char* path);
  PdfStatus openSession(const char* path);
  // Appends a contiguous group at the logical end in one storage write. The
  // caller retains the open session and decides when to close or sync it.
  PdfStatus appendRecords(const void* records, uint32_t count);
  PdfStatus closeSession();
  void abortClose();
  PdfFixedRecordStore store();
  bool isOpen() const { return handle_.valid(); }
  uint32_t recordCount() const { return recordCount_; }

 private:
  static PdfStatus readRecord(void* context, uint32_t ordinal, void* record, size_t recordSize);
  static PdfStatus writeRecord(void* context, uint32_t ordinal, const void* record, size_t recordSize);

  const PdfCacheIo* io_ = nullptr;
  PdfCacheHandle handle_{};
  size_t recordSize_ = 0;
  uint32_t capacity_ = 0;
  uint32_t recordCount_ = 0;
};

#if UINTPTR_MAX == UINT32_MAX
static_assert(sizeof(PdfFixedRecordSpool) <= 40, "fixed-record spool state must remain small");
static_assert(sizeof(PdfMutableRecordSpool) == 20U, "mutable spool state must not grow on RV32");
#endif
