#pragma once

#include <cstddef>
#include <cstdint>

#include "PdfCacheIo.h"
#include "PdfSourceIdentity.h"

inline constexpr size_t PDF_SAVED_ITEM_ANCHOR_BYTES = 10;
inline constexpr uint16_t PDF_SAVED_ITEMS_MAX_BOOKMARKS = 64;
inline constexpr uint16_t PDF_SAVED_ITEMS_MAX_CLIPPINGS = 64;
inline constexpr uint16_t PDF_SAVED_ITEMS_MAX_RECORDS = PDF_SAVED_ITEMS_MAX_BOOKMARKS + PDF_SAVED_ITEMS_MAX_CLIPPINGS;

inline constexpr size_t PDF_SAVED_ITEMS_HEADER_BYTES = 80;
inline constexpr size_t PDF_SAVED_ITEMS_RECORD_BYTES = 56;
inline constexpr size_t PDF_SAVED_ITEMS_MAX_SLOT_BYTES =
    PDF_SAVED_ITEMS_HEADER_BYTES + PDF_SAVED_ITEMS_MAX_RECORDS * PDF_SAVED_ITEMS_RECORD_BYTES;

// These offsets are part of PSIT v1 and are exposed for format-level tests and
// migration tooling. Production callers should use PdfSavedItemsStore.
inline constexpr size_t PDF_SAVED_ITEMS_SEQUENCE_OFFSET = 16;
inline constexpr size_t PDF_SAVED_ITEMS_RECORDS_CRC_OFFSET = 60;
inline constexpr size_t PDF_SAVED_ITEMS_HEADER_CRC_OFFSET = 64;

enum class PdfSavedItemKind : uint8_t {
  Bookmark = 1,
  Clipping = 2,
};

inline constexpr uint8_t PDF_SAVED_ITEM_HAS_START_SEMANTIC = 1U;
inline constexpr uint8_t PDF_SAVED_ITEM_HAS_END_SEMANTIC = 2U;
inline constexpr uint8_t PDF_SAVED_ITEM_HAS_FALLBACK_PAGES = 4U;

struct PdfSavedItem {
  uint32_t timestamp = 0;
  uint32_t startGlobalWordOrdinal = 0;
  uint32_t endGlobalWordOrdinal = 0;
  uint32_t startBlockWordOffset = 0;
  uint32_t endBlockWordOffset = 0;
  // Fingerprint of the exact layout that produced the fallback page tuple.
  // Zero means no page fallback is safe.
  uint32_t fallbackLayoutFingerprint = 0;
  // Stable PDF-only identity. Integration may carry this through the existing
  // uint16_t paragraphIndex field without changing EPUB persistence bytes.
  uint16_t itemId = 0;
  uint16_t sectionIndex = 0;
  uint16_t fallbackStartPage = 0;
  uint16_t fallbackEndPage = 0;
  uint16_t fallbackPageCount = 0;
  PdfSavedItemKind kind = PdfSavedItemKind::Bookmark;
  uint8_t flags = 0;
  char startBlockAnchor[PDF_SAVED_ITEM_ANCHOR_BYTES] = {};
  char endBlockAnchor[PDF_SAVED_ITEM_ANCHOR_BYTES] = {};
};

static_assert(sizeof(PdfSavedItem) == PDF_SAVED_ITEMS_RECORD_BYTES,
              "PDF saved items must stay equal to the fixed PSIT record budget");

struct PdfSavedItemsBuffer {
  PdfSavedItem* items = nullptr;
  uint16_t capacity = 0;
  uint16_t count = 0;
};

// Canonical PSIT semantic-record validation. Integration must call this exact
// validator before mutating its legacy display record.
PdfStatus pdfValidateSavedItem(const PdfSavedItem& item, uint32_t totalWords);

class PdfSavedItemsStore {
 public:
  PdfStatus initialize(const PdfCacheIo& io, const char* cacheRoot, const PdfSourceIdentity& source,
                       uint32_t totalWords);
  PdfStatus load(PdfSavedItemsBuffer* output) const;
  PdfStatus save(const PdfSavedItem* items, uint16_t count) const;
  PdfStatus validate(const PdfSavedItem& item) const;

 private:
  struct SlotInfo;
  union IoWorkspace {
    char path[PDF_CACHE_PATH_CAPACITY];
    uint8_t encoded[PDF_SAVED_ITEMS_HEADER_BYTES];
  };
  static_assert(sizeof(IoWorkspace) == PDF_CACHE_PATH_CAPACITY,
                "Saved-item I/O reuses one bounded path/codec workspace");

  PdfStatus inspectSlot(const char* name, SlotInfo* info) const;
  PdfStatus readSlot(const char* name, const SlotInfo& expected, PdfSavedItemsBuffer* output) const;
  PdfStatus writeSlot(const char* name, uint32_t sequence, const PdfSavedItem* items, uint16_t count,
                      uint16_t bookmarkCount, uint16_t clippingCount, uint32_t recordsCrc) const;
  bool formatPath(const char* name, char destination[PDF_CACHE_PATH_CAPACITY]) const;

  PdfCacheIo io_{};
  const char* cacheRoot_ = nullptr;
  PdfSourceIdentity source_{};
  uint32_t totalWords_ = 0;
  bool initialized_ = false;
  // A save that reached WriteTruncate but did not receive a fully successful
  // close is unconfirmed for this object lifetime. Keep its slot quarantined
  // without deleting possibly durable bytes.
  mutable const char* pendingSlot_ = nullptr;
  mutable uint32_t pendingSequence_ = 0;
  // PdfReflowDocument serializes saved-item operations. Reusing one object-owned
  // workspace removes a 192-byte RV32 task-stack local without heap churn.
  mutable IoWorkspace ioWorkspace_{};
};
