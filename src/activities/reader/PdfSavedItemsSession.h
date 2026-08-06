#pragma once

#include <cstdint>

#include "PdfSavedItemsStore.h"

inline constexpr uint8_t PDF_SAVED_ITEMS_CAP_BOOKMARKS = 1U;
inline constexpr uint8_t PDF_SAVED_ITEMS_CAP_CLIPPINGS = 2U;
inline constexpr uint8_t PDF_SAVED_ITEMS_CAP_ID_ALLOCATION = 4U;

struct PdfSavedItemsPersistence {
  void* context = nullptr;
  PdfStatus (*load)(void* context, PdfSavedItemsBuffer* output) = nullptr;
  PdfStatus (*save)(void* context, const PdfSavedItem* items, uint16_t count) = nullptr;
  PdfStatus (*validate)(void* context, const PdfSavedItem& item) = nullptr;

  bool valid() const { return load != nullptr && save != nullptr && validate != nullptr; }
};

enum class PdfSavedItemsLegacyMutationResult : uint8_t {
  Applied,
  // The mutation was definitively rejected before the legacy store changed.
  Rejected,
  // The durable legacy outcome cannot be inferred from the callback result.
  Ambiguous,
};

// A view over one already-loaded legacy store. The coordinator never owns or
// allocates legacy records. The callbacks must remain valid for the session
// lifetime and must not open the PDF or PSIT file themselves. A successful
// mutation must update the same count/read view before it returns.
struct PdfSavedItemsLegacyAccess {
  void* context = nullptr;
  bool (*count)(void* context, uint16_t* output) = nullptr;
  // For PDF legacy records this is the stable ID carried in paragraphIndex.
  bool (*readItemId)(void* context, uint16_t index, uint16_t* output) = nullptr;
  PdfSavedItemsLegacyMutationResult (*addItem)(void* context, uint16_t itemId) = nullptr;
  PdfSavedItemsLegacyMutationResult (*removeItem)(void* context, uint16_t itemId) = nullptr;
  PdfSavedItemsLegacyMutationResult (*clearItems)(void* context) = nullptr;

  bool readable() const { return count != nullptr && readItemId != nullptr; }
  bool mutableAccess() const {
    return readable() && addItem != nullptr && removeItem != nullptr && clearItems != nullptr;
  }
};

enum class PdfSavedItemsSessionResult : uint8_t {
  Applied,
  // Reserved for retrying a reconciliation write discovered during begin().
  SavePending,
  // Destroy and reload the session so PSIT/legacy intersection reconciliation
  // can resolve an outcome that may already be durable.
  ReloadRequired,
  LimitReached,
  Unavailable,
  InvalidArgument,
  InvalidItem,
  NotFound,
  LegacyFailure,
};

// Allocation-free PDF-only coordinator. PdfSavedItemsBuffer remains owned by
// the caller because its maximum 7,168-byte payload is too large for a reader
// task stack and must be allocated once for the activity lifetime.
class PdfSavedItemsSession {
 public:
  PdfStatus begin(PdfSavedItemsBuffer* buffer, const PdfSavedItemsPersistence& persistence,
                  const PdfSavedItemsLegacyAccess& bookmarks, const PdfSavedItemsLegacyAccess& clippings);

  uint8_t capabilityMask() const { return capabilities_; }
  bool supports(PdfSavedItemKind kind) const;
  bool canAllocateItemIds() const { return (capabilities_ & PDF_SAVED_ITEMS_CAP_ID_ALLOCATION) != 0; }
  bool dirty() const { return dirty_; }

  uint16_t count(PdfSavedItemKind kind) const;
  const PdfSavedItem* find(PdfSavedItemKind kind, uint16_t itemId) const;
  const PdfSavedItem* findBookmarkInPage(uint16_t sectionIndex, uint32_t firstGlobalWordOrdinal,
                                         uint32_t lastGlobalWordOrdinal) const;

  PdfSavedItemsSessionResult add(const PdfSavedItem& item, uint16_t* assignedItemId);
  PdfSavedItemsSessionResult remove(PdfSavedItemKind kind, uint16_t itemId);
  PdfSavedItemsSessionResult clear(PdfSavedItemKind kind);
  PdfStatus flush();

  PdfSavedItemsSessionResult queueJump(PdfSavedItemKind kind, uint16_t itemId);
  bool pendingJump(PdfSavedItem* output) const;
  // A transient mapping/layout failure passes false and retains the request.
  // Only a successfully applied jump passes true.
  void resolvePendingJump(bool applied);
  void cancelPendingJump();

 private:
  static constexpr uint8_t capabilityFor(PdfSavedItemKind kind) {
    return kind == PdfSavedItemKind::Bookmark ? PDF_SAVED_ITEMS_CAP_BOOKMARKS : PDF_SAVED_ITEMS_CAP_CLIPPINGS;
  }

  PdfSavedItemsLegacyAccess* accessFor(PdfSavedItemKind kind);
  const PdfSavedItemsLegacyAccess* accessFor(PdfSavedItemKind kind) const;
  bool probeLegacy(PdfSavedItemKind kind);
  bool legacyMatchCount(const PdfSavedItemsLegacyAccess& access, uint16_t itemId, uint16_t* matches) const;
  bool idUsed(uint16_t itemId);
  bool allocateItemId(uint16_t* output);
  void initializeNextItemId();
  void reconcile();
  void maskCapability(PdfSavedItemKind kind);
  void requireReload();
  uint16_t findIndex(PdfSavedItemKind kind, uint16_t itemId) const;
  bool validKind(PdfSavedItemKind kind) const;

  PdfSavedItemsBuffer* buffer_ = nullptr;
  PdfSavedItemsPersistence persistence_{};
  PdfSavedItemsLegacyAccess bookmarks_{};
  PdfSavedItemsLegacyAccess clippings_{};
  uint16_t nextItemId_ = 1;
  uint16_t pendingItemId_ = 0;
  PdfSavedItemKind pendingKind_ = PdfSavedItemKind::Bookmark;
  uint8_t capabilities_ = 0;
  bool initialized_ = false;
  bool dirty_ = false;
  bool hasPendingJump_ = false;
};
